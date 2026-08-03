# ROM vs Brute-Force Monte Carlo — Validation Report (Reform PR 10)

Status: measured results from `tests/regression/test_rom_coverage.cpp`
(2026-07-08, macOS/clang, this repository). This PR closes the reform
checklist: it measures the composite effect of PRs 4–9.

## 1. Experiment

- **Network**: the Phase-9 five-junction chain (J1→…→J5→O1, 100 m circular
  conduits at 5% slope, Manning n = 0.013), 0.1 m³/s dry-weather inflow at J1
  — free-surface, conveyance-controlled flow (depths ≈ 0.47 m, no surcharge).
- **Reference**: 21 deterministic engine runs (no ROM) with every conduit's
  Manning's n scaled by the LHS strata midpoints of ±20% — the identical prior
  the ROM stratifies. Empirical q05/q50/q95 per (junction, minute) from the 21
  sorted heads.
- **ROM run**: same network, `[UNCERTAINTY] 1D MANNINGS_N 0.20`, M = 50,
  deviation-form 1D ROM (PRs 6+9), depth sensitivity reference (§3).
- **Window**: 60 report times over 1 h; coverage evaluated for t > 60 s,
  width compared in the saturated regime (t ≥ 30 min; §4). 22 engine runs
  complete in ~90 ms total.

## 2. Results (measured)

| Metric | Checklist floor | Measured | Test assertion |
|---|---|---|---|
| Coverage: ROM [q05,q95] ∋ MC median | ≥ 0.90 | **0.997** (294/295) | ≥ 0.95 |
| Width ratio ROM/MC in [0.3, 3.0] (saturated) | ≥ 0.80 | **1.000** (155/155) | ≥ 0.95 |
| Width ratio min / median / max | — | **0.676 / 1.251 / 1.610** | median ∈ [0.5, 2.0] |

The ROM band is a calibrated estimate of the brute-force band in this regime:
it brackets the Monte-Carlo median essentially always, and its width sits
within a factor of ~1.6 of the truth everywhere, with a slightly conservative
median (1.25×) — the right side for a linearized surrogate to err on.

## 3. Finding fixed during validation: depth sensitivity reference

The first run of this experiment failed spectacularly (coverage 0.56, width
ratio median **14.7×**, max **2769×**). Root cause: the 1D ROM's
Manning-sensitivity forcing projected the **absolute head** (invert + depth),
so its `(mm−1)·b_j` steady state scaled the network's entire invert relief
(~25 m here) by the Manning multiplier — but inverts cannot move; roughness
acts only on conveyance, i.e. on the *depth* component. (The 2D ROM never had
this defect: it works in depth space by construction.)

Fix (this PR): `SpectralROM1D::advance()` accepts an optional Manning
**sensitivity reference** field; the engine passes depth (head − invert).
Absolute head remains the quantile anchor and reconstruction reference —
only the `b_j` projection inside the sensitivity term changes. Standalone /
unit-test callers that pass no reference keep the previous behavior. With the
fix, the width-ratio median moved 14.7 → 1.25.

## 4. Documented behaviors and limitations observed

1. **Spread spin-up is by design.** Deviation-form spread starts at exactly
   zero at seed and grows toward the parametric steady state
   `δa = (mm−1)·b_j` with per-mode time constants `1/(λ_j·K1d)` — minutes to
   tens of minutes on this network, slowest at the upstream end. Width
   comparisons are therefore made in the saturated regime; in the first
   ~10 minutes the ROM under-reports spread relative to an always-saturated
   MC reference. Interpretation for users: the band reflects parameter
   uncertainty *accumulated since simulation start*, converging upward to the
   full parametric band.
2. **Surcharge / backwater regime over-predicts.** An earlier fixture variant
   (0.5 m³/s inflow, J1 flooded, whole chain surcharged ~4 m) showed the ROM
   over-predicting steady spread by ~50–190×: under surcharge, heads are set
   by mass balance and backwater, nearly independent of n, while the ROM's
   conveyance-based sensitivity still scales with (large) depth. This is the
   quantitative face of USER_GUIDE §8 limitation 2 (diffusion-wave ROM vs
   full Saint-Venant): treat 1D bands in surcharged reaches as qualitative.
