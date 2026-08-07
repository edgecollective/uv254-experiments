#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// Buttons (active-low, internal pull-up).
#define BTN_CAL_PIN   9   // Button B -> CALIBRATE
#define BTN_MEAS_PIN  7   // Button A -> COMPUTE

// LED modulation output (MICRO_PULSE on the LED CTRL SELECT jumper).
// 200 Hz sits at the peak of the detector chain's bandpass (see freq_test/exp2.png).
#define LED_PIN        12
const uint32_t LED_FREQ_HZ   = 200;
const uint32_t LED_WARMUP_MS = 5000;

// Detector input: envelope-detected DC from the detector daughterboard
// (raw detect -> rectifier -> RC low-pass at ~88 Hz -> A5).
#define DETECT_PIN A5

// ADC scaling for the on-display voltage readout. 3.3 V reference, 16-bit
// analogReadResolution -> full-scale count = 65535.
const float V_REF          = 3.3f;
const float ADC_FULL_SCALE = 65535.0f;
static inline float counts_to_volts(float c) { return c * V_REF / ADC_FULL_SCALE; }

// SSD1306 on default I2C.
#define OLED_W       128
#define OLED_H       64
#define OLED_ADDR    0x3C
Adafruit_SSD1306 display(OLED_W, OLED_H, &Wire, -1);
bool display_ok = false;

const uint8_t  N_SAMPLES = 5;
const uint32_t SAMPLE_INTERVAL_MS = 1000;

enum State { READY, WARMING_UP, AVERAGING };
enum Phase { PHASE_NONE, PHASE_BLANK, PHASE_SAMPLE };

State state = READY;
Phase phase = PHASE_NONE;

uint8_t  avg_count = 0;
double   avg_sum   = 0.0;
uint32_t next_sample_ms = 0;
uint32_t warmup_done_ms = 0;

float i_blank    = 0.0f;
float i_sample   = 0.0f;
float absorbance = 0.0f;
bool  have_calibration = false;
bool  have_sample      = false;

struct Button {
  uint8_t pin;
  int     last_stable;
  uint32_t last_change;
};
Button btn_cal  = {BTN_CAL_PIN,  HIGH, 0};
Button btn_meas = {BTN_MEAS_PIN, HIGH, 0};
const uint32_t BTN_DEBOUNCE_MS = 30;

static void led_on() {
  tone(LED_PIN, LED_FREQ_HZ);
}

static void led_off() {
  noTone(LED_PIN);
  digitalWrite(LED_PIN, LOW);
}

static const char* phase_label() {
  switch (phase) {
    case PHASE_BLANK:  return "BLANK";
    case PHASE_SAMPLE: return "SAMPLE";
    default:           return "";
  }
}

static void draw_bottom_readouts() {
  display.setTextSize(1);

  display.setCursor(0, 40);
  display.print("Blk= ");
  if (have_calibration) {
    display.print((uint32_t)(i_blank + 0.5f));
    display.print(' ');
    display.print(counts_to_volts(i_blank), 2);
    display.print('V');
  }

  display.setCursor(0, 48);
  display.print("Smp= ");
  if (have_sample) {
    display.print((uint32_t)(i_sample + 0.5f));
    display.print(' ');
    display.print(counts_to_volts(i_sample), 2);
    display.print('V');
  }

  display.setCursor(0, 56);
  display.print("Abs= ");
  if (have_sample && have_calibration) display.print(absorbance, 4);
}

static void render() {
  if (!display_ok) return;
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);

  switch (state) {
    case READY:
      display.println("Ready (M0)");
      display.println("B: Calibrate");
      if (have_calibration) display.println("A: Compute");
      break;

    case WARMING_UP:
      display.print("Warming LED for ");
      display.println(phase_label());
      break;

    case AVERAGING:
      display.print("Measuring ");
      display.print(phase_label());
      display.print(": ");
      display.print(avg_count);
      display.print('/');
      display.println(N_SAMPLES);
      break;
  }

  draw_bottom_readouts();
  display.display();
}

