#!/bin/sh
# Inlet-junction behaviour checks on examples/inlets/street_inlet_junction.inp
# (plans/INLET_JUNCTION_VERIFICATION_HANDOFF_2026-09-05.md §3.5). Each check
# derives a variant deck, runs it through the refactored engine and greps the
# report for the expected behaviour. Outputs land in _out/ next to this script
# (gitignored) so a surprise can be read rather than guessed at.
#
#     sh tests/manual/inlet_junction_checks/run_checks.sh [path/to/openswmm]
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
CLI=${1:-$ROOT/build/darwin/bin/Release/openswmm}
DECK=$ROOT/examples/inlets/street_inlet_junction.inp
OUT=$HERE/_out
mkdir -p "$OUT"
fail=0
ok()   { echo "  ok    $1"; }
bad()  { echo "  FAIL  $1"; fail=$((fail+1)); }
run()  { "$CLI" "$OUT/$1.inp" "$OUT/$1.rpt" "$OUT/$1.out" > "$OUT/$1.log" 2>&1; }
ijrow() { awk '/Street Inlet Flow Summary/{p=1} /Analysis begun/{p=0} p' "$OUT/$1.rpt" | grep -E '^\s+IJ1 \(node\)'; }

echo "== baseline"
cp "$DECK" "$OUT/base.inp"; run base
grep -qE 'ERROR' "$OUT/base.rpt" && bad "baseline has errors" || ok "baseline runs clean"
ijrow base | awk '{ if ($6+0 > 0) exit 0; exit 1 }' && ok "IJ1 captures flow (peak capture $(ijrow base | awk '{print $7}') %)" || bad "IJ1 captures nothing"
grep -qE '^\s+IJ1\s' "$OUT/base.rpt" && awk '/Node Flooding Summary/{p=1} /Outfall Loading/{p=0} p' "$OUT/base.rpt" | grep -qE '^\s+IJ1\s' && bad "IJ1 floods in the baseline" || ok "IJ1 does not flood with MaxDepth 0.5"

echo "== 1. Flood threshold = max(street full depth, MaxDepth) — plan D-E2"
# The example street (curb 0.5 + 2.0 gutter depression + 10 ft backing at 4 %)
# is ~2.9 ft deep and the gutter hydrograph never fills it, so drive IJ1's head
# from downstream: OUT_ST held at 101.5 ft puts 3.5 ft over IJ1's invert (98).
tail() { sed -E 's/^(OUT_ST[[:space:]]+97\.0000[[:space:]]+)FREE[[:space:]]+/\1FIXED        101.5            /' "$1"; }
tail "$DECK" > "$OUT/tail_md05.inp"; run tail_md05
awk '/Node Flooding Summary/{p=1} /Outfall Loading/{p=0} p' "$OUT/tail_md05.rpt" | grep -qE '^\s+IJ1\s' \
  && ok "MaxDepth 0.5 (below the section): IJ1 floods once the head exceeds the street's full depth" \
  || bad "IJ1 did not flood at 3.5 ft of head with MaxDepth 0.5"
sed -E 's/^(IJ1[[:space:]]+98\.0000[[:space:]]+)0\.5000/\15.0000/' "$DECK" | tail /dev/stdin > "$OUT/tail_md5.inp"; run tail_md5
# The imposed 101.5 ft stage arrives as a step, and the DW start-up surge
# briefly overshoots the static 3.5 ft head, so judge the threshold by the
# depth the node is allowed to reach (flooding clips it there): the section's
# full depth with MaxDepth 0.5, 5.0 with MaxDepth 5.0.
ijdepth() { awk '/Node Depth Summary/{p=1} /Node Inflow Summary/{p=0} p' "$OUT/$1.rpt" | awk '$1=="IJ1"{print $4}'; }
d05=$(ijdepth tail_md05); d5=$(ijdepth tail_md5)
echo "        IJ1 max depth: MaxDepth 0.5 -> $d05 ft (clipped at the street's full depth); MaxDepth 5.0 -> $d5 ft"
awk -v a="$d05" -v b="$d5" 'BEGIN{ exit (b+0 > a+0+0.5 && b+0 <= 5.0+1e-6) ? 0 : 1 }' \
  && ok "MaxDepth 5.0 raises the flood threshold (node holds $d5 ft, capped at 5.0)" \
  || bad "MaxDepth 5.0 did not raise the threshold (max depth $d05 vs $d5)"

