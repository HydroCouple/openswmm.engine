#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Bit-parity gate for the runoff scaling study. Runs a candidate CLI on the
# roofs/mixed decks and compares its results against the legacy engine's.
#
# Usage: runoff_parity_gate.sh <candidate-cli> <legacy-cli> <deck-dir> <ref-dir> <outdir>
#
# Two checks per deck:
#  1. The binary output (.out). Legacy and engine outputs differ only in the
#     4-byte version field at offset 4, so the comparison skips the first 8
#     bytes and also requires equal sizes. The .out stores 32-bit floats, so
#     on its own it cannot see a 1-ULP change in a double that does not grow.
#  2. The hot start file, which stores every subcatchment's subarea depths,
#     runoff and infiltration state as doubles. Each deck is cut short to end
#     at 01:30 (mid-storm) and at 03:00 (recession) with SAVE HOTSTART at the
#     end of the run (end-of-run saves also work with a legacy CLI built
#     before its dated SAVE HOTSTART fix); the two files must be
#     byte-identical.
#
# <ref-dir> caches the legacy outputs; a missing one is produced with
# <legacy-cli> first. Candidate outputs are deleted after a match to save
# disk. Exit status is the number of failed checks.
set -uo pipefail

cand=$1 legacy=$2 decks=$3 refs=$4 outdir=$5
mkdir -p "$refs" "$outdir"
fails=0
for m in roofs_20k mixed_20k roofs_200k mixed_200k; do
    deck="$decks/$m.inp"
    ref="$refs/$m.leg.out"
    if [ ! -f "$ref" ]; then
        "$legacy" "$deck" "$refs/$m.leg.rpt" "$ref" > "$refs/$m.leg.log" 2>&1 ||
            { echo "$m: legacy run FAILED"; fails=$((fails + 1)); continue; }
    fi
    out="$outdir/$m.cand.out"
    if ! "$cand" "$deck" "$outdir/$m.cand.rpt" "$out" > "$outdir/$m.cand.log" 2>&1; then
        echo "$m: candidate run FAILED (see $outdir/$m.cand.log)"
        fails=$((fails + 1))
        continue
    fi
    if [ "$(stat -f %z "$ref")" = "$(stat -f %z "$out")" ] && cmp -s -i 8 "$ref" "$out"; then
        echo "$m: IDENTICAL to legacy"
        rm -f "$out"
    else
        echo "$m: DIFFERS from legacy ($(cmp -i 8 "$ref" "$out" 2>&1 | head -1))"
        fails=$((fails + 1))
    fi
done

# Write <deck> cut to end at <hh:mm:ss>, saving a hot start file <hsf> at the end.
cut_deck() {
    awk -v end="$2" -v hsf="$3" '
        /^END_TIME/ { print "END_TIME             " end; next }
        /^\[EVAPORATION\]/ { print "[FILES]"; print "SAVE HOTSTART \"" hsf "\""; print "" }
        { print }' "$1" > "$4"
}
for m in roofs_20k mixed_20k roofs_200k mixed_200k; do
    for end in 01:30:00 03:00:00; do
        tag="${m}_${end//:/}"
        ref="$refs/$tag.leg.hsf"
        if [ ! -f "$ref" ]; then
            cut_deck "$decks/$m.inp" "$end" "$ref" "$refs/$tag.leg.inp"
            "$legacy" "$refs/$tag.leg.inp" "$refs/$tag.leg.rpt" "$refs/$tag.leg.out" \
                > "$refs/$tag.leg.log" 2>&1 || { echo "$tag: legacy run FAILED"; fails=$((fails + 1)); continue; }
            rm -f "$refs/$tag.leg.out"
        fi
        hsf="$outdir/$tag.cand.hsf"
        cut_deck "$decks/$m.inp" "$end" "$hsf" "$outdir/$tag.cand.inp"
        if ! "$cand" "$outdir/$tag.cand.inp" "$outdir/$tag.cand.rpt" "$outdir/$tag.cand.out" \
            > "$outdir/$tag.cand.log" 2>&1; then
            echo "$tag: candidate run FAILED (see $outdir/$tag.cand.log)"
            fails=$((fails + 1))
            continue
        fi
        rm -f "$outdir/$tag.cand.out"
        if cmp -s "$ref" "$hsf"; then
            echo "$tag: hot start IDENTICAL to legacy"
            rm -f "$hsf"
        else
            echo "$tag: hot start DIFFERS from legacy ($(cmp "$ref" "$hsf" 2>&1 | head -1))"
            fails=$((fails + 1))
        fi
    done
done
exit "$fails"
