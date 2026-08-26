// Absorbance measurement firmware, ItsyBitsy nRF52840, ver8.0 variant.
//
// ver8.0 changes vs ver7.0: extends the 2-anchor exact affine correction to
// a THREE-anchor LEAST-SQUARES linear fit. Same model as ver7.0:
//
//     A_corr = m * A_raw + b
//
// but now (m, b) are the least-squares best fit through three
// (A_meas, A_known) pairs rather than the exact solution through two.
// Defaults: std_lo = 0.020 cm^-1, std_mid = 0.100 cm^-1, std_hi = 0.150 cm^-1.
//
// Rationale: in the Aug 12 run the 2-point fit's slope was ~8% off optimal
// because the low anchor (0.010) was near the noise floor and its residual
// propagated straight into m. A third anchor lets the fit average anchor
// noise instead of amplifying it, and it also gives one residual you can
// inspect after the fact (any anchor whose residual is much larger than the
// others is a data-quality flag).
//
// The OLED shows A_raw and A_corr side-by-side; the affine params m and b
// are logged over serial and shown in compact form on the OLED bottom rows.
//
// Enforces a linear FIVE-step calibration workflow. After the LED warm-up:
//
//   STEP 1/5 BLANK    Insert blank,     press B -> blank measurement
//   STEP 2/5 LO       Insert lo  std,   press B -> low-std measurement
//   STEP 3/5 MID      Insert mid std,   press B -> mid-std measurement
//   STEP 4/5 HI       Insert hi  std,   press B -> high-std measurement
//                                             -> derive m and b (LSQ)
//   STEP 5/5 SAMPLE   Insert sample,    press B -> corrected absorbance
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
// Correction (n = 3):
//   x_i = A_i_meas = log10(I_blank / I_std_i)
//   y_i = std_i_abs  (known absorbance for anchor i)
//   m = (n * SUM(x_i*y_i) - SUM(x_i)*SUM(y_i)) / (n * SUM(x_i^2) - SUM(x_i)^2)
//   b = (SUM(y_i) - m * SUM(x_i)) / n
//   A_corr = m * A_raw + b
//
// Serial (USB CDC) commands, line-terminated:
//   stdlo  <float>   override the low-standard known absorbance
//                    (default 0.020). Live-updates m,b if all 3 measured.
//   stdmid <float>   override the mid-standard known absorbance
//                    (default 0.100). Live-updates m,b similarly.
//   stdhi  <float>   override the high-standard known absorbance
//                    (default 0.150). Live-updates m,b similarly.
//   reset            restart from STEP 1 (ignored during a measurement).
//
// CSV cols: t_s, phase, subphase, round, adc, volts
//   phase    in {warmup, blank, std_lo, std_mid, std_hi, sample}
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
  STATE_AWAIT_STD_MID,
  STATE_AWAIT_STD_HI,
  STATE_AWAIT_SAMPLE,
  STATE_MEASURING
};
enum Phase    { PHASE_NONE, PHASE_WARMUP, PHASE_BLANK,
                PHASE_STD_LO, PHASE_STD_MID, PHASE_STD_HI, PHASE_SAMPLE };
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
float    i_std_mid  = 0.0f;
float    i_std_hi   = 0.0f;
float    i_sample   = 0.0f;
float    absorbance_raw = 0.0f;
float    absorbance     = 0.0f;
bool     have_blank   = false;
bool     have_std_lo  = false;
bool     have_std_mid = false;
bool     have_std_hi  = false;
bool     have_sample  = false;