3. **Front-arrival timing spread is not captured.** In the same surcharged
   variant, a filling front's arrival time varied ~minutes across MC members,
   producing transient 2–4 m empirical widths at front passage that the ROM
   (an amplitude-sensitivity method anchored to the deterministic trajectory)
   does not represent. Phase/timing uncertainty is out of scope for the
   current formulation.
4. **Engine note**: with adaptive routing active, `swmm_engine_step` can
   stall making sub-nanosecond time progress at the very final report
   boundary (same family as the fixed OADate rounding bug in `stepRunoff`).
   The test sidesteps it (END_TIME beyond the sampling window) and guards
   with a stall detector; an engine-side fix is a candidate follow-up.

## 5. Reproduction

```
ctest --test-dir build/darwin-tests-local -R test_rom_coverage
```

The test prints the measured summary line
(`[ROM-vs-MC] samples=… coverage=… width-ratio …`) on every run; assertion
thresholds are the checklist floors tightened toward the measured actuals
with margin for solver noise across platforms.

---

# Soft-Rainfall ROM vs Monte Carlo — Validation (SR-5)

Status: measured results from `tests/regression/test_soft_rain_coverage.cpp`
(2026-07-16). Analog of the reform PR-10 experiment above, for the soft-rainfall
location-scale forcing path instead of the Manning's-n parameter.

## 1. Experiment

- **Network**: 5 large-area storage nodes (J1..J5) fed by 5 subcatchments
  (S1..S5) on a single rain gage RG1, chained to a free outfall. Storage area
  is large so heads rise gradually across the whole 1 h run (transient dh/dt),
  keeping the rainfall-rate-driven soft spread active at every report boundary.
- **Prior**: NORMAL, CV = 0.20 on the gage rainfall (a location-scale family;
  the deterministic rain is the location, CV·rain is the standard deviation).
- **Reference (MC)**: 21 deterministic engine runs (no ROM), each with the gage
  rain scaled by the materialized member `1 + z_i·CV`, `z_i = probit((i+0.5)/21)`
  — the same NORMAL prior the ROM propagates, no soft-forcing linearization.
- **ROM**: identical network with `[SOFT_RAINGAGES] RG1 NORMAL CV 0.20`, M = 50.
- **Window**: 12 report times over 1 h across 5 junctions; coverage evaluated
  for t > 60 s; width ratio evaluated in the saturated regime (second half).

## 2. Results (measured)

| Metric | Checklist floor | Measured | Test assertion |
|---|---|---|---|
| Coverage: ROM [q05,q95] ∋ MC median | ≥ 0.90 | **1.000** (60/60) | ≥ 0.90 |
| Width ratio ROM/MC in [0.3, 3.0] (saturated) | ≥ 0.80 | **1.000** (35/35) | ≥ 0.80 |
| Width ratio min / median / max | — | **0.754 / 0.821 / 0.872** | — |

The soft-rain ROM band brackets the Monte-Carlo median at every sample. Its
width is slightly narrower than the brute-force band (median ratio ~0.82) — the
expected mild under-prediction of the delta-linearized location-scale forcing
(`loc + z_i·spread`) relative to the fully nonlinear rain→runoff→routing
response — but comfortably inside the [0.3×, 3×] band. This is the soft-rainfall
analog of the PR-10 credibility check and closes Wave SR-E.

## 3. Reproduction

```
ctest --test-dir build/darwin-tests-local -R test_soft_rain_coverage
```

The test prints `[SoftRain-vs-MC] samples=… coverage=… width-ratio …` on every
run; thresholds are the SR-5 checklist floors.

---

# Correlated Soft-Rainfall ROM vs Monte Carlo — Validation (CL-1e)

Status: measured results from `tests/regression/test_soft_rain_corr_coverage.cpp`
(2026-07-20). Sibling of the SR-5 experiment above for the *spatially-correlated*
soft-rainfall path (`COHERENCE CORR_LEN <meters>`), the last Phase-1 item of the
CORR_LEN feature. Where SR-5 validated the comonotone (`COHERENCE FULL`) scalar
coefficient against a scalar-scaled MC, this validates the finite-correlation-
length coefficient field against a *correlated* MC generated by the same
`CorrelatedFieldGenerator`.

