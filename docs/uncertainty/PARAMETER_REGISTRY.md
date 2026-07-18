# Parameter Registry — Design Note (Reform PR 9)

Status: normative spec for PRs 9a (types, this doc), 9b (ensemble + ROM
consumption), 9c (parser + engine wiring). Fixes review finding **F5**
("hard-wired parameter set — not 'any parameter'").

## 1. Problem

The sidecar's promise is *"propagate uncertainty from any input or parameter"*,
but the implementation hard-wires exactly four parameters (2D Manning, 2D
rainfall, soil Ks, coupling Cd) as named struct fields with bespoke plumbing
each (`mannings_mult_2d`, `setEnsembleRainfall`, `soilSamples()`,
`setCdSamples`). Adding a fifth parameter today means touching
`UncertaintyEnsemble`, both ROM structs, the parser, and the engine — the
definition of not-extensible.

The fix is a registry: parameters become *data* (name, layer, how-it-enters,
distribution, perturbation), not *fields*. The insight that makes this small
is that under the deviation form (PRs 6–7) every scalar parameter enters the
modal ODE in exactly one of three ways.

## 2. Taxonomy — how a scalar parameter enters the modal ODE

The deviation-form member ODE (DEVIATION_FORM.md §3) is

```
d(δa_ij)/dt = −rate_ij·δa_ij + g_ij
```

Every scalar parameter θ with per-member samples θ_i shapes `rate` and/or `g`
in one of these ways:

```cpp
enum class ParamEntry : int8_t {
    RATE_MULT,       // roughness-like: divides the decay rate
    FORCING_MULT,    // rainfall-like: scales an existing forcing projection
    FORCING_VECTOR,  // inflow/boundary-like: scales a registered per-node field
    COUPLING_MULT,   // exchange-flux multiplier (outside the modal ODE)
};
```

| Entry | Rate effect | Deviation forcing sensitivity | Prototype |
|---|---|---|---|
| `RATE_MULT` | `rate_ij = λ_j·K/θ_i` | `−λ_j·K·(1/θ_i − 1)·b_j(t)` | Manning's n |
| `FORCING_MULT` | — | `(θ_i − 1)·r_j(t)`, `r_j = Pᵀ·f` | rainfall / runoff scale |
| `FORCING_VECTOR` | — | `(θ_i − 1)·(Pᵀ·v)_j`, `v` = registered per-node field | DWF/inflow scale, boundary-stage offset, per-member RunoffEnsemble rates |
| `COUPLING_MULT` | — (not in the ODE) | scales the orifice exchange flux `Q_i = Cd·θ_i·A·√(2g·Δh)` | discharge coefficient Cd |

Notes:
- `FORCING_VECTOR` differs from `FORCING_MULT` only in *which field* it
  scales: `FORCING_MULT` rides the ROM's existing forcing argument
  (rainfall/runoff), while a `FORCING_VECTOR` param carries its own non-owning
  pointer to a per-node/per-cell field supplied by the caller and re-projected
  each `advance()`. It subsumes `setEnsembleRunoff` (a per-member rate scale on
  a nodal field) and is the natural slot for the soft-rainfall spread plane
  (SR-3) later.
