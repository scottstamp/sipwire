#!/usr/bin/env bash
# QEMU smoke test: boots the real firmware (CONFIG_SIPWIRE_QEMU) and drives it
# over its serial console against a private Asterisk on the host (5080/udp):
# register, echo call with hold/DTMF/RTP both ways, remote hangup, inbound
# answer/reject, 404, re-register on account change, unregister.
#
#   tools/qemu/build.sh && tools/qemu/smoke.sh [scenario] [log]
#
# Needs: exported ESP-IDF env, qemu-xtensa (idf_tools.py install qemu-xtensa),
# asterisk (Debian/Ubuntu package). Stops only the Asterisk it started.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
AST_CFG="$ROOT/test_host/sip/asterisk"
ASTERISK="${ASTERISK:-/usr/sbin/asterisk}"
SCENARIO="${1:-$HERE/smoke.txt}"
LOG="${2:-$ROOT/build_qemu/smoke.log}"

[ -f "$ROOT/build_qemu/qemu_flash.bin" ] || { echo "run tools/qemu/build.sh first" >&2; exit 2; }
[ -x "$ASTERISK" ] || { echo "asterisk not found at $ASTERISK" >&2; exit 2; }
if [ -z "${QEMU:-}" ]; then
    QEMU="$(command -v qemu-system-xtensa || ls "${IDF_TOOLS_PATH:-$HOME/.espressif}"/tools/qemu-xtensa/*/qemu/bin/qemu-system-xtensa 2>/dev/null | head -1)"
fi
[ -x "$QEMU" ] || { echo "qemu-system-xtensa not found (idf_tools.py install qemu-xtensa)" >&2; exit 2; }
export QEMU

MODDIR=""
for d in /usr/lib/x86_64-linux-gnu/asterisk/modules /usr/lib/aarch64-linux-gnu/asterisk/modules \
         /usr/lib/asterisk/modules /usr/lib64/asterisk/modules; do
    if [ -f "$d/chan_pjsip.so" ]; then MODDIR="$d"; break; fi
done
[ -n "$MODDIR" ] || { echo "Asterisk modules (chan_pjsip.so) not found" >&2; exit 2; }
DATADIR="${ASTERISK_DATADIR:-/usr/share/asterisk}"

STATE="$(mktemp -d /tmp/sipwire-qemu.XXXXXX)"
mkdir -p "$STATE"/{etc,cache,varlib,db,keys,agi,spool,run,log}
sed -e "s|@STATE@|$STATE|g" -e "s|@MODDIR@|$MODDIR|g" -e "s|@DATADIR@|$DATADIR|g" \
    "$AST_CFG/asterisk.conf.in" > "$STATE/etc/asterisk.conf"
cp "$AST_CFG"/{modules,extensions,rtp,logger}.conf "$STATE/etc/"
# Inside slirp the host is 10.0.2.2: advertise that in SDP instead of 127.0.0.1.
sed '/^\[phone\](!)/a media_address = 10.0.2.2' "$AST_CFG/pjsip.conf" > "$STATE/etc/pjsip.conf"
CONF="$STATE/etc/asterisk.conf"

AST_PID=""
cleanup() {
    local rc=$?
    if [ -n "$AST_PID" ] && kill -0 "$AST_PID" 2>/dev/null; then
        kill -TERM "$AST_PID" 2>/dev/null || true
        for _ in $(seq 50); do kill -0 "$AST_PID" 2>/dev/null || break; sleep 0.1; done
        kill -KILL "$AST_PID" 2>/dev/null || true
        wait "$AST_PID" 2>/dev/null || true
    fi
    cp "$STATE/log/messages.log" "$(dirname "$LOG")/asterisk.log" 2>/dev/null || true
    rm -rf "$STATE"
    exit "$rc"
}
trap cleanup EXIT INT TERM

"$ASTERISK" -C "$CONF" -f -n -q </dev/null >"$STATE/console.log" 2>&1 &
AST_PID=$!
for _ in $(seq 100); do
    kill -0 "$AST_PID" 2>/dev/null || break
    if [ -S "$STATE/run/asterisk.ctl" ] && "$ASTERISK" -C "$CONF" -rx "core waitfullybooted" >/dev/null 2>&1; then break; fi
    sleep 0.2
done
"$ASTERISK" -C "$CONF" -rx "pjsip show transports" | grep -q "127.0.0.1:5080" \
    || { echo "Asterisk failed to start" >&2; tail -n 30 "$STATE/console.log" >&2; exit 1; }

export ASTX="$ASTERISK -C $CONF -rx"
mkdir -p "$(dirname "$LOG")"
python -I "$HERE/run.py" "$ROOT/build_qemu" "$SCENARIO" "$LOG"