## 1. Experiment

- **Network**: 5 large-area storage nodes (J1..J5) fed by 5 subcatchments on a
  straight chain 200 m apart (`[COORDINATES]`), chained to a free outfall —
  same transient-dh/dt storage design as SR-5 so soft spread stays active at
  every report boundary.
- **Prior**: NORMAL, CV = 0.40 on the rain (higher than SR-5's 0.20 to make the
  spatial cancellation resolvable).
- **Correlation length**: ℓ = 120 m for the coverage comparison (between the
  200 m node spacing and the comonotone limit); ℓ = 30 m for the downstream
  narrowing check (ℓ ≪ spacing ⇒ each node ranks members independently).
- **Reference (correlated MC)**: 21 deterministic engine runs, each with a
  *per-node* rain scaling `base · (1 + W[i][t]·CV)`, where `W` is the
  marginal-preserving rank/copula field from
  `CorrelatedFieldGenerator::generateCoefficientField` over the five junction
  coordinates at ℓ = 120 m, with `coeff_i = z_i = probit((i+0.5)/21)`. Each MC
  member therefore has the exact per-node NORMAL marginal *and* the finite
  spatial correlation the feature introduces (wet upstream / dry downstream).
- **ROM**: identical network with `[SOFT_RAINGAGES] RG1 NORMAL CV 0.40 COHERENCE
  CORR_LEN 120`, M = 50 — the engine builds its own correlated field over the
  same coordinates.
- **Window**: 60 (node, report-time) samples over 1 h; coverage for t > 60 s;
  width ratio in the saturated regime (second half).

## 2. Results (measured)

| Metric | Checklist floor | Measured | Test assertion |
|---|---|---|---|
| Coverage: ROM [q05,q95] ∋ correlated-MC median | ≥ 0.90 | **1.000** (60/60) | ≥ 0.90 |
| Width ratio ROM/MC in [0.3, 3.0] (saturated) | ≥ 0.80 | **1.000** (35/35) | ≥ 0.80 |
| Width ratio min / median / max | — | **0.482 / 0.711 / 0.878** | — |
| Downstream (J5) band: correlated(ℓ=30) vs comonotone | strictly < | **0.214 vs 0.306** (ratio 0.701) | correlated < comonotone |

The correlated ROM band brackets the correlated-MC median at every sample. Its
width sits slightly below the brute-force band (median ratio ~0.71) — a touch
narrower than the SR-5 comonotone case (~0.82), consistent with the delta-
linearized soft forcing under-predicting more when spatial decorrelation
partially cancels the accumulated response — but comfortably inside [0.3×, 3×].

The final row is the *physical point* of the feature: at the most downstream
junction, the short-correlation-length band (0.214 m) is ~30% narrower than the
comonotone band (0.306 m). Comonotone forces every member to be uniformly wet or
dry across all five subcatchments, so upstream uncertainties compound at J5;
finite ℓ lets a member be wet upstream and dry downstream, so the contributions
partially cancel — exactly the tighter, more physically realistic band CORR_LEN
was built to produce.

## 3. Reproduction

```
ctest --test-dir build/darwin-tests-local -R test_soft_rain_corr_coverage
```

The test prints `[SoftRainCorr-vs-MC] …` and `[SoftRainCorr-narrowing] …` on
every run; thresholds are the CL-1e checklist floors.

---

# CL-2a — Profiling Gate: Correlated Projection Cost (Phase-2 Decision)

Status: measured results from `tests/regression/test_corr_len_profile.cpp`
(2026-07-20). The CL-2a profiling gate benchmarks the correlated soft-forcing
projection (the O(M·k·n) inner loop in `SpectralROM::advance`) on a large mesh
and compares it to the comonotone baseline and the total per-step ROM cost. If
the correlated projection is < ~5% of the total routing-step time, CL-1 is
sufficient and CL-2 (reduced-basis optimization) is not worth building.

## 1. Experiment

- **Mesh**: 70×70 structured triangular grid = **9,800 triangles** in a 1 km
  square domain.  (The CL-2a checklist specifies ≥ 20k cells; the Lanczos
  eigensolve on 20k triangles with k=20 takes minutes on this machine, so we
  measure at 10k/k=10 and extrapolate linearly — the projection inner loop is a
  simple dot product whose wall time scales exactly as M·k·n.)
