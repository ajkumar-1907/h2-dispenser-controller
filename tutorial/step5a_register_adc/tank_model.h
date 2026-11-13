// tank_model.h - SIMULATED vehicle tank + hose (stands in for the real car).
// Deliberately simple, for learning - NOT a validated fueling model:
//  - ideal-gas-like scaling instead of real-gas (compressibility) tables
//  - time-scaled: a full fill takes ~1 minute instead of ~3-5 minutes
#pragma once
#include <stdint.h>

const float TANK_FULL_KG   = 5.0f;    // mass at 70 MPa, 15 C (= 100 % SOC)
const float P_NOMINAL_MPA  = 70.0f;   // H70 nominal working pressure
const float T_REF_K        = 288.15f; // 15 C
const float P_SUPPLY_MPA   = 90.0f;   // station storage bank pressure
const float FLOW_K         = 0.017f;  // kg/s per sqrt(MPa) with valve fully open
const float GAMMA          = 1.4f;    // filling heats the gas: T -> gamma * T_in
const float H_COOL         = 0.002f;  // 1/s heat loss to ambient
const float T_AMB_K        = 298.15f; // 25 C ambient
const float LEAK_KG_S      = 0.020f;  // injected hose leak

struct Tank {
  float m;        // kg of H2 in tank + hose
  float T;        // gas temperature [K]
  float P;        // pressure [MPa]
  float flow;     // last inflow [kg/s]
};

inline float tankPressure(float m, float T) {
  return P_NOMINAL_MPA * (m / TANK_FULL_KG) * (T / T_REF_K);
}

inline void tankReset(Tank& t) {         // a new car arrives: 20 % full, warm
  t.m = 1.0f; t.T = T_AMB_K; t.flow = 0; t.P = tankPressure(t.m, t.T);
}

// u = valve opening 0..1, tInC = gas temperature delivered by the pre-cooler
inline void tankStep(Tank& t, float u, float tInC, bool leak, float dt) {
  float dp = P_SUPPLY_MPA - t.P;
  float flow = (u > 0 && dp > 0) ? FLOW_K * u * __builtin_sqrtf(dp) : 0.0f;
  float out  = leak ? LEAK_KG_S : 0.0f;
  float tIn  = tInC + 273.15f;
  t.T += ((flow / t.m) * (GAMMA * tIn - t.T) - H_COOL * (t.T - T_AMB_K)) * dt;
  t.m += (flow - out) * dt;
  if (t.m < 0.05f) t.m = 0.05f;
  t.flow = flow;
  t.P = tankPressure(t.m, t.T);
}

inline float tankSOC(const Tank& t) { return t.m / TANK_FULL_KG; }
