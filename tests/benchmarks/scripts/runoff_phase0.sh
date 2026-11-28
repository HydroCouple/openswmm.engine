#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Phase 0 driver of the runoff / infiltration / ODE / 2D subsurface profiling
# round (2026-11-26). Four passes, engine runs never overlapping:
#
#   identity  the tip CLI, the plain candidate CLI and the profile CLI (with
#             OPENSWMM_PERF unset) on every deck; the .out files must match
#             byte for byte past the 8-byte header (the version field).
#   legacy    one legacy-CLI run per 1D deck: a reference wall time.
#   timing    the plain CLI under OPENSWMM_PERF=1 and /usr/bin/time -l, REPS
#             times per deck and thread count; wall, peak footprint and the
#             [PERF-*] lines (section self times) are kept.
#   counters  one profile-CLI run per deck, one thread, OPENSWMM_PERF=1, for
#             the [PERF-PROCESS-COUNTS] event counters.
#
# Usage: runoff_phase0.sh <plain-cli> <profile-cli> <tip-cli> <legacy-cli>
#                         <deck-dir> <work-dir> <results-dir> [reps] [pass ...]
#
# <deck-dir> holds the gen_load_bench.py decks and the gen_runoff_phase0_2d.py
# decks. Runs go to <work-dir>; the summaries and perf lines are copied to
# <results-dir> at the end. With pass names given, only those passes run.
set -uo pipefail

plain=$1 profile=$2 tip=$3 legacy=$4 decks=$5 work=$6 results=$7 reps=${8:-3}
shift 7; [ $# -gt 0 ] && shift
passes=${*:-"identity legacy timing counters"}
mkdir -p "$work/runs" "$work/perf" "$work/counters" "$results"

# 1D decks: 20k for every variant, 200k for the thread sweep.
ONE_D_SMALL="roofs_20k mixed_20k perv_horton_20k perv_modhorton_20k perv_grnampt_20k perv_modgrnampt_20k perv_cn_20k half_horton_20k gw_horton_20k snow_20k"
ONE_D_BIG="roofs_200k mixed_200k perv_horton_200k perv_grnampt_200k"
BIG_THREADS="1 2 4 8"
# 2D decks: whatever gen_runoff_phase0_2d.py produced. The aquifer decks run
# minutes each even at 10 simulated minutes, so they get one rep, and only the
# Russo σ column is also run on 4 threads; the identity pass takes one of them.
TWO_D=$(cd "$decks" && ls bench2d_*.inp 2>/dev/null | sed 's/\.inp$//' | sort)
TWO_D_IDENTITY=""
for d in $TWO_D; do
    if [[ $d != *_aq_* || $d == bench2d_19602_aq_sigma_russo ]]; then
        TWO_D_IDENTITY="$TWO_D_IDENTITY $d"
    fi
done
two_d_threads() { if [[ $1 == *_aq_* && $1 != *_aq_sigma_russo ]]; then echo "1"; else echo "1 4"; fi; }
two_d_reps() { if [[ $1 == *_aq_* ]]; then echo 1; else echo "$reps"; fi; }

# with_threads <deck.inp> <threads> <out.inp>: set [OPTIONS] THREADS.
with_threads() {
    awk -v t="$2" '
        /^THREADS[ \t]/ { print "THREADS              " t; seen = 1; next }
        { print }
        /^\[OPTIONS\]/ && !seen { print "THREADS              " t; seen = 1 }' "$1" > "$3"
}

# run_one <cli> <deck.inp> <stem> <perf 0|1>: writes <stem>.rpt/.out/.log;
# echoes "wall peak_MB" (or "- -") and returns the CLI's status.
run_one() {
    local cli=$1 deck=$2 stem=$3 perf=$4 status
    if [ "$perf" = 1 ]; then
        OPENSWMM_PERF=1 /usr/bin/time -l "$cli" "$deck" "$stem.rpt" "$stem.out" > "$stem.log" 2>&1
    else
        /usr/bin/time -l "$cli" "$deck" "$stem.rpt" "$stem.out" > "$stem.log" 2>&1
    fi
    status=$?
    local wall foot
    wall=$(awk '/ real /{print $1; exit}' "$stem.log")
    foot=$(awk '/peak memory footprint/{printf "%.0f", $1/1048576; exit}' "$stem.log")
    echo "${wall:--} ${foot:--}"
    return $status
}

deck_path() { echo "$decks/$1.inp"; }
have_deck() { [ -f "$(deck_path "$1")" ]; }

# ---------------------------------------------------------------- identity --
if [[ " $passes " == *" identity "* ]]; then
    : > "$work/identity.txt"
    for d in $ONE_D_SMALL $TWO_D_IDENTITY; do
        have_deck "$d" || { echo "$d: no deck" >> "$work/identity.txt"; continue; }
        deck=$(deck_path "$d")
        ok=1
        for label in tip plain profile; do
            case $label in tip) cli=$tip;; plain) cli=$plain;; profile) cli=$profile;; esac
            run_one "$cli" "$deck" "$work/runs/${d}_id_$label" 0 > /dev/null ||
                { echo "$d: $label run FAILED (see $work/runs/${d}_id_$label.log)" >> "$work/identity.txt"; ok=0; }
        done
        [ $ok = 1 ] || continue
        ref="$work/runs/${d}_id_tip.out"
        for label in plain profile; do
            out="$work/runs/${d}_id_$label.out"
            if [ "$(stat -f %z "$ref")" = "$(stat -f %z "$out")" ] && cmp -s -i 8 "$ref" "$out"; then
                echo "$d: $label IDENTICAL to tip" >> "$work/identity.txt"
            else
                echo "$d: $label DIFFERS from tip ($(cmp -i 8 "$ref" "$out" 2>&1 | head -1))" >> "$work/identity.txt"
            fi
        done
        rm -f "$work/runs/${d}_id_"*.out
    done
    cat "$work/identity.txt"
