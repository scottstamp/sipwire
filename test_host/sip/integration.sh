#!/usr/bin/env bash
# Integration test: the real UA (sipcli = sip core + sip_udp.c) against a
# private Asterisk instance on 127.0.0.1:5080. Not part of ctest.
#
#   test_host/sip/integration.sh            (builds sipcli if needed)
#   KEEP_STATE=1 ... keeps the Asterisk state dir for inspection
#   VERBOSE=1 ...    prints full SIP traces
#   ONLY=features    only the call feature scenarios (10-12: find the handset,
#                    forwarding); the G.722 cross-check still runs
#
# Starts Asterisk, runs the scenarios, stops only the Asterisk it started.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUILD="${BUILD_DIR:-$ROOT/build_host}"
SIPCLI="$BUILD/sip/sipcli"
G722TOOL="$BUILD/codec/g722tool"
ASTERISK="${ASTERISK:-/usr/sbin/asterisk}"
PORT=5080

if [ ! -x "$SIPCLI" ] || [ ! -x "$G722TOOL" ]; then
    : "${IDF_PATH:?IDF_PATH must be set to build the host tests}"
    cmake -S "$ROOT/test_host" -B "$BUILD" -G Ninja >/dev/null
    cmake --build "$BUILD" --target sipcli g722tool >/dev/null
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
cp "$HERE"/asterisk/{modules,pjsip,extensions,rtp,logger,voicemail}.conf "$STATE/etc/"
REC="$STATE/rec"
printf '[globals]\nSIPWIRE_REC=%s\n' "$REC" >> "$STATE/etc/extensions.conf"
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

# G.722 bitstream cross-check: our codec vs Asterisk's codec_g722, both
# directions, through Asterisk's own "file convert" (sln16 <-> g722).
echo "=== 0 G.722: components/codec vs Asterisk codec_g722 (file convert)"
X="$STATE/xcheck"
mkdir -p "$X"
"$G722TOOL" ka "$X/ka.sln16" 2
"$G722TOOL" enc "$X/ka.sln16" "$X/ours.g722"
ast "file convert $X/ka.sln16 $X/ast.g722" >/dev/null
ast "file convert $X/ours.g722 $X/ast.sln16" >/dev/null
"$G722TOOL" dec "$X/ours.g722" "$X/ours.sln16"
for f in ast.g722 ast.sln16; do
    [ -s "$X/$f" ] || { echo "FAIL: Asterisk did not write $f" >&2; exit 1; }
done
if ! cmp "$X/ours.g722" "$X/ast.g722"; then
    echo "FAIL: G.722 encoder output differs from Asterisk" >&2; exit 1
fi
if ! cmp "$X/ours.sln16" "$X/ast.sln16"; then
    echo "FAIL: G.722 decoder output differs from Asterisk" >&2; exit 1
fi
echo "G.722 encode and decode bit-exact with Asterisk ($(stat -c %s "$X/ours.g722") bytes)"
V=()
[ "${VERBOSE:-0}" = 1 ] && V=(-v)

rc=0
if [ "${ONLY:-}" != features ]; then
set +e
"$SIPCLI" "${V[@]}" --server "127.0.0.1:$PORT" --local-ip 127.0.0.1 --user 1001 --pass secret1001 --rtp 40000 \
    "echo:1 register 1001 with digest auth" \
    register \
    "shell:$ASTX 'pjsip show contacts' | tee /dev/stderr | grep -q '1001/sip:1001@127.0.0.1'" \
    \
    "echo:2 call 600 (Echo), G.722, 5 kHz tone echoed, hold/resume, BYE" \
    dial:600 wait:active expect-media:9 "expect-log:pt 9 dtmf 101" tone:5000:3 \
    hold unhold hangup sleep:1 \
    "expect-log:BYE answered 200" \
    \
    "echo:2b call 602: Asterisk records our G.722 5 kHz tone through its own decoder" \
    dial:602 wait:active expect-media:9 send-tone:5000:4 wait:ended:5 \
    "shell:python3 -I '$HERE/tone_check.py' '$REC.sln16' 16000 5000" \
    \
    "echo:3 call 601, Asterisk hangs up" \
    dial:601 wait:active expect-media wait:ended:10 expect-end:remote \
    \
    "echo:4 inbound: channel originate PJSIP/1001 application Wait 3" \
    "shell:($ASTX 'channel originate PJSIP/1001 application Wait 3' >/dev/null 2>&1 &)" \
    incoming:10 answer wait:active expect-media wait:ended:10 expect-end:remote \
    \
    "echo:4b blind transfer (REFER) of a bridged inbound call to 603, NOTIFY sipfrag 200" \
    "shell:($ASTX 'channel originate Local/1001@sipwire-test/n application Echo' >/dev/null 2>&1 &)" \
    incoming:10 answer wait:active sleep:1 xfer:603 expect-xfer:success:10 \
    "shell:$ASTX 'core show channels concise' | grep -q '^Local/1001@sipwire-test-.*;2!sipwire-test!603!'" \
    hangup sleep:1 "expect-log:BYE answered 200" \
    \
    "echo:5 unregister" \
    unregister \
    "shell:! $ASTX 'pjsip show contacts' | grep -q '1001/sip:1001@'" \
    2>&1 | tee "$STATE/sipcli.log" | grep -E "^(===|STEP|EVENT|RESULT|FAIL|TONE|LOG \[[EW]\])|Contact:"
