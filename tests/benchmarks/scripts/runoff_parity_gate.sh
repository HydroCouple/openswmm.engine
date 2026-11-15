#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Bit-parity gate for the runoff scaling study. Runs a candidate CLI on the
# roofs/mixed decks and compares each binary output against the legacy
# engine's output for the same deck.
#
# Usage: runoff_parity_gate.sh <candidate-cli> <legacy-cli> <deck-dir> <ref-dir> <outdir>
#
# <ref-dir> caches the legacy outputs (<deck>.leg.out); a missing one is
# produced with <legacy-cli> first. The legacy and engine outputs differ only
# in the 4-byte version field at offset 4, so the comparison skips the first
# 8 bytes (magic + version) and also requires equal file sizes. The candidate
# outputs are deleted after a match to save disk. Exit status is the number
# of decks that failed.
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
exit "$fails"
