# Phase 6 (relational LINK side-tables) — progress & resume plan

Branch `swmm6_rel`. Foundation + Stage A.1 **committed** at `71ae12a5` (on top of
Phase 5 `c268af1a`). All gates green at the checkpoint.

## RESUME POINT (latest) — Stage C done (`c241d574`) + Stage D in progress
**Stage C committed** (`c241d574`): InpWriter / GeoPackageReader+Writer /
convert_internal_to_display sourced from side-tables; build() made size-guarded.
**Stage D started (uncommitted):** removed the init-time `build()` mirror call at
SWMMEngine initHydraulics — the side-tables are now populated **only** by the
authoritative parse/resolve/Router::init dual-writes (+ GeoPackage read). **.out
parity stayed EXACT ×18 → this PROVES every dual-write is complete & correct.**
`ensure_built()` is KEPT for now (hand-built unit-test ctx); production no-ops it
(subtype_row already sized by parse).

### Remaining Stage D (the physical cutover)
1. Delete wide subtype arrays from `LinkData.hpp` (decls + resize/grow_to/
   erase_at/shrink_to_fit lines): conduit length/roughness/slope/mod_length/
   barrels/beta/rough_factor/q_full/q_max/loss_*/seep_rate/culvert_code; pump
   curve/init_state/startup/shutoff/curve_type; structure param1/param2/cd/orate/
   crest_height; moved per-step evap_loss_rate/seep_loss_rate/normal_flow_limited/
   inlet_control/full_state. KEEP: xsect_*, has_flap_gate, pump_curve_name, dqdh,
   setting/target_setting/direction/time_last_set, topology, flow/volume state, q0/
   q_limit, comments/tags.
2. Drop the remaining wide dual-writes (LinksHandler, PostParseResolver,
   openswmm_links_impl setters, HydStructures pump_curve_type, Router::init
   mod_length/conveyance, GeoPackageReader, TypeConverter) → side-table-only.
3. Remove build() + ensure_built() + all ensure_built call sites
   (Routing computeConduitLosses/executeSteadyFlow, DW execute/getRoutingStep,
   KW execute, Culvert).
4. **Migrate hand-built-ctx tests** that set wide subtype arrays directly — they
   won't compile once the arrays are deleted. Affected (set ctx.links.<subtype>):
   test_routing, test_gap_fixes (38 ctx), test_quality_routing, test_exfiltration,
   test_lid, test_snow, test_rdii, test_groundwater, test_geopackage (already
   ensure_built'd — but it sets wide arrays, so migrate to set_link_type+rows or
   keep a thin test helper). Compiler will list every site.
5. Update DynamicWave.hpp:270-273 doc comments; full gates; measure peak RSS
   (before=HEAD~Phase6, after) via `/usr/bin/time -l` on a big QA model; squash
   71ad…→ this into one Phase-6 commit.

## (historical) RESUME POINT — `e60b3b36` (Stages A + B COMPLETE)
**Stage B committed.** Mutable per-step conduit state (evap_loss_rate,
seep_loss_rate, normal_flow_limited, inlet_control, full_state) MOVED to new
ConduitData columns (full move — producers+readers+resets); pump_curve_type
authoritative in PumpData (wide dual-write kept); mod_length + lengthened
beta/q_full/q_max authority dual-written in Router::init. Gates: build clean,
**ctest 86/86**, **.out EXACT ×18** (validates the per-step moves bit-for-bit),
`openswmm_links.h` byte-unchanged. **KEY invariant used:** in the DW kernels the
ConduitData row == conduit-tile index `uci` (both ascending-link-order dense), so
per-step CD reads use `[uci]` with no row lookup.

### Remaining (resume here): Stage C → D
- **Stage C** IO (validated by ctest IO tests + INP round-trip, NOT by .out
  parity): repoint InpWriter ([CONDUITS]/[ORIFICES]/[WEIRS]/[OUTLETS]/[XSECTIONS]/
  [LOSSES] + pump curve name) and GeoPackageWriter `write_links` to read the
  side-tables; flip GeoPackageReader `read_links` to set_link_type+populate rows;
  flip PostParseResolver `convert_internal_to_display` (the ~548-555 block) to
  scale side-table length/seep_rate/crest_height. GPKG on-disk schema stays FLAT.
