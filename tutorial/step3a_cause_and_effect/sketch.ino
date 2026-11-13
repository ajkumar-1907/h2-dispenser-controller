// ===== H2 dispenser controller - Step 3a: fault register + cause & effect + valves =====
#include "sensor_420.h"
#include "redundancy.h"
#include "rate_check.h"
#include "faults.h"
#include "safety.h"

// ---- inputs ----
#define PIN_BTN_RESET  PB4
#define PIN_BTN_START  PB5     // NEW
#define PIN_BTN_STOP   PB3     // NEW
// ---- outputs ----
#define PIN_LED_FAULT  PC6     // red    : any fault active
#define PIN_XV1        PC7     // green  : supply valve open
#define PIN_XV2        PA8     // blue   : dispenser valve open
#define PIN_XV3_VENT   PB0     // orange : ON = venting (valve de-energised)
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

// process limits (example values)
const float P_MAX_MPA     = 87.5f;   // 125 % of 70 MPa nominal working pressure
const float T_MAX_C       = 85.0f;
const float GAS_WARN_LEL  = 10.0f;
const float GAS_ALARM_LEL = 25.0f;

Safety   safety;
uint32_t faults = 0, lastFaults = 0;
const uint32_t SAMPLE_MS = 10, PRINT_MS = 1000;
uint32_t lastSample = 0, lastPrint = 0;
bool lastReset = false, lastStart = false, lastStop = false;

// ---------- build the fault register from all checks ----------
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
  // process limits: only judge a value that is trustworthy
  if (pressure.state == PAIR_OK || pressure.state == PAIR_DEGRADED || pressure.state == PAIR_DISAGREE)
    if (pressure.value >= P_MAX_MPA) f |= FLT_OVERPRESSURE;
  if (usable(sensors[2]) && sensors[2].value >= T_MAX_C) f |= FLT_OVERTEMP;
  if (usable(sensors[3])) {
    if (sensors[3].value >= GAS_WARN_LEL)  f |= FLT_GAS_WARN;
    if (sensors[3].value >= GAS_ALARM_LEL) f |= FLT_GAS_ALARM;
  }
  return f;
}

void logTime(uint32_t now) { Serial.print("["); Serial.print(now); Serial.print(" ms] "); }

// print every bit that turned ON or OFF since last time
void logFaultChanges(uint32_t now, uint32_t oldF, uint32_t newF) {
  uint32_t changed = oldF ^ newF;               // XOR: bits that differ
  for (int b = 0; b < N_FAULT_BITS; b++) {
    if (!(changed & (1u << b))) continue;
    logTime(now);
    Serial.print((newF & (1u << b)) ? "FAULT+ " : "FAULT- ");
    Serial.print(faultName(b));
    if (newF & (1u << b)) {
      static const char* actName[] = { "none", "WARN", "INHIBIT", "STOP", "ESD" };
      Serial.print("  -> action "); Serial.print(actName[faultAction(1u << b)]);
    }
    Serial.println();
  }
}

// true only at the moment the button goes down
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
  pinMode(PIN_LED_FAULT, OUTPUT);
  pinMode(PIN_XV1, OUTPUT);
  pinMode(PIN_XV2, OUTPUT);
  pinMode(PIN_XV3_VENT, OUTPUT);
  pinMode(PIN_LED_HEART, OUTPUT);
  safetyInit(safety);
  Serial.println("H2 dispenser - Step 3a: fault register + cause & effect");
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
    if (reset) {                              // reset the latched plausibility checks too
      pairReset(pressure);
      for (int i = 0; i < 3; i++) rateReset(rates[i]);
      logTime(now); Serial.println("RESET pressed");
    }
    if (start) { logTime(now); Serial.println("START pressed"); }
    if (stop)  { logTime(now); Serial.println("STOP pressed"); }

    faults = collectFaults();
    if (faults != lastFaults) { logFaultChanges(now, lastFaults, faults); lastFaults = faults; }

    DispState before = safety.state;
    if (safetyUpdate(safety, faults, start, stop, reset, now)) {
      logTime(now); Serial.print("STATE "); Serial.print(dispStateName(before));
      Serial.print(" -> "); Serial.println(dispStateName(safety.state));
    }
    if (start && safety.state == DS_IDLE) {
      logTime(now); Serial.println("START refused: inhibit/stop/ESD condition active");
    }

    // outputs: hardware follows the safety logic, nothing else touches them
    digitalWrite(PIN_XV1, safety.out.xv1Supply);
    digitalWrite(PIN_XV2, safety.out.xv2Dispense);
    digitalWrite(PIN_XV3_VENT, !safety.out.xv3VentClosed);   // LED on = venting
    digitalWrite(PIN_LED_FAULT, faults != 0);
  }

  if (now - lastPrint >= PRINT_MS) {
    lastPrint += PRINT_MS;
    digitalWrite(PIN_LED_HEART, !digitalRead(PIN_LED_HEART));
    Serial.print(dispStateName(safety.state));
    Serial.print("  P="); Serial.print(pressure.value, 1);
    Serial.print("  T="); Serial.print(sensors[2].value, 1);
    Serial.print("  GD1="); Serial.print(sensors[3].value, 1);
    Serial.print("  faults=0x"); Serial.print(faults, HEX);
    Serial.print("  XV1="); Serial.print(safety.out.xv1Supply);
    Serial.print(" XV2="); Serial.print(safety.out.xv2Dispense);
    Serial.print(" VENT="); Serial.println(!safety.out.xv3VentClosed);
  }
}
