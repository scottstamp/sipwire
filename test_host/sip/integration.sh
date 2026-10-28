#!/usr/bin/env bash
# Integration test: the real UA (sipcli = sip core + sip_udp.c) against a
# private Asterisk instance on 127.0.0.1:5080. Not part of ctest.
#
#   test_host/sip/integration.sh            (builds sipcli if needed)
#   KEEP_STATE=1 ... keeps the Asterisk state dir for inspection
#   VERBOSE=1 ...    prints full SIP traces
#
# Starts Asterisk, runs the scenarios, stops only the Asterisk it started.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUILD="${BUILD_DIR:-$ROOT/build_host}"
SIPCLI="$BUILD/sip/sipcli"
ASTERISK="${ASTERISK:-/usr/sbin/asterisk}"
PORT=5080

if [ ! -x "$SIPCLI" ]; then
    : "${IDF_PATH:?IDF_PATH must be set to build the host tests}"
    cmake -S "$ROOT/test_host" -B "$BUILD" -G Ninja >/dev/null
    cmake --build "$BUILD" --target sipcli >/dev/null
fi
[ -x "$ASTERISK" ] || { echo "asterisk not found at $ASTERISK" >&2; exit 2; }

MODDIR=""
for d in /usr/lib/x86_64-linux-gnu/asterisk/modules /usr/lib/aarch64-linux-gnu/asterisk/modules \
         /usr/lib/asterisk/modules /usr/lib64/asterisk/modules; do
    if [ -f "$d/chan_pjsip.so" ]; then MODDIR="$d"; break; fi
done
[ -n "$MODDIR" ] || { echo "Asterisk modules (chan_pjsip.so) not found" >&2; exit 2; }
DATADIR="${ASTERISK_DATADIR:-/usr/share/asterisk}"
[ -d "$DATADIR/documentation" ] || { echo "Asterisk data dir $DATADIR has no documentation/" >&2; exit 2; }

# short path: the control socket lives in here (sun_path limit)
STATE="$(mktemp -d /tmp/sipwire-ast.XXXXXX)"
mkdir -p "$STATE"/{etc,cache,varlib,db,keys,agi,spool,run,log}
sed -e "s|@STATE@|$STATE|g" -e "s|@MODDIR@|$MODDIR|g" -e "s|@DATADIR@|$DATADIR|g" \
    "$HERE/asterisk/asterisk.conf.in" > "$STATE/etc/asterisk.conf"
cp "$HERE"/asterisk/{modules,pjsip,extensions,rtp,logger}.conf "$STATE/etc/"
CONF="$STATE/etc/asterisk.conf"

ast() { "$ASTERISK" -C "$CONF" -rx "$1"; }

AST_PID=""
cleanup() {
    local rc=$?
    # signal our own PID only (never "core stop" through a socket that
    # might belong to someone else's instance)
    if [ -n "$AST_PID" ] && kill -0 "$AST_PID" 2>/dev/null; then
        kill -TERM "$AST_PID" 2>/dev/null || true
        for _ in $(seq 50); do kill -0 "$AST_PID" 2>/dev/null || break; sleep 0.1; done
        if kill -0 "$AST_PID" 2>/dev/null; then kill -KILL "$AST_PID" 2>/dev/null || true; fi
        wait "$AST_PID" 2>/dev/null || true
    fi
    if [ "${KEEP_STATE:-0}" = 1 ]; then
        echo "state kept in $STATE"
    else
        rm -rf "$STATE"
    fi
    exit "$rc"
}
trap cleanup EXIT INT TERM

"$ASTERISK" -C "$CONF" -f -n -q </dev/null >"$STATE/console.log" 2>&1 &
AST_PID=$!
booted=0
for _ in $(seq 100); do
    if ! kill -0 "$AST_PID" 2>/dev/null; then break; fi
    if [ -S "$STATE/run/asterisk.ctl" ] && ast "core waitfullybooted" >/dev/null 2>&1; then booted=1; break; fi
    sleep 0.2
done
# make sure the CLI talks to *our* instance
if [ "$booted" = 1 ] && ! ast "core show settings" | grep -q "Run/Sockets directory: *$STATE/run"; then
    echo "Asterisk did not pick up the private directories" >&2
    exit 1
fi
if [ "$booted" != 1 ] || ! ast "pjsip show transports" | grep -q "127.0.0.1:$PORT"; then
    echo "Asterisk failed to start (pid $AST_PID)" >&2
    tail -n 30 "$STATE/console.log" "$STATE/log/messages.log" 2>/dev/null >&2 || true
    exit 1
fi
echo "Asterisk $("$ASTERISK" -V | cut -d' ' -f2) running as pid $AST_PID on 127.0.0.1:$PORT"

ASTX="$ASTERISK -C $CONF -rx"
V=()
[ "${VERBOSE:-0}" = 1 ] && V=(-v)

set +e
"$SIPCLI" "${V[@]}" --server "127.0.0.1:$PORT" --local-ip 127.0.0.1 --user 1001 --pass secret1001 --rtp 40000 \
    "echo:1 register 1001 with digest auth" \
    register \
    "shell:$ASTX 'pjsip show contacts' | tee /dev/stderr | grep -q '1001/sip:1001@127.0.0.1'" \
    \
    "echo:2 call 600 (Echo), media, 2 s, hold/resume, BYE" \
    dial:600 wait:active expect-media sleep:2 hold unhold hangup sleep:1 \
    "expect-log:BYE answered 200" \
    \
    "echo:3 call 601, Asterisk hangs up" \
    dial:601 wait:active expect-media wait:ended:10 expect-end:remote \
    \
    "echo:4 inbound: channel originate PJSIP/1001 application Wait 3" \
    "shell:($ASTX 'channel originate PJSIP/1001 application Wait 3' >/dev/null 2>&1 &)" \
    incoming:10 answer wait:active expect-media wait:ended:10 expect-end:remote \
    \
    "echo:5 unregister" \
    unregister \
    "shell:! $ASTX 'pjsip show contacts' | grep -q '1001/sip:1001@'" \
    2>&1 | tee "$STATE/sipcli.log" | grep -E "^(===|STEP|EVENT|RESULT|FAIL|LOG \[[EW]\])|Contact:"
rc=${PIPESTATUS[0]}
set -e

if [ "$rc" -ne 0 ]; then
    echo "--- sipcli failed (rc=$rc); Asterisk log tail:"
    tail -n 40 "$STATE/log/messages.log" 2>/dev/null || true
    exit 1
fi
echo "integration: all scenarios passed"
