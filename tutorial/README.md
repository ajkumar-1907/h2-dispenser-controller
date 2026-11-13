# Tutorial: build the controller in 12 steps

Each folder is a **complete, compiling Wokwi project** (`sketch.ino` + `diagram.json` + headers) and a `NOTES.md` that explains *why* the step exists, what to try, and which bugs it found. Every step was run in Wokwi; the logs and screenshots are in [`docs/`](../docs).

To run a step: open its Wokwi link, or create a new **STM32 Nucleo-C031C6** project on [wokwi.com](https://wokwi.com) and paste in the files of the folder (Step 6: **ESP32 DevKit V1**, plus `libraries.txt`).

| # | Step | New concept | Bugs found here | Wokwi |
|---|---|---|---|---|
| 1a | [ADC → mA → MPa](step1a_adc_to_milliamps/NOTES.md) | 4-20 mA loop, shunt, 12-bit ADC | - | [run](https://wokwi.com/projects/480581678802888705) |
| 1b | [NE43 status](step1b_namur_ne43_status/NOTES.md) | live zero, fault bands, `millis()` scheduler | - | [run](https://wokwi.com/projects/480582655817101313) |
| 1c | [Confirmation + filter](step1c_confirmation_filter/NOTES.md) | 3-sample confirm, low-pass on value only | #1, #2 | [run](https://wokwi.com/projects/480583160625925121) |
| 1d | [Four transmitters](step1d_four_transmitters/NOTES.md) | `Sensor` struct, SNS_INIT fail-safe start | #3 | [run](https://wokwi.com/projects/480583501788650497) |
| 2a | [Redundant pressure](step2a_redundant_pressure/NOTES.md) | PT1/PT2 cross-check, latched DISAGREE | #4 | [run](https://wokwi.com/projects/480584227167506433) |
| 2b | [Rate of change](step2b_rate_of_change/NOTES.md) | physics limits, no rate check on gas | - | [run](https://wokwi.com/projects/480585016511683585) |
| 3a | [Cause & effect](step3a_cause_and_effect/NOTES.md) | fault register, C&E matrix, fail-safe valves | #5, #6, #9 | [run](https://wokwi.com/projects/480585616207179777) |
| 3b | [E-stop interrupt](step3b_estop_interrupt/NOTES.md) | EXTI ISR, `volatile`, critical section | #7, #8, #10 | [run](https://wokwi.com/projects/480587120335285249) |
| 4 | [Fueling sequence](step4_fueling_sequence/NOTES.md) | tank model, leak check, PI ramp, NO_RISE | #11, #12, #13 | [run](https://wokwi.com/projects/480589443349184513) |
| 5a | [Register ADC](step5a_register_adc/NOTES.md) | bare-metal ADC driver with timeouts | #14 | [run](https://wokwi.com/projects/480593222624966657) |
| 5b | [HMI link](step5b_hmi_link/NOTES.md) | framed protocol, checksum, link watchdog | #15 | [run](https://wokwi.com/projects/480593603046375425) |
| 6 | [ESP32 HMI](step6_esp32_hmi/NOTES.md) | LCD operator panel, timeouts, remote STOP/ESD | #16, #17 | [run](https://wokwi.com/projects/480597402130418689) |

Bug numbers refer to the [debugging log](../docs/debugging-log.md).

**Flash budget on the C031C6 (32 KB)** - a real constraint that shaped Step 5a:

```
1a ████████████████████▊           65 %
1d ████████████████████████▋       77 %
3b ████████████████████████████▌   89 %
4  ██████████████████████████████▍ 95 %   ← too close
5a ███████████████████████████▏    85 %   ← register-level ADC
5b ████████████████████████████▊   90 %   (+ HMI protocol)
```
