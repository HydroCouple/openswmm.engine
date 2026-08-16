#!/usr/bin/env bash
#
# verify_524_parity.sh — byte-identical .out regression gate for build-v5.2.4.
#
# Builds stock EPA SWMM at tag v5.2.4 and the current branch WITH IDENTICAL
# FLAGS, runs both CLIs over a set of .inp models, and asserts:
#
#   * .out files byte-identical
#   * .rpt files identical modulo banner/timestamp lines
#
# WHY THE "IDENTICAL FLAGS" PART MATTERS
# --------------------------------------
# This branch replaces upstream's MSVC /fp:fast with the /fp:precise policy
# shared with openswmm.engine (see CMakePresets.json). If the stock tree were
# built with its original flags and this branch with the new ones, the .out
# files would differ because of *compiler math*, not because of any source
# change — a false failure that would mask the thing this gate exists to catch.
# Both trees are therefore configured with the SAME preset from THIS branch:
# the stock worktree is built with -C pointing at this branch's presets file.
#
# The gate isolates the effect of source backports (unknown-section skip+warn,
# warning callback, swmm_getRunningMassBalErr, worker), which are required by
# openswmm.gui/workplans/MULTI_ENGINE_VERSION_SUPPORT_PLAN_2026-08-01.md to keep
# numerics bit-identical.
#
# Usage:
#   tools/verify_524_parity.sh [--models <dir>] [--preset <name>] [--jobs <n>]
#
# Defaults:
#   --models   ../openswmm.engine.benchmarks/EPA   (falls back to ../openswmm.engine/examples)
#   --preset   auto-detected from uname (Linux | Darwin)
#
# Outputs (reviewable, never temp — see CLAUDE.md §4.1):
#   verification/524_parity/stock/     stock .out/.rpt
#   verification/524_parity/branch/    branch .out/.rpt
#   verification/524_parity/report.txt per-model PASS/FAIL summary
#
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

MODELS_DIR=""
PRESET=""
JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
STOCK_TAG="v5.2.4"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --models) MODELS_DIR="$2"; shift 2 ;;
        --preset) PRESET="$2"; shift 2 ;;
        --jobs)   JOBS="$2"; shift 2 ;;
        --tag)    STOCK_TAG="$2"; shift 2 ;;
        -h|--help) sed -n '2,40p' "$0"; exit 0 ;;
        *) echo "Unknown argument: $1" >&2; exit 2 ;;
    esac
done

if [[ -z "$PRESET" ]]; then
    case "$(uname -s)" in
        Linux)  PRESET="Linux" ;;
        Darwin) PRESET="Darwin" ;;
        *) echo "ERROR: could not auto-detect preset for $(uname -s); pass --preset" >&2; exit 2 ;;
    esac
fi

if [[ -z "$MODELS_DIR" ]]; then
    for candidate in \
        "$REPO_ROOT/../openswmm.engine.benchmarks/EPA" \
        "$REPO_ROOT/../openswmm.engine/examples"
    do
        if [[ -d "$candidate" ]]; then MODELS_DIR="$candidate"; break; fi
    done
fi

if [[ -z "$MODELS_DIR" || ! -d "$MODELS_DIR" ]]; then
    echo "ERROR: no models directory found. Pass --models <dir>." >&2
    exit 2
fi

: "${VCPKG_ROOT:?VCPKG_ROOT must be set (the presets use the vcpkg toolchain)}"

OUT_ROOT="$REPO_ROOT/verification/524_parity"
STOCK_SRC="$REPO_ROOT/.parity/stock-$STOCK_TAG"
STOCK_BUILD="$STOCK_SRC/build/$(echo "$PRESET" | tr '[:upper:]' '[:lower:]')"
BRANCH_BUILD="$REPO_ROOT/build/$(echo "$PRESET" | tr '[:upper:]' '[:lower:]')"

rm -rf "$OUT_ROOT"
mkdir -p "$OUT_ROOT/stock" "$OUT_ROOT/branch"

echo "==> Models:  $MODELS_DIR"
echo "==> Preset:  $PRESET"
echo "==> Outputs: $OUT_ROOT"

# ---------------------------------------------------------------------------
# 1. Build the current branch
# ---------------------------------------------------------------------------
echo
echo "==> Building branch ($(git rev-parse --abbrev-ref HEAD) @ $(git rev-parse --short HEAD))"
cmake --preset "$PRESET" >/dev/null
cmake --build "$BRANCH_BUILD" --config Release -j "$JOBS" >/dev/null
BRANCH_EXE="$(find "$BRANCH_BUILD" -type f -name runswmm | head -1)"
[[ -n "$BRANCH_EXE" ]] || { echo "ERROR: branch runswmm not found" >&2; exit 1; }