static void start_averaging() {
  avg_count = 0;
  avg_sum = 0.0;
  next_sample_ms = millis();
  state = AVERAGING;
}

static void start_warmup() {
  warmup_done_ms = millis() + LED_WARMUP_MS;
  state = WARMING_UP;
}

static void on_calibrate_press() {
  if (state != READY) return;
  // Re-calibrating invalidates the previous sample/absorbance.
  have_calibration = false;
  have_sample = false;
  phase = PHASE_BLANK;
  led_on();
  start_warmup();
  render();
}

static void on_compute_press() {
  if (state != READY) return;
  if (!have_calibration) return;
  phase = PHASE_SAMPLE;
  led_on();
  start_warmup();
  render();
}

static void on_avg_complete() {
  float v = (float)(avg_sum / N_SAMPLES);
  switch (phase) {
    case PHASE_BLANK:
      i_blank = v;
      Serial.print("blank="); Serial.println(i_blank, 4);
      led_off();
      have_calibration = true;
      phase = PHASE_NONE;
      state = READY;
      break;

    case PHASE_SAMPLE: {
      i_sample = v;
      Serial.print("sample="); Serial.println(i_sample, 4);
      led_off();
      have_sample = true;
      absorbance = (i_sample > 0.0f && i_blank > 0.0f)
                     ? log10f(i_blank / i_sample)
                     : 0.0f;
      Serial.print("absorbance="); Serial.println(absorbance, 6);
      phase = PHASE_NONE;
      state = READY;
      break;
    }

    default:
      break;
  }
  Serial.flush();
}

static void tick_warmup() {
  if (state != WARMING_UP) return;
  if ((int32_t)(millis() - warmup_done_ms) < 0) return;

  // Throwaway sample so the first averaged read is at steady state, not on the
  // tail of the LED warm-up transient.
  float discard = (float)analogRead(DETECT_PIN);
  Serial.print("discard ");
  Serial.println(discard, 4);
  Serial.flush();

  start_averaging();
  render();
}

static void tick_averaging() {
  if (state != AVERAGING) return;
  if ((int32_t)(millis() - next_sample_ms) < 0) return;

  float v = (float)analogRead(DETECT_PIN);
  avg_sum += v;
  avg_count++;
  next_sample_ms += SAMPLE_INTERVAL_MS;

  Serial.print("avg "); Serial.print(avg_count);
  Serial.print('/'); Serial.print(N_SAMPLES);
  Serial.print(' '); Serial.println(v, 4);
  Serial.flush();

  if (avg_count >= N_SAMPLES) {
    on_avg_complete();
  }
  render();
}

static void check_button(Button& b, void (*handler)()) {
  int now = digitalRead(b.pin);
  uint32_t t = millis();
  if (now != b.last_stable && (t - b.last_change) > BTN_DEBOUNCE_MS) {
    b.last_stable = now;
    b.last_change = t;
    if (now == LOW) handler();
  }
}

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(BTN_CAL_PIN,  INPUT_PULLUP);
  pinMode(BTN_MEAS_PIN, INPUT_PULLUP);

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);   // LED off until CALIBRATE/COMPUTE drives it

  // SAMD21 SAADC native max is 12 bits (0..4095). The Arduino SAMD core
  // scales requests above 12 by enabling hardware oversampling+decimation
  // via ADC.AVGCTRL, so 16 bits gives 0..65535 with ~14-bit ENOB.
  analogReadResolution(16);

  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && (millis() - t0) < 2000) { delay(10); }

  Wire.begin();
  display_ok = display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
  if (display_ok) {
    display.setRotation(0);
    display.setTextColor(SSD1306_WHITE);
    render();
  }

  Serial.print("ready (M0), oled=");
  Serial.println(display_ok ? "ok" : "FAIL");
  Serial.flush();
}

void loop() {
  check_button(btn_cal,  on_calibrate_press);
  check_button(btn_meas, on_compute_press);
  tick_warmup();
  tick_averaging();
}
