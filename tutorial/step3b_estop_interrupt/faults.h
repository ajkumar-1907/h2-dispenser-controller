// faults.h - the FAULT REGISTER: one bit per fault, many can be active at once.
// Fixes the Step 2 flaw where one state variable could hide a second fault.
#pragma once
#include <stdint.h>

enum FaultBit : uint32_t {
  FLT_PT1_WIRING   = 1u << 0,
  FLT_PT2_WIRING   = 1u << 1,
  FLT_TT1_WIRING   = 1u << 2,
  FLT_GD1_WIRING   = 1u << 3,
  FLT_P_DEGRADED   = 1u << 4,   // only one pressure sensor left
  FLT_P_DISAGREE   = 1u << 5,
  FLT_P_LOST       = 1u << 6,   // no pressure measurement at all
  FLT_PT1_RATE     = 1u << 7,
  FLT_PT2_RATE     = 1u << 8,
  FLT_TT1_RATE     = 1u << 9,
  FLT_GAS_WARN     = 1u << 10,  // GD1 >= 10 %LEL
  FLT_GAS_ALARM    = 1u << 11,  // GD1 >= 25 %LEL
  FLT_OVERPRESSURE = 1u << 12,  // P >= 87.5 MPa
  FLT_OVERTEMP     = 1u << 13,  // TT1 >= 85 C
  FLT_ESTOP        = 1u << 14,  // NEW: emergency-stop button / circuit open
};
const int N_FAULT_BITS = 15;

inline const char* faultName(int bit) {
  static const char* names[N_FAULT_BITS] = {
    "PT1_WIRING", "PT2_WIRING", "TT1_WIRING", "GD1_WIRING",
    "P_DEGRADED", "P_DISAGREE", "P_LOST",
    "PT1_RATE", "PT2_RATE", "TT1_RATE",
    "GAS_WARN", "GAS_ALARM", "OVERPRESSURE", "OVERTEMP", "ESTOP" };
  return (bit >= 0 && bit < N_FAULT_BITS) ? names[bit] : "?";
}

// ---------- CAUSE & EFFECT MATRIX ----------
// What the station must DO for each fault. Higher number = more severe.
enum Action : uint8_t {
  ACT_NONE    = 0,
  ACT_WARN    = 1,   // alarm to operator, keep running
  ACT_INHIBIT = 2,   // do not START a new fill
  ACT_STOP    = 3,   // controlled stop: close valves in sequence
  ACT_ESD     = 4    // emergency shutdown: close everything now, vent the hose
};

inline Action faultAction(uint32_t bit) {
  switch (bit) {
    case FLT_GAS_WARN:     return ACT_INHIBIT;
    case FLT_P_DEGRADED:   return ACT_STOP;    // Step 2 decision: no redundancy -> stop
    case FLT_P_DISAGREE:   return ACT_STOP;
    case FLT_PT1_RATE:
    case FLT_PT2_RATE:     return ACT_STOP;
    case FLT_TT1_WIRING:
    case FLT_TT1_RATE:     return ACT_STOP;    // cannot verify gas temperature
    case FLT_PT1_WIRING:
    case FLT_PT2_WIRING:   return ACT_WARN;    // covered by P_DEGRADED / P_LOST
    case FLT_GD1_WIRING:   return ACT_ESD;     // blind gas detector = assume gas
    case FLT_P_LOST:       return ACT_ESD;
    case FLT_GAS_ALARM:    return ACT_ESD;
    case FLT_OVERPRESSURE: return ACT_ESD;
    case FLT_OVERTEMP:     return ACT_ESD;
    case FLT_ESTOP:        return ACT_ESD;
  }
  return ACT_NONE;
}

inline const char* actionName(Action a) {
  static const char* n[] = { "none", "WARN", "INHIBIT", "STOP", "ESD" };
  return n[a];
}

// The most severe action required by all active faults together.
inline Action worstAction(uint32_t faults) {
  Action worst = ACT_NONE;
  for (int b = 0; b < N_FAULT_BITS; b++)
    if (faults & (1u << b)) {
      Action a = faultAction(1u << b);
      if (a > worst) worst = a;
    }
  return worst;
}
