#!/usr/bin/env bash
#
# fv_perf_phase0.sh -- the Phase 0 measurement of
# plans/FV1D_CLOSURE_KERNEL_PERF_PLAN_2026-09-11.md, as one detached job.
#
#   fv_perf_phase0.sh <engine-cli> <bench-fv-closure> <outroot> [label]
#
# Runs, in order, on what should be a QUIET host (check `uptime` and
# `ps aux | grep '[o]penswmm'` first -- a foreign engine run contaminated a
# prior East Boston benchmark, see memory fv-eastboston-perf-gap):
#
#   1. bench_fv_closure            per-op closure cost (ns/call) -> closure_bench.csv
#   2. fv_perf_baseline.py         Example1 + the six synthetic reaches, best-of-3
#   3. fv_perf_baseline.py         East Boston 77-submodel + TwinOaks v2, as
#                                  authored, dynwave/fv x THREADS 1/8, best-of-2
#   4. sample(1) profile           East Boston FV at THREADS 1, 30 s of samples
#
# Everything lands under <outroot>/<label>_<date>/ so it can be reviewed
# (CLAUDE.md 4.1). Nothing is deleted; re-running with a new label keeps the
# old results.
#
set -u

ENGINE=${1:?engine cli}
BENCH=${2:?bench_fv_closure}
OUTROOT=${3:?outroot}
LABEL=${4:-baseline}
TIMEOUT=${5:-3600}          # per engine run, seconds; TwinOaks FV at one thread
                            # needs more than the driver's 1800 s default
DATE=$(date +%Y-%m-%d)
OUT=$OUTROOT/${LABEL}_$DATE
HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd -- "$HERE/../../.." && pwd)

EB="$HOME/Downloads/Physically-Based-Model/East%20Boston%20Sewer%20Network_77_submodel_baseline.inp"
# TwinOaks v2 truncated to its first 4 h (END_TIME 04:00:00; the window the
# parallel cell loops were tuned on in d7c56274). The full 15 h deck costs
# ~30 min per FV run at one thread and timed out at 1800 s in the first pass.
# Generated from the authored deck by the Phase 0 round; no sidecar files.
TO="$OUTROOT/decks/TwinOaks_v2_4h.inp"

mkdir -p "$OUT"
LOG=$OUT/phase0.log
exec > >(tee -a "$LOG") 2>&1

echo "== fv_perf_phase0 $LABEL $DATE"
echo "engine:  $ENGINE"
echo "engine sha256: $(shasum -a 256 "$ENGINE" | cut -c1-16)  dylib: $(shasum -a 256 "$(dirname "$ENGINE")/libopenswmm.engine.6.0.0.dylib" 2>/dev/null | cut -c1-16)"
echo "timeout: $TIMEOUT s per run"
echo "commit:  $(git -C "$ROOT" rev-parse --short HEAD) (working tree may carry uncommitted peer edits -- see git status below)"
git -C "$ROOT" status --short -- src/ | head -20
echo "host:    $(uname -m) $(sysctl -n hw.ncpu 2>/dev/null) cpus; $(uptime)"
echo "foreign: $(ps aux | grep '[o]penswmm' | grep -v -E 'fv_perf|bench_fv|SWMMVis|claude' | wc -l | tr -d ' ') other engine processes"
date

echo; echo "== 1. bench_fv_closure"; date
"$BENCH" --samples 4096 > "$OUT/closure_bench.csv" 2> "$OUT/closure_bench.err"
grep -c , "$OUT/closure_bench.csv"

echo; echo "== 2. Example1 + synthetic reaches (best-of-3)"; date
python3 "$HERE/fv_perf_baseline.py" --engine "$ENGINE" --label "$LABEL" --repeat 3 \
    --timeout "$TIMEOUT" --outdir "$OUT/synthetic"

echo; echo "== 3. real decks as authored (best-of-2)"; date
python3 "$HERE/fv_perf_baseline.py" --engine "$ENGINE" --label "${LABEL}_real" --repeat 2 \
    --timeout "$TIMEOUT" --no-synthetics --no-example1 \
    --real-deck "$EB" --real-deck "$TO" \
    --outdir "$OUT/real"

echo; echo "== 4. sample profile, East Boston FV THREADS 1"; date
PROF=$OUT/profile
mkdir -p "$PROF"
EBDIR=$OUT/real/runs/$(basename "${EB%.inp}")/fv_t1
if [ -f "$EBDIR/$(basename "$EB")" ]; then
    ( cd "$EBDIR" && OMP_NUM_THREADS=1 "$ENGINE" "$(basename "$EB")" prof.rpt prof.out > "$PROF/engine.log" 2>&1 ) &
    PID=$!
    sleep 90
    # `sample` attaches to the engine, not the subshell; find the child.
    EPID=$(pgrep -P "$PID" -f openswmm | head -1)
    [ -n "$EPID" ] || EPID=$PID
    sample "$EPID" 30 -mayDie -file "$PROF/eastboston_fv_t1.sample.txt" || true
    wait "$PID"
    rm -f "$EBDIR/prof.out" "$EBDIR/prof.rpt"
    echo "profile: $PROF/eastboston_fv_t1.sample.txt"
else
    echo "no East Boston fv_t1 run dir -- profile skipped"
fi

echo; echo "== done"; date
