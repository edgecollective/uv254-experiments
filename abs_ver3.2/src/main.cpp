// Absorbance measurement firmware, ItsyBitsy nRF52840, ver3.2 variant.
//
// LED is driven by an external 555 (soldered on) - the micro does NOT drive
// the LED pin. On boot the firmware runs a 2-minute LED warm-up (the LED
// itself is always on because the 555 keeps running - the "warm-up" here is
// really just waiting until the thermal drift on the plateau is negligible;
// the led_drift_july7 analysis showed that by 2 min drift is ~0.02 %/min).
// During warm-up we still sample at 10 Hz and stream one CSV row every 5 s
// so the log has an unbroken record, but the CALIBRATE / COMPUTE buttons
// are disabled until warm-up completes.
//
// After warm-up:
//   Button B (pin 9) -> CALIBRATE  (measure I_blank)
//   Button A (pin 7) -> COMPUTE    (measure I_sample, then A = log10(I_blank / I_sample))
//
// Each measurement is 3 x 5 s settle + 10 x 5 s avg = 65 s. The final value
// is the mean of the 10 avg rounds. (Trimmed from 5 settle rounds after the
// july_18_2026_blank_LOD analysis showed no residual equilibration by round 3.)
//
// CSV cols: t_s, phase, subphase, round, adc, volts
//   phase    ∈ {warmup, blank, sample}
//   subphase ∈ {warmup, settle, avg}
//   round    = 1-indexed within the current subphase

#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#define BTN_CAL_PIN   7   // Button A -> CALIBRATE (blank)
#define BTN_MEAS_PIN  9   // Button B -> COMPUTE   (sample -> absorbance)
#define DETECT_PIN    A5

const uint32_t SUBSAMPLE_INTERVAL_MS = 100;  // 10 Hz
const uint16_t SAMPLES_PER_ROUND     = 50;   // 5 s per round
const uint8_t  SETTLE_ROUNDS         = 3;    // 15 s settle per measurement
const uint8_t  AVG_ROUNDS            = 10;   // 50 s avg window per measurement
const uint32_t WARMUP_MS             = 120000UL;  // 2 minutes

const uint32_t ROUND_MS  = (uint32_t)SAMPLES_PER_ROUND * SUBSAMPLE_INTERVAL_MS;
const uint32_t SETTLE_MS = (uint32_t)SETTLE_ROUNDS * ROUND_MS;

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

enum State    { STATE_WARMUP, STATE_READY, STATE_MEASURING };
enum Phase    { PHASE_NONE, PHASE_WARMUP, PHASE_BLANK, PHASE_SAMPLE };
enum Subphase { SUB_NONE, SUB_WARMUP, SUB_SETTLE, SUB_AVG };

State    state    = STATE_WARMUP;
Phase    phase    = PHASE_WARMUP;
Subphase subphase = SUB_WARMUP;

uint8_t  round_index_in_sub = 0;   // 0..(N-1) within current subphase, for measurements
uint16_t warmup_round       = 0;   // 1-indexed round counter for warm-up rows
uint16_t sample_index       = 0;   // 0..SAMPLES_PER_ROUND-1
double   sample_sum         = 0.0;
uint32_t next_subsample_ms  = 0;

uint32_t warmup_start_ms    = 0;
uint32_t subphase_start_ms  = 0;

float    avg_rounds[10] = {0};
float    last_warmup_avg = 0.0f;   // for OLED
float    last_live_counts = 0.0f;  // most recent raw sample, refreshed at 1 Hz for OLED
uint32_t last_live_update_ms = 0;

float    i_blank    = 0.0f;
float    i_sample   = 0.0f;
float    absorbance = 0.0f;
bool     have_calibration = false;
bool     have_sample      = false;

uint32_t boot_ms = 0;

struct Button {
  uint8_t pin;
  int     last_stable;
  uint32_t last_change;
};
Button btn_cal  = {BTN_CAL_PIN,  HIGH, 0};
Button btn_meas = {BTN_MEAS_PIN, HIGH, 0};
const uint32_t BTN_DEBOUNCE_MS = 30;

