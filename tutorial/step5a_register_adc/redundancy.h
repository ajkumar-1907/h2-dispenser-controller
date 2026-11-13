// redundancy.h - two sensors measuring the same thing: are they telling the truth?
#pragma once
#include "sensor_420.h"

enum PairState {
  PAIR_INIT,       // not yet checked
  PAIR_OK,         // both usable and they agree
  PAIR_DEGRADED,   // one sensor lost: running on the other one, no redundancy left
  PAIR_DISAGREE,   // both look healthy but report different values: one is lying
  PAIR_LOST        // both unusable: no measurement at all
};

struct SensorPair {
  const char* name;
  Sensor*  a;
  Sensor*  b;
  float    maxDiff;        // allowed difference [engineering units]
  uint32_t confirmMs;      // how long a difference must last before it counts
  // outputs
  PairState state;
  float     value;         // the value the safety logic should use
  float     diff;
  // internal
  uint32_t  diffSinceMs;
  bool      diffActive;
};

inline const char* pairName(PairState p) {
  switch (p) {
    case PAIR_INIT:     return "INIT";
    case PAIR_OK:       return "OK";
    case PAIR_DEGRADED: return "DEGRADED";
    case PAIR_DISAGREE: return "DISAGREE";
    case PAIR_LOST:     return "LOST";
  }
  return "?";
}

// Returns true when the pair state changed.
inline bool pairUpdate(SensorPair& p, uint32_t nowMs) {
  PairState old = p.state;
  bool okA = usable(*p.a), okB = usable(*p.b);

  if (okA && okB) {
    p.diff = p.a->value - p.b->value;
    if (p.diff < 0) p.diff = -p.diff;

    // Conservative choice for PRESSURE: use the HIGHER reading, so an
    // over-pressure check can never be fooled by the lower sensor.
    p.value = (p.a->value > p.b->value) ? p.a->value : p.b->value;

    if (p.diff > p.maxDiff) {
      if (!p.diffActive) { p.diffActive = true; p.diffSinceMs = nowMs; }
      if (nowMs - p.diffSinceMs >= p.confirmMs) p.state = PAIR_DISAGREE;
    } else {
      p.diffActive = false;
      if (p.state != PAIR_DISAGREE) p.state = PAIR_OK;   // DISAGREE is latched
    }
  } else if (okA || okB) {
    p.value = okA ? p.a->value : p.b->value;
    p.diff  = 0;
    p.diffActive = false;
    if (p.state != PAIR_DISAGREE) p.state = PAIR_DEGRADED;
  } else {
    p.value = 0;
    p.state = PAIR_LOST;
  }
  return p.state != old;
}

// Operator pressed RESET. DISAGREE clears only if the sensors agree NOW.
// FIX (Step 3b): go DIRECTLY to the correct state. Going to PAIR_INIT first
// made the station see "no pressure measurement" for one cycle = ESD,
// so the reset caused its own refusal.
inline bool pairReset(SensorPair& p) {
  if (p.state != PAIR_DISAGREE || p.diffActive) return false;
  bool okA = usable(*p.a), okB = usable(*p.b);
  if (okA && okB)      p.state = PAIR_OK;
  else if (okA || okB) p.state = PAIR_DEGRADED;
  else                 p.state = PAIR_LOST;
  return true;
}