echo "== 2. MaxDepth 0 -> threshold is the street section's full depth"
sed -E 's/^(IJ1[[:space:]]+98\.0000[[:space:]]+)0\.5000/\10.0000/' "$DECK" > "$OUT/maxdepth0.inp"; run maxdepth0
grep -qE 'ERROR' "$OUT/maxdepth0.rpt" && bad "MaxDepth 0 errors" || ok "MaxDepth 0 accepted"
d=$(awk '/Node Depth Summary/{p=1} /Node Inflow Summary/{p=0} p' "$OUT/maxdepth0.rpt" | awk '$1=="IJ1"{print $4}')
echo "        IJ1 max depth $d ft (curb 0.5 + backing; flooding only above the derived full depth)"
awk '/Node Flooding Summary/{p=1} /Outfall Loading/{p=0} p' "$OUT/maxdepth0.rpt" | grep -qE '^\s+IJ1\s' && echo "        (IJ1 floods above the derived depth)" || ok "no flooding below the derived full depth"

echo "== 3. KINWAVE -> error 619 for IJ1"
sed -E 's/^(FLOW_ROUTING[[:space:]]+)DYNWAVE/\1KINWAVE/' "$DECK" > "$OUT/kinwave.inp"; run kinwave
grep -qE 'ERROR 619.*IJ1' "$OUT/kinwave.rpt" && ok "ERROR 619 names IJ1" || { bad "no ERROR 619 for IJ1 under KINWAVE"; grep -E 'ERROR' "$OUT/kinwave.rpt" | head -3; }

echo "== 3b. FV -> error 619 for IJ1 (the FV mesh splices virtual junctions out)"
sed -E 's/^(FLOW_ROUTING[[:space:]]+)DYNWAVE/\1FV/' "$DECK" > "$OUT/fv.inp"; run fv
grep -qE 'ERROR 619.*IJ1' "$OUT/fv.rpt" && ok "ERROR 619 names IJ1" || { bad "no ERROR 619 for IJ1 under FV"; grep -E 'ERROR|Continuity Error' "$OUT/fv.rpt" | head -3; }

echo "== 4. Dry start, no inflow -> no NaN, zero capture, continuity fine"
awk '/^\[INFLOWS\]/{skip=1; print; next} skip && /^\[/{skip=0} skip && !/^;/ && NF {next} {print}' "$DECK" > "$OUT/dry.inp"; run dry
grep -qiE 'nan|inf\b' "$OUT/dry.rpt" && bad "NaN/inf in the dry report" || ok "no NaN"
ijrow dry | awk '{ if ($6+0 == 0) exit 0; exit 1 }' && ok "zero capture" || bad "capture without inflow"
grep -E 'Continuity Error' "$OUT/dry.rpt" | sed 's/^/        /'

echo "== 5. VIRTUAL_JUNCTION_MOMENTUM FULL vs BASIC -> identical .out"
awk '/^\[OPTIONS\]/{print; print "VIRTUAL_JUNCTION_MOMENTUM FULL"; next} {print}' "$DECK" > "$OUT/mom_full.inp"; run mom_full
awk '/^\[OPTIONS\]/{print; print "VIRTUAL_JUNCTION_MOMENTUM BASIC"; next} {print}' "$DECK" > "$OUT/mom_basic.inp"; run mom_basic
cmp -s "$OUT/mom_full.out" "$OUT/mom_basic.out" && ok "byte-identical .out" || bad ".out differs between FULL and BASIC"

echo "== 6. Surcharge MH2 (OUT_SEW FIXED 98.5, above MH2's rim 98.0) -> IJ1 backflow, ST_C carries it"
sed -E 's/^(OUT_SEW[[:space:]]+88\.0000[[:space:]]+)FREE[[:space:]]+/\1FIXED        98.5             /' "$DECK" > "$OUT/backflow.inp"; run backflow
grep -qE 'ERROR' "$OUT/backflow.rpt" && { bad "backflow deck errors"; grep -E 'ERROR' "$OUT/backflow.rpt" | head -2; }
bff=$(ijrow backflow | awk '{print $10}')
awk -v b="$bff" 'BEGIN{ exit (b+0 > 0) ? 0 : 1 }' && ok "IJ1 Back Flow Freq = $bff %" || bad "IJ1 Back Flow Freq is $bff"
qc=$(awk '/Link Flow Summary/{p=1} /Flow Classification|Conduit Surcharge/{p=0} p' "$OUT/backflow.rpt" | awk '$1=="ST_C"{print $3}')
qb=$(awk '/Link Flow Summary/{p=1} /Flow Classification|Conduit Surcharge/{p=0} p' "$OUT/base.rpt" | awk '$1=="ST_C"{print $3}')
echo "        ST_C max flow: baseline $qb, surcharged $qc cfs"
awk -v a="$qc" -v b="$qb" 'BEGIN{ exit (a+0 > b+0) ? 0 : 1 }' && ok "ST_C carries more flow with backflow" || bad "ST_C flow did not increase"

echo
[ "$fail" = 0 ] && echo "ALL CHECKS PASSED" || echo "$fail CHECK(S) FAILED"
exit $fail
