#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_DotStar.h>

// Button B (active-low, internal pull-up) toggles the LED.
#define BTN_PIN     9

// LED modulation output. Pin 12 on the ItsyBitsy M0 = PA19, which peripheral
// function F routes to TCC0/WO[3] (driven by CC[3]). We configure TCC0
// directly so the modulation frequency is deterministic, unlike the SAMD21
// Arduino core's ISR-based tone() which we saw drop to half frequency
// intermittently.
#define LED_PIN         12
const uint32_t LED_FREQ_HZ = 200;

// Detector input (envelope-detected DC from the daughterboard).
#define DETECT_PIN  A5

const uint32_t SAMPLE_INTERVAL_MS = 1000;

// 3.3 V reference, 12-bit native ADC (0..4095, no oversampling).
const float V_REF          = 3.3f;
const float ADC_FULL_SCALE = 4095.0f;

#define OLED_W     128
#define OLED_H     64
#define OLED_ADDR  0x3C
Adafruit_SSD1306 display(OLED_W, OLED_H, &Wire, -1);
bool display_ok = false;

// Onboard DotStar (APA102): data on pin 41, clock on pin 40. Held dark to
// avoid any visible-light leakage into the detector.
Adafruit_DotStar onboard_pixel(1, PIN_DOTSTAR_DATA, PIN_DOTSTAR_CLK, DOTSTAR_BGR);

bool     led_state      = false;
uint32_t boot_ms        = 0;
uint32_t next_sample_ms = 0;
uint32_t sample_count   = 0;
float    last_adc       = 0.0f;
float    last_t_s       = 0.0f;

int      btn_last_stable = HIGH;
uint32_t btn_last_change = 0;
const uint32_t BTN_DEBOUNCE_MS = 30;

// ---------------------------------------------------------------------------
// Direct TCC0 configuration for a deterministic 50%-duty square wave on PA19.
// GCLK0 = 48 MHz (DFLL48M, set up by the Arduino SAMD core). We feed that
// into TCC0 with the /4 prescaler, giving a 12 MHz counter clock, and run
// TCC0 in NPWM (Normal PWM) mode:
//   f_out = f_counter / (PER + 1)
//   duty  = CC[3] / (PER + 1)
// For 200 Hz, 50 % duty:  PER = 59999, CC[3] = 30000.
//
// (An earlier draft used NFRQ mode, which toggles the output on counter wrap
// and gives f_out = f_counter / (2 * (PER + 1)) -- i.e. half the intended
// frequency. NPWM is unambiguous.)
// ---------------------------------------------------------------------------
static void led_pwm_init() {
  // Enable the TCC0 bus clock.
  PM->APBCMASK.reg |= PM_APBCMASK_TCC0;

  // Feed TCC0/TCC1 from GCLK0 (48 MHz).
  GCLK->CLKCTRL.reg = GCLK_CLKCTRL_ID(GCM_TCC0_TCC1)
                    | GCLK_CLKCTRL_GEN_GCLK0
                    | GCLK_CLKCTRL_CLKEN;
  while (GCLK->STATUS.bit.SYNCBUSY) {}

  // Reset TCC0.
  TCC0->CTRLA.bit.SWRST = 1;
  while (TCC0->SYNCBUSY.bit.SWRST) {}

  // /4 prescaler -> 12 MHz counter clock.
  TCC0->CTRLA.reg = TCC_CTRLA_PRESCALER_DIV4;

  // Normal PWM: output set at wrap, cleared on CC match.
  TCC0->WAVE.reg = TCC_WAVE_WAVEGEN_NPWM;
  while (TCC0->SYNCBUSY.bit.WAVE) {}

  const uint32_t counter_hz = 48000000UL / 4;
  const uint32_t period     = (counter_hz / LED_FREQ_HZ) - 1;   // 59999 for 200 Hz

  TCC0->PER.reg = period;
  while (TCC0->SYNCBUSY.bit.PER) {}
  TCC0->CC[3].reg = (period + 1) / 2;                            // 30000 -> 50 % duty
  while (TCC0->SYNCBUSY.bit.CC3) {}

  // Configure PA19 as an output owned by the peripheral MUX, but leave the
  // MUX detached for now so digitalWrite still controls the pin until led_on().
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
}

static void led_on() {
  // Route PA19 to peripheral function F (TCC0/WO[7]).
  PORT->Group[PORTA].PINCFG[19].bit.PMUXEN = 1;
  PORT->Group[PORTA].PMUX[19 >> 1].bit.PMUXO = MUX_PA19F_TCC0_WO3;

  TCC0->CTRLA.bit.ENABLE = 1;
  while (TCC0->SYNCBUSY.bit.ENABLE) {}
  led_state = true;
}