// Known absorbances for the three standards, and derived LSQ correction.
float    std_lo_abs  = 0.020f;
float    std_mid_abs = 0.100f;
float    std_hi_abs  = 0.150f;
float    m_slope     = 1.0f;
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
    case PHASE_WARMUP:  return "warmup";
    case PHASE_BLANK:   return "blank";
    case PHASE_STD_LO:  return "std_lo";
    case PHASE_STD_MID: return "std_mid";
    case PHASE_STD_HI:  return "std_hi";
    case PHASE_SAMPLE:  return "sample";
    default:            return "-";
  }
}
// Short upper labels chosen so "Measure: <label>" (max 15 chars including
// the "Measure: " prefix that renders on the top row) fits within the 21-char
// row width without colliding with the top-right "verX.X" tag. "STD MID"
// would be 16 chars there, so mid/lo/hi are shown as plain "MID"/"LO"/"HI".
static const char* phase_name_upper(Phase p) {
  switch (p) {
    case PHASE_WARMUP:  return "WARMUP";
    case PHASE_BLANK:   return "BLANK";
    case PHASE_STD_LO:  return "LO";
    case PHASE_STD_MID: return "MID";
    case PHASE_STD_HI:  return "HI";
    case PHASE_SAMPLE:  return "SAMPLE";
    default:            return "-";
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

// Recompute the LSQ affine (m, b) from the three measured standards.
// Falls back to m=1, b=0 if not all three anchors are measured or the
// three x_i are degenerate (variance ~0).
static void recompute_affine() {
  m_slope = 1.0f;
  b_intercept = 0.0f;

  if (!have_blank || !have_std_lo || !have_std_mid || !have_std_hi) return;
  if (i_blank <= 0.0f || i_std_lo <= 0.0f
      || i_std_mid <= 0.0f || i_std_hi <= 0.0f) return;

  float x[3];
  float y[3];
  x[0] = log10f(i_blank / i_std_lo);   y[0] = std_lo_abs;
  x[1] = log10f(i_blank / i_std_mid);  y[1] = std_mid_abs;
  x[2] = log10f(i_blank / i_std_hi);   y[2] = std_hi_abs;

  const int n = 3;
  float sum_x = 0.0f, sum_y = 0.0f, sum_xx = 0.0f, sum_xy = 0.0f;
  for (int i = 0; i < n; i++) {
    sum_x  += x[i];
    sum_y  += y[i];
    sum_xx += x[i] * x[i];
    sum_xy += x[i] * y[i];
  }
  float denom = (float)n * sum_xx - sum_x * sum_x;
  if (fabsf(denom) < 1e-9f) {
    Serial.println("# lsq: x variance ~0; m,b left at 1,0");
    return;
  }
  m_slope = ((float)n * sum_xy - sum_x * sum_y) / denom;
  b_intercept = (sum_y - m_slope * sum_x) / (float)n;
}

// Log the per-anchor residuals (y_i - (m*x_i + b)) once (m,b) are known.
// A big residual on one anchor is a data-quality flag worth noting in the
// serial record.
static void log_lsq_residuals() {
  if (!have_blank || !have_std_lo || !have_std_mid || !have_std_hi) return;
  float x_lo  = log10f(i_blank / i_std_lo);
  float x_mid = log10f(i_blank / i_std_mid);
  float x_hi  = log10f(i_blank / i_std_hi);
  float r_lo  = std_lo_abs  - (m_slope * x_lo  + b_intercept);
  float r_mid = std_mid_abs - (m_slope * x_mid + b_intercept);
  float r_hi  = std_hi_abs  - (m_slope * x_hi  + b_intercept);
  Serial.print("# lsq residuals (mAU): lo=");
  Serial.print(r_lo * 1000.0f, 2);
  Serial.print(", mid=");
  Serial.print(r_mid * 1000.0f, 2);
  Serial.print(", hi=");
  Serial.println(r_hi * 1000.0f, 2);
}

static void render() {
  if (!display_ok) return;
  display.clearDisplay();
  display.setTextSize(1);

  // Top-right tag, 6 chars wide (x=92, ends at x=127) so it never collides
  // with the widest left-side header used here ("STEP 5/5 SAMPLE" / "Measure:
  // SAMPLE", both 15 chars = 90 px, ending at x=89 with a 2 px gap).
  // "CAL" prefix indicates the 3-pt LSQ correction is armed.
  display.setCursor(OLED_W - 6 * 6, 0);      // x = 92
  if (have_std_lo && have_std_mid && have_std_hi) {
    display.print("CAL8.0");
  } else {
    display.print("ver8.0");
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
    display.println("STEP 1/5 BLANK");
    display.setCursor(0, 14);
    display.println("Insert blank");
    display.setCursor(0, 24);
    display.println("A:Restart  B:Start");
  } else if (state == STATE_AWAIT_STD_LO) {
    display.println("STEP 2/5 LO");
    display.setCursor(0, 14);
    display.print("Insert lo A=");
    display.println(std_lo_abs, 3);
    display.setCursor(0, 24);
    display.println("A:Restart  B:Start");
  } else if (state == STATE_AWAIT_STD_MID) {
    display.println("STEP 3/5 MID");
    display.setCursor(0, 14);
    display.print("Insert mid A=");
    display.println(std_mid_abs, 3);
    display.setCursor(0, 24);
    display.println("A:Restart  B:Start");
  } else if (state == STATE_AWAIT_STD_HI) {
    display.println("STEP 4/5 HI");
    display.setCursor(0, 14);
    display.print("Insert hi A=");
    display.println(std_hi_abs, 3);
    display.setCursor(0, 24);
    display.println("A:Restart  B:Start");
  } else if (state == STATE_AWAIT_SAMPLE) {
    display.println("STEP 5/5 SAMPLE");
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
  // spacing so raw and LSQ-corrected absorbance are simultaneously visible.
  // Compact affine params m and b are shown alongside on the same rows.
  // Before all three standards are measured, m=1.000 and b=0.000, so the
  // two absorbance values will be equal -- the fact that they match is the
  // visual cue that no correction is being applied yet.
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
  have_std_mid = false;
  have_std_hi = false;
  have_sample = false;
  m_slope = 1.0f;
  b_intercept = 0.0f;
  i_blank = 0.0f;
  i_std_lo = 0.0f;
  i_std_mid = 0.0f;
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
    have_std_mid = false;
    have_std_hi = false;
    have_sample = false;
    m_slope = 1.0f;
    b_intercept = 0.0f;
  } else if (p == PHASE_STD_LO) {
    have_std_lo = false;
    have_std_mid = false;
    have_std_hi = false;
    have_sample = false;
    m_slope = 1.0f;
    b_intercept = 0.0f;
  } else if (p == PHASE_STD_MID) {
    have_std_mid = false;
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
  if (state == STATE_AWAIT_BLANK)   { start_measurement(PHASE_BLANK);   return; }
  if (state == STATE_AWAIT_STD_LO)  { start_measurement(PHASE_STD_LO);  return; }
  if (state == STATE_AWAIT_STD_MID) { start_measurement(PHASE_STD_MID); return; }
  if (state == STATE_AWAIT_STD_HI)  { start_measurement(PHASE_STD_HI);  return; }
  if (state == STATE_AWAIT_SAMPLE)  { start_measurement(PHASE_SAMPLE);  return; }
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
    state = STATE_AWAIT_STD_MID;
  } else if (phase == PHASE_STD_MID) {
    i_std_mid = final_val;
    float a_mid_meas = (i_std_mid > 0.0f && i_blank > 0.0f)
                         ? log10f(i_blank / i_std_mid) : 0.0f;
    if (a_mid_meas > 1e-6f && have_std_lo) {
      have_std_mid = true;
      Serial.print(", A_mid_meas=");
      Serial.print(a_mid_meas, 6);
      Serial.print(", std_mid_abs=");
      Serial.println(std_mid_abs, 4);
    } else {
      have_std_mid = false;
      Serial.println(", A_mid_meas ~0 or no std_lo; std_mid rejected");
    }
    phase = PHASE_NONE;
    subphase = SUB_NONE;
    state = STATE_AWAIT_STD_HI;
  } else if (phase == PHASE_STD_HI) {
    i_std_hi = final_val;
    float a_hi_meas = (i_std_hi > 0.0f && i_blank > 0.0f)
                        ? log10f(i_blank / i_std_hi) : 0.0f;
    if (a_hi_meas > 1e-6f && have_std_lo && have_std_mid) {
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
      log_lsq_residuals();
    } else {
      have_std_hi = false;
      m_slope = 1.0f;
      b_intercept = 0.0f;
      Serial.println(", A_hi_meas ~0 or missing lo/mid; m,b left at 1,0");
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

// Small helper: apply a value that arrived over serial to one of the three
// anchor known-absorbance floats, then re-fit if all three anchors were
// already measured. Keeps handle_serial_line short and duplication-free.
static bool apply_std_override(const char* label, float& target, float v) {
  if (!(v > 0.0f && v < 10.0f)) {
    Serial.print("# ");
    Serial.print(label);
    Serial.println(": value must be > 0 and < 10");
    return false;
  }
  target = v;
  Serial.print("# ");
  Serial.print(label);
  Serial.print(" set to ");
  Serial.println(target, 4);
  if (have_std_lo && have_std_mid && have_std_hi) {
    recompute_affine();
    Serial.print("# m updated to ");
    Serial.print(m_slope, 6);
    Serial.print(", b updated to ");
    Serial.println(b_intercept, 6);
    log_lsq_residuals();
  }
  render();
  return true;
}

static void handle_serial_line(char* line) {
  while (*line == ' ' || *line == '\t') line++;
  if (*line == '\0') return;

  // Order matters: check "stdmid" before "stdlo"/"stdhi" would also work
  // (all three are distinct prefixes), but we require the trailing space
  // so partial matches (e.g. "stdlomax ...") are rejected cleanly.
  if (strncmp(line, "stdlo", 5) == 0 && (line[5] == ' ' || line[5] == '\t')) {
    apply_std_override("std_lo_abs", std_lo_abs, atof(line + 6));
  } else if (strncmp(line, "stdmid", 6) == 0 && (line[6] == ' ' || line[6] == '\t')) {
    apply_std_override("std_mid_abs", std_mid_abs, atof(line + 7));
  } else if (strncmp(line, "stdhi", 5) == 0 && (line[5] == ' ' || line[5] == '\t')) {
    apply_std_override("std_hi_abs", std_hi_abs, atof(line + 6));
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
    Serial.println("' ; usage: 'stdlo <f>', 'stdmid <f>', 'stdhi <f>', 'reset'");
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

  Serial.println("# ver8.0 (nRF52840) ready -- 3-point LSQ AFFINE cal");
  Serial.println("# LED driven by external 555; micro does not drive LED");
  Serial.print  ("# warm-up = ");
  Serial.print  (WARMUP_MS / 1000);
  Serial.println(" s (buttons disabled during warm-up)");
  Serial.println("# workflow: STEP 1 blank -> STEP 2 std_lo -> STEP 3 std_mid -> STEP 4 std_hi -> STEP 5 sample loop");
  Serial.println("# B advances at each prompt; A restarts to STEP 1");
  Serial.println("# measurement = 3 settle + 10 avg rounds x 5 s = 65 s");
  Serial.println("# final = mean of 10 avg rounds");
  Serial.println("# CSV cols = t_s,phase,subphase,round,adc,volts");
  Serial.print  ("# std_lo_abs  default = ");
  Serial.println(std_lo_abs, 4);
  Serial.print  ("# std_mid_abs default = ");
  Serial.println(std_mid_abs, 4);
  Serial.print  ("# std_hi_abs  default = ");
  Serial.println(std_hi_abs, 4);
  Serial.println("# correction: A_corr = m * A_raw + b, (m,b) = LSQ best fit through 3 anchors");
  Serial.println("# serial commands: 'stdlo <float>', 'stdmid <float>', 'stdhi <float>', 'reset'");
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
