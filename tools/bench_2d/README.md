# bench_2d — 2D solver benchmark harness

Vendored from the 2026-07 `bench_3min` campaign (previously unversioned in
`~/Downloads/7_SWMM/bench_3min/`). Drives the acceptance ladder for the explicit
2D marcher (see `openswmm.gui/workplans/2D_SOLVER_REIMPLEMENTATION_PLAN_2026-07-29.md`).

- `run_one.py TAG INP` — runs one case through the CLI, times it, extracts
  wall/engine seconds, 2D solver stats, continuity %, the `[PERF]` split, and
  appends a `results.csv` row in `--outdir` (default `out/`, repo-visible).
  Pins `OPENSWMM_2D_BACKEND=cpu` (the >=20k-cell GPU plugin auto-gate must never
  contaminate serial benchmarks) and `OPENSWMM_PERF=1`.
- `make_slice.py BASE OUT [--start ...] [--hours H] [--threads N] [--set2d K=V] [--del2d K]`
  — derives time-sliced / option-overridden variants; rewrites relative mesh and
  rain-file paths to absolute; everything else passes through unchanged.
- `run_bellinge.sh MODE [stages]` — the staged ladder (`storm30m`, `8h`, `48h`)
  for `MODE = cvode` (reference) or `explicit` (marcher). Every stage drops the
  `MIN_TIMESTEP` floor and runs `THREADS 1`.

Acceptance gate (Phase 4): `run_bellinge.sh explicit 48h` < 300 s wall, serial,
continuity < 0.5 %.
