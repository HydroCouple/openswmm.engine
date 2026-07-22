# 2D Reformulation — Phase 0 Trusted Baselines (2026-07-21)

Engine: commit `d1db2e39` (decoupled coupling + conservation fixes), Release
`build/darwin`, serial/OpenMP CPU on Apple Silicon (this Mac). All runs via
`build/darwin/src/cli/openswmm`. Every later phase's gate compares against THIS
table — do not mix in numbers from the strategy docs (different configs/machines).

Plan: `~/.claude/plans/let-us-revisit-the-eager-salamander.md` ("One ODE, One
Solver, Nothing Else"). Phases: 0 baselines → 1 RHS hygiene → 2 analytic
Jacobian → 3 simplification rewrite → 4 multi-scale error control → 5 scale +
Bellinge gate.

## Baseline table

| ID | Model | Size / sim | Wall (real) | 2D continuity | Exchange totals (m³) | Notes |
|---|---|---|---|---|---|---|
| B1 | weir_culvert (GUI examples/demo_weir_culvert) | small mesh / 42 h | **60.28 s** | 0.003% | drain 626.602, spill 0 | user 58.7 s ⇒ ~serial |
| B2 | road_culvert (GUI examples/demo_road_culvert) | ~8k tris / 18 h | **44.32 s** | −0.020% | drain 47,006.995, spill 8,499.067 | user 172.6 s ⇒ OMP threads active |
| B3 | overdraw repro ×3 (~/Downloads/7_SWMM/overdraw_repro) | 4 tris / 1 h | <1 s | **0.000% ×3** (default, NO_INTERP, tight-tol) | drain 40.637 (default) | the conservation gate |
| B4 | scale100k (gen_scaling_mesh 224×224) | 100,352 tris / 3 h | (running — fill in) | | pure 2D, walls | uncoupled MOL floor |
| MS-A(10²) | gen_multiscale_mesh --dx-fine 20 | 8,192 tris / 3 h | **1.70 s** | 0.000% | pure 2D | area ratio 10²:1 |
| MS-A(10⁴) | gen_multiscale_mesh (default) | 8,192 tris / 3 h | **28.29 s** | 0.000% | pure 2D | area ratio 10⁴:1 — **16.6× slower than 10²:1 at identical tri count = the large-cell problem, quantified** (Phase 4 gate: ≤2×) |
| MS-B | gen_coupled_multiscale (new) | 8,192 tris + 5 inlets / 3 h | (running — fill in) | | two-way drain/spill | coupled multiscale = miniature Bellinge; windows collapse to ~0.6 s, hundreds of failed windows (Phase 3 gate: minimum 1D step ≥ 1 s, wall ≪) |

Bellinge (BellingeSWMM_v021_nopervious + sliver .2dm): **excluded** until the
mesh is regenerated in the GUI (fixed pipeline). Reference pathology numbers,
fixed engine, 2026-07-21: wall 14,240 s vs 105 s 1D-only; 99.24% of 775,353
windows failed; 2D −251.3%; 1D avg step 0.22 s (1.55 s 1D-only). Remaining
Bellinge-scale leak suspect: failure-path `reinitialize()` head-reseed zeroing
negative-volume cells (superseded by Phase 3, which deletes the failure path).

## Fixture provenance

- `examples/imex_scaling/ms_a_r100.inp` ← `gen_multiscale_mesh.py ms_a_r100.inp --dx-fine 20`
- `examples/imex_scaling/ms_a_r10000.inp` ← `gen_multiscale_mesh.py ms_a_r10000.inp`
- `examples/imex_scaling/ms_b.inp` ← `gen_coupled_multiscale.py ms_b.inp` (NEW
  generator; junction RIM must sit at the local bed — MaxDepth == bury — or
  free-inlet capture never fires; conduit lengths floored at 30 m to keep the
  1D Courant step honest)
- `examples/imex_scaling/scale100k.inp` ← `gen_scaling_mesh.py 224 224 scale100k.inp`

## Measurement protocol

- Wall: `/usr/bin/time` real (record user to detect OMP).
- Continuity + exchange: `grep -A14 "2D Surface Routing Continuity" X.rpt`.
- Per-window ledger: `OPENSWMM_2D_DEBUG_COUPLE=1` (stderr `[couple]` lines).
- BDF invariant probe: `OPENSWMM_2D_DEBUG_SINK=1` (stderr `[sink]` lines).
- CPU profile: run CLI in background, then `sample $(pgrep -f
  "cli/openswmm <model>" | head -1) 15 -file out.txt` — pgrep the BINARY path;
  a compound-shell wrapper matches the same pattern and samples zsh instead.
- Solver step counts: Phase 1 adds the permanent "2D Solver Statistics" block;
  until then rely on `[sink]` nst deltas.
