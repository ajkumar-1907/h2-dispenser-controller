// station_sim.h - the STM32 main loop (firmware/stm32_controller/sketch.ino),
// re-created on a PC so whole fills can be tested in milliseconds.
//
// It includes the REAL firmware headers (sensor_420.h, redundancy.h, ...), so
// the logic under test is the exact code that runs on the Nucleo. Only the
// Arduino I/O (analogRead, digitalRead, Serial) is replaced by plain variables.
// Keep collectFaults() and step() in line with sketch.ino when that changes.
#pragma once
#include "../firmware/stm32_controller/sensor_420.h"
#include "../firmware/stm32_controller/redundancy.h"
#include "../firmware/stm32_controller/rate_check.h"
#include "../firmware/stm32_controller/faults.h"
#include "../firmware/stm32_controller/safety.h"
#include "../firmware/stm32_controller/tank_model.h"
#include "../firmware/stm32_controller/fueling.h"

// engineering value -> ADC counts, through the 4-20 mA loop and 150 ohm shunt
inline uint16_t engToCounts(float v, float lo, float hi) {
  float ma = 4.0f + 16.0f * (v - lo) / (hi - lo);
  float c = ma * SHUNT_OHMS / 1000.0f / ADC_VREF * ADC_MAX;
  return (uint16_t)(c < 0 ? 0 : (c > 4095 ? 4095 : c));
}
inline uint16_t maToCounts(float ma) {
  float c = ma * SHUNT_OHMS / 1000.0f / ADC_VREF * ADC_MAX;
  return (uint16_t)(c < 0 ? 0 : (c > 4095 ? 4095 : c));
}

inline bool hyst(bool& active, float value, float trip, float clear) {
  if (!active && value >= trip)  active = true;
  if (active  && value <  clear) active = false;
  return active;
}

enum PtMode { PT_HEALTHY, PT1_CUT, PT1_STUCK, PT_BOTH_STUCK };

struct Station {
  // ---- the firmware objects (same initialisers as sketch.ino) ----
  Sensor     s[4];
  SensorPair pair;
  RateCheck  rates[3];
  Safety     safety;
  Tank       tank;
  Fueling    fuel;
  uint32_t   faults = 0, now = 0;
  bool leakLatched = false, noRiseLatched = false;
  bool opActive = false, tankTActive = false, gasWarnActive = false, gasAlarmActive = false;
  uint16_t pt1Frozen = 0, pt2Frozen = 0;

  // ---- what the test "does to the hardware" (the knobs and switches) ----
  float  tt1C    = -40.0f;   // pre-cooler outlet temperature
  float  gasLel  = 0.0f;     // gas detector reading
  float  pt2Ofs  = 0.0f;     // PT2 drift
  PtMode ptMode  = PT_HEALTHY;
  bool   leak    = false;    // hose leak switch
  bool   estop   = false;    // E-stop circuit open
  float  tt1mA   = -1;       // >= 0: force a raw TT1 current (wiring faults)

  static constexpr float P_TRIP = 87.5f, P_CLEAR = 85.0f;
  static constexpr float TANK_T_TRIP = 85.0f, TANK_T_CLEAR = 80.0f;
  static constexpr float PRECOOL_MAX_C = -33.0f;
  static constexpr float GAS_WARN_LEL = 10.0f, GAS_ALARM_LEL = 25.0f, GAS_HYST = 2.0f;

  Station() {
    s[0] = Sensor{ "PT1", "MPa",  0, 0.0f, 100.0f };
    s[1] = Sensor{ "PT2", "MPa",  0, 0.0f, 100.0f };
    s[2] = Sensor{ "TT1", "C",    0, -50.0f, 100.0f };
    s[3] = Sensor{ "GD1", "%LEL", 0, 0.0f, 100.0f };
    pair = SensorPair{ "P", &s[0], &s[1], 2.0f, 500 };
    rates[0] = RateCheck{ &s[0], 20.0f, 100 };
    rates[1] = RateCheck{ &s[1], 20.0f, 100 };
    rates[2] = RateCheck{ &s[2],  5.0f, 100 };
    safetyInit(safety);
    tankReset(tank);
    fuel = Fueling{};
  }

