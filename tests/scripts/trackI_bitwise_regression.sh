#!/usr/bin/env bash
# Track I gate G1 — bitwise regression.
#
# Every surface-only 2D model with NO [2D_INFILTRATION*] section must produce
# byte-identical results before and after the per-cell infiltration work.
# Runs each model through the pristine-HEAD binary and the working-tree binary
# and compares the .out result file byte-for-byte, plus the .rpt with the
# version/date banner stripped.
#
# Usage: tests/scripts/trackI_bitwise_regression.sh [baseline_cli] [candidate_cli]
#
# Artifacts land in tests/output/trackI_g1/ (CLAUDE.md §4.1 — reviewable, not
# a temp directory).
set -u

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BASE_CLI="${1:-$REPO/../openswmm.engine.trackI-baseline/build/darwin-tests/src/cli/openswmm}"
CAND_CLI="${2:-$REPO/build/darwin-tests/src/cli/openswmm}"
OUTDIR="$REPO/tests/output/trackI_g1"

[ -x "$BASE_CLI" ] || { echo "baseline CLI not executable: $BASE_CLI"; exit 2; }
[ -x "$CAND_CLI" ] || { echo "candidate CLI not executable: $CAND_CLI"; exit 2; }

rm -rf "$OUTDIR"; mkdir -p "$OUTDIR"

# Every .inp in the repo that carries a 2D mesh (inline or sidecar) and no
# infiltration section. Sorted so the report is stable.
# (bash 3.2 on macOS has no mapfile)
MODELS=()
while IFS= read -r line; do
    [ -n "$line" ] && MODELS+=("$line")
done < <(
  # Underscore-prefixed decks are gtest SCRATCH fixtures (the suites write
  # them into the data cwd as they run) — ephemeral, sometimes deliberately
  # malformed, and present or absent depending on what ran last. Sweeping
  # them in made the census's verdict depend on test-run residue: the S2
  # check's own refusal fixtures showed up as "differing" decks.
  find "$REPO/tests" -name '*.inp' -not -name '_*' \
       -not -path '*/trackI_g1/*' -print0 \
  | xargs -0 grep -l -E '^\[2D_(TRIANGLES|MESH_FILE)\]' 2>/dev/null \
  | xargs grep -L -E '^\[2D_INFILTRATION' 2>/dev/null | sort
)

echo "G1 bitwise regression: ${#MODELS[@]} 2D models"
echo "  baseline : $BASE_CLI"
echo "  candidate: $CAND_CLI"
echo

pass=0; fail=0; skip=0
for m in "${MODELS[@]}"; do
    name="$(basename "${m%.inp}")_$(echo "$m" | md5 -q | cut -c1-6)"
    for side in base cand; do
        d="$OUTDIR/$side/$name"; mkdir -p "$d"
        # Run in the model's own directory so relative mesh/rain paths resolve,
        # writing results into the side-specific directory.
        cli=$([ "$side" = base ] && echo "$BASE_CLI" || echo "$CAND_CLI")
        ( cd "$(dirname "$m")" && \
          "$cli" "$m" "$d/run.rpt" "$d/run.out" ) > "$d/stdout.log" 2>&1
        echo "$?" > "$d/exit"
    done

    be="$(cat "$OUTDIR/base/$name/exit")"; ce="$(cat "$OUTDIR/cand/$name/exit")"
    if [ "$be" != "$ce" ]; then
        echo "FAIL  $m  (exit $be vs $ce)"; fail=$((fail+1)); continue
    fi
    if [ ! -s "$OUTDIR/base/$name/run.out" ]; then
        echo "SKIP  $m  (no .out produced; exit $be)"; skip=$((skip+1)); continue
    fi
    if cmp -s "$OUTDIR/base/$name/run.out" "$OUTDIR/cand/$name/run.out"; then
        # The banner lines legitimately differ, and the 2D continuity block
        # legitimately gains ONE new row: the infiltration loss channel. A
        # ZERO-valued row is the intended report change on a project with no
        # [2D_INFILTRATION*] section. A NON-zero one is left in, so an
        # unconfigured project that somehow infiltrated still fails here.
        norm() {
            sed -E -e '/VERSION|Analysis begun|Analysis ended|Total elapsed time/d' \
                   -e '/^  Infiltration Loss \.+ +0\.000 +0\.000 *$/d' "$1"
        }
        norm "$OUTDIR/base/$name/run.rpt" > "$OUTDIR/base/$name/run.rpt.norm"
        norm "$OUTDIR/cand/$name/run.rpt" > "$OUTDIR/cand/$name/run.rpt.norm"
        if cmp -s "$OUTDIR/base/$name/run.rpt.norm" "$OUTDIR/cand/$name/run.rpt.norm"; then
            pass=$((pass+1))
        else
            echo "FAIL  $m  (.out identical but .rpt differs)"
            diff "$OUTDIR/base/$name/run.rpt.norm" "$OUTDIR/cand/$name/run.rpt.norm" \
                 > "$OUTDIR/$name.rpt.diff" 2>&1
            fail=$((fail+1))
        fi
    else
        echo "FAIL  $m  (.out differs)"; fail=$((fail+1))
    fi
done

echo
echo "G1: $pass identical, $fail differing, $skip skipped (of ${#MODELS[@]})"
[ "$fail" -eq 0 ] || exit 1
[ "$pass" -gt 0 ] || { echo "G1 ran no comparable models — the gate is vacuous"; exit 2; }
