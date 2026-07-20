# SPDE Spatial Basis for Correlated Soft-Rain Coherence (CL-2b)

Status: normative design note for CL-2b/CL-2c, written 2026-07-20.
Implements the "spatial basis construction" task of `CORR_LEN_PR_CHECKLIST.md`
Phase 2. Component: `src/engine/uncertainty/SpdeSpatialBasis.{hpp,cpp}`.

## 1. Problem

CL-1 materializes the correlated coherence field as an explicit `M×n` array via
`CorrelatedFieldGenerator::generateCoefficientField` (exponential-kernel
smoothing of white noise + per-cell rank map). CL-2a profiling showed two costs:

| Cost | CL-1 measured (9.8k tri, M=50, ℓ=200 m) |
|---|---|
| Field generation (one-time) | **64,159 ms** — `O(M·n·n_nbr)`, and `n_nbr → n` when `3ℓ` ~ domain size |
| Per-step correlated projection | 14.1 ms — `O(M·k·n)` |

CL-2b replaces the generator with a small spatial basis: `K_s` deterministic
modes `φ_m(x)` with weights `w_m`, per-member random coefficients `a_im`, and

```
W_i(t) = Σ_m a_im · φ_m(t)          (member i's coefficient field)
```

Build cost is `O(K_s·n)` trig evaluations (milliseconds), materialization —
if wanted at all — is `O(M·K_s·n)` flops, and CL-2c can project the `K_s`
basis-weighted spread fields instead of `M` member fields per step.

## 2. What covariance must the basis reproduce? (key correction)

The checklist asked for "empirical covariance vs `exp(−d/ℓ)`". **That is not
what CL-1 produces, and it is not a practical reduced-basis target.** Both
facts follow from the same analysis:

**(a) Kernel smoothing yields the kernel's *self-convolution*, not the
kernel.** The CL-1 field is `ẑ(t) = Σ_s k(d_ts) z_s / Σ_s k(d_ts)` with
`k(d) = exp(−d/ℓ)` and white `z`. Its correlation is

```
ρ(d) = (k ⋆ k)(d) / (k ⋆ k)(0).
```

In 2D the Fourier transform of `exp(−κr)` is `k̂(ξ) ∝ (κ² + |ξ|²)^{−3/2}`
(κ = 1/ℓ), so the smoothed field has spectral density `k̂² ∝ (κ² + |ξ|²)^{−3}`
— a **Whittle–Matérn field with ν = 2** (from `ν + d/2 = 3`, d = 2) and the
*same* κ. Closed form:

```
ρ(d) = ½ (κd)² K₂(κd),     κ = 1/ℓ.
```

**Measured (2026-07-20, 48×48 grid, 12 m spacing, ℓ = 72 m, M = 200,
`generateCoefficientField` — the soft path CL-2 replaces):**

| Reference curve | RMS error of empirical correlation, d ∈ (0, 3ℓ] |
|---|---|
| Matérn ν=2, κ=1/ℓ | **0.027** |
| `exp(−d/ℓ)` | 0.339 |

(The rank map is a Gaussian copula in normal scores, so it preserves the
Gaussian field's correlation almost exactly. `generate()` — the multiplier
path — additionally subtracts the per-member spatial mean, which depresses
long-range correlation on small domains; the soft path does not centre.)

**(b) A true `exp(−d/ℓ)` (ν = ½) field is too rough for a small basis.** With
spectral density `(κ²+λ)^{−3/2}`, capturing 95 % of the variance needs modes up
to `Λ ≈ 400κ²`, i.e. `K_s ≈ 32·(L_pad/ℓ)²` — order 10³ for typical geometry.
The ν=2 density `(κ²+λ)^{−3}` needs only `Λ ≈ 3.5κ²`, i.e.
`K_s ≈ 0.28·(L_pad/ℓ)²` — order 10–40. The checklist's "typically 10–30 modes"
estimate is achievable *only* for the ν=2 target.

**Decision: the CL-2 basis reproduces Matérn ν=2 with κ = 1/ℓ** — the
covariance CL-1 actually realizes. This keeps `CORR_LEN` semantics identical
across CL-1/CL-2 (required for the CL-2c equivalence test) and makes the
reduced basis feasible. `ℓ` remains the kernel scale the user configures;
neither phase ever produced literal `exp(−d/ℓ)` decay. (For calibration:
ρ(ℓ) ≈ 0.81, ρ(3ℓ) ≈ 0.29, vs 0.37 / 0.05 for the exponential.)