- **ROM**: M = 50 ensemble members, k = 10 retained modes, ℓ = 200 m.
- **Measured**: 3 warm-up + 20 timed `advance()` calls for each path
  (comonotone scalar `c_i` vs correlated spatial field), plus 20 timed
  `computeQuantiles()` calls.
- **Hardware**: macOS, Apple Silicon (base configuration).

## 2. Results (measured)

| Component | Measured (9.8k tri, k=10) | Extrapolated (20k tri, k=20) |
|---|---|---|
| Field generation (one-time) | **64,159 ms** | ~130,000 ms |
| Comonotone advance | **1.5 ms/step** | ~3 ms/step |
| Correlated advance | **15.6 ms/step** | ~60 ms/step |
| Projection delta (corr − comonotone) | **14.1 ms/step** | **57.4 ms/step** |
| Quantile reconstruction | **57.2 ms/step** | **116.7 ms/step** |
| Total advance + quantile | **72.7 ms/step** | **174.1 ms/step** |
| **Projection as % of total** | **19.3%** | **33.0%** |

## 3. Decision: **PROCEED to CL-2**

The correlated projection is **19.3%** of the total per-step ROM cost at 10k
triangles (k=10) and extrapolates to **33%** at the CL-2a target of 20k
triangles (k=20) — well above the 5% decision gate.  CL-1's O(M·k·n) per-step
projection is a significant fraction of the ROM's per-step cost on large meshes.

**However**, two observations refine the CL-2 priority:

1. **Quantile reconstruction is the larger per-step cost** (57 ms vs 14 ms at
   10k).  Even with a perfect CL-2 reduced basis (K_s ≪ M, projection drops to
   O(K_s·k·n)), the total per-step cost would drop from 72.7 ms to ~58.6 ms —
   only a ~19% improvement.  The quantile cost O(M·n) is independent of the
   spatial basis and would not be affected by CL-2.

2. **Field generation is the dominant one-time cost** (64 seconds at 10k
   triangles).  This is a *startup* cost, not per-step, but it is the single
   largest time component and would benefit from optimization regardless of
   CL-2.  The `CorrelatedFieldGenerator::generateCoefficientField` neighbourhood
   search (3ℓ radius, exponential kernel) is O(n · avg_neighbours) — for ℓ=200
   m on a 10 m grid, avg_neighbours ≈ 120, giving ~1.2M kernel evaluations.  A
   spatial index (grid bucketing) would reduce this; the current implementation
   already uses grid-indexed neighbourhoods but the per-cell sorting/ranking
   step adds O(M · n · log M) overhead.

**Recommendation**: CL-2 is justified for large meshes (≥ 10k cells) where the
projection is > 15% of per-step cost.  But the field-generation one-time cost
(64+ seconds) is the more urgent optimization target — it makes `CORR_LEN`
 impractical for interactive use on large meshes regardless of the per-step
projection cost.  CL-2b should consider optimizing field generation alongside
the reduced-basis projection.

## 4. Reproduction

```
ctest --test-dir build/darwin-tests-local -R test_corr_len_profile
```

The benchmark prints `=== CL-2a Profiling Gate ===` with all measured and
extrapolated numbers on every run.  The test always passes (it is a benchmark,
not a correctness test).

---

# CL-2b / CL-2c — SPDE Reduced Spatial Basis (Correlated Soft Rain)

Status: measured results from `tests/unit/engine/test_spde_spatial_basis.cpp`
(CL-2b + CL-2c) and the re-run of `test_soft_rain_corr_coverage.cpp` against the
reduced path (2026-07-20). Design note: `docs/uncertainty/SPDE_SPATIAL_BASIS.md`.

## 1. What CL-2b/CL-2c deliver

CL-2b replaced the CL-1 materialized `M×n` correlated field with an analytic
Whittle–Matérn ν=2 spatial basis (`K_s` Neumann-cosine modes). CL-2c integrated
it into both ROMs (1D gage, 2D grid) via a reduced projection
`R_{ij} = Σ_m a_im·(Pᵀ(spread⊙ψ_m))_j`, folding the per-point normalization
`g(t)` into the mode fields (`ψ_m = g·φ_m`, the "seam").

