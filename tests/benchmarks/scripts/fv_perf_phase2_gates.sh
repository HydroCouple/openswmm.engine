#!/usr/bin/env bash
#
# fv_perf_phase2_gates.sh -- the Phase 2 (exact-geometry closure) gates of
# plans/FV1D_CLOSURE_KERNEL_PERF_PLAN_2026-09-11.md §2c, as one job.
#
#   fv_perf_phase2_gates.sh <engine-cli> <bench-fv-closure> <outroot> <frozen-phase0-dir> [label]
#
#   1. bench_fv_closure in exact and legacy mode (OPENSWMM_FV_CLOSURE)
#   2. SWASHES 1d-fv column, forced, then verdict/metric compare vs the
#      score file as it was before the run
#   3. transitions fv + fv-lts, same
#   4. mixed-flow lab cells e2_2006:C1/C2, e3_negative:C2, e4_aureli:C1/C2
#   5. synthetic sweep (Example1 + reaches) vs the frozen Phase 0 run:
#      hashes are EXPECTED to differ (Tier C); continuity, substeps, counters
#      and wall are what is read
#
# The engine passed in should be a FROZEN copy (see the memory note on
# shared-tree hazards): the analytic suites take an hour and other sessions
# relink build/darwin without warning.
#
set -u

ENGINE=${1:?engine cli}
BENCH=${2:?bench_fv_closure}
OUTROOT=${3:?outroot}
FROZEN=${4:?frozen phase0 dir}
LABEL=${5:-p2gates}
# FV_GATE_STEPS="345" runs only those steps (SWASHES alone is ~90 min on a
# loaded host; a kernel-only change needs the transitions/lab/sweep steps).
STEPS=${FV_GATE_STEPS:-12345}
want() { case "$STEPS" in *"$1"*) return 0 ;; *) return 1 ;; esac; }
DATE=$(date +%Y-%m-%d)
OUT=$OUTROOT/${LABEL}_$DATE
HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd -- "$HERE/../../.." && pwd)
QA=$HOME/Downloads/epaswmm5_qa
STUDY=$ROOT/studies/mixed_flow_closures

mkdir -p "$OUT"
LOG=$OUT/gates.log
exec > >(tee -a "$LOG") 2>&1

echo "== fv_perf_phase2_gates $LABEL $DATE"
echo "engine:  $ENGINE  sha256 $(shasum -a 256 "$ENGINE" | cut -c1-16)"
echo "dylib:   $(shasum -a 256 "$(dirname "$ENGINE")/libopenswmm.engine.6.0.0.dylib" | cut -c1-16)"
echo "commit:  $(git -C "$ROOT" rev-parse --short HEAD)"
echo "host:    $(uptime)"
date

if want 1; then
echo; echo "== 1. bench_fv_closure exact vs legacy"; date
OPENSWMM_FV_CLOSURE=exact  "$BENCH" --samples 4096 > "$OUT/closure_bench_exact.csv"  2> "$OUT/closure_bench_exact.err"
OPENSWMM_FV_CLOSURE=legacy "$BENCH" --samples 4096 > "$OUT/closure_bench_legacy.csv" 2> "$OUT/closure_bench_legacy.err"
python3 - "$OUT" <<'EOF'
import csv, sys
out = sys.argv[1]
def load(p):
    d = {}
    for r in csv.DictReader(l for l in open(p) if not l.startswith('#')):
        d[(r['shape'], r['op'])] = float(r['ns_med'])
    return d
e, l = load(f"{out}/closure_bench_exact.csv"), load(f"{out}/closure_bench_legacy.csv")
print("| shape | op | legacy ns | exact ns | speedup |")
print("|---|---|---:|---:|---:|")
for k in sorted(l):
    if k in e:
        print(f"| {k[0]} | {k[1]} | {l[k]:.1f} | {e[k]:.1f} | {l[k]/e[k] if e[k] > 0 else 0:.2f} |")
