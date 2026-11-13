// ===== H2 dispenser controller - Step 5b: HMI link (request/response, heartbeat, remote stop) =====
#include "sensor_420.h"
#include "redundancy.h"
#include "rate_check.h"
#include "faults.h"
#include "safety.h"
#include "tank_model.h"
#include "fueling.h"
#include "hw_adc.h"      // Step 5a: register-level ADC (frees ~2 KB)
#include "link.h"        // Step 5b: HMI protocol

// ---- inputs ----
#define PIN_POT_PTINJ  PA0     // CHANGED: pressure-sensor fault injection (zones)
#define PIN_POT_PT2OFS PA1     // CHANGED: PT2 drift, centre = 0, +/-20 MPa
#define PIN_TT1        PA4     // pre-cooler outlet gas temperature (4-20 mA)
#define PIN_GD1        PA6     // gas detector (4-20 mA)
#define PIN_BTN_RESET  PB4
#define PIN_BTN_START  PB5
#define PIN_BTN_STOP   PB3     // also: in IDLE, STOP = "next car" (new empty tank)
#define PIN_ESTOP      PB6
#define PIN_LEAK_SW    PB7     // NEW: slide switch, injects a hose leak
// ---- outputs ----
#define PIN_LED_FAULT  PC6
#define PIN_XV1        PC7
#define PIN_XV2        PA8
#define PIN_XV3_VENT   PB0
#define PIN_LED_HEART  PA5

Sensor sensors[] = {
  { "PT1", "MPa",  0,   0.0f, 100.0f },   // pin 0: counts now come from the model
  { "PT2", "MPa",  0,   0.0f, 100.0f },
  { "TT1", "C",    PIN_TT1, -50.0f, 100.0f },
  { "GD1", "%LEL", PIN_GD1,   0.0f, 100.0f },
};
SensorPair pressure = { "P", &sensors[0], &sensors[1], 2.0f, 500 };
RateCheck rates[] = {
  { &sensors[0], 20.0f, 100 },
  { &sensors[1], 20.0f, 100 },
  { &sensors[2],  5.0f, 100 },
};

const float P_TRIP = 87.5f, P_CLEAR = 85.0f;
const float TANK_T_TRIP = 85.0f, TANK_T_CLEAR = 80.0f;
const float PRECOOL_MAX_C = -33.0f;          // start only with gas at or below this
const float GAS_WARN_LEL = 10.0f, GAS_ALARM_LEL = 25.0f, GAS_HYST = 2.0f;
bool opActive = false, tankTActive = false, gasWarnActive = false, gasAlarmActive = false;

Safety   safety;
Tank     tank;
Fueling  fuel;
uint32_t faults = 0, lastFaults = 0;
const uint32_t SAMPLE_MS = 10, PRINT_MS = 1000;
uint32_t lastSample = 0, lastPrint = 0;
bool lastReset = false, lastStart = false, lastStop = false;
bool leakLatched = false, noRiseLatched = false;

// ---------- Step 5b: HMI link ----------
// Today the HMI is YOU typing in the serial monitor (USART2).
// In Step 6 the ESP32 takes over: change this ONE line to Serial1 (USART1, PA9/PA10).
#define HMI Serial
const uint32_t LINK_TIMEOUT_MS = 5000;   // HMI must talk at least every 5 s
LineRx   rx;
uint32_t lastValidFrameMs = 0;
bool     linkSeen = false, remoteEsdLatched = false, remoteStopReq = false;
uint32_t framesOk = 0, framesBad = 0;

// ---------- E-stop interrupt (unchanged from 3b) ----------
volatile bool     estopTripped = false;
volatile uint32_t estopAtUs    = 0;
void estopISR() {
  digitalWrite(PIN_XV1, LOW); digitalWrite(PIN_XV2, LOW); digitalWrite(PIN_XV3_VENT, HIGH);
  if (!estopTripped) estopAtUs = micros();
  estopTripped = true;
}
void writeOutputs(const Valves& v) {
  noInterrupts();
  if (estopTripped) { digitalWrite(PIN_XV1, LOW); digitalWrite(PIN_XV2, LOW); digitalWrite(PIN_XV3_VENT, HIGH); }
  else { digitalWrite(PIN_XV1, v.xv1Supply); digitalWrite(PIN_XV2, v.xv2Dispense); digitalWrite(PIN_XV3_VENT, !v.xv3VentClosed); }
  interrupts();
}