## 2. Results (measured)

| Metric | CL-1 (materialized) | CL-2 (SPDE reduced) | Source |
|---|---|---|---|
| Field generation, 10k cells, M=50 | **64,159 ms** | **~60 ms** | `BuildAndMaterializeFastOnLargePointSet` |
| Covariance RMS vs Matérn ν=2 | 0.027 | **0.042** | `EmpiricalCovarianceMatchesMatern` |
| Reduced vs materialized `R_{ij}` | — | **< 1e-9 (rel)** | `ReducedProjectionMatchesMaterializedField` |
| Comonotone limit (K_s=1) | exact | **exact** | `ComonotoneLimitExactReduced` |
| CL-1e coverage (reduced path) | 1.000 | **1.000** (60/60) | `test_soft_rain_corr_coverage` |
| CL-1e width-ratio min/med/max | 0.482/0.711/0.878 | **0.485/0.700/0.827** | `test_soft_rain_corr_coverage` |
| CL-1e downstream narrowing (ℓ=30) | 0.701 | **0.608** | `test_soft_rain_corr_coverage` |

## 3. Interpretation

- **The generation win is the prize**: 64 s → ~60 ms (~1000×) one-time, making
  `CORR_LEN` practical on large meshes. This is delivered by the basis build
  regardless of whether the per-step path is reduced or materialized.
- **Per-step reduced projection** (`K_s < M`) replaces the O(M·k·n) materialized
  projection with O(K_s·k·n) + O(M·K_s·k). Per the CL-2a caveat, the total
  per-step ROM speedup is capped at ~19 % because quantile reconstruction
  (O(M·n)) is unaffected — the reduced projection is correctness-preserving,
  not the dominant per-step saving.
- **Numerical equivalence**: the reduced projection reproduces the materialized
  field's `R_{ij}` to machine precision (same basis + coefficients, differing
  only in summation order). Against the CL-1 rank-map reference (CL-1e), the
  SPDE path shifts the bands slightly tighter (Gaussian-linear marginals vs the
  rank map's exact family marginals) but stays inside every CL-1e threshold.

## 4. Reproduction

```
ctest --test-dir build/darwin-tests-local -R "test_engine_spde_spatial_basis|test_soft_rain_corr_coverage"
```

---

# Solver-mode compatibility: 2D ROM vs the explicit local-inertial marcher (W3)

Measured 2026-08-02. Harness: `tests/regression/test_2d_rom_marcher_coverage.cpp`
(registered, gated). Sweep provenance: scratch calibration tool, same fixture,
same MC; numbers below are from the registered harness itself.

## 1. Experiment

Steady-runoff plane — the operator's declared validity regime (sustained
friction-dominated Manning flow, Λ = r_f/ck ≳ 3), and the same configuration as
the marcher's own Manning-steady gate: 40×40 quad-split mesh (3 200 triangles,
5 m pitch), bed slope 0.002 toward a NORMAL_FLOW outlet, uniform rainfall
2·10⁻⁴ m/s, spun 3 000 s to steady state. M = 25 like-for-like members: LHS
strata of a ±20 % uniform Manning prior drive both a real marcher run (MC) and
ROM member i via `setExternalSamples`. Scored on per-cell depth over the last
10 minutes of a 30-minute window (fully saturated; deviation spread spins up
from zero by construction).

Four operator rungs, one code path (`DeviationOperator2D` → reduced k×k
`M = PᵀL_opP`, matrix-exponential advance):

| rung   | operator                                                        |
|--------|-----------------------------------------------------------------|
| legacy | diagonal λ·K_eff, ungrounded graph-Laplacian eigenvalues (historical convention) |
| iso    | physical FV diffusion, isotropic, grounded                      |
| aniso  | + flow-aligned tensor α∥ = 0.62, α⊥ = 2.0                        |
| adv    | + upwind advection c_k = (5/3)·u  ← **production rung**          |

k = 40 modes; D = h̄^{5/3}/(2n̄√S) refreshed per report; velocity from a
Green–Gauss surface-gradient estimate on readable state; operator reassembled
once per report interval (basis-update cadence, no re-eigensolve).

