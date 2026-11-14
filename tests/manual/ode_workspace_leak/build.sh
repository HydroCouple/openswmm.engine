#!/bin/sh
# Build the OdeWorkspace leak falsifier.
#
# OdeSolver.cpp is a self-contained TU (headers only, no engine deps), so this
# compiles it directly rather than needing the full engine build.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
SRC="$ROOT/src/engine/math"

mkdir -p "$HERE/obj"
clang++ -std=c++17 -O2 -g -Wall -Wextra \
    -I"$SRC" \
    "$SRC/OdeSolver.cpp" "$HERE/main.cpp" \
    -o "$HERE/obj/ode_workspace_leak"

echo "built: $HERE/obj/ode_workspace_leak"
