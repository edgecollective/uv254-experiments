// Absorbance measurement firmware, ItsyBitsy nRF52840, ver7.0 variant.
//
// ver7.0 changes vs ver6.1: replaces the single-standard multiplicative
// correction (A_corr = k * A_raw) with a TWO-standard AFFINE correction
// (A_corr = m * A_raw + b) using two fluorescein standards. Defaults are
// std_lo = 0.010 cm^-1 and std_hi = 0.100 cm^-1.
//
// Rationale: with a single anchor the correction can only rescale around the
// origin, so a small additive offset in the raw response (~+7 mAU intercept
// seen in fluorescein data) survives the correction. A second anchor near
// the low end pins the intercept as well as the slope.
//
// The OLED shows both A_raw and A_corr so raw and corrected values are
// simultaneously visible; the affine params m and b are logged over serial
// (and shown in compact form on the bottom lines of the OLED).
//
// Enforces a linear FOUR-step calibration workflow. After the LED warm-up:
//
//   STEP 1/4 BLANK    Insert blank,     press B -> blank measurement
//   STEP 2/4 STD LO   Insert 0.010 std, press B -> low-std measurement
//   STEP 3/4 STD HI   Insert 0.100 std, press B -> high-std measurement
//                                             -> derive m and b
//   STEP 4/4 SAMPLE   Insert sample,    press B -> affine-corrected abs
//                                             (stays in this loop; each B
//                                              measures another sample)
//
// Buttons at every prompt:
//   B = "inserted, start measuring"
//   A = restart from STEP 1 (clears all calibration). During a running
//       measurement, A aborts and returns to STEP 1.
//
// LED is driven by an external 555 (soldered on); the micro does NOT drive
// the LED pin. During warm-up we sample at 10 Hz and stream one CSV row
// every 5 s so the log has an unbroken record, but buttons are disabled.
//
// Each measurement = 3 x 5 s settle + 10 x 5 s avg = 65 s.
//
// Correction:
//   A_lo_meas = log10(I_blank / I_std_lo)
//   A_hi_meas = log10(I_blank / I_std_hi)
//   m = (std_hi_abs - std_lo_abs) / (A_hi_meas - A_lo_meas)
//   b = std_lo_abs - m * A_lo_meas
//   A_corr = m * A_raw + b
//
// Serial (USB CDC) commands, line-terminated:
//   stdlo <float>   override the low standard's known absorbance
//                   (default 0.010). Live-updates m,b if both standards
//                   have been measured.
//   stdhi <float>   override the high standard's known absorbance
//                   (default 0.100). Live-updates m,b similarly.
//   reset           restart from STEP 1 (ignored during a measurement).
//
// CSV cols: t_s, phase, subphase, round, adc, volts
//   phase    in {warmup, blank, std_lo, std_hi, sample}
//   subphase in {warmup, settle, avg}
//   round    = 1-indexed within the current subphase

#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#define BTN_A_PIN     7   // A -> restart / abort
#define BTN_B_PIN     9   // B -> advance / start
#define DETECT_PIN    A5

const uint32_t SUBSAMPLE_INTERVAL_MS = 100;   // 10 Hz
const uint16_t SAMPLES_PER_ROUND     = 50;    // 5 s per round
const uint8_t  SETTLE_ROUNDS         = 3;     // 15 s settle per measurement
const uint8_t  AVG_ROUNDS            = 10;    // 50 s avg window per measurement
const uint32_t WARMUP_MS             = 120000UL;  // 2 minutes

const uint32_t ROUND_MS  = (uint32_t)SAMPLES_PER_ROUND * SUBSAMPLE_INTERVAL_MS;
const uint32_t SETTLE_MS = (uint32_t)SETTLE_ROUNDS * ROUND_MS;

const float V_REF          = 3.0f;
const float ADC_FULL_SCALE = 4095.0f;
static inline float counts_to_volts(float c) { return c * V_REF / ADC_FULL_SCALE; }

#define OLED_W     128
#define OLED_H     64
#define OLED_ADDR  0x3C
Adafruit_SSD1306 display(OLED_W, OLED_H, &Wire, -1);
bool display_ok = false;