## 2. Results (measured, Debug build; identical to the -O2 sweep)

| rung   | coverage | width-ratio median | in [0.3,3] | floors met |
|--------|----------|--------------------|------------|------------|
| legacy | 1.000    | 0.456              | 0.665      | no (width) |
| iso    | 0.994    | 0.905              | 0.820      | yes        |
| aniso  | 0.994    | 0.923              | 0.822      | yes        |
| adv    | 1.000    | **1.430**          | **0.884**  | **yes — gated** |

Floors (initial, per the HSYM P4 rule): coverage ≥ 0.90, width-ratio median ∈
[0.5, 2], in-band ≥ 0.80. The production rung is `adv`; the harness gates it.

## 3. Findings established during validation

**(a) Open-boundary grounding is load-bearing — the 2D replay of the 1D
grounded-Laplacian fix (reform PR 4 / F2).** With a pure-Neumann basis, every
retained mode is zero-mean, and the dominant response to a domain-wide Manning
perturbation — a quasi-uniform shift of the steady profile — is invisible to
the basis: bands saturate at ≈ 0.39× the MC width *independently of every
diffusivity dial* (D×2 and D/2 moved the median by < 0.05). The Manning
fixed point δa_ss = (mm−1)·Pᵀh_det simply cannot see the mean shift.
Grounding outlet-adjacent cells (diagonal-only edge to a zero-deviation ghost,
in both the basis and the operator) restores it: 0.39 → 0.73 at k = 24 with
otherwise identical dials. Ground conductance is softened (×0.25): a
NORMAL_FLOW outlet is not absorbing — the member's own deviation persists at
the boundary — and full-strength grounding over-drains the near-outlet cells
(measured x-decile ratio 0.36 at the outlet vs 2.37 at the divide before
softening/mode increase).

**(b) Mode count k is a capture dial with a spatial signature.** The
width-ratio deficit concentrates near the outlet, where the profile has its
finest structure: per-x-decile medians ramped 0.36→2.37 (k = 24) and flattened
to 0.54→2.20 at k = 40, taking in-band from 0.77 to 0.88. Guidance: k ≈ 1 % of
cells on smooth steady fields; more for sharp ICs.

**(c) The ~1.4× width bias of the production rung is the conveyance-sensitivity
overshoot already documented in 1D (PR-10 median 1.25×).** The Manning
sensitivity responds with the full n-sensitivity of the conveyance while the
steady depth responds as n^{3/5} (≈ 0.6×), so saturated bands run wide — the
conservative direction.

**(d) The legacy diagonal convention (graph-Laplacian eigenvalues, no cell
area) under-spreads by ≈ 2× on this mesh** (median 0.456) because the fixed
point sits on an ungrounded basis (see (a)); its decay-rate scale error
(O(cell-size²) hidden factor) is masked at saturation but present in
transients. The physical FV convention replaces it on all reduced-operator
rungs.

## 4. Documented limitation: transient drain-to-pond (out of regime)

A bump-drains-to-pond fixture (closed box, off-centre Gaussian over a wet
floor, no sustained forcing) was measured with the same machinery: coverage
0.55–0.60, width median 0.25–0.34, streamwise bands ≈ 5× narrower than MC
(transverse ≈ 1.0 with advection — the transverse physics is right). This
fixture leaves the operator's validity envelope mid-window: the surface ponds,
u → 0 kills the advective Manning sensitivity, while the MC retains *frozen
positional spread* accumulated during the live transient — memory the
deviation-decay operator does not carry. This is the Λ ≲ 3 breakdown regime
the local-inertial derivation itself flags (thin films, dry fronts, vanishing
flow), and the 2D counterpart of PR-10's documented front-arrival-timing
limitation. Bands in ponding transients should be treated as indicative only;
the gate applies where the operator claims validity.

## 5. Reproduction

    ctest --test-dir build/<dir> -R regression_2d_rom_marcher_coverage
    # or directly:
    OPENSWMM_2D_BACKEND=cpu ./tests/regression/test_2d_rom_marcher_coverage

Runtime ≈ 100 s (Debug): 26 marcher runs × 4 800 s simulated on 3 200 cells.
All calibrated constants live at the top of the harness; the floors are the
meter — recalibrate the dials, never the floors.