fi

# ------------------------------------------------------------------ legacy --
if [[ " $passes " == *" legacy "* ]]; then
    echo -e "deck\twall_s\tpeak_MB\tstatus" > "$work/legacy.tsv"
    for d in $ONE_D_SMALL $ONE_D_BIG; do
        have_deck "$d" || continue
        res=$(run_one "$legacy" "$(deck_path "$d")" "$work/runs/${d}_legacy" 0); st=$?
        echo -e "$d\t${res// /\\t}\t$st" >> "$work/legacy.tsv"
        rm -f "$work/runs/${d}_legacy.out"
    done
    cat "$work/legacy.tsv"
fi

# ------------------------------------------------------------------ timing --
if [[ " $passes " == *" timing "* ]]; then
    echo -e "deck\tthreads\trep\twall_s\tpeak_MB\tstatus" > "$work/timing.tsv"
    time_deck() {  # <deck> <reps> <threads...>
        local d=$1 n=$2; shift 2
        have_deck "$d" || return 0
        for t in "$@"; do
            local run_deck="$work/runs/${d}_t$t.inp"
            with_threads "$(deck_path "$d")" "$t" "$run_deck"
            for rep in $(seq 1 "$n"); do
                local stem="$work/runs/${d}_t${t}_r$rep"
                res=$(run_one "$plain" "$run_deck" "$stem" 1); st=$?
                echo -e "$d\t$t\t$rep\t${res// /\\t}\t$st" >> "$work/timing.tsv"
                grep -E '^\[PERF' "$stem.log" > "$work/perf/${d}_t${t}_r$rep.perf"
                rm -f "$stem.out"
            done
            rm -f "$run_deck"
        done
    }
    for d in $ONE_D_SMALL; do time_deck "$d" "$reps" 1; done
    for d in $ONE_D_BIG; do time_deck "$d" "$reps" $BIG_THREADS; done
    for d in $TWO_D; do time_deck "$d" "$(two_d_reps "$d")" $(two_d_threads "$d"); done
    cat "$work/timing.tsv"
fi

# ---------------------------------------------------------------- counters --
if [[ " $passes " == *" counters "* ]]; then
    for d in $ONE_D_SMALL $TWO_D; do
        have_deck "$d" || continue
        run_deck="$work/runs/${d}_t1_cnt.inp"
        with_threads "$(deck_path "$d")" 1 "$run_deck"
        stem="$work/runs/${d}_counters"
        run_one "$profile" "$run_deck" "$stem" 1 > /dev/null
        grep -E '^\[PERF' "$stem.log" > "$work/counters/$d.perf"
        rm -f "$stem.out" "$run_deck"
    done
    ls "$work/counters"
fi

# ----------------------------------------------------------------- publish --
for f in identity.txt legacy.tsv timing.tsv; do
    [ -f "$work/$f" ] && cp "$work/$f" "$results/"
done
for sub in perf counters; do
    [ -d "$work/$sub" ] && { mkdir -p "$results/$sub"; cp "$work/$sub"/*.perf "$results/$sub/" 2>/dev/null; }
done
echo "results in $results"
