// Basic LOD test, ItsyBitsy nRF52840.
//
// Boot sequence:
//   1) 2 min warm-up (buttons disabled)
//   2) Run repeated "blank" measurements back-to-back, forever, until you
//      either hit MAX_BLANKS or press button B to stop.
//
// Each blank measurement is exactly the absorbance_july7 protocol:
//   5 rounds of 5-s settle + 10 rounds of 5-s avg (75 s total)
// The final value is the mean of the 10 avg rounds.
//
// After each blank we compute running mean, std, CoV, and an estimated
// absorbance limit of detection (LOD) using the 3σ rule:
//     LOD_absorbance = log10( mean_blank / (mean_blank - 3 * sigma_blank) )
//
// CSV cols: t_s, phase, subphase, blank_idx, round, adc, volts
//   phase    ∈ {warmup, blank}
//   subphase ∈ {warmup, settle, avg}
//   blank_idx = 0 during warm-up, 1..N for the Nth blank measurement
//   round    = 1-indexed within the current subphase
//
// Human summary lines are printed on transitions and after each blank:
//   # blank <k> final = <adc> counts, <V> V
//   # LOD stats: n=<k>, mean=<m>, std=<s>, CoV=<c>%, LOD_A3sigma=<L>

#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <math.h>

#define BTN_STOP_PIN  9   // Button B -> STOP (any time after warm-up)
#define DETECT_PIN    A5

const uint32_t SUBSAMPLE_INTERVAL_MS = 100;  // 10 Hz
const uint16_t SAMPLES_PER_ROUND     = 50;   // 5 s per round
const uint8_t  SETTLE_ROUNDS         = 5;    // 25 s settle per blank
const uint8_t  AVG_ROUNDS            = 10;   // 50 s avg window per blank
const uint32_t WARMUP_MS             = 120000UL;  // 2 min

const uint16_t MAX_BLANKS = 60;   // safety cap; ~75 min of blanks

const uint32_t ROUND_MS  = (uint32_t)SAMPLES_PER_ROUND * SUBSAMPLE_INTERVAL_MS;
const uint32_t SETTLE_MS = (uint32_t)SETTLE_ROUNDS * ROUND_MS;

// nRF52840 SAADC, 12-bit native (0..4095). AR_INTERNAL_3_0 -> 3.0 V FS.
const float V_REF          = 3.0f;
const float ADC_FULL_SCALE = 4095.0f;
static inline float counts_to_volts(float c) { return c * V_REF / ADC_FULL_SCALE; }

#define OLED_W     128
#define OLED_H     64
#define OLED_ADDR  0x3C
Adafruit_SSD1306 display(OLED_W, OLED_H, &Wire, -1);
bool display_ok = false;

enum State    { STATE_WARMUP, STATE_MEASURING, STATE_STOPPED };
enum Subphase { SUB_NONE, SUB_WARMUP, SUB_SETTLE, SUB_AVG };

State    state    = STATE_WARMUP;
Subphase subphase = SUB_WARMUP;

uint8_t  round_index_in_sub = 0;
uint16_t warmup_round       = 0;
uint16_t sample_index       = 0;
double   sample_sum         = 0.0;
uint32_t next_subsample_ms  = 0;

uint32_t warmup_start_ms   = 0;
uint32_t subphase_start_ms = 0;

float    avg_rounds[10] = {0};
float    last_warmup_avg = 0.0f;

// LOD statistics.
uint16_t blank_idx        = 0;   // index of the CURRENT blank measurement (1-indexed once started)
uint16_t blank_count      = 0;   // number of completed blank measurements
float    blank_finals[MAX_BLANKS];
float    lod_mean         = 0.0f;
float    lod_std          = 0.0f;
float    lod_cov_pct      = 0.0f;
float    lod_absorbance_3sigma = 0.0f;

uint32_t boot_ms = 0;

// Button B (STOP) debounce.
int      btn_last_stable = HIGH;
uint32_t btn_last_change = 0;
const uint32_t BTN_DEBOUNCE_MS = 30;

static const char* subphase_name(Subphase s) {
  switch (s) {
    case SUB_WARMUP: return "warmup";
    case SUB_SETTLE: return "settle";
    case SUB_AVG:    return "avg";
    default:         return "-";
  }
}

static const char* phase_name_for_state() {
  return (state == STATE_WARMUP) ? "warmup" : "blank";
}

