#!/usr/bin/env bash
# Build the off-device gr0m character renderer (compiles the real
# src/buddy.cpp + src/buddies/gr0m.cpp against host stub headers).
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/../../src"
g++ -std=c++17 -O2 -Wno-narrowing \
  -I"$HERE/shims" -I"$SRC" \
  "$HERE/render_main.cpp" "$SRC/buddy.cpp" "$SRC/buddies/gr0m.cpp" \
  -o "$HERE/gr0m_render"
echo "built $HERE/gr0m_render"
