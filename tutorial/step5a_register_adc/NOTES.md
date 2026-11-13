# Step 5a - register-level ADC driver (flash was at 95 %)

▶ **Run it in Wokwi:** [https://wokwi.com/projects/480593222624966657](https://wokwi.com/projects/480593222624966657) · files in this folder: `sketch.ino`, `diagram.json`, headers

## Why this step
After Step 4 the C031C6 (32 KB flash) was **95 % full**. Instead of guessing, the symbol table was checked: the HAL-based `analogRead()` path was ≈ 2 KB. Writing the ADC driver directly on the registers also shows what the HAL hides - which is exactly the "debug tooling / low-level access" point of the controller evaluation.

## What you build
[`hw_adc.h`](hw_adc.h), from the STM32C0 reference manual:

1. `RCC->APBENR2 |= ADCEN`, analog mode in `GPIOA->MODER`
2. clock mode `CKMODE`, enable the internal regulator `ADVREGEN`, wait 20 µs
3. calibrate `ADCAL`, wait until it clears
4. `ADEN`, wait for `ADRDY`
5. per read: `CHSELR`, wait `CCRDY`, `ADSTART`, wait `EOC`, read `DR`

**Every wait has a timeout** and returns `0xFFFF` on failure. The sensor layer turns 0xFFFF into > 21 mA = **FAULT-SHORT**: a dead ADC becomes a *visible sensor fault*, never a frozen loop.

## Try it
Same tests as Step 4 - behaviour must be identical. Flash: **95 % → 85 %**.

## What I learned / bugs found
**Bug #14**: first idea was "print integers instead of floats to save flash" - measured, it saved nothing here (float printing was already linked by other code). Measure before optimising.
**Bug #13** was also found here (the RESET-refused message regression).

---
[← all steps](../README.md) · [main README](../../README.md)