## 3. Eigenbasis: analytic Neumann cosines on an embedding rectangle

The checklist suggested a sparse SPDE precision matrix + the existing Lanczos
machinery, and warned that graph-Laplacian eigenvalues must be mapped to the
kernel's spectral density correctly or "the correlation length is silently
incorrect". We sidestep that hazard entirely:

- The field's domain is the **bounding box of the target points** (2D triangle
  centroids, or 1D node coordinates — both are scattered points in the plane;
  both CL-1 generators already use *Euclidean*, not geodesic, distance, so a
  convex embedding is the faithful match).
- On a rectangle `[x₀, x₀+Lx] × [y₀, y₀+Ly]` the Neumann Laplacian has
  **analytic eigenpairs in physical units**:

  ```
  φ_pq(x,y) = √(e_p e_q) · cos(pπ(x−x₀)/Lx) · cos(qπ(y−y₀)/Ly)
  λ_pq      = (pπ/Lx)² + (qπ/Ly)²          [1/m²]
  e_0 = 1, e_{p>0} = 2   (so the domain average of φ² is exactly 1)
  ```

  No graph construction, no edge-weight calibration, no Lanczos, no mesh
  dependence — the eigenvalue-scaling pitfall cannot occur because λ is exact.
- **Padding.** The box is padded on each side by
  `pad = padding_factor · min(ℓ, D)`, `D` = max raw extent, default
  `padding_factor = 3`. Padding pushes the Neumann boundary images away from
  the points: the image term inflates variance by `≈ ρ(2·pad)`; for ν=2,
  ρ(4ℓ) ≈ 0.14 but ρ(6ℓ) ≈ 0.03, hence 3ℓ (not the 2ℓ customary for
  exponential kernels — the ν=2 tail is fatter). The `min(ℓ, D)` cap keeps the
  padded box from scaling with ℓ when ℓ ≫ D: with `pad = 3ℓ` the mode
  structure would be scale-invariant and the comonotone limit would never be
  reached; with `pad = 3D`, `λ₁/κ² = (πℓ/7D)² → ∞` as ℓ grows and all
  variance collapses into the constant mode. (CL-1's smoothing has boundary
  effects of the same sign — less averaging near edges — so the match is fair.)

## 4. Spectral weights, truncation, normalization

Relative variance of candidate mode `(p,q)`:

```
v_pq = e_p · e_q · (1 + λ_pq/κ²)^{−3}         (dimensionless; v_00 = 1)
```

- Candidate set: `p ≤ p_max`, `q ≤ q_max` with the cutoff eigenvalue
  `Λ_max = κ²·(ε^{−1/2} − 1)`, ε = 10⁻³ (99.9 % of the analytic total),
  `p_max = ceil(Lx·√Λ_max/π)` clamped to [1, 512].
- `T = Σ v` over candidates. The **constant mode (0,0) is always kept in slot
  0** (see §5); remaining slots are filled in descending `v` (ties broken by
  `(p,q)` for determinism) until `Σ_kept ≥ f_target·T` (default 0.95) or
  `K_s = max_modes` (default 64). Note (0,0) is *not* always the largest-`v`
  mode — for `L_pad ≫ ℓ`, `v_10 = 2·(1+λ₁/κ²)^{−3} → 2 > 1` — but it is
  pinned to slot 0 regardless, for the comonotone anchoring below.
- Weights: `w_m = √(v_m / Σ_kept v)`, so `Σ w_m² = 1` exactly. Since the
  domain-average of each `φ_m²` is 1, the domain-averaged field variance is
  `Var(ξ)·Σw² = Var(ξ)`. `captured_variance_fraction = Σ_kept v / T` is
  reported so CL-2c can gate/warn.
