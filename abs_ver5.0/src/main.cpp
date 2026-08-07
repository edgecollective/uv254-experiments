// Absorbance measurement firmware, ItsyBitsy nRF52840, ver5.0 variant.
//
// LED is driven by an external 555 (soldered on) - the micro does NOT drive
// the LED pin. On boot the firmware runs a 2-minute LED warm-up (the LED
// itself is always on because the 555 keeps running - the "warm-up" here is
// really just waiting until the thermal drift on the plateau is negligible;
// the led_drift_july7 analysis showed that by 2 min drift is ~0.02 %/min).
// During warm-up we still sample at 10 Hz and stream one CSV row every 5 s
// so the log has an unbroken record, but the buttons are disabled.
//
// After warm-up (READY with no cal):
//   A -> blank-only cal (I_blank measurement)
//   B -> two-point cal (I_blank, then prompt for the standard, then I_std;
//                       derives k = std_abs / A_std_measured)
//
// After warm-up (READY with any cal):
//   A -> re-run blank cal (drops any k)
//   B -> sample measurement; reports A_corr = k * log10(I_blank / I_sample)
//
// A during a measurement aborts back to READY (preserving prior cal unless
// the aborted measurement was part of a two-point flow, in which case all
// cal is dropped). A during the "insert standard" prompt aborts two-point.
//
// Each measurement is 3 x 5 s settle + 10 x 5 s avg = 65 s. The final value
// is the mean of the 10 avg rounds.
//
// Serial (USB CDC) commands, line-terminated:
//   std <float>   set the known absorbance of the two-point standard
//                 (default 0.1). Updates k live if two-point cal is active.
//   reset         clear all calibration (ignored during a measurement).
//
// CSV cols: t_s, phase, subphase, round, adc, volts
//   phase    ∈ {warmup, blank, std, sample}
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

enum State    { STATE_WARMUP, STATE_READY, STATE_AWAIT_STD, STATE_MEASURING };
enum Phase    { PHASE_NONE, PHASE_WARMUP, PHASE_BLANK, PHASE_STD, PHASE_SAMPLE };
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
float    i_std      = 0.0f;
float    i_sample   = 0.0f;
float    absorbance_raw = 0.0f;   // uncorrected log10(I_blank/I_sample)
float    absorbance     = 0.0f;   // = k_correction * absorbance_raw (reported value)
bool     have_calibration = false;
bool     have_two_point   = false;
bool     have_sample      = false;
bool     two_point_in_progress = false;   // true from B-press until std cal done or aborted

// Two-point cal parameters. std_abs is the known absorbance of the standard
// solution; k = std_abs / A_std_measured is applied multiplicatively to raw
// sample absorbances. k defaults to 1.0 (no correction).
float    std_abs      = 0.1f;
float    k_correction = 1.0f;

// Serial (USB CDC) line buffer for commands ("std <float>", "reset").
char     serial_buf[64];
uint8_t  serial_len = 0;

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
    case PHASE_STD:    return "std";
    case PHASE_SAMPLE: return "sample";
    default:           return "-";
  }
}

