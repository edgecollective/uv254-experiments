#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_DotStar.h>

// Button B (active-low, internal pull-up) toggles the LED.
#define BTN_PIN     9

// LED modulation output. Pin 12 (PA19) -> TCC0/WO[3] via mux F. TCC0 in NPWM
// mode gives a deterministic 200 Hz, 50%-duty square wave.
#define LED_PIN         12
const uint32_t LED_FREQ_HZ = 200;

// Detector input.
#define DETECT_PIN  A5

// Averaging schedule: 20 samples at 10 Hz = one 20-point average every 2 s.
const uint32_t SUBSAMPLE_INTERVAL_MS = 100;   // 10 Hz
const uint16_t SAMPLES_PER_AVERAGE   = 20;    // 2 s window

// 3.3 V reference, 12-bit native ADC (0..4095).
const float V_REF          = 3.3f;
const float ADC_FULL_SCALE = 4095.0f;

#define OLED_W     128
#define OLED_H     64
#define OLED_ADDR  0x3C
Adafruit_SSD1306 display(OLED_W, OLED_H, &Wire, -1);
bool display_ok = false;

Adafruit_DotStar onboard_pixel(1, PIN_DOTSTAR_DATA, PIN_DOTSTAR_CLK, DOTSTAR_BGR);

bool     led_state       = false;
uint32_t boot_ms         = 0;
uint32_t next_subsample_ms = 0;
uint32_t average_count   = 0;
uint16_t sample_index    = 0;   // 0..SAMPLES_PER_AVERAGE-1
double   sample_sum      = 0.0;
float    last_avg        = 0.0f;
float    last_t_s        = 0.0f;

int      btn_last_stable = HIGH;
uint32_t btn_last_change = 0;
const uint32_t BTN_DEBOUNCE_MS = 30;

// TCC0 NPWM setup constants.
static const uint32_t TCC0_COUNTER_HZ = 48000000UL / 4;
static const uint32_t TCC0_PER        = (TCC0_COUNTER_HZ / LED_FREQ_HZ) - 1;
static const uint32_t TCC0_CC3        = (TCC0_PER + 1) / 2;

static void led_pwm_init() {
  PM->APBCMASK.reg |= PM_APBCMASK_TCC0;

  GCLK->CLKCTRL.reg = GCLK_CLKCTRL_ID(GCM_TCC0_TCC1)
                    | GCLK_CLKCTRL_GEN_GCLK0
                    | GCLK_CLKCTRL_CLKEN;
  while (GCLK->STATUS.bit.SYNCBUSY) {}

  TCC0->CTRLA.bit.SWRST = 1;
  while (TCC0->SYNCBUSY.bit.SWRST) {}

  TCC0->CTRLA.reg = TCC_CTRLA_PRESCALER_DIV4;
  TCC0->WAVE.reg  = TCC_WAVE_WAVEGEN_NPWM;
  while (TCC0->SYNCBUSY.bit.WAVE) {}

  TCC0->PER.reg = TCC0_PER;
  while (TCC0->SYNCBUSY.bit.PER) {}
  TCC0->CC[3].reg = TCC0_CC3;
  while (TCC0->SYNCBUSY.bit.CC3) {}

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
}

static void led_on() {
  PORT->Group[PORTA].PINCFG[19].bit.PMUXEN = 1;
  PORT->Group[PORTA].PMUX[19 >> 1].bit.PMUXO = MUX_PA19F_TCC0_WO3;

  TCC0->CTRLA.bit.ENABLE = 1;
  while (TCC0->SYNCBUSY.bit.ENABLE) {}
  led_state = true;
}

static void led_off() {
  TCC0->CTRLA.bit.ENABLE = 0;
  while (TCC0->SYNCBUSY.bit.ENABLE) {}

  PORT->Group[PORTA].PINCFG[19].bit.PMUXEN = 0;
  digitalWrite(LED_PIN, LOW);
  led_state = false;
}

static void reset_average() {
  sample_index    = 0;
  sample_sum      = 0.0;
  next_subsample_ms = millis();
}

static void render() {
  if (!display_ok) return;
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("led_average");

  display.setCursor(0, 12);
  display.print("LED: ");
  display.print(led_state ? "ON  " : "OFF ");
  display.print("f=");
  display.print(LED_FREQ_HZ);

  display.setCursor(0, 24);
  display.print("bin ");
  display.print(sample_index);
  display.print('/');
  display.println(SAMPLES_PER_AVERAGE);

  display.setCursor(0, 36);
  display.print("t = ");
  display.print(last_t_s, 1);
  display.println(" s");

  display.setCursor(0, 48);
  display.print("avg = ");
  display.println(last_avg, 2);

  display.setCursor(0, 56);
  display.print("V   = ");
  display.print(last_avg * V_REF / ADC_FULL_SCALE, 4);
  display.println("V");
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
  reset_average();
  render();
}

static void tick_subsample() {
  if ((int32_t)(millis() - next_subsample_ms) < 0) return;

  float v = (float)analogRead(DETECT_PIN);
  sample_sum += (double)v;
  sample_index++;
  next_subsample_ms += SUBSAMPLE_INTERVAL_MS;

  if (sample_index >= SAMPLES_PER_AVERAGE) {
    float avg = (float)(sample_sum / SAMPLES_PER_AVERAGE);
    float t_s = (millis() - boot_ms) / 1000.0f;

    Serial.print(t_s, 3);
    Serial.print(',');
    Serial.print(avg, 4);
    Serial.print(',');
    Serial.print(avg * V_REF / ADC_FULL_SCALE, 5);
    Serial.print(',');
    Serial.println(led_state ? 1 : 0);
    Serial.flush();

    last_avg = avg;
    last_t_s = t_s;
    average_count++;

    sample_index = 0;
    sample_sum   = 0.0;
  }

  render();
}

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(BTN_PIN,  INPUT_PULLUP);

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
    render();
  }

  led_pwm_init();
  led_on();

  boot_ms = millis();
  reset_average();

  Serial.println("led_average ready (TCC0 direct)");
  Serial.print("LED freq = ");
  Serial.print(LED_FREQ_HZ);
  Serial.println(" Hz");
  Serial.print("Averaging ");
  Serial.print(SAMPLES_PER_AVERAGE);
  Serial.print(" samples @ 10 Hz -> one row every ");
  Serial.print(SAMPLES_PER_AVERAGE * SUBSAMPLE_INTERVAL_MS);
  Serial.println(" ms");
  Serial.println("B toggles LED. CSV cols = t_s,adc,volts,led");
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
  tick_subsample();
}
