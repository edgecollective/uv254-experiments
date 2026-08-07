#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// Button B (active-low with internal pull-up).
#define BTN_PIN     9

// LED modulation output (MICRO_PULSE side of the LED CTRL SELECT jumper).
#define LED_PIN     12
const uint32_t LED_FREQ_HZ = 200;

// Detector input (envelope-detected DC from the daughterboard).
#define DETECT_PIN  A5

const uint32_t SAMPLE_INTERVAL_MS = 1000;

#define OLED_W     128
#define OLED_H     64
#define OLED_ADDR  0x3C
Adafruit_SSD1306 display(OLED_W, OLED_H, &Wire, -1);
bool display_ok = false;

enum State { READY, RUNNING, STOPPED };
State state = READY;

uint32_t run_start_ms  = 0;
uint32_t next_sample_ms = 0;
uint32_t sample_count  = 0;
float    last_count    = 0.0f;
float    last_t_s      = 0.0f;

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
  display.println("LED bake");

  switch (state) {
    case READY:
      display.setCursor(0, 16);
      display.println("Press B to start");
      display.setCursor(0, 32);
      display.print("f=");
      display.print(LED_FREQ_HZ);
      display.println(" Hz");
      break;

    case RUNNING:
      display.setCursor(0, 16);
      display.print("t = ");
      display.print(last_t_s, 1);
      display.println(" s");
      display.setCursor(0, 32);
      display.print("adc = ");
      display.println(last_count, 1);
      display.setCursor(0, 48);
      display.print("n = ");
      display.println(sample_count);
      display.setCursor(0, 56);
      display.print("B = stop");
      break;

    case STOPPED:
      display.setCursor(0, 16);
      display.println("Stopped.");
      display.setCursor(0, 32);
      display.print("last t = ");
      display.print(last_t_s, 1);
      display.println(" s");
      display.setCursor(0, 48);
      display.print("n = ");
      display.println(sample_count);
      display.setCursor(0, 56);
      display.print("B = restart");
      break;
  }
  display.display();
}

static void start_run() {
  led_on();
  run_start_ms   = millis();
  next_sample_ms = run_start_ms;
  sample_count   = 0;
  last_count     = 0.0f;
  last_t_s       = 0.0f;
  state = RUNNING;

  Serial.println();
  Serial.println("=== CSV ===");
  Serial.println("t_s,adc");
  Serial.flush();
}

static void stop_run() {
  led_off();
  state = STOPPED;
  Serial.println("=== END ===");
  Serial.flush();
}

static void on_button_press() {
  if (state == RUNNING) {
    stop_run();
    render();
  } else {
    start_run();
    render();
  }
}

static void tick_running() {
  if (state != RUNNING) return;
  if ((int32_t)(millis() - next_sample_ms) < 0) return;

  float t_s = (millis() - run_start_ms) / 1000.0f;
  float v   = (float)analogRead(DETECT_PIN);

  Serial.print(t_s, 3);
  Serial.print(',');
  Serial.println(v, 4);
  Serial.flush();

  last_t_s    = t_s;
  last_count  = v;
  sample_count++;
  next_sample_ms += SAMPLE_INTERVAL_MS;

  render();
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

  Serial.println("led_bake ready");
  Serial.print("LED freq = ");
  Serial.print(LED_FREQ_HZ);
  Serial.println(" Hz");
  Serial.println("Press B to start; press B again to stop.");
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
  tick_running();
}