  void readSensors() {
    uint16_t c1 = engToCounts(tank.P, 0, 100), c2 = engToCounts(tank.P + pt2Ofs, 0, 100);
    switch (ptMode) {
      case PT_HEALTHY:    pt1Frozen = c1; pt2Frozen = c2; break;
      case PT1_CUT:       c1 = 0; break;
      case PT1_STUCK:     c1 = pt1Frozen; pt2Frozen = c2; break;
      case PT_BOTH_STUCK: c1 = pt1Frozen; c2 = pt2Frozen; break;
    }
    sensorUpdate(s[0], c1);
    sensorUpdate(s[1], c2);
    sensorUpdate(s[2], tt1mA >= 0 ? maToCounts(tt1mA) : engToCounts(tt1C, -50, 100));
    sensorUpdate(s[3], engToCounts(gasLel, 0, 100));
  }

  uint32_t collectFaults() {
    uint32_t f = 0;
    if (!usable(s[0])) f |= FLT_PT1_WIRING;
    if (!usable(s[1])) f |= FLT_PT2_WIRING;
    if (!usable(s[2])) f |= FLT_TT1_WIRING;
    if (!usable(s[3])) f |= FLT_GD1_WIRING;
    if (pair.state == PAIR_DEGRADED) f |= FLT_P_DEGRADED;
    if (pair.state == PAIR_DISAGREE) f |= FLT_P_DISAGREE;
    if (pair.state == PAIR_LOST || pair.state == PAIR_INIT) f |= FLT_P_LOST;
    if (rates[0].fault) f |= FLT_PT1_RATE;
    if (rates[1].fault) f |= FLT_PT2_RATE;
    if (rates[2].fault) f |= FLT_TT1_RATE;
    if (pair.state != PAIR_LOST && pair.state != PAIR_INIT)
      if (hyst(opActive, pair.value, P_TRIP, P_CLEAR)) f |= FLT_OVERPRESSURE;
    if (hyst(tankTActive, tank.T - 273.15f, TANK_T_TRIP, TANK_T_CLEAR)) f |= FLT_TANK_OVERTEMP;
    if (usable(s[2]) && s[2].value > PRECOOL_MAX_C) f |= FLT_PRECOOL;
    if (usable(s[3])) {
      if (hyst(gasWarnActive,  s[3].value, GAS_WARN_LEL,  GAS_WARN_LEL  - GAS_HYST)) f |= FLT_GAS_WARN;
      if (hyst(gasAlarmActive, s[3].value, GAS_ALARM_LEL, GAS_ALARM_LEL - GAS_HYST)) f |= FLT_GAS_ALARM;
    }
    if (estop)         f |= FLT_ESTOP;
    if (leakLatched)   f |= FLT_LEAK;
    if (noRiseLatched) f |= FLT_NO_RISE;
    return f;           // (HMI link bits are tested separately, in the link tests)
  }

  // one 10 ms tick of the main loop. Buttons are EDGES (true for one tick).
  void step(bool start = false, bool stop = false, bool reset = false) {
    now += 10;
    const float dt = 0.01f;
    readSensors();
    pairUpdate(pair, now);
    for (auto& r : rates) rateUpdate(r, now);

    if (reset) {
      pairReset(pair);
      for (auto& r : rates) rateReset(r);
      leakLatched = false; noRiseLatched = false;
    }
    if (stop && safety.state == DS_IDLE) { nextCar(); }

    faults = collectFaults();
    if (start && safety.state == DS_IDLE && tankSOC(tank) >= SOC_END) start = false;
    DispState before = safety.state;
    if (safetyUpdate(safety, faults, start, stop || fuel.done, reset, now)) {
      if (safety.state == DS_FILLING) fuelStart(fuel, pair.value, now);
      else if (before == DS_FILLING)  fuelAbort(fuel);
    }
    if (safety.state == DS_FILLING) {
      fuelStep(fuel, pair.value, tankSOC(tank), now, dt);
      if (fuel.leakFault)   leakLatched = true;
      if (fuel.noRiseFault) noRiseLatched = true;
    } else { fuel.u = 0; fuel.done = false; }

    bool flowPath = safety.out.xv1Supply && safety.out.xv2Dispense;
    tankStep(tank, flowPath ? fuel.u : 0.0f, usable(s[2]) ? s[2].value : 25.0f, leak, dt);
  }

  void nextCar() { tankReset(tank); rateRearm(rates[0]); rateRearm(rates[1]); }
  // power-up: sensors leave SNS_INIT, the pair leaves PAIR_INIT. Until then the
  // station sees "no pressure measurement" = ESD (fail-safe start), so the
  // operator's first action is RESET - exactly like in Wokwi.
  void powerUp() { run(1000); step(false, false, true); run(100); }
  void run(uint32_t ms) { for (uint32_t t = 0; t < ms; t += 10) step(); }
  float soc() const { return tankSOC(tank); }
  float tankC() const { return tank.T - 273.15f; }
};
