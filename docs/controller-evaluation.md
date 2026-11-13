# Controller evaluation: ESP32 vs STM32 for a hydrogen dispenser

> Technical report, in the form of the evaluation I did during my internship - rewritten from scratch for this public project and backed by measurements from the simulated build in this repository. No Hynics documents or data are used.

## 1. Question

A hydrogen refueling station needs a controller that reads the dispenser's sensors, runs the fill, and shuts the station down safely when something goes wrong. It also needs an operator display and, ideally, remote monitoring. **Should one microcontroller family do both jobs, and if not, which one does which?**

## 2. What the primary controller must guarantee

| Requirement | Why it matters on a 70 MPa dispenser |
|---|---|
| Bounded reaction time to an E-stop / gas alarm | valves must close in a known, short time - not "usually fast" |
| Precise, trustworthy analog inputs | every pressure/temperature/gas value arrives as 4-20 mA |
| Deterministic sampling | rate-of-change and leak checks assume a fixed sample period |
| Fieldbus to other station equipment | compressor, chiller, storage panel are typically on CAN/FDCAN or Modbus |
| Debuggability | you must be able to stop the core, inspect registers and trace a fault |
| Fail-safe when the software dies | watchdog, safe output states |
| Freedom from non-safety workload | Wi-Fi stacks, displays, logging must not delay the control loop |

## 3. Comparison by peripheral

| Area | STM32 (Cortex-M) | ESP32 (Xtensa / RISC-V, Wi-Fi) | Matters for |
|---|---|---|---|
| **Timers** | rich general-purpose + advanced timers, can trigger the ADC in hardware | general timers available; fewer hardware trigger links | fixed-rate sampling |
| **Interrupts** | NVIC: nested, prioritised, short documented latency; EXTI on any GPIO | interrupts shared with the Wi-Fi/BT stack and FreeRTOS on the same chip; latency varies with radio activity | E-stop path |
| **DMA** | ADC → memory by DMA with circular buffers | available, mostly used for SPI/I2S | continuous sampling without CPU |
| **ADC** | 12-bit SAR, factory + self calibration (`ADCAL`), good linearity | 12-bit, known non-linearity near the rails; on the original ESP32, ADC2 cannot be used while Wi-Fi is on | 4-20 mA accuracy |
| **UART / SPI / I²C** | many instances, all standard | all present | HMI link, displays, expanders |
| **CAN / FDCAN** | FDCAN on many families (e.g. STM32G0B/G4/H7) | TWAI = classic CAN 2.0, **no CAN FD** | station fieldbus |
| **Debug tooling** | SWD on every part, ST-LINK on every Nucleo board, register view, breakpoints, SWO trace | JTAG via external probe (built-in USB-JTAG on newer S3/C3/C6) | finding faults on the bench |
| **Wireless** | none on most parts | **Wi-Fi + Bluetooth built in** | remote monitoring, telemetry |

## 4. Evidence from this project

The simulated build was split exactly along the recommendation below, so each claim could be tried:

| Claim | What the build showed |
|---|---|
| Interrupt-driven safety path | E-stop on EXTI closes the valves **inside the ISR**; the main loop logged it **4.45 ms** later ([Step 3b](../tutorial/step3b_estop_interrupt/NOTES.md)). Safety did not wait for the loop. |
| Concurrency needs care even on one core | the loop could re-open valves the ISR had just closed - fixed with a critical section ([bug #10](debugging-log.md#10)) |
| Low-level ADC access + every wait time-limited | register-level ADC driver from the reference manual; a dead ADC returns `0xFFFF` → reported as FAULT-SHORT, never a hang ([Step 5a](../tutorial/step5a_register_adc/NOTES.md)) |
| Resource limits are real | the 32 KB C031C6 hit **95 % flash**; symbol analysis found ≈ 2 KB in the HAL ADC path → **85 %** ([bug #14](debugging-log.md#14)) |
| HMI must not be part of the safety chain | link loss for 5 s raises **HMI_LINK = WARN only**; the fill continues and all shutdown logic stays on the controller. Remote START/RESET are refused (`LOCAL_ONLY`) |
| ESP32 is good at the operator side | LCD panel, three pages, remote STOP/ESD, link diagnostics, plausibility of received values ([Step 6](../tutorial/step6_esp32_hmi/NOTES.md)) |
| ESP32 platform quirks | `LINE_MAX` (C-library macro) and `link` (POSIX function) clashed with project names - the ESP32 toolchain carries a full OS environment ([bug log](debugging-log.md#portability)) |

## 5. Recommendation

**STM32 as the primary dispenser controller, ESP32 for wireless monitoring and the operator interface.**

- The STM32 owns everything that can open or close a valve: sensor acquisition, plausibility, cause & effect, fueling sequence, E-stop. No radio stack shares its CPU.
- The ESP32 owns the operator panel and telemetry. It may ask the controller to be **safer** (STOP, ESD) but can never make it **less safe** (START and RESET stay at the dispenser).
- They talk over a checked, request/response serial link with timeouts on both ends, so a dead or confused HMI is detected and reported, not trusted.
- For a production station, a larger STM32 with FDCAN and more flash (e.g. G4 / H7 class) would replace the C031C6 used here; the C031C6 was chosen because Wokwi simulates it and its tight flash made the resource trade-offs visible.

## 6. Limits of this evaluation

Everything was measured in simulation (Wokwi) and on a PC model, not on station hardware. Interrupt latency in Wokwi is indicative only. No functional-safety (IEC 61508 / SIL) assessment is implied: a real station adds a hard-wired, independent safety system that does not rely on any microcontroller.
