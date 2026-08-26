// Absorbance measurement firmware, ItsyBitsy nRF52840, ver6.0 variant.
//
// Enforces a linear two-point calibration workflow. After the LED warm-up,
// the device walks through three prompts in sequence:
//
//   STEP 1/3 BLANK   Insert blank,  press B -> blank measurement
//   STEP 2/3 STD     Insert std,    press B -> std measurement -> derive k
//   STEP 3/3 SAMPLE  Insert sample, press B -> corrected absorbance
//                    (stays in this loop; each B measures another sample)
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
// Correction: A_corr = k * log10(I_blank / I_sample),
//             k = std_abs / A_std_measured  (default std_abs = 0.1).
//
// Serial (USB CDC) commands, line-terminated:
//   std <float>   override the standard's known absorbance (default 0.1).
//                 Live-updates k if a two-point cal is already complete.
//   reset         restart from STEP 1 (ignored during a measurement).
//
// CSV cols: t_s, phase, subphase, round, adc, volts
//   phase    in {warmup, blank, std, sample}
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
  STATE_AWAIT_STD,
  STATE_AWAIT_SAMPLE,
  STATE_MEASURING
};
enum Phase    { PHASE_NONE, PHASE_WARMUP, PHASE_BLANK, PHASE_STD, PHASE_SAMPLE };
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
float    i_std      = 0.0f;
float    i_sample   = 0.0f;
float    absorbance_raw = 0.0f;
float    absorbance     = 0.0f;
bool     have_blank  = false;
bool     have_std    = false;
bool     have_sample = false;

// Standard's known absorbance and derived multiplicative correction.
float    std_abs      = 0.1f;
float    k_correction = 1.0f;

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

  // Version tag, top-right corner ("ver6.0" = 6 chars * 6 px = 36 px).
  display.setCursor(OLED_W - 6 * 6, 0);
  display.print("ver6.0");

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
    display.println("STEP 1/3 BLANK");
    display.setCursor(0, 14);
    display.println("Insert blank");
    display.setCursor(0, 24);
    display.println("A:Restart  B:Start");
  } else if (state == STATE_AWAIT_STD) {
    display.println("STEP 2/3 STD");
    display.setCursor(0, 14);
    display.print("Insert std A=");
    display.println(std_abs, 3);
    display.setCursor(0, 24);
    display.println("A:Restart  B:Start");
  } else if (state == STATE_AWAIT_SAMPLE) {
    display.println("STEP 3/3 SAMPLE");
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

  // Bottom info block: shown once we've moved past STEP 1.
  if (state != STATE_WARMUP && state != STATE_AWAIT_BLANK) {
    display.setCursor(0, 32);
    display.print("Blank= ");
    if (have_blank) {
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
    if (have_sample && have_blank && have_std) display.print(absorbance, 4);
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
  have_std = false;
  have_sample = false;
  k_correction = 1.0f;
  i_blank = 0.0f;
  i_std = 0.0f;
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
    have_std = false;
    have_sample = false;
    k_correction = 1.0f;
  } else if (p == PHASE_STD) {
    have_std = false;
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
  if (state == STATE_AWAIT_BLANK)  { start_measurement(PHASE_BLANK);  return; }
  if (state == STATE_AWAIT_STD)    { start_measurement(PHASE_STD);    return; }
  if (state == STATE_AWAIT_SAMPLE) { start_measurement(PHASE_SAMPLE); return; }
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
    state = STATE_AWAIT_STD;
  } else if (phase == PHASE_STD) {
    i_std = final_val;
    float a_std_meas = (i_std > 0.0f && i_blank > 0.0f)
                         ? log10f(i_blank / i_std) : 0.0f;
    if (a_std_meas > 1e-6f) {
      k_correction = std_abs / a_std_meas;
      have_std = true;
      Serial.print(", A_std_meas=");
      Serial.print(a_std_meas, 6);
      Serial.print(", std_abs=");
      Serial.print(std_abs, 4);
      Serial.print(", k=");
      Serial.println(k_correction, 6);
    } else {
      k_correction = 1.0f;
      have_std = false;
      Serial.println(", A_std_meas ~0 or negative; k left at 1.0");
    }
    phase = PHASE_NONE;
    subphase = SUB_NONE;
    state = STATE_AWAIT_SAMPLE;
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

  if (strncmp(line, "std", 3) == 0 && (line[3] == ' ' || line[3] == '\t')) {
    float v = atof(line + 4);
    if (v > 0.0f && v < 10.0f) {
      std_abs = v;
      Serial.print("# std_abs set to ");
      Serial.println(std_abs, 4);
      if (have_std && i_std > 0.0f && i_blank > 0.0f) {
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
    if (state == STATE_MEASURING) {
      Serial.println("# reset ignored: measurement in progress");
      return;
    }
    Serial.println("# reset -> restart to STEP 1 (BLANK)");
    restart_to_blank();
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

  Serial.println("# ver6.0 (nRF52840) ready");
  Serial.println("# LED driven by external 555; micro does not drive LED");
  Serial.print  ("# warm-up = ");
  Serial.print  (WARMUP_MS / 1000);
  Serial.println(" s (buttons disabled during warm-up)");
  Serial.println("# workflow: STEP 1 blank -> STEP 2 std -> STEP 3 sample loop");
  Serial.println("# B advances at each prompt; A restarts to STEP 1");
  Serial.println("# measurement = 3 settle + 10 avg rounds x 5 s = 65 s");
  Serial.println("# final = mean of 10 avg rounds");
  Serial.println("# CSV cols = t_s,phase,subphase,round,adc,volts");
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
  check_button(btn_a, on_a_press);
  check_button(btn_b, on_b_press);
  tick_sampling();
}