- **Per-point variance normalization** (applied in `materializeField`). The
  `Σw² = 1` rule preserves variance averaged over the *padded* box, but the
  target points occupy its interior where the low cosines are suppressed
  (`cos²` over a central sub-window averages < ½), so raw per-cell variance
  falls short — measured ≈ 0.59·Var(c) at ℓ = 90 m on a 372 m domain. Each
  cell is therefore rescaled by `g(t) = √(Var(c) / rawvar(t))`, where the
  target `Var(c)` is recovered from the mode-0 column (`Var_i(a_i0)/w_0²`) and
  `rawvar(t)` is the empirical per-cell member variance. Result: **exact**
  per-cell variance `= Var(c)` (matches CL-1's rank map, which is a permutation
  of `{c_i}` at every cell). A per-point scalar cancels in the correlation
  ratio, so `g(t)` leaves the covariance *shape* untouched — it only removes
  the boundary-driven variance non-stationarity. The comonotone limit stays
  exact: `K_s = 1 ⇒ w_0 = 1 ⇒ rawvar = Var(c) ⇒ g ≡ 1`. **CL-2c seam:** the
  reduced-projection path must fold `g(t)` into the mode fields
  (`ψ_m(t) = g(t)·φ_m(t)`) before projecting, or normalize its reconstructed
  band per node; the raw linear form is only variance-correct after `g`.

## 5. Per-member coefficients — comonotone anchoring

`a` is `M × K_s`, `a_im = w_m · ξ_im`:

- **Mode 0 (constant): `ξ_i0 = c_i`**, the ROM's *existing* per-member soft
  coefficient (`softCoeff()` — `probit(u_i)` for NORMAL/LOGNORMAL, `2u_i−1`
  for UNIFORM). This anchors member identity (member i's domain-wide
  wet/dry tendency keeps its comonotone percentile) and yields the **exact
  comonotone limit**: as ℓ → ∞ only (0,0) survives, `K_s = 1`, `w_0 = 1`,
  `φ_0 ≡ 1` ⇒ `W_i(t) = c_i` bit-exactly.
