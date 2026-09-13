#!/usr/bin/env bash
#
# verify_worker_vs_cli.sh — the SWMMVis worker must not change the numbers.
#
# Runs openswmm-legacy-worker-5.2.4 with tickIntervalMs=0 (so the running
# continuity getter is called on EVERY step) and runswmm over the same decks,
# and asserts .out byte-identical and .rpt identical modulo banner/timestamps.
#
# This is the regression gate for swmm_getRunningMassBalErr being PURE: the
# end-of-run getters it mirrors have side effects on the per-node continuity
# column of the .rpt (massbal_getStorage(TRUE) adds to NodeOutflow[]), and
# calling them mid-run would show up here as a .rpt diff.
#
# Usage:
#   tools/verify_worker_vs_cli.sh [--build <dir>] [--models <dir>] [--max <n>]
#
# Defaults:
#   --build    build/darwin-tests (must contain runswmm + the worker)
#   --models   ../../openswmm.engine.benchmarks/corpus/epa (also ../ variant),
#              else ../openswmm.engine/examples; <case>/model.inp or flat .inp
#   --max      12 decks (sorted order); 0 = all
#
# Outputs (reviewable, never temp — CLAUDE.md §4.1):
#   verification/worker_vs_cli/{cli,worker}/<name>.{rpt,out}, report.txt

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$REPO_ROOT/build/darwin-tests"
MODELS_DIR=""
MAX=12

while [[ $# -gt 0 ]]; do
    case "$1" in
        --build)  BUILD_DIR="$2"; shift 2 ;;
        --models) MODELS_DIR="$2"; shift 2 ;;
        --max)    MAX="$2"; shift 2 ;;
        *) echo "unknown arg: $1" >&2; exit 2 ;;
    esac
done

if [[ -z "$MODELS_DIR" ]]; then
    for candidate in \
        "$REPO_ROOT/../openswmm.engine.benchmarks/corpus/epa" \
        "$REPO_ROOT/../../openswmm.engine.benchmarks/corpus/epa" \
        "$REPO_ROOT/../openswmm.engine/examples"
    do
        if [[ -d "$candidate" ]]; then MODELS_DIR="$(cd "$candidate" && pwd)"; break; fi
    done
fi
[[ -n "$MODELS_DIR" && -d "$MODELS_DIR" ]] || { echo "ERROR: no models dir; pass --models" >&2; exit 1; }

CLI="$(find "$BUILD_DIR" -type f -name runswmm | head -1)"
WORKER="$(find "$BUILD_DIR" -type f \( -name 'openswmm-legacy-worker-5.2.4' -o -name 'openswmm-legacy-worker-5.2.4.exe' \) | head -1)"
[[ -n "$CLI" && -n "$WORKER" ]] || { echo "ERROR: runswmm/worker not found under $BUILD_DIR" >&2; exit 1; }

OUT_ROOT="$REPO_ROOT/verification/worker_vs_cli"
rm -rf "$OUT_ROOT"; mkdir -p "$OUT_ROOT/cli" "$OUT_ROOT/worker"
REPORT="$OUT_ROOT/report.txt"; : > "$REPORT"

echo "==> CLI:    $CLI"
echo "==> Worker: $WORKER"
echo "==> Models: $MODELS_DIR (max $MAX)"

strip() { grep -viE '^\s*(SWMM|Version|EPA|STORM|WATER|Analysis begun|Analysis ended|Total elapsed time)' "$1"; }

pass=0; fail=0; skip=0; n=0
while IFS= read -r inp; do
    inp="$(cd "$(dirname "$inp")" && pwd)/$(basename "$inp")"
    name="$(basename "${inp%.*}")"
    [[ "$name" == "model" ]] && name="$(basename "$(dirname "$inp")")"
    n=$((n+1)); [[ "$MAX" -gt 0 && $n -gt $MAX ]] && break

    c_rpt="$OUT_ROOT/cli/$name.rpt";    c_out="$OUT_ROOT/cli/$name.out"
    w_rpt="$OUT_ROOT/worker/$name.rpt"; w_out="$OUT_ROOT/worker/$name.out"
    ( cd "$(dirname "$inp")" && "$CLI" "$inp" "$c_rpt" "$c_out" ) >/dev/null 2>&1 || true
    ( cd "$(dirname "$inp")" && "$WORKER" "$inp" "$w_rpt" "$w_out" 0 ) > "$OUT_ROOT/worker/$name.stdout" 2>&1 || true

    if [[ ! -s "$c_out" || ! -s "$w_out" ]]; then
        echo "SKIP  $name  (no .out from one side)" | tee -a "$REPORT"; skip=$((skip+1)); continue
    fi
    if ! cmp -s "$c_out" "$w_out"; then
        echo "FAIL  $name  (.out differs)" | tee -a "$REPORT"; fail=$((fail+1)); continue
    fi
    if ! diff -q <(strip "$c_rpt") <(strip "$w_rpt") >/dev/null; then
        echo "FAIL  $name  (.rpt differs beyond banner — running getter has a side effect?)" | tee -a "$REPORT"; fail=$((fail+1)); continue
    fi
    ticks="$(grep -c '"type":"progress"' "$OUT_ROOT/worker/$name.stdout" || true)"
    echo "PASS  $name  ($ticks progress ticks)" | tee -a "$REPORT"; pass=$((pass+1))
done < <(find "$MODELS_DIR" -maxdepth 2 -type f -name '*.inp' | sort)

echo; echo "==> $pass passed, $fail failed, $skip skipped"; echo "==> Report: $REPORT"
[[ $fail -eq 0 ]]
