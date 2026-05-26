#!/usr/bin/env bash
# ============================================================================
# Build & run the GeoPackage C-API verification harness.
#
# Standalone — does NOT depend on the engine's vcpkg toolchain or CMake.
# Compiles only the two TUs needed by swmm_gpkg_open and friends, links
# against the system libsqlite3, and runs ./verify_geopackage_c_api.
#
# Usage:  bash verify_geopackage_c_api.sh
#
# Requirements: g++ (or clang++), system libsqlite3 (apt: libsqlite3-dev, or
# brew: sqlite). On Linux without -dev, we fall back to linking the .so.0
# directly.
# ============================================================================
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"

# Header search paths
INCLUDES=(
    "-I" "$REPO/include"
    "-I" "$REPO/src/engine/input/geopackage"
)

# vcpkg-installed sqlite3 header (host-os triplet)
for triplet in arm64-osx x64-osx arm64-linux x64-linux x64-windows; do
    h="$REPO/build/local/vcpkg_installed/$triplet/include"
    if [[ -f "$h/sqlite3.h" ]]; then
        INCLUDES+=("-I" "$h")
        echo "Using sqlite3 headers from $h"
        break
    fi
done

# Build the two needed TUs as PIC objects.
WORKDIR="$(mktemp -d)"
trap "rm -rf '$WORKDIR'" EXIT
cd "$WORKDIR"

for src in \
    "$REPO/src/engine/input/geopackage/openswmm_geopackage_impl.cpp" \
    "$REPO/src/engine/input/geopackage/GeoPackageSchema.cpp"
do
    g++ -c -std=c++17 -fPIC -O0 -g \
        -DSWMM_ENGINE_API='__attribute__((visibility("default")))' \
        "${INCLUDES[@]}" \
        "$src" -o "$(basename "$src" .cpp).o"
done

# Pick a libsqlite3 to link against.
SQLITE_LIB=""
for cand in \
    "/usr/lib/x86_64-linux-gnu/libsqlite3.so" \
    "/lib/aarch64-linux-gnu/libsqlite3.so.0" \
    "/lib/x86_64-linux-gnu/libsqlite3.so.0" \
    "/usr/lib/libsqlite3.so" \
    "/opt/homebrew/lib/libsqlite3.dylib" \
    "/usr/lib/libsqlite3.dylib"
do
    if [[ -f "$cand" ]]; then SQLITE_LIB="$cand"; break; fi
done

if [[ -z "$SQLITE_LIB" ]]; then
    # Last resort: let the linker resolve it
    SQLITE_LIB="-lsqlite3"
fi
echo "Linking against $SQLITE_LIB"

# Link: ignore unresolved symbols from the registration code path
# (GeoPackagePluginInfo::instance) — the harness never calls them.
# -Wl,--unresolved-symbols=ignore-all is GNU ld; on macOS clang use the
# equivalent -Wl,-undefined,dynamic_lookup.
LINKFLAGS=()
if [[ "$(uname -s)" == "Darwin" ]]; then
    LINKFLAGS+=("-Wl,-undefined,dynamic_lookup")
else
    LINKFLAGS+=("-Wl,--unresolved-symbols=ignore-all")
fi

g++ -std=c++17 -O0 -g \
    "${INCLUDES[@]}" \
    "$HERE/verify_geopackage_c_api.cpp" \
    openswmm_geopackage_impl.o GeoPackageSchema.o \
    "$SQLITE_LIB" -lpthread -ldl \
    "${LINKFLAGS[@]}" \
    -o verify_geopackage_c_api

echo
./verify_geopackage_c_api
