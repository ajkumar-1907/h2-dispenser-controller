// ===== H2 dispenser controller - Step 3b: E-stop interrupt + bug fixes =====
#include "sensor_420.h"
#include "redundancy.h"
#include "rate_check.h"
#include "faults.h"
#include "safety.h"

// ---- inputs ----
#define PIN_BTN_RESET  PB4
#define PIN_BTN_START  PB5
#define PIN_BTN_STOP   PB3
#define PIN_ESTOP      PB6     // NEW: E-stop loop, LOW = healthy, HIGH = STOP
// ---- outputs ----
#define PIN_LED_FAULT  PC6
#define PIN_XV1        PC7
#define PIN_XV2        PA8
#define PIN_XV3_VENT   PB0
#define PIN_LED_HEART  PA5

Sensor sensors[] = {
  { "PT1", "MPa",  PA0,   0.0f, 100.0f },
  { "PT2", "MPa",  PA1,   0.0f, 100.0f },
  { "TT1", "C",    PA4, -50.0f, 100.0f },
  { "GD1", "%LEL", PA6,   0.0f, 100.0f },
};
const int N_SENSORS = 4;
SensorPair pressure = { "P", &sensors[0], &sensors[1], 2.0f, 500 };
RateCheck rates[] = {
  { &sensors[0], 20.0f, 100 },
  { &sensors[1], 20.0f, 100 },
  { &sensors[2],  5.0f, 100 },
};

// process limits with HYSTERESIS (trip value, clear value) - example values
const float P_TRIP = 87.5f, P_CLEAR = 85.0f;
const float T_TRIP = 85.0f, T_CLEAR = 80.0f;
const float GAS_WARN_LEL = 10.0f, GAS_ALARM_LEL = 25.0f, GAS_HYST = 2.0f;
bool opActive = false, otActive = false, gasWarnActive = false, gasAlarmActive = false;

Safety   safety;
uint32_t faults = 0, lastFaults = 0;
const uint32_t SAMPLE_MS = 10, PRINT_MS = 1000;
uint32_t lastSample = 0, lastPrint = 0;
bool lastReset = false, lastStart = false, lastStop = false;

// ================= E-STOP INTERRUPT =================
// 'volatile': the variable is changed inside an interrupt, so the compiler
// must re-read it from memory every time instead of keeping a cached copy.
volatile bool     estopTripped = false;
volatile uint32_t estopAtUs    = 0;

void estopISR() {
  // FAST PATH: close the valves RIGHT NOW, without waiting for loop().
  // Kept tiny: no Serial, no delay, no maths.
  digitalWrite(PIN_XV1, LOW);
  digitalWrite(PIN_XV2, LOW);
  digitalWrite(PIN_XV3_VENT, HIGH);      // LED on = venting
  if (!estopTripped) estopAtUs = micros();
  estopTripped = true;
}

// Writes the valve outputs, but never re-opens a valve after the E-stop ISR
// closed it. Interrupts are blocked between the CHECK and the WRITE, so the
// ISR cannot fire in the gap (that gap would be a race condition).
void writeOutputs(const Valves& v) {
  noInterrupts();
  if (estopTripped) {
    digitalWrite(PIN_XV1, LOW);
    digitalWrite(PIN_XV2, LOW);
    digitalWrite(PIN_XV3_VENT, HIGH);
  } else {
    digitalWrite(PIN_XV1, v.xv1Supply);
    digitalWrite(PIN_XV2, v.xv2Dispense);
    digitalWrite(PIN_XV3_VENT, !v.xv3VentClosed);
  }
  interrupts();
}

// ================= fault collection =================
// trip at 'trip', clear only below 'clear' (prevents chattering at the limit)
bool hyst(bool& active, float value, float trip, float clear) {
  if (!active && value >= trip)  active = true;
  if (active  && value <  clear) active = false;
  return active;
}

