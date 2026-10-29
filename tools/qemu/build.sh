#!/usr/bin/env bash
# Build the QEMU smoke-test firmware into build_qemu/ and make the flash image.
# Needs an exported ESP-IDF environment (idf.py on PATH).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
B="$ROOT/build_qemu"
cd "$ROOT"
DEFAULTS=(sdkconfig.defaults tools/qemu/sdkconfig.qemu)
ARGS=(-B "$B" -DSDKCONFIG="$B/sdkconfig" -DSDKCONFIG_DEFAULTS="$(IFS=';'; echo "${DEFAULTS[*]}")")
# An existing sdkconfig keeps its values over changed defaults (a stored "is not set" wins),
# so regenerate it when the defaults change, and only then: a new sdkconfig rebuilds everything.
STAMP="$B/sdkconfig.defaults.sha256"
sum="$(cat "${DEFAULTS[@]}" | sha256sum | cut -d' ' -f1)"
if [ -f "$B/sdkconfig" ] && [ "$(cat "$STAMP" 2>/dev/null)" != "$sum" ]; then
    echo "sdkconfig defaults changed: regenerating $B/sdkconfig"
    rm -f "$B/sdkconfig"
fi
[ -f "$B/sdkconfig" ] || idf.py "${ARGS[@]}" set-target esp32s3
echo "$sum" > "$STAMP"
idf.py "${ARGS[@]}" build
cd "$B"
# The merged image is 16 MB: skip writing it when no input (flash_args and the binaries it
# lists, e.g. "0x20000 sipwire.bin") is newer than it.
stale=0
[ -f qemu_flash.bin ] || stale=1
for f in flash_args $(awk '!/^--/ && NF == 2 {print $2}' flash_args); do
    [ -f "$f" ] && [ ! "$f" -nt qemu_flash.bin ] || stale=1
done
if [ "$stale" = 1 ]; then
    python -m esptool --chip esp32s3 merge-bin --fill-flash-size 16MB -o qemu_flash.bin @flash_args
else
    echo "qemu_flash.bin is up to date"
fi
echo "QEMU image: $B/qemu_flash.bin"
