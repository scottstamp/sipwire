#!/usr/bin/env bash
# Usage: tools/ci.sh host-test | build
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

case "${1:-}" in
host-test)
    cmake -S "$ROOT/test_host" -B "$ROOT/build_host" -G Ninja
    cmake --build "$ROOT/build_host"
    ctest --test-dir "$ROOT/build_host" --output-on-failure
    ;;
build)
    cd "$ROOT"
    idf.py set-target esp32s3
    idf.py build
    ;;
*)
    echo "usage: $0 host-test|build" >&2
    exit 2
    ;;
esac