static const char* phase_name(Phase p) {
  switch (p) {
    case PHASE_WARMUP: return "warmup";
    case PHASE_BLANK:  return "blank";
    case PHASE_SAMPLE: return "sample";
    default:           return "-";
  }
}

static const char* phase_name_upper(Phase p) {
  switch (p) {
    case PHASE_WARMUP: return "WARMUP";
    case PHASE_BLANK:  return "BLANK";
    case PHASE_SAMPLE: return "SAMPLE";
    default:           return "-";
  }
}

static const char* subphase_name(Subphase s) {
  switch (s) {
    case SUB_WARMUP: return "warmup";
    case SUB_SETTLE: return "settle";
    case SUB_AVG:    return "avg";
    default:         return "-";
  }
}

static float running_avg_mean() {
  if (round_index_in_sub == 0) return 0.0f;
  double sum = 0.0;
  for (uint8_t i = 0; i < round_index_in_sub; i++) sum += avg_rounds[i];
  return (float)(sum / round_index_in_sub);
}

static void render() {
  if (!display_ok) return;
  display.clearDisplay();
  display.setTextSize(1);

  // Version tag, top-right corner ("ver3.2" = 6 chars * 6 px = 36 px wide).
  display.setCursor(OLED_W - 6 * 6, 0);
  display.print("ver3.2");

  display.setCursor(0, 0);

  if (state == STATE_WARMUP) {
    display.println("WARM-UP");
    display.setCursor(0, 10);
    uint32_t elapsed = millis() - warmup_start_ms;
    uint32_t remain_s = (elapsed >= WARMUP_MS)
                         ? 0 : (WARMUP_MS - elapsed + 999) / 1000;
    display.print("Remaining: ");
    display.print(remain_s);
    display.println("s");
    display.setCursor(0, 22);
    display.print("Counts: ");
    display.println((uint32_t)(last_live_counts + 0.5f));
    display.setCursor(0, 32);
    display.print("Volts: ");
    display.println(counts_to_volts(last_live_counts), 3);
    display.setCursor(0, 54);
    display.println("Buttons Disabled");
  } else if (state == STATE_READY) {
    display.println("Ready");
    display.setCursor(0, 18);
    display.println(have_calibration ? "A:Cal  B:Compute"
                                     : "A: Calibrate");
  } else {  // STATE_MEASURING
    display.print("Measure: ");
    display.println(phase_name_upper(phase));

    display.setCursor(0, 10);
    if (subphase == SUB_SETTLE) {
      uint32_t elapsed = millis() - subphase_start_ms;
      uint32_t remain_s = (elapsed >= SETTLE_MS)
                           ? 0 : (SETTLE_MS - elapsed + 999) / 1000;
      display.print("SETTLING ");
      display.print(remain_s);
      display.println("s left");
    } else if (subphase == SUB_AVG) {
      display.print("AVERAGING ");
      display.print(round_index_in_sub);
      display.print('/');
      display.println(AVG_ROUNDS);

      display.setCursor(0, 20);
      display.print("mean= ");
      if (round_index_in_sub > 0) {
        float m = running_avg_mean();
        display.print(m, 1);
        display.print(' ');
        display.print(counts_to_volts(m), 4);
        display.print('V');
      }
    }
  }

  if (state != STATE_WARMUP) {
    display.setCursor(0, 32);
    display.print("Blank= ");
    if (have_calibration) {
      display.print((uint32_t)(i_blank + 0.5f));
      display.print(' ');
      display.print(counts_to_volts(i_blank), 3);
      display.print('V');
    }

    display.setCursor(0, 42);
    display.print("Sample= ");
    if (have_sample) {
      display.print((uint32_t)(i_sample + 0.5f));
      display.print(' ');
      display.print(counts_to_volts(i_sample), 3);
      display.print('V');
    }

    display.setCursor(0, 54);
    display.print("Absorb= ");
    if (have_sample && have_calibration) display.print(absorbance, 4);
  }

  display.display();
}

