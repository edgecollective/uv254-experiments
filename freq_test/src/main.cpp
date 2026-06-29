#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// CALIBRATE button (active-low with internal pull-up).
#define BTN_PIN     9    // Button B

// LED modulation output (MICRO_PULSE side of the LED CTRL SELECT jumper).
#define LED_PIN     12

// Detector input (envelope-detected DC from the daughterboard).
#define DETECT_PIN  A5

// Sweep parameters.
const int32_t  FREQ_START_HZ = 1000;
const int32_t  FREQ_END_HZ   = 20;     // stop when the next step would land below this
const int32_t  FREQ_STEP_HZ  = -20;

const uint32_t SETTLE_MS         = 500;  // give LED + RC filter time to stabilize
const uint8_t  N_SAMPLES         = 50;
const uint32_t SAMPLE_INTERVAL_MS = 10;

// SSD1306 on default I2C.
#define OLED_W     128
#define OLED_H     64
#define OLED_ADDR  0x3C
Adafruit_SSD1306 display(OLED_W, OLED_H, &Wire, -1);
bool display_ok = false;

int      btn_last_stable = HIGH;
uint32_t btn_last_change = 0;
const uint32_t BTN_DEBOUNCE_MS = 30;

static void render(const char* line1, const char* line2 = nullptr, int32_t freq = -1) {
  if (!display_ok) return;
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("Freq sweep");
  display.setCursor(0, 16);
  display.println(line1);
  if (line2) {
    display.setCursor(0, 32);
    display.println(line2);
  }
  if (freq >= 0) {
    display.setCursor(0, 48);
    display.print("Freq: ");
    display.print(freq);
    display.println(" Hz");
  }
  display.display();
}

static float measure_at(int32_t freq_hz) {
  tone(LED_PIN, freq_hz);
  delay(SETTLE_MS);
  double sum = 0.0;
  for (uint8_t i = 0; i < N_SAMPLES; i++) {
    sum += (double)analogRead(DETECT_PIN);
    delay(SAMPLE_INTERVAL_MS);
  }
  return (float)(sum / N_SAMPLES);
}

static void run_sweep() {
  Serial.println("count,freq");
  Serial.flush();

  for (int32_t f = FREQ_START_HZ; f >= FREQ_END_HZ; f += FREQ_STEP_HZ) {
    render("Sweeping...", nullptr, f);
    float avg = measure_at(f);
    Serial.print(avg, 2);
    Serial.print(',');
    Serial.println(f);
    Serial.flush();
  }

  noTone(LED_PIN);
  digitalWrite(LED_PIN, LOW);
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
    render("Press B to start");
  }

  Serial.println("freq_test ready");
  Serial.flush();
}

void loop() {
  int now = digitalRead(BTN_PIN);
  uint32_t t = millis();
  if (now != btn_last_stable && (t - btn_last_change) > BTN_DEBOUNCE_MS) {
    btn_last_stable = now;
    btn_last_change = t;
    if (now == LOW) {
      render("Sweeping...");
      run_sweep();
      render("Done.", "Press B to rerun");
    }
  }
}
