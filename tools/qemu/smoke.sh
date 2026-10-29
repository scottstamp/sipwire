#!/usr/bin/env bash
# QEMU smoke test: boots the real firmware (CONFIG_SIPWIRE_QEMU) and drives it
# over its serial console against a private Asterisk on the host. The scenario
# is split into shards (tools/qemu/smoke.d/[0-9]*.txt) that run concurrently,
# each with its own QEMU, its own Asterisk (own SIP port, RTP range, state dir)
# and its own OTA HTTP server:
#   10-call-echo     register, G.722 echo call with hold/resume/DTMF/RTP/AEC, remote hangup
#   20-call-inbound  inbound answer/reject, unknown number (404), re-register on account
#                    change, unregister, OTA refusals (wrong/junk/missing image)
#   30-blf-mwi       BLF (ringing/busy/idle of an originated call), MWI (voicemail left
#                    and deleted)
#   40-ota           time settings, OTA update from a local HTTP server (installs the
#                    build into ota_1, reboots, checks it boots from ota_1 and is valid)
#
#   tools/qemu/build.sh && tools/qemu/smoke.sh            all shards in parallel
#   tools/qemu/smoke.sh smoke.d/30-blf-mwi.txt [log]      one scenario file, output streamed
#
# Logs: build_qemu/smoke-<shard>.log, smoke-<shard>.asterisk.log, smoke-<shard>.ota-http.log
# Env: SMOKE_PORT_BASE (SIP port of shard 0, default 5180), SMOKE_RTP_BASE (default 12100).
# Needs: exported ESP-IDF env, qemu-xtensa (idf_tools.py install qemu-xtensa),
# asterisk (Debian/Ubuntu package). Stops only the Asterisk instances it started.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUILD="$ROOT/build_qemu"
AST_CFG="$ROOT/test_host/sip/asterisk"
ASTERISK="${ASTERISK:-/usr/sbin/asterisk}"
PORT_BASE="${SMOKE_PORT_BASE:-5180}"
RTP_BASE="${SMOKE_RTP_BASE:-12100}"

[ -f "$BUILD/qemu_flash.bin" ] || { echo "run tools/qemu/build.sh first" >&2; exit 2; }
[ -x "$ASTERISK" ] || { echo "asterisk not found at $ASTERISK" >&2; exit 2; }
FW_BIN="$BUILD/sipwire.bin"
[ -f "$FW_BIN" ] || { echo "missing $FW_BIN (run tools/qemu/build.sh)" >&2; exit 2; }
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

# Images the OTA scenario serves (shared, read-only; each shard serves its own copy):
#   sipwire.bin  the firmware just built (installing it must boot from ota_1)
#   other.bin    the same image with another project name  -> must be rejected
#   junk.bin     random bytes                               -> must be rejected
OTA_SRC="$(mktemp -d /tmp/sipwire-ota.XXXXXX)"
RESDIR="$(mktemp -d /tmp/sipwire-res.XXXXXX)"
python3 -I - "$FW_BIN" "$OTA_SRC/other.bin" "$OTA_SRC/junk.bin" <<'PY'
import os, sys
src, other, junk = sys.argv[1:4]
img = bytearray(open(src, 'rb').read())
# esp_app_desc_t sits after the 24-byte image header and the 8-byte segment header;
# project_name is at +48 inside it (magic, secure_version, reserved[2], version[32]).
off = 32 + 48
assert img[off:off + 8] == b'sipwire\0', 'project_name not where expected'
img[off:off + 32] = b'otherapp'.ljust(32, b'\0')
open(other, 'wb').write(img)
open(junk, 'wb').write(os.urandom(8192))
PY
cp "$FW_BIN" "$OTA_SRC/sipwire.bin"

