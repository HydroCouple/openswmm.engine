#!/bin/bash
# Staged Bellinge ladder: 30-min storm slice -> 8 h probe -> 48 h full.
# Usage: run_bellinge.sh MODE [STAGE...]   MODE = explicit (the only 2D
# integrator since the D2 retirement of the CVODE/ARKODE stack).
# Env: BELLINGE_INP overrides the base model path.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
BASE="${BELLINGE_INP:-$HOME/Downloads/7_SWMM/BellingeSWMM_v021_nopervious.inp}"
MODE="${1:?mode: explicit}"; shift || true
STAGES=("${@:-storm30m 8h 48h}"); [ $# -gt 0 ] && STAGES=("$@") || STAGES=(storm30m 8h 48h)
STAMP="$(date +%Y%m%d)"
OUT="$HERE/out/${MODE}_${STAMP}"
mkdir -p "$OUT"

# Common hygiene for every run: serial threads, and strip the [2D_OPTIONS]
# keys retired with the CVODE/ARKODE stack (D2) — the base model predates the
# retirement and would hard-error unedited (by design).
COMMON=(--threads 1
        --del2d MIN_TIMESTEP --del2d REL_TOLERANCE --del2d ABS_TOLERANCE
        --del2d MAX_KRYLOV_DIM --del2d COUPLING_INTERVAL --del2d COUPLING_WINDOW
        --del2d ACTIVE_SET --del2d ACTIVE_SET_HALO --del2d MAX_CVODE_STEPS
        --del2d LINEAR_SOLVER --del2d PRECONDITIONER)
case "$MODE" in
  explicit) MODEOPTS=(--set2d COUPLING_AREA=AUTO) ;;
  *) echo "unknown mode: $MODE (the explicit marcher is the only 2D integrator)" >&2; exit 2 ;;
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
