# Step 3a - fault register + cause & effect matrix + valves

▶ **Run it in Wokwi:** [https://wokwi.com/projects/480585616207179777](https://wokwi.com/projects/480585616207179777) · files in this folder: `sketch.ino`, `diagram.json`, headers

## Why this step
This is the step where *measurement* turns into *action*. In a real station that mapping is written down as a **cause & effect matrix** (every cause → which shutdown), reviewed by safety people, and implemented exactly. Here it is one function, `faultAction()`.

## What you build
- [`faults.h`](faults.h): a **32-bit fault register**, one bit per fault, so several faults are visible at once.
- Each bit maps to an action: `NONE < WARN < INHIBIT < STOP < ESD`; `worstAction()` takes the most severe.
- [`safety.h`](safety.h): states `IDLE → FILLING → STOPPING → STOPPED / ESD`.
- Valves are **fail-safe**: XV1 (supply, PC7) and XV2 (dispenser, PA8) are normally-closed (power off = closed); XV3 vent (PB0) is normally-open (power off = vent the hose).
- **Sequenced stop**: XV2 closes first, XV1 one second later. **ESD**: all closed at once + vent.
- STOP/ESD **latch**; RESET is refused while the cause is still there.
- Limits with **hysteresis** (over-pressure trips at 87.5 MPa, clears below 85).

## Try it
| Do | See |
|---|---|
| START (PB5) | FILLING, XV1+XV2 LEDs on |
| STOP (PB3) | XV2 off, XV1 off 1 s later |
| GD1 knob > 25 %LEL | ESD: everything closed, vent LED on |
| RESET with gas still high | refused, reason printed |

## What I learned / bugs found
**Bug #5**: Step 2 kept one `state` variable - a second fault could hide behind the first. The register fixes it.
**Bug #6**: RESET was not logged and repeated while the button was held → edge detection + log line.
**Bug #9**: over-pressure chattered on/off around 87.5 MPa → hysteresis.

---
[← all steps](../README.md) · [main README](../../README.md)