// ---------- NEW: simulated pressure transmitters (model -> mA -> counts) ----------
uint16_t pressureToCounts(float mpa) {
  float ma = 4.0f + 16.0f * mpa / 100.0f;
  float c = ma * SHUNT_OHMS / 1000.0f / ADC_VREF * ADC_MAX;
  return (uint16_t)(c < 0 ? 0 : (c > 4095 ? 4095 : c));
}

uint16_t pt1Frozen = 0, pt2Frozen = 0;
void readPressureSensors() {
  int inj = adcRead(0);              // fault injection zones
  float ofs = (adcRead(1) - 2048) / 2048.0f * 20.0f;
  if (ofs > -1.5f && ofs < 1.5f) ofs = 0;          // dead band around centre
  uint16_t c1 = pressureToCounts(tank.P);
  uint16_t c2 = pressureToCounts(tank.P + ofs);
  if (inj < 410)       c1 = 0;                      // zone A: PT1 cable cut
  else if (inj < 1638) { pt1Frozen = c1; pt2Frozen = c2; }   // zone B: healthy
  else if (inj < 2867) { c1 = pt1Frozen; pt2Frozen = c2; }   // zone C: PT1 stuck
  else                 { c1 = pt1Frozen; c2 = pt2Frozen; }   // zone D: BOTH stuck
  sensorUpdate(sensors[0], c1);
  sensorUpdate(sensors[1], c2);
}

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
  // tank temperature: on a real car it is sent by the vehicle (IR link, SAE J2799)
  if (hyst(tankTActive, tank.T - 273.15f, TANK_T_TRIP, TANK_T_CLEAR)) f |= FLT_TANK_OVERTEMP;
  if (usable(sensors[2]) && sensors[2].value > PRECOOL_MAX_C) f |= FLT_PRECOOL;
  if (usable(sensors[3])) {
    if (hyst(gasWarnActive,  sensors[3].value, GAS_WARN_LEL,  GAS_WARN_LEL  - GAS_HYST)) f |= FLT_GAS_WARN;
    if (hyst(gasAlarmActive, sensors[3].value, GAS_ALARM_LEL, GAS_ALARM_LEL - GAS_HYST)) f |= FLT_GAS_ALARM;
  }
  if (estopTripped || digitalRead(PIN_ESTOP) == HIGH) f |= FLT_ESTOP;
  if (leakLatched)   f |= FLT_LEAK;
  if (noRiseLatched) f |= FLT_NO_RISE;
  if (!linkSeen || millis() - lastValidFrameMs > LINK_TIMEOUT_MS) f |= FLT_HMI_LINK;
  if (remoteEsdLatched) f |= FLT_REMOTE_ESD;
  return f;
}

void logTime(uint32_t now) { Serial.print('['); Serial.print(now); Serial.print(" ms] "); }

void logFaultChanges(uint32_t now, uint32_t oldF, uint32_t newF) {
  uint32_t changed = oldF ^ newF;
  for (int b = 0; b < N_FAULT_BITS; b++) {
    if (!(changed & (1u << b))) continue;
    logTime(now);
    Serial.print((newF & (1u << b)) ? "FAULT+ " : "FAULT- ");
    Serial.print(faultName(b));
    if (newF & (1u << b)) { Serial.print("  -> "); Serial.print(actionName(faultAction(1u << b))); }
    Serial.println();
  }
}

void printReasons(uint32_t f, Action minAct) {
  for (int b = 0; b < N_FAULT_BITS; b++)
    if ((f & (1u << b)) && faultAction(1u << b) >= minAct) { Serial.print(' '); Serial.print(faultName(b)); }
  Serial.println();
}

bool pressedEdge(uint32_t pin, bool& last) {
  bool now = digitalRead(pin) == LOW;
  bool edge = now && !last;
  last = now;
  return edge;
}

// ---------- Step 5b: answer one HMI request ----------
void reply(TxFrame& t) { HMI.println(txEnd(t)); }

