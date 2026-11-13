// rate_check.h - "can the real process really change this fast?"
// A pressure pipe cannot jump 30 MPa in 0.1 s. If the reading does, the
// SIGNAL is broken (loose contact, EMI spike, failing transmitter), not the pipe.
#pragma once
#include "sensor_420.h"

struct RateCheck {
  Sensor*  s;
  float    maxRate;        // allowed change [units per second]
  uint32_t windowMs;       // measure the change over this time
  // outputs
  float    rate;           // last measured rate [units/s]
  bool     fault;          // latched until operator reset
  uint32_t trips;
  // internal
  float    lastValue;
  uint32_t lastMs;
  bool     started;
};

// Returns true when a NEW rate fault was latched.
inline bool rateUpdate(RateCheck& r, uint32_t nowMs) {
  if (!usable(*r.s)) { r.started = false; return false; }   // wiring fault wins

  // use the RAW current: the low-pass filter would hide fast jumps
  float v = maToEng(r.s->maRaw, r.s->lo, r.s->hi);

  if (!r.started) { r.lastValue = v; r.lastMs = nowMs; r.started = true; return false; }
  uint32_t dt = nowMs - r.lastMs;
  if (dt < r.windowMs) return false;

  r.rate = (v - r.lastValue) * 1000.0f / (float)dt;
  r.lastValue = v;
  r.lastMs = nowMs;

  float mag = r.rate < 0 ? -r.rate : r.rate;
  if (mag > r.maxRate && !r.fault) { r.fault = true; r.trips++; return true; }
  return false;
}

inline bool rateReset(RateCheck& r) {
  if (!r.fault) return false;
  r.fault = false;
  r.started = false;       // start measuring fresh
  return true;
}

// Restart measuring from the next sample, e.g. after a nozzle disconnect/reconnect.
// The check stays ACTIVE - it just doesn't compare across the expected jump.
inline void rateRearm(RateCheck& r) { r.started = false; }
