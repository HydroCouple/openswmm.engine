# PR 13a — Water-Quality Uncertainty Layer: Design One-Pager

**Tier**: O (design) · **Status**: proposed · **Impl**: PR 13b (tier S) · **Docs**: PR 13c (tier H)
**Branch**: `post/pr13-wq-uncertainty-layer`

## Goal

Wire the already-implemented, tested `WQUncertaintyBounds` (analytic first-order
decay bounds) to the input grammar and output path so that

```
[UNCERTAINTY]
QUALITY  <pollutant>  <pert>  [DIST]
```

produces per-node concentration bounds alongside deterministic quality routing.
Today `WQUncertaintyBounds::compute()` is test-only; nothing in the engine calls it.

This document fixes the three design decisions flagged in
`POST_REFORM_PR_CHECKLIST.md` §PR 13. The implementer (13b) follows these
verbatim; deviations require re-escalation to tier O/F.

---

## Decision (i) — `LayerTarget::QUALITY` enum + parser acceptance

### Enum

Append to `LayerTarget` in `src/engine/uncertainty/UncertaintyTypes.hpp`:

```cpp
enum class LayerTarget : int8_t {
    NONE    = 0,
    TWO_D   = 1,
    ONE_D   = 2,
    RUNOFF  = 3,
    QUALITY = 4,   // NEW — first-order decay-rate uncertainty (does NOT feed the modal ROM)
};
```

**Append, do not insert.** The values are stable ordinals relied on by
`RegisteredParam`, the legacy back-compat copies in `UncertaintyEnsemble::generate()`,
and any `switch`. Inserting would silently renumber `RUNOFF`.

### Parser (`parseUncertaintyLine`, `SectionHandlers2D.cpp`)

The existing grammar is `LAYER NAME PERT [DIST] [ENTRY]`. For the QUALITY layer
the **`NAME` token is a pollutant name**, not a parameter name — this is the one
semantic difference and must be documented in the grammar comment. Accept
`QUALITY` as a third `LAYER` value:

```cpp
else if (iequals(layer_str, "QUALITY"))
    layer = LayerTarget::QUALITY;
```

Rules specific to `QUALITY`:

- **NAME = pollutant name.** Do **not** upper-case-match against the known
  parameter names (`MANNINGS_N` / `RAINFALL` / `INFLOW`). Store the pollutant
  name as given. Cross-reference validation (does this pollutant exist?) is
  **deferred to engine init**, because `[UNCERTAINTY]` may parse before
  `[POLLUTANTS]` — mirror how other name references are resolved late.
- **Implied `ParamEntry` = `RATE_MULT`** (k is a rate multiplier). An explicit
  `ENTRY` token is rejected for `QUALITY` in v1 (the entry is fixed).
- **`DIST`: v1 accepts `UNIFORM` only.** `WQUncertaintyBounds` generates its own
  ascending *uniform* strata internally; honoring `NORMAL`/`LOGNORMAL` would
  require Option B (below). Reject non-uniform with a clear message rather than
  silently producing wrong bounds:
  `"QUALITY layer supports only UNIFORM distribution in this version"`.
- **Precedence block**: the existing `if (layer == TWO_D) { opts.enable_rom = ... }`
  tail does **not** apply. QUALITY specs update only `config.sources`; they never
  touch `SolverOptions2D` or enable any ROM.

The spec recorded is:
`{ name = <pollutant>, layer = QUALITY, entry = RATE_MULT, dist = UNIFORM, perturbation = pert }`.

---

## Decision (ii) — Where and how bounds are evaluated

### The honest-scope problem

`WQUncertaintyBounds::compute(c0, k, dt, M)` returns percentiles of
`c0 · exp(−k · kmᵢ · dt)`, i.e. bounds on decay applied over an interval `dt`
starting from a known concentration `c0`. The rigorous quantity — cumulative
concentration uncertainty *since the pollutant was injected* — needs the parcel
age `t` since injection. **Quality routing does not carry parcel age** (only
storage nodes track a hydraulic residence time). So a fully cumulative band is
out of scope for v1 and must not be faked.

### v1 semantics (documented limitation)

Evaluate at each **report boundary**, anchoring `c0` at the **start of the
report interval**:

- Cache the deterministic node concentration at the previous report boundary in
  a buffer `wq_conc_prev_[node·nPollut + p]` (seeded at simulation start from
  `nodes.conc`).
- At report time `t`, for each active QUALITY pollutant `p` and each node `n`:

  ```
  band = WQUncertaintyBounds{ .decay_pert = pert }
             .compute(c_prev, k_day, dt_rpt_days, M);
  ```

  where
  - `c_prev = wq_conc_prev_[n·nPollut + p]`  (concentration at interval start)
  - `k_day  = ctx.pollutants.k_decay[p]`      (1/day — native unit)
  - `dt_rpt_days = ctx.options.report_step / 86400.0`  (report step, seconds→days)
  - `M = n_members`

  To first order the **median multiplier ≈ 1**, so `band.q50 ≈ c_prev·exp(−k·dt)`
  which recovers the deterministic concentration at `t` — the band is centred on
  the deterministic value, as required.
- After writing, refresh `wq_conc_prev_ = nodes.conc` for the next interval.

**What v1 reports**: the spread in concentration attributable to ±`pert`
uncertainty in the decay rate `k` **over the most recent report interval only**.
It is *not* cumulative-since-injection uncertainty. 13c must state this plainly
in the USER_GUIDE, and VALIDATION should show a monotone widening test only over
a single interval (not across the run).

