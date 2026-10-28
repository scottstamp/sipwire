#!/usr/bin/env bash
# Serve sim/web/build/ on http://localhost:8080 (PORT=... to change).
#   --watch   rebuild when components/ui/** or sim/web/src/** change
#             (polls mtimes every second; no inotify needed)
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
port="${PORT:-8080}"
watch=0
[ "${1:-}" = "--watch" ] && watch=1

[ -f "$here/build/index.html" ] || "$here/build.sh"

python3 -m http.server "$port" --bind 127.0.0.1 --directory "$here/build" &
srv=$!
trap 'kill $srv 2>/dev/null || true' EXIT INT TERM
echo "serving http://localhost:$port/"

if [ "$watch" = 1 ]; then
    snapshot() {
        find "$root/components/ui" "$here/src" "$here/shell.html" "$here/lv_conf.h" \
            -type f \( -name '*.c' -o -name '*.h' -o -name '*.html' \) \
            -printf '%p %T@\n' 2>/dev/null | sort | cksum
    }
    last="$(snapshot)"
    echo "watching components/ui and sim/web/src (Ctrl-C to stop)"
    while kill -0 "$srv" 2>/dev/null; do
        sleep 1
        cur="$(snapshot)"
        if [ "$cur" != "$last" ]; then
            last="$cur"
            echo "change detected, rebuilding..."
            if "$here/build.sh"; then echo "rebuilt; reload the page"; else echo "build failed"; fi
        fi
    done
else
    wait "$srv"
fi