static void led_off() {
  TCC0->CTRLA.bit.ENABLE = 0;
  while (TCC0->SYNCBUSY.bit.ENABLE) {}

  // Detach the peripheral MUX so digitalWrite takes over the pin again,
  // and hold it low.
  PORT->Group[PORTA].PINCFG[19].bit.PMUXEN = 0;
  digitalWrite(LED_PIN, LOW);
  led_state = false;
}

// ---------------------------------------------------------------------------

static void render() {
  if (!display_ok) return;
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("long_led_test");

  display.setCursor(0, 12);
  display.print("LED: ");
  display.print(led_state ? "ON  " : "OFF ");
  display.print("f=");
  display.print(LED_FREQ_HZ);

  display.setCursor(0, 28);
  display.print("t = ");
  display.print(last_t_s, 1);
  display.println(" s");

  display.setCursor(0, 40);
  display.print("adc = ");
  display.println((uint32_t)(last_adc + 0.5f));

  display.setCursor(0, 52);
  display.print("V   = ");
  display.print(last_adc * V_REF / ADC_FULL_SCALE, 3);
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
  render();
}

// ---------------------------------------------------------------------------
// Coherent averaging of the detector output.
//
// The detector RC low-pass (fc ~ 88 Hz) only knocks the 200 Hz drive down by
// ~7 dB, so residual 200 Hz ripple rides on top of the "DC" envelope. If we
// take one analogRead per second we hit a random phase of that ripple every
// time, and readings scatter over the ripple's peak-to-peak range regardless
// of what the true DC is.
//
// Fix: sample at rate SUBSAMPLES_PER_PERIOD * f_LED (i.e. that many samples
// per modulation cycle) for exactly N_PERIODS_PER_SAMPLE full modulation
// periods, then average. Because the samples span an integer number of
// modulation cycles, every ripple contribution cancels in the sum -- the
// average is the DC content of the envelope, independent of the arbitrary
// starting phase.
// ---------------------------------------------------------------------------
static const uint8_t  SUBSAMPLES_PER_PERIOD = 10;   // 500 us spacing at 200 Hz
static const uint8_t  N_PERIODS_PER_SAMPLE  = 5;    // 25 ms sampling window

static float sample_coherent() {
  const uint32_t interval_us =
      (1000000UL / LED_FREQ_HZ) / SUBSAMPLES_PER_PERIOD;
  const uint16_t total = (uint16_t)SUBSAMPLES_PER_PERIOD * N_PERIODS_PER_SAMPLE;

  uint32_t next_us = micros();
  double   sum     = 0.0;
  for (uint16_t i = 0; i < total; i++) {
    while ((int32_t)(micros() - next_us) < 0) { /* busy-wait */ }
    sum += (double)analogRead(DETECT_PIN);
    next_us += interval_us;
  }
  return (float)(sum / total);
}

static void tick_sample() {
  if ((int32_t)(millis() - next_sample_ms) < 0) return;

  float t_s   = (millis() - boot_ms) / 1000.0f;
  float v     = sample_coherent();
  float volts = v * V_REF / ADC_FULL_SCALE;

  Serial.print(t_s, 3);
  Serial.print(',');
  Serial.print(v, 4);
  Serial.print(',');
  Serial.print(volts, 5);
  Serial.print(',');
  Serial.println(led_state ? 1 : 0);
  Serial.flush();

  last_t_s = t_s;
  last_adc = v;
  sample_count++;
  next_sample_ms += SAMPLE_INTERVAL_MS;

  render();
}

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(BTN_PIN, INPUT_PULLUP);

  // Turn the onboard NeoPixel off immediately.
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
  next_sample_ms = boot_ms;

  Serial.println("long_led_test ready (TCC0 direct)");
  Serial.print("LED freq = ");
  Serial.print(LED_FREQ_HZ);
  Serial.println(" Hz");
  Serial.print("Coherent averaging: ");
  Serial.print((int)(SUBSAMPLES_PER_PERIOD * N_PERIODS_PER_SAMPLE));
  Serial.print(" samples over ");
  Serial.print(N_PERIODS_PER_SAMPLE);
  Serial.println(" LED periods");
  Serial.println("B toggles LED. Sampling at 1 Hz.");
  Serial.println("t_s,adc,volts,led");
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
