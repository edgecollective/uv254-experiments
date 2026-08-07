// Dark-measurement protocol firmware.
//
// LED is driven by an external 555 (the micro does NOT drive the LED pin).
// The 555 is soldered on and cannot be disconnected mid-run, so "dark" is
// produced optically by inserting an opaque cuvette / blocker in the beam.
// Button B walks the operator through the protocol:
//   step 1: DARK_PRE   - insert opaque blocker, press B
//   step 2: REF        - insert blank cuvette, press B
//   step 3: SAMPLE     - swap in sample cuvette, press B
//   step 4: DARK_POST  - insert opaque blocker, press B
//   done               - press B to restart
//
// Each step runs 30 s of settle + 30 s of measurement. Sampling is 10 Hz,
// averaged in 50-sample (5 s) bins. CSV columns:
//   t_s, adc, volts, step, phase, t_phase_s
// where step is 1..4, phase is 1=settle / 2=measure, and t_phase_s is
// seconds since the current phase began. Human-readable state transitions
// are also emitted as "# ..." comment lines for log-navigation.

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_DotStar.h>

#define BTN_PIN     9
#define DETECT_PIN  A5

const uint32_t SUBSAMPLE_INTERVAL_MS = 100;   // 10 Hz
const uint16_t SAMPLES_PER_AVERAGE   = 50;    // -> 5 s per row
const uint32_t SETTLE_MS             = 30000; // 30 s settle
const uint32_t MEASURE_MS            = 30000; // 30 s measurement

const float V_REF          = 3.3f;
const float ADC_FULL_SCALE = 4095.0f;

#define OLED_W     128
#define OLED_H     64
#define OLED_ADDR  0x3C
Adafruit_SSD1306 display(OLED_W, OLED_H, &Wire, -1);
bool display_ok = false;

Adafruit_DotStar onboard_pixel(1, PIN_DOTSTAR_DATA, PIN_DOTSTAR_CLK, DOTSTAR_BGR);

enum Step {
  STEP_IDLE      = 0,
  STEP_DARK_PRE  = 1,
  STEP_REF       = 2,
  STEP_SAMPLE    = 3,
  STEP_DARK_POST = 4,
  STEP_DONE      = 5,
};

enum Phase {
  PHASE_PROMPT  = 0,
  PHASE_SETTLE  = 1,
  PHASE_MEASURE = 2,
};

uint8_t  cur_step  = STEP_IDLE;
uint8_t  cur_phase = PHASE_PROMPT;
uint32_t phase_start_ms = 0;
uint32_t boot_ms        = 0;

// Averaging state (only advances while cur_phase is SETTLE or MEASURE).
uint32_t next_subsample_ms = 0;
uint16_t sample_index      = 0;
double   sample_sum        = 0.0;

// Latest emitted values for display.
float    last_avg = 0.0f;
float    last_t_s = 0.0f;

// Button debounce.
int      btn_last_stable = HIGH;
uint32_t btn_last_change = 0;
const uint32_t BTN_DEBOUNCE_MS = 30;

static const char* step_name(uint8_t s) {
  switch (s) {
    case STEP_IDLE:      return "idle";
    case STEP_DARK_PRE:  return "dark_pre";
    case STEP_REF:       return "ref";
    case STEP_SAMPLE:    return "sample";
    case STEP_DARK_POST: return "dark_post";
    case STEP_DONE:      return "done";
  }
  return "?";
}

static const char* phase_name(uint8_t p) {
  switch (p) {
    case PHASE_PROMPT:  return "prompt";
    case PHASE_SETTLE:  return "settle";
    case PHASE_MEASURE: return "measure";
  }
  return "?";
}

static void reset_average() {
  sample_index      = 0;
  sample_sum        = 0.0;
  next_subsample_ms = millis();
}

static void render() {
  if (!display_ok) return;
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("dark_test ");
  display.print(cur_step);
  display.print("/4 ");
  display.println(step_name(cur_step));

  display.setCursor(0, 12);
  if (cur_phase == PHASE_PROMPT) {
    switch (cur_step) {
      case STEP_IDLE:
        display.println("Press B to start.");
        display.setCursor(0, 24);
        display.println("Steps 1..4: dark,");
        display.setCursor(0, 32);
        display.println("blank, sample, dark.");
        break;
      case STEP_DARK_PRE:
        display.println("STEP 1: DARK PRE");
        display.setCursor(0, 24);
        display.println("Insert opaque");
        display.setCursor(0, 32);
        display.println("blocker.");
        display.setCursor(0, 40);
        display.println("Press B when ready.");
        break;
      case STEP_REF:
        display.println("STEP 2: BLANK");
        display.setCursor(0, 24);
        display.println("Insert blank cuvette");
        display.setCursor(0, 32);
        display.println("in beam.");
        display.setCursor(0, 40);
        display.println("Press B when ready.");
        break;
      case STEP_SAMPLE:
        display.println("STEP 3: SAMPLE");
        display.setCursor(0, 24);
        display.println("Swap in sample");
        display.setCursor(0, 32);
        display.println("cuvette.");
        display.setCursor(0, 40);
        display.println("Press B when ready.");
        break;
      case STEP_DARK_POST:
        display.println("STEP 4: DARK POST");
        display.setCursor(0, 24);
        display.println("Disconnect 555.");
        display.setCursor(0, 32);
        display.println("Press B when ready.");
        break;
      case STEP_DONE:
        display.println("PROTOCOL DONE");
        display.setCursor(0, 24);
        display.println("Press B to restart.");
        break;
    }
  } else {
    uint32_t elapsed_ms = millis() - phase_start_ms;
    uint32_t total_ms   = (cur_phase == PHASE_SETTLE) ? SETTLE_MS : MEASURE_MS;
    uint32_t remain_s   = (elapsed_ms >= total_ms) ? 0
                         : (total_ms - elapsed_ms + 999) / 1000;

    display.print(cur_phase == PHASE_SETTLE ? "SETTLE " : "MEASURE ");
    display.print(remain_s);
    display.println("s left");

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
    display.println(last_avg, 1);

    display.setCursor(0, 56);
    display.print("V   = ");
    display.print(last_avg * V_REF / ADC_FULL_SCALE, 4);
    display.println("V");
  }
  display.display();
}