- **Modes m ≥ 1: independent LHS draws**, `u = shuffledStrata(M, seed+4+m)`
  (per the checklist's seed schedule), family-aware transform:
  `ξ = probit(u)` (NORMAL/LOGNORMAL, Var ≈ 1) or `ξ = 2u−1` (UNIFORM,
  Var = ⅓) — matching the variance of the family's `c_i` so the per-cell
  variance equals the comonotone coefficient variance for every family.

**Invariants preserved.** LHS strata are symmetric and both transforms are
odd, so `mean_i ξ_im = 0` to fp round-off for m ≥ 1, hence the per-cell
column mean is `w_0·φ_0·c̄ = c̄` — the deviation-form requirement (nominal
member zero-deviation; q50 tracks the deterministic answer; the 2D ROM's
debug assert `|colmean − c̄| < 1e-9` holds).

**Marginals (documented approximation).** `W_i(t)` is a *linear* combination
of independent draws, so for `K_s > 1` per-cell marginals are approximately
Gaussian with the family's variance rather than exactly the family's shape
(CL-1's rank map had exact marginals but is nonlinear and cannot be reduced).
For UNIFORM this widens the q05–q95 band by ≈ 5 % (1.645·σ_normal vs
0.9·halfrange at equal variance); for NORMAL/LOGNORMAL the marginal family is
unchanged. This is part of the CL-2c "truncation + statistical tolerance"
equivalence budget.

## 6. Costs

| Operation | Complexity | Expected (10k pts, M=50, K_s≈33) |
|---|---|---|
| `build()` | O(#cand log #cand + K_s·n) | ~ms (vs CL-1's 64 s generation) |
| `sampleCoefficients()` | O(M·K_s) | µs |
| `materializeField()` | O(M·K_s·n) | ~10–20 ms |
| CL-2c per-step projection | O(K_s·k·n) + O(M·K_s·k) | wins iff `K_s < M` |

**Guidance for CL-2c.** `K_s ≈ 0.28·(L_pad/ℓ)²` (95 % target). When
`K_s < M` (moderate ℓ), use reduced projections `R_m = Pᵀ(φ_m ⊙ spread)`.
When ℓ is small enough that `K_s` hits `max_modes` (≥ M), reduced projection
loses to the materialized path — but `materializeField()` still replaces the
64 s generator with milliseconds, so CL-2c should then materialize from the
basis and reuse the existing CL-1 spatial consumption path unchanged. Either
way the dominant CL-2a cost (generation) is eliminated.

## 7. API

```cpp
namespace openswmm::uncertainty {

struct SpdeSpatialBasisConfig {
    double target_variance_fraction = 0.95;
    int    max_modes                = 64;
    double padding_factor           = 3.0;
};

class SpdeSpatialBasis {
    void build(const double* x, const double* y, int n_points,
               double corr_len, const SpdeSpatialBasisConfig& cfg = {});
    bool   is_built() const;  int n_modes() const;  int n_points() const;
    double capturedVarianceFraction() const;
    const std::vector<double>& modeValues()  const;  // K_s × n, φ_m[t]
    const std::vector<double>& modeWeights() const;  // w_m, Σw² = 1
    const std::vector<int>& modeP() const;           // cosine index p per mode
    const std::vector<int>& modeQ() const;           // cosine index q per mode
    void sampleCoefficients(const std::vector<double>& mode0_coeff,  // c_i
                            DistType family, uint64_t seed,
                            std::vector<double>& a_out) const;       // M × K_s
    void materializeField(const std::vector<double>& a, int n_members,
                          std::vector<double>& field_out) const;     // M × n
    static double modelCorrelation(double d, double corr_len);       // ½(κd)²K₂(κd)
};
}
```

Shared header/source under `src/engine/uncertainty/` (no `OPENSWMM_HAS_2D`
guard): the 1D gage path and the 2D grid path both consume it; output plugs
into `SoftSpatialField.values` / `SpatialUncertaintyField.values` (both are
`M×n` row-major `vector<double>`).

Errors: `build()` throws `std::invalid_argument` on `corr_len ≤ 0` or
`n_points ≤ 0` (comonotone is the *caller's* scalar path, not a basis).
Degenerate geometry (all points coincident) builds the DC-only basis.

## 8. Tests (`tests/unit/engine/test_spde_spatial_basis.cpp`)

- `ComonotoneLimitExact` — ℓ ≫ domain ⇒ `K_s == 1`, constant mode, materialized
  rows `== c_i` (exact).
- `Mode0IsConstantAndWeightsNormalized` — slot 0 is (0,0), `φ_0 ≡ 1`,
  `Σw² = 1`, captured fraction ∈ (0, 1].
- `EmpiricalCovarianceMatchesMatern` — binned empirical correlation of the
  materialized NORMAL-family field vs `modelCorrelation`. **Measured
  2026-07-20** (48×48 grid, 12 m spacing, ℓ = 60 m, domain/ℓ ≈ 9.4, M = 200,
  K_s = 64): RMS = 0.042, worst-bin 0.06. The fit is a finite-domain effect:
  smaller domain/ℓ decays faster than the ideal (RMS ratio 6.3 → 0.066, 9.4 →
  0.042, 12.5 → 0.037) — the CL-1 smoother behaves the same way, so at equal
  geometry the two paths agree. The test also asserts `exp(−d/ℓ)` misfits by
  ≥ 3× (guards against confusing the kernel with the field's covariance).
- `CL1SoftFieldMatchesSameModel` — `generateCoefficientField`'s empirical
  correlation vs the same model (locks the §2 claim; guards CL-1/CL-2
  consistency against future kernel changes).
- `ColumnMeanPreservesCbar` — deviation-form invariant.
- `FamilyVarianceMatched` — per-cell member variance equals the comonotone
  coefficient variance (≈ 1 for NORMAL, ≈ ⅓ for UNIFORM) to within ±3 %, for
  both families, via the per-point normalization of §4.
- `DeterministicAcrossCalls` — bitwise reproducibility.
- `BuildAndMaterializeFastOnLargePointSet` — 10k points, M=50: asserts a
  generous wall-time bound and prints measured ms (the CL-2a 64 s comparison).
- `ThrowsOnInvalidArgs`.

## 9. Deviations from the checklist (with reasons)

1. **Covariance target is Matérn ν=2 (κ=1/ℓ), not `exp(−d/ℓ)`** — §2: it is
   what CL-1 actually produces (measured RMS 0.027 vs 0.339), and the only
   target reachable with `K_s ≪ M`.
2. **Analytic rectangle eigenpairs instead of sparse Lanczos** — §3: exact λ
   in physical units eliminates the checklist's own "silently incorrect
   correlation length" pitfall; nothing to solve, nothing to calibrate.
3. **`a_i0` is anchored to the existing member coefficients `c_i`** rather
   than a fresh `seed+4` draw — exact comonotone limit and member-identity
   continuity (§5). Higher modes follow the checklist's `seed+4+m` schedule.
