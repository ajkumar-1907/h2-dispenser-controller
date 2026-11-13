// fueling.h - the fueling SEQUENCE that runs while the safety layer says FILLING.
// Simplified, inspired by how real H70 protocols are structured
// (leak check -> controlled pressure ramp -> end of fill). Not SAE J2601.
#pragma once
#include <stdint.h>

enum FuelPhase { FP_IDLE, FP_LEAK_CHECK, FP_MAIN_FILL, FP_COMPLETE, FP_ABORTED };

inline const char* fuelPhaseName(FuelPhase p) {
  switch (p) {
    case FP_IDLE:       return "-";
    case FP_LEAK_CHECK: return "LEAKCHK";
    case FP_MAIN_FILL:  return "RAMP";
    case FP_COMPLETE:   return "DONE";
    case FP_ABORTED:    return "ABORT";
  }
  return "?";
}

// ---- parameters (example / demo values) ----
const uint32_t LEAK_HOLD_MS   = 3000;   // valve closed, pressure must hold
const float    LEAK_MAX_DROP  = 0.5f;   // MPa allowed drop during the hold
const float    RAMP_MPA_S     = 1.0f;   // target pressure ramp rate (demo speed)
const float    SOC_END        = 0.97f;  // normal end of fill
const float    P_END_MPA      = 84.0f;  // never ramp beyond this
const float    KP = 0.30f, KI = 0.20f;  // PI on pressure (valve opening per MPa)
const uint32_t RISE_WINDOW_MS = 5000;   // process-model check window
const float    RISE_MIN_MPA   = 0.5f;   // must rise at least this much ...
const float    RISE_MIN_U     = 0.2f;   // ... whenever the valve is this open

struct Fueling {
  FuelPhase phase;
  uint32_t  phaseSinceMs;
  float     pStart;      // pressure when the phase started
  float     pTarget;     // ramp target
  float     u;           // valve command 0..1
  float     integ;
  // process-model (no-rise) check
  uint32_t  winStartMs;
  float     winStartP;
  // results
  bool      leakFault;
  bool      noRiseFault;
  bool      done;
};

inline void fuelStart(Fueling& f, float pMeas, uint32_t now) {
  f.phase = FP_LEAK_CHECK; f.phaseSinceMs = now; f.pStart = pMeas;
  f.pTarget = pMeas; f.u = 0; f.integ = 0;
  f.leakFault = false; f.noRiseFault = false; f.done = false;
}

inline void fuelAbort(Fueling& f) {
  if (f.phase == FP_LEAK_CHECK || f.phase == FP_MAIN_FILL) f.phase = FP_ABORTED;
  f.u = 0;
}

// Returns true when the phase changed. dt in seconds.
inline bool fuelStep(Fueling& f, float pMeas, float soc, uint32_t now, float dt) {
  FuelPhase old = f.phase;
  switch (f.phase) {
    case FP_LEAK_CHECK:
      f.u = 0;                                         // hold closed
      if (f.pStart - pMeas > LEAK_MAX_DROP) { f.leakFault = true; f.phase = FP_ABORTED; }
      else if (now - f.phaseSinceMs >= LEAK_HOLD_MS) {
        f.phase = FP_MAIN_FILL; f.pTarget = pMeas;
        f.winStartMs = now; f.winStartP = pMeas;
      }
      break;

    case FP_MAIN_FILL: {
      // 1. move the target up at the ramp rate
      f.pTarget += RAMP_MPA_S * dt;
      if (f.pTarget > P_END_MPA) f.pTarget = P_END_MPA;
      // 2. PI controller: valve opening follows the pressure error
      float e = f.pTarget - pMeas;
      float uNew = KP * e + f.integ + KI * e * dt;
      if (uNew > 0.0f && uNew < 1.0f) f.integ += KI * e * dt;   // anti-windup
      f.u = uNew < 0 ? 0 : (uNew > 1 ? 1 : uNew);
      // 3. process-model check: valve open but pressure not rising?
      if (now - f.winStartMs >= RISE_WINDOW_MS) {
        if (f.u >= RISE_MIN_U && pMeas - f.winStartP < RISE_MIN_MPA) {
          f.noRiseFault = true; f.phase = FP_ABORTED; f.u = 0;
        }
        f.winStartMs = now; f.winStartP = pMeas;
      }
      // 4. end of fill
      if (soc >= SOC_END || pMeas >= P_END_MPA) { f.phase = FP_COMPLETE; f.u = 0; f.done = true; }
      break;
    }
    default:
      f.u = 0;
      break;
  }
  if (f.phase != old) f.phaseSinceMs = now;
  return f.phase != old;
}
