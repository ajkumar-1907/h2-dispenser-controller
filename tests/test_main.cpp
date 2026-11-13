// Host unit + regression tests for the H2 dispenser controller.
// Build and run:  make -C tests      (needs only g++)
//
// Each "BUG #n" test re-creates a defect found during the Wokwi runs
// (see docs/debugging-log.md) and proves the fix still holds.
#include <cstdio>
#include <cstring>
#include <cmath>
#include "station_sim.h"
#include "../firmware/stm32_controller/link.h"
#include "../firmware/esp32_hmi/sim_controller.h"

static int g_pass = 0, g_fail = 0;
static const char* g_test = "";
#define CHECK(cond) do { if (cond) g_pass++; else { g_fail++; \
  printf("  FAIL [%s] line %d: %s\n", g_test, __LINE__, #cond); } } while (0)
#define TEST(name) static void name(); struct name##_r { name##_r() { reg(#name, name); } } name##_i; static void name()

typedef void (*Fn)();
static struct { const char* n; Fn f; } g_tests[128]; static int g_n = 0;
static void reg(const char* n, Fn f) { g_tests[g_n].n = n; g_tests[g_n].f = f; g_n++; }

static Sensor mkSensor(float lo = 0, float hi = 100) { Sensor s{}; s.tag = "T"; s.lo = lo; s.hi = hi; return s; }
static void feed(Sensor& s, float ma, int n) { for (int i = 0; i < n; i++) sensorUpdate(s, maToCounts(ma)); }

// =============================== 1. 4-20 mA sensor ===============================
TEST(ne43_bands) {
  CHECK(classify(2.0f)  == SNS_FAULT_OPEN);
  CHECK(classify(3.59f) == SNS_FAULT_OPEN);
  CHECK(classify(3.7f)  == SNS_UNDER);
  CHECK(classify(4.0f)  == SNS_OK);
  CHECK(classify(20.0f) == SNS_OK);
  CHECK(classify(20.7f) == SNS_OVER);
  CHECK(classify(21.5f) == SNS_FAULT_SHORT);
}
TEST(scaling_is_clamped) {
  CHECK(std::fabs(maToEng(12.0f, 0, 100) - 50.0f) < 1e-4);
  CHECK(maToEng(2.0f, 0, 100) == 0.0f);          // never below range
  CHECK(maToEng(22.0f, 0, 100) == 100.0f);       // never above range
  CHECK(std::fabs(maToEng(4.0f, -50, 100) + 50.0f) < 1e-4);
}
TEST(bug3_sensor_starts_untrusted) {               // SNS_INIT: fail-safe start
  Sensor s = mkSensor();
  CHECK(s.status == SNS_INIT && !usable(s));
  feed(s, 12.0f, 2); CHECK(!usable(s));             // not yet confirmed
  feed(s, 12.0f, 1); CHECK(s.status == SNS_OK);     // 3rd identical sample
}
TEST(bug1_single_noise_sample_is_ignored) {        // 3-sample confirmation
  Sensor s = mkSensor(); feed(s, 12.0f, 5);
  feed(s, 2.0f, 1); feed(s, 12.0f, 1);              // one glitch
  CHECK(s.status == SNS_OK && s.faultEvents == 0);
  feed(s, 2.0f, 3);
  CHECK(s.status == SNS_FAULT_OPEN && s.faultEvents == 1);
}
TEST(bug2_filter_does_not_delay_fault) {           // status comes from the RAW current
  Sensor s = mkSensor(); feed(s, 20.0f, 50);
  feed(s, 0.0f, 3);                                 // cable cut
  CHECK(s.status == SNS_FAULT_OPEN);                // detected after exactly 3 samples
  CHECK(s.value > 10.0f);                           // ...while the filtered value still lags (~18 MPa)
}

// =============================== 2. plausibility ===============================
struct PairFix {
  Sensor a = mkSensor(), b = mkSensor();
  SensorPair p{ "P", &a, &b, 2.0f, 500 };
  void set(float va, float vb, uint32_t& t, int n = 1) {
    for (int i = 0; i < n; i++) {
      sensorUpdate(a, engToCounts(va, 0, 100)); sensorUpdate(b, engToCounts(vb, 0, 100));
      t += 10; pairUpdate(p, t);
    }
  }
};
TEST(bug4_pair_detects_disagreement) {
  PairFix f; uint32_t t = 0;
  f.set(30, 30, t, 10); CHECK(f.p.state == PAIR_OK);
  f.set(30, 80, t, 30); CHECK(f.p.state == PAIR_OK);          // < 500 ms: not yet
  f.set(30, 80, t, 25); CHECK(f.p.state == PAIR_DISAGREE);    // > 500 ms
  CHECK(f.p.value > 79.0f);                                   // uses the HIGHER reading
  f.set(30, 30, t, 10); CHECK(f.p.state == PAIR_DISAGREE);    // latched
}
TEST(pair_degraded_and_lost) {
  PairFix f; uint32_t t = 0;
  f.set(40, 40, t, 10);
  for (int i = 0; i < 5; i++) { sensorUpdate(f.a, 0); sensorUpdate(f.b, engToCounts(40, 0, 100)); t += 10; pairUpdate(f.p, t); }
  CHECK(f.p.state == PAIR_DEGRADED && std::fabs(f.p.value - 40) < 0.5f);
  for (int i = 0; i < 5; i++) { sensorUpdate(f.a, 0); sensorUpdate(f.b, 0); t += 10; pairUpdate(f.p, t); }
  CHECK(f.p.state == PAIR_LOST);
}
TEST(bug7_pair_reset_goes_direct_never_init) {
  PairFix f; uint32_t t = 0;
  f.set(30, 30, t, 10); f.set(30, 80, t, 60);
  CHECK(f.p.state == PAIR_DISAGREE);
  CHECK(!pairReset(f.p));                                     // still disagreeing: refused
  f.set(30, 30, t, 40);                                       // filters settle, agree again
  CHECK(pairReset(f.p));
  CHECK(f.p.state == PAIR_OK);                                // straight to OK, not INIT
}
TEST(rate_check_trips_on_jump_not_on_ramp) {
  Sensor s = mkSensor(); RateCheck r{ &s, 20.0f, 100 }; uint32_t t = 0;
  for (int i = 0; i < 300; i++) { feed(s, 4.0f + 16.0f * (10.0f + i * 0.01f) / 100.0f, 1); t += 10; rateUpdate(r, t); }
  CHECK(!r.fault);                                            // 1 MPa/s ramp is fine
  for (int i = 0; i < 20; i++) { sensorUpdate(s, engToCounts(60, 0, 100)); t += 10; rateUpdate(r, t); }
  CHECK(r.fault && r.trips == 1);                             // a 50 MPa jump is not physics
  CHECK(rateReset(r) && !r.fault);
}
TEST(bug11_next_car_jump_with_rearm_does_not_trip) {
  // without rearm the same jump DOES trip -> the re-arm is what prevents it
  Station raw; raw.powerUp(); raw.tank.m = 4.0f; raw.run(500);
  CHECK(raw.faults & FLT_PT1_RATE);
  Station fixed; fixed.powerUp(); fixed.tank.m = 4.0f; fixed.nextCar(); fixed.run(500);
  CHECK(!(fixed.faults & FLT_PT1_RATE));
}

// =============================== 3. fault register + C&E ===============================
TEST(every_fault_bit_has_name_and_action) {
  for (int b = 0; b < N_FAULT_BITS; b++) {
    CHECK(strcmp(faultName(b), "?") != 0);
    CHECK(faultAction(1u << b) != ACT_NONE);
  }
  CHECK(strcmp(faultName(N_FAULT_BITS), "?") == 0);
}
TEST(bug5_register_keeps_second_fault_visible) {
  uint32_t f = FLT_GAS_WARN | FLT_P_LOST;
  CHECK(worstAction(f) == ACT_ESD);
  CHECK(worstAction(FLT_GAS_WARN) == ACT_INHIBIT);
  CHECK(worstAction(FLT_HMI_LINK) == ACT_WARN);               // safety never depends on the HMI
  CHECK(worstAction(0) == ACT_NONE);
}

// =============================== 4. safety state machine ===============================
TEST(sequenced_stop_closes_xv2_first) {
  Safety s; safetyInit(s);
  safetyUpdate(s, 0, true, false, false, 0);   CHECK(s.state == DS_FILLING);
  CHECK(s.out.xv1Supply && s.out.xv2Dispense);
  safetyUpdate(s, 0, false, true, false, 100); CHECK(s.state == DS_STOPPING);
  CHECK(s.out.xv1Supply && !s.out.xv2Dispense);               // XV2 closed, XV1 still open
  safetyUpdate(s, 0, false, false, false, 1050); CHECK(s.state == DS_STOPPING);
  safetyUpdate(s, 0, false, false, false, 1100); CHECK(s.state == DS_IDLE);
  CHECK(!s.out.xv1Supply && !s.out.xv2Dispense && s.out.xv3VentClosed);
}
TEST(esd_from_any_state_vents) {
  for (int start = 0; start < 2; start++) {
    Safety s; safetyInit(s);
    if (start) safetyUpdate(s, 0, true, false, false, 0);
    safetyUpdate(s, FLT_GAS_ALARM, false, false, false, 10);
    CHECK(s.state == DS_ESD);
    CHECK(!s.out.xv1Supply && !s.out.xv2Dispense && !s.out.xv3VentClosed);
  }
}
TEST(inhibit_blocks_start_but_not_running_fill) {
  Safety s; safetyInit(s);
  safetyUpdate(s, FLT_GAS_WARN, true, false, false, 0); CHECK(s.state == DS_IDLE);
  safetyUpdate(s, 0, true, false, false, 10);           CHECK(s.state == DS_FILLING);
  safetyUpdate(s, FLT_PRECOOL, false, false, false, 20); CHECK(s.state == DS_FILLING);
}
TEST(bug8_reset_refused_while_cause_active_and_latched_after) {
  Safety s; safetyInit(s);
  safetyUpdate(s, FLT_ESTOP, false, false, false, 0);   CHECK(s.state == DS_ESD);
  safetyUpdate(s, FLT_ESTOP, false, false, true, 10);   CHECK(s.state == DS_ESD);   // refused
  safetyUpdate(s, 0, true, false, false, 20);           CHECK(s.state == DS_ESD);   // cause gone: still LATCHED
  safetyUpdate(s, 0, false, false, true, 30);           CHECK(s.state == DS_IDLE);  // reset accepted
  CHECK(s.latched == 0);
}
TEST(bug9_overpressure_hysteresis_no_chatter) {
  bool a = false;
  CHECK(!hyst(a, 87.4f, 87.5f, 85.0f));
  CHECK(hyst(a, 87.5f, 87.5f, 85.0f));
  CHECK(hyst(a, 86.0f, 87.5f, 85.0f));     // noise just below trip: stays active
  CHECK(hyst(a, 85.0f, 87.5f, 85.0f));
  CHECK(!hyst(a, 84.9f, 87.5f, 85.0f));
}

// =============================== 5. whole-fill scenarios ===============================
static uint32_t fillUntilEnd(Station& st, uint32_t maxMs, float* tMax = nullptr) {
  st.powerUp();                    // sensors confirm, pair leaves INIT
  st.step(true);                   // START
  uint32_t t0 = st.now;
  float tm = 0;
  while (st.now - t0 < maxMs && (st.safety.state == DS_FILLING || st.safety.state == DS_STOPPING)) {
    st.step(); if (st.tankC() > tm) tm = st.tankC();
  }
  if (tMax) *tMax = tm;
  return st.now - t0;
}
TEST(scenario_normal_fill_completes) {
  Station st; float tMax;
  uint32_t ms = fillUntilEnd(st, 200000, &tMax);
  CHECK(st.safety.state == DS_IDLE);
  CHECK(st.soc() >= 0.97f);
  CHECK(tMax < 85.0f);
  CHECK(worstAction(st.faults) <= ACT_INHIBIT);
  CHECK(ms > 40000 && ms < 90000);
  printf("    normal fill: %.1f s, SOC %.1f %%, Ttank max %.1f C\n", ms / 1000.0, st.soc() * 100, tMax);
}
TEST(scenario_precool_failure_stops_on_tank_overtemp) {
  // pre-cooler fails 5 s into the fill: gas warms 3 C/s (below the 5 C/s rate limit,
  // so only the PROCESS limit can catch it). PRECOOL is only INHIBIT: it blocks the
  // NEXT start, it does not stop a running fill - the tank temperature does.
  Station st; st.powerUp(); st.step(true); st.run(5000);
  while (st.safety.state == DS_FILLING) { if (st.tt1C < 25.0f) st.tt1C += 0.03f; st.step(); }
  st.run(1500);
  CHECK(st.safety.state == DS_STOPPED);
  CHECK(st.safety.latched & FLT_TANK_OVERTEMP);
  CHECK(!(st.safety.latched & FLT_TT1_RATE));
  CHECK(st.soc() < 0.97f);
  printf("    precool failure: STOPPED at SOC %.0f %%, latched 0x%05X\n", st.soc() * 100, (unsigned)st.safety.latched);
}
TEST(scenario_hose_leak_found_in_leak_check_esd) {
  Station st; st.leak = true; st.powerUp(); st.step(true);
  st.run(4000);
  CHECK(st.safety.state == DS_ESD);
  CHECK(st.safety.latched & FLT_LEAK);
  CHECK(!st.safety.out.xv3VentClosed);                         // hose vented
  CHECK(st.fuel.phase == FP_ABORTED);
}
TEST(scenario_both_sensors_frozen_caught_by_no_rise) {
  Station st; st.powerUp(); st.step(true); st.run(8000);       // into the ramp
  st.ptMode = PT_BOTH_STUCK;
  float socAtFreeze = st.soc();
  for (int i = 0; i < 3000 && st.safety.state == DS_FILLING; i++) st.step();
  CHECK(st.safety.state != DS_FILLING);
  st.run(1500);
  CHECK(st.safety.latched & FLT_NO_RISE);
  CHECK(st.pair.state == PAIR_OK);                             // redundancy alone could NOT see it
  printf("    frozen sensors: true P %.1f vs shown %.1f MPa, SOC %.0f -> %.0f %%\n",
         st.tank.P, st.pair.value, socAtFreeze * 100, st.soc() * 100);
}
TEST(scenario_pt1_cable_cut_stops_on_degraded) {
  Station st; st.powerUp(); st.step(true); st.run(6000);
  st.ptMode = PT1_CUT; st.step();
  // the RATE check sees the jump to 0 mA on the first sample, 20 ms before the
  // NE43 classifier has its 3 confirmations: two independent checks, the faster wins
  CHECK(st.faults & FLT_PT1_RATE);
  st.run(2000);
  CHECK(st.safety.state == DS_STOPPED);
  CHECK(st.faults & FLT_P_DEGRADED);
  CHECK(st.faults & FLT_PT1_WIRING);
}
TEST(scenario_gas_alarm_esd_and_reset_flow) {
  Station st; st.powerUp(); st.step(true); st.run(5000);
  st.gasLel = 30; st.run(200);
  CHECK(st.safety.state == DS_ESD);
  st.step(false, false, true); CHECK(st.safety.state == DS_ESD);   // gas still there: refused
  st.gasLel = 0; st.run(200);
  st.step(false, false, true); st.run(50); CHECK(st.safety.state == DS_IDLE);
}
TEST(scenario_pt1_stuck_disagrees_when_pressure_moves) {
  Station st; st.powerUp(); st.step(true); st.run(5000);
  st.ptMode = PT1_STUCK; st.run(6000);
  CHECK(st.safety.state == DS_STOPPED);
  CHECK(st.safety.latched & FLT_P_DISAGREE);
}
TEST(scenario_gas_warn_inhibits_start) {
  Station st; st.gasLel = 15; st.powerUp(); st.step(true); st.run(100);
  CHECK(st.safety.state == DS_IDLE && (st.faults & FLT_GAS_WARN));
}
TEST(scenario_gas_detector_cable_cut_is_esd) {
  Station st; st.powerUp(); st.step(true); st.run(5000);
  st.gasLel = -10;   // below 4 mA range -> counts for ~2.4 mA = FAULT-OPEN
  st.run(100);
  CHECK(st.faults & FLT_GD1_WIRING);
  CHECK(st.safety.state == DS_ESD);
}
TEST(scenario_estop) {
  Station st; st.powerUp(); st.step(true); st.run(5000);
  st.estop = true; st.step();
  CHECK(st.safety.state == DS_ESD);
}
TEST(scenario_precool_too_warm_inhibits_start) {
  Station st; st.tt1C = -20; st.powerUp(); st.step(true); st.run(100);
  CHECK(st.safety.state == DS_IDLE);
  CHECK(st.faults & FLT_PRECOOL);
}

// =============================== 6. HMI link protocol ===============================
static FrameResult rxLine(const char* s, char** bodyOut = nullptr) {
  LineRx r{}; char* body = nullptr; FrameResult fr = FR_NONE;
  for (const char* p = s; *p; p++) lineFeed(r, *p, body);
  fr = lineFeed(r, '\n', body);
  if (bodyOut) *bodyOut = body;
  return fr;
}
static const char* frame(const char* body) { TxFrame t; txBegin(t); txStr(t, body); return txEnd(t); }

TEST(link_known_checksums) {
  CHECK(!strcmp(frame("HB"), "$HB*0A"));
  CHECK(!strcmp(frame("ST"), "$ST*07"));
  CHECK(!strcmp(frame("STOP"), "$STOP*18"));
  CHECK(!strcmp(frame("ESD"), "$ESD*52"));
  CHECK(!strcmp(frame("START"), "$START*40"));
  CHECK(!strcmp(frame("RESET"), "$RESET*55"));
}
TEST(link_error_codes) {
  char* b;
  CHECK(rxLine("$HB*0A", &b) == FR_OK && !strcmp(b, "HB"));
  CHECK(rxLine("$HB*0a") == FR_OK);                  // lower-case hex accepted
  CHECK(rxLine("HB*0A") == FR_NO_DOLLAR);
  CHECK(rxLine("$HB") == FR_NO_STAR);
  CHECK(rxLine("$HB*0") == FR_NO_STAR);
  CHECK(rxLine("$HB*0B") == FR_BAD_CS);              // one bit flipped
  CHECK(rxLine("$HB*ZZ") == FR_BAD_CS);
  LineRx r{}; char* body; CHECK(lineFeed(r, '\r', body) == FR_NONE);   // empty line ignored
}
TEST(link_overflow_never_parses_truncated_frame) {
  char big[100]; memset(big, 'A', 90); big[0] = '$'; big[90] = 0;
  CHECK(rxLine(big) == FR_OVERFLOW);
  LineRx r{}; char* body;                             // receiver recovers on the next line
  for (const char* p = big; *p; p++) lineFeed(r, *p, body);
  lineFeed(r, '\n', body);
  FrameResult fr = FR_NONE; for (const char* p = "$HB*0A\n"; *p; p++) fr = lineFeed(r, *p, body);
  CHECK(fr == FR_OK);
}
TEST(bug16_longest_status_frame_fits) {
  // worst case: longest state + phase names, negative/4-digit numbers, 10-digit counter
  TxFrame t; txBegin(t);
  txStr(t, "ST,"); txStr(t, "STOPPING"); txStr(t, ","); txStr(t, "LEAKCHK");
  txStr(t, ","); txInt(t, 1000); txStr(t, ","); txInt(t, -500); txStr(t, ","); txInt(t, 100);
  txStr(t, ","); txHex(t, 0xFFFFF, 5); txStr(t, ","); txInt(t, 2147483647);
  const char* line = txEnd(t);
  printf("    worst-case frame: %s (%zu chars, limit %d)\n", line, strlen(line), FRAME_MAX);
  CHECK(strlen(line) == 54);
  CHECK(strlen(line) <= FRAME_MAX);
  char* b; CHECK(rxLine(line, &b) == FR_OK);
  // the frame that actually broke the 40-char receiver during LEAKCHK (41 chars)
  CHECK(rxLine("$ST,FILLING,LEAKCHK,145,250,20,00000,0*22") == FR_OK);
}
TEST(link_txint_negative_and_zero) {
  TxFrame t; txBegin(t); txInt(t, -405); txStr(t, ","); txInt(t, 0); t.buf[t.len] = 0;
  CHECK(!strncmp(t.buf, "-405,0", 6));
}

// =============================== 7. ESP32 side: simulated controller ===============================
static const char* ask(SimController& c, Pipe& toC, Pipe& fromC, const char* body, uint32_t now,
                       bool cut = false) {
  static char line[80]; toC.putLine(frame(body));
  simStep(c, toC, fromC, false, cut, false, false, now);
  int n = 0; char ch; while (fromC.get(ch) && ch != '\n' && n < 79) line[n++] = ch; line[n] = 0;
  return line;
}
TEST(sim_controller_same_rules_as_firmware) {
  SimController c; simInit(c); Pipe a{}, b{};
  CHECK(!strcmp(ask(c, a, b, "HB", 10), frame("ACK,HB")));
  CHECK(strstr(ask(c, a, b, "START", 20), "LOCAL_ONLY"));      // remote START refused
  CHECK(strstr(ask(c, a, b, "RESET", 30), "LOCAL_ONLY"));      // remote RESET refused
  CHECK(strstr(ask(c, a, b, "STOP", 40), "NOT_FILLING"));
  CHECK(strstr(ask(c, a, b, "NC", 50), "ACK,NC"));
  simStep(c, a, b, false, false, true, false, 60);             // local START
  CHECK(c.st == SimController::FILLING);
  CHECK(strstr(ask(c, a, b, "NC", 70), "NOT_IDLE"));           // bug #12: next car only in IDLE
  CHECK(strstr(ask(c, a, b, "ESD", 80), "ACK,ESD"));
  simStep(c, a, b, false, false, false, false, 90);
  CHECK(c.st == SimController::ESD && (c.faults & FLT_REMOTE_ESD));
  simStep(c, a, b, false, false, false, true, 100);            // LOCAL reset clears it
  CHECK(c.st == SimController::IDLE);
}
TEST(sim_controller_cut_cable_gives_silence) {
  SimController c; simInit(c); Pipe a{}, b{};
  CHECK(ask(c, a, b, "HB", 10, true)[0] == 0);
}
TEST(sim_controller_bad_frame_is_nakd_and_counted) {
  SimController c; simInit(c); Pipe a{}, b{};
  a.putLine("$HB*FF"); simStep(c, a, b, false, false, false, false, 10);
  char line[40]; int n = 0; char ch; while (b.get(ch) && ch != '\n') line[n++] = ch; line[n] = 0;
  CHECK(strstr(line, "NAK,FRAME,5") != nullptr);
  CHECK(c.framesBad == 1);
}

int main() {
  for (int i = 0; i < g_n; i++) {
    g_test = g_tests[i].n;
    int before = g_fail;
    g_tests[i].f();
    printf("%s %s\n", g_fail == before ? "PASS" : "FAIL", g_test);
  }
  printf("\n%d tests, %d checks passed, %d failed\n", g_n, g_pass, g_fail);
  return g_fail ? 1 : 0;
}