static void enter_subphase(Subphase s) {
  subphase = s;
  round_index_in_sub = 0;
  sample_index = 0;
  sample_sum = 0.0;
  subphase_start_ms = millis();
  next_subsample_ms = millis();
  if (s == SUB_AVG) {
    for (uint8_t i = 0; i < AVG_ROUNDS; i++) avg_rounds[i] = 0.0f;
  }
  Serial.print("# ");
  Serial.print(phase_name(phase));
  Serial.print(' ');
  Serial.print(subphase_name(s));
  Serial.print(" begin at t=");
  Serial.print((subphase_start_ms - boot_ms) / 1000.0f, 3);
  Serial.println("s");
  Serial.flush();
  render();
}

static void start_measurement(Phase p) {
  phase = p;
  state = STATE_MEASURING;

  if (p == PHASE_BLANK) {
    have_calibration = false;
    have_sample = false;
  }

  Serial.print("# ");
  Serial.print(phase_name(p));
  Serial.print(" begin at t=");
  Serial.print((millis() - boot_ms) / 1000.0f, 3);
  Serial.println("s");
  Serial.flush();

  enter_subphase(SUB_SETTLE);
}

static void abort_measurement() {
  Serial.print("# ");
  Serial.print(phase_name(phase));
  Serial.print(" ABORTED at t=");
  Serial.print((millis() - boot_ms) / 1000.0f, 3);
  Serial.println("s");
  Serial.flush();
  // Preserve any prior i_blank / have_calibration; only drop the in-progress run.
  phase = PHASE_NONE;
  subphase = SUB_NONE;
  state = STATE_READY;
  round_index_in_sub = 0;
  sample_index = 0;
  sample_sum = 0.0;
  render();
}

static void on_calibrate_press() {
  if (state == STATE_WARMUP) return;   // silently ignored during warm-up
  if (state == STATE_MEASURING) { abort_measurement(); return; }
  start_measurement(PHASE_BLANK);
}

static void on_compute_press() {
  if (state != STATE_READY) return;
  if (!have_calibration) return;
  start_measurement(PHASE_SAMPLE);
}

static void finish_measurement() {
  double sum = 0.0;
  for (uint8_t i = 0; i < AVG_ROUNDS; i++) sum += avg_rounds[i];
  float final_val = (float)(sum / AVG_ROUNDS);

  if (phase == PHASE_BLANK) {
    i_blank = final_val;
    have_calibration = true;
  } else if (phase == PHASE_SAMPLE) {
    i_sample = final_val;
    have_sample = true;
    absorbance = (i_sample > 0.0f && i_blank > 0.0f)
                   ? log10f(i_blank / i_sample)
                   : 0.0f;
  }

  Serial.print("# ");
  Serial.print(phase_name(phase));
  Serial.print(" final = ");
  Serial.print(final_val, 4);
  Serial.print(" counts, ");
  Serial.print(counts_to_volts(final_val), 5);
  Serial.print(" V (mean of ");
  Serial.print(AVG_ROUNDS);
  Serial.println(" avg rounds)");

  if (phase == PHASE_SAMPLE) {
    Serial.print("# absorbance = log10(");
    Serial.print(i_blank, 4);
    Serial.print(" / ");
    Serial.print(i_sample, 4);
    Serial.print(") = ");
    Serial.println(absorbance, 6);
  }
  Serial.flush();

  phase = PHASE_NONE;
  subphase = SUB_NONE;
  state = STATE_READY;
  render();
}

static void finish_warmup() {
  Serial.print("# warmup complete at t=");
  Serial.print((millis() - boot_ms) / 1000.0f, 3);
  Serial.println("s; buttons enabled");
  Serial.flush();
  state = STATE_READY;
  phase = PHASE_NONE;
  subphase = SUB_NONE;
  render();
}

