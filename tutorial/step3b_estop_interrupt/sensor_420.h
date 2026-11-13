// sensor_420.h - one 4-20 mA transmitter: counts -> mA -> value + NE43 status
// No Arduino calls in here (only plain C++), so it can be unit-tested on a PC.
#pragma once
#include <stdint.h>

const float ADC_VREF   = 3.3f;
const float ADC_MAX    = 4095.0f;
const float SHUNT_OHMS = 150.0f;
const uint8_t CONFIRM_N = 3;      // samples a new status must repeat
const float   ALPHA     = 0.3f;   // low-pass filter for the value

enum SensorStatus { SNS_INIT, SNS_OK, SNS_UNDER, SNS_OVER, SNS_FAULT_OPEN, SNS_FAULT_SHORT };

struct Sensor {
  const char* tag;
  const char* unit;
  uint32_t    pin;
  float       lo, hi;
  float        maRaw, maFilt, value;
  SensorStatus status, candidate;
  uint8_t      count;
  uint32_t     faultEvents;
  bool         started;
};

inline SensorStatus classify(float ma) {
  if (ma < 3.6f)  return SNS_FAULT_OPEN;
  if (ma > 21.0f) return SNS_FAULT_SHORT;
  if (ma < 3.8f)  return SNS_UNDER;
  if (ma > 20.5f) return SNS_OVER;
  return SNS_OK;
}

inline const char* statusName(SensorStatus s) {
  switch (s) {
    case SNS_INIT:        return "INIT";
    case SNS_OK:          return "OK";
    case SNS_UNDER:       return "UNDER";
    case SNS_OVER:        return "OVER";
    case SNS_FAULT_OPEN:  return "FAULT-OPEN";
    case SNS_FAULT_SHORT: return "FAULT-SHORT";
  }
  return "?";
}

inline float maToEng(float ma, float lo, float hi) {
  if (ma < 4.0f)  ma = 4.0f;
  if (ma > 20.0f) ma = 20.0f;
  return lo + (ma - 4.0f) * (hi - lo) / 16.0f;
}

inline bool usable(const Sensor& s) {
  return s.status == SNS_OK || s.status == SNS_UNDER || s.status == SNS_OVER;
}

// Process one ADC sample. Returns true when the confirmed status changed
// (the caller decides what to do with that, e.g. print an event).
inline bool sensorUpdate(Sensor& s, uint16_t counts) {
  float ma = counts * (ADC_VREF / ADC_MAX) / SHUNT_OHMS * 1000.0f;
  s.maRaw = ma;

  if (!s.started) { s.maFilt = ma; s.started = true; }
  s.maFilt += ALPHA * (ma - s.maFilt);
  s.value   = maToEng(s.maFilt, s.lo, s.hi);

  SensorStatus raw = classify(ma);
  if (raw == s.status) { s.count = 0; return false; }
  if (raw != s.candidate) { s.candidate = raw; s.count = 0; }
  if (++s.count < CONFIRM_N) return false;

  s.status = raw;
  s.count  = 0;
  if (!usable(s)) s.faultEvents++;
  return true;
}
