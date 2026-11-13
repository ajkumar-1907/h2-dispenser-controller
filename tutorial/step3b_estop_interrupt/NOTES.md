# Step 3b - E-stop interrupt + the bugs found in 3a

▶ **Run it in Wokwi:** [https://wokwi.com/projects/480587120335285249](https://wokwi.com/projects/480587120335285249) · files in this folder: `sketch.ino`, `diagram.json`, headers

## Why this step
The emergency stop must not wait for the main loop. It is wired **normally-closed** (a cut wire = a pressed E-stop) and handled by an **EXTI interrupt** that drives the valves to the safe state *inside the ISR*. The loop only logs it afterwards. This is the "deterministic response" argument for the STM32 in the [controller evaluation](../../docs/controller-evaluation.md).

## What you build
- PB6 E-stop with `INPUT_PULLUP`, NC contact to GND: healthy = LOW, pressed **or cable cut** = HIGH.
- `estopISR()` writes the outputs directly; `estopTripped` is `volatile`.
- `writeOutputs()` uses `noInterrupts()` / `interrupts()` so the loop can never re-open a valve the ISR just closed.
- Status line now shows PT1 and PT2 separately; START refusals say *why*.

## Try it
Press E-stop during a fill: valves close immediately; the log line appears ~4 ms later (measured **4.45 ms**) - the loop noticed, but the ISR had already acted.

## What I learned / bugs found
**Bug #7** - *RESET caused its own refusal*: `pairReset()` went to `PAIR_INIT` first; for one cycle the station saw "no pressure measurement" = ESD, so the reset immediately re-tripped. Fix: go directly to OK/DEGRADED/LOST.
**Bug #8** - refusal messages printed no reason (or an empty list when the cause had gone but the shutdown was latched). Fix: print reasons; if causes are clear say *"shutdown is LATCHED → press RESET first"*.
**Bug #10** - race: the loop could re-open valves right after the ISR closed them → critical section.

---
[← all steps](../README.md) · [main README](../../README.md)
