# Step 1c - 3-sample confirmation + low-pass filter

▶ **Run it in Wokwi:** [https://wokwi.com/projects/480583160625925121](https://wokwi.com/projects/480583160625925121) · files in this folder: `sketch.ino`, `diagram.json`

## Why this step
Real 4-20 mA signals are noisy (EMI from pumps and compressors, contact bounce). A single bad sample must not trip a station - but a real fault must still be seen within milliseconds. Two different tools solve two different problems.

## What you build
- **Status**: a new status must be seen **3 samples in a row** (30 ms) before it is accepted → kills single-sample spikes.
- **Value**: exponential low-pass `y += 0.3 × (x − y)` → smooth number for the display/control.
- The status is classified from the **raw** current, **not** the filtered one.

## Try it
| Do | See |
|---|---|
| wiggle the knob near 3.6 mA | status no longer flickers |
| snap the knob from 20 mA to 0 | FAULT-OPEN after exactly 3 samples, while the filtered value is still sliding down |

## What I learned / bugs found
**Bug #1 / #2 (see [debugging log](../../docs/debugging-log.md))**: first version filtered the current and classified the *filtered* value. Result: a cut cable took ~150 ms to be detected, and on the way down the filter "invented" values (12 → 8 → 5 mA) that the cable never carried. Fix: classify raw, filter only what is shown.

---
[← all steps](../README.md) · [main README](../../README.md)
