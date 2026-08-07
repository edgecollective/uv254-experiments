// Absorbance measurement firmware, M0, "settle" variant.
//
// LED is driven by an external 555 (soldered on) - the micro does NOT drive
// the LED pin. Each of the two measurements (BLANK / SAMPLE) runs 10 rounds
// of 5-second boxcar averages sampled at 10 Hz (50 s total per measurement).
// The mean of the LAST 5 rounds is what we use as the light-intensity
// value in the absorbance calculation - the first 5 rounds are treated as
// settling and are printed only so we can eyeball how long the system
// needs after a cuvette swap.
//
// Buttons:
//   B (pin 9) -> CALIBRATE  (measure I_blank)
//   A (pin 7) -> COMPUTE    (measure I_sample, then A = log10(I_blank / I_sample))
//
// Serial output during a measurement:
//   # BLANK begin at t=NN.NNNs
//   <t_s>,<phase>,<round>,<adc>,<volts>
//   ...  (10 rows, one per 5-s round)
//   # BLANK final = <adc> counts, <volts> V  (mean of rounds 6-10)
// where phase = "blank" or "sample" and round = 1..10.

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_DotStar.h>

#define BTN_CAL_PIN   9   // Button B -> CALIBRATE (blank)
#define BTN_MEAS_PIN  7   // Button A -> COMPUTE   (sample -> absorbance)
#define DETECT_PIN    A5

const uint32_t SUBSAMPLE_INTERVAL_MS = 100;   // 10 Hz
const uint16_t SAMPLES_PER_ROUND     = 50;    // 5 s per round
const uint8_t  ROUNDS_PER_MEASUREMENT = 10;   // 50 s total

// 3.3 V reference, 12-bit native ADC.
const float V_REF          = 3.3f;
const float ADC_FULL_SCALE = 4095.0f;
static inline float counts_to_volts(float c) { return c * V_REF / ADC_FULL_SCALE; }

#define OLED_W     128
#define OLED_H     64
#define OLED_ADDR  0x3C
Adafruit_SSD1306 display(OLED_W, OLED_H, &Wire, -1);
bool display_ok = false;

Adafruit_DotStar onboard_pixel(1, PIN_DOTSTAR_DATA, PIN_DOTSTAR_CLK, DOTSTAR_BGR);

enum State { STATE_READY, STATE_MEASURING };
enum Phase { PHASE_NONE, PHASE_BLANK, PHASE_SAMPLE };

State state = STATE_READY;
Phase phase = PHASE_NONE;

// Per-measurement state.
uint8_t  round_index      = 0;      // 0..ROUNDS_PER_MEASUREMENT-1
uint16_t sample_index     = 0;      // 0..SAMPLES_PER_ROUND-1
double   sample_sum       = 0.0;
uint32_t next_subsample_ms = 0;
uint32_t phase_start_ms    = 0;

float    last_round_avg = 0.0f;     // most recently completed round's average
float    round_avgs[10]  = {0};      // history for the OLED trend (up to 8)

// Locked-in values for the absorbance calculation (last round of each).
float i_blank  = 0.0f;
float i_sample = 0.0f;
float absorbance = 0.0f;
bool  have_calibration = false;
bool  have_sample      = false;

uint32_t boot_ms = 0;

struct Button {
  uint8_t pin;
  int     last_stable;
  uint32_t last_change;
};
Button btn_cal  = {BTN_CAL_PIN,  HIGH, 0};
Button btn_meas = {BTN_MEAS_PIN, HIGH, 0};
const uint32_t BTN_DEBOUNCE_MS = 30;

static const char* phase_name(Phase p) {
  switch (p) {
    case PHASE_BLANK:  return "blank";
    case PHASE_SAMPLE: return "sample";
    default:           return "-";
  }
}

static void render() {
  if (!display_ok) return;
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);

  if (state == STATE_READY) {
    display.println("Ready (M0 settle)");
    display.setCursor(0, 8);
    display.println(have_calibration ? "B:Cal  A:Compute"
                                     : "B: Calibrate");
  } else {
    display.print("Measuring ");
    display.println(phase_name(phase));
    display.setCursor(0, 8);
    display.print("round ");
    display.print(round_index + 1);
    display.print('/');
    display.print(ROUNDS_PER_MEASUREMENT);
    display.print("  ");
    display.print(sample_index);
    display.print('/');
    display.print(SAMPLES_PER_ROUND);
  }

  // Trend of round averages so we can see settling on the OLED.
  display.setCursor(0, 16);
  display.print("rnd:");
  for (uint8_t i = 0; i < round_index; i++) {
    if (i >= 6) break;   // avoid overflowing the line
    display.print(' ');
    display.print((uint32_t)(round_avgs[i] + 0.5f));
  }

  display.setCursor(0, 32);
  display.print("Blk= ");
  if (have_calibration) {
    display.print((uint32_t)(i_blank + 0.5f));
    display.print(' ');
    display.print(counts_to_volts(i_blank), 3);
    display.print('V');
  }

  display.setCursor(0, 42);
  display.print("Smp= ");
  if (have_sample) {
    display.print((uint32_t)(i_sample + 0.5f));
    display.print(' ');
    display.print(counts_to_volts(i_sample), 3);
    display.print('V');
  }

  display.setCursor(0, 54);
  display.print("Abs= ");
  if (have_sample && have_calibration) display.print(absorbance, 4);

  display.display();
}

