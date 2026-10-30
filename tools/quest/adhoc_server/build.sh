#!/usr/bin/env bash
# Builds the standalone ad hoc server (main.cpp) as one static x86-64 Linux binary: it runs on any distribution
# without libraries installed. Output: out/adhoc-server/wipeout-adhoc-server.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
PK="$ROOT/vendor/portablekit/host"
CXX="${CXX:-$HOME/tools/hostclang/clang++}"
[[ -x "$CXX" ]] || CXX=clang++
OUT="$ROOT/out/adhoc-server"
mkdir -p "$OUT"
"$CXX" -std=c++20 -O2 -pthread -static -I"$PK" -I"$PK/.." \
    "$HERE/main.cpp" "$PK/adhoc/server.cpp" "$PK/adhoc/client.cpp" "$PK/adhoc/discovery.cpp" \
    -o "$OUT/wipeout-adhoc-server"
strip "$OUT/wipeout-adhoc-server" 2>/dev/null || true
ls -l "$OUT/wipeout-adhoc-server"
