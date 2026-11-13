# Step 1a - ADC counts → volts → mA → MPa

▶ **Run it in Wokwi:** [https://wokwi.com/projects/480581678802888705](https://wokwi.com/projects/480581678802888705) · files in this folder: `sketch.ino`, `diagram.json`

## Why this step
Every sensor on a hydrogen station (pressure, temperature, gas detection) talks to the controller with a **4-20 mA current loop**. The controller cannot measure current directly, so the loop runs through a **150 Ω shunt resistor** and the ADC measures the voltage across it. Before any safety logic, you have to be sure this chain of units is right - every later step depends on it.

## What you build
One knob on **PA0** plays pressure transmitter PT1. The knob gives 0-3.3 V, which stands for the shunt voltage.

```
counts  = analogRead(PA0)                 0..4095  (12-bit; call analogReadResolution(12)!)
volts   = counts × 3.3 / 4095
mA      = volts / 150 Ω × 1000            150 Ω × 20 mA = 3.0 V  → fits under 3.3 V
MPa     = (mA − 4) × 100 / 16             4 mA = 0 MPa, 20 mA = 100 MPa
```

## Try it
| Knob | Expect |
|---|---|
| fully left | 0 counts, 0 mA, **−25 MPa** (impossible - see below) |
| ~1/3 | ≈ 12 mA, 50 MPa |
| ~0.9 | ≈ 20 mA, 100 MPa |

## What I learned / bugs found
- `analogReadResolution(12)` matters: the Arduino default is 10 bits, so readings are 4× too small without it.
- **The −25 MPa at 0 mA is the whole point of "live zero".** 0 mA is not "0 MPa", it means *the cable is cut*. A 0-10 V sensor can't tell "zero pressure" from "broken wire"; 4-20 mA can. Step 1b turns that into a status.
- `delay(500)` is fine here only because nothing else runs yet. It goes in Step 1b.

---
[← all steps](../README.md) · [main README](../../README.md)