static void enter_phase(uint8_t phase) {
  cur_phase = phase;
  phase_start_ms = millis();
  reset_average();

  if (phase == PHASE_SETTLE || phase == PHASE_MEASURE) {
    Serial.print("# STEP ");
    Serial.print(cur_step);
    Serial.print(' ');
    Serial.print(step_name(cur_step));
    Serial.print(' ');
    Serial.print(phase_name(phase));
    Serial.print(" begin at t=");
    Serial.print((phase_start_ms - boot_ms) / 1000.0f, 3);
    Serial.println("s");
    Serial.flush();
  }
  render();
}

static void enter_step(uint8_t step) {
  cur_step = step;
  cur_phase = PHASE_PROMPT;
  Serial.print("# STEP ");
  Serial.print(step);
  Serial.print(' ');
  Serial.print(step_name(step));
  Serial.println(" prompt");
  Serial.flush();
  render();
}

static void on_button_press() {
  if (cur_phase != PHASE_PROMPT) return;  // ignore during settle/measure

  switch (cur_step) {
    case STEP_IDLE:
      enter_step(STEP_DARK_PRE);
      enter_phase(PHASE_SETTLE);
      break;
    case STEP_DARK_PRE:
    case STEP_REF:
    case STEP_SAMPLE:
    case STEP_DARK_POST:
      enter_phase(PHASE_SETTLE);
      break;
    case STEP_DONE:
      enter_step(STEP_IDLE);
      break;
  }
}

static void advance_after_measure() {
  Serial.print("# STEP ");
  Serial.print(cur_step);
  Serial.print(' ');
  Serial.print(step_name(cur_step));
  Serial.print(" measure end at t=");
  Serial.print((millis() - boot_ms) / 1000.0f, 3);
  Serial.println("s");
  Serial.flush();

  switch (cur_step) {
    case STEP_DARK_PRE:  enter_step(STEP_REF);       break;
    case STEP_REF:       enter_step(STEP_SAMPLE);    break;
    case STEP_SAMPLE:    enter_step(STEP_DARK_POST); break;
    case STEP_DARK_POST: enter_step(STEP_DONE);      break;
    default:             enter_step(STEP_IDLE);      break;
  }
}

static void tick_subsample() {
  if (cur_phase != PHASE_SETTLE && cur_phase != PHASE_MEASURE) return;
  if ((int32_t)(millis() - next_subsample_ms) < 0) return;

  float v = (float)analogRead(DETECT_PIN);
  sample_sum += (double)v;
  sample_index++;
  next_subsample_ms += SUBSAMPLE_INTERVAL_MS;

  if (sample_index >= SAMPLES_PER_AVERAGE) {
    float avg = (float)(sample_sum / SAMPLES_PER_AVERAGE);
    uint32_t now = millis();
    float t_s = (now - boot_ms) / 1000.0f;
    float t_phase_s = (now - phase_start_ms) / 1000.0f;

    Serial.print(t_s, 3);
    Serial.print(',');
    Serial.print(avg, 4);
    Serial.print(',');
    Serial.print(avg * V_REF / ADC_FULL_SCALE, 5);
    Serial.print(',');
    Serial.print(cur_step);
    Serial.print(',');
    Serial.print(cur_phase);
    Serial.print(',');
    Serial.println(t_phase_s, 3);
    Serial.flush();

    last_avg = avg;
    last_t_s = t_s;
    sample_index = 0;
    sample_sum   = 0.0;
  }

  render();
}

static void tick_phase_timeout() {
  uint32_t elapsed = millis() - phase_start_ms;
  if (cur_phase == PHASE_SETTLE && elapsed >= SETTLE_MS) {
    enter_phase(PHASE_MEASURE);
  } else if (cur_phase == PHASE_MEASURE && elapsed >= MEASURE_MS) {
    advance_after_measure();
  }
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
  }

  boot_ms = millis();

  Serial.println("# dark_test firmware ready");
  Serial.println("# LED is driven by external 555 (micro does not drive LED)");
  Serial.print  ("# 50-sample avg @ 10 Hz -> row every ");
  Serial.print  (SAMPLES_PER_AVERAGE * SUBSAMPLE_INTERVAL_MS);
  Serial.println(" ms");
  Serial.print  ("# settle = ");
  Serial.print  (SETTLE_MS / 1000);
  Serial.print  ("s, measure = ");
  Serial.print  (MEASURE_MS / 1000);
  Serial.println("s per step");
  Serial.println("# CSV cols = t_s,adc,volts,step,phase,t_phase_s");
  Serial.println("# step: 1=dark_pre 2=ref 3=sample 4=dark_post");
  Serial.println("# phase: 1=settle 2=measure");
  Serial.flush();

  enter_step(STEP_IDLE);
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
  tick_phase_timeout();
}
