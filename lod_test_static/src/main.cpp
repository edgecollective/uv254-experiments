#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// Button B (active-low with internal pull-up).
#define BTN_PIN     9

// LED modulation output (MICRO_PULSE side of the LED CTRL SELECT jumper).
#define LED_PIN     12

// Detector input (envelope-detected DC from the daughterboard).
#define DETECT_PIN  A5

const uint32_t LED_FREQ_HZ        = 200;
const uint32_t LED_WARMUP_MS      = 3000;
const uint8_t  N_SAMPLES          = 5;
const uint32_t SAMPLE_INTERVAL_MS = 1000;

const uint8_t  N_TRIALS = 20;

// SSD1306 on default I2C.
#define OLED_W     128
#define OLED_H     64
#define OLED_ADDR  0x3C
Adafruit_SSD1306 display(OLED_W, OLED_H, &Wire, -1);
bool display_ok = false;

enum State { READY, WARMING_UP, RUNNING, DONE };
State state = READY;

uint8_t trial = 0;                 // current trial index, 0..N_TRIALS-1
float   readings[N_TRIALS];

uint8_t  avg_count = 0;
double   avg_sum   = 0.0;
uint32_t next_sample_ms = 0;
uint32_t warmup_done_ms = 0;

int      btn_last_stable = HIGH;
uint32_t btn_last_change = 0;
const uint32_t BTN_DEBOUNCE_MS = 30;

static void led_on()  { tone(LED_PIN, LED_FREQ_HZ); }
static void led_off() { noTone(LED_PIN); digitalWrite(LED_PIN, LOW); }

static void render() {
  if (!display_ok) return;
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("LOD static test");

  switch (state) {
    case READY:
      display.setCursor(0, 16);
      display.println("Load blank,");
      display.println("press B to run");
      display.setCursor(0, 48);
      display.print("Will collect ");
      display.print(N_TRIALS);
      display.println(" reads");
      break;

    case WARMING_UP:
      display.setCursor(0, 16);
      display.println("Warming LED...");
      break;

    case RUNNING:
      display.setCursor(0, 16);
      display.print("Trial ");
      display.print(trial + 1);
      display.print('/');
      display.println(N_TRIALS);
      display.setCursor(0, 32);
      display.print("sample ");
      display.print(avg_count);
      display.print('/');
      display.println(N_SAMPLES);
      if (trial > 0) {
        display.setCursor(0, 56);
        display.print("last: ");
        display.print(readings[trial - 1], 1);
      }
      break;

    case DONE:
      display.setCursor(0, 16);
      display.println("Run complete.");
      display.setCursor(0, 32);
      display.println("CSV on serial.");
      display.setCursor(0, 48);
      display.println("Press B = rerun");
      break;
  }
  display.display();
}

static void print_csv() {
  Serial.println();
  Serial.println("=== CSV ===");
  Serial.println("trial,count");
  for (uint8_t i = 0; i < N_TRIALS; i++) {
    Serial.print(i + 1);
    Serial.print(',');
    Serial.println(readings[i], 4);
  }
  Serial.println("=== END ===");
  Serial.flush();
}

static void start_run() {
  trial = 0;
  led_on();
  warmup_done_ms = millis() + LED_WARMUP_MS;
  state = WARMING_UP;
  Serial.println();
  Serial.print("Starting back-to-back ");
  Serial.print(N_TRIALS);
  Serial.println("-trial run.");
  Serial.flush();
}

static void on_button_press() {
  if (state == READY || state == DONE) {
    render();
    start_run();
    render();
  }
}

static void tick_warmup() {
  if (state != WARMING_UP) return;
  if ((int32_t)(millis() - warmup_done_ms) < 0) return;

  // Throwaway sample so the first averaged sample is at steady state.
  float discard = (float)analogRead(DETECT_PIN);
  Serial.print("discard ");
  Serial.println(discard, 4);
  Serial.flush();

  avg_count = 0;
  avg_sum   = 0.0;
  next_sample_ms = millis();
  state = RUNNING;
  render();
}

static void on_trial_complete() {
  float v = (float)(avg_sum / N_SAMPLES);
  readings[trial] = v;
  Serial.print("trial ");
  Serial.print(trial + 1);
  Serial.print(" reading=");
  Serial.println(v, 4);
  Serial.flush();

  trial++;
  if (trial >= N_TRIALS) {
    led_off();
    state = DONE;
    print_csv();
    render();
  } else {
    // Roll straight into the next trial without resetting next_sample_ms,
    // so the 1 Hz cadence stays seamless across trial boundaries.
    avg_count = 0;
    avg_sum   = 0.0;
    render();
  }
}

static void tick_running() {
  if (state != RUNNING) return;
  if ((int32_t)(millis() - next_sample_ms) < 0) return;

  float v = (float)analogRead(DETECT_PIN);
  avg_sum += v;
  avg_count++;
  next_sample_ms += SAMPLE_INTERVAL_MS;

  Serial.print("  trial ");
  Serial.print(trial + 1);
  Serial.print(" sample ");
  Serial.print(avg_count);
  Serial.print('/');
  Serial.print(N_SAMPLES);
  Serial.print(' ');
  Serial.println(v, 4);
  Serial.flush();

  if (avg_count >= N_SAMPLES) {
    on_trial_complete();
  } else {
    render();
  }
}

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(BTN_PIN, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  analogReadResolution(14);

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
    render();
  }

  Serial.println("lod_test_static ready");
  Serial.print("Will collect ");
  Serial.print(N_TRIALS);
  Serial.println(" trials back-to-back.");
  Serial.println("Load blank, do NOT touch cuvette during run.");
  Serial.println("Press B to start.");
  Serial.flush();
}

void loop() {
  int now = digitalRead(BTN_PIN);
  uint32_t t = millis();
  if (now != btn_last_stable && (t - btn_last_change) > BTN_DEBOUNCE_MS) {
    btn_last_stable = now;
    btn_last_change = t;
    if (now == LOW) on_button_press();
  }
  tick_warmup();
  tick_running();
}