enum State {
  STATE_WARMUP,
  STATE_AWAIT_BLANK,
  STATE_AWAIT_STD_LO,
  STATE_AWAIT_STD_HI,
  STATE_AWAIT_SAMPLE,
  STATE_MEASURING
};
enum Phase    { PHASE_NONE, PHASE_WARMUP, PHASE_BLANK, PHASE_STD_LO, PHASE_STD_HI, PHASE_SAMPLE };
enum Subphase { SUB_NONE, SUB_WARMUP, SUB_SETTLE, SUB_AVG };

State    state    = STATE_WARMUP;
Phase    phase    = PHASE_WARMUP;
Subphase subphase = SUB_WARMUP;

uint8_t  round_index_in_sub = 0;
uint16_t warmup_round       = 0;
uint16_t sample_index       = 0;
double   sample_sum         = 0.0;
uint32_t next_subsample_ms  = 0;

uint32_t warmup_start_ms    = 0;
uint32_t subphase_start_ms  = 0;

float    avg_rounds[10] = {0};
float    last_warmup_avg = 0.0f;
float    last_live_counts = 0.0f;
uint32_t last_live_update_ms = 0;

float    i_blank    = 0.0f;
float    i_std_lo   = 0.0f;
float    i_std_hi   = 0.0f;
float    i_sample   = 0.0f;
float    absorbance_raw = 0.0f;
float    absorbance     = 0.0f;
bool     have_blank   = false;
bool     have_std_lo  = false;
bool     have_std_hi  = false;
bool     have_sample  = false;

// Known absorbances for the two standards, and derived affine correction.
float    std_lo_abs = 0.010f;
float    std_hi_abs = 0.100f;
float    m_slope    = 1.0f;
float    b_intercept = 0.0f;

char     serial_buf[64];
uint8_t  serial_len = 0;

uint32_t boot_ms = 0;

struct Button { uint8_t pin; int last_stable; uint32_t last_change; };
Button btn_a = {BTN_A_PIN, HIGH, 0};
Button btn_b = {BTN_B_PIN, HIGH, 0};
const uint32_t BTN_DEBOUNCE_MS = 30;