- **Stage D**: delete wide subtype arrays from LinkData.hpp (+ resize/grow_to/
  erase_at/shrink_to_fit lines), drop all remaining wide dual-writes (LinksHandler,
  PostParse, openswmm_links_impl setters, HydStructures pump_curve_type,
  Router::init mod_length, edit-API), remove build() + ensure_built() + their call
  sites, update DynamicWave.hpp:270-273 doc comments, compiler enforces zero wide
  refs; measure peak RSS; squash WIP commits (71ea…/366…/07d…/b9f…/903…/e60… →
  one). KEEP on base: xsect_*, has_flap_gate, pump_curve_name, dqdh, setting/
  target_setting/direction/time_last_set, topology, flow/volume state.

## (historical) RESUME POINT — `903b6524` (Stage A COMPLETE)
**Stage A.2 + A.3 fully committed** (`b9f5e966` reads/edit-API + `903b6524`
parse/resolve dual-writes & pre-build reads). Every link-subtype READ now sources
the side-tables; every WRITER (C-API setters, edit API, parse handlers, resolve)
dual-writes wide+side; the SWMMEngine pre-build init reads are repointed. Gates
green: build clean, **ctest 86/86**, **.out EXACT ×18**, `openswmm_links.h`
byte-unchanged.

### Remaining (resume here): Stage B → C → D
- **Stage B** (mutable per-step state — header change, rebuild ALL targets):
  add ConduitData columns `evap_loss_rate`/`seep_loss_rate`/`normal_flow_limited`
  (uint8)/`inlet_control`(uint8)/`full_state`(int8); MOVE producers+readers+resets
  together (mirror can't track per-step state). `pump_curve_type` → repoint the one
  reader (SWMMEngine ~2332, `PD.curve_type`), drop the mirror write
  (HydStructures ~97). Flip `mod_length` authority: repoint Router::init writes
  (Routing.cpp 143-213) to `CD.mod_length` + delete the non-conduit write. `dqdh`
  STAYS on base.
- **Stage C** IO: InpWriter / GeoPackageReader+Writer / convert_internal_to_display
  (the Stage-C half of PostParse, ~lines 548-555) source the side-tables; GPKG
  on-disk schema stays FLAT (Phase 7 redesigns it).
- **Stage D** delete wide subtype arrays from LinkData.hpp + remove build() AND the
  new ensure_built() guard + their call sites; compiler enforces zero wide refs;
  measure peak RSS; squash WIP commits.

---
## (historical) earlier resume note — `b9f5e966`
**Stage A.2 (all reads) + Stage A.3 (C-API/edit-API half) committed.** Gates green:
build clean (asserts live), **ctest 86/86**, **.out EXACT ×18**, `openswmm_links.h`
byte-unchanged.
- Done: every conduit/pump/orifice/weir/outlet config READ repointed to the
  side-tables (cold paths, C-API getters, Routing/KW/Culvert/Inlet/Outfall,
  DynamicWave per-step via new `tile_roughness_`/`tile_loss_avg_` columns +
  getLinkStep/computeAASkipFlags, SWMMEngine post-build reads); C-API setters
  DUAL-WRITE (wide+side); edit API (`swmm_link_add`→set_link_type, `pop_last`/
  `delete_link`→erase_link, `convert_link`→set_link_type+new-defaults).
- New scaffold: `LinkSubtypes::ensure_built()` lazy mirror guard at solver execute
  entries (for unit tests that drive solvers on hand-built ctx, bypassing engine
  init). Removed with `build()` at Stage D.
- **Gotcha learned:** adding members to `DynamicWave.hpp` changes `DWSolver`
  layout → MUST rebuild ALL targets (`cmake --build <dir>` with no `--target`),
  not just `openswmm_engine openswmm`, or stale test executables hit an ABI skew
  and hang. The handoff's pre/post-`build()` line classification for SWMMEngine is
  unreliable — `init_modules()` (→ build at ~3936) is called at `initialize()`
  line ~581, so reads above 581 (incl. line ~367 wet-well pump_curve) are
  PRE-build and stay on wide; verify against the call site, not file order.

### Remaining (resume here)
- **Stage A.3 rest:** dual-write parse writers `LinksHandler` (set_link_type +
  fields) and `PostParseResolver` (convert_inputs_to_internal,
  recompute_conduit_flow_properties beta/q_full/q_max, slope loop, pump/outlet
  curve resolve); then repoint the deferred SWMMEngine PRE-build reads
  (413/415/423 q0, 512/513 backwater, 567/568 hotstart, 3813 adverse-slope, 367
  wet-well) once the side-table is populated at parse/resolve.
- **Stage B / C / D** as below.

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