uint32_t collectFaults() {
  uint32_t f = 0;
  if (!usable(sensors[0])) f |= FLT_PT1_WIRING;
  if (!usable(sensors[1])) f |= FLT_PT2_WIRING;
  if (!usable(sensors[2])) f |= FLT_TT1_WIRING;
  if (!usable(sensors[3])) f |= FLT_GD1_WIRING;
  if (pressure.state == PAIR_DEGRADED) f |= FLT_P_DEGRADED;
  if (pressure.state == PAIR_DISAGREE) f |= FLT_P_DISAGREE;
  if (pressure.state == PAIR_LOST || pressure.state == PAIR_INIT) f |= FLT_P_LOST;
  if (rates[0].fault) f |= FLT_PT1_RATE;
  if (rates[1].fault) f |= FLT_PT2_RATE;
  if (rates[2].fault) f |= FLT_TT1_RATE;
  if (pressure.state != PAIR_LOST && pressure.state != PAIR_INIT)
    if (hyst(opActive, pressure.value, P_TRIP, P_CLEAR)) f |= FLT_OVERPRESSURE;
  if (usable(sensors[2]) && hyst(otActive, sensors[2].value, T_TRIP, T_CLEAR)) f |= FLT_OVERTEMP;
  if (usable(sensors[3])) {
    if (hyst(gasWarnActive,  sensors[3].value, GAS_WARN_LEL,  GAS_WARN_LEL  - GAS_HYST)) f |= FLT_GAS_WARN;
    if (hyst(gasAlarmActive, sensors[3].value, GAS_ALARM_LEL, GAS_ALARM_LEL - GAS_HYST)) f |= FLT_GAS_ALARM;
  }
  // E-stop: latched flag from the ISR, OR the circuit still open right now
  if (estopTripped || digitalRead(PIN_ESTOP) == HIGH) f |= FLT_ESTOP;
  return f;
}

// ================= logging =================
void logTime(uint32_t now) { Serial.print("["); Serial.print(now); Serial.print(" ms] "); }

void logFaultChanges(uint32_t now, uint32_t oldF, uint32_t newF) {
  uint32_t changed = oldF ^ newF;
  for (int b = 0; b < N_FAULT_BITS; b++) {
    if (!(changed & (1u << b))) continue;
    logTime(now);
    Serial.print((newF & (1u << b)) ? "FAULT+ " : "FAULT- ");
    Serial.print(faultName(b));
    if (newF & (1u << b)) { Serial.print("  -> action "); Serial.print(actionName(faultAction(1u << b))); }
    Serial.println();
  }
}

// list every active fault whose action is at least 'minAct': the REASON
void printReasons(uint32_t f, Action minAct) {
  for (int b = 0; b < N_FAULT_BITS; b++)
    if ((f & (1u << b)) && faultAction(1u << b) >= minAct) { Serial.print(" "); Serial.print(faultName(b)); }
  Serial.println();
}

bool pressedEdge(uint32_t pin, bool& last) {
  bool now = digitalRead(pin) == LOW;
  bool edge = now && !last;
  last = now;
  return edge;
}

void setup() {
  Serial.begin(115200);
  analogReadResolution(12);
  pinMode(PIN_BTN_RESET, INPUT_PULLUP);
  pinMode(PIN_BTN_START, INPUT_PULLUP);
  pinMode(PIN_BTN_STOP,  INPUT_PULLUP);
  pinMode(PIN_ESTOP,     INPUT_PULLUP);    // open circuit -> HIGH -> STOP (fail-safe)
  pinMode(PIN_LED_FAULT, OUTPUT);
  pinMode(PIN_XV1, OUTPUT);
  pinMode(PIN_XV2, OUTPUT);
  pinMode(PIN_XV3_VENT, OUTPUT);
  pinMode(PIN_LED_HEART, OUTPUT);
  safetyInit(safety);
  attachInterrupt(digitalPinToInterrupt(PIN_ESTOP), estopISR, RISING);
  if (digitalRead(PIN_ESTOP) == HIGH) estopTripped = true;   // already open at power-up
  Serial.println("H2 dispenser - Step 3b: E-stop interrupt + fixes");
}

