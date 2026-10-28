#!/usr/bin/env bash
# Run the sipwire development PBX in the foreground, from tools/asterisk/conf,
# without root or system install paths. All runtime state (rendered config,
# run/spool/log/db/keys dirs, CLI socket) lives in one state dir.
#
#   tools/asterisk/run-local.sh      foreground, logs to the terminal
#   tools/asterisk/run-local.sh -i   same, with the interactive CLI (-c)
#
# Environment overrides:
#   ASTERISK_STATE   state dir (default /tmp/sipwire-asterisk)
#   ASTERISK_MODDIR  Asterisk module dir (default: probed, see below)
#   ASTERISK_DATADIR packaged data dir with sounds/ (default /usr/share/asterisk)
#   ASTERISK_MOHDIR  packaged MOH dir (default /usr/share/asterisk/moh)
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CONF_SRC="$HERE/conf"

STATE="${ASTERISK_STATE:-/tmp/sipwire-asterisk}"
case "$STATE" in
    /*) ;;
    *) STATE="$PWD/$STATE" ;;
esac

INTERACTIVE=0
case "${1:-}" in
    "") ;;
    -i) INTERACTIVE=1 ;;
    *) echo "usage: $0 [-i]" >&2; exit 2 ;;
esac

die() { echo "run-local.sh: $*" >&2; exit 1; }

command -v asterisk >/dev/null 2>&1 || die "asterisk binary not found (install the asterisk package for dev only)"

# Module dir: honour an override, otherwise probe the Debian/Ubuntu multiarch paths.
MODDIR="${ASTERISK_MODDIR:-}"
if [ -z "$MODDIR" ]; then
    for cand in /usr/lib/x86_64-linux-gnu/asterisk/modules \
                /usr/lib/aarch64-linux-gnu/asterisk/modules \
                /usr/lib/asterisk/modules; do
        if [ -f "$cand/res_pjsip.so" ]; then
            MODDIR="$cand"
            break
        fi
    done
fi
[ -n "$MODDIR" ] && [ -f "$MODDIR/res_pjsip.so" ] \
    || die "module dir not found; set ASTERISK_MODDIR to the dir containing res_pjsip.so"

DATADIR="${ASTERISK_DATADIR:-/usr/share/asterisk}"
[ -d "$DATADIR/sounds/en" ] \
    || die "no sounds at $DATADIR/sounds/en; install asterisk-core-sounds-en-gsm or set ASTERISK_DATADIR"

MOHDIR="${ASTERISK_MOHDIR:-/usr/share/asterisk/moh}"
if [ ! -d "$MOHDIR" ] || [ -z "$(ls -A "$MOHDIR" 2>/dev/null)" ]; then
    echo "run-local.sh: warning: no MOH files in $MOHDIR (install asterisk-moh-opsound-gsm); MusicOnHold will be silent" >&2
    MOHDIR="$STATE/moh"
fi

# Packaged defaults for the optional modules we do not override (ari.conf,
# cdr.conf, pjsip_wizard.conf, ...). Our conf/ files are layered on top.
BASE_ETC="${ASTERISK_BASE_ETC:-/etc/asterisk}"
[ -d "$BASE_ETC" ] || die "no packaged config at $BASE_ETC; install asterisk-config or set ASTERISK_BASE_ETC"

ETC="$STATE/etc"
VARLIB="$STATE/lib"
DB="$STATE/db"
AGI="$STATE/agi-bin"
SPOOL="$STATE/spool"
RUN="$STATE/run"
LOG="$STATE/log"
CACHE="$STATE/cache"

# Fresh rendered config every run; the rest of the state dir is kept.
rm -rf "$ETC"
mkdir -p "$ETC" "$VARLIB/keys" "$DB" "$AGI" "$RUN" "$LOG" "$CACHE" \
         "$SPOOL/outgoing" "$SPOOL/voicemail" "$SPOOL/monitor" "$SPOOL/tmp"
cp -R "$BASE_ETC"/. "$ETC"/

# Render every conf/*.conf over the packaged defaults. Only the tokens are
# substituted; everything else is copied as-is.
shopt -s nullglob
conf_files=("$CONF_SRC"/*.conf)
[ "${#conf_files[@]}" -gt 0 ] || die "no .conf files in $CONF_SRC"
for src in "${conf_files[@]}"; do
    dst="$ETC/$(basename "$src")"
    sed \
        -e "s|@ETCDIR@|$ETC|g" \
        -e "s|@MODDIR@|$MODDIR|g" \
        -e "s|@DATADIR@|$DATADIR|g" \
        -e "s|@VARLIBDIR@|$VARLIB|g" \
        -e "s|@DBDIR@|$DB|g" \
        -e "s|@AGIDIR@|$AGI|g" \
        -e "s|@SPOOLDIR@|$SPOOL|g" \
        -e "s|@RUNDIR@|$RUN|g" \
        -e "s|@LOGDIR@|$LOG|g" \
        -e "s|@CACHEDIR@|$CACHE|g" \
        -e "s|@MOHDIR@|$MOHDIR|g" \
        "$src" > "$dst"
    if grep -q '@[A-Z]*DIR@' "$dst"; then
        die "unrendered @TOKEN@ left in $dst; check the sed list in run-local.sh"
    fi
done

ASTERISK_CONF="$ETC/asterisk.conf"
[ -f "$ASTERISK_CONF" ] || die "asterisk.conf was not rendered"

echo "run-local.sh: state=$STATE" >&2
echo "run-local.sh: moddir=$MODDIR datadir=$DATADIR moh=$MOHDIR" >&2
echo "run-local.sh: starting in foreground (Ctrl-C or 'cli.sh \"core stop now\"' to stop)" >&2

if [ "$INTERACTIVE" -eq 1 ]; then
    exec asterisk -f -c -C "$ASTERISK_CONF" -vvv
fi
exec asterisk -f -C "$ASTERISK_CONF" -vvv