void handleFrame(char* body, uint32_t now) {
  TxFrame t; txBegin(t);
  framesOk++;
  lastValidFrameMs = now;
  if (!linkSeen) { linkSeen = true; logTime(now); Serial.println("HMI link established"); }

  if (!strcmp(body, "HB")) {                         // heartbeat: "I am alive"
    txStr(t, "ACK,HB");
  } else if (!strcmp(body, "ST")) {                  // status request
    txStr(t, "ST,");  txStr(t, dispStateName(safety.state));
    txStr(t, ",");    txStr(t, fuelPhaseName(fuel.phase));
    txStr(t, ",");    txInt(t, (int32_t)(pressure.value * 10));         // 0.1 MPa
    txStr(t, ",");    txInt(t, (int32_t)((tank.T - 273.15f) * 10));     // 0.1 C
    txStr(t, ",");    txInt(t, (int32_t)(tankSOC(tank) * 100));         // %
    txStr(t, ",");    txHex(t, faults, 5);                              // fault register
    txStr(t, ",");    txInt(t, (int32_t)framesBad);
  } else if (!strcmp(body, "STOP")) {                // remote normal stop: makes it SAFER
    if (safety.state == DS_FILLING) { remoteStopReq = true; txStr(t, "ACK,STOP"); }
    else txStr(t, "NAK,STOP,NOT_FILLING");
  } else if (!strcmp(body, "ESD")) {                 // remote emergency shutdown: SAFER
    remoteEsdLatched = true; txStr(t, "ACK,ESD");
  } else if (!strcmp(body, "NC")) {                  // Step 6: "next car" from the HMI's own button
    if (safety.state == DS_IDLE) {
      tankReset(tank); rateRearm(rates[0]); rateRearm(rates[1]);
      txStr(t, "ACK,NC");
    } else txStr(t, "NAK,NC,NOT_IDLE");
  } else if (!strcmp(body, "START") || !strcmp(body, "RESET")) {
    txStr(t, "NAK,"); txStr(t, body); txStr(t, ",LOCAL_ONLY");   // makes it LESS safe: refused
  } else {
    framesOk--; framesBad++;
    txStr(t, "NAK,UNKNOWN");
  }
  logTime(now); Serial.print("HMI cmd: "); Serial.println(body);
  reply(t);
}

void serviceLink(uint32_t now) {
  while (HMI.available()) {
    char* body;
    FrameResult fr = lineFeed(rx, (char)HMI.read(), body);
    if (fr == FR_NONE) continue;
    if (fr == FR_OK) { handleFrame(body, now); continue; }
    framesBad++;                                      // every bad frame is counted
    TxFrame t; txBegin(t); txStr(t, "NAK,FRAME,"); txInt(t, fr); reply(t);
  }
}

void setup() {
  Serial.begin(115200);
  adcInit();
  pinMode(PIN_BTN_RESET, INPUT_PULLUP);
  pinMode(PIN_BTN_START, INPUT_PULLUP);
  pinMode(PIN_BTN_STOP,  INPUT_PULLUP);
  pinMode(PIN_ESTOP,     INPUT_PULLUP);
  pinMode(PIN_LEAK_SW,   INPUT_PULLUP);
  pinMode(PIN_LED_FAULT, OUTPUT);
  pinMode(PIN_XV1, OUTPUT); pinMode(PIN_XV2, OUTPUT); pinMode(PIN_XV3_VENT, OUTPUT);
  pinMode(PIN_LED_HEART, OUTPUT);
  safetyInit(safety);
  tankReset(tank);
  attachInterrupt(digitalPinToInterrupt(PIN_ESTOP), estopISR, RISING);
  if (digitalRead(PIN_ESTOP) == HIGH) estopTripped = true;
  Serial.println("H2 dispenser - Step 5b: HMI link");
}

