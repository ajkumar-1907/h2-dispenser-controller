// safety.h - turns "what is wrong" (fault register) into "what the valves do".
// Pure logic, no Arduino calls: unit-testable on a PC.
//
// Valves are NORMALLY CLOSED and SPRING-RETURN: power ON = open.
// So if the controller dies or loses power, every valve closes by itself.
// The vent valve is NORMALLY OPEN: power ON = closed. No power = hose vented.
#pragma once
#include "faults.h"

enum DispState { DS_IDLE, DS_FILLING, DS_STOPPING, DS_STOPPED, DS_ESD };

inline const char* dispStateName(DispState s) {
  switch (s) {
    case DS_IDLE:     return "IDLE";
    case DS_FILLING:  return "FILLING";
    case DS_STOPPING: return "STOPPING";
    case DS_STOPPED:  return "STOPPED";
    case DS_ESD:      return "ESD";
  }
  return "?";
}

struct Valves {
  bool xv1Supply;     // supply isolation valve      (true = open)
  bool xv2Dispense;   // dispenser valve to nozzle   (true = open)
  bool xv3VentClosed; // vent valve energised        (true = closed, NOT venting)
};

const uint32_t STOP_SEQ_MS = 1000;   // dispenser valve closes first, supply 1 s later

struct Safety {
  DispState state;
  uint32_t  stateSinceMs;
  Valves    out;
  uint32_t  latched;        // faults that caused STOP/ESD, kept until reset
};

inline void safetyInit(Safety& s) {
  s.state = DS_IDLE; s.stateSinceMs = 0; s.latched = 0;
  s.out = { false, false, true };
}

// Returns true when the state changed.
inline bool safetyUpdate(Safety& s, uint32_t faults, bool startPressed,
                         bool stopPressed, bool resetPressed, uint32_t now) {
  DispState old = s.state;
  Action act = worstAction(faults);

  // ---- 1. ESD overrides everything, from any state ----
  if (act >= ACT_ESD && s.state != DS_ESD) {
    s.state = DS_ESD; s.latched |= faults;
  }

  switch (s.state) {
    case DS_IDLE:
      if (startPressed && act < ACT_INHIBIT) s.state = DS_FILLING;
      break;

    case DS_FILLING:
      if (act >= ACT_STOP) { s.state = DS_STOPPING; s.latched |= faults; }
      else if (stopPressed) s.state = DS_STOPPING;          // normal end by user
      break;

    case DS_STOPPING:                                       // sequenced close
      if (now - s.stateSinceMs >= STOP_SEQ_MS)
        s.state = s.latched ? DS_STOPPED : DS_IDLE;
      break;

    case DS_STOPPED:                                        // waits for RESET
    case DS_ESD:
      // reset is accepted only if nothing that requires STOP/ESD is still active
      if (resetPressed && act < ACT_STOP) { s.state = DS_IDLE; s.latched = 0; }
      break;
  }
  if (s.state != old) s.stateSinceMs = now;

  // ---- 2. outputs follow ONLY from the state ----
  switch (s.state) {
    case DS_IDLE:     s.out = { false, false, true  }; break;
    case DS_FILLING:  s.out = { true,  true,  true  }; break;
    case DS_STOPPING: s.out = { true,  false, true  }; break;  // XV2 first, XV1 still open
    case DS_STOPPED:  s.out = { false, false, true  }; break;
    case DS_ESD:      s.out = { false, false, false }; break;  // all closed + VENT
  }
  return s.state != old;
}