# run_shard <index> <name> <scenario>: one QEMU + Asterisk + HTTP server; runs in a subshell.
run_shard() {
    local idx="$1" name="$2" scenario="$3"
    local t0=$SECONDS
    local sip_port=$((PORT_BASE + idx)) rtp_start=$((RTP_BASE + idx * 100))
    local state ast_pid="" http_pid="" run_pid=""
    state="$(mktemp -d /tmp/sipwire-qemu.XXXXXX)"   # short path: Asterisk's control socket lives in here
    mkdir -p "$state"/{etc,cache,varlib,db,keys,agi,spool,run,log,ota}

    cleanup() {
        local rc=$?
        trap - EXIT INT TERM
        for pid in "$run_pid" "$http_pid"; do
            if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
                kill -TERM "$pid" 2>/dev/null || true
                wait "$pid" 2>/dev/null || true
            fi
        done
        if [ -n "$ast_pid" ] && kill -0 "$ast_pid" 2>/dev/null; then
            kill -TERM "$ast_pid" 2>/dev/null || true
            for _ in $(seq 50); do kill -0 "$ast_pid" 2>/dev/null || break; sleep 0.1; done
            kill -KILL "$ast_pid" 2>/dev/null || true
            wait "$ast_pid" 2>/dev/null || true
        fi
        cp "$state/log/messages.log" "$BUILD/smoke-$name.asterisk.log" 2>/dev/null || true
        cp "$state/http.log" "$BUILD/smoke-$name.ota-http.log" 2>/dev/null || true
        rm -rf "$state"
        echo "$rc $((SECONDS - t0))" > "$RESDIR/$name"
        exit "$rc"
    }
    trap cleanup EXIT INT TERM

    sed -e "s|@STATE@|$state|g" -e "s|@MODDIR@|$MODDIR|g" -e "s|@DATADIR@|$DATADIR|g" \
        "$AST_CFG/asterisk.conf.in" > "$state/etc/asterisk.conf"
    cp "$AST_CFG"/{modules,extensions,logger,voicemail}.conf "$state/etc/"
    # Own SIP port and RTP range per shard. Inside slirp the host is 10.0.2.2: advertise that
    # in SDP instead of 127.0.0.1.
    sed -e "s|127.0.0.1:5080|127.0.0.1:$sip_port|" \
        -e '/^\[phone\](!)/a media_address = 10.0.2.2' "$AST_CFG/pjsip.conf" > "$state/etc/pjsip.conf"
    sed -e "s|^rtpstart *=.*|rtpstart = $rtp_start|" -e "s|^rtpend *=.*|rtpend = $((rtp_start + 99))|" \
        "$AST_CFG/rtp.conf" > "$state/etc/rtp.conf"
    local conf="$state/etc/asterisk.conf"

    "$ASTERISK" -C "$conf" -f -n -q </dev/null >"$state/console.log" 2>&1 &
    ast_pid=$!
    for _ in $(seq 100); do
        kill -0 "$ast_pid" 2>/dev/null || break
        if [ -S "$state/run/asterisk.ctl" ] && "$ASTERISK" -C "$conf" -rx "core waitfullybooted" >/dev/null 2>&1; then break; fi
        sleep 0.1
    done
    "$ASTERISK" -C "$conf" -rx "pjsip show transports" | grep -q "127.0.0.1:$sip_port" \
        || { echo "Asterisk failed to start (SIP port $sip_port)" >&2; tail -n 30 "$state/console.log" >&2; exit 1; }

    export ASTX="$ASTERISK -C $conf -rx"
    export AST_SPOOL="$state/spool"   # voicemail lives here (30-blf-mwi deletes a message)
    export SIP_PORT="$sip_port"

    # OTA update server for the scenario's `ota <url>` steps (the guest reaches the host at 10.0.2.2)
    cp "$OTA_SRC"/*.bin "$state/ota/"
    local ota_port
    ota_port="$(python3 -I -c 'import socket; s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])')"
    python3 -I -m http.server "$ota_port" --bind 127.0.0.1 --directory "$state/ota" >"$state/http.log" 2>&1 &
    http_pid=$!
    export HTTP_LOG="$state/http.log"
    for _ in $(seq 100); do
        (exec 3<>"/dev/tcp/127.0.0.1/$ota_port") 2>/dev/null && break
        sleep 0.05
    done
    export OTA_BASE="http://10.0.2.2:$ota_port"

    echo "shard $name: SIP 127.0.0.1:$sip_port, RTP $rtp_start-$((rtp_start + 99)), OTA $ota_port"
    python -I "$HERE/run.py" "$BUILD" "$scenario" &
    run_pid=$!
    local rc=0
    wait "$run_pid" || rc=$?
    run_pid=""
    exit "$rc"
}

rm -f "$RESDIR"/* 2>/dev/null || true
trap 'rm -rf "$OTA_SRC" "$RESDIR"' EXIT

if [ $# -ge 1 ]; then
    # one scenario file: run it alone, output streamed (and kept in the log)
    SCEN="$1"
    case "$SCEN" in /*) ;; *) [ -f "$SCEN" ] || SCEN="$HERE/$SCEN" ;; esac
    [ -f "$SCEN" ] || { echo "no such scenario: $1" >&2; exit 2; }
    NAME="$(basename "$SCEN" .txt)"
    LOG="${2:-$BUILD/smoke-$NAME.log}"
    mkdir -p "$(dirname "$LOG")"
    set +e
    ( run_shard 0 "$NAME" "$SCEN" ) 2>&1 | tee "$LOG"
    rc=${PIPESTATUS[0]}
    set -e
    exit "$rc"
fi

SHARDS=("$HERE"/smoke.d/[0-9]*.txt)
[ -f "${SHARDS[0]}" ] || { echo "no shard files in $HERE/smoke.d" >&2; exit 2; }
T_ALL=$SECONDS
PIDS=()
NAMES=()
i=0
for f in "${SHARDS[@]}"; do
    n="$(basename "$f" .txt)"
    NAMES+=("$n")
    ( run_shard "$i" "$n" "$f" ) >"$BUILD/smoke-$n.log" 2>&1 &
    PIDS+=($!)
    i=$((i + 1))
done
stop_all() { kill -TERM "${PIDS[@]}" 2>/dev/null || true; wait 2>/dev/null || true; exit 130; }
trap stop_all INT TERM

fail=0
for k in "${!PIDS[@]}"; do
    wait "${PIDS[$k]}" || true
done
printf '%-18s %-5s %s\n' shard result wall
slowest="" slow_s=-1
for n in "${NAMES[@]}"; do
    read -r rc secs < "$RESDIR/$n" 2>/dev/null || { rc=99; secs=0; }
    if [ "$rc" -eq 0 ]; then res=PASS; else res=FAIL; fail=1; fi
    printf '%-18s %-5s %ss   (build_qemu/smoke-%s.log)\n' "$n" "$res" "$secs" "$n"
    if [ "$secs" -gt "$slow_s" ]; then slow_s=$secs; slowest=$n; fi
    if [ "$rc" -ne 0 ]; then
        echo "--- tail of smoke-$n.log:"
        tail -n 25 "$BUILD/smoke-$n.log"
    fi
done
echo "total wall ${SECONDS}s (shards ran for $((SECONDS - T_ALL))s, slowest: $slowest ${slow_s}s)"
if [ "$fail" -eq 0 ]; then echo "RESULT OK"; else echo "RESULT FAIL"; fi
exit "$fail"
