#!/bin/zsh
# Phase-6 parity: run each .inp through the CURRENT engine and byte-compare the
# .out against the pre-Phase-6 baseline (phase_5/work/<m>/golden.out, captured
# at c268af1a). Phase 6 is a pure in-memory refactor → .out must be IDENTICAL.
ENG=/Users/calebbuahin/Documents/Projects/cbuahin_github/openswmm.engine
QA=/Users/calebbuahin/Downloads/epaswmm5_qa/data
BIN=$ENG/build-arm64-osx/bin/Debug/openswmm
BASE=$ENG/docs/relational/qa_runs/phase_5/work
OUT=$ENG/docs/relational/qa_runs/phase_6/work
mkdir -p "$OUT"

MODELS=("$@")
if [[ ${#MODELS[@]} -eq 0 ]]; then
  MODELS=(extran1 extran2 extran3 extran4 extran6 extran7 extran9 extran10 \
          test1 test2 test3 test4 test5 user1 user2 user3 user4 user5)
fi
printf "%-10s %-6s %s\n" model flow result
printf "%-10s %-6s %s\n" ---------- ----- --------
for m in $MODELS; do
  inp=$QA/$m/$m.inp
  base=$BASE/$m/golden.out
  [[ -f $inp ]] || { printf "%-10s %s\n" "$m" "missing-inp"; continue; }
  [[ -f $base ]] || { printf "%-10s %s\n" "$m" "missing-baseline"; continue; }
  fu=$(grep -i "FLOW_UNITS" "$inp" | head -1 | awk '{print $2}')
  "$BIN" "$inp" "$OUT/$m.rpt" "$OUT/$m.out" >/dev/null 2>&1
  if cmp -s "$base" "$OUT/$m.out"; then res=EXACT; else res="DIFF($(cmp -l "$base" "$OUT/$m.out" 2>/dev/null | wc -l | tr -d ' ')b)"; fi
  printf "%-10s %-6s %s\n" "$m" "$fu" "$res"
done
