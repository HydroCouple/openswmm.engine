#!/bin/bash
# Staged Bellinge ladder: 30-min storm slice -> 8 h probe -> 48 h full.
# Usage: run_bellinge.sh MODE [STAGE...]   MODE = cvode | explicit
# Env: BELLINGE_INP overrides the base model path.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
BASE="${BELLINGE_INP:-$HOME/Downloads/7_SWMM/BellingeSWMM_v021_nopervious.inp}"
MODE="${1:?mode: cvode|explicit}"; shift || true
STAGES=("${@:-storm30m 8h 48h}"); [ $# -gt 0 ] && STAGES=("$@") || STAGES=(storm30m 8h 48h)
STAMP="$(date +%Y%m%d)"
OUT="$HERE/out/${MODE}_${STAMP}"
mkdir -p "$OUT"

# Common hygiene for every run: no MIN_TIMESTEP floor, serial threads.
COMMON=(--threads 1 --del2d MIN_TIMESTEP)
case "$MODE" in
  cvode)    MODEOPTS=(--set2d INTEGRATOR=CVODE) ;;
  explicit) MODEOPTS=(--set2d INTEGRATOR=EXPLICIT --set2d COUPLING_AREA=AUTO) ;;
  *) echo "unknown mode: $MODE" >&2; exit 2 ;;
esac

for st in "${STAGES[@]}"; do
  inp="$OUT/bellinge_${st}.inp"
  case "$st" in
    storm30m) python3 "$HERE/make_slice.py" "$BASE" "$inp" --start "06/29/2012 04:15" --hours 0.5 "${COMMON[@]}" "${MODEOPTS[@]}" ;;
    8h)       python3 "$HERE/make_slice.py" "$BASE" "$inp" --hours 8  "${COMMON[@]}" "${MODEOPTS[@]}" ;;
    48h)      python3 "$HERE/make_slice.py" "$BASE" "$inp" "${COMMON[@]}" "${MODEOPTS[@]}" ;;
    *) echo "unknown stage: $st" >&2; exit 2 ;;
  esac
  python3 "$HERE/run_one.py" "${MODE}_${st}" "$inp" --outdir "$OUT"
done
echo "results: $OUT/results.csv"
