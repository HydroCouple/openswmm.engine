#!/usr/bin/env bash
# ============================================================================
# Build libomp (LLVM OpenMP runtime) from source for a low macOS deployment
# target, so it can be bundled into wheels / engine artifacts whose minimum
# target is below what the current Homebrew libomp bottle supports.
#
# Why: Apple clang ships no libomp. `brew install libomp` on the macos-15
# runners yields a bottle with min-target 15.0, and delocate refuses to bundle
# a dylib whose minimum exceeds the wheel's MACOSX_DEPLOYMENT_TARGET. To ship
# wheels for macOS 11+, we build libomp ourselves at the requested target and
# install it into the Homebrew opt prefix that cmake/FindOpenMP.cmake probes
# (/opt/homebrew/opt/libomp on Apple Silicon, /usr/local/opt/libomp on Intel).
#
# Env:
#   MACOSX_DEPLOYMENT_TARGET  minimum macOS version (default 11.0)
#   LLVM_OPENMP_VERSION       LLVM release to build (default 20.1.8)
#
# The default must stay >= 20.x: AppleClang 17 (on the macos-15 runners since
# 2026-08) emits __kmpc_dispatch_deinit at the end of schedule(dynamic/guided)
# loops (e.g. ExplicitFvSolver::updateCells), and that runtime entry point
# first appeared in the LLVM 20 OpenMP runtime (present in 20.1.0's dllexports,
# absent through 19.1.7) — linking an older libomp fails with
# "Undefined symbols: ___kmpc_dispatch_deinit". The post-install check below
# asserts the built dylib actually exports it.
#
# NOTE: this runs in CI on macOS runners; it needs cmake + ninja on PATH
# (the caller's `brew install ninja` covers ninja; runners ship cmake).
#
# ⚠ Requires a real CI run to validate: standalone libomp builds occasionally
#   need a version/flag tweak across LLVM releases. Pin LLVM_OPENMP_VERSION if a
#   release regresses the standalone build.
# ============================================================================
set -euo pipefail

TARGET="${MACOSX_DEPLOYMENT_TARGET:-11.0}"
LLVM_VER="${LLVM_OPENMP_VERSION:-20.1.8}"

case "$(uname -m)" in
  arm64)  ARCH=arm64;  PREFIX=/opt/homebrew/opt/libomp ;;
  x86_64) ARCH=x86_64; PREFIX=/usr/local/opt/libomp ;;
  *) echo "unsupported macOS arch: $(uname -m)" >&2; exit 1 ;;
esac

echo ">>> building libomp ${LLVM_VER} for ${ARCH} @ macOS ${TARGET} -> ${PREFIX}"

work="$(mktemp -d)"
trap 'rm -rf "${work}"' EXIT
cd "${work}"

base="https://github.com/llvm/llvm-project/releases/download/llvmorg-${LLVM_VER}"
curl -fLsS -o openmp.src.tar.xz "${base}/openmp-${LLVM_VER}.src.tar.xz"
curl -fLsS -o cmake.src.tar.xz  "${base}/cmake-${LLVM_VER}.src.tar.xz"
tar xf openmp.src.tar.xz
tar xf cmake.src.tar.xz
# The standalone openmp build references ../cmake/Modules from the matching
# LLVM cmake utilities; provide them under the expected sibling name.
mv "cmake-${LLVM_VER}.src" cmake

cmake -S "openmp-${LLVM_VER}.src" -B build-omp -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_DEPLOYMENT_TARGET="${TARGET}" \
  -DCMAKE_OSX_ARCHITECTURES="${ARCH}" \
  -DCMAKE_INSTALL_PREFIX="${PREFIX}" \
  -DLIBOMP_INSTALL_ALIASES=OFF \
  -DOPENMP_ENABLE_LIBOMPTARGET=OFF

cmake --build build-omp
mkdir -p "${PREFIX}"
# Drop stale artifacts from earlier installs (e.g. an old libomp.a — this
# script only ships the dylib) so nothing outdated can be picked up.
rm -f "${PREFIX}/lib/libomp.a"
cmake --install build-omp

echo ">>> installed:"
ls -la "${PREFIX}/lib"
# Confirm the produced dylib's minimum matches the requested target.
otool -l "${PREFIX}/lib/libomp.dylib" | grep -A3 LC_BUILD_VERSION || true
# Fail fast if the runtime predates the compiler's dispatch-loop codegen
# (see the LLVM_OPENMP_VERSION note above) — a missing symbol here otherwise
# surfaces much later as a cryptic engine link error.
if ! nm -g "${PREFIX}/lib/libomp.dylib" | grep -q "___kmpc_dispatch_deinit"; then
  echo "ERROR: libomp ${LLVM_VER} lacks ___kmpc_dispatch_deinit (need LLVM >= 20)" >&2
  exit 1
fi