static const char* phase_name(Phase p) {
  switch (p) {
    case PHASE_WARMUP: return "warmup";
    case PHASE_BLANK:  return "blank";
    case PHASE_STD_LO: return "std_lo";
    case PHASE_STD_HI: return "std_hi";
    case PHASE_SAMPLE: return "sample";
    default:           return "-";
  }
}
static const char* phase_name_upper(Phase p) {
  switch (p) {
    case PHASE_WARMUP: return "WARMUP";
    case PHASE_BLANK:  return "BLANK";
    case PHASE_STD_LO: return "STD LO";
    case PHASE_STD_HI: return "STD HI";
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

// Recompute the affine (m, b) from the two measured standards. Falls back to
// m=1, b=0 if the two anchors are too close together (denominator near zero).
static void recompute_affine() {
  if (!have_std_lo || !have_std_hi || !have_blank) {
    m_slope = 1.0f;
    b_intercept = 0.0f;
    return;
  }
  if (i_std_lo <= 0.0f || i_std_hi <= 0.0f || i_blank <= 0.0f) {
    m_slope = 1.0f;
    b_intercept = 0.0f;
    return;
  }
  float a_lo_meas = log10f(i_blank / i_std_lo);
  float a_hi_meas = log10f(i_blank / i_std_hi);
  float denom = a_hi_meas - a_lo_meas;
  if (fabsf(denom) < 1e-6f) {
    m_slope = 1.0f;
    b_intercept = 0.0f;
    Serial.println("# affine: A_hi_meas ~ A_lo_meas; m,b left at 1,0");
    return;
  }
  m_slope = (std_hi_abs - std_lo_abs) / denom;
  b_intercept = std_lo_abs - m_slope * a_lo_meas;
}

static void render() {
  if (!display_ok) return;
  display.clearDisplay();
  display.setTextSize(1);

  // Top-right tag, always 6 chars wide so it fits alongside the widest
  // left-side headers ("STEP 4/4 SAMPLE" / "Measure: SAMPLE", both 15 chars =
  // 90 px). "CAL" prefix indicates the 2-pt affine correction is armed.
  display.setCursor(OLED_W - 6 * 6, 0);      // x = 92
  if (have_std_lo && have_std_hi) {
    display.print("CAL7.0");
  } else {
    display.print("ver7.0");
  }

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
  } else if (state == STATE_AWAIT_BLANK) {
    display.println("STEP 1/4 BLANK");
    display.setCursor(0, 14);
    display.println("Insert blank");
    display.setCursor(0, 24);
    display.println("A:Restart  B:Start");
  } else if (state == STATE_AWAIT_STD_LO) {
    display.println("STEP 2/4 STD LO");
    display.setCursor(0, 14);
    display.print("Insert lo A=");
    display.println(std_lo_abs, 3);
    display.setCursor(0, 24);
    display.println("A:Restart  B:Start");
  } else if (state == STATE_AWAIT_STD_HI) {
    display.println("STEP 3/4 STD HI");
    display.setCursor(0, 14);
    display.print("Insert hi A=");
    display.println(std_hi_abs, 3);
    display.setCursor(0, 24);
    display.println("A:Restart  B:Start");
  } else if (state == STATE_AWAIT_SAMPLE) {
    display.println("STEP 4/4 SAMPLE");
    display.setCursor(0, 14);
    display.println("Insert sample");
    display.setCursor(0, 24);
    display.println("A:Restart  B:Start");
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

  // Bottom info block: shown once we've moved past STEP 1. Four rows at 8 px
  // spacing so raw and affine-corrected absorbance are simultaneously visible.
  // Compact affine params m and b are shown alongside on the same rows.
  // When either standard has not yet been measured, m=1.000 and b=0.000, so
  // the two absorbance values will be equal -- the very fact that they match
  // is the visual cue that no affine correction is being applied yet.
  if (state != STATE_WARMUP && state != STATE_AWAIT_BLANK) {
    display.setCursor(0, 32);
    display.print("Blank= ");
    if (have_blank) {
      display.print((uint32_t)(i_blank + 0.5f));
      display.print(' ');
      display.print(counts_to_volts(i_blank), 3);
      display.print('V');
    }

    display.setCursor(0, 40);
    display.print("Sample=");
    if (have_sample) {
      display.print((uint32_t)(i_sample + 0.5f));
      display.print(' ');
      display.print(counts_to_volts(i_sample), 3);
      display.print('V');
    }

    display.setCursor(0, 48);
    display.print("A_raw= ");
    if (have_sample && have_blank) {
      display.print(absorbance_raw, 4);
      display.print(" m=");
      display.print(m_slope, 2);
    }

    display.setCursor(0, 56);
    display.print("A_corr=");
    if (have_sample && have_blank) {
      display.print(absorbance, 4);
      // b shown as signed int mAU (b_intercept * 1000) so it stays 3-4 chars
      // even when A_corr goes negative. Full-precision b is in the serial log.
      int b_mAU = (int)(b_intercept * 1000.0f
                        + (b_intercept >= 0 ? 0.5f : -0.5f));
      display.print(" b=");
      if (b_mAU >= 0) display.print('+');
      display.print(b_mAU);
    }
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

static void restart_to_blank() {
  have_blank = false;
  have_std_lo = false;
  have_std_hi = false;
  have_sample = false;
  m_slope = 1.0f;
  b_intercept = 0.0f;
  i_blank = 0.0f;
  i_std_lo = 0.0f;
  i_std_hi = 0.0f;
  i_sample = 0.0f;
  absorbance = 0.0f;
  absorbance_raw = 0.0f;
  phase = PHASE_NONE;
  subphase = SUB_NONE;
  state = STATE_AWAIT_BLANK;
  render();
}

static void start_measurement(Phase p) {
  phase = p;
  state = STATE_MEASURING;

  if (p == PHASE_BLANK) {
    have_blank = false;
    have_std_lo = false;
    have_std_hi = false;
    have_sample = false;
    m_slope = 1.0f;
    b_intercept = 0.0f;
  } else if (p == PHASE_STD_LO) {
    have_std_lo = false;
    have_std_hi = false;
    have_sample = false;
    m_slope = 1.0f;
    b_intercept = 0.0f;
  } else if (p == PHASE_STD_HI) {
    have_std_hi = false;
    have_sample = false;
  } else if (p == PHASE_SAMPLE) {
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
  Serial.println("s; restarting to STEP 1 (BLANK)");
  Serial.flush();
  restart_to_blank();
}

static void on_a_press() {
  if (state == STATE_WARMUP) return;
  if (state == STATE_MEASURING) { abort_measurement(); return; }
  Serial.println("# restart to STEP 1 (BLANK)");
  Serial.flush();
  restart_to_blank();
}

static void on_b_press() {
  if (state == STATE_WARMUP) return;
  if (state == STATE_MEASURING) return;
  if (state == STATE_AWAIT_BLANK)   { start_measurement(PHASE_BLANK);  return; }
  if (state == STATE_AWAIT_STD_LO)  { start_measurement(PHASE_STD_LO); return; }
  if (state == STATE_AWAIT_STD_HI)  { start_measurement(PHASE_STD_HI); return; }
  if (state == STATE_AWAIT_SAMPLE)  { start_measurement(PHASE_SAMPLE); return; }
}

static void finish_measurement() {
  double sum = 0.0;
  for (uint8_t i = 0; i < AVG_ROUNDS; i++) sum += avg_rounds[i];
  float final_val = (float)(sum / AVG_ROUNDS);

  Serial.print("# ");
  Serial.print(phase_name(phase));
  Serial.print(" final = ");
  Serial.print(final_val, 4);
  Serial.print(" counts, ");
  Serial.print(counts_to_volts(final_val), 5);
  Serial.print(" V (mean of ");
  Serial.print(AVG_ROUNDS);
  Serial.print(" avg rounds)");

  if (phase == PHASE_BLANK) {
    i_blank = final_val;
    have_blank = true;
    Serial.println("");
    phase = PHASE_NONE;
    subphase = SUB_NONE;
    state = STATE_AWAIT_STD_LO;
  } else if (phase == PHASE_STD_LO) {
    i_std_lo = final_val;
    float a_lo_meas = (i_std_lo > 0.0f && i_blank > 0.0f)
                        ? log10f(i_blank / i_std_lo) : 0.0f;
    if (a_lo_meas > 1e-6f) {
      have_std_lo = true;
      Serial.print(", A_lo_meas=");
      Serial.print(a_lo_meas, 6);
      Serial.print(", std_lo_abs=");
      Serial.println(std_lo_abs, 4);
    } else {
      have_std_lo = false;
      Serial.println(", A_lo_meas ~0 or negative; std_lo rejected");
    }
    phase = PHASE_NONE;
    subphase = SUB_NONE;
    state = STATE_AWAIT_STD_HI;
  } else if (phase == PHASE_STD_HI) {
    i_std_hi = final_val;
    float a_hi_meas = (i_std_hi > 0.0f && i_blank > 0.0f)
                        ? log10f(i_blank / i_std_hi) : 0.0f;
    if (a_hi_meas > 1e-6f && have_std_lo) {
      have_std_hi = true;
      recompute_affine();
      Serial.print(", A_hi_meas=");
      Serial.print(a_hi_meas, 6);
      Serial.print(", std_hi_abs=");
      Serial.print(std_hi_abs, 4);
      Serial.print(", m=");
      Serial.print(m_slope, 6);
      Serial.print(", b=");
      Serial.println(b_intercept, 6);
    } else {
      have_std_hi = false;
      m_slope = 1.0f;
      b_intercept = 0.0f;
      Serial.println(", A_hi_meas ~0 or no std_lo; m,b left at 1,0");
    }
    phase = PHASE_NONE;
    subphase = SUB_NONE;
    state = STATE_AWAIT_SAMPLE;
  } else if (phase == PHASE_SAMPLE) {
    i_sample = final_val;
    have_sample = true;
    absorbance_raw = (i_sample > 0.0f && i_blank > 0.0f)
                       ? log10f(i_blank / i_sample) : 0.0f;
    absorbance = m_slope * absorbance_raw + b_intercept;
    Serial.print(", A_raw=");
    Serial.print(absorbance_raw, 6);
    Serial.print(", m=");
    Serial.print(m_slope, 6);
    Serial.print(", b=");
    Serial.print(b_intercept, 6);
    Serial.print(", A_corr=");
    Serial.println(absorbance, 6);
    phase = PHASE_NONE;
    subphase = SUB_NONE;
    state = STATE_AWAIT_SAMPLE;
  } else {
    Serial.println("");
    phase = PHASE_NONE;
    subphase = SUB_NONE;
    state = STATE_AWAIT_BLANK;
  }
  Serial.flush();
  render();
}

static void finish_warmup() {
  Serial.print("# warmup complete at t=");
  Serial.print((millis() - boot_ms) / 1000.0f, 3);
  Serial.println("s; buttons enabled -> STEP 1 (BLANK)");
  Serial.flush();
  state = STATE_AWAIT_BLANK;
  phase = PHASE_NONE;
  subphase = SUB_NONE;
  render();
}

static void tick_sampling() {
  // Sample during WARMUP and MEASURING; AWAIT_* states are idle.
  if (state != STATE_WARMUP && state != STATE_MEASURING) return;
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

static void handle_serial_line(char* line) {
  while (*line == ' ' || *line == '\t') line++;
  if (*line == '\0') return;

  if (strncmp(line, "stdlo", 5) == 0 && (line[5] == ' ' || line[5] == '\t')) {
    float v = atof(line + 6);
    if (v > 0.0f && v < 10.0f) {
      std_lo_abs = v;
      Serial.print("# std_lo_abs set to ");
      Serial.println(std_lo_abs, 4);
      if (have_std_lo && have_std_hi) {
        recompute_affine();
        Serial.print("# m updated to ");
        Serial.print(m_slope, 6);
        Serial.print(", b updated to ");
        Serial.println(b_intercept, 6);
      }
      render();
    } else {
      Serial.println("# stdlo: value must be > 0 and < 10");
    }
  } else if (strncmp(line, "stdhi", 5) == 0 && (line[5] == ' ' || line[5] == '\t')) {
    float v = atof(line + 6);
    if (v > 0.0f && v < 10.0f) {
      std_hi_abs = v;
      Serial.print("# std_hi_abs set to ");
      Serial.println(std_hi_abs, 4);
      if (have_std_lo && have_std_hi) {
        recompute_affine();
        Serial.print("# m updated to ");
        Serial.print(m_slope, 6);
        Serial.print(", b updated to ");
        Serial.println(b_intercept, 6);
      }
      render();
    } else {
      Serial.println("# stdhi: value must be > 0 and < 10");
    }
  } else if (strcmp(line, "reset") == 0) {
    if (state == STATE_MEASURING) {
      Serial.println("# reset ignored: measurement in progress");
      return;
    }
    Serial.println("# reset -> restart to STEP 1 (BLANK)");
    restart_to_blank();
  } else {
    Serial.print("# unknown command: '");
    Serial.print(line);
    Serial.println("' ; usage: 'stdlo <float>', 'stdhi <float>', 'reset'");
  }
}

static void poll_serial() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      serial_buf[serial_len] = '\0';
      handle_serial_line(serial_buf);
      serial_len = 0;
    } else if (serial_len < sizeof(serial_buf) - 1) {
      serial_buf[serial_len++] = c;
    } else {
      serial_len = 0;
    }
  }
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
  pinMode(BTN_A_PIN, INPUT_PULLUP);
  pinMode(BTN_B_PIN, INPUT_PULLUP);

  analogReadResolution(12);

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

  Serial.println("# ver7.0 (nRF52840) ready -- 2-point AFFINE cal");
  Serial.println("# LED driven by external 555; micro does not drive LED");
  Serial.print  ("# warm-up = ");
  Serial.print  (WARMUP_MS / 1000);
  Serial.println(" s (buttons disabled during warm-up)");
  Serial.println("# workflow: STEP 1 blank -> STEP 2 std_lo -> STEP 3 std_hi -> STEP 4 sample loop");
  Serial.println("# B advances at each prompt; A restarts to STEP 1");
  Serial.println("# measurement = 3 settle + 10 avg rounds x 5 s = 65 s");
  Serial.println("# final = mean of 10 avg rounds");
  Serial.println("# CSV cols = t_s,phase,subphase,round,adc,volts");
  Serial.print  ("# std_lo_abs default = ");
  Serial.println(std_lo_abs, 4);
  Serial.print  ("# std_hi_abs default = ");
  Serial.println(std_hi_abs, 4);
  Serial.println("# correction: A_corr = m * A_raw + b");
  Serial.println("# serial commands: 'stdlo <float>', 'stdhi <float>', 'reset'");
  Serial.print  ("# warmup begin at t=");
  Serial.print  ((warmup_start_ms - boot_ms) / 1000.0f, 3);
  Serial.println("s");
  Serial.flush();

  render();
}

void loop() {
  poll_serial();
  check_button(btn_a, on_a_press);
  check_button(btn_b, on_b_press);
  tick_sampling();
}