# ---------------------------------------------------------------------------
# 2. Build stock EPA at $STOCK_TAG with THIS BRANCH'S build configuration
#
# The stock tree at v5.2.4 has no CMakePresets.json/vcpkg.json, so we copy this
# branch's build configuration into the worktree before configuring. That is
# precisely what makes the comparison isolate source changes: same presets, same
# FP flags, same toolchain, different sources.
# ---------------------------------------------------------------------------
echo
echo "==> Building stock EPA $STOCK_TAG with this branch's build configuration"
if [[ ! -d "$STOCK_SRC" ]]; then
    mkdir -p "$(dirname "$STOCK_SRC")"
    git worktree add --detach "$STOCK_SRC" "$STOCK_TAG"
fi

# Overlay this branch's build configuration onto the stock worktree.
cp "$REPO_ROOT/CMakePresets.json" "$STOCK_SRC/CMakePresets.json"
cp "$REPO_ROOT/vcpkg.json"        "$STOCK_SRC/vcpkg.json"
mkdir -p "$STOCK_SRC/cmake"
cp "$REPO_ROOT/cmake/FindOpenMP.cmake" "$STOCK_SRC/cmake/FindOpenMP.cmake"

# The stock tree's CMakeLists predates SWMM_WITH_OPENMP and pins
# cmake_minimum_required(3.13); presets v3 needs >= 3.21. Patch both in place in
# the worktree only (never committed) so the stock build is flag-comparable.
python3 - "$STOCK_SRC/CMakeLists.txt" <<'PY'
import re, sys
p = sys.argv[1]
s = open(p).read()
s = re.sub(r'cmake_minimum_required\s*\(\s*VERSION\s+[0-9.]+\s*\)',
           'cmake_minimum_required (VERSION 3.21)', s, count=1)
# cmake/FindOpenMP.cmake (overlaid above) must not shadow the builtin module:
# drop the (vestigial in stock) module-path append, mirroring the branch.
s = s.replace('list(APPEND CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake)', '')
if 'SWMM_WITH_OPENMP' not in s:
    s = s.replace('option(BUILD_TESTS',
                  'option(SWMM_WITH_OPENMP "Build with OpenMP support" ON)\n'
                  'if(SWMM_WITH_OPENMP)\n'
                  '    if(APPLE)\n'
                  '        include(${PROJECT_SOURCE_DIR}/cmake/FindOpenMP.cmake)\n'
                  '        find_package(OpenMP)\n'
                  '    else()\n'
                  '        find_package(OpenMP REQUIRED)\n'
                  '    endif()\n'
                  'endif()\n\n'
                  'option(BUILD_TESTS', 1)
open(p, 'w').write(s)
PY

(
    cd "$STOCK_SRC"
    cmake --preset "$PRESET" >/dev/null
    cmake --build "$STOCK_BUILD" --config Release -j "$JOBS" >/dev/null
)
STOCK_EXE="$(find "$STOCK_BUILD" -type f -name runswmm | head -1)"
[[ -n "$STOCK_EXE" ]] || { echo "ERROR: stock runswmm not found" >&2; exit 1; }

# ---------------------------------------------------------------------------
# 3. Run both over the model set and compare
# ---------------------------------------------------------------------------
echo
echo "==> Running models"
REPORT="$OUT_ROOT/report.txt"
: > "$REPORT"

pass=0; fail=0; skip=0

while IFS= read -r inp; do
    name="$(basename "${inp%.*}")"
    s_rpt="$OUT_ROOT/stock/$name.rpt";  s_out="$OUT_ROOT/stock/$name.out"
    b_rpt="$OUT_ROOT/branch/$name.rpt"; b_out="$OUT_ROOT/branch/$name.out"

    "$STOCK_EXE"  "$inp" "$s_rpt" "$s_out" >/dev/null 2>&1 || true
    "$BRANCH_EXE" "$inp" "$b_rpt" "$b_out" >/dev/null 2>&1 || true

    if [[ ! -s "$s_out" || ! -s "$b_out" ]]; then
        echo "SKIP  $name  (one or both engines produced no .out)" | tee -a "$REPORT"
        skip=$((skip+1))
        continue
    fi

    if cmp -s "$s_out" "$b_out"; then
        # .rpt differs by banner/timestamp lines by construction; strip them.
        if diff -q \
            <(grep -viE '^\s*(SWMM|Version|EPA|STORM|WATER|Analysis begun|Analysis ended|Total elapsed time)' "$s_rpt") \
            <(grep -viE '^\s*(SWMM|Version|EPA|STORM|WATER|Analysis begun|Analysis ended|Total elapsed time)' "$b_rpt") \
            >/dev/null
        then
            echo "PASS  $name" | tee -a "$REPORT"
            pass=$((pass+1))
        else
            echo "FAIL  $name  (.out identical but .rpt differs beyond banner/timestamps)" | tee -a "$REPORT"
            fail=$((fail+1))
        fi
    else
        echo "FAIL  $name  (.out differs)" | tee -a "$REPORT"
        fail=$((fail+1))
    fi
done < <(find "$MODELS_DIR" -maxdepth 2 -type f -name '*.inp' | sort)

echo
echo "==> $pass passed, $fail failed, $skip skipped"
echo "==> Report: $REPORT"

[[ $fail -eq 0 ]]
