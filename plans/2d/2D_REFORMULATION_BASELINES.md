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
| B3 | overdraw repro ×3 (~/Downloads/7_SWMM/overdraw_repro) | 4 tris / 1 h | <1 s | **0.003% / 0.038% / 0.000%** (default / NO_INTERP / tight-tol; 2D block readout, post-`c88e31f9`) | drain 40.637 (default) | the conservation gate; readout MUST grep the "2D Surface Routing Continuity" block (the first "Continuity Error" in the .rpt is the always-0.000 runoff block) |
| B4 | scale100k (gen_scaling_mesh 224×224) | 100,352 tris / 3 h | **15.72 s** | 0.000% | pure 2D, walls | uncoupled MOL floor confirmed healthy |
| MS-A(10²) | gen_multiscale_mesh --dx-fine 20 | 8,192 tris / 3 h | **1.70 s** | 0.000% | pure 2D | area ratio 10²:1 |
| MS-A(10⁴) | gen_multiscale_mesh (default) | 8,192 tris / 3 h | **28.29 s** | 0.000% | pure 2D | area ratio 10⁴:1 — **16.6× slower than 10²:1 at identical tri count = the large-cell problem, quantified** (Phase 4 gate: ≤2×) |
| MS-B (pre-fix) | gen_coupled_multiscale (new) | 8,192 tris + 5 inlets / 3 h | **1073.21 s** | **−2.613%** | drain 21,944, spill 14,038 (36 k m³ churned for ~7.9 k net) | pre-`c88e31f9` record: miniature Bellinge — 646 failed windows, 1D avg step 0.67 s, 12.95% non-converging |
| MS-B | same, post-`c88e31f9` (volume-exact resync) | 8,192 tris + 5 inlets / 3 h | **1052.62 s** | **+0.006%** (was −2.613%) | drain 21,757, spill 14,168 | **resync fix validated at scale**: −2.6% → 0.006% continuity. Wall still pathological (655 failed windows, 1D min step 0.50 s, 15% non-converging) — the window/coupling architecture Phase 3 replaces. Phase 3 target: min 1D step ≥ 1 s, wall ≪ 1053 s |

Bellinge (BellingeSWMM_v021_nopervious + sliver .2dm): **excluded** until the
mesh is regenerated in the GUI (fixed pipeline). Reference pathology numbers,
fixed engine, 2026-07-21: wall 14,240 s vs 105 s 1D-only; 99.24% of 775,353
windows failed; 2D −251.3%; 1D avg step 0.22 s (1.55 s 1D-only). Remaining
Bellinge-scale leak suspect: failure-path `reinitialize()` head-reseed zeroing
negative-volume cells (superseded by Phase 3, which deletes the failure path).

## Phase 3c decision — global VFR default REJECTED (evidence-based, 2026-07-22)

A/B of FLAT (default) vs VFR + VFR_FACE on the inundation models, analytic
Jacobian active in both:

| Model | FLAT | VFR | Ratio | Note |
|---|---|---|---|---|
| weir | 47.7 s | 78.4 s | 1.64× slower | BDF steps ~equal (75.8k vs 75.9k); Krylov iters 59k→120k |
| road | 24.1 s | 41.7 s | 1.73× slower | drain 42.0k→47.0k (physics shift) |
| MS-A 10⁴:1 | 28.3 s | 92.6 s | 3.3× slower | — |

The cost is linear-solve conditioning at shorelines (Krylov iters double at
near-equal step count), i.e. the "3–8× more CVODE steps" the SolverOptions
comment warned of — a genuine closure COST, not a bug (the VFR analytic-Jv
parity test passes to 5e-5). VFR's fully-wet branch IS flat, so deep-water
inundation models pay for shoreline wet/dry resolution they don't need.

**Decision:** do NOT flip the global default to VFR. The uphill-ratchet VFR
fixes is a LARGE-high-relief-cell (watershed) pathology; the right treatment is
the relief-gated **AUTO** closure (FLAT on low-relief urban/inundation cells,
VFR only where per-cell relief exceeds a threshold), implemented in **Phase 4**
alongside the graded-atol large-cell work — not a blanket Phase 3 default flip.
FLAT stays the default; VFR/VFR_FACE remain opt-in and are now analytic-J·v
capable. Artifacts: /tmp/wv.rpt, /tmp/rv.rpt, ms_a_r10000_vfr.inp.

## Phase 3d finding — live coupling is NO LONGER "10× slower" (2026-07-22)

The strategy docs' central premise (`2D_SOLVER_STEPPING_PERFORMANCE_PLAN.md`
Phase 3: "the 1D↔2D orifice coupling stiffness is THE obstacle … ~10× SLOWER")
no longer holds. Measured on weir, current engine:

| Path | Wall | BDF steps | Krylov iters | J·v |
|---|---|---|---|---|
| HELD (default) | 47.7 s | 75,808 | 59,242 | analytic |
| LIVE (`OPENSWMM_2D_LIVE_COUPLING=1`) | 60.0 s | **75,824** | 175,830 | FD |

The live path takes the SAME number of BDF steps as held — the stiff orifice is
NOT collapsing the step. The entire 1.26× penalty is finite-difference J·v
(175,830 FD-Jv RHS evals), because the analytic Jacobian falls back to FD on the
live path. The accumulated fixes (clock-resync guard, volume-exact resync,
Phase 1 lighter RHS) already removed the step collapse that motivated the whole
"coupling-aware preconditioner" line.

**Implication:** the definitive fix is NOT a coupling-aware preconditioner (the
step count is already fine) — it is the analytic ORIFICE TANGENT so the live
path uses analytic J·v like the held path. That tangent is clean ONLY under
**single-cell coupling** (η_2d = η_cell, scatter weight 1): the current
vertex-STENCIL scatter has head-dependent weights whose tangent is as messy as
the deleted deviation's. So Phase 3d's real content is: single-cell coupling
(a coupling-distribution behavior change) → trivial orifice tangent → analytic
live J·v → live becomes competitive with/faster than held → make live the
default (the conservative, temporal-approximation-free coupling). This is a
deliberate behavior change (single-cell vs stencil exchange distribution) and is
the next major increment.

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
