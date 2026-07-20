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
