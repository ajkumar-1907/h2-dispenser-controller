#!/usr/bin/env python3
"""The same header lives in several folders, because Wokwi (and the Arduino IDE)
only compile files that sit next to sketch.ino. This script makes sure the copies
never drift apart: the firmware/ folders are the source of truth.
   python3 tools/sync_check.py      (exit code 1 on any mismatch)"""
import pathlib, sys
ROOT = pathlib.Path(__file__).resolve().parent.parent
FW = ROOT / "firmware"
checks = [
    # the protocol and the fault list MUST be identical on both ends of the cable
    (FW / "stm32_controller/link.h",   FW / "esp32_hmi/link.h"),
    (FW / "stm32_controller/faults.h", FW / "esp32_hmi/faults.h"),
]
# the last tutorial steps ARE the firmware
for f in (FW / "stm32_controller").iterdir():
    checks.append((f, ROOT / "tutorial/step5b_hmi_link" / f.name))
for f in (FW / "esp32_hmi").iterdir():
    checks.append((f, ROOT / "tutorial/step6_esp32_hmi" / f.name))
bad = 0
for a, b in checks:
    ok = b.exists() and a.read_bytes() == b.read_bytes()
    bad += not ok
    print(f"{'OK  ' if ok else 'DIFF'} {a.relative_to(ROOT)}  ==  {b.relative_to(ROOT)}")
print(f"\n{len(checks) - bad}/{len(checks)} copies in sync")
sys.exit(1 if bad else 0)
