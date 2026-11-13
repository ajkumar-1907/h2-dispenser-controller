// sim_controller.h - a SIMULATED STM32 dispenser controller, for Wokwi only.
//
// Wokwi cannot wire two microcontroller projects together, so this file plays
// the STM32: it receives the HMI's frames, answers with EXACTLY the same
// protocol as the real Step 5 firmware (same link.h, same faults.h), and runs
// a tiny scripted fill. On real hardware: set USE_SIM_CONTROLLER 0 in the
// sketch and connect ESP32 GPIO17(TX)->STM32 PA10(RX), GPIO16(RX)<-PA9(TX), GND.
#pragma once
#include "link.h"
#include "faults.h"

// ---- a software "wire": bytes written at one end come out at the other ----
struct Pipe {
  char buf[256]; uint16_t head, tail;
  void put(char c) { uint16_t n = (head + 1) % 256; if (n != tail) { buf[head] = c; head = n; } }
  bool get(char& c) { if (head == tail) return false; c = buf[tail]; tail = (tail + 1) % 256; return true; }
  void putLine(const char* s) { while (*s) put(*s++); put('\n'); }
};

struct SimController {
  // what the panel at the dispenser does (local, at the STM32)
  enum { IDLE, FILLING, STOPPING, ESD } st;
  uint8_t  phase;        // 0 '-', 1 LEAKCHK, 2 RAMP, 3 DONE, 4 ABORT
  float    p, t, soc;
  uint32_t faults, stateMs, framesBad;
  bool     remoteEsd;
  LineRx   rx;
};

inline const char* simStateName(int s) {
  static const char* n[] = { "IDLE", "FILLING", "STOPPING", "ESD" };
  return n[s];
}
inline const char* simPhaseName(int p) {
  static const char* n[] = { "-", "LEAKCHK", "RAMP", "DONE", "ABORT" };
  return n[p];
}

inline void simInit(SimController& s) {
  s.st = SimController::IDLE; s.phase = 0; s.p = 14.5f; s.t = 25.0f; s.soc = 0.20f;
  s.faults = 0; s.stateMs = 0; s.framesBad = 0; s.remoteEsd = false;
  s.rx.len = 0; s.rx.overflow = false;
}

// the simulated STM32 answers one request (same rules as the real firmware)
inline void simAnswer(SimController& s, char* body, Pipe& out, uint32_t now) {
  TxFrame t; txBegin(t);
  if (!strcmp(body, "HB")) txStr(t, "ACK,HB");
  else if (!strcmp(body, "ST")) {
    txStr(t, "ST,"); txStr(t, simStateName(s.st));
    txStr(t, ",");   txStr(t, simPhaseName(s.phase));
    txStr(t, ",");   txInt(t, (int32_t)(s.p * 10));
    txStr(t, ",");   txInt(t, (int32_t)(s.t * 10));
    txStr(t, ",");   txInt(t, (int32_t)(s.soc * 100));
    txStr(t, ",");   txHex(t, s.faults, 5);
    txStr(t, ",");   txInt(t, (int32_t)s.framesBad);
  }
  else if (!strcmp(body, "STOP")) {
    if (s.st == SimController::FILLING) { s.st = SimController::STOPPING; s.stateMs = now; s.phase = 4; txStr(t, "ACK,STOP"); }
    else txStr(t, "NAK,STOP,NOT_FILLING");
  }
  else if (!strcmp(body, "ESD")) { s.remoteEsd = true; txStr(t, "ACK,ESD"); }
  else if (!strcmp(body, "NC")) {
    if (s.st == SimController::IDLE) { s.p = 14.5f; s.t = 25.0f; s.soc = 0.20f; s.phase = 0; txStr(t, "ACK,NC"); }
    else txStr(t, "NAK,NC,NOT_IDLE");
  }
  else if (!strcmp(body, "START") || !strcmp(body, "RESET")) { txStr(t, "NAK,"); txStr(t, body); txStr(t, ",LOCAL_ONLY"); }
  else { s.framesBad++; txStr(t, "NAK,UNKNOWN"); }
  out.putLine(txEnd(t));
}

// call every 10 ms. localStart/localReset = the buttons ON THE DISPENSER (sim)
inline void simStep(SimController& s, Pipe& in, Pipe& out, bool injectFault, bool linkCut,
                    bool localStart, bool localReset, uint32_t now) {
  // 1. receive (a cut cable: bytes are simply lost)
  char c, *body;
  while (in.get(c)) {
    if (linkCut) continue;
    FrameResult fr = lineFeed(s.rx, c, body);
    if (fr == FR_OK) simAnswer(s, body, out, now);
    else if (fr != FR_NONE) {
      s.framesBad++;
      TxFrame t; txBegin(t); txStr(t, "NAK,FRAME,"); txInt(t, fr); out.putLine(txEnd(t));
    }
  }
  // 2. faults: injected ones + latched remote ESD
  s.faults = 0;
  if (injectFault) s.faults |= FLT_GAS_WARN | FLT_P_DISAGREE;
  if (s.remoteEsd) s.faults |= FLT_REMOTE_ESD;
  // 3. a scripted fill (1 MPa/s, ~1 %/s SOC)
  const float dt = 0.01f;
  if (s.remoteEsd && s.st != SimController::ESD) { s.st = SimController::ESD; s.phase = 4; }
  switch (s.st) {
    case SimController::IDLE:
      if (localStart && !injectFault && s.soc < 0.97f) { s.st = SimController::FILLING; s.phase = 1; s.stateMs = now; }
      break;
    case SimController::FILLING:
      if (injectFault) { s.st = SimController::STOPPING; s.phase = 4; s.stateMs = now; break; }
      if (s.phase == 1 && now - s.stateMs > 3000) s.phase = 2;
      if (s.phase == 2) { s.p += 1.0f * dt; s.soc += 0.0128f * dt; s.t += (46.0f - s.t) * 0.03f * dt; }
      if (s.soc >= 0.97f) { s.phase = 3; s.st = SimController::STOPPING; s.stateMs = now; }
      break;
    case SimController::STOPPING:
      if (now - s.stateMs > 1000) s.st = SimController::IDLE;
      break;
    case SimController::ESD:
      if (localReset) { s.remoteEsd = false; s.st = SimController::IDLE; }
      break;
  }
}
