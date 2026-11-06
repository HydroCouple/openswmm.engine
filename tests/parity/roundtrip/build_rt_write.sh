#!/bin/sh
# Build the round-trip driver against a chosen engine build.
#
# The driver must link the engine whose WRITER is under audit. build/darwin is
# the default because it carries the FP flags the bit-exact work depends on and
# is the tree's current Release build. Override with ENGINE_BUILD.
#
#   ./build_rt_write.sh                     # build/darwin
#   ENGINE_BUILD=../../../build/other ./build_rt_write.sh
#
# NOTE: the installed Python bindings are NOT a substitute — they are a
# snapshot and have lagged the source tree by two weeks.

set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
build=${ENGINE_BUILD:-"$root/build/darwin"}

# The soname is @rpath/libopenswmm.engine.6.dylib; only src/engine carries that
# symlink, bin/Release has just the versioned file.
lib="$build/src/engine/libopenswmm.engine.6.dylib"
[ -f "$lib" ] || lib="$build/bin/Release/libopenswmm.engine.6.dylib"
if [ ! -f "$lib" ]; then
    echo "error: no engine dylib under $build" >&2
    exit 1
fi
libdir=$(dirname "$lib")

echo "engine: $lib"
clang -O1 -o "$here/rt_write" "$here/rt_write.c" \
    -I"$root/include" "$lib" -Wl,-rpath,"$libdir"
echo "built:  $here/rt_write"
