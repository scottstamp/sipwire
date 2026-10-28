#!/usr/bin/env bash
# Send one CLI command to the dev PBX started by run-local.sh.
#
#   tools/asterisk/cli.sh "pjsip show endpoints"
#   tools/asterisk/cli.sh "channel originate PJSIP/1001 extension 601@sipwire"
#
# Uses the same ASTERISK_STATE as run-local.sh (default /tmp/sipwire-asterisk).
set -euo pipefail

if [ "$#" -lt 1 ]; then
    echo "usage: $0 \"<asterisk cli command>\"" >&2
    exit 2
fi

STATE="${ASTERISK_STATE:-/tmp/sipwire-asterisk}"
CONF="$STATE/etc/asterisk.conf"

if [ ! -f "$CONF" ]; then
    echo "cli.sh: no rendered config at $CONF; start tools/asterisk/run-local.sh first" >&2
    exit 1
fi

exec asterisk -C "$CONF" -rx "$*"