static float running_avg_mean() {
  if (round_index_in_sub == 0) return 0.0f;
  double sum = 0.0;
  for (uint8_t i = 0; i < round_index_in_sub; i++) sum += avg_rounds[i];
  return (float)(sum / round_index_in_sub);
}

static void recompute_lod_stats() {
  if (blank_count == 0) {
    lod_mean = lod_std = lod_cov_pct = lod_absorbance_3sigma = 0.0f;
    return;
  }
  double sum = 0.0;
  for (uint16_t i = 0; i < blank_count; i++) sum += blank_finals[i];
  lod_mean = (float)(sum / blank_count);

  if (blank_count < 2) {
    lod_std = 0.0f;
    lod_cov_pct = 0.0f;
    lod_absorbance_3sigma = 0.0f;
    return;
  }
  double ss = 0.0;
  for (uint16_t i = 0; i < blank_count; i++) {
    double d = blank_finals[i] - lod_mean;
    ss += d * d;
  }
  lod_std = (float)sqrt(ss / (blank_count - 1));
  lod_cov_pct = 100.0f * lod_std / lod_mean;

  // Absorbance LOD (3-sigma rule): smallest A distinguishable from noise.
  // A_LOD = log10( mean / (mean - 3*sigma) )   (undefined if 3*sigma >= mean)
  float denom = lod_mean - 3.0f * lod_std;
  lod_absorbance_3sigma = (denom > 0.0f) ? log10f(lod_mean / denom) : NAN;
}

static void render() {
  if (!display_ok) return;
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);

  if (state == STATE_WARMUP) {
    display.println("WARM-UP (2 min)");
    uint32_t elapsed = millis() - warmup_start_ms;
    uint32_t remain_s = (elapsed >= WARMUP_MS)
                         ? 0 : (WARMUP_MS - elapsed + 999) / 1000;
    display.setCursor(0, 10);
    display.print("remain: ");
    display.print(remain_s);
    display.println("s");
    display.setCursor(0, 20);
    display.print("round ");
    display.print(warmup_round);
    display.print('/');
    display.print(WARMUP_MS / ROUND_MS);
    display.setCursor(0, 30);
    display.print("last= ");
    if (warmup_round > 0) {
      display.print(last_warmup_avg, 1);
      display.print(' ');
      display.print(counts_to_volts(last_warmup_avg), 3);
      display.print('V');
    }
    display.setCursor(0, 54);
    display.println("basic_LOD");
  } else if (state == STATE_MEASURING) {
    display.print("Blank #");
    display.println(blank_idx);
    display.setCursor(0, 10);
    if (subphase == SUB_SETTLE) {
      uint32_t elapsed = millis() - subphase_start_ms;
      uint32_t remain_s = (elapsed >= SETTLE_MS)
                           ? 0 : (SETTLE_MS - elapsed + 999) / 1000;
      display.print("SETTLING ");
      display.print(remain_s);
      display.println("s left");
    } else if (subphase == SUB_AVG) {
      display.print("AVG ");
      display.print(round_index_in_sub);
      display.print('/');
      display.print(AVG_ROUNDS);
      display.setCursor(0, 20);
      if (round_index_in_sub > 0) {
        display.print("cur= ");
        display.print(running_avg_mean(), 1);
      }
    }
  } else {  // STATE_STOPPED
    display.println("STOPPED");
    display.setCursor(0, 10);
    display.print("n = ");
    display.println(blank_count);
  }

  // Running LOD stats (once we have at least one blank).
  if (blank_count >= 1) {
    display.setCursor(0, 34);
    display.print("mean=");
    display.println(lod_mean, 2);
    display.setCursor(0, 43);
    display.print("sd=");
    display.print(lod_std, 3);
    display.print(" cov=");
    display.print(lod_cov_pct, 3);
    display.println("%");
    display.setCursor(0, 56);
    display.print("LOD_A3s= ");
    if (isnan(lod_absorbance_3sigma)) display.print("--");
    else                              display.print(lod_absorbance_3sigma, 5);
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
  Serial.print("# blank ");
  Serial.print(blank_idx);
  Serial.print(' ');
  Serial.print(subphase_name(s));
  Serial.print(" begin at t=");
  Serial.print((subphase_start_ms - boot_ms) / 1000.0f, 3);
  Serial.println("s");
  Serial.flush();
  render();
}