for k in sorted(e):
    if k not in l:
        print(f"| {k[0]} | {k[1]} | — | {e[k]:.1f} | — |")
EOF
fi

if want 2; then
echo; echo "== 2. SWASHES 1d-fv"; date
cp "$QA/suites/swashes/swashes_scores.json" "$OUT/swashes_scores_before.json"
( cd "$QA" && OPENSWMM_EXE="$ENGINE" conda run -n openswmm python run_regression.py --suite swashes -- --solvers 1d-fv --force ) > "$OUT/swashes_run.log" 2>&1
echo "swashes run exit: $?"
cp "$QA/suites/swashes/swashes_scores.json" "$OUT/swashes_scores_after.json"
python3 "$HERE/fv_perf_compare_scores.py" "$OUT/swashes_scores_before.json" "$OUT/swashes_scores_after.json" \
    --solvers 1d-fv --md "$OUT/SWASHES_COMPARE.md"
echo "swashes compare exit: $?"
fi

if want 3; then
echo; echo "== 3. transitions fv, fv-lts"; date
cp "$QA/suites/transitions/transitions_scores.json" "$OUT/transitions_scores_before.json"
( cd "$QA" && OPENSWMM_EXE="$ENGINE" conda run -n openswmm python run_regression.py --suite transitions -- --solvers fv,fv-lts --force ) > "$OUT/transitions_run.log" 2>&1
echo "transitions run exit: $?"
cp "$QA/suites/transitions/transitions_scores.json" "$OUT/transitions_scores_after.json"
python3 "$HERE/fv_perf_compare_scores.py" "$OUT/transitions_scores_before.json" "$OUT/transitions_scores_after.json" \
    --solvers fv,fv-lts --md "$OUT/TRANSITIONS_COMPARE.md"
echo "transitions compare exit: $?"
fi

if want 4; then
echo; echo "== 4. lab cells"; date
cp "$STUDY/results/summary.csv" "$OUT/lab_summary_before.csv" 2>/dev/null || true
( cd "$STUDY" && conda run -n openswmm python scripts/run_matrix.py --engine-bin "$ENGINE" \
      --only e2_2006:C1,e2_2006:C2,e3_negative:C2,e4_aureli:C1,e4_aureli:C2 --keep-going ) > "$OUT/lab_run.log" 2>&1
echo "lab run exit: $?"
cp "$STUDY/results/summary.csv" "$OUT/lab_summary_after.csv"
python3 - "$OUT" <<'EOF'
import csv, sys
out = sys.argv[1]
def load(p):
    d = {}
    try:
        for r in csv.DictReader(open(p)):
            if r['nse']:
                d[(r['experiment'], r['column'], r['series'])] = float(r['nse'])
    except FileNotFoundError:
        pass
    return d
b, a = load(f"{out}/lab_summary_before.csv"), load(f"{out}/lab_summary_after.csv")
print("| experiment | column | series | NSE before | NSE after | delta |")
print("|---|---|---|---:|---:|---:|")
for k in sorted(a):
    if k[1] in ('C1', 'C2'):
        bb = b.get(k)
        print(f"| {k[0]} | {k[1]} | {k[2]} | {'—' if bb is None else f'{bb:.4f}'} | {a[k]:.4f} | "
              f"{'—' if bb is None else f'{a[k]-bb:+.4f}'} |")
EOF
fi

if want 5; then
echo; echo "== 5. synthetic sweep vs frozen Phase 0"; date
python3 "$HERE/fv_perf_baseline.py" --engine "$ENGINE" --label "$LABEL" --repeat 2 --timeout 3600 \
    --outdir "$OUT/synthetic"
python3 "$HERE/fv_perf_compare.py" "$FROZEN/synthetic/results.json" "$OUT/synthetic/results.json" \
    --md "$OUT/SYNTHETIC_COMPARE_vs_frozen.md" || echo "(hash differences are expected for Tier C)"
fi

echo; echo "== done"; date
