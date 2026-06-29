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

// 200 Hz sits at the detector chain's bandpass peak (see freq_test/exp2.png).
const uint32_t LED_FREQ_HZ        = 200;
const uint32_t LED_WARMUP_MS      = 3000;   // doubled; 1.5 s wasn't enough for the LED to settle
const uint8_t  N_SAMPLES          = 5;
const uint32_t SAMPLE_INTERVAL_MS = 1000;

const uint8_t  N_TRIALS = 20;

// SSD1306 on default I2C.
#define OLED_W     128
#define OLED_H     64
#define OLED_ADDR  0x3C
Adafruit_SSD1306 display(OLED_W, OLED_H, &Wire, -1);
bool display_ok = false;

enum State { READY, WARMING_UP, AVERAGING, DONE };
State state = READY;

uint8_t trial = 0;                 // number of completed trials (0..N_TRIALS)
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
  display.println("LOD blank test");

  switch (state) {
    case READY:
      display.setCursor(0, 16);
      display.print("Trial ");
      display.print(trial + 1);
      display.print('/');
      display.println(N_TRIALS);
      display.setCursor(0, 32);
      display.println("Load blank,");
      display.println("press B");
      if (trial > 0) {
        display.setCursor(0, 56);
        display.print("last: ");
        display.print(readings[trial - 1], 1);
      }
      break;

    case WARMING_UP:
      display.setCursor(0, 16);
      display.print("Trial ");
      display.print(trial + 1);
      display.print('/');
      display.println(N_TRIALS);
      display.setCursor(0, 32);
      display.println("Warming LED...");
      break;

    case AVERAGING:
      display.setCursor(0, 16);
      display.print("Trial ");
      display.print(trial + 1);
      display.print('/');
      display.println(N_TRIALS);
      display.setCursor(0, 32);
      display.print("Measuring: ");
      display.print(avg_count);
      display.print('/');
      display.println(N_SAMPLES);
      break;

    case DONE:
      display.setCursor(0, 16);
      display.println("All trials done");
      display.setCursor(0, 32);
      display.println("CSV on serial.");
      display.setCursor(0, 48);
      display.println("Press B = restart");
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

static void start_warmup() {
  led_on();
  warmup_done_ms = millis() + LED_WARMUP_MS;
  state = WARMING_UP;
}

static void start_averaging() {
  avg_count = 0;
  avg_sum = 0.0;
  next_sample_ms = millis();
  state = AVERAGING;
}

static void on_button_press() {
  if (state == READY && trial < N_TRIALS) {
    Serial.print("trial ");
    Serial.print(trial + 1);
    Serial.println(": measuring");
    Serial.flush();
    start_warmup();
    render();
  } else if (state == DONE) {
    trial = 0;
    state = READY;
    render();
    Serial.println();
    Serial.println("Restarted. Load blank, press B for trial 1.");
    Serial.flush();
  }
}

static void on_measurement_done() {
  float v = (float)(avg_sum / N_SAMPLES);
  readings[trial] = v;
  Serial.print("trial ");
  Serial.print(trial + 1);
  Serial.print(" reading=");
  Serial.println(v, 4);
  Serial.flush();

  led_off();
  trial++;
  if (trial >= N_TRIALS) {
    state = DONE;
    print_csv();
  } else {
    state = READY;
  }
  render();
}

static void tick_warmup() {
  if (state != WARMING_UP) return;
  if ((int32_t)(millis() - warmup_done_ms) < 0) return;

  // Throwaway read so any residual settling doesn't get counted in the average.
  float discard = (float)analogRead(DETECT_PIN);
  Serial.print("  discard ");
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

  Serial.print("  sample ");
  Serial.print(avg_count);
  Serial.print('/');
  Serial.print(N_SAMPLES);
  Serial.print(' ');
  Serial.println(v, 4);
  Serial.flush();

  if (avg_count >= N_SAMPLES) {
    on_measurement_done();
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

  Serial.println("lod_test ready");
  Serial.print("Will collect ");
  Serial.print(N_TRIALS);
  Serial.println(" blank readings.");
  Serial.println("Load blank in cuvette, press B for trial 1.");
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
  tick_averaging();
}
