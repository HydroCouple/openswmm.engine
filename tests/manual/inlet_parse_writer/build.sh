#!/bin/sh
# Build + run the inlet parse/write link harness (see main.cpp).
#
# Not part of the CMake test suite: it links only the ~20 translation units the
# [INLETS] / [INLET_USAGE] / [INLET_JUNCTIONS] parse-write path needs, so it can
# be built with a bare compiler when the full engine (HDF5, SQLite, GDAL) is not
# configured. Run from the repository root:
#
#     sh tests/manual/inlet_parse_writer/build.sh
#
# Outputs land in this directory: written.inp, written2.inp.
set -e
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$ROOT"
OBJ=${OBJ:-tests/manual/inlet_parse_writer/obj}
INC="-I include -I src -I src/engine -I build-arm64-osx/include"
mkdir -p "$OBJ"

SRC="
tests/manual/inlet_parse_writer/main.cpp
src/engine/core/ErrorCodes.cpp
src/engine/core/InpWriter.cpp
src/engine/core/PathResolver.cpp
src/engine/core/UnitConversion.cpp
src/engine/edit/ObjectDeleter.cpp
src/engine/edit/VirtualJunctionOps.cpp
src/engine/hydraulics/ForceMain.cpp
src/engine/hydraulics/Link.cpp
src/engine/hydraulics/Street.cpp
src/engine/hydraulics/Transect.cpp
src/engine/hydraulics/XSection.cpp
src/engine/input/MultiColumnSeriesFile.cpp
src/engine/input/PostParseResolver.cpp
src/engine/input/Tokenizer.cpp
src/engine/input/handlers/InfraHandler.cpp
src/engine/input/handlers/LinksHandler.cpp
src/engine/input/handlers/NodesHandler.cpp
src/engine/plugins/ProcessComponentRegistry.cpp
"

OBJS=""
for f in $SRC; do
    o="$OBJ/$(echo "$f" | tr '/' '_' | sed 's/\.cpp$/.o/')"
    c++ -std=c++20 -c -O0 $INC "$f" -o "$o"
    OBJS="$OBJS $o"
done
c++ -o "$OBJ/inlet_parse_writer" $OBJS
exec "$OBJ/inlet_parse_writer"