static const char* phase_name_upper(Phase p) {
  switch (p) {
    case PHASE_WARMUP: return "WARMUP";
    case PHASE_BLANK:  return "BLANK";
    case PHASE_STD:    return "STD";
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

  // Version tag, top-right corner ("ver5.0" = 6 chars * 6 px = 36 px wide).
  display.setCursor(OLED_W - 6 * 6, 0);
  display.print("ver5.0");

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
    if (!have_calibration) {
      display.setCursor(0, 14);
      display.println("A: Blank cal");
      display.setCursor(0, 24);
      display.print("B: 2-pt (std=");
      display.print(std_abs, 3);
      display.println(")");
    } else {
      display.setCursor(0, 18);
      display.println("A:Recal  B:Measure");
    }
  } else if (state == STATE_AWAIT_STD) {
    display.println("AWAIT STD");
    display.setCursor(0, 12);
    display.print("Insert std A=");
    display.println(std_abs, 3);
    display.setCursor(0, 22);
    display.println("A:Abort  B:Start");
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

static void clear_all_cal() {
  have_calibration = false;
  have_two_point = false;
  have_sample = false;
  k_correction = 1.0f;
  i_blank = 0.0f;
  i_std = 0.0f;
  i_sample = 0.0f;
  absorbance = 0.0f;
  absorbance_raw = 0.0f;
  two_point_in_progress = false;
}

static void start_measurement(Phase p, bool is_two_point_start) {
  phase = p;
  state = STATE_MEASURING;

  if (p == PHASE_BLANK) {
    // Any fresh blank drops all prior cal (both single and two-point).
    clear_all_cal();
    two_point_in_progress = is_two_point_start;
  } else if (p == PHASE_STD) {
    // Continuation of a two-point flow: keep the blank (have_calibration true),
    // drop any prior std/sample outcome.
    have_sample = false;
    have_two_point = false;
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
  Serial.println("s");
  // A two-point flow abort drops everything (the just-partial blank or std,
  // plus any blank we already have from earlier in the same flow).
  if (two_point_in_progress) {
    clear_all_cal();
    Serial.println("# two-point flow aborted; all cal cleared");
  }
  // Non-two-point abort of a fresh blank: have_calibration was already cleared
  // at the start of PHASE_BLANK, so the user is left with no cal. Consistent
  // with the prior ver3.2 behavior.
  Serial.flush();
  phase = PHASE_NONE;
  subphase = SUB_NONE;
  state = STATE_READY;
  round_index_in_sub = 0;
  sample_index = 0;
  sample_sum = 0.0;
  render();
}

static void abort_await_std() {
  Serial.print("# two-point aborted at STD prompt; t=");
  Serial.print((millis() - boot_ms) / 1000.0f, 3);
  Serial.println("s; all cal cleared");
  Serial.flush();
  clear_all_cal();
  phase = PHASE_NONE;
  subphase = SUB_NONE;
  state = STATE_READY;
  render();
}

static void on_calibrate_press() {
  if (state == STATE_WARMUP) return;
  if (state == STATE_MEASURING) { abort_measurement(); return; }
  if (state == STATE_AWAIT_STD) { abort_await_std(); return; }
  // STATE_READY: A -> blank cal (drops any prior cal).
  start_measurement(PHASE_BLANK, /*is_two_point_start=*/false);
}

static void on_compute_press() {
  if (state == STATE_WARMUP) return;
  if (state == STATE_MEASURING) return;
  if (state == STATE_AWAIT_STD) {
    // Begin the std measurement, continuing the two-point flow.
    start_measurement(PHASE_STD, /*is_two_point_start=*/false);
    return;
  }
  // STATE_READY.
  if (!have_calibration) {
    // No cal yet -> B starts the two-point flow (blank first, then STD prompt).
    start_measurement(PHASE_BLANK, /*is_two_point_start=*/true);
    return;
  }
  // Have cal -> B measures a sample.
  start_measurement(PHASE_SAMPLE, /*is_two_point_start=*/false);
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
    have_calibration = true;
    Serial.println("");

    if (two_point_in_progress) {
      // Wait for the user to insert the standard before starting PHASE_STD.
      phase = PHASE_NONE;
      subphase = SUB_NONE;
      state = STATE_AWAIT_STD;
      Serial.print("# blank done in two-point flow; INSERT ");
      Serial.print(std_abs, 4);
      Serial.println(" std, then press B (A aborts)");
      Serial.flush();
      render();
      return;
    }
  } else if (phase == PHASE_STD) {
    i_std = final_val;
    float a_std_meas = (i_std > 0.0f && i_blank > 0.0f)
                         ? log10f(i_blank / i_std) : 0.0f;
    if (a_std_meas > 1e-6f) {
      k_correction = std_abs / a_std_meas;
      have_two_point = true;
      Serial.print(", A_std_meas=");
      Serial.print(a_std_meas, 6);
      Serial.print(", std_abs=");
      Serial.print(std_abs, 4);
      Serial.print(", k=");
      Serial.println(k_correction, 6);
    } else {
      k_correction = 1.0f;
      have_two_point = false;
      Serial.println(", A_std_meas ~0 or negative; k left at 1.0");
    }
    two_point_in_progress = false;
  } else if (phase == PHASE_SAMPLE) {
    i_sample = final_val;
    have_sample = true;
    absorbance_raw = (i_sample > 0.0f && i_blank > 0.0f)
                       ? log10f(i_blank / i_sample) : 0.0f;
    absorbance = k_correction * absorbance_raw;
    Serial.print(", A_raw=");
    Serial.print(absorbance_raw, 6);
    Serial.print(", k=");
    Serial.print(k_correction, 6);
    Serial.print(", A_corr=");
    Serial.println(absorbance, 6);
  } else {
    Serial.println("");
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
  // unbroken record); READY and AWAIT_STD are idle.
  if (state == STATE_READY || state == STATE_AWAIT_STD) return;
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
  // Trim leading whitespace.
  while (*line == ' ' || *line == '\t') line++;
  if (*line == '\0') return;

  if (strncmp(line, "std", 3) == 0 && (line[3] == ' ' || line[3] == '\t')) {
    float v = atof(line + 4);
    if (v > 0.0f && v < 10.0f) {
      std_abs = v;
      Serial.print("# std_abs set to ");
      Serial.println(std_abs, 4);
      // If a two-point cal is already active, recompute k with the new std_abs.
      if (have_two_point && i_std > 0.0f && i_blank > 0.0f) {
        float a_std_meas = log10f(i_blank / i_std);
        if (a_std_meas > 1e-6f) {
          k_correction = std_abs / a_std_meas;
          Serial.print("# k updated to ");
          Serial.println(k_correction, 6);
        }
      }
      render();
    } else {
      Serial.println("# std: value must be > 0 and < 10");
    }
  } else if (strcmp(line, "reset") == 0) {
    if (state == STATE_MEASURING || state == STATE_AWAIT_STD) {
      Serial.println("# reset ignored: measurement or two-point flow in progress");
      return;
    }
    clear_all_cal();
    Serial.println("# all calibration cleared");
    render();
  } else {
    Serial.print("# unknown command: '");
    Serial.print(line);
    Serial.println("' ; usage: 'std <float>' or 'reset'");
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
      serial_len = 0;   // line too long; drop
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

  Serial.println("# ver5.0 (nRF52840) ready");
  Serial.println("# LED driven by external 555; micro does not drive LED");
  Serial.print  ("# warm-up = ");
  Serial.print  (WARMUP_MS / 1000);
  Serial.println(" s (buttons disabled during warm-up)");
  Serial.println("# measurement = 3 settle + 10 avg rounds x 5 s = 65 s");
  Serial.println("# final = mean of 10 avg rounds");
  Serial.println("# CSV cols = t_s,phase,subphase,round,adc,volts");
  Serial.println("# READY (no cal): A=blank-cal, B=two-point cal");
  Serial.println("# READY (any cal): A=re-cal blank (drops k), B=measure");
  Serial.println("# A during measurement or AWAIT_STD prompt = abort");
  Serial.print  ("# std_abs default = ");
  Serial.println(std_abs, 4);
  Serial.println("# serial commands: 'std <float>', 'reset'");
  Serial.print  ("# warmup begin at t=");
  Serial.print  ((warmup_start_ms - boot_ms) / 1000.0f, 3);
  Serial.println("s");
  Serial.flush();

  render();
}

void loop() {
  poll_serial();
  check_button(btn_cal,  on_calibrate_press);
  check_button(btn_meas, on_compute_press);
  tick_sampling();
}
