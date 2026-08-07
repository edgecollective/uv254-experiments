#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_DotStar.h>

// Button B (active-low, internal pull-up) toggles the LED.
#define BTN_PIN     9

// LED modulation output. Pin 12 on the ItsyBitsy M0 = PA19, mux F -> TCC0/WO[3]
// (driven by CC[3]). We configure TCC0 directly for a deterministic 200 Hz
// 50%-duty square wave in NPWM mode.
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

// TCC0 NPWM configuration -- must match the values programmed into TCC0->PER
// and TCC0->CC[3] below, since sample_phase_locked() references them.
static const uint32_t TCC0_COUNTER_HZ = 48000000UL / 4;              // 12 MHz
static const uint32_t TCC0_PER        = (TCC0_COUNTER_HZ / LED_FREQ_HZ) - 1;  // 59999
static const uint32_t TCC0_CC3        = (TCC0_PER + 1) / 2;                    // 30000

// Phase we sample at, in TCC0 counter units. PER/4 puts us in the middle of
// the LED-HIGH half of the modulation cycle, well clear of the transition
// edges at COUNT = 0 and COUNT = CC[3].
static const uint32_t PHASE_TARGET = TCC0_PER / 4;                   // ~14999

// Number of phase-locked samples averaged per 1 Hz output row. Each sample
// costs one modulation period (5 ms at 200 Hz), so 10 samples = 50 ms.
static const uint8_t  N_PHASE_SAMPLES = 10;

static void led_pwm_init() {
  PM->APBCMASK.reg |= PM_APBCMASK_TCC0;

  GCLK->CLKCTRL.reg = GCLK_CLKCTRL_ID(GCM_TCC0_TCC1)
                    | GCLK_CLKCTRL_GEN_GCLK0
                    | GCLK_CLKCTRL_CLKEN;
  while (GCLK->STATUS.bit.SYNCBUSY) {}

  TCC0->CTRLA.bit.SWRST = 1;
  while (TCC0->SYNCBUSY.bit.SWRST) {}

  TCC0->CTRLA.reg = TCC_CTRLA_PRESCALER_DIV4;

  // NPWM: output set at wrap, cleared on CC[3] match.
  TCC0->WAVE.reg = TCC_WAVE_WAVEGEN_NPWM;
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

// ---------------------------------------------------------------------------
// Phase-locked ADC sampling.
//
// On the SAMD21 the TCC COUNT register is read-synchronised: to see the live
// value, write CTRLBSET.CMD = READSYNC and wait for SYNCBUSY.CTRLB to clear
// before reading COUNT.reg. tcc0_count() wraps that dance.
//
// sample_phase_locked_avg() takes n samples, each fired at the same phase
// (COUNT == PHASE_TARGET) of the LED modulation cycle. It waits for the
// counter to wrap past PHASE_TARGET and then climb to it before each read,
// so every sample lands on the same slice of the 200 Hz waveform regardless
// of when the routine was entered. The residual 200 Hz ripple riding on the
// envelope-detected DC output therefore contributes a constant offset rather
// than random sample-to-sample jitter.
//
// If the LED is off, TCC0 is disabled and the COUNT read never advances --
// fall back to plain, unlocked reads in that case.
// ---------------------------------------------------------------------------
static uint32_t tcc0_count() {
  TCC0->CTRLBSET.reg = TCC_CTRLBSET_CMD_READSYNC;
  while (TCC0->SYNCBUSY.bit.CTRLB) {}
  return TCC0->COUNT.reg;
}

static float sample_phase_locked_avg(uint8_t n) {
  double sum = 0.0;
  for (uint8_t i = 0; i < n; i++) {
    // Wait for the counter to be past the target, then wait for wrap
    // (count drops below target), then wait until count climbs to target.
    while (tcc0_count() <  PHASE_TARGET) { /* advance to >= target */ }
    while (tcc0_count() >= PHASE_TARGET) { /* wait for wrap */ }
    while (tcc0_count() <  PHASE_TARGET) { /* wait for climb */ }
    sum += (double)analogRead(DETECT_PIN);
  }
  return (float)(sum / n);
}

static float sample_reading() {
  if (led_state) {
    return sample_phase_locked_avg(N_PHASE_SAMPLES);
  } else {
    // LED off -> TCC0 disabled, phase-lock would spin forever. Take a plain
    // read; this is the dark baseline anyway.
    double sum = 0.0;
    for (uint8_t i = 0; i < N_PHASE_SAMPLES; i++) {
      sum += (double)analogRead(DETECT_PIN);
    }
    return (float)(sum / N_PHASE_SAMPLES);
  }
}

// ---------------------------------------------------------------------------

static void render() {
  if (!display_ok) return;
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("phase_lock");

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
  display.println(last_adc, 2);

  display.setCursor(0, 52);
  display.print("V   = ");
  display.print(last_adc * V_REF / ADC_FULL_SCALE, 4);
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

static void tick_sample() {
  if ((int32_t)(millis() - next_sample_ms) < 0) return;

  float t_s   = (millis() - boot_ms) / 1000.0f;
  float v     = sample_reading();
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
  next_sample_ms = boot_ms;

  Serial.println("phase_lock ready (TCC0 direct)");
  Serial.print("LED freq = ");
  Serial.print(LED_FREQ_HZ);
  Serial.println(" Hz");
  Serial.print("Phase-locked sampling: ");
  Serial.print(N_PHASE_SAMPLES);
  Serial.print(" samples at TCC0 COUNT = ");
  Serial.println(PHASE_TARGET);
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
