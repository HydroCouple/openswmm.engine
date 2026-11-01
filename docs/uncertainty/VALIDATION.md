# Interval-coverage audit of every ROM-vs-MC gate (P7, 2026-10-04)

Follow-up to the SP3 cross-vendor review. Two metric defects were shared by
every 1D ROM-vs-MC gate written since PR 10: the 21-member MC "q95 − q05" was
`h[19] − h[1]` (the 0.071–0.929 span, ~11% narrow), and "coverage" meant *the MC
median lies inside the ROM band*. The second is close to tautological under the
deviation form: the ROM median tracks the deterministic run by construction and
the MC median sits near it, so any band of nonzero width "covers". All gates now
use midpoint plotting positions (`tests/regression/mc_quantiles.hpp`, the ROMs'
own convention) and report **empirical member coverage**: the fraction of MC
members inside the ROM's q05–q95 band, nominal 0.905 for 21 strata midpoints
(≈0.92 for the 2D test's 25). **Gate verdicts did not change.** The numbers did.

## 1. Every MC cell, corrected — with the C1 status

**C1 decision (owner, 2026-10-05) and C2 (owner, 2026-10-06, option 1).** A
cell is **calibrated** when its empirical member coverage is ≥ 0.80 (the
21-member LHS ceiling is 0.905; 0.80 leaves one member of granularity) **and**
its median ROM/MC width ratio is ≤ 1.5. Coverage ≥ 0.80 with width > 1.5 is
**conservative (over-wide)**: enough outcomes inside, by being too wide. Coverage
below 0.80 is **ranking only**: the band still shows where uncertainty
concentrates and which nodes are more uncertain than others, but is not a 90%
interval. Conservative and ranking-only cells stay registered, print their
numbers and their fix PR (every run prints a `[C1/C2]` line per cell), and are
never loosened. Cells that claim "validated" assert the floor; the ceiling is a
label, applied uniformly from `tests/regression/mc_quantiles.hpp`.

| Gate / cell | Width ratio median, old → corrected | Member coverage | Gate verdict | **C1 status** |
|---|---|---|---|---|
| PR-10 free-surface chain | 0.102 → 0.097 (old fixture) → 1.961 (P8 fixture) → 1.177 (H5b) → **1.210** (H14) | 0.20 → 0.94 → 0.867 → **0.993** | **green since P8** | **calibrated** since H5b (`alpha_free = 0.6` closes most of H13); H14 gates its steep reaches too |
| H5 surcharged, EXPLICIT | 0.033 → 0.033 → 0.694 (H5b) → **0.781** (H14) | 0.38 → 0.69 → **0.73** | **green on H5's bounds since H14** | ranking only (floor unmet), documented limitation |
| H5 surcharged, SEMI_IMPLICIT | 1.031 → 0.982 → 5.446 (H5b) → **1.208** (H14) | 0.51 → 0.842 → **0.832** | **green since H14** (ceiling 3.0 never moved) | **calibrated**; J3 (above the pool) still 11× over — basis truncation on a 5-node fixture, see H14 §6 |
| H11 front passage, phase coordinate | 1.354 → 1.295 → **1.290** (H5b) | 0.83 → **0.829** | passes [0.5, 2] | **calibrated** (0.83 ≥ 0.80, 1.29 ≤ 1.5; floor asserted) |
| H11 amplitude-only baseline (ungated) | 0.009 → **0.009** | **0.05** | ungated | superseded by H11 |
| SR-5 soft rain, FULL | 0.822 → 0.710 → **0.976** (SR-6) | 0.76 → **0.88** | passes | **calibrated** since SR-6 (floor asserted) |
| CL-1e soft rain, CORR_LEN | 0.659 → 0.570 → **0.804** (SR-6) | 0.68 → **0.83** | passes | **calibrated** since SR-6 (floor asserted) |
| W3 2D marcher, production "adv" rung | 1.321 (same-index rule; 1.291 vs midpoint span) | **0.82** | passes | **calibrated** (0.82, 1.32 ≤ 1.5; floor asserted); the 1.32 = 1/0.6 elasticity × ≈0.8 basis capture — **H13 (2D)**, dial shipped default 1.0 |
| W3 2D marcher, iso / aniso rungs | 0.835 / 0.857 | **0.66 / 0.67** | pass | reference rungs, ungated |
| W4 plane, correlated rain ℓ = 20 / 100 m (adv) | **1.31 / 1.42** | **0.90 / 0.89** | floor asserted | **calibrated** — first MC behind 2D CORR_LEN |
| W4 plane, correlated Manning ℓ = 20 / 100 m | 0.68 / 1.24 (diagonal, pre-H15) → **0.92 / 1.32** (H15 per-member M_i) | 0.65 / 0.84 → **0.80 / 0.84** | measured, not asserted | calibrated at both lengths after H15; the 20 m cell by 0.001 |
| W4 channel (thalweg), comonotone Manning, adv | **2.93** | 0.95 | measured | **conservative** → H13 (2D) + W4b |
| W4 channel, correlated rain ℓ = 20 m (adv) | 1.60 | 0.91 | measured | conservative |

(The 2D test compares 25 like-for-like members under the same `round(p·(M−1))`
rule on both sides, so its gated width ratio is a fair same-rule comparison and
was left as the gated quantity; the midpoint-span ratio is reported beside it.)

## 2. What the member coverage says that the width ratio did not

- **H5 SEMI_IMPLICIT: median width ratio ≈ 1 but member coverage 0.51.** The
  band is the right width *on median* yet misses half the outcomes. The width
  ratio ranges 0.000–2.67 across samples, so "lands almost exactly on 1.0" (the
  H5 record) described the median of a very wide distribution, not a calibrated
  band. The band is mis-centred or mis-sized sample by sample. This is the most
  consequential correction in the table.
- **W3 2D "adv": over-wide (1.3×) yet member coverage only 0.82.** Same pattern:
  width is not the whole story; centring is.
- **H11 phase coordinate** is the best-calibrated cell measured (0.83), and the
  amplitude-only baseline (0.05) shows how badly the pre-H11 band missed.
- **Soft-rain paths** under-cover (0.76 / 0.68) by exactly the amount their width
  deficit predicts; they are not mis-centred.

## 3. Member count is a cost knob, not a calibration knob (SR-5 sweep)

| M (ROM members) | width ratio min / med / max | member coverage |
|---|---|---|
| 20 | 0.632 / 0.710 / 0.751 | 0.761 |
| 50 (default) | 0.632 / 0.710 / 0.751 | 0.761 |
| 100 | 0.632 / 0.710 / 0.751 | 0.761 |
| 200 | 0.632 / 0.710 / 0.751 | 0.761 |

Identical to three decimals. The ROM's q05/q95 sit on exact LHS strata midpoints,
and the forcing channel is linear in the per-member coefficient, so M changes
nothing about the band here except cost (O(M·k) per step). M = 20 reproduces
M = 200. Raising M will not improve coverage on this path.

## 4. The soft-rain under-dispersion is a formulation constant (SR-5 sweeps)

| CV | width ratio min / med / max | member coverage |
|---|---|---|
| 0.10 | 0.629 / 0.708 / 0.749 | 0.762 |
| 0.20 | 0.632 / 0.710 / 0.751 | 0.761 |
| 0.40 | 0.640 / 0.720 / 0.760 | 0.762 |

Independent of CV, so **not** a nonlinearity of the rain → runoff → routing
response (that would grow with CV). Per report time (CV 0.20): the ratio is
0.65 at t = 300 s, peaks at 0.74 around 1500 s, and drifts down to 0.68 at
3600 s, while both ROM and MC widths grow steadily. A near-constant factor of
≈0.7 present from the first report, with a mild late decline, and insensitive to
CV and M, is a property of how the forcing channel is formulated: the
deviation-form modal ODE dissipates at `λ_j·K1d` while the storage-dominated
fixture integrates rain-rate deviations without loss, so the ROM carries less of
the accumulated forcing uncertainty than the physical system does. That is a
testable lead (the ratio should move with K1d), not yet tested. It belongs to
whoever revisits the forcing-channel formulation (F/O48 class), not to a test
tolerance.

## 5. The calibration rule — decided, two-sided

C1 (2026-10-05): floor, member coverage ≥ 0.80, asserted by validated cells.
C2 (2026-10-06, option 1): ceiling, median width ratio ≤ 1.5, a label. Both
applied uniformly in one commit each; see the status column in §1. Current
state: **calibrated** H11, 2D production rung, SR-5, CL-1e; **conservative**
the P8 free-surface chain (1.96, → H13); **ranking only** H5 SEMI_IMPLICIT
(→ H5b), H5 EXPLICIT (limitation), the 2D reference rungs. The P8 cell is the
reason C2 exists: C1 alone called it calibrated at coverage 0.94 while it was
twice too wide. No bound was lowered anywhere to make a cell pass.

## 6. Reproduction

    build/<dir>/tests/regression/test_rom_coverage            # all five 1D cells
    build/<dir>/tests/regression/test_soft_rain_coverage      # SR5_CV=… SR5_MEMBERS=… SR5_TRACE=1
    build/<dir>/tests/regression/test_soft_rain_corr_coverage
    build/<dir>/tests/regression/test_2d_rom_marcher_coverage

---

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

> *Correction 2026-10-04 (interval-coverage audit, top of this file): this section's "coverage" was median containment and its width ratios used the 0.071–0.929 MC span. The current fixture's corrected median width ratio is 0.097 with member coverage 0.20 (the deliberately red result); the 1.251 headline below was never reproducible on either base.*

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
3. **Front-arrival timing spread is not captured by the amplitude channel
   alone.** In the same surcharged variant, a filling front's arrival time
   varied ~minutes across MC members, producing transient 2–4 m empirical
   widths at front passage that a pure amplitude-sensitivity method (anchored
   to the deterministic trajectory) does not represent. **PR H11's per-member
   phase coordinate closes most of this gap** — see the "Per-member phase
   coordinate (PR H11)" section below for the measured recovery on a
   dedicated front-passage fixture (median width ratio 0.009 → 1.354).
   H11 is 1D only; the 2D counterpart (drain-to-pond, `H11b`) is unaffected
   and remains open — see §4 of the 2D marcher-compatibility section above.
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

> **Metric correction, 2026-10-04 (cross-vendor review of PR SP3).** Two defects
> in this gate's metrics, shared by its correlated sibling and by the PR-10
> harness idiom: (1) the MC "q95 − q05" was `h[19] − h[1]` of 21 members, the
> 0.071–0.929 span, ~11% narrower than the true 5–95 span, so every width ratio
> below was biased **high by ~12%**; (2) "coverage" counted only whether the MC
> **median** lay inside the ROM band, which is median containment, not interval
> coverage. Both soft-rain gates now use midpoint-plotting-position quantiles
> (the ROM's own convention, `tests/regression/mc_quantiles.hpp`) and also report
> the empirical fraction of MC members inside the ROM band. Corrected numbers are
> in §2a. The PR-10 harness (`test_rom_coverage.cpp`, three sites) was corrected the
> same day (P7); every cell's corrected numbers and the empirical member
> coverage of ALL nine MC cells are in the "Interval-coverage audit" section.

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

### 2a. Re-port check on the marcher line (PR SP1, 2026-10-03)

The gage-level path was re-ported onto `port/v2-on-marcher` (two-plane API,
`[SOFT_RAINGAGES]` parser and 1D wiring restored), and this same test was
registered and run **with no expectation edits**:

| Metric | July (pre-port line) | SP1 re-port |
|---|---|---|
| Coverage | 1.000 (60/60) | **1.000** (60/60) |
| Width ratio in [0.3, 3.0] | 1.000 (35/35) | **1.000** (35/35) |
| Width ratio min / median / max | 0.754 / 0.821 / 0.872 | ~~0.731 / 0.822 / 0.869~~ (old idiom) |

**Corrected (2026-10-04, midpoint quantiles):** width ratio **0.632 / 0.710 /
0.751**; median containment 1.000; **empirical member coverage 0.761 overall,
0.769 in the saturated regime, against a nominal 0.905** (19 of 21 strata
midpoints lie in [0.05, 0.95]). The under-coverage is the width under-prediction
seen from the other side: a band 0.71× the MC width around a normal spread
captures about 0.76 of the mass, which is what was measured. Nothing is gated on
member coverage yet; a floor for it is a validation-design decision (see the
checklist's open decisions), not something to invent to make the gate green.

The median reproduces to three digits. The min and max moved by about 0.02 and
0.003, so the run is close to but **not bit-identical** with July's. The base
line changed underneath (explicit marcher era, NODE_CONTINUITY, Anderson fixes),
and I did not isolate which change moves the extremes; the acceptance floors are
cleared with wide margin either way. Scope is unchanged: single `NORMAL` gage,
free-surface chain. Mixed-family networks are covered by structural tests
(both coefficient planes reach the ROM, no fallback), not by an MC comparison.

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

# SP4 — RUNOFF / INFLOWS grid targets re-ported; three runtime-forcing defects fixed (2026-10-05)

SP4 re-ports SR-2d (`SWMMEngine::initSoftGridRuntimes` / `stageSoftGridForcings`)
verbatim and registers its two dormant gates (`test_engine_soft_rain_grid_targets`,
2 cases; `test_engine_soft_rain_grid_engine`, 3 cases). Getting the end-to-end
gate green required fixing three defects in the engine's runtime-forcing path
that are **not SP4-specific**: every `swmm_forcing_node_lat_inflow()` user hit
them. All three act only when a forcing is staged, so parity runs are
bit-identical (full gate 140/142 with only the two known reds).

| Defect | Symptom measured | Fix |
|---|---|---|
| Runoff clock ignored a grid rainfall override | grid RUNOFF run used the dry step while the gage control used the wet step; continuity errors −0.161 vs −0.043 with identical rainfall totals | `is_raining` also true when a subcatchment rainfall forcing > 0 is staged (the pre-port SR-2d fix, missing here) |
| ADD forcing with RESET persistence accumulated | forced lateral inflow grew 0.00057, 0.00113, 0.0017, … every step | transient forcings live in a new per-step `nodes.forcing_lat_flow`, rebuilt by `applyForcings()` each step; `user_lat_flow` is the API setter's persistent value only |
| Forcing value consumed in the wrong units | 0.02 CMS staged as 0.02 cfs | converted once in `applyForcings()` with `UCF(FLOW)`; the API documents project units and now gets them |
| Forced volume outside the continuity total | routing continuity error **−4371** (forced volume over initial storage) | `routing_error()` includes `routing_forcing_inflow` |

**A base-level residual, flagged, not fixed.** With all three fixed, the grid
INFLOWS run and an equivalent `[INFLOWS]`-timeseries control close to the
*same* routing continuity error, **−0.1426**, carrying the same 211.864 ft³.
That is a 14% continuity error on a 5-minute, one-junction, one-conduit model
with constant inflow, independent of how the inflow is supplied. The gate now
asserts grid = control (error to 1e-9 absolute, volume to 1e-6 relative; the
two unit-conversion paths differ at 2e-8), replacing the pre-port absolute
`|error| < 1e-4`, which this base cannot meet for any inflow path on this
fixture. Candidate follow-up: the early-time storage accounting on short runs.

Reproduction: `ctest -R 'soft_rain_grid_(engine|targets)'`; `SP4_TRACE=1` on
the engine binary prints per-step inflow and every mass-balance total.

---

# P8 — Free-surface MC fixture redesigned; the saturated band is ~2× over-wide (2026-10-05)

The PR-10 free-surface chain was deliberately red from 2026-08-04: its 100 m
conduits give the dominant mode a time constant of **10.2 h** (measured in-run
from the basis eigenvalue λ₀ = 0.0807 and the K1d actually used, 3.38e-4 1/s),
so its 1 h window sampled 9% of the band and reported a spin-up artefact
(P4 root cause). P8 changed **one** geometric parameter, conduit length
100 m → 10 m, and samples the second half of a 4 h window. Everything else
(5 m drop per conduit, 1 m diameter, 0.1 CMS inflow, ±20% prior, M = 50) is
the original. The test now prints τ₀ and the saturation at the sampling
window on every run, and the old geometry is kept as an ungated
spin-up demonstration.

## 1. Geometry search (all measured; only the chosen row is the fixture)

| Length | DWF | Window | τ₀ | Saturation at window midpoint | Width ratio min / med / max | Member coverage |
|---|---|---|---|---|---|---|
| 100 m (old) | 0.1 | 1 h | 10.2 h | 0.05 | 0.018 / 0.097 / 1.02 | 0.20 |
| 30 m | 0.1 | 1 h | 2.7 h | 0.31 | 0.045 / 0.498 / 1.50 | 0.44 |
| 10 m | 0.1 | 2 h | 0.85 h | 0.69 | 1.24 / 1.75 / 2.02 | 0.88 |
| **10 m (P8)** | **0.1** | **4 h** | **0.85 h** | **0.90** | **1.75 / 1.96 / 2.14** | **0.94** |
| 10 m | 0.3 | 4 h | 0.36 h | 0.996 | 1.97 / 2.06 / 2.20 | 0.98 |
| 20 m | 0.3 | 6 h | 0.73 h | 0.98 | 1.93 / 2.04 / 2.19 | 0.97 |
| 100 m (old) | 0.1 | 30 h | 10.2 h | 0.77 | 1.47 / 1.89 / 2.11 | 0.90 |

The ratio climbs with saturation toward **≈2.0** on every geometry, including
the old one given enough time. That is the answer the old fixture could never
reach.

## 2. Finding: the free-surface Manning channel over-predicts by ~2× at saturation

At saturation the 1D band is about twice the brute-force band and
over-covers (0.94–0.98 of outcomes inside, nominal 0.905). This is the
conveyance-sensitivity over-prediction already on record for 2D (W3 production
rung, 1.43×) and in the original PR-10 write-up (1.25×, on numbers that were
never reproducible), now measured cleanly in 1D. The deviation-form fixed point
`δa = (mm−1)·b_j` responds to a Manning perturbation with the full conveyance
sensitivity, while the hydraulics settle a mass-balance-set head that moves
less. It is the free-surface sibling of H5's surcharged over-prediction. Not
fixed here; logged for the formulation queue (candidate **H13**).

## 3. Gate status

Verdict: **green**, with no bound moved. The standing bounds (median
containment ≥ 0.95, width ratio in [0.3, 3] for ≥ 0.95 of samples, median
width ratio in [0.5, 2.0]) are kept, and the C1 member-coverage floor is now
asserted. Under C2 (2026-10-06) the cell is labelled **conservative
(over-wide)**, not calibrated: 1.96 is above the 1.5 ceiling. The measured median 1.96 sits just under the 2.0 ceiling; the
deeper-inflow variant reads 2.06 and would fail it. That is reported here
rather than avoided by picking a different fixture: the fixture was chosen as
the smallest change from the original that reaches saturation, not for its
number.

**A gap in C1 this exposes.** The member-coverage floor is one-sided. A band
twice too wide passes it comfortably. Calibration needs an upper bound too;
the existing width-ratio ceiling plays that role here by accident of history.
The checklist carries this as decision **C2**: make calibration two-sided
(member coverage within a band around nominal, or coverage ≥ 0.80 **and**
width ratio ≤ 1.5), applied uniformly.

## 4. Reproduction

    build/<dir>/tests/regression/test_rom_coverage --gtest_filter='RomCoverage.*'
    P8_LEN=100 P8_WINDOW_H=1 build/<dir>/tests/regression/test_rom_coverage --gtest_filter='RomCoverage.Bands*'   # the old red

---

# SR-6 step 1 — the soft-rain shortfall is the rain-to-runoff elasticity (2026-10-05)

Falsification sweeps on the SR-5 gate (`test_soft_rain_coverage`, env hooks
`SR5_K1D_SCALE`, `SR5_PIPE_DIAM`, `SR5_SUBCATCH_WIDTH`, `SR5_SUBCATCH_AREA`;
defaults reproduce the gate bit-for-bit). Numbers are the median ROM/MC width
ratio and the empirical member coverage.

## 1. Two hypotheses killed

| Knob | Range | Width-ratio median | Member coverage | Verdict |
|---|---|---|---|---|
| K1d scale (modal dissipation) | ×0.1 … ×10 | 0.710 → 0.699 | 0.764 → 0.746 | **not the lever** |
| Pipe diameter (surcharge) | 0.5 … 8 ft (max fill 0.23 → 0.01) | 0.710 → 0.723 | 0.761 → 0.787 | **not the lever** |

The forcing channel's dissipation and the pipe hydraulics are both irrelevant
to the shortfall. A direct decomposition at J1, t = 3600 s, explains why:

| Quantity | Measured |
|---|---|
| ROM band ÷ (3.29·CV·Δh_det), i.e. ROM vs *proportional scaling of the deterministic head rise* | **1.008** |
| MC band ÷ (3.40·CV·Δh_det), i.e. MC vs the same | **1.390** |
| Inflow volume ÷ stored volume (outflow share) | 1.017 |
| **Runoff elasticity to rain**, ln(V_hi/V_lo)/ln(rain_hi/rain_lo) from the extreme members | **1.395** |

The ROM does exactly what its formulation says: the band is a proportional
scaling of the deterministic response. The Monte Carlo heads move 1.39× more
than proportionally, and that factor is the **runoff** elasticity: on this
fixture the subcatchments (5 acres, 5 ft flow width) never reach equilibrium,
runoff delivered by t = 1 h is 12% of rain × area, and on the rising limb of
Manning overland flow (Q ∝ d^{5/3}) the elasticity of delivered runoff to rain
sits between 1 and 5/3. Widening the flow path to 5,000 ft raises it to
**1.64** (≈ 5/3). The soft-rain mapping `spread = dh/dt · CV` assumes runoff
scales one-for-one with rain, i.e. elasticity 1.

## 2. The confirming experiment: let runoff reach equilibrium

| Subcatchment | runoff ÷ rain at 1 h | runoff elasticity | width-ratio min / med / max | member coverage |
|---|---|---|---|---|
| 5 ac, 5 ft (the gate) | 0.12 | 1.395 | 0.632 / 0.710 / 0.751 | 0.761 |
| 0.05 ac, 5 ft | 0.84 | 1.061 | 0.042 / 0.864 / 0.957 | 0.763 |
| 0.005 ac, 5 ft | 0.91 | 1.005 | 0.916 / **0.945** / 0.975 | **0.893** |

*(Correction 2026-10-05: these two rows were first recorded as 5,000 ft wide;
the shell sweep had passed two settings as one, so the width stayed at the
fixture's 5 ft. A smaller area reaches equilibrium at any width, so the
conclusion is unchanged; the labels were wrong.)*

With runoff proportional to rain, the soft-rain ROM is calibrated (0.893
against the 0.905 ceiling) with no change to the ROM. This also explains the
earlier observations exactly: constant elasticity is CV-independent (a power
law), M-independent (the band is a scaled deterministic response), K1d- and
pipe-independent (it sits upstream of routing), and only mildly
time-dependent (the subcatchments slowly approach equilibrium).

## 3. The fix (SR-6, 2026-10-05) and its result

Implemented as design (A), refined: a **single-step trial cannot see the
elasticity** (most of it is in the accumulated ponded depth, not in one step's
rain), so the engine carries **two persistent perturbed runoff states** at
rain × (1 ± 0.1), advanced through the unmodified production kernel every
runoff step with the same arguments as the deterministic call
(`uncertainty::RunoffElasticityProbe`, `RunoffSolver::saveState/restoreState`,
a `rain_scale` argument that is a no-op at 1.0). Per subcatchment,
`E_s = ln(q₊/q₋) / ln(1.1/0.9)`, and the soft-rain spread uses `CV·E_s`
instead of `CV`. Active only when gage-level soft rain is configured. Cost:
two extra runoff-kernel calls per *runoff* step.

| Cell | Before SR-6: width ratio min / med / max, member coverage | After SR-6 | C1 status |
|---|---|---|---|
| SR-5 soft rain, FULL (CV 0.20) | 0.632 / 0.710 / 0.751, **0.761** | 0.863 / **0.976** / 0.984, **0.881** | **calibrated**, asserted |
| CL-1e soft rain, CORR_LEN (CV 0.40, ℓ = 120 m) | 0.379 / 0.570 / 0.700, **0.684** | 0.543 / **0.804** / 0.942, **0.827** | **calibrated**, asserted |
| CL-1e downstream narrowing, ℓ = 30 m | 0.563 | 0.565 | the feature's point, unchanged |

Robustness of SR-5 with the correction on: CV 0.10 → 0.886, CV 0.40 → 0.901;
5,000 ft flow path (elasticity 1.64) → 0.851; equilibrium subcatchments →
0.902; M = 20 → 0.881 (identical to M = 50). With the correction off
(`SR5_RUNOFF_ELASTICITY=0`) every pre-SR-6 number reproduces exactly.

**Deterministic path bit-identical, tested**: end-of-run node heads, runoff
statistics accumulators and runoff rates are `EXPECT_EQ`-equal with the probe
on and off (`RunoffElasticityProbeLeavesDeterministicPathBitIdentical`), and a
model with no `[SOFT_RAINGAGES]` never seeds the probe.

**Not covered (documented approximations)**: LID surface runoff is added to
the subcatchment runoff after the kernel and is not perturbed; cascaded
subcatchments' members read the deterministic run-on. Both leave E_s at the
non-LID, non-cascaded value. Design (B), the full `RunoffEnsemble`, was not
needed: the gates' brute-force Monte Carlo *is* the exact reference, and (A)
reaches 0.88 against a 0.905 ceiling.


---

# Correlated soft rainfall re-port on the marcher line (PR SP3)

Measured 2026-10-03. SP3 re-homes CL-1c/CL-2c (`COHERENCE CORR_LEN`) onto
`port/v2-on-marcher` for both the gage path (SP1) and the grid path (SP2), using
the CL-2b analytic SPDE basis, which SP3 added to the engine library. The CL-1e
gate (`tests/regression/test_soft_rain_corr_coverage.cpp`) was registered and run
**with no expectation edits**.

## 1. Results (measured)

| Metric | July (pre-port line, CL-1e) | SP3 re-port |
|---|---|---|
| Median containment, ROM [q05,q95] ∋ correlated-MC median | ≥ 0.90 floor | **1.000** (60/60) |
| **Empirical member coverage** (fraction of MC members inside the ROM band; nominal 0.905) | not gated | **0.684** overall, **0.652** saturated |
| Width ratio in [0.3, 3.0], saturated regime | ≥ 0.80 floor | **1.000** (35/35) |
| Width ratio min / median / max (midpoint quantiles) | see CL-1e section | **0.379 / 0.570 / 0.700** |
| J5 band, correlated (ℓ = 30 m) ÷ comonotone | < 1 required | **0.563** |

*Corrected 2026-10-04 after cross-vendor review; the first-run values 0.436 /
0.659 / 0.806 used the biased `h[19] − h[1]` MC width and "coverage" meant
median containment (see the SR-5 section's correction banner).*

The MC reference is built from `CorrelatedFieldGenerator` (the CL-1 rank/copula
field) while the engine now uses the SPDE Matérn ν = 2 basis, so this is a
cross-method comparison, not a self-check. The ROM median band is about 0.57 of
the correlated MC band, narrower than the comonotone case's 0.71 (SR-5), and the
member coverage of 0.65–0.68 against a nominal 0.905 is that under-prediction
seen from the other side. Correlated bands from this path are therefore
**narrower than the reference by a factor the gate's floors do not catch**; the
floors were set on width-ratio bands and median containment.

## 2. Findings

- **The 2D correlated path WIDENS the band on the 4-cell test mesh (2.02×).**
  On the SP2 live-ROM fixture, `CORR_LEN 0.1 m` (far below the ~1.5 m centroid
  spacing) gives band 9.73e-4 against 4.82e-4 comonotone. Comonotone rainfall
  uncertainty on a tiny, nearly flat patch is mostly a uniform depth shift, which
  lives in the discarded constant eigenmode; independent per-cell coefficients
  project more onto the retained zero-mean modes. The 1D chain narrows downstream
  (0.563 above) because uncertainty accumulates along the flow path. The two are
  not in conflict; the test records the direction rather than asserting it.
  **Update 2026-10-07 (W4)**: a correlated marcher MC now exists — the 2D
  CORR_LEN rain path measures 0.897 / 0.886 member coverage at ℓ = 20 / 100 m
  on the W3 plane (both branches); see the W4 section.
- **Reduced vs materialized is decided by K_s against M.** On 4 points the SPDE
  basis retained K_s = 64 ≥ M = 20, so the 2D fixture takes the materialized
  field branch; the 1D chain and the CL-2a profile mesh take the reduced one.
  Both are the correlated path.
- **`initialize()` used to swallow init-time hard errors.** `set_error()` puts
  the engine in ERROR_STATE, but `initialize()` then overwrote that with
  INITIALIZED and returned OK, so `start()` proceeded. This affected the
  pre-existing 2D-init failure path too. SP3 makes `initialize()` return the
  error instead. Behaviour change, deliberate, called out in the checklist.
- **"No coordinates" cannot be detected by emptiness** on this line:
  `PostParseResolver` zero-fills `spatial.node_x/y` whether or not
  `[COORDINATES]` was present. Detected as a zero-extent point set instead;
  falls back to FULL with one warning and reproduces the FULL output exactly
  (test `CorrLenWithoutCoordinatesWarnsAndFallsBackToFull`).
- **CL-2a profile after SP3** (`regression_corr_len_profile`, same 9.8k-triangle
  mesh): comonotone advance 1.52 ms/step, correlated advance 14.82 ms/step,
  quantile reconstruction 18.81 ms/step, projection delta 13.31 ms/step. The gate
  passes; the quantile phase is no longer the dominant term after H4.

## 3. Reproduction

    ctest --test-dir build/<dir> -R 'regression_soft_rain_corr_coverage|test_engine_soft_rain_gage_engine|test_engine_soft_rain_grid_2d'

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

# Two-channel 2D deviation: Manning fractional, forcings absolute; dials retired (2026-10-11)

**Branch** `hsym2/t1-relative-default`. Goal set by the owner: one behaviour,
no tuning switches, so the sidecar is handled consistently.

## 1. What making H16's relative mode the default exposed

Flipping the default and running every 2D gate showed H16 had only ever been
measured on Manning cells. Its relative reconstruction multiplied every
ADDITIVE forcing by h_det a second time:

| Cell | Absolute | H16 relative (as shipped) |
|---|---|---|
| plane rain ℓ = 20 m | 0.894 / 1.316 | **0.120 / 0.082** |
| plane rain ℓ = 100 m | 0.883 / 1.415 | **0.165 / 0.109** |
| channel rain ℓ = 20 m | 0.938 / 1.867 | **0.116 / 0.048** |

Three corrections were measured in turn, each for a stated reason:

1. **Forcings divided by h_det, plain operator**: rain over-wide (1.81 / 2.35 /
   2.88). The operator was assembled for absolute deviations; applied to
   fractions it integrates rain/h along the flow path and over-counts shallow
   upstream cells.
2. **Similarity transform Pᵀ·H⁻¹·L·H·P**: plane right (rain 0.886 / 1.135,
   0.890 / 1.400; Manning 0.835 / 1.320) but the channel diverged (widths
   ~1e41). Exact before truncation; after it, non-normal enough at a 400:1
   depth contrast to grow.
3. **Galerkin projection onto span(H·P)**, M = G⁻¹(HP)ᵀL(HP), forcings
   G⁻¹Pᵀ(h ⊙ s): stable and right for Manning everywhere (plane 0.864 / 1.360,
   channel 0.818 / 1.308) — but channel rain collapsed (0.047 / 0.032): a
   depth-weighted representation cannot hold rain landing on a 3 mm film.

**Reading.** Manning uncertainty is multiplicative and wants fractional
coordinates; additive forcings want absolute coordinates. One coordinate
system for both fails one of them.

## 2. The design

Each member carries two coefficient sets (SpectralROM::twoChannel()):
- Manning / RATE_MULT, fractional, propagated with the Galerkin operator of
  §1.3 (DeviationOperator2D::similarity_h, setRelativeGram);
- rain, soft rain, FORCING_VECTOR fields and 1D↔2D coupling, absolute,
  propagated with the plain operator (setAdditiveOperator) — exactly the
  earlier absolute ROM.

h_i = h_det·(1 + P·δa_i) + P·δa_add_i. Linear, so the split is exact to
first order. A closed domain (no open boundary) keeps the absolute form for
Manning: forcing relative there collapsed its band 256× (router test).

## 3. Results (member coverage / median width ratio)

| Cell | Absolute (rebased) | Two-channel |
|---|---|---|
| **W3 asserted gate (plane Manning)** | 0.800 / 1.261 | **0.864 / 1.360** |
| channel Manning | 0.912 / 2.460 | **0.818 / 1.308** |
| plane Manning ℓ = 20 m (H15) | 0.784 / 0.892 | **0.907 / 1.463** |
| plane Manning ℓ = 100 m (H15) | 0.831 / 1.283 | 0.906 / 1.516 (conservative) |
| plane rain ℓ = 20 m / 100 m | 0.894 / 1.316, 0.883 / 1.415 | identical |
| channel rain ℓ = 20 m | 0.938 / 1.867 | identical (conservative) |

Every Manning cell is now calibrated or conservative; the asserted gate's
margin over its 0.80 floor goes from 0.000 to 0.064. Rain cells are
unchanged by construction (unit test: rain-only two-channel ≡ absolute ROM to
1e-12). Channel rain stays 1.87× wide — the one uncalibrated 2D cell, same
cause W4b named for Manning (absolute deviation through smooth modes), not
addressed here.

## 4. Step 2 of the plan dissolved

The plan was to replace the global elasticity constant by one derived from
each cell's geometry, because the plane and channel wanted different values.
Under the two-channel form both calibrate with the same constant (1.0), so
that work is not needed. The operator dials (LEGACY_OPERATOR, ALPHA_PAR,
ALPHA_PERP, C_FACTOR, GROUND_SCALE, MANNING_ELASTICITY, DIFFUSIVITY_SCALE,
RELATIVE_DEVIATION, K_EFF) are no longer .inp settings: a line restating the
built-in value loads, any other value is refused with the reason. The C++
fields remain as internal test hooks.

## 5. Reproduction

    W4_CELLS=<cell> OPENSWMM_2D_BACKEND=cpu build/<dir>/tests/regression/test_2d_rom_marcher_coverage_corr
    W4_RELATIVE=0 ...   # the absolute form, for comparison
    W3_RELATIVE=0 build/<dir>/tests/regression/test_2d_rom_marcher_coverage

# Rebase onto swmm6_rel 2026-10-09: every MC gate re-measured (2026-10-11)

**Branch** `hsym2/stack-on-swmm6rel`. The whole stack (48 first-parent commits,
each merged PR replayed as one commit) was moved from a base 725 upstream
commits old onto `origin/swmm6_rel` `a070e98c`. Upstream had rewritten its
history after 2026-07-31; its `d15e8ab7` has the old base's exact tree.
Adaptation to upstream (mixed triangle/quad meshes, the infiltration bank,
Kdecay in 1/s, stricter input validation) is commit `4ad71f3c`.

**Every gate passes. No bound, floor or tolerance was changed.** Numbers,
member coverage / median width ratio:

| Cell | Before rebase | After | Note |
|---|---|---|---|
| P8 free-surface chain (1D) | 0.993 / 1.21 | 0.993 / 1.210 | unchanged |
| H5 SEMI_IMPLICIT surcharged (H14) | 0.832 / 1.21 | 0.845 / 1.317 | still CALIBRATED |
| H5 EXPLICIT | 0.73 / 0.78 | 0.736 / 0.782 | documented limitation |
| H11 front, phase coordinate | 0.829 / 1.290 | 0.831 / 1.308 | unchanged |
| SR-5 soft rain, FULL | 0.881 / 0.976 | 0.881 / 0.976 | identical |
| CL-1e soft rain, CORR_LEN | 0.827 / 0.804 | 0.827 / 0.804 | identical |
| **W3 2D production (asserted)** | 0.822 / 1.321 | **0.800 / 1.261** | **margin over the 0.80 floor now ~0** |
| W4 plane rain ℓ = 20 m / 100 m | 0.897 / 1.31, 0.886 / 1.42 | 0.894 / 1.316, 0.883 / 1.415 | unchanged |
| **H15 plane Manning ℓ = 20 m** (not asserted) | 0.801 / 0.92 | **0.784 / 0.892** | now RANKING ONLY |
| H15 plane Manning ℓ = 100 m | 0.844 / 1.32 | 0.831 / 1.283 | CALIBRATED |
| W4 channel comonotone Manning (adv) | 0.95 / 2.9 | 0.912 / 2.46 | CONSERVATIVE, narrower |
| Bellinge lock, band/depth p50 / max | 0.316 / 44.5 | 0.303 / 42.3 | 24.00 h reached |

**Reading.** The 1D cells and the soft-rain cells are unchanged to printed
precision: the deterministic 1D solver they anchor on was not changed
materially for them. The 2D cells all moved the same way. The legacy, iso
and aniso reference rungs dropped by 0.015-0.02 coverage alongside the
production rung. That is upstream's marcher work (wet/dry hysteresis band,
resting-face and source-ledger changes) shifting the MC reference itself,
not a change in the ROM. Two consequences, both left as findings:

1. The W3 production gate passes with no margin. Any further marcher change
   can turn it red. Per rule 2 the floor is not lowered; if it reds, the fix
   belongs in the operator calibration (W3's dials), measured, not in the gate.
2. H15's 20 m cell, recorded as "calibrated at 0.001 margin, deliberately not
   asserted", is now below the floor. The mode-capture reading from the H16
   re-check (k = 40 cannot represent a 20 m field on a 5 m mesh) stands.

**Also verified**: the strict upstream regression suite (0.001 abs / 0.1 %
rel at every routing step) passes for Examples 1-3, 6-8 and the slot model;
all are bit-identical across two runs; the 2D ROM on/off bit-identity test
passes; the 1D ROM's deterministic path is unchanged by the ROM.

# H6 — WQ decay-rate bands: PR 13b ported, NORMAL/LOGNORMAL families added (2026-10-10)

**Branch** `hsym2/prh6-wq-families` (stacked on H8). H6 was blocked because the
layer it extends had never reached this line: `WQUncertaintyBounds` was a
test-only header, and the engine wiring (PR 13b on the pre-port sidecar) was
missing. This PR ports 13b and then adds the families.

## 1. What changed

- **PR 13b port** (`SWMMEngine`): QUALITY specs resolve pollutant names at init
  (late, so `[UNCERTAINTY]` may precede `[POLLUTANTS]`); an unknown name is a
  *recorded* warning (report + `swmm_get_warning_count`), not just a callback;
  the anchor buffer is seeded from the start state; at every report boundary
  each node × configured pollutant gets `q05/q50/q95` of
  `c_prev·exp(−k·kmᵢ·dt_report)` written to `<rpt>.wq_uncertainty.csv`
  (12 significant digits); the anchor then rolls forward. Independent of every
  ROM, exactly as `WQ_LAYER_DESIGN.md` specified.
- **H6**: `WQUncertaintyBounds` gains a `dist` field and maps its strata through
  the registry's own `invCdfMultiplier()` (PR 9b) — no second inverse-normal.
  The stashed August draft inlined one and clipped LOGNORMAL to `[1−p, 1+p]`,
  which silently changed what LOGNORMAL means on this layer; neither was kept.
  Both parsers accept the three families.
- The dormant `test_engine_wq_uncertainty` (two lifecycle tests, unregistered
  since the 2026-08-18 sweep) is registered and passes.

## 2. Validation

The two woken tests check only file shape, and their fixture carries no
pollutant load: every concentration in it is zero. Content is tested separately.

**Unit (closed forms, `WQDecayBoundsFamilies`)**: UNIFORM is `EXPECT_EQ`
bit-identical to the v1 hand-written strata at M = 2/7/50/51; the median member
reproduces `c₀·exp(−k·Δt_s/86400)` for every family (the design's days
convention); zero perturbation collapses every family exactly; NORMAL's width
is `(2·1.645/3)/1.8 = 0.609` of UNIFORM's (±5 %); LOGNORMAL's
lower : upper half-width is `1+p` (±5 %), the opposite skew to UNIFORM's
curvature-only asymmetry.

**Engine (`EngineWQUncertaintyContent`)**, a 1-junction fixture with a real TSS
load: UNIFORM relative width equals the physical `k·Δt·0.9·2p` at every
report boundary (±5 %); the three families through the parser and the CSV
reproduce 0.61× and the `1+p` skew row by row, with identical deterministic
medians; zero decay gives an exactly zero-width band; an unknown pollutant
warns and writes no rows. **Mutation-checked**: forcing every spec to UNIFORM
fails only the family test; dividing the decay rate by 86,400 (the post-rebase
hazard below) fails only the physical-width test.

## 3. Two quality-routing defects found on this line (fixed by the 2026-10 rebase)

**Update 2026-10-11:** the stack was rebased onto `swmm6_rel` (2026-10-09) and
both defects below are gone: Kdecay is stored in 1/s and routed correctly, and
pollutant `[DWF]` rows load mass instead of water. The band code now converts
the stored 1/s rate to per-day (the hazard named at the end of this section),
and `EngineWQUncertaintyContent.RealLoadGivesOrderedNonZeroBands` passes at
the same physical width, confirming the conversion.

Building a fixture with a non-zero concentration exposed both. Neither is in
the WQ layer; both are **already fixed on upstream `swmm6_rel`** and arrive
with a rebase, so neither was patched in this PR.

1. **Decay applied per second.** `QualityHandler` stores `Kdecay` as entered
   (1/day); `QualityRouting` applies `1 − k·dt` with `dt` in seconds and clamps
   at zero. Any k near 1/day zeroes a node's concentration in one routing step;
   k = 0.01/day decays 86,400× too fast. Upstream "KD1" divides by
   `SEC_PER_DAY` at parse, as legacy `landuse.c` does.
2. **Pollutant `[DWF]` rows booked as water.** `InflowSolver::computeAll` sums
   every `[DWF]` row into `dwf_inflow` regardless of constituent: `J1 TSS 100`
   adds 100 cfs (fixture: 1.759 ML dry-weather inflow vs legacy's 0.060 ML) and
   the TSS load is never applied. Upstream sums FLOW rows only. None of EPA
   Examples 1–8 has a pollutant `[DWF]` row, which is why H8's flow comparison
   could not see it.

The stack's merge-base with `swmm6_rel` is 2026-07-06; upstream has 725 commits
since, including a quality-routing parity campaign. **Rebase hazard for this
layer**: after the rebase `k_decay` holds 1/s, and the WQ band code (which reads
it as 1/day) must convert, or every band shrinks 86,400×. The site is marked in
`SWMMEngine.cpp`, and the physical-width test fails if it is missed.

## 4. Scope unchanged from the design

One report interval only (routing carries no parcel age); no feedback into
hydraulics or any ROM; multipliers are the calculator's own strata, not an
`UncertaintyEnsemble` column (Option B).

## 5. Reproduction

    ctest --test-dir build/<dir> -R 'test_engine_wq_uncertainty|test_engine_runoff_ensemble'

# H16 — relative-deviation reconstruction removes the cross-depth leakage W4b found (2026-10-10)

**Branch** `hsym2/h16-depth-relative-deviation` (stacked on W4b). The fix
candidate W4b's root-cause analysis named: a thin cell beside deep water no
longer inherits the deep cell's absolute-magnitude deviation through the
truncated basis.

## 1. The change

`SpectralROM::relative_deviation` (default **false**, bit-identical to
every prior PR when unset). When true:

- The Manning-sensitivity source projected into `b_coarse` is the CONSTANT
  field (all ones), not `h_det`. A spatially uniform target is captured
  almost exactly by any retained mode on a grounded basis, regardless of
  how sharply `h_det` itself varies — unlike `h_det`, which on the W4
  channel varies 40×.
- Every place a member's absolute depth is reconstructed from `δa` (both
  `computeQuantiles()` paths — naive and the H4 GEMM path — and
  `applyCouplingFlux()`) becomes `h_i = h_det · (1 + Σ_j P[j,t]·δa_{i,j})`:
  a LOCAL FRACTION of `h_det`, not an added constant.
- `manning_elasticity` (H13) keeps its exact meaning: the frozen-source
  fixed point is `δa* = (mm−1)·manning_elasticity·(Pᵀ𝟙)[j]`, reconstructing
  to `h_i(t) ≈ h_det(t)·(1 + (mm_i−1)·manning_elasticity)` — the SAME
  uniform relative perturbation at every cell, deep or shallow.

A secondary idea was tried and dropped first: depth-weighting the
EIGENBASIS itself (the existing, unused-in-production `buildDepthWeighted`)
rather than the state variable. On the channel mesh the depth-weighted
Lanczos solve failed outright (silent fallback to the plain geometric
basis — confirmed, not assumed, via the return code). Not pursued further:
even had it converged, it changes which modes exist, not what units the
reconstructed deviation carries, so it would mitigate rather than remove
the mechanism W4b measured.

## 2. Validation: the W4 depth-binned harness, before/after (`W4_RELATIVE=1`)

**Channel, adv rung, `manning_elasticity = 1.0` (unchanged):**

| MC depth class | width ratio before → after | elasticity ROM before → after | elasticity MC |
|---|---|---|---|
| 3–10 mm | 8.6 → **0.86** | 4.79 → **0.86** | 0.57 |
| 1–2 cm | 3.8 → **0.93** | 2.10 → **0.93** | 0.57 |
| 2–5 cm | 3.5 → **0.93** | 1.63 → **0.93** | 0.44 |
| 5–10 cm | 1.0 → **0.64** | 1.14 → **0.90** | 1.23 |
| 10–20 cm | 1.1 → **0.98** | 0.82 → **0.79** | 0.79 |
| > 20 cm (thalweg) | 1.75 → **1.85** | 0.63 → **0.72** | 0.40 |
| pooled | 0.946 / 2.933 | → **0.855 / 1.620** | — |

This is the core result: the ROM's own elasticity collapses from a
0.63–4.79 range (a 7.6× spread, the leakage signature) to **0.64–0.93** —
tight, and close to `manning_elasticity`'s value of 1.0, exactly as the
fixed-point formula in §1 predicts. No per-surface tuning was applied yet;
this is the mechanism working, nothing more.

**Plane, adv rung (the already-good W3 cell) — regression check:**
`manning_elasticity = 1.0`: 0.822/1.321 (before, absolute) → **0.852/1.323**
(after, relative) — unchanged within noise. The plane's depth contrast is
mild (a smooth kinematic profile, not a 40× step), so absolute and relative
reconstruction nearly coincide there; this is the expected limit, not a
coincidence, and is the honest regression test for "did this break the one
cell that was already fine."

## 3. Recalibrating `manning_elasticity` under relative mode

With the leakage confound removed, a single elasticity constant now
produces a genuinely interpretable number per surface (sweep, adv rung):

| elasticity | plane | channel |
|---|---|---|
| 1.0 | 0.852 / 1.323 | 0.855 / 1.620 (conservative) |
| 0.8 | — | **0.809 / 1.296 (calibrated)** |
| 0.7 | 0.733 / 0.926 | 0.775 / 1.134 |
| 0.6 | 0.651 / 0.794 | 0.729 / 0.972 |
| 0.5 | 0.548 / 0.661 | 0.668 / 0.810 |

The plane wants `e` near 1.0; the channel wants `e` near 0.8. This is the
SAME geometry-dependent-elasticity residual H13-2D and W4b already
documented (sheet-flow 0.6, V-section 3/8 ≈ 0.375, by Manning's own
exponents for the two cross-section families) — relative_deviation does
not resolve that part, and was never meant to: it removes the reconstruction
artefact that made the channel's per-bin numbers UNREADABLE as physics
(W4b's whole point), leaving a smaller, already-understood, geometry-
dependent calibration gap. The 5–10 cm bin stays the outlier at every `e`
(small sample, n≈1260, likely a transition-zone effect between regimes);
not chased further here.

## 4. What ships, what does not

**Superseded 2026-10-11:** relative reconstruction is now the default and
not a setting (applied wherever the basis is grounded; a closed domain keeps
the absolute form, which a router test pins: forcing relative mode on a
closed box collapsed its band 256×). The asserted W3 gate under relative
mode on the rebased engine: 0.830 / 1.295 (absolute: 0.800 / 1.261). Text
below is the original H16 record.

`relative_deviation` ships as an opt-in option (`[2D_ROM] RELATIVE_DEVIATION
YES`, default NO) — new capability, zero change to any existing number
unless a caller opts in. `manning_elasticity`'s default is untouched (H13's
call stands). Not validated in combination with `spatial_mannings` (H15's
per-member operators), a non-shared reduced operator, H7's two-family soft
forcing, or 1D coupling via `applyCouplingFlux`'s `rom1d` argument — the
option composes with the code paths this session actually measured
(shared-operator advance, both quantile paths, the coupling flux's own
depth reconstruction) and is undocumented, not refused, elsewhere. The 1D
ROM does not get this option: H5b's analogous case (J3, directly above the
surcharged pool) was already root-caused to basis truncation on a 5-node
fixture with no production analogue, lower priority than the 2D case W4b
measured on a 3,200-triangle mesh.

## 5. Review re-check (2026-10-10): the 30-minute numbers are at the fixed point, not mid-transient

A review concern on the first write-up: relative mode moves the whole
Manning source onto the constant field, which the grounded basis carries in
its *slowest* mode, so the 30-minute window might be reporting a band still
converging. Re-measured at a 180-minute window (scored over the last 10
minutes, `W4_REPORTS=180 W4_SCORE_FROM=170`):

| Cell (relative mode, adv) | 30 min | 180 min |
|---|---|---|
| plane, e = 1.0 | 0.852 / 1.32 | 0.858 / 1.355 |
| plane, e = 0.8 | — | 0.800 / 1.084 |
| plane, e = 0.6 | — | 0.666 / 0.813 |
| channel, e = 1.0 | 0.855 / 1.62 | 0.857 / 1.664 |
| channel, e = 0.8 (calibrated point) | **0.809 / 1.30** | **0.811 / 1.331** |

Growth of 2–3 % in width over six times the window: the 30-minute figures
were already at the fixed point, and the e = 0.8 channel calibration stands
(CALIBRATED under C1/C2 at both windows). The per-bin picture at 180 min is
the same as §2: ROM elasticity 0.85/0.93/0.94/0.94/0.84/0.76 across the six
depth classes at e = 1.0, and the 5–10 cm bin remains the one outlier
(MC elasticity 1.23, coverage 0.62 at e = 1.0, 0.51 at e = 0.8 — n = 1,266,
the transition zone between flank and thalweg; still not chased).

**H15's short-ℓ remainder does NOT close under relative mode** (the
"same mechanism" hypothesis from the W4b write-up was tested and fails):
`plane-mann-corr20` per-member M_i goes 0.801 / 0.92 (absolute) → **0.769 /
0.829** (relative), i.e. slightly *narrower*; `plane-mann-corr100` 0.844 /
1.32 → 0.857 / 1.278, unchanged within noise. A 20 m correlated field on a
5 m mesh is a capture problem (k = 40 modes cannot represent it), not a
leakage problem — W3's "k is a capture dial" reading holds, and the ~8 %
narrowness stays with H15 as an open mode-count question.

## 6. Reproduction

    ctest --test-dir build/<dir> -R 'test_engine_2d_spectral_rom|test_engine_2d_rom_input_parser|test_engine_2d_rom_router_operator'
    W4_BINS=1 W4_RELATIVE=1 W4_CELLS=channel-mann-comono OPENSWMM_2D_BACKEND=cpu \
        build/<dir>/tests/regression/test_2d_rom_marcher_coverage_corr
    W4_RELATIVE=1 W4_CELLS=channel-mann-comono W4_ELASTICITY=0.8 ...   # the calibrated point
    W4_RELATIVE=1 W4_REPORTS=180 W4_SCORE_FROM=170 ...                 # the §5 window check

# W4b — the channel's 2.9× is modal leakage onto thin cells, not an advection or elasticity constant (2026-10-09)

**Branch** `hsym2/w4b-channel-calibration` (stacked on H13-2D). The plan
called for a calibration sweep of `c_factor`/`alpha_*` on the thalweg
channel. The sweep was not run, because the measurement that was supposed
to motivate it says the constants are not the problem. No model change; the
harness gained depth-binned scoring (`W4_BINS=1`) that reports, per MC
depth class, member coverage, width ratio, and — the quantity that settled
this — the MC's and the ROM's own depth-to-roughness elasticity,
`(q95−q05 of depth)/(median depth)` over `(q95−q05 of mm)`.

## 1. Where the width lives (channel, adv rung, elasticity 1, k = 40)

| MC depth class | share of samples | width ratio | elasticity MC | elasticity ROM |
|---|---|---|---|---|
| 3–10 mm | 9 % | **8.6** | 0.57 | 4.79 |
| 1–2 cm | 29 % | 3.8 | 0.57 | 2.10 |
| 2–5 cm | 42 % | 3.5 | 0.44 | 1.63 |
| 5–10 cm | 4 % | 1.0 | 1.23 | 1.14 |
| 10–20 cm | 5 % | 1.1 | 0.79 | 0.82 |
| > 20 cm (thalweg) | 11 % | 1.75 | 0.40 | 0.63 |

Three things this table says. (a) The Monte Carlo's elasticity is
0.4–0.6 in every class — Manning's 0.6, give or take, on the thin flanks
too; the "channel elasticity ≈ 0.27" inferred in H13-2D from the pooled
median was an artefact of pooling. (b) The ROM's elasticity is right
where the water is deep (0.63 vs 0.40 in the thalweg — the same ~1.6× the
plane shows at depth) and wrong by up to 8× on the thin flanks, which are
80 % of the samples. (c) A thin flank cell's ROM deviation is several
times its own depth: it is not that cell's roughness response, it is the
neighbouring thalweg's large absolute deviation reconstructed onto it by
k = 40 smooth global modes. The same mechanism as H15's short-ℓ remainder
and H5b's J3.

The plane has the same signature, milder (depth varies smoothly):
1–2 cm cells 2.6× (ROM elasticity 2.67 vs MC 1.02), 2–5 cm 2.1×, 5–10 cm
1.18× (0.80 vs 0.66), 10–20 cm 1.31× (0.63 vs 0.53).

## 2. Why the planned sweep would not have helped

The adv-vs-iso difference on the channel (2.93 vs 2.20 at 30 min) is the
same thing H13-2D measured on the plane: advection reaches the
operator-independent fixed point faster. A `c_factor` sweep moves how fast
the band arrives at the wrong thin-cell value, not the value. An elasticity
constant scales every class alike (H13-2D: ×0.6 takes the thalweg to 1.05
and the flanks to 2.2) and cannot fix a ratio that varies 8× with depth.

## 3. Does capture fix it?

Channel, adv, elasticity 1, k = 160 (vs k = 40):

| MC depth class | width ratio k = 40 → 160 | elasticity ROM k = 40 → 160 | elasticity MC |
|---|---|---|---|
| 3–10 mm | 8.6 → 3.9 | 4.79 → 2.19 | 0.57 |
| 1–2 cm | 3.8 → 2.0 | 2.10 → 1.13 | 0.57 |
| 2–5 cm | 3.5 → 2.1 | 1.63 → 1.00 | 0.44 |
| 5–10 cm | 1.0 → 1.0 | 1.14 → 1.32 | 1.23 |
| 10–20 cm | 1.1 → 1.3 | 0.82 → 1.08 | 0.79 |
| > 20 cm (thalweg) | 1.75 → 2.5 | 0.63 → 0.96 | 0.40 |

Capture does what it should: with 160 modes the ROM's elasticity is ≈ 1 in
every class (the source's own assumption), the thin-cell leakage halves
(a residual remains on the 3–10 mm films), and each class's over-width
becomes simply 1 over the Monte Carlo's elasticity — 1/0.57 ≈ 1.75 on the
flanks, 1/0.40 = 2.5 in the thalweg (which therefore got *wider* with more
modes: the k = 40 thalweg value of 1.75 was capture loss masking the
elasticity error, the plane's story again). The thalweg's 0.40 is itself
readable: Manning in a triangular section gives `Q ∝ d^{8/3}/n`, so
`d ∝ n^{3/8} = 0.375` — the V-channel's exponent, not the wide-sheet 0.6.
So the right elasticity is geometry-dependent (0.6 on a sheet, 0.375 in a
V-section), which the local cross-section's `A·R^{2/3}` scaling would
provide; together with a depth-relative deviation variable (§4) that is
the H16 design. Pooled: 0.946 / 2.93 → 0.906 / 2.09 at k = 160.

## 4. What this means for the band

In relative terms the 2D band is approximately calibrated where the flow
is (≥ 5 cm here) and a ranking on films adjacent to deep water. That is a
reconstruction limit of the node-based deviation (absolute depth) in a
truncated basis. The candidate that addresses it at the root is a
deviation in a depth-relative variable — `δ(ln d)` or `δd/d` — in which
the Manning response is a near-uniform field that a grounded basis
represents with its first mode and no leakage of absolute magnitude across
a depth contrast; the fixed point becomes `(mm−1)·e` uniformly. That is a
formulation change touching reconstruction, coupling and both ROMs: logged
as candidate **H16**.

**Closed the next day — see "H16" above.** The reconstruction changed
from an added constant to a local fraction of `h_det`; the ROM's own
per-bin elasticity went from a 0.63–4.79 range to 0.64–0.93, and a single
elasticity constant now gets the channel to calibrated (0.8 → 0.809/1.296).
`W4_BINS=1` remains the honest way to read a 2D cell, and the per-surface
elasticity gap (plane wants ≈1.0, channel ≈0.8) is now a measured,
geometry-dependent calibration question, not an unreadable leakage
artefact.

## 5. Reproduction

    W4_BINS=1 W4_CELLS=channel-mann-comono OPENSWMM_2D_BACKEND=cpu \
        build/<dir>/tests/regression/test_2d_rom_marcher_coverage_corr
    W4_BINS=1 W4_MODES=160 ...        # capture check
    W4_BINS=1 W4_CELLS=plane-mann-comono W4_SWEEP="1.0:1" ...

# H13 (2D half) — the 2D band's 1.3× is elasticity × basis capture; a dial, not a default (2026-10-07)

**Branch** `hsym2/h13-2d-elasticity` (stacked on H14b). The 1D half closed
with H5b (`alpha_free = 0.6`). The 2D ROM's Manning-sensitivity source
likewise assumes depth moves one-for-one with n; sheet-flow normal depth
moves as n^0.6. This section measures what that costs in 2D and what
happens when it is corrected. Every number is the W4 harness
(`regression_2d_rom_marcher_coverage_corr`, new `plane-mann-comono` cell and
`W4_SWEEP` / `W4_MODES` / `W4_ELASTICITY` / `W4_REPORTS` / `W4_SCORE_FROM`
knobs), 25 members, member coverage / median width ratio.

## 1. The plane is at its fixed point, and the fixed point is operator-independent

Scoring the last 10 minutes of 30-, 90- and 180-minute windows after the
3000 s spin (comonotone ±20 % Manning):

| rung | 30 min | 90 min | 180 min |
|---|---|---|---|
| legacy (diagonal, Neumann) | 0.47 / 0.46 | 0.47 / 0.46 | 0.47 / 0.46 |
| iso | 0.66 / 0.84 | 0.80 / 1.20 | 0.82 / 1.32 |
| aniso | 0.67 / 0.86 | 0.80 / 1.21 | 0.83 / 1.33 |
| adv (production) | 0.82 / 1.32 | 0.83 / 1.34 | 0.83 / 1.34 |

All three physical rungs converge to the same 1.32–1.34: the deviation
form's fixed point `(mm−1)·b` does not depend on the operator (as W3 stated
and H14 unit-tests). The adv rung's advantage at 30 minutes is faster
convergence, not a different answer. (An earlier version of this series
read as a slow transient; it was a shell word-splitting slip that scored
every window from report 0 — recorded in `history_failures.md`.)

## 2. Where the 1.3× comes from

Elasticity × retained modes on the plane, adv rung, 30-minute window:

| elasticity | k = 40 (W3 gate) | k = 80 | k = 160 |
|---|---|---|---|
| 1.0 (shipped) | 0.822 / **1.321** | 0.965 / 1.534 | 0.988 / 1.518 |
| 0.6 (physical) | 0.652 / 0.793 | 0.784 / 0.920 | **0.800 / 0.911** |
| 0.75 | 0.746 / 1.000 | — | — |

The diffusivity scale does not enter (×1…×16 at e = 0.6 all give 0.79–0.81,
as a fixed point must). Reading the table: the physical over-width is
1/0.6 = 1.67; at k = 40 the basis captures about 0.8 of the depth field, so
the shipped configuration lands at 0.8 × 1.67 ≈ 1.32 — an accidental
compensation, not a calibration. Raise k and the compensation disappears
(1.53). Apply the physical elasticity and the band is right only once the
basis captures the field (k = 160: 0.91 width, coverage exactly at the
floor). Elasticity 0.75 at k = 40 gives width 1.00 with coverage 0.75: a
mis-shaped band (truncation redistributes the deviation spatially) that a
median-width gate would have called calibrated — the C2 trap.

## 3. The channel does not have elasticity 0.6

| elasticity | k = 40 | k = 80 |
|---|---|---|
| 1.0 | 0.946 / 2.933 | 0.910 / 2.668 |
| 0.6 | 0.873 / 1.760 | 0.831 / 1.601 |

With capture ≈ 0.8 the channel's implied MC elasticity *from the pooled
median* is about 0.27. **Corrected by W4b (2026-10-09, section above)**:
binned by depth the MC's elasticity is 0.4–0.6 in every class; the pooled
number was the ROM's thin-cell over-width (modal leakage from the
thalweg) read backwards as physics. A single constant still cannot serve
the channel, but for a different reason: the error is a reconstruction
limit, not an elasticity.

## 4. What ships

- `SpectralROM::manning_elasticity` and `[2D_ROM] MANNING_ELASTICITY`
  (default **1.0**), applied to the Manning-sensitivity term on every
  advance() path; 1.0 is bit-identical to pre-H13 (tested), 0.6 scales the
  fixed point by exactly 0.6 (tested).
- `[2D_ROM] DIFFUSIVITY_SCALE` (default 1.0), a multiplier on K_eff handed
  to the operator, kept from the sweep as a legitimate convergence-rate dial
  (it moves nothing at the fixed point — measured — but shortens the
  approach on the iso/aniso rungs).
- The default stays 1.0 because the C1 floor is asserted on the W3 gate at
  k = 40, and 0.6 there measures 0.652 — below the floor, honestly. Flipping
  the default is coupled to raising `MODES` (≈5 % of cells on this plane)
  and to W4b's channel answer; both are owner decisions with a cost.
  **Recommended configuration for sheet-flow surfaces**:
  `MANNING_ELASTICITY 0.6` with `MODES` ≥ 5 % of cells (measured 0.80 / 0.91
  at k = 160 on 3200 cells).

## 5. Reproduction

    W4_CELLS=plane-mann-comono W4_MODES=160 W4_SWEEP="1.0:1,0.6:1" OPENSWMM_2D_BACKEND=cpu \
        build/<dir>/tests/regression/test_2d_rom_marcher_coverage_corr
    W4_CELLS=plane-mann-comono W4_REPORTS=180 W4_SCORE_FROM=170 ...        # the fixed-point series
    W4_CELLS=channel-mann-comono W4_ELASTICITY=0.6 W4_MODES=80 ...

# H14b — batch propagator: the per-member matrix exponential shares the operator's powers (2026-10-07)

**Branch** `hsym2/h14b-batch-propagator` (stacked on H14). Exact; no model
change. Every MC cell reproduces its H14 numbers to printed precision and
the gate stays 142/142.

## 1. What changed

Every member of a ROM ensemble integrates the same operator scaled by its
own scalar, `δa_i ← exp(s_i·A)·δa_i + φ₁(s_i·A)·c_i` with `A = −Δt·K·M` and
`s_i = 1/mm_i`. H14 (and the 2D ROM since W3) paid one full Padé
exponential of the augmented `(k+1)×(k+1)` matrix per member per step.
`BatchPropagator` (`RomDensePropagator.hpp`) evaluates Higham's degree-13
Padé approximant instead, on the augmented matrix
`B_i = [[s_i·A, c_i],[0,0]]`, whose powers are
`B_i^m = [[s_i^m·A^m, s_i^{m−1}·A^{m−1}·c_i],[0,0]]`: the matrix powers
`A^1..A^13` are formed once per step (`prepare`), and each member forms its
numerator/denominator as an `s`-weighted combination (O(13·k²)), runs 12
matrix–vector products for the forcing column, and one `(k+1)×(k+1)` solve
(`apply`). The scaling exponent is chosen once from `max_i|s_i|`; squarings,
when needed, stay per member (the scalar is inside the exponent). The 1D
reduced path and the 2D SHARED-operator path use it; H15's per-member
operators keep `propagateDense` (their matrices differ). Parity with the
per-member Padé: 1e-13 relative without scaling, 1e-11 with several
squarings (tested, random non-symmetric k = 9 / 12); scalar and `A = 0`
limits exact.

## 2. Measured

Per routing step, M = 50, k = 20, Bellinge-scale assembly (n = 1011,
E = 1010), scratch benchmark of the header-only code:

| | `-O0` (Debug) | `-O2` |
|---|---|---|
| per-member Padé (H14) | 16.3 ms | 1.62 ms |
| batch propagator (H14b) | 8.6 ms | 0.72 ms |
| directional assembly | 2.2 ms | 0.36 ms |

Bellinge full-window lock (65,589 steps, most reaches gated), Debug:

| | wall |
|---|---|
| H5b (no operator) | 460 s |
| H14 | 1,853 s |
| **H14b** | **1,428 s** |

**Release build** (`-O2`, same machine, Bellinge lock, `H14_DIRECTIONAL`
knob added to the gate for the A/B):

| | wall |
|---|---|
| directional operator off (pre-H14 numerics) | 123 s |
| H14b, first version (per-call scratch allocation) | 251 s |
| **H14b, scratch buffers hoisted** | **183 s** |

So in the build users run, the directional operator costs about half again
on a network where most reaches are gated (+49 %), down from double. The
allocation fix alone was worth 68 s: six `std::vector` allocations per
member per step is 20 million allocations on this run, and in an optimized
build they outweighed the arithmetic they wrapped.

The batch halves the propagator and does not touch the floor: an exact
per-member exponential costs at least one O(k³) solve, and in Debug the
unoptimized `std::vector` indexing dominates everything else. The remaining
exact options are a real Schur decomposition of `A` once per step with a
blocked Schur–Parlett per member (O(k²) per member, but a non-trivial
eigensolver to get right), or a cached propagator for fixed-step runs
keyed on `(dt, K1d, M)` — Bellinge's adaptive steps would rarely hit it.
Neither is done; both are recorded.

## 3. Reproduction

    ctest --test-dir build/<dir> -R test_engine_rom_directional_operator   # BatchPropagator parity tests
    H14_DIRECTIONAL=0 build/<dir>/tests/regression/test_rom_coverage_bellinge   # A/B timing knob

# H14 — Froude-gated directional 1D operator (2026-10-07)

**Branch** `hsym2/h14-directional-1d-operator` (stacked on H15). Closes the
H5b residual: `RomCoverageSurcharged.SemiImplicitContinuity` is green on
H5's own bounds, and `regression_rom_coverage` with it — the full gate is
142/142 for the first time on this line.

## 1. Problem

The 1D ROM's operator is the weighted graph Laplacian built from the Picard
`dqdh` (one coefficient per conduit, applied to both endpoints): symmetric,
no flow direction. On H5b's surcharged chain the pool's 10 ft of deviation
diffused *upstream* across 5 % supercritical reaches and left J1–J3 7–15×
over-wide (H5b §4 showed by sweep that this was not their own source).

## 2. What was built

- `RomDirectionalOperator.hpp` (pure): `M = Pᵀ·L_dir·P`, with `L_dir` the
  same grounded weighted Laplacian except that on a conduit whose Froude
  number exceeds the gate the UPSTREAM node's coupling to the downstream one
  is removed (`L[up,down] → −w·(1−g)`), the downstream row untouched. Gate
  `g = clamp((Fr − 0.8)/0.4, 0, 1)`. With every gate closed `M = diag(λ)` up
  to the Ritz residual (tested); open gates make it non-symmetric (tested
  against a hand-built dense `L_dir` to 1e-12).
- `RomDensePropagator.hpp`: the dense [6/6] Padé `expm` and the augmented
  φ₁ step moved verbatim out of `DeviationOperator2D.cpp` so the 1D ROM
  (built unconditionally) can use it; 2D delegates and is bit-identical.
- `SpectralROM1D::setReducedOperator(M)` and a reduced path in `advance()`:
  `d(δa_i)/dt = −(K1d·M/mm_i)·δa_i − K1d·(1/mm_i − 1)·M·b + g_i`, every
  forcing term unchanged. `M = diag(λ)` reproduces the diagonal path to
  1e-10 (tested); a successful `updateBasis()` clears M (P changed).
- Engine: `refreshRom1dDirectionalOperator()` each routing step from
  `HSnapshot::link_froude` + the conduit flow sign (ci → link map checked
  against `node1` so a reordering can never misattribute a Froude number).
  **Installs nothing when no conduit is gated**, so every subcritical
  network is bit-identical to pre-H14 — SR-5 and CL-1e reproduce their
  numbers exactly, H11 is unchanged. `rom1dDirectionalConfig()` /
  `rom1dDirectionalGatedFraction()`; harness knobs `H14_DIRECTIONAL`,
  `H14_FR_LO/HI`, `H14_DROP`.

## 3. A property worth knowing before reading the numbers

The deviation form's fixed point `δa* = (mm−1)·b` does not depend on the
operator (W3 noted this for 2D; the unit test
`FrozenSourceFixedPointIsOperatorIndependent` pins it for 1D). So the gate
cannot change a *steady* band at all. What it changes is the transient: an
upstream node now relaxes on its own local physics while the pool below it
fills, instead of being dragged by the pool's growth. H5b's window IS that
regime (the pool keeps rising through it), which is why the gate acts there
and nowhere else.

## 4. Results (every 1D MC cell; member coverage / median width ratio)

| cell | pre-H14 | **H14** | verdict |
|---|---|---|---|
| H5 surcharged, SEMI_IMPLICIT | 0.842 / 5.446 (red on ≤ 3.0) | **0.832 / 1.208** | **calibrated — green** |
| H5 surcharged, EXPLICIT | 0.693 / 0.694 | 0.732 / 0.781 | H5 bounds pass; floor unmet, documented limitation |
| PR-10 free-surface (P8) | 0.867 / 1.177 | 0.993 / 1.210 | calibrated (reaches are gated too; slightly over-covers) |
| H11 front passage | 0.829 / 1.290 | 0.829 / 1.290 | unchanged (no gate opens) |
| SR-5 / CL-1e soft rain | 0.881 / 0.976, 0.827 / 0.804 | identical | unchanged (no gate opens) |

`H14_DIRECTIONAL=0` reproduces 0.842 / 5.446 exactly. Per node on the SEMI
cell (late window, ROM width / MC width):

| node | pre-H14 | **H14** |
|---|---|---|
| J1 | 0.743 / 0.108 (7.5×) | 0.130 / 0.108 (**1.2×**) |
| J2 | 0.721 / 0.108 (7.4×) | 0.157 / 0.108 (**1.5×**) |
| J3 | 1.525 / 0.108 (15×) | 1.198 / 0.108 (11×) |
| J4 | 6.65 / 9.23 (0.70) | 7.17 / 9.23 (0.76) |
| J5 | 10.19 / 10.03 (1.02) | 10.76 / 10.03 (1.08) |

J1 and J2 are fixed. J3 — the node directly above the pool — is not.

## 5. J3: what was tried, measured, and rejected

The solver's Froude number is a mid-depth quantity. C3 (J3 → J4) is steep
but its lower end is drowned by the pool, so its mid-depth is large and it
reads subcritical: the Froude gate never opens on C3, and J3 keeps its
coupling to J4. A second, node-level gate was built: open when the
water-surface drop across the conduit exceeds the upstream node's own depth
(`(h_up − h_dn)/d_up` ramped over [1, 2]) — J3 sits 20 depths above the
pool. Measured: J3 moved only 1.20 → 1.07 ft, while the same criterion gated
steep-but-subcritical reaches on the soft-rain fixtures and took **CL-1e
from 0.827 to 0.762 (red)** and SR-5 from 0.881 to 0.869. It ships off
(`use_drop = false`), kept as a dial so the measurement is reproducible.

That J3 barely responds to being fully decoupled from J4 says its residual
is not relayed through the operator. The remaining suspect is basis
truncation: this 5-node fixture retains k = 4 modes (the eigensolver's
ceiling is n − 1), so the operator-independent fixed point
`(mm−1)·PPᵀ(alpha ⊙ depth)` smears the pool's large source into whichever
node the missing mode weights most. See §6 for the measurement.

## 6. Truncation check

Lifting both mode caps (`k_req = n` in `GraphEigenBasis`, `k = n_active`
in `buildROM1D`; a scratch build, reverted) on the SEMI cell:

| configuration | J1 | J2 | J3 | J4 | J5 | member cov / width-med |
|---|---|---|---|---|---|---|
| shipped: k = 4 of 5, Froude gate on | 0.130 | 0.157 | 1.198 | 7.17 | 10.76 | 0.832 / 1.208 |
| full basis k = 5, gate **on** | 0.131 | 0.131 | **0.173** | 2.56 | 14.5 | 0.805 / 1.213 |
| full basis k = 5, gate **off** | 0.748 | 0.754 | 0.763 | 2.11 | 13.97 | 0.815 / 5.104 |

(ROM widths in ft; MC is 0.108 at J1–J3, 9.23 at J4, 10.03 at J5.) Two
separate effects, now separated: the symmetric operator's transient relay
(0.75 ft at J1–J3 even with a full basis — H5b's own experiment,
reproduced) is what H14 removes; J3's remaining 1.2 ft on the shipped
configuration is the missing fifth mode of a five-node chain, and vanishes
with the gate once that mode is present (0.17 ft, 1.6×). The full basis is
not shipped: `GraphEigenBasis` caps at n − 1 by design (the Lanczos start
vector is built orthogonal to the constant), and the full basis also
under-predicts J4 (2.6 vs 9.2 ft) for reasons not chased here — the same
J4 anomaly H5b's full-basis experiment recorded. On a production network
(k = 20 of ~1000) the one-mode truncation of a five-node fixture has no
analogue; J3 is a fixture artefact, recorded, not a formulation gap.

## 7. Cost — a real one, recorded

The reduced path does one (k+1)×(k+1) matrix exponential **per member per
routing step** whenever any conduit is gated (the 2D ROM has paid the same
since W3, at k = 40). On Bellinge (1011 nodes, k = 20, M = 50, 65,589
steps, most reaches gated) the full-window lock went from **460 s to
1,853 s** (Debug). The lock itself passes with its distribution essentially
unmoved (band/depth p50 0.308 → 0.316, max 44.6 → 44.5; band/head max
0.276 → 0.268). Exactness was kept over cost here: the per-member scaling
`1/mm_i` is inside the exponential, so the propagator cannot be shared
across members by a scalar trick. Follow-up **H14b** (done the same day,
section above): sharing the matrix powers across members halves the
propagator (1,853 → 1,428 s Debug); the O(k³) per-member solve is the
exact floor without a Schur decomposition.

## 8. Reproduction

    build/<dir>/tests/regression/test_rom_coverage                      # all 1D cells (green)
    H14_DIRECTIONAL=0 build/<dir>/tests/regression/test_rom_coverage --gtest_filter='RomCoverageSurcharged.SemiImplicit*'   # pre-H14
    H14_DROP=1 H5B_TRACE=1 ... --gtest_filter='RomCoverageSurcharged.SemiImplicit*'   # the rejected drop gate
    ctest --test-dir build/<dir> -R 'rom_directional_operator|1d_rom_lifecycle'

# H15 — spatially correlated Manning through the 2D reduced operator (2026-10-07)

**Branch** `hsym2/h15-spatial-manning-reduced-op` (stacked on W4). Closes
W4 finding (b).

## 1. Problem

`SpectralROM::advance()` took the reduced-operator path only when
`spatial_mannings` was unset. Any spatially correlated Manning field (the
`[2D_ROM] MANNINGS_CORR_LEN` key) therefore ran on the diagonal `λ·K_eff`
path W3 retired for everything else. W4 measured it: 0.649 member coverage
and 0.68 width ratio at a 20 m correlation length, 0.835 / 1.24 at 100 m.

## 2. Fix

One reduced operator per member. `DeviationOperator2D::assemble` gained an
optional per-cell conductance multiplier `cond_mult` (the local `1/W_n(t)`):
it multiplies each edge's diffusive conductance by the harmonic mean of its
two cells' values (series conductance, the depth-weighting convention), each
cell's velocity before the face average (so advection scales as 1/n too),
and the grounding term. A uniform value `c` gives `c·M` of the unscaled
assembly to 1e-12; null is bit-identical to all-ones (both tested).
`SpectralROM::setReducedOperatorPerMember(M_all)` installs `n_ensemble` such
operators; `advance()` then integrates each member on its own `M_i`:

    d(δa_i)/dt = −(M_i/ρ_i)·δa_i − (M_i/ρ_i − M₀)·b + g_i

with `M₀` the nominal operator and `ρ_i` the extra `RATE_MULT` product. For
a spatially uniform field `W_i ≡ mm_i` this is the shared-operator form
exactly (`M_i = M₀/mm_i`), tested to 1e-10; a field identically 1 gives
`M_i ≡ M₀` and exactly zero deviation (tested with `EXPECT_EQ`). Without
per-member operators the diagonal fallback is unchanged (tested to be
bit-identical to the no-operator path). `SurfaceRouter2D::refreshROMOperator`
assembles the per-member set whenever the ROM carries a spatial Manning
field, on the same cadence as the nominal operator (engine test:
`MANNINGS_CORR_LEN > 0` ⇒ `hasPerMemberOperators()`). Cost: `M` extra
assemblies per refresh, `O(M·k²·n)`; per step nothing changes — the shared
path already did one `k×k` exponential per member.

## 3. Result (W4 harness, M = 25; member coverage / median width ratio)

| cell | pre-H15 (diagonal, grounded) | **H15 (adv, per-member M_i)** | verdict |
|---|---|---|---|
| plane, correlated Manning, ℓ = 20 m | 0.649 / 0.68 | **0.801 / 0.92** | calibrated by 0.001 — **not asserted** |
| plane, correlated Manning, ℓ = 100 m | 0.835 / 1.24 | **0.844 / 1.32** | calibrated |

The short-length cell moved from ranking-only to the floor: coverage +0.15,
width 0.68 → 0.92. It is still ~8 % narrow and sits on the floor by a margin
that is noise at this M, so the cell prints its verdict and does not gate —
asserting a 0.001 margin would be a tolerance choice dressed as a result.
The remaining narrowness at short ℓ is consistent with W3's observation that
`k` is a capture dial with a spatial signature: a 20 m field on a 5 m mesh
has structure the 40 retained modes carry only partly, on the ROM side only
(the marcher resolves it). Not pursued here. The 100 m cell was already
calibrated on the diagonal path and stays so; the per-member operator
widens it slightly, in the direction of the plane's known ~1.3× over-width.

## 4. Reproduction

    W4_CELLS=plane-mann-corr20,plane-mann-corr100 OPENSWMM_2D_BACKEND=cpu \
        build/<dir>/tests/regression/test_2d_rom_marcher_coverage_corr        # ~5 min
    ctest --test-dir build/<dir> -R 'test_engine_2d_deviation_operator|test_engine_2d_rom_router_operator'

# W4 — 2D validation breadth: correlated-field marcher MC and a second surface (2026-10-07)

**Branch** `hsym2/w4-2d-validation-breadth` (stacked on H5b). A measurement,
not a fix. Before W4 every 2D band claim rested on one cell — the W3 steady
runoff plane under a comonotone Manning multiplier (member coverage 0.82,
width-med 1.32) — and SP3's `COHERENCE CORR_LEN` on the 2D ROM had no
correlated Monte Carlo behind it at all. New harness:
`tests/regression/test_2d_rom_marcher_coverage_corr.cpp`
(`regression_2d_rom_marcher_coverage_corr`, label `slow`, ~11 min Debug,
25 members per cell, `W4_CELLS`/`W4_MEMBERS` narrow it).

## 1. What was built

- **Correlated fields from the engine's own basis.** Member fields come from
  `SpdeSpatialBasis` (Matérn ν=2, CL-2b) seeded with the ROM's own per-member
  coefficients, exactly as `SurfaceRouter2D::buildGridSoftField` does, so the
  marcher MC member i and ROM member i see the SAME realization:
  `rain_i(t) = R·(1 + CV·W_i(t))` (NORMAL, CV 0.20) and
  `n_i(t) = n̄·(1 + 0.2·V_i(t))` (UNIFORM, mode 0 = the W3 strata). The
  ROM's CORR_LEN path takes the production branch rule (reduced ψ_m/a_im
  when `K_s < M`, materialized field otherwise); both branches are exercised
  below (`K_s = 64` at 20 m, `23` at 100 m, `M = 25`).
- **A second surface.** A channel with a defined thalweg,
  `z = S·x + T·|y − y_c|`, `T = 0.02` (2 m lateral rise over the 100 m
  half-width, 0.4 m streamwise drop), spun 6000 s. Max depth 0.44 m on the
  centre line; the whole surface stays wet (rain-fed sheet flow on the
  flanks), window drift 2 cm.

## 2. Results (M = 25; member coverage / median width ratio, same-index rule)

| cell | path | member cov | width-med | in [0.3,3] | C1/C2 |
|---|---|---|---|---|---|
| plane, correlated rain, ℓ = 20 m | adv, materialized | **0.897** | 1.31 | 0.99 | **calibrated** (floor asserted) |
| plane, correlated rain, ℓ = 100 m | adv, reduced | **0.886** | 1.42 | 0.77 | **calibrated** (floor asserted) |
| plane, correlated Manning, ℓ = 20 m | diagonal, grounded basis | **0.649** | 0.68 | 0.81 | ranking only → **H15** |
| plane, correlated Manning, ℓ = 100 m | diagonal, grounded basis | 0.835 | 1.24 | 0.88 | calibrated |
| plane, correlated Manning, ℓ = 20 / 100 m | diagonal, Neumann basis | 0.39 / 0.48 | 0.29 / 0.39 | 0.49 / 0.62 | reference, ungated |
| channel, comonotone Manning | legacy / iso / aniso / **adv** | 0.92 / 0.89 / 0.90 / **0.946** | 3.04 / 2.20 / 2.28 / **2.93** | 0.45 / 0.58 / 0.57 / 0.49 | **conservative** → H13 (2D) |
| channel, correlated rain, ℓ = 20 m | adv, materialized | 0.910 | 1.60 | 0.77 | conservative (just over 1.5) |

(The W3 plane under comonotone Manning, adv rung: 0.82 / 1.32, unchanged —
that gate is `regression_2d_rom_marcher_coverage` and was not re-run here.)

## 3. Findings

**(a) The 2D CORR_LEN rain path is calibrated, on both branches.** This is
the first MC behind SP3's 2D wiring: 0.897 and 0.886 member coverage at two
correlation lengths spanning the reduced/materialized split, width within the
1.5 ceiling. The soft-forcing `R_ij` enters the production reduced operator
unchanged, so this validates the path users reach through
`[SOFT_RAINFALL_GRID] COHERENCE CORR_LEN`. Both cells now assert the C1 floor.

**(b) Spatially correlated Manning never reaches the production operator**
(**fixed by H15 the same day — see the H15 section above; the numbers below
are the pre-H15 record**).
`SpectralROM::advance()` takes the reduced-operator path only when
`spatial_mannings` is unset; a correlated Manning field (the `[2D_ROM]
MANNINGS_CORR_LEN` key, `CorrelatedFieldGenerator` in `seedROM`) runs on the
diagonal `λ·K_eff` path W3 retired for everything else. Measured on that
path: at ℓ = 100 m the band is calibrated (0.835 / 1.24); at ℓ = 20 m it is
**too narrow** (0.649 / 0.68, ranking only). Short-range roughness variation
produces a local depth response the diagonal Rayleigh-quotient rate cannot
carry. The Neumann basis is far worse at both lengths (0.39 / 0.48) —
grounding matters here exactly as it did in W3. Logged as candidate **H15**:
route spatial Manning through the reduced operator (per-member `M_i` is k×k
per member — affordable at k = 40 — or a member-wise effective scalar).
Until then, `MANNINGS_CORR_LEN` below the mesh's hydraulic scale is a
ranking, and the USER_GUIDE says so.

**(c) On a converging channel the comonotone-Manning band is ~2.9× too wide
on every rung, and the production rung is the widest.** This is the 2D face
of H13 (free-surface Manning elasticity): on the plane W3 measured 1.32×, on
the channel 2.2× (iso) to 2.9× (adv). Coverage is high (0.95) because the
band is over-wide, not because it is right. The advection term widens the
band further on this surface (adv 2.93 vs iso 2.20) — the opposite of its
effect on the plane (1.43 vs 0.91 in W3), which says `c_k = (5/3)u` from a
Green–Gauss velocity on a laterally converging field is not the same object
it is on a uniform sheet. Two follow-ups, not one: the elasticity (H13's 2D
half — the 1D `alpha_free = 0.6` has no 2D counterpart, the 2D ROM has no
`alpha` path) and the advection constant on non-sheet flow (W4b, a
calibration sweep on the channel, same protocol as W3).

**(d) Correlated rain on the channel: 0.910 / 1.60.** Calibrated coverage,
width a hair over the ceiling — the same ~1.2–1.6× over-width the rain path
shows on the plane, plus a little of (c).

## 4. What W4 does not do

No fix. No change to any operator, constant, or default. The two cells that
assert are the ones that validate a claim nothing else validated; every other
cell prints its `[C1/C2]` verdict. The surfaces are still synthetic
(structured 40×40 meshes); an irregular real-terrain mesh is the next breadth
step if one is wanted.

## 5. Reproduction

    ctest --test-dir build/<dir> -R regression_2d_rom_marcher_coverage_corr        # ~11 min, label slow
    W4_CELLS=plane-rain-corr20,channel-mann-comono W4_MEMBERS=25 \
        OPENSWMM_2D_BACKEND=cpu build/<dir>/tests/regression/test_2d_rom_marcher_coverage_corr
    # knobs: W4_CV (0.20), W4_THALWEG (0.02), W4_MEMBERS (25)

# Bellinge baseline correction — the "24 h" window was truncated (PR P6)

**Measured 2026-10-02. This supersedes every Bellinge band-width number quoted
before it, including the per-cell table in the P4 section below.**

`tests/regression/test_rom_coverage_bellinge.cpp` was documented as the 24 h
saturation baseline (coverage 0.990, max width ratio ≈ 0.007). Its stepping
loop stopped at 20,000 engine steps. Reaching 24 h on this model takes
**65,589 steps**, so the run ended roughly a third of the way in — before the
29 Jun 2012 rain burst (04:26–08:04). The test now terminates on simulated
time and **asserts the clock reached 24.00 h**, so a truncated window can no
longer pass silently.

| Quantity (1,011 junctions × 288 report times) | Old (truncated) | Complete 24 h run |
|---|---|---|
| Fraction of samples with nonzero spread | 0.990 | **0.988** |
| Band ÷ absolute head, max | 0.007 | **0.277** |
| Band ÷ absolute head, p50 / p90 / p99 | not recorded | 0.0002 / 0.0013 / 0.0118 |
| Band ÷ local depth (depth ≥ 0.05 ft), p50 / p90 / p99 / max | not recorded | **0.153 / 0.73 / 2.00 / 10.7** |
| Absolute band, p50 / p90 / p99 / max (ft) | not recorded | 0.019 / 0.095 / 0.58 / 22.4 |

**Read the ratios correctly.** The harness's "width ratio" divides by
*absolute head* (about 328 ft on this model, internal feet), which mostly
measures how high the network sits above datum, not how uncertain the water
level is. Band ÷ local depth is the quantity that compares a metres-deep trunk
with centimetre-deep laterals; its median (0.153) agrees with the independent
peak-instant analysis (0.117), which samples each node only at its own flow
peak.

**What this test is and is not.** It runs the ROM once; there is no brute-force
Monte Carlo in it. Its assertions are a regression lock around the measured
values, not a coverage validation, and the harness's "coverage" is the fraction
of samples with nonzero spread, not MC bracketing. Band magnitude on Bellinge
at storm peak remains **unvalidated**: 113 nodes have a band wider than their
own water column, and only 14% of those are at crown, so the documented
surcharge over-prediction (H5) does not explain them.

**What this does and does not invalidate.**
- H5's and H11's gates are *not* affected: H5's 0.990 / 1.031 come from its own
  dedicated surcharged-chain fixture, and H11 built its own front-passage
  fixture. Neither measured on Bellinge. (Checked 2026-10-02.)
- The P4 per-cell Bellinge table **was** truncated too (its EXPLICIT/off cell
  reported max ratio 0.0068, the truncated baseline's value). **Re-run
  2026-10-10 on the full 24 h window** — see the full-window table in the P4
  section; the structural conclusion (four cells consistent) holds and the
  magnitudes are now the complete-storm ones.

**Reproduction.** The test needs the model and rainfall record, which live
outside the repository and not in the same directory. Set
`OPENSWMM_BELLINGE_INP` and `OPENSWMM_BELLINGE_RAIN` (defaults:
`~/Projects/SWMM_inp/Bellinge/7_SWMM/BellingeSWMM_v021_nopervious.inp` and
`~/Downloads/article/runs/bellinge/rg_bellinge_Jun2010_Aug2021.dat`); the test
skips if either is missing. Rainfall format is
`gage year month day hour minute value`. Runtime is 5–10 minutes, so the test
carries a `slow` label (`ctest -LE slow` skips it).

    ctest --test-dir build/<dir> -R regression_rom_coverage_bellinge

---

# Solver-mode compatibility: 1D ROM vs NODE_CONTINUITY × Anderson (PR P4)

Measured 2026-08-04. Two complementary harnesses, both registered:
`tests/regression/test_rom_solver_mode_compat.cpp` (structural invariants,
fast, gated) and a validation-only scratch rerun of the Bellinge
self-consistency baseline (`test_rom_coverage_bellinge.cpp`'s methodology,
not itself re-registered per cell — see §3 for why).

## 1. Experiment

**Structural invariants** (`test_rom_solver_mode_compat.cpp`, gated,
~0.2 s): a 5-junction chain sized so peak inflow (1.0 CMS) stays at ~65% of
the 1 m pipe's Manning full-flow capacity at n = 0.03 — enough backwater to
keep average Picard iterations at 3.6–3.9 across all four cells (so
Anderson's mixing path genuinely executes on most steps) while never
surcharging. Four cells: `NODE_CONTINUITY {EXPLICIT, SEMI_IMPLICIT} ×
Anderson {off, on}`. Checked per cell: (a) deterministic head trajectory
bit-identical with the ROM built vs not; (b) zero-perturbation exactness
(q05 = q50 = q95 = deterministic head) and monotone quantiles with a real
perturbation; (c) ROM band width at matched (node, time) within 2× across
every pair of the four cells.

**Bellinge self-consistency rerun** (validation-only, not a permanent ctest
target — see §3). **⚠ Likely truncated at ~1/3 of 24 h; see the P6 correction
above — magnitudes below are pre-storm until re-run:** the same 1020-node, 24 h Bellinge fixture as the main
Bellinge baseline (`docs/uncertainty` cross-reference: `.memory/current.md`,
"BELLINGE FIXTURE COMPLETE"), rerun once per cell with `NODE_CONTINUITY` /
`ANDERSON_ACCEL` injected into `[OPTIONS]`. "Coverage" here is the Bellinge
harness's own metric (fraction of (node, report-time) samples with
resolvable ROM spread, > 1e-8), not a brute-force-MC-vs-ROM comparison — see
§3 for the scope tradeoff.

## 2. Results (measured)

**Structural invariants — all four tests pass in every cell:**

| Check | Result |
|---|---|
| Deterministic path bit-identical (ROM on vs off) | pass, all 4 cells |
| Zero-perturbation exactness | pass, all 4 cells |
| Monotone quantiles (q05≤q50≤q95) | pass, all 4 cells |
| Cross-cell band width within 2× | **99.4%** of 900 comparisons (worst 2.86, early-transient) |
| avg Picard iterations per cell | EXPLICIT 3.59–3.85, SEMI_IMPLICIT 3.63–3.88 |

**Bellinge self-consistency, per solver-mode cell:**

| NODE_CONTINUITY | Anderson | Coverage | Width ratio min/med/max |
|---|---|---|---|
| EXPLICIT | off (baseline) | 0.992 | 0.00000 / 0.00003 / 0.00679 |
| EXPLICIT | on | 0.964 | 0.00000 / 0.00002 / 0.01315 |
| SEMI_IMPLICIT | off | 0.984 | 0.00000 / 0.00002 / 0.03359 |
| SEMI_IMPLICIT | on | 0.969 | 0.00000 / 0.00002 / 0.00611 |

All four cells clear the checklist's coverage ≥ 0.95 floor; width ratios stay
in the same small, bounded range as the EXPLICIT/off baseline (Bellinge is a
flat, wide network — see the main Bellinge section for why absolute ratios
are small here relative to the Phase-9 chain).

**Full-window re-run (2026-10-10, P6 follow-up).** The table above was
produced on the truncated window P6 found (it stops before the storm). The
Bellinge test now takes `P4_NODE_CONTINUITY=EXPLICIT|SEMI_IMPLICIT` and
`P4_ANDERSON=0|1`, injected inside the real `[OPTIONS]` block and read back
from the parsed context (`CELL: node_continuity=… anderson=…`) so a silently
no-op'd option cannot masquerade as a cell. Release build, 24.00 h reached in
every cell, engine state as of H16 (H14 directional operator on, H5b):

| NODE_CONTINUITY | Anderson | steps | nonzero-spread | band/abs head p50 / p99 / max | band/local depth p50 / p90 / p99 / max |
|---|---|---|---|---|---|
| EXPLICIT | off | 65,589 | 0.987 | 0.0005 / 0.0227 / 0.268 | 0.316 / 2.07 / 4.98 / 44.5 |
| EXPLICIT | on | 65,618 | 0.983 | 0.0005 / 0.0204 / 0.266 | 0.306 / 2.04 / 4.82 / 42.7 |
| SEMI_IMPLICIT | off | 65,051 | 0.991 | 0.0004 / 0.0164 / 0.255 | 0.259 / 1.87 / 4.39 / 18.7 |
| SEMI_IMPLICIT | on | 64,930 | 0.991 | 0.0005 / 0.0156 / 0.258 | 0.277 / 1.87 / 4.31 / 30.3 |

Reading: the four cells agree to within ~20 % on every percentile — the
structural conclusion survives the full storm. The one real difference is
the tail: SEMI_IMPLICIT's unified continuity shrinks the worst band/depth
(44.5 → 18.7 off, 30.3 with Anderson), consistent with H5's finding that EXPLICIT's discrete
surcharge branch produces the steeper, less regular response at pool
nodes. Absolute-head ratios ~40× the truncated values, exactly as P6
predicted. All four pass the P6 locks (band/depth p50 ∈ [0.02, 0.5],
max band/head ≤ 0.5). Still a regression lock, not MC validation.

## 3. Findings and scope

**(a) A genuine fixture-design trap, not a solver-mode bug.** The first
attempt at the structural-invariants fixture reused PR-10's shape with a
tripled inflow peak (3.0 CMS) — ~2× the pipe's full-flow capacity — which
floods J1 under *both* continuity modes, but at different times. That
produced spurious cross-cell band-width ratios up to **105×**. Root cause:
`EXPLICIT`'s discrete surcharge branch and `SEMI_IMPLICIT`'s unified
`dy = dV/(surfArea + sumdqdh·dt)` formulation are *designed* to diverge once
a node floods — that divergence is the entire reason `SEMI_IMPLICIT` exists.
Comparing ROM statistics riding on two deterministic solutions that
legitimately disagree for physical reasons is not a compatibility test; it
tests whether the fixture avoids the one regime the two modes were never
meant to agree in. Reducing peak inflow to 1.0 CMS (~65% of capacity)
resolved it: EXPLICIT and SEMI_IMPLICIT then converge to near-identical
deterministic depths (measured 1.509 vs 1.509 ft at the transient peak).
Full diagnosis in `history_decisions.md`, "P4 compat matrix: first fixture
attempt flooded the network."

**(b) Anderson acceleration does not measurably reduce Picard iterations on
either fixture tried** (structural: 3.592→3.845; Bellinge: no iteration
counter exposed, but coverage/width numbers are consistent with/without it).
Consistent with the pre-existing StormCity finding that this
implementation's depth-1 mixing provides little benefit. Anderson's code
path does execute (`t_steps > 1` reached on most stressed steps), which is
what the checklist's "not vacuously green" requirement needs — this is a
compatibility finding (nothing breaks), not a performance one.

**(c) Scope tradeoff — no true brute-force MC vs ROM comparison across all
four cells.** The checklist's original phrasing ("rerun the PR-10 MC
coverage harness... brute-force members and ROM in the SAME mode") assumed
the Phase-9 chain's cheap (22-run, ~90 ms) brute-force methodology. That
fixture cannot legitimately assert coverage in a 1-hour window regardless of
solver mode (root-caused separately: τ₀ ≈ 15 h spin-up, not a solver-mode
effect — see the main PR-10 section and `.memory/current.md`). Running a
21-member brute-force MC on the 1020-node, 24 h Bellinge model across 4
solver-mode cells would cost on the order of hours of compute for a
regression gate, which is disproportionate to what this check needs to
prove. Instead: (i) the fast structural-invariants test proves the ROM
genuinely does not care which solver-mode path produced its deterministic
inputs (bit-identical, exact, monotone, band-consistent) — arguably a
stronger compatibility claim than a single brute-force snapshot would be;
(ii) the Bellinge self-consistency rerun confirms the already-established
saturation-regime baseline (coverage, width-ratio) does not degrade under
`SEMI_IMPLICIT`/Anderson. A true brute-force-vs-ROM Bellinge comparison
remains a possible future addition if a specific solver-mode regression is
ever suspected there.

## 4. Reproduction

    # Structural invariants (gated, ~0.2 s):
    ctest --test-dir build/<dir> -R regression_rom_solver_mode_compat

    # Bellinge full-window regression lock (gated, label `slow`, 5-10 min; see the P6 section):
    ctest --test-dir build/<dir> -R regression_rom_coverage_bellinge

    # Bellinge per-solver-mode rerun (validation-only, not a ctest target,
    # ~400 s for all 4 cells): scratch harness pattern, compile against the
    # engine dylib per the W3 calibration convention in .memory/current.md.

---

# Surcharged-regime sensitivity attenuation (PR H5)

Measured 2026-08-06. `tests/regression/test_rom_coverage.cpp`, new
`RomCoverageSurcharged` suite (2 tests, one per `NODE_CONTINUITY` mode),
alongside the pre-existing `RomCoverage` (free-surface, still deliberately
red per its own header, unrelated to and unaffected by H5).

## 1. Experiment

**Problem**: in surcharged regime the pre-H5 ROM over-predicted band widths
50–190× (PR-10's documented limit). Mechanism: the modal Manning-sensitivity
source term `-λ·K1d·(1/mm-1)·b_j` encodes free-surface conveyance sensitivity
(`K ~ h^(5/3)/n`), which no longer governs once a pipe runs full and heads are
set by mass balance/backwater instead.

> **Superseded 2026-10-06 by H5b** (section above): the ramp-to-a-floor
> below is replaced by a two-level elasticity (0.6 free-surface, 2.0
> surcharged); `alpha_floor` no longer exists. This section is kept as the
> record of how the floor was arrived at and why it was wrong.

**Fix** (`src/engine/uncertainty/RomSurchargeAttenuation.hpp`, PR H5's F
design decision): a per-active-node factor `alpha_n` folds into the
sensitivity reference *before* projection (`b_j = Pᵀ(alpha ⊙ bref)`),
damping only the Manning-sensitivity channel — the forcing-sensitivity
channel (runoff/soft-rain, projected separately) is untouched. `alpha_n` is a
smooth ramp of `ratio_n = (head-invert)/(crown_elev-invert)` over
`[ramp_lo, ramp_hi] = [0.9, 1.1]`, floored at `alpha_floor` rather than
ramping to a hard 0 — see §2 for why the floor exists and how it was
calibrated. One amendment to the checklist's literal candidate ("fraction of
incident conduits free-surface, per-conduit ramp"): the engine exposes crown
data as a per-*node* scalar (the same quantity `HSnapshot::node_surcharged`
already uses), not per-conduit, so `alpha_n` ramps that same node-level
quantity rather than inventing new per-conduit geometry plumbing for a value
that would immediately re-aggregate to a node scalar anyway.

**Fixture** (`fixtureInpSurcharged()`): same 5-junction chain as the
free-surface test, but C1–C4 stay at a generous 1.0 m diameter and C5 (into
the outfall) is undersized to 0.3 m — a single, localized bottleneck rather
than a uniformly undersized chain. DWF at J1 raised to 0.40 CMS. This
specific design **replaced an earlier, abandoned attempt** — see §3(a) for
why the obvious "just raise inflow past chain-wide capacity" approach isn't a
validatable fixture at all. Same 21-member brute-force MC design as PR-10
(LHS strata of the checklist's ±20% Manning prior), run once per
`NODE_CONTINUITY` mode, MC members and the ROM in the same mode per cell (the
P4 compat-matrix pattern).

## 2. Results (measured)

| NODE_CONTINUITY | Coverage | Width ratio min/med/max | Surcharged frac | Verdict |
|---|---|---|---|---|
| EXPLICIT | 0.997 | 0.016 / **0.034** / 1.866 | 1.000 | **FAILS** ratio_med ≥ 0.3 |
| SEMI_IMPLICIT | 0.990 | 0.021 / **1.031** / 2.799 | 1.000 | **passes** |
| *corrected 2026-10-04 (midpoint MC quantiles)* | median containment 0.997 · **member coverage 0.51** | 0.000 / **0.982** / 2.665 | — | still passes [0.3, 3]; **not calibrated**: half the MC members fall outside the band |

Both cells clear coverage ≥ 0.90 and surcharged_frac ≥ 0.50 comfortably. The
alpha floor (see below) was calibrated against this exact fixture, so
SEMI_IMPLICIT's ratio_med landing near 1.0 (0.982 after the 2026-10-04 metric correction; member coverage only 0.51, so the median width is right while the per-sample band is often mis-centred — see the interval-coverage audit) is a genuine
calibration result, not a coincidence — but the same floor leaves EXPLICIT's
median an order of magnitude below the checklist's own 0.3 floor. Both
findings are explained in §3, not adjusted away.

**Calibration record** (`RomSurchargeAttenuation.hpp`,
`SurchargeAttenuationConfig::alpha_floor`): the checklist's literal candidate
ramps `alpha_n` to a hard 0 deep in surcharge. Validated against this
fixture, that collapsed the SEMI_IMPLICIT band to ratio_med ≈ 0 (measured,
not estimated) — an over-correction from "50–190× too wide" to "collapsed to
near-nothing," because a pressurized pipe still loses head to friction, and
that loss still depends on n (just not through the free-surface `h^(5/3)/n`
law the un-attenuated ROM assumes). `alpha_floor = 0.05` was chosen as the
value that lands SEMI_IMPLICIT's median closest to the ideal 1.0 while
keeping its max under the checklist's 3.0 ceiling; `alpha_floor = 0.15` was
also measured (SEMI_IMPLICIT ratio_med 1.031 → 0.565, non-monotonic — see
§3(c) — while EXPLICIT's median moved only 0.034 → 0.061, nowhere near 0.3)
and rejected as strictly worse on both counts.

## 3. Findings and scope

**(a) A fixture-design trap specific to this PR, found and fixed by
redesign, not by threshold tuning.** The first fixture attempt uniformly
undersized *every* conduit and pushed DWF well past the whole chain's
capacity (mirroring PR-10's/P4's own topology-reuse habit). Scratch-harness
measurement (2026-08-06) found this was not a validatable regime at all: at a
fixed DWF, the ±20% Manning prior alone flipped the network between "stays
below crown" (rough_mult 0.83, never surcharges) and ">36 m over crown"
(rough_mult 1.17) — a near-bifurcation in capacity-vs-demand, not a graded
response. MC members landing on opposite sides of that split don't have a
comparable band to measure a ROM against. Separately, pushing DWF far enough
past capacity to guarantee *every* member surcharges was found to hit an
apparent flooding ceiling (identical settled head regardless of DWF or
roughness once deep enough over capacity — confirmed via a MaxDepth sweep
that the *node* values, not a diagnostic bug, genuinely saturate). Localizing
the bottleneck to a single conduit (C5) turned the chain-wide compounding
into a single-stage, continuously-graded response (§1's measured 90–95%
surcharged fraction across the same roughness range, monotonic, no jump) —
this *is* a working fixture. Recorded in full in `history_decisions.md`.

**(b) EXPLICIT's discrete surcharge branch shows steeper Manning sensitivity
at a chokepoint than SEMI_IMPLICIT's unified formula — root-caused, not
assumed.** Per-node breakdown (scratch diagnostic, 2026-08-06) of the
EXPLICIT cell's failure: J1 (never attenuated, alpha≈1) has ratio ≈ 1.87,
comfortably in-band — the ROM's ordinary free-surface behavior is intact.
J4/J5 (downstream of the C5 bottleneck, alpha at the floor) have ratio ≈
0.02, and the brute-force MC's *own* q05–q95 width there is ≈28 m: the same
±20% Manning prior that gave EXPLICIT's settled backwater depths of ~19 m
(rough_mult 0.83) to ~47 m (rough_mult 1.17) at nominal DWF (§1's design
measurement) reproduces almost exactly as the MC width at the nearest-rank
5th/95th-percentile members. This is a real, steep, physical sensitivity of
single-conduit pressurized backwater to conveyance near a chokepoint under
EXPLICIT's discrete branch — bridging it would need `alpha_floor` in the
range of 0.7–0.8 (measured by direct trial), which would erase most of the
attenuation this PR exists to add, and is not compatible with keeping
SEMI_IMPLICIT's max under the checklist's 3.0 ceiling (see (c)). **Left RED**
rather than loosening the checklist's [0.3, 3.0]/≥0.90 acceptance bounds
(hard rule 2) — see `tests/regression/test_rom_coverage.cpp`'s own
`@warning` block on `RomCoverageSurcharged.ExplicitContinuity` for the same
finding recorded next to the failing assertion.

**(c) `alpha_floor` response is not monotonic across both cells
simultaneously — a genuine model-parameter finding, not noise.** Raising the
floor 0.05 → 0.15 (3×) moved EXPLICIT's median only 0.034 → 0.061 (≈1.8×,
not 3×) and *decreased* SEMI_IMPLICIT's median (1.031 → 0.565) while also
decreasing its max (2.799 → 1.853). A larger `alpha_floor` raises
`b_coarse[j]` for every attenuated node, which can cross `mode_drop_
threshold` and activate previously-inactive modes (`by_manning` in
`SpectralROM1D::advance()`'s Step 3) whose own sign/contribution isn't
predictable from the floor's magnitude alone — a single global scalar
does not respond linearly once mode activation is in play. This rules out
further blind grid-search over `alpha_floor` as a productive path to close
(b): the mechanism that would need to change is not "the floor is too
small," it is something structurally different between how EXPLICIT and
SEMI_IMPLICIT drive `b_coarse[j]` near a chokepoint.

**(d) Resolved 2026-08-09: the gap is not fixture-severity-dependent, so it
is accepted and documented rather than chased further.** Before deciding
between "try a different mechanism" and "document as a limitation," checked
the cheap alternative first: does a *milder* surcharge (smaller excess
inflow over the C5 bottleneck's capacity) narrow EXPLICIT's dynamic range
enough to pass without changing `alpha_floor`? Swept DWF from 0.25–0.40 CMS
(vs. the fixture's own 0.40) and measured settled head-crown across the same
rough_mult ∈ {0.83, 1.0, 1.17}. Result: it does not help, and can make the
*ratio* of extremes worse — near the threshold the low-roughness member's
settled depth over crown shrinks toward zero (e.g. DWF=0.30 gives
3.0/12.9/23.0 m across the three strata, a **7.5×** spread vs. the fixture's
own 2.4× at DWF=0.40), because the ratio's denominator is what's shrinking,
not the physical spread narrowing. DWF=0.40 (the shipped fixture) is in fact
the best-behaved point measured in this sweep, not an unlucky choice.
**Conclusion**: EXPLICIT's chokepoint sensitivity is a property of the
regime (discrete surcharge branch near a single restrictive conduit), not an
artifact of how severely this particular fixture forces surcharge — a
different mechanism (e.g. a flow/Froude-keyed attenuation signal instead of
a pure depth-ratio one, reusing PR H3's existing `link_froude` machinery)
remains a real option but is its own formulation R&D effort, not a quick
follow-up, and the floor-response was already shown non-monotonic (finding
(c) above) so further constant-tuning is not it either. **Decision: accepted
as a documented limitation of the source-side-attenuation formulation
specifically under `NODE_CONTINUITY EXPLICIT`.** Recorded in
`docs/uncertainty/HOW_IT_WORKS.md` §8 and `USER_GUIDE.md` limitation 10,
both recommending `SEMI_IMPLICIT` when surcharged bands are needed (validated for width on median; member coverage 0.51, see the interval-coverage audit) — i.e. when they are
needed. `RomCoverageSurcharged.ExplicitContinuity` stays registered and
deliberately red — a live regression guard against this getting *worse*,
and a ready-made gate if a future session takes on the flow-keyed-signal
option above.

## 4. Reproduction

    # Both NODE_CONTINUITY cells (SEMI_IMPLICIT passes, EXPLICIT deliberately red):
    ctest --test-dir build/<dir> -R regression_rom_coverage

    # Isolate one cell:
    build/<dir>/tests/regression/test_rom_coverage --gtest_filter='RomCoverageSurcharged.*'

    # Pure-function ramp/floor unit tests (independent of the MC fixture):
    ctest --test-dir build/<dir> -R test_engine_rom_surcharge_attenuation

---

# H5b — Surcharged band recalibrated against member coverage (2026-10-06)

**Branch** `hsym2/h5b-surcharge-recal` (stacked on C2). F design + impl in one
session, per the owner's "please proceed with H5b". Everything below is
measured on the committed fixtures (`tests/regression/test_rom_coverage.cpp`),
21-member LHS MC vs ROM M=50, midpoint plotting-position quantiles, member
coverage per the P7 audit.

## 1. Breakdown first (spec step 1)

Per-node, late-window (t ≥ 1800 s) means on the H5 surcharged chain
(`fixtureInpSurcharged`, 5 % slope, 1.0 m pipes, C5 = 0.3 m chokepoint,
DWF 0.40 CMS), `NODE_CONTINUITY SEMI_IMPLICIT`, **pre-H5b** (H5's ramp to
`alpha_floor = 0.05`):

| node | regime (det head above invert) | MC q05–q95 width | ROM width | ratio |
|---|---|---|---|---|
| J1 | free-surface, supercritical reach | 0.108 ft | 0.21 ft | 1.9 |
| J2 | free-surface | 0.108 ft | 0.20 ft | 1.9 |
| J3 | free-surface, directly upstream of the pool | 0.108 ft | 0.43 ft | 4.0 |
| J4 | surcharged (pool, +1.4 m over crown) | 9.23 ft | 2.9 ft | **0.31** |
| J5 | surcharged (pool, +6.4 m) | 10.03 ft | 3.1 ft | **0.31** |

Exactly the shape the checklist predicted: wide upstream, ~3× **narrow** at
and below the chokepoint. The H5 median of 0.98 was the median of a bimodal
set (member coverage 0.51).

Two diagnostics decided the design rather than a floor sweep:

- **The attenuation sign was wrong at the pool.** In the MC the surcharged
  heads move *more* with n than the free-surface ones, not less: J5's 10 ft
  of spread for ±20 % n is the pressurised-friction law (`h_f ∝ n²` at fixed
  Q, elasticity **2.0**), whereas the free-surface normal-depth law gives
  `d ∝ n^0.6` (Manning, wide section; elasticity **0.6**). H5 had the
  surcharged source *damped* to 5 % of a channel that itself assumed
  elasticity 1. Both the "50–190× over-prediction" that motivated H5 and the
  0.31× under-prediction left after it are the same mistake viewed from two
  sides: an elasticity of 1 where the physics says 0.6 (free) or 2 (full).
- **P8 had already measured the free-surface factor.** The redesigned
  free-surface fixture sits at ratio 1.96 at saturation, i.e. the channel
  is ~1/0.6 too strong — the "H13 candidate" (~2× over-prediction). The
  same 0.6 appears here at J1/J2 (1.9×).

## 2. Design (spec step 2): a two-level elasticity, not a floor

`RomSurchargeAttenuation.hpp` is rewritten. `alpha_n` is no longer a damping
factor that ramps from 1 toward a floor; it is the **Manning elasticity of
the local head**, blended between two physical values on the same
depth/crown ramp H5 used:

    alpha_n = alpha_free + (alpha_surcharged − alpha_free) · s,
    s = clamp((depth/crown − ramp_lo) / (ramp_hi − ramp_lo), 0, 1)

with `alpha_free = 0.6`, `alpha_surcharged = 2.0`, `[ramp_lo, ramp_hi] =
[0.9, 1.1]`. `alpha_floor` is gone. Degenerate/out-of-range nodes get
`alpha_free`. `SWMMEngine::rom1dSurchargeConfig()` exposes the config (the
MC harness reads `H5B_ALPHA_FREE` / `H5B_ALPHA_SURCHARGED` / `H5B_RAMP_LO` /
`H5B_RAMP_HI` from the environment for sweeps; nothing in the `.inp`).
Pure-function tests rewritten (`test_rom_surcharge_attenuation.cpp`, 10),
lifecycle test now asserts the factor *rises* 0.6 → ≥1.9 as a node
surcharges.

This is model calibration against ground truth in the same sense as H5's
own floor was, with one difference worth recording: neither constant was
fitted to this fixture. 0.6 and 2.0 are the textbook exponents, and they were
checked against four independent cells before being adopted (table below).

## 3. Result (spec step 3), every 1D MC cell, committed values

| cell | pre-H5b | **H5b** | C1/C2 verdict |
|---|---|---|---|
| PR-10 free-surface chain (P8 fixture) | 0.94 / 1.961 | **0.867 / 1.177** | **calibrated** (was conservative → H13) |
| H11 front passage, phase coordinate | 0.83 / 1.295 | **0.829 / 1.290** | calibrated (unchanged within noise) |
| H5 surcharged, SEMI_IMPLICIT | 0.51 / 0.982 | **0.842 / 5.446** | **conservative** (coverage met, over-wide) |
| H5 surcharged, EXPLICIT | 0.38 / 0.033 | **0.693 / 0.694** | ranking only (documented limitation, unchanged status) |

(coverage / median width ratio.) Per node on the SEMI cell, **H5b**:

| node | MC width | ROM width | ratio | member coverage |
|---|---|---|---|---|
| J1 | 0.108 ft | 0.743 ft | 7.5 | 0.868 |
| J2 | 0.108 ft | 0.721 ft | 7.4 | 0.904 |
| J3 | 0.108 ft | 1.525 ft | 15.1 | 0.954 |
| J4 | 9.234 ft | 6.651 ft | 0.70 | 0.776 |
| J5 | 10.034 ft | 10.187 ft | **1.02** | 0.709 |

H5b's own acceptance is met: member coverage 0.842 ≥ 0.80 and no junction
below 0.60 (min 0.709 at J5). The surcharged nodes are now right (J5 exact,
J4 0.70 — J4 sits on the ramp's shoulder, partially blended). What remains is
the **opposite** problem at the free-surface nodes: J1–J3 are 7–15× over.

## 4. The residual is leakage through the symmetric operator, not the source

Three sweeps, all on the SEMI cell, late-window J1/J2/J3 ROM widths (MC is
0.108 ft at all three):

| `alpha_surcharged` | `alpha_free` | J1 | J2 | J3 | J5 |
|---|---|---|---|---|---|
| 2.0 | 0.6 (shipped) | 0.743 | 0.721 | 1.525 | 10.19 |
| 1.0 | 0.6 | 0.312 | 0.300 | 0.931 | 4.98 |
| 0.6 | 0.6 | 0.140 | 0.131 | 0.503 | 2.89 |
| 2.0 | **0.0** | 0.861 | 0.842 | 1.602 | 10.13 |

The last row is decisive: with the free-surface source **zeroed**, J1–J3
keep (slightly exceed) their full width. Their band is not their own
Manning sensitivity at all; it is J4/J5's 10 ft of surcharged deviation
carried upstream by the operator. The first three rows show the same thing
from the other side — J1–J3 scale with `alpha_surcharged`, the parameter
that only acts two nodes downstream.

Physically the reach C3 (J3→J4) is a 5 % slope running ~0.18 m deep at
0.4 CMS: supercritical, so nothing about the pool can influence J3 in the
Saint-Venant system, and the MC confirms it (J3 width 0.108 ft, identical to
J1). The ROM's operator is the symmetric weighted Laplacian from the Picard
`dqdh` (one coefficient per link, applied both ways) — it has no notion of
flow direction, so a 10 ft deviation at J4 diffuses upstream exactly as it
would downstream. This is the gap H3 named ("the skew lives in the gap", the
`fr_trust` diagnostic) made concrete: **on a supercritical reach feeding a
surcharged pool, the band upstream of the pool is the pool's band, smeared.**

Ruled out along the way, measured not inferred:
- **Basis truncation.** Lifting both mode caps (`k = n_active` in
  `buildROM1D`, `k_req = n` in `GraphEigenBasis`) left J1–J3 at 0.748 /
  0.754 / 0.763 and worsened J4 (2.1 ft). Reverted; all golden-reference
  basis tests passed under the experiment, so it was a null result, not a
  broken one.
- **Pool-node misclassification.** The depth/crown ramp classifies J3 as
  free-surface — correctly: it is free-surface, and its MC band says so.

## 5. Status of the SEMI_IMPLICIT cell and what was *not* done

`RomCoverageSurcharged.SemiImplicitContinuity` now **fails** H5's own
`ratio_med ≤ 3.0` ceiling (5.45), having previously passed it on a median
that hid a 0.51 coverage. Per standing rule 2 the ceiling is not moved, and
per C2 the cell's honest label is **conservative**: every node's member
coverage clears the floor, the surcharged nodes are calibrated, and the
free-surface nodes upstream of the pool are over-wide by a known mechanism.
The test stays registered and red alongside the EXPLICIT cell, with the
cause written in the test file.

The fix is not a constant. It needs a directional (upwind / Froude-gated)
term in the 1D operator so that deviations do not propagate upstream across
a supercritical reach — the same family as H3's finding and the 2D
advection term W3 added. Logged in the checklist as candidate **H14**. Not
attempted here: it changes the operator every 1D consumer sees and needs its
own MC against both the P8 and the surcharged fixtures.
**Done the next day — see the H14 section: 0.842 / 5.446 → 0.832 / 1.208,
cell green, J1/J2 at ~1×; J3 remains (truncation, not the operator).**

**EXPLICIT** moved from 0.38 to 0.693 coverage (and from 0.033 to 0.69 on the
median) under the same constants, with no tuning for it. It still fails the
0.80 floor: J3 is a *pool* node under EXPLICIT (MC width 13 ft, ROM 0.69 ft,
coverage 0.53) because the discrete surcharge branch drowns J3 as well, and
the depth/crown ramp cannot see that from J3's own local depth. Status
unchanged: documented limitation.

**H11 bit-identity.** The spec asked that H11's fixture be unchanged
bit-identically with surcharge absent. It is not — `alpha_free = 0.6` acts on
every free-surface node — and that was the point of adopting the P8 finding:
H11 moved 0.83/1.295 → 0.829/1.290 (noise), while the free-surface chain went
from conservative to calibrated. Recorded as a deliberate supersession.

**Bellinge (regression lock, not MC).** P6's full-24 h gate
(`test_rom_coverage_bellinge`, cherry-picked onto this stack in the same
session — it had been left off) still passes, but its distribution moved,
and the direction deserves a sentence. Under H5b, with the same 65,589
steps and 24.00 h reached:

| statistic | P6 (H5 floor) | **H5b** |
|---|---|---|
| nonzero-spread fraction | 0.988 | 0.987 |
| band / absolute head, max | 0.277 | 0.276 |
| band / local depth, p50 / p90 / p99 / max | 0.153 / 0.73 / 2.00 / 10.7 | **0.308 / 2.02 / 4.86 / 44.6** |
| absolute band (ft), p50 / p99 / max | — | 0.035 / 1.08 / 22.4 |

The free-surface factor 0.6 should have *shrunk* the typical band; instead
band/depth roughly doubled across the distribution. The surcharged source is
now 40× what H5's floor gave it (2.0 vs 0.05), and §4 shows that a surcharged
node's deviation does not stay at that node — on a real storm network the
pool nodes' now-large (and, at the pool, correct) deviations are carried into
their free-surface neighbours by the same mechanism. Bellinge has no MC, so
this is a direction, not a verdict: the lock's bounds (p50 within [0.02, 0.5],
band/head max ≤ 0.5) hold with margin, and the band remains a ranking there,
as D1 already says. It does make H14 the next formulation item rather than a
curiosity: the leak is now the dominant way surcharged uncertainty reaches
the rest of a network.

## 6. Reproduction

    build/<dir>/tests/regression/test_rom_coverage                 # all 1D cells
    H5B_TRACE=1 build/<dir>/tests/regression/test_rom_coverage \
        --gtest_filter='RomCoverageSurcharged.SemiImplicit*'        # per-node breakdown
    H5B_ALPHA_FREE=0 H5B_TRACE=1 ... --gtest_filter='RomCoverageSurcharged.SemiImplicit*'   # leak test
    ctest --test-dir build/<dir> -R 'rom_surcharge_attenuation|1d_rom_lifecycle'
    ctest --test-dir build/<dir> -R regression_rom_coverage_bellinge   # ~8 min, label slow

# Per-member phase coordinate (PR H11)

Measured 2026-08-10. `tests/regression/test_rom_coverage.cpp`, new
`RomCoverageFront` suite (2 tests: the gated `PhaseCoordinate` case and the
ungated `AmplitudeOnlyBaseline` "before" measurement), alongside the
pre-existing `RomCoverage`/`RomCoverageSurcharged` suites (unaffected —
numbers reproduced bit-identically before and after this PR, see §3).

## 1. Experiment

**Problem**: the 1D sidecar is an *amplitude* method — member `i` carries
`δa_i = Pᵀ(h_i − h_det)`, a deviation on a fixed spatial basis evolved by a
dissipative operator. A filling front's *arrival time* is a phase error, not
an amplitude one: it neither decays nor is expressible as a deviation on a
fixed basis. PR-10's original validation (§4 item 3 above) measured a 2–4 m
transient width at front passage the amplitude channel does not represent at
all.

**Fix** (`src/engine/uncertainty/RomPhaseCoordinate.hpp`): a per-member phase
offset `τ_i(x) = (mm_i − 1)·T̄(x)`, the same `(mm−1)` structure as the
already-validated amplitude fixed point `δa_ss = (mm−1)·b_j` — both are the
first-order response of a `1/n` conveyance law. `T̄(x)` is a **path integral**
over the deterministic flow graph (`t_e = L_e/((5/3)|u_e|)` per edge, oriented
by flow sign, accumulated downstream by a bounded max-relaxation), refreshed
every 60 s from the engine's own conduit velocity state
(`SWMMEngine::refreshRom1dTravelTime()`) — **not** a solved travel-time field.
Reconstruction in `computeQuantiles()` becomes
`h_i(x,t) = H(t − τ_i) + P·δa_i`, where `H(·)` samples a bounded ring buffer
of `h_det` planes (`DetHistoryRing`), with a past-anchored reflection
`h_ref = 2H(t) − H(t−|τ|)` for faster members (`τ<0`, a query into the future
no history buffer holds). A stagnant edge (`|u_e| < u_min`) contributes
**exactly zero** travel time, not a large/undefined value — the deliberate
answer to the `u→0` regime that broke the 2D W3 drain-to-pond fixture (H11b,
still open, out of scope here). Full design rationale, including the two
non-obvious calls (stagnation → zero not infinity; negative τ → reflection not
clamp-to-now) is in the H11 design plan
(`~/.claude/plans/please-plan-h11-implementation-woolly-pearl.md`, kept as the
O48 design record per the checklist's own routing — no separate normative doc
was written since the plan already fixes every formulation decision).

**Invariant preserved**: `τ_i = 0` exactly at `mm_i = 1` for any `T̄`
(`phaseOffset(1.0, ·) ≡ 0.0`), so every pre-H11 `DeviationForm.*`/
`SurchargeAttenuation.*` test in `test_spectral_rom1d.cpp` passes **unchanged**
— `SpectralROM1D` defaults to unphased (`setTravelTime()` never called) and
`computeQuantiles()`'s phase branch is bit-identical to the pre-H11 code path
in that state (`PhaseCoordinate.NoTravelTimeIsBitIdentical`,
`PhaseCoordinate.ZeroPerturbationExactWithPhase`). On a *steady* `h_det`
(`dh_det/dt ≈ 0`, the regime the existing saturated-window gates measure in)
the phase channel adds **exactly zero** extra width by construction
(`PhaseCoordinate.SteadyDetTrajectoryPhaseAddsNothing`) — the reason §3 below
confirms the pre-existing fixtures' numbers did not move.

**Fixture** (`fixtureInpFront()`): same 5-junction chain topology as the
free-surface fixture, but **800 m conduits** (vs. 100 m) so a filling front's
arrival time is spread over multiple minutes — resolvable at
`REPORT_STEP=60s` — instead of arriving within a single routing step;
`InitDepth=0` (a genuine dry start, not the free-surface fixture's 0.10 m) so
there is an actual front to pass; a single generous 1.0 m CIRCULAR conduit
everywhere keeps the whole run free-surface (H5's `alpha≈1` throughout),
isolating the phase channel from the attenuation channel H5 already validates
separately. **Verified by scratch probe before trusting it** (this project's
standing practice after several abandoned fixture designs elsewhere in this
file): at DWF=0.15 CMS, measured front-arrival time (`t_half`, the first
report time crossing the midpoint of a node's own `[min,max]` range) at
`rough_mult ∈ {0.8, 1.0, 1.2}`:

| node | t_half (s) at 0.8 / 1.0 / 1.2 | spread | report steps (60 s) |
|---|---|---|---|
| J4 | 1350.5 / 1590.5 / 1800.5 | 450 s | 7.5 |
| J5 | 1830.5 / 2160.5 / 2430.5 | 600 s | 10.0 |

comfortably clearing the design's own "≥2 report steps" bar. Head stayed
2.5–2.7 ft below crown at every node throughout the 2-hour window at every
`rough_mult` tried — never surcharges, confirming the fixture isolates the
phase channel from H5's attenuation channel as intended. Same 21-member
brute-force MC design as PR-10 (LHS strata of the ±20% Manning prior).

**Sample selection**: per node, from the ROM run's own deterministic
trajectory, `t_50` = first report time crossing the midpoint of that node's
`[min,max]` head range over the window; compared samples are every report
time within `±3·REPORT_STEP` of `t_50` (`frontWindowIndices()`). This targets
the actual transient instead of the whole 2-hour window, most of which is
either pre-front (flat) or fully settled (also flat, and already covered by
the saturated-regime gates above).

## 2. Results (measured)

| Cell | Coverage | Width ratio min/med/max | Samples | Verdict |
|---|---|---|---|---|
| `AmplitudeOnlyBaseline` (phase disabled) | 1.000 | 0.000 / **0.009** / 0.025 | 34 | *not gated — "before" measurement* |
| `PhaseCoordinate` (phase enabled) | 1.000 | 0.000 / **1.354** / 2.554 | 34 | **passes** (coverage ≥ 0.90, ratio_med ∈ [0.5, 2.0]) |
| *corrected 2026-10-04 (midpoint MC quantiles)* | median containment 1.000 · **member coverage 0.83** | 0.000 / **1.295** / 2.448 | 34 | still passes; the best-calibrated cell measured |

**H11 recovers essentially the whole front-passage width gap**: median width
ratio goes from **0.009** (≈150× too narrow — the amplitude channel alone
barely registers the transient at all) to **1.354** (comfortably inside the
checklist's `[0.5, 2.0]` acceptance band), on the **first run of the fixture,
with no calibration of the acceptance bounds** — the physical dials
(`celerity_factor=5/3`, `u_min`, `tau_edge_max`, `tau_total_max`) were left at
their design-time defaults (`RomPhaseCoordinate.hpp`) and never tuned against
this MC result. Both cells' `min` ratio is 0.000 at the same (J1, upstream)
sample: J1 sits at the DWF source with `T̄=0` by construction (zero cumulative
travel time), so the phase channel contributes nothing there in either cell —
expected, not a defect (see the invariant note in §1: `tbar_t=0` ⇒ every
member's reference is `h_det` regardless of `mm_i`).

## 3. Findings and scope

- **Zero drift on the pre-existing fixtures.** `RomCoverage.BandsBracket
  BruteForceMonteCarlo` (coverage 0.993, ratio_med 0.102) and
  `RomCoverageSurcharged.{Explicit,SemiImplicit}Continuity` (ratio_med 0.033 /
  1.031) reproduce **bit-identically** before and after this PR — confirming
  the §1 saturated-regime invariant empirically, not just analytically.
- **No escalation needed.** Per the checklist's hard rule 2 ("any coverage
  failure returns to design, not to threshold tuning") and this PR's own §5
  escalation note: the front-passage gate passed on the design's first
  implementation, at the design's own default physical dials, against the
  checklist's own unmodified bounds. There was no tolerance tuning, fixture
  reshaping to force green, or bound loosening at any point.
- **`AmplitudeOnlyBaseline` is deliberately ungated** (asserts only that the
  run completed) — it exists purely to state, honestly, how much of the gap
  H11 recovers, matching PR-10's own "before/after" reporting style for the
  `computeK1d` PHI fix.
- **Scope boundary**: 1D only. The 2D counterpart (`H11b`, a travel-time
  *field* rather than a path integral) remains open, with the W3
  drain-to-pond fixture (coverage 0.55–0.60, §4 of the 2D marcher-
  compatibility section above) as its ready-made failing gate — not attempted
  in this PR.
- **Coupling path untouched.** `reconstructHead()` (the 2D↔1D coupling read
  path) stays unphased by design — coupling is an instantaneous per-step
  exchange anchored on the deterministic booked flux (the W2 median-drift
  fix), and a time shift there was judged to desynchronize it for no measured
  benefit. Not measured against MC in this PR; a candidate follow-up if 2D
  coupling ever needs front-passage timing fidelity.

## 4. Reproduction

    # Front-passage gate + baseline, alongside the unaffected pre-existing suites:
    ctest --test-dir build/<dir> -R regression_rom_coverage

    # Isolate the H11 cells:
    build/<dir>/tests/regression/test_rom_coverage --gtest_filter='RomCoverageFront.*'

    # Pure-function travel-time/history-ring unit tests (independent of the MC fixture):
    ctest --test-dir build/<dir> -R test_engine_rom_phase_coordinate

    # SpectralROM1D-level h_ref selection + bit-identity invariants:
    build/<dir>/bin/Debug/test_engine_spectral_rom1d --gtest_filter='PhaseCoordinate.*'

    # Engine-level T̄ plumbing (real conduit velocity -> travel time):
    build/<dir>/bin/Debug/test_engine_1d_rom_lifecycle --gtest_filter='*TravelTime*'

---

# Quantile-reconstruction GEMM (PR H4)

Measured 2026-08-16. Scratch benchmark (compiled standalone against the
engine dylib, per this project's established calibration-sweep pattern —
not a permanent `bench_*` target; the numbers below are the full record).
`test_spectral_rom1d.cpp`'s new `QuantileGemm` suite and
`test_2d_spectral_rom.cpp`'s new `QuantileGemm2D` suite (the latter newly
registered in `tests/unit/engine/CMakeLists.txt` — found dormant, see §3)
carry the correctness/equivalence/determinism assertions this section's
numbers rest on.

## 1. Problem

`computeQuantiles()`'s ensemble reconstruction (`ΔH[t,i] = Σ_j P[j,t]·a[i,j]`
over active modes, for every node/member pair) is an O(N·M·k) hand-rolled
triple loop — the dominant ROM cost at large N (the roadmap's own perf
table; CL-2d recorded it as the ~19% cap on that PR's own speedup).

## 2. Fix

`src/engine/uncertainty/RomQuantileGemm.hpp` (new, shared between the 1D and
2D ROMs — the matrix layouts are identical): `reconstructEnsembleGemm()`
compacts `P`/`a_ensemble` down to active-mode rows/columns only (preserving
the exact masking semantics the hand-rolled loop had — including a mode that
was active in the past and has since deactivated, which can still hold a
small nonzero "frozen" coefficient that must stay excluded, not just an
all-zero column), then computes the whole `[N × M]` reconstruction in one
`cblas_dgemm` call (Apple's Accelerate framework — zero new dependency, per
the checklist's own framing) or a portable compacted triple-loop fallback
everywhere else (`OPENSWMM_HAVE_CBLAS`, set by CMake only when the Accelerate
framework is found). Both `SpectralROM1D::computeQuantiles()` and 2D
`SpectralROM::computeQuantiles()` gained a `rom_quantile_naive` config field
(default `false`): when `true`, the original hand-rolled loop runs instead,
kept byte-for-byte, for equivalence testing — both paths are always compiled
in, never behind a build flag that would leave one of them untested.

**1D kept its full `std::sort`; 2D switched to three `std::nth_element`
selections — a deliberate, load-bearing difference, not an oversight.** PR
H10's `sortedMemberValues()` hands PR H10's `exceedanceFraction()`/
`gapModality()` the *whole* per-node row in ascending order — `gapModality()`
specifically needs the gap between *every* pair of consecutive order
statistics, which has no correct formulation without a full sort. The 2D ROM
has no equivalent consumer (`parametric_tails`'s log-normal tail fit sums
over all M members regardless of order — verified by reading the actual
implementation, not assumed), so nth_element's full O(M) → 3×O(M) saving
applies there cleanly. Implementing the checklist's literal "replace the
per-node full sort with three nth_element selections" for 1D as well would
have silently broken H10's shipped, tested modality flag — left as the naive
path's `std::sort`, unaffected by `rom_quantile_naive` either way.

## 3. Side finding: `test_2d_spectral_rom.cpp` was dormant

49 tests (`SpectralROM`, `UncertaintyEnsemble`, `SpectralROMSpatial`,
`DeviationForm2D`, `FiedlerDiagnostic` suites), present on disk since an
early mechanical-port pass, never registered in
`tests/unit/engine/CMakeLists.txt` — the exact same class of gap as PR-10's
own coverage harness before P4, `test_spectral_rom1d.cpp` before H1, and
(found the same session as this PR, on a different branch) PR11's
final-boundary-stall tests. Registered it, since it's the natural home for
this PR's 2D equivalence tests; **all 49 pre-existing tests passed
immediately** against the current codebase with zero fixes needed — a strong
independent correctness signal for the GEMM path (it now runs by default,
so passing `DeviationForm2D.*`/`SpectralROMSpatial.*`/coupling-spread tests
means GEMM reproduces the ROM's already-validated spread/median/coupling
invariants, not just the new equivalence tests written for this PR
specifically).

## 4. Benchmark (measured, not a permanent target)

**2D**: `test_corr_len_profile.cpp`'s own reference scale (N=70 structured
mesh → 9,800 triangles — that file's own documented reason for 9.8k over the
checklist's literal "≥20k": the Lanczos eigensolve at 20k/k=20 takes minutes
on this machine, and the O(quantile) cost this PR targets scales linearly in
N regardless of which N it's measured at).

**1D**: the checklist names StormCity (8k nodes) explicitly. **StormCity is
not available anywhere in this repository** — checked for a committed `.inp`,
a fixture, and a generator script; none exist (it was evidently an
external/local file used only in an earlier session, per `.memory`
references to "StormCity real-network benchmark," never committed).
**Corrected to Bellinge** (real 1020-node sewer network, already the
established production baseline for PR-10/H5/H11's own MC coverage
validation — `tests/regression/test_rom_coverage_bellinge.cpp` — rather than
a synthetic stand-in; an initial pass of this benchmark used a synthetic
8,000-node chain before this correction, which is why the node count below
differs from the checklist's literal "8k"). Driven through the real engine
exactly as `test_rom_coverage_bellinge.cpp` does (same `[UNCERTAINTY] 1D
MANNINGS_N 0.20` injection, same `REPORT_START_TIME`/rainfall-path fixes),
stepped 1 simulated hour to reach a real, non-trivial ensemble state, then
`computeQuantiles()` timed directly off the engine's own live `rom1d()`.

| Case | N | M | k | naive computeQuantiles() | GEMM computeQuantiles() | Speedup |
|---|---|---|---|---|---|---|
| 1D (Bellinge, real network) | 1,011 active nodes | 50 | 20 | 4.67–4.70 ms | 0.443–0.454 ms | **10.3–10.6×** |
| 2D (CL-2a reference mesh) | 9,800 triangles | 50 | 20 | 109.4–111.8 ms | 15.5–15.7 ms | **7.0–7.1×** |

Each row is two independent runs of the same benchmark binary (not a single
sample) — both cases reproduced within ~3% run-to-run. Both comfortably
clear the checklist's own ≥3× gate on the quantile phase, with no tuning:
these are the numbers `RomQuantileGemm.hpp`'s first working version produced.

## 5. Reproduction

    # Correctness: naive-vs-GEMM equivalence, mode-masking edge cases,
    # invert-clamp interaction, run-to-run determinism (1D):
    build/<dir>/bin/Debug/test_engine_spectral_rom1d --gtest_filter='QuantileGemm.*'

    # Same, 2D (also exercises the newly-registered dormant suite, 49 tests):
    ctest --test-dir build/<dir> -R test_engine_2d_spectral_rom

    # H10's sortedMemberValues() full-sort contract, explicitly guarded under GEMM:
    build/<dir>/bin/Debug/test_engine_spectral_rom1d \
        --gtest_filter='QuantileGemm.SortedMemberValuesStaysFullySortedUnderGemm'

The benchmark itself is not a checked-in target (compiled standalone per
this project's calibration-sweep pattern); the numbers in §4 are the full
record of what was measured.

---

# True per-family coefficient planes (PR H7)

## 1. Problem

Two v1 approximations from the soft-rainfall (SR) arc shared one root cause:
the ROM carried exactly **one** `(spread, family)` soft-forcing channel, so a
source whose cells or gages did not all share a distribution family had to be
squeezed into a single per-member coefficient column.

- **SR-4b (MIXED grids)** used the NORMAL coefficient `z_i` for *every* cell
  and pre-scaled UNIFORM cells' spread by `max|z| / max|2u−1| ≈ 3.0` so the
  band width came out comparable. That matches the *range* but distorts the
  *marginal shape*: a UNIFORM cell's ensemble came out z-shaped (dense near
  the median, sparse in the tails) rather than evenly spaced.
- **SR-1b (multi-gage)** did not even range-match: mixed families across gages
  fell back to the first gage's family with a one-shot warning.

## 2. Fix

`SpectralROM1D::setSoftForcing()` and 2D `SpectralROM::setSoftForcing()` gained
two trailing, defaulted parameters — a second spread plane and its own family:

    setSoftForcing(loc, spread, family, soft_field,
                   spread_b /* = nullptr */, family_b /* = UNIFORM */)

`advance()` projects **both** planes (two `k·n` projections instead of one) and
each member's modal forcing becomes

    g_ij  +=  c_i · (Pᵀ·spread)_j  +  c_i^B · (Pᵀ·spread_b)_j

so each source plane keeps its own family's marginal. The caller splits a MIXED
source by family: family A's cells/gages carry their spread in `spread` with
zeros elsewhere, family B's in `spread_b` with zeros elsewhere; the two planes
are disjoint and sum to the original spread field. The SR-4b `×3.0` pre-scale
and the SR-1b first-family fallback both become unnecessary — there is no
longer a single column to squeeze onto.

**Coherence is preserved by construction, not by convention.** Both `c_i` and
`c_i^B` are drawn from the *same* `u_i = shuffledStrata(M, sample_seed + 4)`
stream — `c_i = probit(u_i)` for NORMAL/LOGNORMAL, `c_i = 2u_i − 1` for
UNIFORM. One rank per member therefore holds across families (the
`COHERENCE FULL` contract), and because the strata midpoints `(k+0.5)/M` are
symmetric about `0.5`, both columns are zero-mean and the nominal member is
nominal in *both* channels simultaneously — the deviation-form invariant
survives the second family without a special case.

**Bit-identity.** A null second plane leaves every expression adding an exact
`0.0`: the projection loop is skipped, the activation metric gains `|0|·0`, and
the per-member forcing gains `c^B·0`. Single-family callers are unchanged.

## 3. Scope guard — CORR_LEN × MIXED is refused, not approximated

The correlated-coherence paths (CL-1b/CL-1c materialized field, CL-2c reduced
basis) fold **one** family's `c_i` into the per-member projection `W_i[t]` /
`a_im` by construction; there is nowhere for a second family's coefficient to
enter without redefining that field. Rather than silently mixing families,
`setSoftForcing()` **refuses** the second plane when a spatial field is active,
leaves the ROM in a valid single-family CORR_LEN state, and reports why via the
new `softPlanesError()` accessor (empty on success). `setSoftForcingReduced()`
is single-family for the same reason and clears any previously armed second
plane. CORR_LEN × MIXED is additive future work and the error string says so.

## 4. Findings (measured)

**Marginal shapes are exact per plane, and materially different from each
other.** With the two planes aligned to orthogonal eigenmodes (plane A ∝ mode
0, plane B ∝ mode 1), each mode is fed by exactly one plane, so the marginal
claim is exact rather than approximate. Fitting each mode's member values to a
single amplitude:

| Mode | Fed by | Misfit to its own family | Misfit to the other family |
|---|---|---|---|
| 0 | NORMAL plane | < 1e-12 | **0.215** |
| 1 | UNIFORM plane | < 1e-12 | **0.460** |

Misfit is relative to the fitted amplitude. The cross-family figures are 20–46×
the 1e-2 assertion bar — the shape difference the pre-H7 hack was papering over
is large, not marginal.

**What a per-cell test cannot show, and why.** The spec phrased this as
"UNIFORM cells' member values uniform, not z-shaped". That cannot be asserted
on a reconstructed per-*cell* value: the ROM's eigenmodes are **global**, so
every mode sees both planes and every cell's reconstruction is a linear
combination of both coefficients. That mixing is a property of the spectral ROM
itself, not something per-family planes can or should remove. The claim that is
exactly true — and is what H7 actually fixes — is per *source plane*. The tests
assert it there, and separately verify the full two-plane decomposition against
an independent closed-form per-member reference on a realistic checkerboard
MIXED grid.

**Adding a second family plane is NOT monotone in band width — expected, not a
defect.** On a two-gage fixture where the gages cover complementary halves of
the network, the per-family band came out *narrower* than the first-family
fallback's (0.415 vs 1.000 at the widest node with equal spreads; a 58%
difference with unequal spreads). Cause: comonotone coherence gives every
member the same rank in both families, and two gages on complementary parts of
the network project onto the shared zero-mean eigenmodes with **opposing
signs**, so their contributions partially cancel. With *equal* spreads the two
projections are exactly `R_B = −R_A` on every mode and the first-family
fallback — one column over the summed spread — collapses to a near-zero band
entirely. This is a concrete demonstration of how badly the removed fallback
could misreport a multi-gage source, and the reason the regression test uses
unequal spreads (an equal-spread fixture would prove less than it appears).

## 5. Port status — ROM half live; callers ported in PR SP1 (gages) and PR SP2 (grid)

> **Update 2026-10-03.** Both callers described below are now ported against the
> two-plane API: gage-level soft rainfall in SP1 and the 2D grid `/spread` in
> SP2 (`SurfaceRouter2D::updateGridSoftSpread`). Neither carries the `×3.0`
> pre-scale or the first-family fallback. The text that follows is the original
> H7-time record of what was missing and is kept as history. Still not ported:
> `COHERENCE CORR_LEN` for gages and grids (SP3) and the `RUNOFF` / `INFLOWS`
> grid targets (SR-2d, no runtime on this line).


The `port/v2-on-marcher` line deliberately ported the ROM-side soft-forcing API
but **not** the SR-1a/SR-1b gage consumption or the SR-4b/CL-1c grid `/spread`
→ ROM path (see `.memory/current.md`, the 2026-08-03 SR-2c entry: only the
deterministic `/location` plane is read; `SimulationContext::soft_rain` does not
exist here). The two call sites the checklist asks to *delete* —
`SurfaceRouter2D::updateRainfall()`'s `sp *= 3.0` and `SWMMEngine`'s
"uses the first family" warning — therefore live only on the frozen pre-port
`origin/feature/uncertainty-sidecar` branch and are not present to remove.

**Consequence for whoever ports SR-1b/SR-4b onto this line**: port them against
the two-plane API, not the v1 hacks. Concretely, `updateRainfall()` should fill
two spread buffers keyed on `GridFileReader::family_code_now()` and pass both to
`setSoftForcing()`, instead of writing one buffer with a `×3.0` correction; and
the gage path should group gages by family into the same two planes instead of
warning and collapsing onto the first. Grids with more than two distinct
families in one file still need a third plane — the API extends the same way,
and nothing about the current shape blocks it.

### 5a. SP2 findings (2026-10-03)

- **The SR-2c re-port had silently broken gridded rainfall past its first
  timestamp.** `GridFileReader::time_next()` returns the *current* plane's own
  time when there is no next plane. The pre-port advance loop guarded on
  `spread_next() != nullptr`; the SR-2c re-port dropped that guard. Once
  simulation time passed the last plane's timestamp the loop ran off the end,
  the reader was marked exhausted, and the grid stopped forcing for the rest of
  the run, with the gages taking over silently. A single-plane grid worked only
  at t = 0. Every existing test ran at t = 0 and only checked that the grid was
  "active", so none noticed. Fixed by restoring the guard (the last plane is
  held); regression test `LastPlaneIsHeldAfterItsTimestamp`, which fails without
  the fix.
- **A 2D grid without `FORCE_LOCATION` was never opened**, on either branch,
  even though the USER_GUIDE said it would supply spread only. It is now opened
  whenever it has a `TWO_D` target; `FORCE_LOCATION` only decides whether
  `/location` overrides the gages.
- **CV spread without a `/location` plane** was zero before (it multiplied by the
  absent plane). It now uses the rainfall actually in force that step.
- **Not validated against Monte Carlo.** SP2 is checked structurally: exact plane
  mapping and conversion at router level, and that the planes reach a live 2D ROM
  (zero-perturbation baseline band is exactly 0; a spread grid makes it nonzero).
  There is no brute-force MC comparison through the HDF5 grid file itself; the
  ROM path it feeds (`CORR_LEN` soft forcing, both branches) was validated
  against a correlated marcher MC in W4 (2026-10-07), so what remains
  structurally-only tested is the file-to-field plumbing, not the band.

## 6. Reproduction

    # All 8 H7 tests (6 in 1D, 2 in 2D), plus the 14 pre-existing SR-3a/CL-1b
    # soft-forcing tests in the same newly-registered binary:
    ctest --test-dir build/<dir> -R test_engine_soft_forcing_rom

    # Marginal-shape and coherence contracts only:
    build/<dir>/bin/Debug/test_engine_soft_forcing_rom \
        --gtest_filter='SoftForcingFamilyPlanes*'

---

# H17 — Structure edges and the flow-response Manning source (2026-10-12)

## 1. What was measured first

EPA Example 8 (one pump, one orifice, four weirs, a storage well, a fixed-stage
outfall; `tests/regression/data/Example8.inp`) had no 1D Manning gate. The new
`regression_rom_coverage_structures` runs 21 copies of the deck with every
conduit's n scaled by the LHS strata midpoints of ±20 % against one ROM run with
`1D MANNINGS_N 0.20`, scored at every junction and the storage node over 12 h.

| ROM | member coverage | width median |
|---|---|---|
| conduit-only graph, depth × elasticity source (pre-H17) | 0.761 | 1.636 |
| + structure edges | 0.773 | 1.966 |
| + flow-response source (shipped) | 0.757 | 1.046 |

Structure edges alone barely moved it, so the missing edges were not the main
defect. A time trace of the force main settled it. While the pump runs, the
pre-H17 band at JI11 was 7 ft wide against 0.2 ft in the Monte Carlo. The ROM
scaled the node's 16 ft of depth by the pressurised elasticity 2.0, but that
depth is backwater from the fixed-stage outfall, not friction.

## 2. The fix: the source is the steady flow response

Manning flow scales as 1/n. Linearizing each conduit about the deterministic
state, with conveyance at its upstream end, and requiring each node's outflow
to be unchanged gives a head shift per unit d(ln n) that depends only on what
lies downstream. It is solved in one sweep from the outfalls
(`src/engine/uncertainty/RomFlowResponse.hpp`). A full pipe adds twice its
friction loss to the downstream response. A steep free-surface reach gives
0.6 × depth. A mild reach carries part of the downstream response. A free
weir, orifice, outlet or pump depends only on its upstream head, so it adds
conductance and no source. A node with no outflow gets 0. This field replaces
depth × α (H5/H5b) as the Manning-sensitivity reference. The relaxation
operator is unchanged.

Pumps, orifices, weirs and outlets are also edges of the ROM graph (their
|dQ/dH| is exported in the H snapshot). The directional operator always gates
a pump one-way.

Two solver attempts were measured and rejected:

- **Global node-continuity solve.** It is singular whenever a wetted cluster
  has not reached an outfall (a filling front), and ill-conditioned at
  millimetre flows. A dense version silently fell back to the old source on
  those steps.
- **BiCGSTAB on the same system.** It returned 1e5–1e10 garbage on the same
  steps.

The upstream sweep is exact on dendritic networks, O(links), and cannot go
singular.

## 3. Every 1D gate, same build, source off → on

| Cell | before | after |
|---|---|---|
| P8 free-surface chain | 0.993 / 1.210 | 0.885 / 0.982 |
| Surcharged chain, `EXPLICIT` | 0.736 / 0.782 ranking only | **0.936 / 1.173 calibrated** |
| Surcharged chain, `SEMI_IMPLICIT` | 0.845 / 1.317 | 0.916 / 1.956 conservative |
| H11 front, phase | 0.831 / 1.308 | 0.831 / 1.306 |
| SR-5, CL-1e (rain) | unchanged | unchanged |

The `EXPLICIT` limitation documented since H5 is closed. The source reaches
the node drowned by the chokepoint through the full pipes below it, which a
depth-keyed rule could not. `SEMI_IMPLICIT` turns conservative because the
pool is still filling at the window: at t = 1 h, 14.1 cfs enters the reach and
11.4 cfs leaves. The ROM relaxes to the steady response faster than the
network fills, so J4 and J5 are 2.7× and 3.9× wide while J1 and J2 sit at
0.99 and 0.90. Both surcharged cells now assert the 0.80 floor.

## 4. Example 8 per node, and what remains

The widths are right node by node:

| Node | width before | width after |
|---|---|---|
| JI4 | 15.9 | 1.03 |
| JI5 | 13.6 | 0.63 |
| Well | 4.6 | 0.99 |
| JI8 | 7.6 | 1.29 |
| J7 | 4.7 | 1.73 |
| JI11 (force main) while pumping | 7 ft band | 0.26 ft vs MC 0.21 ft |

Coverage stays under the floor (0.757), so the cell is **ranking only**. The
misses are centimetre-scale spreads at nodes with no steady flow:

- **The force main after the pump stops** (JI10–JI13, coverage 0.13–0.20).
  Members hold 1–4 cm of different stored volume, set by when their pump
  stopped.
- **The channel below the weirs** (J5, J8, J9, J11, coverage 0.66–0.68).
  Members differ by how much spilled.

A steady head response cannot carry a volume memory. That gap is the
storage-and-timing family H11 addressed for fronts, not a Manning-source
defect. The gate asserts the run and a width median inside [0.3, 3.0], and
prints the C1 verdict.

## 5. Reproduction

```
build/darwin-v3/tests/regression/test_rom_coverage_structures      # ~3 min Debug
H17_FLOW_SOURCE=0 ...                                              # pre-H17 source
H17_NODE=JI11,JI5 H17_WINDOW_H=6 ...                               # time traces
```

## 6. Per-band validity flags (step 4)

`<rpt>.uncertainty.csv` gains a `flag` column (`steady`, `transient`,
`no_flow`; `SWMMEngine::refreshRom1dBandFlags()`). The flags were scored
against the same Monte Carlo runs, per sample:

| Cell | steady | transient | no_flow |
|---|---|---|---|
| P8 free-surface chain | 0.885 / 0.982 (all samples) | — | — |
| Surcharged, `EXPLICIT` | 0.937 / 1.173 | 0.912 / n.a. (19 samples) | — |
| Surcharged, `SEMI_IMPLICIT` | 0.894 / 1.018 | 1.000 / 2.696 | — |
| Example 8 | 0.905 / 1.332 | 0.315 / 0.000 | 0.689 / 0.212 |

(member coverage / width median.) `steady` is calibrated on every cell,
including the two that are not as a whole. The semi-implicit over-width sits
entirely in `transient` (the filling pool). Example 8's misses sit in
`transient` (the draining force main) and `no_flow` (the weir-fed channel).

A `supercritical` flag was tried and dropped. With the H14 directional
operator, supercritical samples are calibrated on every fixture (0.88–0.91),
so it would have warned about bands that are right.

The 10 % imbalance threshold is a display rule and was set before scoring;
it was not adjusted to these numbers. Example 8 now asserts that its
`steady`-flagged samples meet the C1/C2 calibrated rule.

## 7. Bellinge re-baseline

Two changes land together: the harness indexing fix (it read quantiles by full
node index, but they are stored by active node; Bellinge lists storage after
its outfalls, so those nodes took another node's band) and the new source.
Same build, full 24 h, source off → on, band / local depth:

| | p50 | p90 | p99 | max |
|---|---|---|---|---|
| all samples | 0.262 → 0.318 | 1.99 → 4.20 | 4.87 → 15.07 | 24.7 → 45.7 |
| `steady` | 0.235 → 0.266 | 1.98 → 4.06 | 4.87 → 14.77 | 24.7 → 41.6 |
| `transient` | 0.418 → 0.603 | 2.09 → 4.88 | 4.95 → 15.86 | 22.8 → 43.8 |
| `no_flow` | 0.110 → 0.096 | 0.78 → 1.15 | 2.91 → 45.0 | 11.6 → 45.7 |

The median barely moves; the upper tail roughly doubles, including in
`steady` samples. The regression lock passes either way.

## 8. First Bellinge Monte Carlo (2026-10-12)

21 full 24 h runs with every conduit's n scaled by the ±20 % strata
midpoints, against one ROM run per source, 1,011 nodes × 1,440 report
times. The members run as parallel processes through the harness's
`OneMemberDump` mode, then `tests/regression/score_h17_mc.py` scores
them with the gates' own definitions. The scorer reproduces the Example 8
gate exactly (0.757 / 1.046).

| Samples | old source | flow source |
|---|---|---|
| all | 0.772 / 2.459 | 0.730 / 1.710 |
| `steady` flag | 0.763 / 2.545 | 0.714 / 1.575 |
| `transient` flag | 0.827 / 2.220 | 0.829 / 2.938 |
| `no_flow` flag | 0.756 / 1.492 | 0.725 / 1.236 |
| MC spread 1 mm – 1 cm | 0.791 / 6.099 | 0.737 / 1.922 |
| MC spread 1 – 5 cm | 0.798 / 2.376 | 0.743 / 2.236 |
| MC spread 5 – 30 cm | 0.720 / 1.030 | 0.692 / 1.123 |
| MC spread ≥ 30 cm | 0.571 / 0.523 | **0.703 / 0.971** |

(member coverage / width median.) Bellinge is **ranking only** under both
sources. The old source reached its higher pooled coverage by being six times
too wide on millimetre spreads, and it was half-width on the largest,
decision-relevant spreads. The flow source is close to the right width in
every spread class and is the better source where spread matters. (An
earlier version of this section read coverage near 0.70 at a width near 1 as
mis-centring. §10 shows that was wrong: misses were symmetric and the centre
exact; the bands were the right width on average but four times too narrow on
one class of nodes and wide on others.)

Two corrections follow:

- **The `steady` flag was calibrated on every test fixture but not on
  Bellinge** (0.714 / 1.575) under the flow source alone. With the quasi-static
  correction (§10) it is calibrated there too (0.855 / 1.009). The Example 8 assertion stays, but the flag
  means "the regime where the band was calibrated on the fixtures", not a
  guarantee on any network.
- The widest Bellinge nodes are the G75S… series, at 15–19× wide and ~0.97
  coverage. They are conservative, not wrong in direction.

Reproduction (about 70 min on 8 cores, Debug):

```
H17_ONE_INP=<prepared deck> H17_ONE_MULT=<m> H17_ONE_ROM=0|1 H17_FLOW_SOURCE=0|1 \
H17_ONE_OUT=<dir>/<tag>.txt H17_WINDOW_H=24 \
  test_rom_coverage_structures --gtest_filter='*OneMember*'
python3 tests/regression/score_h17_mc.py <dir> rom_off rom_on
```

The prepared deck is the Bellinge `.inp` with REPORT_START_TIME 00:00:00 and an
absolute rainfall path, the same edits the Bellinge lock test makes.

## 9. Settings audit: the three remaining 2D numerics settings

Measured on the 2D production gate (member coverage / width / wall time):

| Variant | Coverage | Width | Time |
|---|---|---|---|
| shipped defaults | 0.864 | 1.360 | 406 s |
| `MODE_DROP_THRESHOLD 0` (never drop) | 0.864 | 1.360 | 411 s |
| `PARAMETRIC_TAILS YES` | 0.875 | 1.465 | 411 s |

- **`MODE_DROP_THRESHOLD`**: no effect. Fixed at its old default.
- **`PARAMETRIC_TAILS`**: +0.011 coverage for +0.105 width on an already
  over-wide band. Removed.
- **`WET_RESEED_*`**: on the shipped path the eigenbasis is fixed geometry, so a
  wet-area re-seed rebuilt nothing and reset every member to zero.
  `SurfaceROMRouterOperator.WettedAreaGrowthDoesNotEraseTheBand` measures it.
  With the trigger armed and the fix removed, a cell's band fell from 1.07e-3
  to 4.6e-4. Re-seeding now happens only on the internal legacy path. Removed.

All three keys refuse any value but their old default.

## 10. Why Bellinge's bands were wrong, and the quasi-static correction

Breaking §8's Bellinge result down by how fast the deterministic head moves:

| Head change per minute / MC width | Samples | Coverage | Width |
|---|---|---|---|
| < 0.01 (steady) | 68,894 | 0.420 | 0.27 |
| 0.01 – 0.1 | 77,439 | 0.783 | 1.62 |
| 0.1 – 1 | 90,917 | 0.823 | 1.60 |

The failures were the steady samples: shallow laterals (G71F…, G72F…, about
1 ft deep) through the 5–24 h recession. There the members' order follows
roughness cleanly, and the Monte Carlo spread is Manning's free-surface
response. Misses were symmetric (15 % below, 15 % above) and the band centre
sat on the Monte Carlo median.

Three hypotheses were tested:

- **Slow modes not yet settled.** Relaxation ×10 changed nothing (0.420 /
  0.272), so the ROM was already at its fixed point.
- **Too few modes.** 40 modes moved the class only to 0.457 / 0.340. An
  80-mode run was too slow to finish in Debug.
- **Wrong scale.** Carrying Manning as a fraction of local depth (the 2D
  two-channel design) made Bellinge much worse (0.390 / 0.209) and Example 8
  worse (0.661). Carrying it as a fraction of the response itself also made
  Example 8 worse (0.646). In 1D the response is not proportional to depth:
  a shallow node in backwater rises with the water below it. Both variants
  were removed. With Manning absolute, splitting it from rainfall changes
  nothing, because the two superpose linearly.

A φ trace at the failing nodes settled it. φ was right there (0.5–1.1 ft,
0.4–0.8 of depth), but the 20-mode reconstruction PPᵀφ kept 0.02–0.1 ft of it,
sometimes with the wrong sign. Truncation was dropping a localized response.

**Fix: the quasi-static (mode-acceleration) correction.** The part of φ the
kept modes cannot represent, r = φ − PPᵀφ, belongs to faster modes than any
kept one. It is added at its steady value (mmᵢ − 1)·r. The kept modes carry the
dynamics. The steady band is exactly (mmᵢ − 1)·φ at every node, which
`QuasiStaticResidual.SteadyBandIsTheFullResponseAtEveryNode` pins to 1e-9
with 6 modes on a 20-node chain.

| Gate (coverage / width) | Before | After |
|---|---|---|
| free-surface chain | 0.885 / 0.982 | 0.894 / 0.997 |
| surcharged, `EXPLICIT` | 0.936 / 1.173 | 0.949 / 1.172 |
| surcharged, `SEMI_IMPLICIT` | 0.916 / 1.956 | 0.906 / 1.672 |
| front passage (H11) | 0.831 / 1.306 | 0.971 / 1.416 |
| Example 8 | 0.757 / 1.046 | **0.810 / 1.102** |
| Bellinge | 0.730 / 1.710 | **0.840 / 1.008** |
| Bellinge `steady` | 0.714 / 1.575 | 0.855 / 1.009 |
| Bellinge `transient` | 0.829 / 2.938 | 0.769 / 1.000 |
| Bellinge `no_flow` | 0.725 / 1.236 | 0.530 / 0.330 |
| Bellinge steady class above | 0.420 / 0.272 | 0.841 / 0.992 |

Every 1D gate is now calibrated except the semi-implicit pool, which is
conservative. Bellinge's misses are symmetric at 8.5 % each side. The `no_flow`
class got narrower: no flow means φ = 0, so it has no residual to add, and its
spread is stored volume (§4).

The Bellinge regression lock with the correction (band / local depth):
p50 0.198, p90 0.835, p99 3.62, max 24.6. For comparison, the flow source
alone gave p90 4.20 and the original source p90 1.99. The tail that §7
flagged as unvalidated was the wide half of the mixture §10 resolved.

## 11. Smaller items re-measured (2026-10-12)

- **H2 (pump-state basis cache).** With pumps in the graph, each of Example 8's
  four pump switches triggers a basis rebuild in the same step (the first one
  cold). They are 4 of 97 rebuilds. A cache would save at most those four,
  under a cost the 60 s rebuild interval already bounds. Not worth building.
- **SP4 early-time continuity residual.** Legacy EPA SWMM 5 gives the
  identical error to three decimals in every case: −9.358 % at 30 s steps over
  5 min, −11.277 % at 1 s steps, −0.779 % over 1 h. It is SWMM 5's own
  accounting for a pipe filling from dry, and it shrinks with run length.
  Inherited by design, not a defect.
- **H9 (dry-bed wetting).** Upstream's `FLOW_ROUTING FV` solver reproduces the
  Ritter dry-bed dam break within 3 % relative L1 (`FvAnalytic.RitterDryBed
  DamBreak`), and Stoker as well. A wetting criterion for the Picard
  dynamic-wave scheme is not needed; dry-bed fronts belong to FV routing.
- **New gap: the sidecar under `FLOW_ROUTING FV` is unvalidated.** A quick
  Example 8 run produces bands up to 32 ft. The basis weights come from the
  dynamic-wave solver's snapshot, which FV does not fill. Needs its own work.

## 12. Up to date with swmm6_rel 5c363861, and the sidecar under FLOW_ROUTING FV (2026-10-13)

**Merge.** 38 upstream commits (engine checkpoint/restart, result continuation,
source coupling) were merged into the stack. Seven conflicts were resolved by
keeping both sides. The one with behaviour is the runoff kernel: upstream added
a source-owner map as a trailing argument. The SR-6 elasticity probe now passes
the same map, so it still runs the kernel with exactly the deterministic
arguments. A suite run showed three extra failures, all from rebuilding the
engine library mid-run (an ABI mismatch, not a merge defect); on a consistent
build they pass.

**Solver-agnostic regime inputs.** The ROM's edge conductances, Froude
numbers and surcharge flags came only from the dynamic-wave snapshot. Under FV
the basis never updated, the pump and supercritical one-way operator was lost,
and so was the surcharge trigger. They now come from the published state
(link flow and depth, node head) with the dynamic-wave solver's own Picard
conductance formula whenever the snapshot is absent. Dynamic wave is
unchanged by construction.

**Supercritical relay in the flow response.** The flow-response sweep relayed
a pool's response up through steep reaches. A supercritical conduit's flow is
set upstream, so its downstream head terms now fade over the same Froude
ramp the one-way operator uses. Under dynamic wave this calibrates the
semi-implicit surcharged chain: 0.906 / 1.672 → **0.892 / 1.128**.

**FV head semantics.** FV can publish an upstream node's head below the
node's own invert: it reports the first cell inside a steep pipe. Three uses
now cope with that:

- The flow response takes the conduit's published depth when the node gives
  none.
- K1d does the same.
- The quantile floor is min(invert, deterministic head). Before, members
  were clamped above the deterministic value and the band collapsed (J1 under
  FV: 0.000 → 0.158 ft against MC 0.173).

**1D gates, final build (dynamic wave):**

| Gate | Coverage / width |
|---|---|
| free-surface chain | 0.894 / 0.997 |
| surcharged, `EXPLICIT` | 0.924 / 1.158 |
| surcharged, `SEMI_IMPLICIT` | **0.892 / 1.128** |
| front passage | 0.971 / 1.416 |
| Example 8 | 0.814 / 1.155 |

**Overflowing nodes are pinned.** A junction overflowing at its rim has its
head pinned by the spill, so no roughness change can raise it. The flow
response sets φ = 0 there, and the node relays nothing upstream. Without this,
Bellinge's G70F143, at its rim with zero Monte Carlo spread, read twice the
drop below it: a band of up to 106 ft, which broke the Bellinge lock's ceiling
on band over head. The pin also matters under FV, where chain nodes overflow.

**Pipes near capacity are bounded by the full-pipe response.** On a steep
pipe near the top of its section, conveyance stops growing with depth, and
with the head-gradient terms gated out the free-surface response 1/(K'/K)
becomes unbounded. Such a pipe pressurizes instead. Wherever the free-surface
sensitivity falls below the full-pipe one, the edge takes the full-pipe form
(twice its friction loss plus the downstream response). That fixes Bellinge's
G70F142: 92 ft band against 0.27 ft in the Monte Carlo. No chain or Example 8
edge reaches the bound; their numbers are unchanged.

**Under FV (same fixtures, both MC and ROM routed with FV):**

| Gate | Coverage / width | Reading |
|---|---|---|
| free-surface chain | 0.904 / 1.204 | calibrated |
| surcharged chain | 0.803 / 2.091 | conservative |
| front passage | 0.925 / 7.74 | conservative |
| Example 8 | 0.638 / 1.770 | ranking only (FV's force main, below) |

No FV chain cell is ranking-only, but two are conservative (2× and 8× wide)
and Example 8 is a ranking. FV is therefore **not validated** as a whole, and the
engine writes a report warning whenever the 1D ROM runs under FV.

Two causes were measured:

- **Relaxation speed.** Under FV the surcharged chain's bottleneck carries the
  flow without a pool. K1d, a diffusion-wave scale dominated by deep pools,
  is 19× smaller than under dynamic wave (0.0038 against 0.073 /s). The kept
  modes lag while the quasi-static residual is already at its steady value.
  K1d ×100 brings J2–J4 to their targets.
- **Example 8.** FV's own force-main solution flows backward against the pump.
  Separately, FV leaves pump and orifice dQ/dH at zero.

**Tried and rejected:**

- **Advective relaxation in K1d**, max(D/L², (5/3)v/L). FV surcharged improves
  to 0.769 / 1.18, but Example 8 under dynamic wave drops to 0.784. It is kept
  as an internal hook only.
- **A physical operator** M = PᵀS⁻¹AP from the flow Jacobian and node storage.
  Projecting a non-symmetric operator on the symmetric basis is unstable
  (bands to 1e33), as the 2D similarity transform was. Removed.

**Engine checkpoints.** swmm6_rel's new engine checkpoint/restart does not
carry the sidecar's ensemble state. A resumed run would restart every band
from zero. Following upstream's own pattern (each unsupported feature is
refused with the reason), enabling a checkpoint while the 1D or 2D ROM or the
water-quality bands are active is now refused: "Engine checkpoint does not yet
recover uncertainty-sidecar state". `test_engine_checkpoint` itself passes per
test but runs past 90 minutes in Debug, so the full suite reports it as a
timeout. Without an `[UNCERTAINTY]` section none of the sidecar code runs.

**The real fix is one rate per mode, not one per network:** build the basis
from the generalized problem L v = λ S v, a conductance Laplacian with node
storage. Each mode then carries its physical rate in 1/s, and the projection
stays symmetric and stable. It touches every validated cell, so it is
scheduled as its own piece of work, not slipped in here.

---

# H18 — Per-mode relaxation rates (2026-10-13)

Design: `docs/uncertainty/RELAXATION_RATES.md`. Implemented behind the internal
hook `SWMMEngine::rom1dPerModeRates()` (default off); harness knob
`H18_PER_MODE=1`.

## 1. What changed

One network-wide relaxation scale (K1d × dimensionless eigenvalues) is replaced
by per-mode rates in 1/s. These come from the generalized eigenproblem
L_c v = λ S v:
- **L_c (conductance):** the Picard head conductance with the Froude relay,
  plus the kinematic and flowing-structure terms on the upstream node.
- **S (storage):** storage-curve area plus half the top width of attached
  conduits.

Every projection is S-weighted. Between rebuilds the rates follow the live
operator as generalized Rayleigh quotients. The target (mm − 1)·φ is
untouched.

## 2. Gates, same build, hook off → on (coverage / width)

| Cell | Off | On | Verdict on |
|---|---|---|---|
| DW free-surface chain | 0.894 / 0.997 | 0.894 / 0.997 | calibrated |
| DW surcharged `EXPLICIT` | 0.924 / 1.158 | 0.925 / 1.158 | calibrated |
| DW surcharged `SEMI_IMPLICIT` | 0.892 / 1.128 | 0.918 / **2.355** | **conservative** (§3) |
| DW front passage | 0.971 / 1.416 | 0.961 / 1.181 | calibrated |
| DW Example 8 | 0.814 / 1.154 | 0.824 / 0.984 | calibrated |
| DW Example 8, steady flag | 0.948 / 1.489 | 0.852 / 0.989 | calibrated |
| DW Bellinge (MC, 21 members) | 0.840 / 1.008 | 0.858 / 1.059 | calibrated |
| FV free-surface chain | 0.904 / 1.204 | 0.904 / 1.204 | calibrated |
| FV surcharged chain | 0.803 / 2.091 | **0.862 / 1.180** | **calibrated** |
| FV front passage | 0.925 / 7.74 | 0.925 / 7.83 | conservative (§4) |

Bellinge by flag, off → on:
- steady: 0.855 / 1.009 → 0.867 / 1.048;
- transient: 0.769 / 0.998 → 0.819 / 1.362;
- no-flow: 0.531 / 0.333 → 0.678 / 0.961.

## 3. `SEMI_IMPLICIT`: the earlier pass was a lag

The cell's median of five nodes sits at J3. In both runs J1 and J2 read 0.99
and J4 and J5 read about 3× (the known transient pool filling). J3 moves from
1.128 to 2.893.

Under the **old** operator with K1d × 10 (`H17_K1D_SCALE=10`, so the modes
settle within the window), J3 reads **2.972** and the cell **0.918 / 2.330**,
the same as H18. The old 1.128 came from a rate too slow to reach the target
in the window: mode 0 had τ = 1,054 s and was 82 % saturated at the window
start.

The target itself is wide at J3: 0.27 ft against an MC width of 0.108 ft,
which equals J1's. This is the H14-era limitation. A Froude number taken at mid
conduit cannot see a steep reach discharging into a drowned pool, so the relay
stays on and φ at J3 carries part of the pool. A fix belongs to the target
(flow response), not to the rates.

## 4. FV front passage is the phase channel

The FV front reads 7.7–7.9 with H18 off, H18 on, H18 on with H14 off, and the
old K1d × 10. Rates do not move it. The amplitude-only baseline under FV reads
0.147, narrow. The over-width comes from the phase coordinate (H11): the travel
time from conduit velocities under FV. This is a separate item.

## 5. Risks measured (RELAXATION_RATES.md §8)

- **Lanczos convergence (Example 8, end of run).** Ritz residual 2.5e-10,
  largest normalized off-diagonal of PᵀL_cP 4.3e-11. Rates 1.7e-8–6.9e-3 /s,
  storage 12.6–1,366 ft².
- **Mode waste.** 10 of 20 kept modes carry more than 90 % of their storage
  mass on nodes with zero response at the end of Example 8's 12 h window. The
  cell is still calibrated.
- **Rebuild churn.** Example 8: 97 attempts, 7 cold, identical with the hook
  on and off.
- **Inertial cap depends on dt.** DW chain with the routing step halved
  (30 → 15 s): every verdict is unchanged (`EXPLICIT` 1.158 → 1.151, front
  1.181 → 1.220, `SEMI_IMPLICIT` 2.355 → 2.298).

## 6. Bellinge lock and runtime (Debug, run alone, same build)

| | Off | On |
|---|---|---|
| wall time | 1,186 s | 1,421 s (+19.8 %, inside the +25 % gate) |
| lock | passes | passes |
| band / head max | 0.279 | 0.440 |
| band / depth p50 / p90 / max | 0.197 / 0.74 / 24.6 | 0.207 / 1.21 / 50.3 |
| band / depth p90, no-flow flag | 0.55 | 5.36 |

The tails are wider with H18. Against the 21-member Monte Carlo the network
is still calibrated (§2), and the no-flow samples moved from 0.531 / 0.333
toward the MC (0.678 / 0.961).

## 7. Verdict against the plan's acceptance rule

The rule was: no dynamic-wave cell loses calibration, and the FV surcharged
chain becomes calibrated.

- FV surcharged becomes calibrated (0.862 / 1.180).
- Bellinge, Example 8, the front and the free-surface and `EXPLICIT` chains
  stay calibrated. The front, Example 8 and Bellinge coverage improve.
- `SEMI_IMPLICIT` goes from calibrated to conservative (2.355). §3 shows the
  earlier pass was the old rate's lag hiding a wide target at J3: the old
  operator gives the same 2.33 once it is allowed to settle.

By the letter of the rule H18 is **not accepted**. (§9 fixes the
`SEMI_IMPLICIT` target; §10 finds the soft-rain gates fail under H18.) The default stays off
pending the owner's decision. The options are: accept H18 with
`SEMI_IMPLICIT` documented as conservative for the reason in §3, or first fix
the J3 target (a drowned steep reach's relay in the flow response) and re-run
the gate.

## 9. The J3 target fixed: Froude at the conduit inlet (2026-10-13)

A trace of the flow response under `SEMI_IMPLICIT` showed C3 (the steep reach
above the pool) with Froude 0.38–0.55 at mid length but **3.57 at its inlet**.
The pool had backed into the pipe, so the mid-conduit depth read subcritical,
while the flow entering the reach stayed supercritical. The relay stayed on,
and φ at J3 rose from 0.296 ft (equal to J1 and J2, as in the MC) to 1.18 ft.
The relay now uses the larger of the mid-conduit and inlet Froude numbers. When
the flow enters a reach supercritical, or passes critical depth along it, the
head above the reach is set at the inlet.

| Cell | H18 off, before → after | H18 on, before → after |
|---|---|---|
| DW `SEMI_IMPLICIT` | 0.892 / 1.128 → 0.897 / 1.394 | 0.918 / 2.355 → **0.851 / 0.990** |
| DW `EXPLICIT` | 0.924 / 1.158 → 0.922 / 1.158 | 0.925 / 1.158 → 0.829 / 0.990 |
| DW free-surface, front | unchanged | unchanged |
| Example 8 | 0.814 / 1.154 (unchanged) | 0.824 / 0.984 → 0.825 / 0.984 |
| FV surcharged | 0.803 / 2.091 → 0.803 / 2.061 | 0.862 / 1.180 → 0.850 / 1.174 |
| SR-5 / CL-1e soft rain | unchanged (0.881 / 0.976, 0.827 / 0.804) | — (§10) |
| Bellinge (MC) | 0.840 / 1.008 → 0.839 / 1.006 | 0.858 / 1.059 → 0.857 / 1.060 |
| Bellinge lock (H18 off) | passes; band / head max 0.279, band / depth p90 0.73 | — |

With H18 off, `SEMI_IMPLICIT` widens from 1.128 to 1.394. That is the old rate's
lag acting on a now-correct target, and the cell is still calibrated.

## 10. H18 breaks the soft-rain gates: additive forcings need a target

These two gates were missing from the plan's A.4 table:

| Gate | H18 off | H18 on |
|---|---|---|
| SR-5 soft rain (FULL) | 0.881 / 0.976 | **0.735 / 0.953**, ranking only |
| CL-1e soft rain (CORR_LEN) | 0.827 / 0.804 | **0.725 / 0.926**, ranking only |

The failure is J5, next to the outfall: its band is 0.3–2 % of the MC spread at
every report time. The other nodes are at about 0.95.

The cause is design note §3. The kinematic part q·K′/K sits on the upstream
node's diagonal, and its transfer into the downstream node is dropped. For the
Manning channel only timing is lost, because the target φ carries the steady
propagation. Rain has no target, so its steady state is the operator's:
L_c⁻¹·δq. Without the transfer, J5 sees only its own extra inflow and not the
extra flow from every node above it.

**Rejected (measured):** putting the kinematic term on a symmetric edge. Soft
rain becomes calibrated (SR-5 0.879 / 0.949, CL-1e 0.880 / 0.960), but DW
`EXPLICIT` diverges (width 1e216: the reduced operator is unstable), and
`SEMI_IMPLICIT` (1.58) and FV surcharged (2.26) turn conservative.

**The fix is to give additive forcings a target, as Manning has.** The steady
response to a member's extra inflow comes from the flow-response sweep, with the
extra inflow as the source. That carries the downstream transfer exactly and
one-way, and the per-mode rates then set only the speed. This is the 1D
counterpart of the 2D two-channel finding (multiplicative and additive channels
need different treatment). Until it exists, H18 cannot be the default.

### 10b. The target attempt (A.4b, 2026-10-14): measured, not adopted

The patch is saved as `docs/uncertainty/prototypes/h18_additive_target.patch`.

**What it did.** Every additive field f (runoff, both soft-rain planes, spatial
rain, FORCING_VECTOR) became its steady head response ψ, computed by the
flow-response sweep with the accumulated extra throughflow as its source
(`accumulateThroughflow` + a source argument to `solveFlowResponse`). Each mode
relaxed toward κ·Qᵀψ at its own rate, and the part of ψ the kept modes miss
was added as a residual.

**Results, in order:**

| Variant | SR-5 | CL-1e | SR-5 per node at 1 h (J1…J5) |
|---|---|---|---|
| target, instant residual | 0.894 / 2.37 | 0.960 / 1.55 | 0.96, 1.89, 2.49, 2.49, **25.5** |
| + head conductance −Jd (capped by Picard) | 0.970 / 2.38 | 0.975 / 1.54 | same |
| + residual relaxed at diag(L_c)/S | 0.968 / 2.52 | 0.967 / 1.72 | 0.97, 1.90, 2.49, 2.97, 3.39 |

**Why it fails.** The fixture is storage-dominated: about 20,000 ft² of
storage per node, rates about 1.9e-5 /s (a 14 h time constant) against a 1 h
window. Relaxing toward the steady target makes the early forcing at a node
equal to rate × target ≈ (extra inflow accumulated from everything upstream) /
that node's own storage. The upstream extra water arrives at once, whereas in
reality it raises each node in turn. The steady target is right only once
flows have equilibrated; rate forcing through the symmetric operator is right
only before. Only dynamics that include the one-way transfer get both right.

**Second finding.** The soft-rain source is the deterministic head rate dh/dt
times the relative spread. In steady flow under steady rain dh/dt = 0, so no
band forms although rain uncertainty changes the flow. A correct additive
channel needs the extra inflow itself (runoff × relative spread × elasticity)
as its source.

### 10c. Today's rain channel under FV (2026-10-16)

The soft-rain gates had never been run under `FLOW_ROUTING FV`. Their harnesses
now take `H17_ROUTING=FV`, which applies to the MC members and the ROM alike.
Rain channel as shipped (H18 off), coverage / width:

| Gate | Dynamic wave | FV |
|---|---|---|
| SR-5 soft rain (FULL) | 0.881 / 0.976 | **0.900 / 0.968**, calibrated |
| CL-1e soft rain (CORR_LEN) | 0.827 / 0.804 | **0.846 / 0.815**, calibrated |

K1d is about 19× smaller under FV, but these fixtures are storage-dominated:
the rain channel there acts as an accumulator of the level rise, and its decay
rate barely matters. So option 1 (H18 for roughness, today's rain channel) loses
nothing for FV rain on these fixtures. The steady-flow blind spot (§10b) applies
under both solvers.

## 11. Reproduction

```
H18_PER_MODE=1 test_rom_coverage   [--gtest_filter=...]     # chain cells
H18_PER_MODE=1 H17_ROUTING=FV test_rom_coverage             # FV chain
H18_PER_MODE=1 H18_ROUTING_STEP=0:00:15 test_rom_coverage   # dt check
H18_PER_MODE=1 H5B_TRACE=1 test_rom_coverage --gtest_filter=RomCoverageSurcharged.SemiImplicitContinuity
H18_PER_MODE=1 test_rom_coverage_structures --gtest_filter='*Example8*'   # prints [H18-diag]
H18_PER_MODE=1 H17_ONE_INP=... H17_ONE_ROM=1 H17_ONE_OUT=rom_h18.txt test_rom_coverage_structures --gtest_filter='*OneMember*'
python3 tests/regression/score_h17_mc.py <dump dir> rom_cap rom_h18
```

---

# H19 — Full-order additive channel (2026-10-16)

Design: `docs/uncertainty/ADDITIVE_CHANNEL.md`. Every additive uncertainty (soft
rain, the runoff multiplier, registered `FORCING_VECTOR` sources) is carried by
one full-order linearized head-deviation field per source, S dΨ/dt = −JΨ + δq.
J is the flow-response Jacobian, including the one-way transfer. The fields are
advanced by backward Euler and Gauss–Seidel, shared by all members, and driven
by the extra inflow itself (runoff × relative spread × elasticity, or the
lateral inflow), not by dh/dt. The Manning channel is unchanged. Internal
hook: `rom1dAdditiveFullOrder()`; harness knob: `H19_FULL_ORDER=1`.

## Gates (coverage / width)

| Gate | Old modal rain channel | H19 |
|---|---|---|
| SR-5 soft rain, DW | 0.881 / 0.976 | **0.921 / 1.029** |
| SR-5 soft rain, FV | 0.900 / 0.968 | **0.917 / 1.006** |
| CL-1e soft rain CORR_LEN, DW | 0.827 / 0.804 | **0.941 / 1.026** |
| CL-1e soft rain CORR_LEN, FV | 0.846 / 0.815 | **0.938 / 1.018** |
| **SR-7 steady rain on steady flow** (new) | 0.973 / 1.370 (J1 2.5×, J5 0.73×, shrinking) | **0.922 / 0.969** (every node 0.96–0.97) |
| surcharged filling variant (0.5 ft pipes) | 0.262 / 0.011 | 0.990 / 1.455 |
| Manning gates (chains, Example 8) | — | identical with the hook on or off (no additive source) |

- **Per node.** SR-5 under H19 has every node within 2–5 % of the MC at every
  report time, J5 included (1.017). Under H18's modal operator J5 had read
  0.3–2 %.
- **H18 makes no difference here.** The additive channel does not use the modes.
- **Cost.** At most 2 planes for gage soft rain, K_s or M planes for CORR_LEN.
  Gauss–Seidel converges in a few sweeps on these fixtures; the count is exposed
  as `rom1dAdditiveSweeps()`.

**SR-7** is registered as `regression_soft_rain_steady_coverage`: the SR-5
fixture with 50 ft² storage, 0.5 acre / 500 ft subcatchments and 3 ft pipes.
The MC widths are constant from 30 to 60 minutes. The old channel's band at
J1 shrinks from 3.45× to 2.54× over that half hour, because the dh/dt it is
driven by dies away. Its pooled median looks calibrated only because J1 is too
wide and J5 too narrow.

**Not covered:**
- The roughness × rain interaction: members do not scale J by 1/mm.
- `FORCING_MULT` extra parameters do not multiply the full-order planes.
- The surcharged filling variant is conservative at 1.455, probably because
  pressurized head goes as Q², which the linearization treats as linear (not
  investigated).

## Tests

- `test_engine_rom_additive_channel` (6): steady state carries the accumulated
  extra flow; the early response stays local; volume is conserved each step;
  Gauss–Seidel equals a dense solve with a structure edge; a pinned node stays
  at zero and passes nothing on; a zero source stays exactly zero.
- `ROMQuality.RegisteredInflowParamProducesSpreadFullOrder`.
- SR-7 gate.

---

# A.5 — Per-mode rates and the full-order additive channel become the default (2026-10-16)

**What changed.**
- The 1D ROM always uses the conductance/storage basis and per-mode rates (H18)
  and carries every additive uncertainty in the full-order channel (H19).
- Deleted: K1d (`computeK1d()`), its scale knob (`rom1dK1dScale()`), the
  advective hook (`rom1dK1dAdvective()`), the H18/H19 switches, the dh/dt
  forcing buffer and the area-weighted dh/dt soft-rain spread.
- Until the first storage-weighted basis exists (no flow yet), deviations are
  held (rate 0), not relaxed by a network constant.
- The one-way operator (H14) stays. Measured under H18 + H19, turning it off
  drops the DW free-surface chain to 0.738 (ranking only); other cells barely
  move.

**Two defects found while gating, both fixed:**
- **A failed basis rebuild overwrote the live basis** (`402a8996`).
  `updateBasis()` built in place, so a failed rebuild left coefficients sized
  for another basis. The result was out-of-bounds reads, run-to-run
  differences and divergence (one cell read 1e62 wide). It now builds into a
  fresh basis and swaps it in on success. On the storage-weighted basis, only
  round-off zeros count as null modes. Mutation-checked unit test.
- **A fully supercritical network got no basis.** The relay removes every
  head-gradient conductance there, and the assembly gave up when the symmetric
  part summed to zero, although the one-sided kinematic terms were present.
  The band was held at zero for the whole run. The floor is now taken against
  all conductance. Caught by `DirectionalOperatorInstalledOnSupercriticalChain`.

**Build note.** The `darwin-v2` build tree still carried
`OPENSWMM_FAST_MANNING_POW=ON` and `OPENSWMM_FAST_XSECT_LOOKUP=ON` from before
these options defaulted to OFF (`de1c7ea7`). Cached options survive a change of
default. The parity suite (`regression_suite`) failed on it, against identical
sources that pass on a bit-exact tree. All A.5 numbers below come from the
bit-exact engine.

## Gates on the default engine (coverage / width)

| Gate | Result | Verdict |
|---|---|---|
| DW free-surface chain | 0.905 / 0.997 | calibrated |
| DW surcharged `EXPLICIT` / `SEMI_IMPLICIT` | 0.825 / 0.990, 0.843 / 0.990 | calibrated |
| DW front passage | 0.959 / 1.181 | calibrated |
| Example 8 (steady flag) | 0.825 / 0.985 (0.851 / 0.990) | calibrated |
| FV free-surface / surcharged | 0.905 / 1.204, 0.850 / 1.174 | calibrated |
| FV front passage | 0.925 / 7.83 | conservative (H11 timing under FV) |
| SR-5 soft rain, DW / FV | 0.921 / 1.029, 0.917 / 1.006 | calibrated |
| CL-1e CORR_LEN, DW / FV | 0.941 / 1.026, 0.938 / 1.018 | calibrated |
| SR-7 steady rain on steady flow | 0.922 / 0.969 | calibrated |
| **PR-10's original fixture** (100 m, 1 h window) | **0.914 / 1.005** | calibrated |

The last row is the fixture P4 diagnosed in August. Its slowest mode took
10 h under K1d, so a 1 h window read about 0.10 of the MC width. With per-mode
rates the same mode settles in about 27 s. `SpinUpDemonstrationOldGeometry` now
pins τ₀ < 0.1 h.

## Bellinge (21-member MC, 24 h, bit-exact engine)

| | Previous default | A.5 default |
|---|---|---|
| all samples | 0.840 / 1.008 | **0.857 / 1.063**, calibrated |
| material (MC ≥ 0.05 ft) | 0.828 / 1.054 | 0.828 / 1.059 |
| steady flag | 0.855 / 1.009 | 0.865 / 1.050 |
| transient flag | 0.769 / 0.998 | 0.819 / 1.380 |
| no-flow flag | 0.531 / 0.333 | 0.685 / 0.998 |
| lock (`regression_rom_coverage_bellinge`) | passes | passes |
| band / depth p50 / p90 / max | 0.198 / 0.83 / 25.2 | 0.208 / 1.34 / 49.7 |
| wall time, Debug, run alone | 1,680.8 s | 1,680.5 s |

- **The Monte Carlo was regenerated.** The original 21 member dumps were lost
  from a temp directory. They now live in `SWMM6/data/bellinge_mc/`
  (`prepare.py`, `mc_member.sh`, resumable `run_mc.sh`, `run_rom.sh`), outside
  git. The old ROM dumps score identically against the new members, so the
  regeneration reproduces the original.
- **Runtime.** No measurable cost against the previous default on the same
  bit-exact build. The +19.8 % measured for H18 alone (§6) came from the
  fast-kernel tree, and the A.5 default also drops the modal rain forcing and
  the dh/dt work.
- **Tails.** The band tails are wider (no-flow p90 0.55 → 7.6), while the MC
  shows the no-flow class moving from too narrow (0.333) to on width (0.998).

# Coupled 1D–2D flooding on Bellinge (2026-10-21, demonstration run)

First run of the sidecar on a real coupled network with a 2D surface:
Bellinge's 1,020-node sewer with its 155,688-triangle terrain mesh, the
29 June 2012 storm (58 mm, peak 06:23), 00:01–10:00, Manning's n ±20 % on
pipes and surface, rainfall `NORMAL CV 0.20` through `[SOFT_RAINGAGES]` and a
spread-only `[SOFT_RAINFALL_GRID]` (one member rank for both), 50 members,
40 modes, `[2D_ROM] EXCEED_DEPTH 0.20`. Release build, bit-exact kernels.
Data and scripts: `SWMM6/data/bellinge_demo/`,
`scripts/uncertainty/bellinge_flood/`.

> **Retraction (2026-10-24): the Monte Carlo reference below never perturbed
> the rain.** `mc_prepare.py` scaled the fourth `[RAINGAGES]` column, which is
> the snow catch factor; the engine applies it to snowfall only (`Gage.cpp`).
> Rain is scaled by the FILE format's trailing factor (`MM * r`). Every member
> in `mc/` and `mc_v2/` therefore had the main run's 5.560 ha·m of rain, and
> varied roughness only. Every rain-related verdict in this section is void:
> the basin bands "1–3 m against 5–13 mm", the 1D "conservative, 2.8×", the 2D
> street scores and the P(>20 cm) reliability table. The corrected members
> (`mc_v3/`, precipitation 3.73–7.39 ha·m) give, for the run of 2026-10-23
> (`demo_v2`): 1D all manholes 0.959 / 1.43, calibrated; trunk transect
> 0.856 / 1.60. Per storage node the band ranges from 50× too wide (G72R83B,
> a pipe over a raised end) to 3× too narrow (the G71F trunk storages).
> The same day a second defect was found in the 2D Manning channel (fix 7):
> its volume-weighted Gram was paired with a plain projection, scaling every
> 2D Manning band by one over the cell area. Both are re-measured in §9.

## 1. Five defects found by three reference-free checks

Before any Monte Carlo, three invariants that need no reference were checked
on a 07:00 window: a zero-perturbation run must give a zero band and a median
equal to the deterministic run, and the member coefficients must stay bounded
through the event. All three failed, for five distinct reasons. None is in a
regime any registered test enters.

| # | Defect | Symptom on Bellinge | Fix | Test |
|---|---|---|---|---|
| 1 | Per-member 1D head entered the 2D exchange in internal feet against a metre mesh | 63,000 dry cells carried 0.2–42 m of water in every member before any rain | member head = deterministic head (m) + 1D deviation × 0.3048 (`SpectralROM1D::detHead()`) | `CouplingFluxConvertsRom1dFeetToMetres` |
| 2 | Members compared with the exchange the 2D integrator booked, not the same law at the deterministic state (W2 design) | zero-perturbation median drifted 4 cm (p99) before rain, 0.46 m at 06:00, 1.9 m at 06:30 | reference = same orifice law at the deterministic state; `q_det` argument removed | `CouplingFluxZeroPerturbationAddsNothing` (fails on the absolute-head mutation) |
| 3 | Explicit per-step orifice exchange | overshoot where manhole and street levels nearly agree (dQ/dh ∝ 1/√\|dh\|) | implicit exchange over the step (bisection, fixed 60 iterations) | `CouplingFluxImplicitExchangeNeverOvershoots` (fails on the explicit mutation) |
| 4 | 2D quantiles on the router's own elapsed-time cadence | saved median lagged the saved depth by up to one report step (≈2 m on the rising limb) | `SurfaceRouter2D::refreshROMQuantiles()` at `output_due` | — (engine wiring) |
| 5 | Additive channel's reduced operator | coefficients grew 15 → 7.6 × 10¹¹ between 05:45 and 07:00 | energy-stable form, §2 | `EnergyStableFormHasNoGrowingModeOnConvergingFlow` |
| 7 | Manning channel's relative Galerkin operator (H16 form, plain inner product) at the onset of rain: 12 of 155,688 cells wet, depth weights spanning 17 orders of magnitude | eigenvalue −0.43/s at 05:25; coefficients 17 → 5 × 10¹⁴ in 20 min | same energy-stable treatment: volume inner product + skew advection on the H·P trial space; depth-weight mean over wet cells floored at the dry depth; Gram inverted by Cholesky instead of Gauss-Jordan. Identical on uniform meshes, so the plane and channel calibrations are unchanged by construction | existing operator tests; probe `rom_operator_probe` (coefficients ≤ 0.2 through the onset) |
| 8 | Exchange projected onto the k smooth modes: a pond cannot be held by them, so a member's exchanged volume landed on the near-constant mode and spread over the mesh (earlier, the uniform 1.3 cm dry-cell band; under the real storm, coverage 0.27 at width 0.04×). A first fix kept the volume as a column on the manhole's own cell: coverage 0.31, band 2 m at the manhole cell and none on the cells around it | 2D bands far too narrow at the ponds | the exchanged volume of each member is kept out of the modes and spread over the deterministic pond around the manhole (connected cells deeper than 1 cm, at most 5,000), raising its level by volume / pond area, as a still pond does; the exchange is solved on that same area for member and reference alike (`SpectralROM::pond_cells_`, `coupling_vol_`). The 2D analogue of the 1D quasi-static residual (H17) | `CouplingFluxSpreadsExchangeOverPondFootprint`; the coupling tests read the pond level via `couplingResidual()` |
| 6 | One-way coupling: a member's manhole level never fell as it poured water onto the street | q95 of 10⁵ m over 73,000 cells by 07:40 | first fixed with a manhole-storage drawdown; REMOVED (2026-10-24): a surcharged manhole's storage is ~0.1 m² once its pipes run full, so the drawdown starved real ponds. The pond footprint (8) and the implicit exchange already bound a member's pond at its own manhole level | — |
| 9 | Structures linearised at the deterministic state: a weir or flow brake just below the level at which it starts has zero local slope (and links with no flow were skipped altogether), so the rainfall channel saw closed outlets and let each member's extra water pile up in the basins behind them | G72R basins: 2–4 m rain band against 5–13 mm in the Monte Carlo; the over-wide upper members then spilled onto dry streets | each orifice, weir and outlet enters the flow response with the secant of its rating over the head range the members span, (Q(h+σ) − Q(h−σ))/2σ with σ the upstream half-band, or its tangent if larger; evaluated by `StructureSolver::flowAtShiftedDepth`, which restores every field it touches | `StructureSecantKeepsDeterministicRunBitIdentical` (Example 8; fails when the link flow is not restored; asserts that non-flowing structures enter the operator) |

Fixes 1–3 and 5 change the coupled path and the additive operator, so the
W3/W4 2D cells and the 1D gates are to be re-measured before this merges
(pending below).

## 2. The additive operator on converging flow

`rom_state_dump` wrote the mesh, basis and flow state at 05:30 and 06:30;
`operator_variants.py` assembled six candidate reduced operators from it.
Eigenvalues of the k = 40 operator (1/s; negative = growing):

| Variant | 05:30 | 06:30 |
|---|---|---|
| engine's additive operator as installed | +2.4e-4 | **−5.9e-3** |
| V1 plain Galerkin of upwind advection (current) | +4.1e-5 | **−6.2e-3** |
| V2 Galerkin in the cell-volume inner product | −2.4e-4 | −4.8e-4 |
| V3 volume inner product, diffusion only | +1.5e-7 | +1.2e-5 |
| **V4 volume inner product + skew-form advection** | **+7.4e-5** | **+2.0e-4** |
| V5 volume inner product + converging cells damped | +3.6e-2 | +4.4e-2 |
| V6 plain Galerkin, skew-form advection | +6.3e-4 | −2.2e-3 |

At 06:30, 79,000 of 155,688 cells have net inflow (ponds filling). The
full-order upwind operator is an M-matrix (stable in the L1 sense) but its
Galerkin projection in the plain inner product is not energy-stable: the
quadratic form carries ½·Σ δh_i²·div_i, negative where flow converges, and the
smooth modes can concentrate there. Writing advection in skew form (diagonal
reduced by half the net outflow) makes the symmetric part exactly the face
dissipation ½·Σ q·(δh_i − δh_j)², and projecting in the volume inner product
preserves that bound. Shipped as V4 (`DeviationOperator2D::skew_advection`,
`volume_inner_product`), for the additive channel only; the Manning channel's
depth-weighted Galerkin form was stable throughout and is unchanged. Coupling
volumes are projected consistently (G⁻¹·P[:,c]·ΔV). Rain and soft-rain
forcings are still projected plainly; the volume Gram has condition 1.3 and 4 %
off-diagonals on this mesh, so the inconsistency is small and is left for the
Monte Carlo to judge. V5 is stable but over-damped; V3 loses the downhill
transport of the extra water.

## 3. After the fixes (07:00 window, perturbed run)

| time | det cells > 20 cm | P>5 % / P>50 % / P>95 % cells | band width median / p99 / max (m) | \|q50 − det\| p99 / max (m) |
|---|---|---|---|---|
| 05:01 | 4,002 | 4,387 / 3,985 / 3,649 | 0.020 / 0.10 / 0.52 | 0.007 / 0.04 |
| 06:01 | 23,698 | 24,604 / 23,635 / 22,732 | 0.024 / 0.18 / 1.0 | 0.013 / 0.09 |
| 06:31 | 54,557 | 55,496 / 54,662 / 53,907 | 0.026 / 0.62 / 2.7 | 0.033 / 0.26 |
| 06:51 | 47,137 | 47,763 / 47,116 / 46,622 | 0.023 / 0.96 / 3.2 | 0.021 / 0.14 |

Bounded, median on the deterministic depth, probability classes bracketing the
deterministic count. Zero-perturbation drift is now at round-off before rain.

## 4. Against brute-force Monte Carlo (10 members, 05:00–10:00)

Members: Latin hypercube over Manning's n (×0.82–1.18, pipes and surface
together) and rainfall (×0.67–1.33 through the gage multiplier), full
deterministic runs of `demo_norain.inp` (`mc_prepare.py`, `run_mc.sh`,
9–21 min each single-threaded). Scored by `score_mc.py` at every 10-minute
frame: member coverage of the ROM 5–95 band (nominal 0.90), ROM width over
the members' Hazen 5–95 span, and the reliability of P(depth > 20 cm).

**Flood probability (the risk map) is reliable.**

| ROM class | members above 20 cm | cells |
|---|---|---|
| 0–5 % | 0.0 % | 1,602,882 |
| 5–25 % | 1.1 % | 18,142 |
| 25–50 % | 19.0 % | 2,890 |
| 50–75 % | 63.7 % | 2,503 |
| 75–95 % | 91.6 % | 2,130 |
| > 95 % | 99.5 % | 49,352 |

Brier score 0.0006 against 0.030 for the no-skill forecast.

**2D depth bands are smeared** (coverage 0.992 overall, width ratio median
3.2): by how much the members themselves disagree at the cell,

| MC 5–95 span | samples | coverage | width ratio | class |
|---|---|---|---|---|
| 0.5–2 cm | 53,495 | 0.964 | 4.88 | conservative |
| 2–5 cm | 36,813 | 0.884 | 2.56 | conservative |
| 5–20 cm | 24,898 | 0.811 | 1.56 | conservative (ceiling 1.5) |
| 20–100 cm | 3,308 | 0.521 | 0.46 | ranking only |
| > 1 m | 29 | 0.228 | 0.23 | ranking only |

Too wide on thin water, too narrow in the pond centres: the W4b mechanism
(smooth modes carry the deep cells' deviation onto their shallow neighbours)
plus the spill-volume leak of §6. The probabilities survive because the
threshold sits where the band is right-sized.

**1D pipe bands are conservative** (all manholes: coverage 0.938, width
ratio median 5.4; trunk transect 0.911 / 6.9). 476 of 1,011 manholes spill in
this storm; the members sit within 6 cm of the rim there while the unpinned
ROM band ran 2.5 m below to 1.5 m above it. Pinning the reconstruction at
spilling nodes (`SpectralROM1D::setPinnedNodes`, p_rim = 1 there) fixed those
nodes but not their neighbours: the 433 surcharged manholes that do not spill
are backed up by pinned nodes, the members disagree by 5–20 cm there, and the
ROM band is 5–11× that. The storm overloads the network, and the surcharged,
backwater-held regime is one the 1D ROM has not been calibrated in (H5's
documented limitation, now with a number on a real network). Open.

**What this means for the figures.** The probability map and the manhole
overflow probabilities at spilling nodes can be read as probabilities. The
depth bands are rankings with the stated bias. The manhole overflow
probabilities at surcharged, non-spilling nodes are overstated.

## 5. The model copy carried a 30× rainfall scale factor (retraction)

The first runs reported here used a copy of the model whose `[RAINGAGES]`
lines end in `MM * 30`. The engine's FILE format takes an optional trailing
scale factor (`CatchmentHandler.cpp`, "Build 5.3.0+, parity with legacy
gage.c"; `*` is the omitted start date), so every gage was multiplied by 30:
the 1D runoff continuity shows 617 mm of precipitation by 10:00 against 19–24
mm at the gages, and with rain on the mesh the 2D surface received 640 mm and
became a flat lake 5 m deep. The original model
(`SWMM_inp/Bellinge/7_SWMM/BellingeSWMM_v021_nopervious.inp`) ends in `MM`.
What an earlier draft of this section called a rain-accounting defect in the
2D marcher was this factor; there is no such defect. The engine's own
`RAINFALL_MODE NONE` remains the right setting for a model whose
subcatchments already carry the storm to the sewer. Sections 3, 4 and 6 are
re-measured below under the real storm (`demo_v2.inp`); the earlier numbers
describe a 1,740 mm storm and are kept only where marked.

A second configuration finding: a spread-only `[SOFT_RAINFALL_GRID]` on the
rainless mesh still forced the members (79 % of dry cells carried a 1–2 cm
q95), because a `CV` spread is scaled by the gage rain in force rather than by
the rain the mesh actually receives (zero under `RAINFALL_MODE NONE`). The
demonstration drops the grid; rainfall uncertainty reaches the surface through
the sewer spill. Worth a guard in `SurfaceRouter2D` (zero spread when no rain
is applied to the mesh) — not done here.

## 6. The demonstration run (`RAINFALL_MODE NONE`, no surface spread grid)

Dry cells everywhere carry a uniform q95 of about 1.3 cm that does not decay
with distance from the ponds (median 1.3 cm at 0–50 m and at 2–5 km): each
member's extra or missing spill volume lands on the near-constant mode and is
spread over the whole mesh (0.6 × 10⁶ m³ over 34.9 km² ≈ 1.7 cm). It is the
2D analogue of the 1D truncation H17 fixed with the quasi-static residual; a
per-coupling-cell residual is the candidate fix. The figures draw the 2D band
only where the median has water.

| time | det cells > 20 cm (ha) | P>5 % / P>50 % / P>95 % (ha) | band width median / p99 / max (m) | \|q50 − det\| p99 / max (m) |
|---|---|---|---|---|
| 06:01 | 6.6 | 37.3 / 6.6 / 5.1 | 0.044 / 0.22 / 0.59 | 0.022 / 0.04 |
| 06:31 | 40.3 | 93.8 / 40.5 / 35.4 | 0.022 / 0.23 / 0.89 | 0.018 / 0.16 |
| 07:01 | 41.8 | 48.3 / 41.9 / 38.1 | 0.018 / 0.14 / 1.36 | 0.013 / 0.08 |
| 08:01 | 49.4 | 54.9 / 49.5 / 45.9 | 0.014 / 0.15 / 1.60 | 0.017 / 0.08 |
| 09:01 | 51.9 | 55.0 / 51.6 / 47.8 | 0.023 / 0.16 / 1.67 | 0.013 / 0.05 |

The "possible" zone (P > 5 %) is wide on the rising limb and converges on the
deterministic extent afterwards. 1D: at 07:00, 44 manholes overflow in ≥ 95 %
of members, 720 are uncertain (5–95 %), 247 in ≤ 5 %. Whether those 720 are
real or over-wide surcharged bands is what §4's Monte Carlo decides.

## 9. Re-measured against a Monte Carlo that perturbs the rain (2026-10-26)

Reference: `mc_v3/`, 10 members, Latin hypercube in (Manning ×0.82–1.18, rain
×0.67–1.33). The rain is scaled by the gage file's trailing factor, and each
member's report shows its precipitation scaled exactly (3.73–7.39 ha·m against
5.56). Window 05:00–10:00, coverage / median width ratio.

| | `demo_v2` (2026-10-23) | `demo_v4` (2026-10-26) |
|---|---|---|
| 1D, all manholes | 0.959 / 1.43, calibrated | 0.957 / 1.40, calibrated |
| 1D, trunk transect | 0.856 / 1.60, conservative | 0.814 / 1.26, calibrated |
| 2D street depth | ranking, width 0.001–0.005 | 0.698 / 0.21, ranking |
| P(depth > 20 cm), Brier (no-skill) | 0.049 (0.106) | 0.041 (0.106) |

`demo_v4` adds the direct solve of the additive channel, the dry-pipe secant
(rain channel only), and the fix to the 2D Manning target (§ retraction). It
has per-member storage levels off. The 2D street bands are still about five times too narrow.

Storage nodes (median band against MC 5–95 span, m), `demo_v4`:

| Node | Outlet | MC | Sidecar |
|---|---|---|---|
| G72R83B | pipe over a raised end | 0.062 | 3.00 (50× wide) |
| G71F090 | weir | 0.084 | 1.41 (17× wide) |
| G72R150 / G72R70B | flow brake, offline pond | 0.454 | 3.45 / 3.28 (7× wide) |
| G80F13P | pump | 0.633 | 4.31 (7× wide) |
| G72R33B | weir + flow brake | 0.531 | 0.91 |
| G71F04R / G71F05R / G71F68Y | trunk storage | 0.25–0.30 | 0.12–0.17 (2× narrow) |
| G80F66Y | two weirs | 0.577 | 0.08 (7× narrow) |
| G72R520 | weir + flow brake | 0.275 | 0.06 (5× narrow) |

Per-member storage levels (ADDITIVE_CHANNEL.md §7) target exactly these nodes,
but they are not yet stable on this network.
