#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// Button between pin 10 and GND (internal pull-up enabled).
#define BTN_PIN 10

// SSD1306 on default I2C (SDA/SCL pins on the ItsyBitsy nRF52840).
#define OLED_W       128
#define OLED_H       64
#define OLED_ADDR    0x3C
Adafruit_SSD1306 display(OLED_W, OLED_H, &Wire, -1);
bool display_ok = false;

int      btn_last_stable = HIGH;
uint32_t btn_last_change = 0;
const uint32_t BTN_DEBOUNCE_MS = 30;

enum State { WAIT_BLANK, WAIT_SAMPLE, AVERAGING, SHOW_RESULTS };
State state = WAIT_BLANK;

float blank_v  = 0.0f;
float sample_v = 0.0f;

const uint8_t  N_SAMPLES = 5;
const uint32_t SAMPLE_INTERVAL_MS = 1000;
uint8_t  avg_count = 0;
double   avg_sum   = 0.0;
uint32_t next_sample_ms = 0;
float*   avg_target = nullptr;     // where to store the average when done
State    avg_next_state = WAIT_BLANK;

// Bottom 24 px (three size-1 lines) always show blank / sample / absorbance
// so positioning stays consistent across all screens.
static void draw_bottom_readouts(bool show_blank, bool show_sample, bool show_absorbance) {
  display.setTextSize(1);

  display.setCursor(0, 32);
  display.print("Blank      = ");
  if (show_blank) display.print(blank_v, 1);

  display.setCursor(0, 40);
  display.print("Sample     = ");
  if (show_sample) display.print(sample_v, 1);

  display.setCursor(0, 56);
  display.print("Absorbance = ");
  if (show_absorbance) {
    float absorbance = (sample_v > 0.0f && blank_v > 0.0f)
                         ? log10f(blank_v / sample_v)
                         : 0.0f;
    display.print(absorbance, 4);
  }
}

static void render() {
  if (!display_ok) return;
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);

  switch (state) {
    case WAIT_BLANK:
      display.println("Insert BLANK and");
      display.println("press button");
      draw_bottom_readouts(false, false, false);
      break;

    case WAIT_SAMPLE:
      display.println("Insert SAMPLE and");
      display.println("press button");
      draw_bottom_readouts(true, false, false);
      break;

    case AVERAGING:
      display.print("Measuring ");
      display.print(avg_target == &blank_v ? "BLANK" : "SAMPLE");
      display.print(": ");
      display.print(avg_count);
      display.print('/');
      display.println(N_SAMPLES);
      // Show blank in the readouts only if it has already been averaged
      // (i.e. we're now averaging the sample).
      draw_bottom_readouts(avg_target == &sample_v, false, false);
      break;

    case SHOW_RESULTS:
      display.println("Press button for");
      display.println("new experiment");
      draw_bottom_readouts(true, true, true);
      break;
  }
  display.display();
}

static void start_averaging(float* target, State next_state) {
  avg_target = target;
  avg_next_state = next_state;
  avg_count = 0;
  avg_sum = 0.0;
  next_sample_ms = millis();   // first sample immediately
  state = AVERAGING;
}

static void on_press() {
  switch (state) {
    case WAIT_BLANK:
      start_averaging(&blank_v, WAIT_SAMPLE);
      break;

    case WAIT_SAMPLE:
      start_averaging(&sample_v, SHOW_RESULTS);
      break;

    case AVERAGING:
      // ignore presses while averaging
      break;

    case SHOW_RESULTS:
      blank_v  = 0.0f;
      sample_v = 0.0f;
      state = WAIT_BLANK;
      render();
      break;
  }
}

static void tick_averaging() {
  if (state != AVERAGING) return;
  if ((int32_t)(millis() - next_sample_ms) < 0) return;

  float v = (float)analogRead(A5);
  avg_sum += v;
  avg_count++;
  next_sample_ms += SAMPLE_INTERVAL_MS;

  Serial.print("avg "); Serial.print(avg_count);
  Serial.print('/'); Serial.print(N_SAMPLES);
  Serial.print(' '); Serial.println(v, 4);
  Serial.flush();

  if (avg_count >= N_SAMPLES) {
    *avg_target = (float)(avg_sum / N_SAMPLES);
    const char* label = (avg_target == &blank_v) ? "blank=" : "sample=";
    Serial.print(label); Serial.println(*avg_target, 4);

    state = avg_next_state;
    if (state == SHOW_RESULTS) {
      float absorbance = (sample_v > 0.0f && blank_v > 0.0f)
                           ? log10f(blank_v / sample_v)
                           : 0.0f;
      Serial.print("absorbance="); Serial.println(absorbance, 6);
    }
    Serial.flush();
  }
  render();
}

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(BTN_PIN, INPUT_PULLUP);
  analogReadResolution(14);   // nRF52840 SAADC max; 0..16383

  if (!TinyUSBDevice.isInitialized()) {
    TinyUSBDevice.begin(0);
  }
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && (millis() - t0) < 2000) { delay(10); }

  Wire.begin();
  display_ok = display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
  if (display_ok) {
    display.setRotation(2);
    display.setTextColor(SSD1306_WHITE);
    render();
  }

  Serial.print("ready, oled=");
  Serial.println(display_ok ? "ok" : "FAIL");
  Serial.flush();
}

void loop() {
  int now = digitalRead(BTN_PIN);
  uint32_t t = millis();

  if (now != btn_last_stable && (t - btn_last_change) > BTN_DEBOUNCE_MS) {
    btn_last_stable = now;
    btn_last_change = t;
    if (now == LOW) on_press();
  }

  tick_averaging();

  digitalWrite(LED_BUILTIN, btn_last_stable == LOW ? HIGH : LOW);
}
