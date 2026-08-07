#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// Button B (active-low with internal pull-up) toggles the LED.
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

bool     led_state      = false;
uint32_t boot_ms        = 0;
uint32_t next_sample_ms = 0;
uint32_t sample_count   = 0;
float    last_count     = 0.0f;
float    last_t_s       = 0.0f;

int      btn_last_stable = HIGH;
uint32_t btn_last_change = 0;
const uint32_t BTN_DEBOUNCE_MS = 30;

static void led_on()  { tone(LED_PIN, LED_FREQ_HZ); led_state = true;  }
static void led_off() { noTone(LED_PIN); digitalWrite(LED_PIN, LOW); led_state = false; }

static void render() {
  if (!display_ok) return;
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("LED bake (M0)");

  display.setCursor(0, 16);
  display.print("LED: ");
  display.println(led_state ? "ON" : "OFF");

  display.setCursor(0, 28);
  display.print("t = ");
  display.print(last_t_s, 1);
  display.println(" s");

  display.setCursor(0, 40);
  display.print("adc = ");
  display.println(last_count, 1);

  display.setCursor(0, 52);
  display.print("n = ");
  display.println(sample_count);
  display.display();
}

static void on_button_press() {
  if (led_state) {
    led_off();
    Serial.println("# LED OFF");
  } else {
    led_on();
    Serial.println("# LED ON");
  }
  Serial.flush();
  render();
}

static void tick_sample() {
  if ((int32_t)(millis() - next_sample_ms) < 0) return;

  float t_s = (millis() - boot_ms) / 1000.0f;
  float v   = (float)analogRead(DETECT_PIN);

  Serial.print(t_s, 3);
  Serial.print(',');
  Serial.print(v, 4);
  Serial.print(',');
  Serial.println(led_state ? 1 : 0);
  Serial.flush();

  last_t_s   = t_s;
  last_count = v;
  sample_count++;
  next_sample_ms += SAMPLE_INTERVAL_MS;

  render();
}

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(BTN_PIN, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

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

  boot_ms = millis();
  next_sample_ms = boot_ms;

  Serial.println("led_bake (M0) ready");
  Serial.print("LED freq = ");
  Serial.print(LED_FREQ_HZ);
  Serial.println(" Hz");
  Serial.println("B toggles LED. Sampling at 1 Hz.");
  Serial.println("t_s,adc,led");
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
  tick_sample();
}