static void start_measurement(Phase p) {
  phase = p;
  state = STATE_MEASURING;
  round_index = 0;
  sample_index = 0;
  sample_sum = 0.0;
  next_subsample_ms = millis();
  phase_start_ms    = millis();
  for (uint8_t i = 0; i < 10; i++) round_avgs[i] = 0.0f;

  if (p == PHASE_BLANK) {
    have_calibration = false;
    have_sample = false;
  }

  Serial.print("# ");
  Serial.print(phase_name(p));
  Serial.print(" begin at t=");
  Serial.print((phase_start_ms - boot_ms) / 1000.0f, 3);
  Serial.println("s");
  Serial.flush();
  render();
}

static void on_calibrate_press() {
  if (state != STATE_READY) return;
  start_measurement(PHASE_BLANK);
}

static void on_compute_press() {
  if (state != STATE_READY) return;
  if (!have_calibration) return;
  start_measurement(PHASE_SAMPLE);
}

static void finish_measurement() {
  // Mean of the last N_TAIL rounds - first (ROUNDS_PER_MEASUREMENT - N_TAIL)
  // rounds are treated as settling and discarded.
  const uint8_t N_TAIL = 5;
  double sum = 0.0;
  for (uint8_t i = ROUNDS_PER_MEASUREMENT - N_TAIL; i < ROUNDS_PER_MEASUREMENT; i++) {
    sum += round_avgs[i];
  }
  float final_val = (float)(sum / N_TAIL);

  if (phase == PHASE_BLANK) {
    i_blank = final_val;
    have_calibration = true;
  } else if (phase == PHASE_SAMPLE) {
    i_sample = final_val;
    have_sample = true;
    absorbance = (i_sample > 0.0f && i_blank > 0.0f)
                   ? log10f(i_blank / i_sample)
                   : 0.0f;
  }

  Serial.print("# ");
  Serial.print(phase_name(phase));
  Serial.print(" final = ");
  Serial.print(final_val, 4);
  Serial.print(" counts, ");
  Serial.print(counts_to_volts(final_val), 5);
  Serial.print(" V (mean of rounds ");
  Serial.print(ROUNDS_PER_MEASUREMENT - N_TAIL + 1);
  Serial.print('-');
  Serial.print(ROUNDS_PER_MEASUREMENT);
  Serial.println(")");

  if (phase == PHASE_SAMPLE) {
    Serial.print("# absorbance = log10(");
    Serial.print(i_blank, 4);
    Serial.print(" / ");
    Serial.print(i_sample, 4);
    Serial.print(") = ");
    Serial.println(absorbance, 6);
  }
  Serial.flush();

  phase = PHASE_NONE;
  state = STATE_READY;
  render();
}

static void tick_sampling() {
  if (state != STATE_MEASURING) return;
  if ((int32_t)(millis() - next_subsample_ms) < 0) return;

  float v = (float)analogRead(DETECT_PIN);
  sample_sum += (double)v;
  sample_index++;
  next_subsample_ms += SUBSAMPLE_INTERVAL_MS;

  if (sample_index >= SAMPLES_PER_ROUND) {
    float avg = (float)(sample_sum / SAMPLES_PER_ROUND);
    uint32_t now = millis();
    float t_s = (now - boot_ms) / 1000.0f;
    uint8_t rnum = round_index + 1;  // 1-indexed for humans

    Serial.print(t_s, 3);
    Serial.print(',');
    Serial.print(phase_name(phase));
    Serial.print(',');
    Serial.print(rnum);
    Serial.print(',');
    Serial.print(avg, 4);
    Serial.print(',');
    Serial.println(counts_to_volts(avg), 5);
    Serial.flush();

    if (round_index < 10) round_avgs[round_index] = avg;
    last_round_avg = avg;
    round_index++;
    sample_index = 0;
    sample_sum = 0.0;

    if (round_index >= ROUNDS_PER_MEASUREMENT) {
      finish_measurement();
      return;
    }
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

  onboard_pixel.begin();
  onboard_pixel.setPixelColor(0, 0);
  onboard_pixel.show();

  analogReadResolution(12);

  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && (millis() - t0) < 2000) { delay(10); }

  Wire.begin();
  display_ok = display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
  if (display_ok) {
    display.setRotation(0);
    display.setTextColor(SSD1306_WHITE);
  }

  boot_ms = millis();

  Serial.println("# absorbance_measurement_m0_settle ready");
  Serial.println("# LED driven by external 555; micro does not drive LED");
  Serial.print  ("# 10 rounds x 50-sample avg @ 10 Hz = 50 s per measurement");
  Serial.println("");
  Serial.println("# CSV cols = t_s,phase,round,adc,volts");
  Serial.println("# B=CALIBRATE (blank), A=COMPUTE (sample -> absorbance)");
  Serial.flush();

  render();
}

void loop() {
  check_button(btn_cal,  on_calibrate_press);
  check_button(btn_meas, on_compute_press);
  tick_sampling();
}
