#!/bin/zsh
# Phase-5 GPKG round-trip parity driver.
# Builds the harness against the freshly-built engine dylib, then for each
# model: golden run (INP) vs round-trip (INP->gpkg->run), byte-compare .out.
set -e
ENG=/Users/calebbuahin/Documents/Projects/cbuahin_github/openswmm.engine
QA=/Users/calebbuahin/Downloads/epaswmm5_qa/data
P5=$ENG/docs/relational/qa_runs/phase_5
LIBDIR=$ENG/build-arm64-osx/src/engine
BIN=$ENG/build-arm64-osx/bin/Debug

clang++ -std=c++17 -g -O0 "$P5/gpkg_roundtrip.cpp" \
  -I "$ENG/include/openswmm/engine" \
  -L "$LIBDIR" -lopenswmm.engine \
  -Wl,-rpath,"$LIBDIR" -Wl,-rpath,"$BIN" \
  -o "$P5/gpkg_roundtrip"

MODELS=("$@")
if [[ ${#MODELS[@]} -eq 0 ]]; then
  MODELS=(extran2 test1 user4 user1 user3)
fi

echo "model      flow  golden_sz   rt_sz       cmp"
echo "---------- ----- ----------- ----------- --------"
for m in $MODELS; do
  inp=$QA/$m/$m.inp
  [[ -f $inp ]] || { echo "$m  (missing $inp)"; continue; }
  wd=$P5/work/$m
  rm -rf "$wd"; mkdir -p "$wd"
  fu=$(grep -i "FLOW_UNITS" "$inp" | head -1 | awk '{print $2}')
  if "$P5/gpkg_roundtrip" "$inp" "$wd" >"$wd/harness.log" 2>&1; then
    gsz=$(stat -f%z "$wd/golden.out" 2>/dev/null || echo "-")
    rsz=$(stat -f%z "$wd/rt.out" 2>/dev/null || echo "-")
    if cmp -s "$wd/golden.out" "$wd/rt.out"; then res="EXACT"; else res="DIFF"; fi
    printf "%-10s %-5s %-11s %-11s %s\n" "$m" "$fu" "$gsz" "$rsz" "$res"
  else
    printf "%-10s %-5s %-11s %-11s %s\n" "$m" "$fu" "-" "-" "HARNESS-FAIL(see $wd/harness.log)"
  fi
done