rc=${PIPESTATUS[0]}
set -e

if [ "$rc" -eq 0 ]; then
    # Narrowband contrast: a PCMU-only UA gets PCMU; the 16 kHz <-> 8 kHz
    # path carries 1 kHz fine (and could not carry the 5 kHz above).
    set +e
    "$SIPCLI" "${V[@]}" --server "127.0.0.1:$PORT" --local-ip 127.0.0.1 --user 1002 --pass secret1002 \
        --rtp 40002 --codecs 0 \
        "echo:6 PCMU-only UA: call 600, 1 kHz tone through the resampled G.711 path" \
        register dial:600 wait:active expect-media:0 tone:1000:3 hangup sleep:1 unregister \
        2>&1 | tee -a "$STATE/sipcli.log" | grep -E "^(===|STEP|EVENT|RESULT|FAIL|TONE|LOG \[[EW]\])"
    rc=${PIPESTATUS[0]}
    set -e
fi

if [ "$rc" -eq 0 ]; then
    # BLF + MWI. 1002 runs in the background and answers the call Asterisk
    # originates to it; 1001 watches 1002 (dialog package) and its own mailbox
    # (message-summary); 1003 leaves the voicemails.
    CLI=("$SIPCLI" "${V[@]}" --server "127.0.0.1:$PORT" --local-ip 127.0.0.1)
    VMDIR="$STATE/spool/voicemail/default"
    WAIT_1002="for i in \$(seq 50); do $ASTX 'pjsip show contacts' | grep -q '1002/sip:1002@' && exit 0; sleep 0.2; done; exit 1"
    LEAVE_VM() { # LEAVE_VM <mailbox digit>: 1003 calls 700<d> and sends a tone for 3 s
        echo "(${CLI[*]} --user 1003 --pass secret1003 --rtp 40008 register dial:700$1 wait:active send-tone:1000:3 hangup sleep:1 unregister >>'$STATE/sipcli-1003.log' 2>&1 &)"
    }
    set +e
    "${CLI[@]}" --user 1002 --pass secret1002 --rtp 40004 \
        register incoming:60 sleep:1 answer wait:active wait:ended:15 sleep:1 unregister \
        >"$STATE/sipcli-1002.log" 2>&1 &
    B_PID=$!
    "${CLI[@]}" --user 1001 --pass secret1001 --rtp 40006 \
        "echo:7 BLF: 1001 watches 1002; Asterisk calls 1002, it answers, Asterisk hangs up" \
        register subscribe-blf:1002 subscribe-blf:1003 expect-blf:1002:idle:10 expect-blf:1003:idle:10 \
        "shell:$WAIT_1002" \
        "shell:$ASTX 'pjsip show subscriptions inbound' | tee /dev/stderr | grep -q '1001'" \
        "shell:($ASTX 'channel originate PJSIP/1002 application Wait 3' >/dev/null 2>&1 &)" \
        expect-blf:1002:ringing:10 expect-blf:1002:busy:10 expect-blf:1002:idle:15 expect-blf:1003:idle \
        unsubscribe-blf:1003 sleep:1 \
        \
        "echo:8 MWI: 1001 subscribes to its mailbox; 1003 leaves a voicemail, then it is deleted" \
        subscribe-mwi sleep:1 "expect-log:mwi 1001: subscribed" expect-mwi:no:0:10 \
        "shell:$(LEAVE_VM 1)" \
        expect-mwi:yes:1:30 \
        "shell:ls $VMDIR/1001/INBOX/ && rm -f $VMDIR/1001/INBOX/*" \
        expect-mwi:no:0:15 \
        \
        "echo:8b unregister ends the subscriptions (un-SUBSCRIBE, Expires: 0)" \
        unregister sleep:1 \
        "shell:! $ASTX 'pjsip show subscriptions inbound' | grep -q '1001'" \
        2>&1 | tee -a "$STATE/sipcli.log" | grep -E "^(===|STEP|EVENT|RESULT|FAIL|LOG \[[EW]\])"
    rc=${PIPESTATUS[0]}
    wait "$B_PID"
    b_rc=$?
    if [ "$rc" -eq 0 ] && [ "$b_rc" -ne 0 ]; then
        echo "--- the watched phone (1002) failed:"; tail -n 20 "$STATE/sipcli-1002.log"; rc=1
    fi
    if [ "$rc" -eq 0 ]; then
        "${CLI[@]}" --user 1002 --pass secret1002 --rtp 40004 \
            "echo:9 unsolicited MWI: 1002 has no subscription; 1003 leaves it a voicemail" \
            register "shell:$(LEAVE_VM 2)" expect-mwi:yes:1:30 \
            "shell:rm -f $VMDIR/1002/INBOX/*" expect-mwi:no:0:15 unregister \
            2>&1 | tee -a "$STATE/sipcli.log" | grep -E "^(===|STEP|EVENT|RESULT|FAIL|LOG \[[EW]\])"
        rc=${PIPESTATUS[0]}
    fi
    set -e