- `COUPLING_MULT` is a **documented extension** beyond the three-entry
  taxonomy originally sketched in the reform checklist: Cd does not enter the
  modal ODE at all (PR 7 removed the orifice `/mm` division precisely because
  roughness doesn't govern orifice discharge), but Cd *is* one of the four
  legacy parameters the registry must own, so it needs an honest tag. Any
  future inlet-conveyance parameter would also use it.
- Combination rule: multiple registered `RATE_MULT` params on one layer
  combine multiplicatively into the effective rate multiplier
  (`mm_eff_i = Π θ_i`); multiple `FORCING_MULT` params likewise
  (`scale_i = Π θ_i`, sensitivity `(scale_i − 1)·r_j`). Each
  `FORCING_VECTOR` param contributes its own independent additive term.

## 3. Distributions

```
enum DistType { UNIFORM, NORMAL, LOGNORMAL }   // existing enum, REUSED
```

**Deviation from the checklist sketch (decided here):** the checklist named a
new `DistKind {UNIFORM, LOGNORMAL, NORMAL_TRUNC}`. The codebase already has
`DistType {UNIFORM, NORMAL, LOGNORMAL}` — parsed by `[UNCERTAINTY]` since
Phase 1 but consumed nowhere ("the dead enum"). Introducing a second
distribution enum would be a permanent source of confusion; instead PR 9
**finally consumes `DistType`**, with `NORMAL` *defined* as the ±3σ-truncated
normal the checklist wanted. (The soft-rainfall design already planned to
consume `DistType` for its families — one enum serves both.)

All three families map a stratum midpoint `t ∈ (0,1)` (from
`shuffledStrata()`) to a multiplier θ with the **same user-facing meaning of
`p`**: `p` is the half-range of the multiplier band.

| Family | Inverse CDF `θ(t; p)` | Band |
|---|---|---|
| `UNIFORM` | `θ = (1−p) + t·2p` | exactly `[1−p, 1+p]`; equal mass everywhere |
| `NORMAL` (±3σ trunc) | `θ = 1 + (p/3)·probit(Φ(−3) + t·(Φ(3)−Φ(−3)))` | hard `[1−p, 1+p]` at ±3σ; mass concentrated near 1 |
| `LOGNORMAL` | `θ = exp(z(t)·σ_log)`, `σ_log = ln(1+p)/1.6449` | median 1; `q95 = 1+p`, `q05 = 1/(1+p)` — **asymmetric**, θ > 0 always |

- `probit(u)` is the standard-normal quantile (Acklam rational approximation,
  |ε| < 1.15e-9; added to `LhsShuffle.hpp` in 9b — the same helper the
  soft-rainfall work needs, added once here).
- `NORMAL` truncation is exact (inverse-CDF of the truncated distribution),
  not clamp-after-sampling: `t` is affinely mapped into `[Φ(−3), Φ(3)]`
  before the probit. Mean stays exactly 1 by symmetry.
- `LOGNORMAL` picks σ_log so the *upper* 95th percentile hits `1+p`
  (`z95 = 1.6449`); the lower tail lands at `1/(1+p)`, not `1−p`. This
  asymmetry is intentional and documented: lognormal is the right family for
  positive-only physical multipliers, and its skew is the point of choosing it.
- `is_active()` semantics unchanged: `p = 0` ⇒ column of exact 1.0s regardless
  of family.

## 4. Registry data model

```cpp
// UncertaintyTypes.hpp (9a)
struct RegisteredParam {
    std::string  name;                       // e.g. "MANNINGS_N", "INFLOW", "MY_PARAM"
    LayerTarget  layer = LayerTarget::NONE;
    ParamEntry   entry = ParamEntry::FORCING_MULT;
    DistType     dist  = DistType::UNIFORM;
    double       pert  = 0.0;                // half-range p (see §3)
    uint64_t     seed_offset = 0;            // column seed = ensemble.seed + seed_offset
    bool         reference_column = false;   // ascending strata, no shuffle (Manning prototype)
    std::vector<double> column;              // filled by generate(); length n_members
};
```

`UncertaintyEnsemble` owns `std::vector<RegisteredParam> params` plus:

```cpp
RegisteredParam& registerParam(name, layer, entry, dist, pert);  // next free seed_offset
const std::vector<double>* column(name, layer) const;             // nullptr if absent
void registerDefaults();  // the four legacy params, from the legacy pert fields
```

`generate()` loops over `params`: strata are ascending for
`reference_column`, else `shuffledStrata(M, seed + seed_offset)`; each stratum
maps through §3's inverse CDF.

### Back-compat contract (bit-exactness)

`registerDefaults()` registers, in order, with these fixed seed offsets:

| # | name | layer | entry | offset | reference |
|---|---|---|---|---|---|
| 0 | `MANNINGS_N` | 2D | RATE_MULT | — | yes (ascending) |
| 1 | `RAINFALL` | 2D | FORCING_MULT | 1 | no |
| 2 | `SOIL` | RUNOFF | RATE_MULT | 2 | no |
| 3 | `CD` | 2D | COUPLING_MULT | 3 | no |

With `UNIFORM` these reproduce the PR-5 columns **bit-exactly** (`lo + t·(hi−lo)`
with the same strata and the same seeds — an acceptance test asserts this).
The legacy vectors (`mannings_mult_2d`, `rainfall_mult_2d`, `soil_mult`,
`cd_mult`) remain as fields, copied from the registered columns after
`generate()`, so every existing accessor and consumer is unchanged.
User-registered params take offsets from 4 upward.

## 5. ROM consumption (9b)

Both ROMs keep their built-in Manning/rainfall machinery untouched (those are
"the first two registered params" in behaviorally-identical form) and gain a
generic path for *additional* registered params:

```cpp
// SpectralROM1D / SpectralROM
void addRegisteredParam(ParamEntry entry,
                        const std::vector<double>& column,   // θ_i, length M (copied)
                        const double* field = nullptr);      // FORCING_VECTOR only; non-owning
void clearRegisteredParams();
```

In `advance()`:
- effective rate multiplier: `mm_eff_i = mannings_mult[i] · Π_{RATE_MULT} θ_i`
  (2D spatial-Manning path: the per-cell Rayleigh quotient is divided by the
  same product);
- forcing scale: `scale_i = (rm_i or ensemble rate ratio) · Π_{FORCING_MULT} θ_i`,
  sensitivity `(scale_i − 1)·r_j`;
- each `FORCING_VECTOR` param: project its field once per call
  (`rv_j = Pᵀ·v`), add `(θ_i − 1)·rv_j` to `g_ij`. The field pointer must stay
  valid across the ROM's lifetime (engine-owned buffers);
- mode-activity: the existing `by_manning`/`by_rain` bounds use the effective
  products; each vector param adds `max_i|θ_i − 1|·|rv_j|·dt ≥ threshold`.

`COUPLING_MULT` params are *not* consumed by `advance()`; the coupling path
keeps reading `cd_mult` (one knob today). A second coupling parameter would
extend `applyCouplingFlux` explicitly — out of scope.

Zero-extra-params behavior is bit-identical to PR 7 (no code path touched).

## 6. Parser grammar (9c)

```
[UNCERTAINTY]
;;Layer  Name        Pert   [Dist]      [Entry]
2D       MANNINGS_N  0.20
1D       RAINFALL    0.15   UNIFORM
1D       INFLOW      0.30   LOGNORMAL   FORCING_VECTOR
1D       MY_KNOB     0.10   NORMAL      RATE_MULT      ; unknown name OK with explicit entry
```

- New order `LAYER NAME PERT [DIST] [ENTRY]`; the **legacy order**
  `LAYER NAME [DIST] PERT` (all existing files, USER_GUIDE examples) remains
  accepted — disambiguation is trivial: if token 3 parses as a number the new
  order is in effect, otherwise the legacy order.
- Name-implied default entries: `MANNINGS_N → RATE_MULT`,
  `RAINFALL → FORCING_MULT`, `INFLOW → FORCING_VECTOR`.
- Unknown NAME **with** explicit ENTRY: accepted (the generality win — the
  engine wires anything it recognizes; unrecognized names still generate
  columns and are exposed via `specs_for(layer)` for API consumers).
- Unknown NAME **without** ENTRY: parse error listing the known names.
- `UncertaintyConfig` gains `specs_for(LayerTarget)` returning all active
  specs for a layer.

### Engine wiring for `INFLOW` (1D)

`buildROM1D()` registers each non-built-in active 1D spec on the 1D ROM. For
`INFLOW` (`FORCING_VECTOR`) the registered field is the engine's per-node
dh/dt buffer (`rom1d_dh_buf_`) — the same DynWave head-rate field the built-in
runoff scale multiplies — so a member with θ_i = 1.3 experiences the observed
lateral-inflow-driven head changes 30% stronger. Columns are generated through
a local `UncertaintyEnsemble` (seed 42, offsets from 4) so the sampling is
identical to API-registered parameters.

## 7. Non-goal (deferred, interface sketched)

**Rank-correlation control (Iman–Conover).** All registered columns are
independently shuffled (near-zero pairwise rank correlation). Physically
correlated priors (e.g. Ks vs porosity) would need
`setRankCorrelation(name_a, name_b, rho)` applying an Iman–Conover
re-ordering of column b against column a after `generate()`. Deliberately not
in 9b/9c: no consumer needs it yet, and it interacts with the bit-exactness
contract (any correlation request would opt those columns out of it).

## 8. Test contract (9b/9c)

- Moments per family (M = 200): mean within 2% of 1 (UNIFORM/NORMAL), median
  within 2% of 1 (LOGNORMAL); LOGNORMAL q05/q95 within 3% of `1/(1+p)` /
  `(1+p)`.
- `probit`: |probit(Φ(z)) − z| < 1.2e-8 on z ∈ [−6, 6].
- Back-compat: `registerDefaults()` + `generate()` reproduces the PR-5 legacy
  vectors bit-exactly (`EXPECT_DOUBLE_EQ` element-wise).
- Registry lookup: `column(name, layer)`; absent → nullptr.
- ROM level: a `FORCING_VECTOR` param on the 1D layer produces spread on a
  zero-perturbation-otherwise ROM; removing it (`clearRegisteredParams`)
  restores bit-identical no-extra-params behavior.
- Parser: both token orders; entry defaulting; unknown-name rules; error texts.
- End-to-end: `.inp` with `1D INFLOW 0.3 LOGNORMAL FORCING_VECTOR` on the
  Phase-9 fixture → nonzero band in `.uncertainty.csv`.