static void tick_sampling() {
  // We sample continuously in both WARMUP and MEASURING (so the log has an
  // unbroken record); READY state has no sampling.
  if (state == STATE_READY) return;
  if ((int32_t)(millis() - next_subsample_ms) < 0) return;

  float v = (float)analogRead(DETECT_PIN);
  if ((millis() - last_live_update_ms) >= 1000) {
    last_live_counts = v;
    last_live_update_ms = millis();
  }
  sample_sum += (double)v;
  sample_index++;
  next_subsample_ms += SUBSAMPLE_INTERVAL_MS;

  if (sample_index >= SAMPLES_PER_ROUND) {
    float avg = (float)(sample_sum / SAMPLES_PER_ROUND);
    uint32_t now = millis();
    float t_s = (now - boot_ms) / 1000.0f;

    if (state == STATE_WARMUP) {
      warmup_round++;
      last_warmup_avg = avg;
      Serial.print(t_s, 3);
      Serial.print(",warmup,warmup,");
      Serial.print(warmup_round);
      Serial.print(',');
      Serial.print(avg, 4);
      Serial.print(',');
      Serial.println(counts_to_volts(avg), 5);
      Serial.flush();

      sample_index = 0;
      sample_sum = 0.0;

      if ((now - warmup_start_ms) >= WARMUP_MS) {
        finish_warmup();
        return;
      }
    } else {  // STATE_MEASURING
      uint8_t rnum = round_index_in_sub + 1;
      Serial.print(t_s, 3);
      Serial.print(',');
      Serial.print(phase_name(phase));
      Serial.print(',');
      Serial.print(subphase_name(subphase));
      Serial.print(',');
      Serial.print(rnum);
      Serial.print(',');
      Serial.print(avg, 4);
      Serial.print(',');
      Serial.println(counts_to_volts(avg), 5);
      Serial.flush();

      if (subphase == SUB_AVG && round_index_in_sub < AVG_ROUNDS) {
        avg_rounds[round_index_in_sub] = avg;
      }
      round_index_in_sub++;
      sample_index = 0;
      sample_sum = 0.0;

      uint8_t rounds_in_this_sub =
        (subphase == SUB_SETTLE) ? SETTLE_ROUNDS : AVG_ROUNDS;
      if (round_index_in_sub >= rounds_in_this_sub) {
        if (subphase == SUB_SETTLE) {
          enter_subphase(SUB_AVG);
          return;
        } else {
          finish_measurement();
          return;
        }
      }
    }
  }

  render();
}

static void check_button(Button& b, void (*handler)()) {
  int now = digitalRead(b.pin);
  uint32_t t = millis();
  if (now != b.last_stable && (t - b.last_change) > BTN_DEBOUNCE_MS) {
    b.last_stable = now;
    b.last_change = t;
    if (now == LOW) handler();
  }
}

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(BTN_CAL_PIN,  INPUT_PULLUP);
  pinMode(BTN_MEAS_PIN, INPUT_PULLUP);

  analogReadResolution(12);   // 0..4095, native SAADC resolution

  if (!TinyUSBDevice.isInitialized()) {
    TinyUSBDevice.begin(0);
  }
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && (millis() - t0) < 2000) { delay(10); }

  Wire.begin();
  display_ok = display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
  if (display_ok) {
    display.setRotation(2);
    display.setTextColor(SSD1306_WHITE);
  }

  boot_ms = millis();
  warmup_start_ms = millis();
  next_subsample_ms = millis();
  sample_index = 0;
  sample_sum = 0.0;
  warmup_round = 0;

  Serial.println("# ver3.2 (nRF52840) ready");
  Serial.println("# LED driven by external 555; micro does not drive LED");
  Serial.print  ("# warm-up = ");
  Serial.print  (WARMUP_MS / 1000);
  Serial.println(" s (buttons disabled during warm-up)");
  Serial.println("# measurement = 3 settle + 10 avg rounds x 5 s = 65 s");
  Serial.println("# final = mean of 10 avg rounds");
  Serial.println("# CSV cols = t_s,phase,subphase,round,adc,volts");
  Serial.println("# B=CALIBRATE (blank), A=COMPUTE (sample -> absorbance)");
  Serial.print  ("# warmup begin at t=");
  Serial.print  ((warmup_start_ms - boot_ms) / 1000.0f, 3);
  Serial.println("s");
  Serial.flush();

  render();
}

void loop() {
  check_button(btn_cal,  on_calibrate_press);
  check_button(btn_meas, on_compute_press);
  tick_sampling();
}