fi
fi # ONLY

if [ "$rc" -eq 0 ]; then
    # Call features. 1002 runs in the background and answers the two calls that 1001
    # forwards to it; 1001 screens like the phone does (sipcli forward / alert-marker).
    CLI=("$SIPCLI" "${V[@]}" --server "127.0.0.1:$PORT" --local-ip 127.0.0.1)
    WAIT_1002="for i in \$(seq 50); do $ASTX 'pjsip show contacts' | grep -q '1002/sip:1002@' && exit 0; sleep 0.2; done; exit 1"
    set +e
    "${CLI[@]}" --user 1002 --pass secret1002 --rtp 40004 \
        register incoming:40 answer wait:active wait:ended:15 \
        incoming:40 answer wait:active wait:ended:15 sleep:1 unregister \
        >"$STATE/sipcli-1002.log" 2>&1 &
    B_PID=$!
    "${CLI[@]}" --user 1001 --pass secret1001 --rtp 40006 \
        "echo:10 find the handset: 652 dials 1001 with Alert-Info info=find; it rings through DND" \
        register alert-marker:find dnd:on \
        "shell:($ASTX 'channel originate PJSIP/1001 application Wait 2' >/dev/null 2>&1 &)" \
        expect-rejected:486 \
        "shell:($ASTX 'channel originate Local/652@sipwire-test application Wait 2' >/dev/null 2>&1 &)" \
        incoming:10 expect-alert hangup sleep:1 dnd:off \
        \
        "echo:11 forward always: 1001 answers 302 Contact sip:1002; Asterisk's Dial follows it to 1002" \
        "shell:$WAIT_1002" forward:1002 \
        "shell:($ASTX 'channel originate Local/1001@sipwire-test application Wait 3' >/dev/null 2>&1 &)" \
        expect-rejected:302 forward-off sleep:5 \
        "shell:grep -q 'EVENT incoming call=.* from=' '$STATE/sipcli-1002.log'" \
        \
        "echo:12 no answer: 1001 rings, then redirects the ringing call (302 after 180) to 1002" \
        "shell:($ASTX 'channel originate Local/1001@sipwire-test application Wait 3' >/dev/null 2>&1 &)" \
        incoming:10 sleep:2 redirect:1002 wait:ended:5 expect-end:local \
        unregister \
        2>&1 | tee -a "$STATE/sipcli.log" | grep -E "^(===|STEP|EVENT|RESULT|FAIL|LOG \[[EW]\])"
    rc=${PIPESTATUS[0]}
    wait "$B_PID"
    b_rc=$?
    if [ "$rc" -eq 0 ] && [ "$b_rc" -ne 0 ]; then
        echo "--- the forwarding target (1002) failed:"; tail -n 20 "$STATE/sipcli-1002.log"; rc=1
    fi
    if [ "$rc" -eq 0 ] && [ "$(grep -c 'EVENT call=.* state=active' "$STATE/sipcli-1002.log")" -lt 2 ]; then
        echo "--- 1002 did not get both forwarded calls:"; tail -n 20 "$STATE/sipcli-1002.log"; rc=1
    fi
    set -e
fi

if [ "$rc" -ne 0 ]; then
    echo "--- sipcli failed (rc=$rc); Asterisk log tail:"
    tail -n 40 "$STATE/log/messages.log" 2>/dev/null || true
    exit 1
fi
echo "integration: all scenarios passed"
