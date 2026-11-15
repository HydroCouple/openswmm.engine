#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# One timing screen for the runoff scaling study (gen_load_bench.py roofs_N /
# mixed_N decks). Runs a CLI on a deck REPS times with OPENSWMM_PERF=1 and
# /usr/bin/time -l, and prints one line per run:
#
#   label deck threads rep wall_s peak_footprint_MB runoff_s n_runoff
#
# Usage: run_runoff_screen.sh <cli> <deck.inp> <outdir> <label> [threads] [reps]
#
# THREADS is written into a copy of the deck's [OPTIONS] (1 = serial, the
# engine default). The .rpt/.out of the last rep are kept; earlier reps'
# outputs are overwritten. The legacy CLI has no OPENSWMM_PERF timers, so its
# runoff columns print "-".
set -euo pipefail

cli=$1 deck=$2 outdir=$3 label=$4 threads=${5:-1} reps=${6:-3}
mkdir -p "$outdir"
base=$(basename "$deck" .inp)
run_deck="$outdir/${base}_t${threads}.inp"
awk -v t="$threads" '{print} /^\[OPTIONS\]/{print "THREADS              " t}' "$deck" > "$run_deck"

for rep in $(seq 1 "$reps"); do
    stem="$outdir/${base}_${label}_t${threads}"
    log="$stem.r$rep.log"
    OPENSWMM_PERF=1 /usr/bin/time -l "$cli" "$run_deck" "$stem.rpt" "$stem.out" > "$log" 2>&1 || {
        echo "$label $base $threads $rep FAILED (see $log)"; continue; }
    wall=$(awk '/ real /{print $1; exit}' "$log")
    foot=$(awk '/peak memory footprint/{printf "%.0f", $1/1048576; exit}' "$log")
    hyd=$(grep -m1 'PERF-HYD' "$log" || true)
    runoff=$(sed -n 's/.* runoff=\([0-9.eE+-]*\).*/\1/p' <<<"$hyd")
    nrun=$(sed -n 's/.* n\.runoff=\([0-9]*\).*/\1/p' <<<"$hyd")
    echo "$label $base $threads $rep ${wall:--} ${foot:--} ${runoff:--} ${nrun:--}"
done
rm -f "$run_deck"
