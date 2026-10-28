#!/usr/bin/env bash
# Build the web simulator into sim/web/build/ (index.html/.js/.wasm).
# Needs emcmake on PATH: `source $EMSDK/emsdk_env.sh` first (see README.md).
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
build="$here/build"

if ! command -v emcmake >/dev/null 2>&1; then
    if [ -n "${EMSDK:-}" ] && [ -f "$EMSDK/emsdk_env.sh" ]; then
        # shellcheck disable=SC1091
        source "$EMSDK/emsdk_env.sh" >/dev/null 2>&1
    fi
fi
command -v emcmake >/dev/null 2>&1 || {
    echo "error: emcmake not found. Install emsdk and 'source \$EMSDK/emsdk_env.sh' (or export EMSDK=...)." >&2
    exit 1
}

if [ ! -f "$build/build.ninja" ]; then
    emcmake cmake -G Ninja -S "$here" -B "$build" -DCMAKE_BUILD_TYPE=Release
fi
cmake --build "$build" "$@"
echo "built: $build/index.html"
