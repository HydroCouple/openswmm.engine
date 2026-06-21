# Phase 6 (relational LINK side-tables) — progress & resume plan

Branch `swmm6_rel`. Foundation + Stage A.1 **committed** at `71ae12a5` (on top of
Phase 5 `c268af1a`). All gates green at the checkpoint.

## Done (committed 71ae12a5) — verified
- **`src/engine/data/LinkSubtypes.hpp`** (new): 5 side-tables — `ConduitData`,
  `PumpData`, split `OrificeData`/`WeirData`/`OutletData` (named columns over the
  wide type-overloaded `param1`/`param2`). Ascending `link_idx` join, `subtype_row`
  reverse map, `set_link_type`/`erase_link`/`add_default`/`rebuild_index` +
  `*_row(i)` O(1) helpers. Temporary **`build(LinkData&)` mirror scaffold**
  (copies wide→side-tables; removed at Stage D).
- **`SimulationContext`**: `LinkSubtypes link_subtypes;` + include.
- **`SWMMEngine.cpp`**: `ctx_.link_subtypes.build(ctx_.links)` just before
  `hydstruct_.init` (line ~3922). NOTE: this is **after** `router_.init` (~3763),
  so `mod_length` + all init-derived conduit fields are already final in the mirror
  → any reader running after 3922 can be repointed parity-safe.
- **`StructureSolver::init`** (HydStructures.cpp): pump/orifice/weir/outlet config
  now sourced from the side-tables (`P.curve/startup/shutoff`, `orifices.cd`,
  `W.cd/weir_type/end_contractions`, `O.coeff/expon/curve`).
- Added **`OutletData.curve`** (TABULAR rating-curve index; legacy `pump_curve` reuse).

### Design deviation from handoff §2 (intentional, documented)
`xsect_*`, `has_flap_gate`, `pump_curve_name` **stay on base `LinkData`** — they are
shared (orifice/weir read `xsect_a_full/y_full/w_max/s_bot`; pump+outlet+conduit-
IRREGULAR share `pump_curve_name`). Handoff §2 put `xsect_*` in `ConduitData`, which
would break orifice/weir reads. So `ConduitData` holds only the conduit-exclusive
set (roughness, length, slope, mod_length, barrels, beta, rough_factor, q_full,
q_max, loss_inlet/outlet/avg, seep_rate, culvert_code).

## Gates at checkpoint
- Build (asserts live) clean; **ctest 86/86**.
- **.out byte-parity EXACT** vs pre-Phase-6 baseline across **all 18** epaswmm5_qa
  models (CFS + CMS), via `p6_parity.sh` (baseline = `phase_5/work/<m>/golden.out`).

## Remaining (the bulk) — resume here
Conduit-field read surface = **161 reads / 17 files** (from
`grep "links\.\(roughness\|length\|slope\|beta\|rough_factor\|q_full\|q_max\|loss_*\|seep_rate\|culvert_code\|barrels\|mod_length\)\["`):
Routing 26, DynamicWave 25, PostParseResolver 20, openswmm_links_impl 19,
SWMMEngine 16, DefaultReportPlugin 9, LinksHandler 8, GeoPackageReader 8,
InpWriter 7, Link 6, KinematicWave 5, Controls 4, Outfall 2, Inlet 2, Culvert 2,
QualityRouting 1, DefaultOutputPlugin 1.

- **Stage A.2 — conduit reads → `ConduitData`.** Safe path: readers running *after*
  `build()` (line 3922) are parity-safe immediately (mirror is faithful & final).
  - **CRITICAL non-conduit-default subtlety:** the wide arrays return type-defaults
    for non-conduit links (`q_full`=0, `length`=0, `slope`=0, `barrels`=1, etc.), and
    many cold reads are *ungated* (e.g. `Controls.cpp:487/491/493/504` resolve a control
    variable on **any** link type; `DefaultReportPlugin` has both gated conduit-only
    branches and ungated ones; `QualityRouting.cpp:406` reads `barrels` for all links).
    `conduit_row(idx)` returns **−1** for non-conduit links, so every *ungated* read
    must become `int cr = conduit_row(idx); v = (cr>=0) ? ConduitData.field[cr] : DEFAULT;`
    (DEFAULT = the wide resize() default: 0.0, or 1 for `barrels`). Reads already inside
    `if (type==CONDUIT)` branches can use the row directly. Audit each of the 161 reads
    for its guard context — this is where parity silently breaks if rushed.
  - Cold/non-hot first (bounded, easy parity): `Controls.cpp` (4), `DefaultReportPlugin`
    (9), `DefaultOutputPlugin` (1), `QualityRouting` (1).
  - Hot-loop (perf-sensitive — handoff §1 says keep reading cached groups, repoint the
    *init-time cache build*, not per-step): DynamicWave/KinematicWave/Routing/Link/
    Culvert/Inlet/Outfall. Check whether each conduit field is read per-step or cached
    at init; repoint the init source-read only. `Router::init` (3763) reads
    roughness/slope/length *before* `build()` — either move an early `build()` after
    `resolve_cross_references` (line ~202) or leave Router::init on wide for Stage A
    (it reads parse-set fields; it's a derivation site → Stage A.3).
- **Stage A.3 — flip writers authoritative.** `LinksHandler` (8) parse writes,
  `openswmm_links_impl` setters (19→ of ~82 total subtype hits), `PostParseResolver`
  resolution + `recompute_conduit_flow_properties` (writes beta/q_full), `TypeConverter::
  convert_link`, `ObjectDeleter`. Then getters → side-tables. Drop the `build()` mirror
  once writers own the data (or keep until Stage D).
- **Stage B — mutable per-step state.** Per handoff §3/§5: `setting`/`target_setting`/
  `direction`/`time_last_set` are **common control state → stay on base**; move only
  `dqdh`, `evap_loss_rate`, `seep_loss_rate`, `normal_flow_limited`, `inlet_control`,
  `full_state`, and `pump_curve_type` (written at init — currently still mirrored into
  `ctx.links.pump_curve_type` by StructureSolver::init).
- **Stage C — IO** (`InpWriter` 7, `GeoPackageReader` 8 + `GeoPackageWriter`,
  `convert_internal_to_display` for unit-converted conduit fields) source side-tables.
  GeoPackage **schema stays flat** (normalization is Phase 7).
- **Stage D — delete** wide `LinkData` subtype arrays; strip `resize/grow_to/erase_at/
  shrink_to_fit`; compiler enforces zero refs; remove `build()` mirror; migrate tests;
  peak-RSS before/after on a conduit/structure-heavy model.

## Verify each stage boundary
`p6_parity.sh` (18 models, CFS+CMS) must stay EXACT; `ctest`; INP round-trip;
C-API edit-then-get; `include/openswmm/engine/openswmm_links.h` unchanged.
Final: squash the WIP `71ae12a5` into one Phase 6 commit.