static void start_next_blank() {
  if (blank_count >= MAX_BLANKS) {
    Serial.print("# reached MAX_BLANKS=");
    Serial.print(MAX_BLANKS);
    Serial.println("; stopping");
    Serial.flush();
    state = STATE_STOPPED;
    subphase = SUB_NONE;
    render();
    return;
  }
  blank_idx = blank_count + 1;   // 1-indexed for humans
  state = STATE_MEASURING;
  Serial.print("# blank ");
  Serial.print(blank_idx);
  Serial.print(" begin at t=");
  Serial.print((millis() - boot_ms) / 1000.0f, 3);
  Serial.println("s");
  Serial.flush();
  enter_subphase(SUB_SETTLE);
}

static void finish_blank() {
  double sum = 0.0;
  for (uint8_t i = 0; i < AVG_ROUNDS; i++) sum += avg_rounds[i];
  float final_val = (float)(sum / AVG_ROUNDS);

  blank_finals[blank_count] = final_val;
  blank_count++;
  recompute_lod_stats();

  Serial.print("# blank ");
  Serial.print(blank_idx);
  Serial.print(" final = ");
  Serial.print(final_val, 4);
  Serial.print(" counts, ");
  Serial.print(counts_to_volts(final_val), 5);
  Serial.println(" V");

  Serial.print("# LOD stats: n=");
  Serial.print(blank_count);
  Serial.print(", mean=");
  Serial.print(lod_mean, 4);
  Serial.print(", std=");
  Serial.print(lod_std, 4);
  Serial.print(", CoV=");
  Serial.print(lod_cov_pct, 4);
  Serial.print("%, LOD_A3sigma=");
  if (isnan(lod_absorbance_3sigma)) Serial.println("NaN");
  else                              Serial.println(lod_absorbance_3sigma, 6);
  Serial.flush();

  // Move straight into the next blank.
  start_next_blank();
}

static void finish_warmup() {
  Serial.print("# warmup complete at t=");
  Serial.print((millis() - boot_ms) / 1000.0f, 3);
  Serial.println("s; starting blank loop");
  Serial.flush();
  start_next_blank();
}

static void on_stop_press() {
  if (state == STATE_STOPPED) return;
  if (state == STATE_WARMUP)  return;   // don't allow abort during warm-up
  Serial.print("# STOP pressed at t=");
  Serial.print((millis() - boot_ms) / 1000.0f, 3);
  Serial.print("s; n=");
  Serial.println(blank_count);
  Serial.flush();
  state = STATE_STOPPED;
  subphase = SUB_NONE;
  render();
}

static void tick_sampling() {
  if (state == STATE_STOPPED) return;
  if ((int32_t)(millis() - next_subsample_ms) < 0) return;

  float v = (float)analogRead(DETECT_PIN);
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
      Serial.print(",warmup,warmup,0,");
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
      Serial.print(",blank,");
      Serial.print(subphase_name(subphase));
      Serial.print(',');
      Serial.print(blank_idx);
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
          finish_blank();
          return;
        }
      }
    }
  }

  render();
}

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(BTN_STOP_PIN, INPUT_PULLUP);

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
    display.setRotation(0);
    display.setTextColor(SSD1306_WHITE);
  }

  boot_ms = millis();
  warmup_start_ms = millis();
  next_subsample_ms = millis();
  sample_index = 0;
  sample_sum = 0.0;
  warmup_round = 0;

  Serial.println("# basic_LOD (nRF52840) ready");
  Serial.println("# LED driven by external 555; micro does not drive LED");
  Serial.print  ("# warm-up = ");
  Serial.print  (WARMUP_MS / 1000);
  Serial.println(" s");
  Serial.println("# each blank = 5 settle + 10 avg rounds x 5 s = 75 s");
  Serial.println("# CSV cols = t_s,phase,subphase,blank_idx,round,adc,volts");
  Serial.print  ("# will run up to ");
  Serial.print  (MAX_BLANKS);
  Serial.println(" blanks back-to-back (B to stop early)");
  Serial.print  ("# warmup begin at t=");
  Serial.print  ((warmup_start_ms - boot_ms) / 1000.0f, 3);
  Serial.println("s");
  Serial.flush();

  render();
}

void loop() {
  int now = digitalRead(BTN_STOP_PIN);
  uint32_t t = millis();
  if (now != btn_last_stable && (t - btn_last_change) > BTN_DEBOUNCE_MS) {
    btn_last_stable = now;
    btn_last_change = t;
    if (now == LOW) on_stop_press();
  }

  tick_sampling();
}