void loop() {
  uint32_t now = millis();

  if (now - lastSample >= SAMPLE_MS) {
    lastSample += SAMPLE_MS;

    for (int i = 0; i < N_SENSORS; i++) sensorUpdate(sensors[i], (uint16_t)analogRead(sensors[i].pin));
    pairUpdate(pressure, now);
    for (int i = 0; i < 3; i++) rateUpdate(rates[i], now);

    bool start = pressedEdge(PIN_BTN_START, lastStart);
    bool stop  = pressedEdge(PIN_BTN_STOP,  lastStop);
    bool reset = pressedEdge(PIN_BTN_RESET, lastReset);

    if (reset) {
      logTime(now); Serial.println("RESET pressed");
      pairReset(pressure);
      for (int i = 0; i < 3; i++) rateReset(rates[i]);
      // the E-stop latch may only clear when the button is released (circuit closed)
      if (estopTripped && digitalRead(PIN_ESTOP) == LOW) estopTripped = false;
    }

    // report the reaction time of the E-stop interrupt (once)
    static bool estopReported = false;
    if (estopTripped && !estopReported) {
      uint32_t atUs; noInterrupts(); atUs = estopAtUs; interrupts();
      logTime(now); Serial.print("E-STOP: valves closed by ISR, loop noticed ");
      Serial.print((micros() - atUs) / 1000.0f, 2); Serial.println(" ms later");
      estopReported = true;
    }
    if (!estopTripped) estopReported = false;

    faults = collectFaults();
    if (faults != lastFaults) { logFaultChanges(now, lastFaults, faults); lastFaults = faults; }

    DispState before = safety.state;
    bool changed = safetyUpdate(safety, faults, start, stop, reset, now);
    if (changed) {
      logTime(now); Serial.print("STATE "); Serial.print(dispStateName(before));
      Serial.print(" -> "); Serial.println(dispStateName(safety.state));
    }

    // NEW: tell the operator WHY a command was refused
    if (start && safety.state != DS_FILLING) {
      logTime(now); Serial.print("START refused (state "); Serial.print(dispStateName(safety.state));
      Serial.print(")");
      if ((safety.state == DS_ESD || safety.state == DS_STOPPED) && worstAction(faults) < ACT_STOP)
        Serial.println(": causes cleared, shutdown is LATCHED -> press RESET first");
      else { Serial.print(", active:"); printReasons(faults, ACT_INHIBIT); }
    }
    if (reset && (safety.state == DS_STOPPED || safety.state == DS_ESD)) {
      logTime(now); Serial.print("RESET refused, still active:"); printReasons(faults, ACT_STOP);
    }

    writeOutputs(safety.out);
    digitalWrite(PIN_LED_FAULT, faults != 0);
  }

  if (now - lastPrint >= PRINT_MS) {
    lastPrint += PRINT_MS;
    digitalWrite(PIN_LED_HEART, !digitalRead(PIN_LED_HEART));
    Serial.print(dispStateName(safety.state));
    Serial.print("  P="); Serial.print(pressure.value, 1);
    Serial.print(" (PT1="); Serial.print(sensors[0].value, 1);
    Serial.print(" PT2="); Serial.print(sensors[1].value, 1); Serial.print(")");
    Serial.print("  T="); Serial.print(sensors[2].value, 1);
    Serial.print("  GD1="); Serial.print(sensors[3].value, 1);
    Serial.print("  faults=0x"); Serial.print(faults, HEX);
    Serial.print("  XV1="); Serial.print(digitalRead(PIN_XV1));
    Serial.print(" XV2="); Serial.print(digitalRead(PIN_XV2));
    Serial.print(" VENT="); Serial.println(digitalRead(PIN_XV3_VENT));
  }
}