### Unit consistency (must-verify test)

`k_decay` is **1/day**; the report step is **seconds**. `compute()` uses the
product `k·dt`, so both must be days. Passing `k` in 1/day with `dt` in days is
the chosen convention. A unit test must assert
`compute(c, k_day, rpt_s/86400, M)` reproduces `c·exp(−k_day·kmᵢ·rpt_s/86400)`.

### Edge cases

- `k_decay[p] == 0` → `exp(0) = 1` → band collapses to `[c_det, c_det]`. Emit the
  row anyway (`q05 = q50 = q95`); a zero-width band is the correct answer (no
  decay ⇒ no decay-rate uncertainty).
- `M < 2` is floored to 2 inside `compute()` (already handled).

### Placement

In `SWMMEngine`, in the same reporting block that currently flushes the 1D ROM
CSV (`SWMMEngine.cpp`, the `if (rom1d_ && ...)` region), add an independent
`if (wq_unc_active_ && wq_csv_.is_open())` block. It is **not** nested under the
ROM condition — WQ uncertainty is available whenever quality routing runs, with
or without any ROM.

---

## Decision (iii) — Output channel: **separate `<rpt>.wq_uncertainty.csv`**

**Chosen: a new file, not a column on the existing one.**

Schema:

```
time_s,node_name,pollutant,q05,q50,q95
```

Opened at simulation start (when WQ uncertainty is active) using the same
path-derivation logic as the existing writer: replace a trailing `.rpt` with
`.wq_uncertainty.csv`, else append.

### Rationale

1. **Schema safety.** The existing `.uncertainty.csv` is
   `time_s,node_name,q05,q50,q95` — exactly one scalar (head/depth) per
   `(time, node)`. WQ needs an extra `pollutant` dimension. Adding a column would
   change the row cardinality and break the existing consumers
   (`scripts/uncertainty/plot_stormcity_profile.py`, `plot_coupling_uncertainty_5A_5B.py`)
   that assume one row per `(time, node)`.
2. **Unit hygiene.** Head/depth (length) and concentration (mass/volume) are
   different physical quantities with different provenance (modal ROM vs analytic
   post-processing). Co-mingling them behind a discriminator column invites
   misreads.
3. **Independent gating.** WQ bounds run without any ROM. A separate writer with
   its own open/close/gate parallels the existing pattern exactly and keeps the
   two features decoupled.
4. **Self-describing.** Anyone opening `*.wq_uncertainty.csv` immediately knows
   the units and the extra key column.

---

## Registry integration (the ROM-bypass note)

`k` uncertainty is `RATE_MULT`-shaped, but it acts on the **quality ODE**, not on
the hydraulic **modal ODE**. It therefore **must not** enter `SpectralROM*` or the
deviation-form member ODE.

- Register one param per active QUALITY spec via `UncertaintyEnsemble`:
  `{ name = <pollutant>, layer = QUALITY, entry = RATE_MULT, dist = UNIFORM, pert }`.
  This makes the spec visible in the config/registry and gives it a stable
  decorrelation `seed_offset` for the future Option B, but the ROM never
  iterates over `LayerTarget::QUALITY` params (it filters by `TWO_D`/`ONE_D`), so
  no ROM code changes are required.
- **v1 compute path = Option A**: `WQUncertaintyBounds` is self-contained and
  regenerates its own LHS strata from `decay_pert`. The registered `column` is
  *not* consumed in v1. This keeps the tested header untouched and is the minimal
  correct wiring.
- **Future Option B** (not in 13b): extend `WQUncertaintyBounds` to accept a
  precomputed multiplier column so it honors `DIST` and cross-parameter
  decorrelation via `UncertaintyEnsemble::column("<pollutant>", LayerTarget::QUALITY)`.
  The registry entry from v1 makes this a drop-in; that is why we register even
  though v1 does not read the column.

---

## Scope boundary for PR 13b (impl, tier S)

In scope:
- Enum value + parser branch (Decision i).
- `wq_conc_prev_` buffer, per-report evaluation, separate CSV writer (Decisions ii, iii).
- Late name→index resolution of QUALITY pollutant specs at engine init.
- Unit tests: reuse the existing `WQDecayBounds` suite invariants; add a unit
  test for the day/second unit convention; add an engine integration test on a
  quality-routing fixture (`Example3`-style or the CSTR benchmark network) that
  asserts (a) the `.wq_uncertainty.csv` is created, (b) `q05 ≤ q50 ≤ q95`,
  (c) band width grows with `pert`, (d) `k=0` ⇒ zero-width band.

Out of scope (escalate if the fixture forces them):
- Cumulative-since-injection uncertainty (needs parcel age in routing).
- Non-uniform distributions on the WQ layer (Option B).
- Feeding WQ uncertainty back into hydraulics or the ROM.

## Files touched (13b)

- `src/engine/uncertainty/UncertaintyTypes.hpp` — enum value.
- `src/engine/2d/input/SectionHandlers2D.cpp` — parser branch.
- `src/engine/core/SWMMEngine.{hpp,cpp}` — buffer, init resolution, report-time
  evaluation, second CSV writer.
- `tests/unit/engine/` — unit + integration tests.
- (13c) `docs/uncertainty/USER_GUIDE.md` — grammar row + semantics/limitation note.
