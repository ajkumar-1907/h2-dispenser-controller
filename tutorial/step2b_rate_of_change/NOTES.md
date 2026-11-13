# Step 2b - rate-of-change: can the process really change this fast?

▶ **Run it in Wokwi:** [https://wokwi.com/projects/480585016511683585](https://wokwi.com/projects/480585016511683585) · files in this folder: `sketch.ino`, `diagram.json`, headers

## Why this step
A pipe at 40 MPa cannot be at 90 MPa 100 ms later. If the reading jumps like that, the **signal** is broken (loose terminal, EMI, failing transmitter), not the process. This is a physics-based plausibility check that also works when there is no redundant sensor (TT1).

## What you build
[`rate_check.h`](rate_check.h), measured over 100 ms windows on the **raw** current (the filter would hide jumps):

| Signal | Limit | Reason |
|---|---|---|
| PT1, PT2 | 20 MPa/s | a fill ramps at ~1 MPa/s |
| TT1 | 5 °C/s | gas has thermal mass |
| GD1 | **none** | a fast rise is exactly what a real leak looks like - never "filter out" the gas detector |

Rate faults latch until RESET. `pairReset()` now returns whether it did anything, so RESET can report what it cleared.

## Try it
Snap PT1 from 20 to 80 MPa → `PT1_RATE` latched. Turn it slowly → nothing. Snap GD1 → no rate fault (by design).

## What I learned / bugs found
A *wiring* fault wins over a rate fault: when the sensor becomes unusable the rate check simply restarts, so one cause is not reported twice.

---
[← all steps](../README.md) · [main README](../../README.md)
