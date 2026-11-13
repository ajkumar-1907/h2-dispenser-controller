// scenarios.cpp - runs whole fills on the PC model and writes one CSV per scenario
// (10 Hz samples). docs/plots are made from these by tools/make_plots.py.
//   make -C tests csv
#include <cstdio>
#include <string>
#include "station_sim.h"

struct Csv {
  FILE* f;
  explicit Csv(const std::string& path) {
    f = fopen(path.c_str(), "w");
    fprintf(f, "t_s,state,phase,p_true_mpa,p_meas_mpa,p_target_mpa,valve_u,tank_c,tt1_c,soc_pct,faults_hex\n");
  }
  void row(const Station& s, uint32_t t0) {
    fprintf(f, "%.2f,%s,%s,%.2f,%.2f,%.2f,%.3f,%.2f,%.2f,%.2f,%05X\n", (s.now - t0) / 1000.0,
            dispStateName(s.safety.state), fuelPhaseName(s.fuel.phase), s.tank.P, s.pair.value,
            s.fuel.phase == FP_MAIN_FILL ? s.fuel.pTarget : 0.0f, s.fuel.u, s.tank.T - 273.15f,
            s.s[2].value, tankSOC(s.tank) * 100, (unsigned)s.faults);
  }
  ~Csv() { fclose(f); }
};

// run until the fill is over (+2 s), calling 'inject' every tick
template <class F> void runFill(const std::string& path, Station& st, F inject) {
  Csv csv(path);
  st.powerUp(); st.step(true);
  uint32_t t0 = st.now, endAt = 0;
  while (st.now - t0 < 120000) {
    inject(st, st.now - t0);
    st.step();
    if ((st.now - t0) % 100 == 0) csv.row(st, t0);
    if (!endAt && st.safety.state != DS_FILLING && st.safety.state != DS_STOPPING) endAt = st.now;
    if (endAt && st.now - endAt > 2000) break;
  }
  printf("%-28s end: %-8s SOC %5.1f %%  t=%5.1f s  latched=0x%05X\n", path.c_str(),
         dispStateName(st.safety.state), st.soc() * 100, (st.now - t0) / 1000.0, (unsigned)st.safety.latched);
}

int main(int argc, char** argv) {
  std::string dir = argc > 1 ? argv[1] : ".";
  { Station st; runFill(dir + "/normal_fill.csv", st, [](Station&, uint32_t) {}); }
  { Station st; runFill(dir + "/precool_failure.csv", st, [](Station& s, uint32_t t) {
      if (t > 5000 && s.tt1C < 25.0f) s.tt1C += 0.03f; }); }               // 3 C/s from t = 5 s
  { Station st; runFill(dir + "/frozen_sensors.csv", st, [](Station& s, uint32_t t) {
      if (t >= 15000) s.ptMode = PT_BOTH_STUCK; }); }
  { Station st; st.leak = true; runFill(dir + "/hose_leak.csv", st, [](Station&, uint32_t) {}); }
  { Station st; runFill(dir + "/pt2_drift.csv", st, [](Station& s, uint32_t t) {
      if (t >= 15000 && s.pt2Ofs < 8.0f) s.pt2Ofs += 0.005f; }); }   // slow 0.5 MPa/s drift
  return 0;
}