void loop() {
  uint32_t now = millis();

  if (now - lastSample >= SAMPLE_MS) {
    lastSample += SAMPLE_MS;
    const float dt = SAMPLE_MS / 1000.0f;

    // 1. measure
    readPressureSensors();
    sensorUpdate(sensors[2], adcRead(4));
    sensorUpdate(sensors[3], adcRead(6));
    pairUpdate(pressure, now);
    for (int i = 0; i < 3; i++) rateUpdate(rates[i], now);

    // 2. operator (local buttons + HMI link)
    serviceLink(now);
    bool start = pressedEdge(PIN_BTN_START, lastStart);
    bool stop  = pressedEdge(PIN_BTN_STOP,  lastStop);
    bool remoteStop = remoteStopReq; remoteStopReq = false;
    bool reset = pressedEdge(PIN_BTN_RESET, lastReset);
    if (stop)  { logTime(now); Serial.println("STOP pressed"); }      // NEW: log every command
    if (start) { logTime(now); Serial.println("START pressed"); }
    if (reset) {
      logTime(now); Serial.println("RESET pressed");
      pairReset(pressure);
      for (int i = 0; i < 3; i++) rateReset(rates[i]);
      if (estopTripped && digitalRead(PIN_ESTOP) == LOW) estopTripped = false;
      leakLatched = false; noRiseLatched = false;
      remoteEsdLatched = false;                    // remote ESD needs a LOCAL reset
    }
    if (stop && safety.state == DS_IDLE) {
      tankReset(tank);
      rateRearm(rates[0]); rateRearm(rates[1]);     // NEW: expected pressure jump, re-arm (not disable)
      logTime(now); Serial.println("Next car: new tank at 20 % SOC");
    }

    // 3. faults -> safety layer (fueling 'done' acts like a normal STOP)
    faults = collectFaults();
    if (faults != lastFaults) { logFaultChanges(now, lastFaults, faults); lastFaults = faults; }

    DispState before = safety.state;
    if (start && safety.state == DS_IDLE && tankSOC(tank) >= SOC_END) {
      logTime(now); Serial.println("START refused: tank already full (press STOP for next car)");
      start = false;
    }
    if (safetyUpdate(safety, faults, start, stop || remoteStop || fuel.done, reset, now)) {
      logTime(now); Serial.print("STATE "); Serial.print(dispStateName(before));
      Serial.print(" -> "); Serial.println(dispStateName(safety.state));
      if (safety.state == DS_FILLING) fuelStart(fuel, pressure.value, now);
      else if (before == DS_FILLING)  fuelAbort(fuel);
    }
    if (start && safety.state != DS_FILLING) {
      logTime(now); Serial.print("START refused (state "); Serial.print(dispStateName(safety.state));
      Serial.print(')');
      if ((safety.state == DS_ESD || safety.state == DS_STOPPED) && worstAction(faults) < ACT_STOP)
        Serial.println(": shutdown LATCHED -> press RESET first");
      else { Serial.print(", active:"); printReasons(faults, ACT_INHIBIT); }
    }
    if (reset && (safety.state == DS_STOPPED || safety.state == DS_ESD)) {
      logTime(now); Serial.print("RESET refused, still active:"); printReasons(faults, ACT_STOP);
    }

    // 4. fueling sequence (only while the safety layer says FILLING)
    if (safety.state == DS_FILLING) {
      if (fuelStep(fuel, pressure.value, tankSOC(tank), now, dt)) {
        logTime(now); Serial.print("FUEL -> "); Serial.println(fuelPhaseName(fuel.phase));
      }
      if (fuel.leakFault)   leakLatched = true;
      if (fuel.noRiseFault) noRiseLatched = true;
    } else {
      fuel.u = 0;
      fuel.done = false;
    }

    // 5. the simulated car reacts to OUR valve (only if the valves are really open)
    bool flowPath = digitalRead(PIN_XV1) && digitalRead(PIN_XV2);
    tankStep(tank, flowPath ? fuel.u : 0.0f, usable(sensors[2]) ? sensors[2].value : 25.0f,
             digitalRead(PIN_LEAK_SW) == LOW, dt);

    writeOutputs(safety.out);
    Action worst = worstAction(faults);             // solid = must act, blinking = warning only
    digitalWrite(PIN_LED_FAULT, worst >= ACT_INHIBIT || (worst == ACT_WARN && (now / 500) % 2));
  }

  if (now - lastPrint >= PRINT_MS) {
    lastPrint += PRINT_MS;
    digitalWrite(PIN_LED_HEART, !digitalRead(PIN_LED_HEART));
    Serial.print(dispStateName(safety.state)); Serial.print('/'); Serial.print(fuelPhaseName(fuel.phase));
    Serial.print("  P=");    Serial.print(pressure.value, 1);
    Serial.print(" (tgt ");  Serial.print(fuel.pTarget, 1);
    Serial.print(") u=");    Serial.print(fuel.u, 2);
    Serial.print("  Ttank="); Serial.print(tank.T - 273.15f, 1);
    Serial.print("  SOC=");  Serial.print(tankSOC(tank) * 100.0f, 0);
    Serial.print("%  TT1="); Serial.print(sensors[2].value, 1);
    Serial.print("  GD1=");  Serial.print(sensors[3].value, 1);
    Serial.print("  f=0x");  Serial.println(faults, HEX);
  }
}