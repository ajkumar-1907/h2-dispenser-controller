#!/usr/bin/env bash
# Compile every sketch in the repo with arduino-cli.
# Wokwi wants the main file to be called sketch.ino, arduino-cli wants it to
# match the folder name, so each sketch is copied into a temp folder "sketch/".
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CLI="${ARDUINO_CLI:-arduino-cli}"
STM32_FQBN="STMicroelectronics:stm32:Nucleo_64:pnum=NUCLEO_C031C6"
ESP32_FQBN="esp32:esp32:esp32"
EXTRA=()
# offline/minimal installs may lack the builtin ctags tool: point CTAGS_DIR at one
[ -n "${CTAGS_DIR:-}" ] && EXTRA=(--build-property "runtime.tools.ctags.path=$CTAGS_DIR")
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
fail=0
printf "%-40s %-8s %s\n" "sketch" "result" "flash used"
for dir in "$ROOT"/tutorial/step* "$ROOT"/firmware/*; do
  [ -f "$dir/sketch.ino" ] || continue
  name="${dir#$ROOT/}"
  rm -rf "$TMP/sketch"; mkdir "$TMP/sketch"; cp "$dir"/*.ino "$dir"/*.h "$TMP/sketch/" 2>/dev/null || true
  if grep -q "esp32" <<<"$name"; then fqbn="$ESP32_FQBN"; else fqbn="$STM32_FQBN"; fi
  if out=$("$CLI" compile "${EXTRA[@]}" --fqbn "$fqbn" "$TMP/sketch" 2>&1); then
    used=$(grep -o "Sketch uses [0-9]* bytes ([0-9]*%)" <<<"$out" | sed 's/Sketch uses //')
    printf "%-40s %-8s %s\n" "$name" "OK" "$used"
  else
    printf "%-40s %-8s\n" "$name" "FAIL"; echo "$out" | grep -E "error" | head -5; fail=1
  fi
done
exit $fail
