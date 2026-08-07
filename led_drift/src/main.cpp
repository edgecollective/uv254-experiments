// LED drift logger, ItsyBitsy nRF52840.
//
// LED is driven by an external 555 (soldered) - the micro does not touch the
// LED pin. Button B toggles logging on/off. While logging, we sample the
// detector at 10 Hz and emit one CSV row per 50-sample (5 s) average.
//
// CSV cols: t_s, round, adc, volts
//   t_s   = seconds since power-on
//   round = 1-indexed 5-second bin since logging was (re)started
//
// This gives a clean long-window trace to analyze LED drift.

#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#define BTN_PIN     9   // Button B toggles logging on/off
#define DETECT_PIN  A5

const uint32_t SUBSAMPLE_INTERVAL_MS = 100;  // 10 Hz
const uint16_t SAMPLES_PER_ROUND     = 50;   // 5 s per row

// nRF52840 SAADC, 12-bit native (0..4095). Default reference is
// AR_INTERNAL_3_0 -> 3.0 V full scale.
const float V_REF          = 3.0f;
const float ADC_FULL_SCALE = 4095.0f;
static inline float counts_to_volts(float c) { return c * V_REF / ADC_FULL_SCALE; }

#define OLED_W     128
#define OLED_H     64
#define OLED_ADDR  0x3C
Adafruit_SSD1306 display(OLED_W, OLED_H, &Wire, -1);
bool display_ok = false;

bool     logging          = false;
uint32_t log_start_ms     = 0;
uint32_t round_index      = 0;   // completed rounds since log start
uint16_t sample_index     = 0;
double   sample_sum       = 0.0;
uint32_t next_subsample_ms = 0;
float    last_avg         = 0.0f;

uint32_t boot_ms = 0;

int      btn_last_stable = HIGH;
uint32_t btn_last_change = 0;
const uint32_t BTN_DEBOUNCE_MS = 30;

static void render() {
  if (!display_ok) return;
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("LED drift logger");

  display.setCursor(0, 12);
  display.print("state: ");
  display.println(logging ? "RUNNING" : "stopped");

  display.setCursor(0, 24);
  if (logging) {
    uint32_t elapsed_s = (millis() - log_start_ms) / 1000;
    display.print("elapsed: ");
    display.print(elapsed_s);
    display.println("s");
  } else {
    display.println("B: start/stop");
  }

  display.setCursor(0, 36);
  display.print("round: ");
  display.println(round_index);

  display.setCursor(0, 48);
  display.print("bin ");
  display.print(sample_index);
  display.print('/');
  display.println(SAMPLES_PER_ROUND);

  display.setCursor(0, 56);
  display.print("last= ");
  if (round_index > 0) {
    display.print(last_avg, 1);
    display.print(' ');
    display.print(counts_to_volts(last_avg), 3);
    display.print('V');
  }
  display.display();
}

static void start_logging() {
  logging = true;
  log_start_ms = millis();
  round_index = 0;
  sample_index = 0;
  sample_sum = 0.0;
  next_subsample_ms = millis();

  Serial.print("# LED drift logging START at t=");
  Serial.print((log_start_ms - boot_ms) / 1000.0f, 3);
  Serial.println("s");
  Serial.flush();
  render();
}

static void stop_logging() {
  logging = false;
  Serial.print("# LED drift logging STOP  at t=");
  Serial.print((millis() - boot_ms) / 1000.0f, 3);
  Serial.print("s (");
  Serial.print(round_index);
  Serial.println(" rounds emitted)");
  Serial.flush();
  render();
}

static void on_button_press() {
  if (logging) stop_logging();
  else         start_logging();
}

static void tick_sampling() {
  if (!logging) return;
  if ((int32_t)(millis() - next_subsample_ms) < 0) return;

  float v = (float)analogRead(DETECT_PIN);
  sample_sum += (double)v;
  sample_index++;
  next_subsample_ms += SUBSAMPLE_INTERVAL_MS;

  if (sample_index >= SAMPLES_PER_ROUND) {
    float avg = (float)(sample_sum / SAMPLES_PER_ROUND);
    round_index++;
    float t_s = (millis() - boot_ms) / 1000.0f;

    Serial.print(t_s, 3);
    Serial.print(',');
    Serial.print(round_index);
    Serial.print(',');
    Serial.print(avg, 4);
    Serial.print(',');
    Serial.println(counts_to_volts(avg), 5);
    Serial.flush();

    last_avg = avg;
    sample_index = 0;
    sample_sum = 0.0;
  }

  render();
}

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(BTN_PIN, INPUT_PULLUP);

  analogReadResolution(12);   // 0..4095

  if (!TinyUSBDevice.isInitialized()) {
    TinyUSBDevice.begin(0);
  }
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

  Serial.println("# led_drift (nRF52840) ready");
  Serial.println("# LED driven by external 555");
  Serial.println("# 10 Hz sampling, 50-sample avg -> one row every 5 s");
  Serial.println("# CSV cols = t_s,round,adc,volts");
  Serial.println("# B toggles logging on/off");
  Serial.flush();

  render();
}

void loop() {
  int now = digitalRead(BTN_PIN);
  uint32_t t = millis();
  if (now != btn_last_stable && (t - btn_last_change) > BTN_DEBOUNCE_MS) {
    btn_last_stable = now;
    btn_last_change = t;
    if (now == LOW) on_button_press();
  }
  tick_sampling();
}
