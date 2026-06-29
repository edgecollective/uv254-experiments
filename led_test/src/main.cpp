#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// CALIBRATE button (active-low with internal pull-up).
#define BTN_PIN  9   // Button B

// LED modulation output (MICRO_PULSE side of the LED CTRL SELECT jumper).
#define LED_PIN  12
const uint32_t LED_FREQ_HZ = 1140;

// SSD1306 on default I2C.
#define OLED_W    128
#define OLED_H    64
#define OLED_ADDR 0x3C
Adafruit_SSD1306 display(OLED_W, OLED_H, &Wire, -1);
bool display_ok = false;

bool led_on = false;

int      btn_last_stable = HIGH;
uint32_t btn_last_change = 0;
const uint32_t BTN_DEBOUNCE_MS = 30;

static void render() {
  if (!display_ok) return;
  display.clearDisplay();
  display.setTextSize(2);
  display.setCursor(0, 24);
  display.println(led_on ? "LED ON" : "LED OFF");
  display.display();
}

static void apply_led() {
  if (led_on) {
    tone(LED_PIN, LED_FREQ_HZ);
  } else {
    noTone(LED_PIN);
    digitalWrite(LED_PIN, LOW);
  }
}

static void on_press() {
  led_on = !led_on;
  apply_led();
  render();
  Serial.print("LED ");
  Serial.println(led_on ? "ON" : "OFF");
  Serial.flush();
}

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(BTN_PIN, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

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

  Serial.print("led_test ready, oled=");
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
  digitalWrite(LED_BUILTIN, led_on ? HIGH : LOW);
}
