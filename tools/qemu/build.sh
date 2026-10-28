#!/usr/bin/env bash
# Build the QEMU smoke-test firmware into build_qemu/ and make the flash image.
# Needs an exported ESP-IDF environment (idf.py on PATH).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
B="$ROOT/build_qemu"
cd "$ROOT"
ARGS=(-B "$B" -DSDKCONFIG="$B/sdkconfig" -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;tools/qemu/sdkconfig.qemu")
[ -f "$B/sdkconfig" ] || idf.py "${ARGS[@]}" set-target esp32s3
idf.py "${ARGS[@]}" build
cd "$B"
python -m esptool --chip esp32s3 merge-bin --fill-flash-size 8MB -o qemu_flash.bin @flash_args
echo "QEMU image: $B/qemu_flash.bin"
