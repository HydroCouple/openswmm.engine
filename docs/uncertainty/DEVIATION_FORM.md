# Deviation-Form ROM — Design Note (Reform PRs 6 & 7)

Status: normative spec for the deviation reformulation of `SpectralROM1D`
(PR 6, this document's primary scope) and `SpectralROM` (PR 7, ported per §6).

## 1. Why reformulate

The original ROM evolved the **total** head projected onto Laplacian modes
(`a = Pᵀh`). Its decay term `−λ_j·K1d·a_j` double-counted diffusion that is
already inside the deterministic solution's evolution, so the nominal member
drifted away from the deterministic run. `checkAndReseed()` patched the drift
by resetting **all members to identical coefficients**, erasing accumulated
ensemble spread every `reseed_min_interval` (~60 s) during dynamic events.
Published quantiles were therefore "spread regrown since the last reseed" — an
artifact of the reseed cadence, not propagated uncertainty (review finding F1).

## 2. The deviation state

Per member `i`, track the modal coefficients of the **deviation from the
deterministic trajectory**:

```
δa_i = Pᵀ (h_i − h_det)
```

`h_det` is the live deterministic solution (active-node heads from the DynWave
solver in 1D; the CVODE solution in 2D). The deterministic run is the reference
trajectory; members carry only how far their parameter perturbation pulls them
away from it.

## 3. Dynamics (1D form)

Member dynamics under the linearized diffusion surrogate with Manning
multiplier `mm_i` and runoff multiplier `rm_i`:

```
dh_i/dt   = −(K1d/mm_i)·L·h_i + rm_i·f
dh_det/dt = −K1d·L·h_det + f          (nominal model, mm = rm = 1)
```

Subtracting and projecting mode-by-mode (λ_j = eigenvalue,
b_j(t) = (Pᵀh_det)_j, r_j(t) = (Pᵀf)_j):

```
d(δa_ij)/dt = −(λ_j·K1d/mm_i)·δa_ij            (decay of the deviation)
              − λ_j·K1d·(1/mm_i − 1)·b_j(t)     (Manning sensitivity forcing)
              + (rm_i − 1)·r_j(t)               (forcing sensitivity)
```

Exact exponential step with `b_j`, `r_j` held constant over `dt`:

```
rate  = λ_j·K1d/mm_i
g     = −λ_j·K1d·(1/mm_i − 1)·b_j + (rm_i − 1)·r_j
steady = g/rate
δa ← (δa − steady)·exp(−rate·dt) + steady      (Euler fallback below rate_floor)
```

Ensemble-runoff path (`setEnsembleRunoff`): replace `(rm_i − 1)` with
`(rate_i/mean_rate − 1)`.

## 4. Properties (each one is an acceptance test)

1. **Zero-perturbation exactness.** `mm_i = rm_i = 1 ⇒ g ≡ 0 ⇒ δa ≡ 0`
   forever: `q05 = q50 = q95 = h_det` to machine precision. Impossible under
   the total-head form.
2. **No drift, no reseed.** The nominal member never leaves the deterministic
   trajectory, so `checkAndReseed()` and all its bookkeeping are **deleted**,
   and spread is never collapsed.
3. **Frozen-forcing steady state (analytic).** With `b_j` frozen and no runoff
   term: `δa_ij → g/rate = −mm_i·(1/mm_i − 1)·b_j = (mm_i − 1)·b_j`. Spread
   saturates at first order proportional to the perturbation — a closed-form
   invariant used directly in unit tests.
4. **Consistent decay.** If the deterministic state decays (`h_det → const`,
   `b_j → b_j^∞`), deviations chase `(mm_i − 1)·b_j(t)`; when `h_det → 0`
   (grounded operator, no forcing) all deviations decay to zero with it.
5. **Median tracks deterministic.** Reconstructed heads are monotone in
   `mm_i` at first order, so the median member stays within a small fraction
   of the spread around `h_det`.

## 5. API changes (1D)

| Old | New |
|---|---|
| `advance(dt, K1d, runoff)` | `advance(dt, K1d, h_det_active, runoff)` — computes `b_j = P[:,j]·h_det` once per call (O(k·n)); stores `h_det_last_`; `h_det_active` must be non-null |
| `computeQuantiles()` | `computeQuantiles(h_det_active, invert_active)` — reconstruction `h = h_det[t] + Σ_j P[j,t]·δa_ij`, clamped to `invert_active[t]` when non-null (replaces the datum-dependent `max(h, 0)`; fixes review finding F6-invert) |
| `reconstructHead(i, t)` | same signature; returns `max(h_det_last_[t] + Σ_j P[j,t]·δa_ij, 0)` — `h_det_last_` is at most one routing step old when called from the 2D coupling path (same freshness as the previous `node_heads` fallback); the `≥ 0` guard is retained as a numerical safety for the orifice formula |
| `seed(h)` | same signature; sets `δa = 0` and `h_det_last_ = h` |
| `checkAndReseed(...)` | **deleted**, with `seed_heads_`, `seed_mean_head_`, `last_reseed_time_`, `reseed_head_fraction`, `reseed_min_interval` |

`updateBasis()` re-projection (`R = P_newᵀP_old`) applies to δa unchanged —
deviations live in the same modal space.

### Mode-activity criterion

Deviation energy `E_j = mean_i(δa_ij²)` starts at zero, so activity must also
consider first-order *forcing* magnitudes or Manning spread never grows.
Mode `j` is active when ANY of:

```
by_energy :  E_j ≥ mode_drop_threshold
by_rain   :  max_i|scale_i − 1| · |r_j| · dt ≥ threshold
             (scale_i = rm_i, or rate_i/mean_rate on the ensemble path)
by_manning:  λ_j·K1d · max_i|1/mm_i − 1| · |b_j| · dt ≥ threshold
```

Note `by_rain` uses `|rm_i − 1|` (the deviation forcing scale), not `rm_i`
as the total-head form did.

## 6. Engine wiring (1D)

- `stepRouting()`: build `h_active` (the loop formerly feeding the reseed
  check) **before** `advance()` and pass it in; delete the reseed block. The
  dh/dt buffer remains the `runoff` argument; with the engine default
  `runoff_pert = 0` it contributes nothing — now by construction
  (`rm_i − 1 = 0`), not by hotfix.
- CSV/report block: build `h_active` and `invert_active`
  (`ctx_.nodes.invert`) at report time; pass to `computeQuantiles`.
- `buildROM1D()`: the `seed(h0)` call now just zeroes δa and primes
  `h_det_last_`.

## 6b. Refinement (PR 10): depth as the Manning-sensitivity reference (1D)

The MC validation (VALIDATION.md §3) showed that projecting the **absolute
head** into `b_j` lets the `(mm−1)·b_j` steady state scale the network's
invert relief — which roughness cannot physically move — overestimating
spread by orders of magnitude on sloped networks. `advance()` therefore
accepts an optional *sensitivity reference* field used only for the `b_j`
projection; the engine passes **depth** (head − invert), while absolute head
remains the anchor for reconstruction, quantiles, and `h_det_last_`. The 2D
ROM needs no such parameter — it operates in depth space natively. Callers
that pass no reference retain the §3 behavior (h_det projection), which is
what every closed-form unit invariant in §4 tests.

## 7. 2D port (PR 7 summary)

Identical structure with the 2D specifics: `h_det` = the CVODE solution
(`h_cell`, becoming required on the ROM path); per-mode rate uses the existing
depth-weighted Rayleigh-quotient `keff_ji`; Manning sensitivity forcing
`−λ_j·(keff_ji − keff_j^nominal)·b_j`; rainfall paths use (member forcing −
nominal forcing). `applyCouplingFlux()` applies only the per-member
**difference** `δQ_i = Q_i − Q_det` (the deterministic exchange is already
inside `h_det` — the old code double-booked it), and the orifice `/mm`
division is removed (Cd uncertainty is the `cd_mult` knob).
