# Phase 6 Continuation Handoff — finishing the relational LINK cutover

## STATUS

### Committed work
- **`71ae12a5`** — Phase 6 foundation + **Stage A.1**. Added `src/engine/data/LinkSubtypes.hpp` (the 5 dense per-subtype side-tables joined to base `LinkData` by an ascending `link_idx` key), wired the `link_subtypes` member into `SimulationContext`, and migrated `StructureSolver::init` (`src/engine/hydraulics/HydStructures.cpp`) to source pump/orifice/weir/outlet **config** from the side-tables. A **temporary** `build(const LinkData&)` mirror copies the wide arrays → side-tables; it is called in `SWMMEngine.cpp:3926`, after `router_.init` (~3763), so `mod_length` + all init-derived conduit fields are final in the mirror before any side-table consumer runs.
- **`366d214f`** (HEAD) — "Phase 6 (WIP): resume plan for the remaining link cutover". The resume doc; no source change beyond it.

### Verified gate results (fresh checkpoint, HEAD = `366d214f`)
- **Build**: OK. Incremental build of `openswmm_engine` + `openswmm` produced zero error/FAILED lines with asserts live (NDEBUG undefined).
- **ctest**: **86/86** passed, 0 failed (matches baseline).
- **.out byte-parity**: **EXACT for all 18** epaswmm5_qa models (CFS+CMS): extran1-4,6,7,9,10; test1-5; user1-5.
- `git status --short` shows NO matches for `LinkSubtypes`/`HydStructures`/`SWMMEngine`/`SimulationContext` → all Phase-6 source files are committed; working tree is clean for those files.
- Notes: the parity script still emits `ld: warning` lines (filter them out); the system date rolled during the run; neither affects gate results.

### Design — settled facts
- **Five side-tables** in `LinkSubtypes.hpp`, joined to base `LinkData` by ascending `link_idx`:
  - **ConduitData**: roughness, length, slope, mod_length, barrels, beta, rough_factor, q_full, q_max, loss_inlet, loss_outlet, loss_avg, seep_rate, culvert_code.
  - **PumpData**: curve, init_state, startup, shutoff, curve_type.
  - **OrificeData**: orifice_type (=wide param1), cd, orate.
  - **WeirData**: weir_type (=wide param1), cd, end_contractions (=wide param2), crest_height.
  - **OutletData**: outlet_type (=wide param1), crest_height, coeff (=wide cd), expon (=wide param2), curve (=wide pump_curve reuse for TABULAR rating).
  - Container API: `subtype_row[]` reverse map; `set_link_type` / `erase_link` / `add_default` / `rebuild_index`; O(1) row accessors `conduit_row(i)` / `pump_row(i)` / `orifice_row(i)` / `weir_row(i)` / `outlet_row(i)` (each returns **-1** if link `i` is not that subtype).
- **xsect-on-base deviation** (intentional, correct, parity-proven): `xsect_*` (all), `has_flap_gate`, and `pump_curve_name` **STAY on base `LinkData`** because they are **shared** across link types — orifices/weirs read `xsect_a_full/y_full/w_max/s_bot`; pump + outlet + conduit-IRREGULAR share `pump_curve_name`. Do **not** move these to ConduitData.
- **dqdh STAYS on base** (audit overturns the original handoff's MOVE suggestion). `dqdh` has no single subtype owner: written by 4 structure types in `HydStructures.cpp` (orifice/weir/outlet/pump, 14 sites) **and** by conduits (`Culvert.cpp:231`), but read only via the non-conduit `nc_idx` loop in `SWMMEngine` `non_conduit_fn` (2256/2280/2327). The DW conduit momentum solver uses a **separate private** array `dqdh_[uj]`, not `links.dqdh`, so the Culvert write to `links.dqdh` is effectively dead at the read sites. Splitting it 4 ways gains nothing and adds parity risk. **Keep on base.**

### Remaining work (the bulk)
Repoint ~161 conduit/structure-field reads/writes off the wide arrays, flip writers authoritative, move the truly-mutable subtype state, handle IO, then DELETE the wide `LinkData` subtype arrays and remove the `build()` mirror. Stages A.2 → A.3 → B → C → D below.

---

## STAGE-BY-STAGE EXECUTION PLAN

Conventions used below:
- `CD` = `ctx.link_subtypes.conduits`, `PD` = `.pumps`, `ORF` = `.orifices`, `WD` = `.weirs`, `OUT` = `.outlets`.
- Row helpers: `cr = ctx.link_subtypes.conduit_row(idx)`, `pr`/`orr`/`wr`/`olr` analogously.
- **Ungated read pattern** (read NOT inside a `type==X` branch): `int cr=conduit_row(idx); v=(cr>=0)?CD.field[cr]:DEFAULT;` — the DEFAULT must match the wide `LinkData::resize`/`add_default` type-default exactly (see PITFALLS).
- **Gated read** (inside `if(type==CONDUIT)` / a `type!=X continue|return`): row is guaranteed ≥0; read it directly, but keep a defensive `cr>=0` to satisfy the asserts-live build.

> **Mirror safety during A/B/C**: `build()` runs once at init (`SWMMEngine.cpp:3926`) AFTER all parse+resolve+`Router::init` writes, so the wide arrays remain authoritative through Stage C. Therefore **getters/reads repointed in A.2/C read a side-table that `build()` keeps in sync — safe**. **Writers repointed in A.3/C must DUAL-WRITE (wide + side-table)** until Stage D deletes the wide arrays and `build()`, otherwise the next `build()` (or an edit-then-get with no re-init) silently clobbers a side-table-only write. This dual-write rule is load-bearing; do not skip it.

### Stage A.2 — conduit/structure READS (getters, reports, controls, hot reads via tiles)
Repoint reads to the side-tables. No writers flip here. Run gates after each file.

**`src/engine/hydraulics/Routing.cpp`** (per-step-hot; one `conduit_row` lookup per conduit is acceptable):
- `503` `length`, `504` `mod_length` (fallback when length≤0) — `computeConduitLosses`, gated by `type!=CONDUIT continue` (489). `cr=conduit_row(j)`; reuse `cr` for 533/548.
- `533` `seep_rate` (`if(CD.seep_rate[cr]>0)`), `548` `seep_rate` (`seep_loss=CD.seep_rate[cr]*width*length`) — reuse `cr`.
- `637` `barrels` — `executeSteadyFlow`, gated by `type!=CONDUIT continue` (601) + DUMMY continue (613): `barrels=max(CD.barrels[cr],1)`.
- `650` `q_full`, `662` `beta`, `683` `mod_length`, `684` `length` (fallback) — same SteadyFlow conduit branch; reuse `cr`.

**`src/engine/hydraulics/DynamicWave.cpp`**:
- **INIT setup loop** (runs once, iterates `conduit_idx_` → all gated): `441` `loss_inlet`, `442` `loss_outlet`, `443` `loss_avg`, `444` `barrels`, `445` `mod_length`, `446` `length` (fallback). `cr=conduit_row(j)`.
- **`refreshConduitTile`** (init, iterates `conduit_idx_`): `606` `length`, `607` `beta`, `608` `q_max`, `609` `rough_factor`, `619` `culvert_code`, `620` `slope`, `622` `loss_inlet`, `623` `loss_outlet`. This is the gather that feeds the hot tile. **ALSO add new tile columns here**: `tile_roughness_[]`, `tile_loss_avg_[]` (see hot reads below). `607` beta-source must run AFTER `Router::init` beta write — it does.
- **Per-step-hot reads that today bypass the tile** — DO NOT add per-step row lookups; add tile columns in `refreshConduitTile` and read `tile_*[uci]`:
  - `1473` `roughness` (classifyMomentumCategories, force-main detection) → `tile_roughness_[uci]`.
  - `1753` `loss_avg` (Manning kernel) → `tile_loss_avg_[uci]` (loss_inlet/outlet already tiled at 1751/1752).
  - `1822` `roughness` (processForceMainLink) → `tile_roughness_[uci]`.
  - `1840` `loss_inlet`, `1841` `loss_outlet` (processForceMainLink) → switch to `tile_loss_inlet_[uci]`/`tile_loss_outlet_[uci]` for consistency (currently read wide here).
  - `1842` `loss_avg` (processForceMainLink) → `tile_loss_avg_[uci]`.
- `1923` `barrels` — updateNodeFlows, inside `type==CONDUIT` (1921): use `tile_barrels_d_[uci]` (already gathered) via `tile_uj_to_ci_[uj]`, or `conduit_row`.
- `1938` `barrels` — updateNodeFlows surface-area scatter, **UNGATED** (loop can be all-links): `int cr=conduit_row(j); barrels=max((cr>=0)?CD.barrels[cr]:1,1)`. **DEFAULT=1.**
- `2054` `crest_height` — computeAASkipFlags, **weir-gated** (`lt==WEIR` at 2053): `int wr=weir_row(j); hcrown=...+WD.crest_height[wr]`.
- `2492` `barrels`, `2500` `length`, `2501` `mod_length` — getLinkStep (adaptive CFL), gated by early-return `type!=CONDUIT` (2488). Prefer the tile via `tile_uj_to_ci_[uj]` (`tile_barrels_d_`, `tile_links_length_`); `cached_length_[uj]` already holds effective length — consider caching `Lscale` in the tile to avoid both 2500/2501 reads.

**`src/engine/hydraulics/DynamicWave.hpp`** — `270/271/272/273` are **comment-only** doc strings on `tile_links_length_`/`tile_beta_`/`tile_q_max_`/`tile_rough_factor_`. Update text to `// ConduitData.<field>`; no code access. **stage: skip (doc).**

**`src/engine/hydraulics/KinematicWave.cpp`** (no conduit tile; one `conduit_row` per conduit per step is acceptable; gated by `type!=CONDUIT continue` 242 + DUMMY 255):
- `279` `barrels`, `285` `q_full`, `288` `beta`, `289` `mod_length`, `290` `length` (fallback). `cr=conduit_row(j)`, reuse.

**`src/engine/hydraulics/Culvert.cpp`** (per-control, gated by `type!=CONDUIT continue` 208):
- `209` `culvert_code`, `215` `slope`. `cr=conduit_row(j)`, reuse.

**`src/engine/hydraulics/Inlet.cpp`** (UNGATED — `li` has no type guard in code even though inlets are conduit-bound):
- `360` `roughness` — `InletSolver::init` else-branch (init): `int cr=conduit_row(li); road_roughness=(cr>=0)?CD.roughness[cr]:0.01`. **DEFAULT=0.01.**
- `401` `slope` — `computeAll` first pass (per-step-hot): `int cr=conduit_row(li); SL=(cr>=0)?CD.slope[cr]:0.0`. **§1-preferred:** cache `SL` into the inlet SoA at init (it's a conduit-config constant) to avoid a per-step lookup; otherwise the guarded read. **DEFAULT=0.0.**

**`src/engine/hydraulics/Outfall.cpp`** (per-step-hot; `link_idx` is ALWAYS a conduit by `buildOutfallLinkMap` invariant):
- `192` `barrels`, `195` `beta`, `195` `q_max`. `int cr=conduit_row(link_idx)` (≥0 by invariant). **§1-preferred:** cache `beta`/`q_max` into the outfall→conduit map alongside `link_idx` to skip the per-step lookup.

**`src/engine/core/openswmm_links_impl.cpp`** — getters/reads (type-GATED pairs return BADPARAM for wrong type, so their row is guaranteed ≥0):
- `258` `param1` (get_orifice_type, orifice-gated): `*type=(ORF.orifice_type[orr]>=0.5)?0:1`.
- `295` `param1` (get_weir_type, weir-gated): `raw=(int)(WD.weir_type[wr]+0.5)`.
- `334` `param1` (get_outlet_rating_type, outlet-gated): `raw=(int)(OUT.outlet_type[olr]+0.5)`.
- `361` `param2` (get_outlet_expon, outlet-gated): `*expon=OUT.expon[olr]`.
- `390` `pump_startup`, `415` `pump_shutoff` (pump-gated): `to_display(..., PD.startup[pr]/PD.shutoff[pr])`.
- `445` `orate` (orifice-gated): `*rate=ORF.orate[orr]`.
- **UNGATED reads — use `(row>=0)?col:DEFAULT`:**
  - `729` `q_full` (get_capacity): `(cr>=0)?CD.q_full[cr]:0.0`. **DEFAULT 0.0.**
  - `912` `q_full` (get_capacities_bulk, per-element): `(cr>=0)?CD.q_full[i]:0.0` — must match scalar `get_capacity` bit-for-bit (parity test pins them).
  - `1004` `pump_curve` (get_pump_curve): dispatch `pr=pump_row`→`PD.curve[pr]`, else `olr=outlet_row`→`OUT.curve[olr]`, else **-1**.
  - `1021` `pump_init_state`: `(pr>=0 && PD.init_state[pr])?1:0`. **DEFAULT 0.**
  - `1044` `crest_height`: `wr=weir_row`→`WD.crest_height[wr]`, else `olr=outlet_row`→`OUT.crest_height[olr]`, else **0.0**.
  - `1061` `cd`: 3-way `orr`→`ORF.cd`, `wr`→`WD.cd`, `olr`→`OUT.coeff`, else **0.0**.
  - `1078` `param2` (get_end_contractions): `wr=weir_row`→`WD.end_contractions[wr]`, else **0.0**.
  - `1103/1104/1105` `loss_inlet/outlet/avg` (get_loss_coeff): `(cr>=0)?CD.loss_*[cr]:0.0`.
  - `1141` `seep_rate`: `(cr>=0)?CD.seep_rate[cr]:0.0`, then `to_display`.
  - `1158` `culvert_code`: `(cr>=0)?CD.culvert_code[cr]:0`. **DEFAULT 0.**
  - `1175` `barrels`: `(cr>=0)?CD.barrels[cr]:1`. **DEFAULT 1 — NOT 0.**
  - `1183` `slope`: `(cr>=0)?CD.slope[cr]:0.0`.
  - **`657` `swmm_link_get_length`** (anchored by audit as the real target; **grep to confirm exact line**): `int cr=conduit_row(idx); L=(cr>=0)?CD.length[cr]:0.0; *length=to_display(LENGTH,L)`. **DEFAULT 0.0.**

**`src/engine/controls/Controls.cpp`** — `getVariableValue` resolves on ANY link (UNGATED, DEFAULT 0.0 for all three to match legacy):
- `487` `q_full` (LINK_FULLFLOW): `(cr>=0)?CD.q_full[cr]:0.0`, then `*ucf_flow`.
- `491` `length` (LINK_LENGTH): `(cr>=0)?CD.length[cr]:0.0`, then `*ucf_len`.
- `493` `slope` (LINK_SLOPE): `(cr>=0)?CD.slope[cr]:0.0`.
- `504` `barrels` (LINK_VELOCITY) — gated (returns MISSING for `type!=CONDUIT` at 496): `CD.barrels[cr]` direct (keep `cr>=0?...:1` defensive).

**`src/engine/plugins/DefaultReportPlugin.cpp`** (all conduit-gated):
- `423` `length`, `424` `slope` (`*100`), `425` `roughness` (Link Summary, `lt==CONDUIT` 420).
- `470` `barrels`, `471` `q_full` (`*Qcf_pre`) (Cross Section Summary, continue 457).
- `1791` `q_full`, `1792` `barrels` (Link Flow Summary, `lt==CONDUIT` 1789).
- `1843` `length`, `1844` `mod_length`/`length` (Flow Classification, continue 1839).

**`src/engine/plugins/DefaultOutputPlugin.cpp`**:
- `317` `length` — `lt==CONDUIT` (316), else-branch writes 0.0f: `CD.length[cr]*ucf_length_`.

**`src/engine/quality/QualityRouting.cpp`** (findLinkQual loops all links, UNGATED):
- `406` `barrels`: `(cr>=0)?CD.barrels[cr]:1`. **DEFAULT 1.** (`407` evap_loss_rate is Stage B — leave on base now.)

**`src/engine/core/SWMMEngine.cpp`** — reads. **ORDERING SPLIT (critical):** some conduit reads happen in `open()` BEFORE the `build()` at 3926; at A.2 the wide arrays are still authoritative so **leave the pre-build reads on the wide arrays for now** — only repoint them in A.3 when writers flip (or hoist build). Post-3926 / step / report reads are safe to repoint in A.2.
- **PRE-build (DO NOT repoint in A.2 — flag, defer to A.3):** `413` barrels / `415` beta / `423` length (q0 loop, 411); `512` barrels / `513` length (q0==0 backwater, 497); `567` barrels / `568` length (hotstart recompute, 562); `3813` slope (adverse-slope validation, 3812).
- **POST-build / step / report (repoint in A.2):**
  - `367` `pump_curve` (type-1 wet-well sizing, pump-gated continue 366): `PD.curve[pr]`.
  - `2180` `orate` (setting-transition loop, **UNGATED**): `int orr=orifice_row(j); orate_sec=((orr>=0)?ORF.orate[orr]:0.0)*3600`. **DEFAULT 0.0** preserves non-orifice→instantaneous transition.
  - `2332` `pump_curve_type` — see Stage B (cheapest win; pump-gated, `pr>=0 && PD.curve_type[pr]==4`).
  - `2509` `barrels` (stats, `type==CONDUIT` 2507); `2545` `mod_length`, `2546` `length`, `2547` `slope` (`type==CONDUIT && y_full>0` 2527).
  - `2738` `barrels` (`type==CONDUIT` 2737).
  - `3012` `barrels` (snapshot velocity, `lt==CONDUIT` 3010); `3272` `barrels` (avg accumulators, `lt==CONDUIT` 3270).
  - `3940` `culvert_code` (culvert_links_ prebuild, `type==CONDUIT && culvert_code>0` 3939) — runs AFTER 3926, safe.

### Stage A.3 — flip WRITERS authoritative + getters; wire incremental edit API
Writers now write the side-table **and keep writing the wide array (DUAL-WRITE)** until Stage D. Repoint the pre-build SWMMEngine reads deferred from A.2 here.

**`src/engine/input/handlers/LinksHandler.cpp`** (parse; each `handle_*` is self-gated). Canonical pattern: replace the bare `ctx.links.type[idx]=<TYPE>` with `ctx.link_subtypes.set_link_type(ctx.links, idx, <TYPE>)` (sets type AND creates the row), capture the row once, write fields through it. Parsing is ascending-idx → end-insert stays O(1).
- `60` CONDUIT: `set_link_type(...,CONDUIT)`; `cr=conduit_row(idx)`. `65` `length`=`CD.length[cr]`; `66` `roughness`=`CD.roughness[cr]`. (`69` q0/q_limit STAY on base — skip.)
- `91` PUMP: `set_link_type(...,PUMP)`; `pr`. `97` `pump_curve`=`PD.curve[pr]=table_names.find(tok[3])` (pump_curve_name line 96 unchanged, stays base); `101` `pump_init_state`=`PD.init_state[pr]`; `103` read it back from the row; `109` `pump_startup`=`PD.startup[pr]`; `111` `pump_shutoff`=`PD.shutoff[pr]`.
- `132` ORIFICE: `set_link_type(...,ORIFICE)`; `orr`. `138` `param1`→`ORF.orifice_type[orr]=(otype=="SIDE")?1.0:0.0`; `143` `cd`→`ORF.cd[orr]`; `147` `orate`→`ORF.orate[orr]`. (`141` offset1, `145` has_flap_gate STAY base.)
- `168` WEIR: `set_link_type(...,WEIR)`; `wr`. `174-177` `param1`→`WD.weir_type[wr]` (0/1/2/3); `180` `crest_height`→`WD.crest_height[wr]`; `182` `cd`→`WD.cd[wr]`; `186` `param2`→`WD.end_contractions[wr]`. (`184` has_flap_gate base.)
- `207` OUTLET: `set_link_type(...,OUTLET)`; `olr`. `210` `crest_height`→`OUT.crest_height[olr]`; `220-221` `param1`→`OUT.outlet_type[olr]`; `223` read back; `232` `cd`(coeff)→`OUT.coeff[olr]`; `236` `param2`(expon)→`OUT.expon[olr]`. (`229` pump_curve_name STAYS base — only the resolved index `OUT.curve` moves, later in PostParse.)
- **Cross-cutting handlers — UNGATED, MUST guard `cr>=0` and no-op otherwise** (reproduces wide TYPE-DEFAULT):
  - `handle_xsections`: `341` `barrels` (`if(cr>=0) CD.barrels[cr]=barrels`), `347` `culvert_code` (`if(cr>=0) CD.culvert_code[cr]=cc`). (`290` xsect_shape stays base.)
  - `handle_losses`: `366` `loss_inlet`, `367` `loss_outlet`, `368` `loss_avg`, `370` `seep_rate` — each `if(cr>=0) CD.<f>[cr]=...`. (`369` has_flap_gate base.)

**`src/engine/input/PostParseResolver.cpp`** (resolve; runs BEFORE build at 3926). Writers flip authoritative; mirror captures final value.
- `convert_inputs_to_internal` (called line 604, runs TOP of resolve): `458` `length` read+write `CD.length[cr]*=inv_len` (conduit-gated, must land before recompute reads); `461` `seep_rate` `CD.seep_rate[cr]*=inv_rain`; `468` `crest_height` **UNGATED** — `wr=weir_row(j); if(wr>=0) WD.crest_height[wr]*=inv_len; else { olr=outlet_row(j); if(olr>=0) OUT.crest_height[olr]*=inv_len; }`.
- `recompute_conduit_flow_properties` (early-returns unless `type==CONDUIT` at 298; capture `cr` once, reuse 304-369): `304` roughness read, `308` slope read, `338` roughness (force-main fallback), `342` `rough_factor` **write**, `346` `beta` **write**, `349` `q_full` **write**, `352` `q_max` **write**, `355` mod_length read, `356` length read (fallback), `357` `mod_length` **write** (placeholder=length since Router::init sets the real value later — see PITFALLS mod_length hazard), `365` length read. (`358` volume stays base.) `342-352` are the single-writer clean flip of beta/rough_factor/q_full/q_max.
- Slope loop / reversal (conduit-gated): `1305` `slope` **write** `CD.slope[cr]=slope`; `1319` `std::swap(CD.loss_inlet[cr],CD.loss_outlet[cr])`; `1320` `CD.slope[cr]=-slope`. `1330` calls recompute (no direct field). Slope loop precedes recompute — keep that order.
- Pump/outlet curve resolution: `792` read `PD.curve[pr]` (pump-gated, `continue` if ≥0); `794` write `PD.curve[pr]=table_names.find(ctx.links.pump_curve_name[uj])`; `802` read `OUT.outlet_type[olr]` (outlet-gated); `804` `if(OUT.curve[olr]>=0) continue`; `806` write `OUT.curve[olr]=table_names.find(...)`. **HAZARD:** mirror splits base `pump_curve` into `PD.curve` (pumps) and `OUT.curve` (outlets) — after flip, write the type-specific row directly.
- Pump unit-division: `816` `if(PD.startup[pr]>0) PD.startup[pr]/=ucf_len`; `818` `if(PD.shutoff[pr]>0) PD.shutoff[pr]/=ucf_len`.
- ELEV→DEPTH crest pass (941-985, `WEIR||OUTLET` 967): `972`/`979` dispatch `wr=weir_row(j)`→`WD.crest_height[wr]` / `olr=outlet_row(j)`→`OUT.crest_height[olr]`. Runs AFTER convert_inputs_to_internal, before build.

**`src/engine/core/openswmm_links_impl.cpp`** — setters. **Keep wide write during A.2/A.3; side-table-only at Stage D.** No `set_link_type` (these never change type).
- `151` `roughness` (UNGATED): `if(cr>=0) CD.roughness[cr]=n`.
- `245` `param1` (set_orifice_type, orifice-gated): `ORF.orifice_type[orr]=(type==0)?1.0:0.0`.
- `281` `param1` (set_weir_type, weir-gated): `WD.weir_type[wr]=(double)type`.
- `322` `param1` (set_outlet_rating_type, outlet-gated): `OUT.outlet_type[olr]=(double)type`.
- `350` `param2` (set_outlet_expon, outlet-gated): `OUT.expon[olr]=expon`.
- `378` `pump_startup`, `403` `pump_shutoff` (pump-gated): `PD.startup/shutoff[pr]=to_internal(...)`.
- `434` `orate` (orifice-gated): `ORF.orate[orr]=rate`.
- **UNGATED overloaded-slot setters (guard each):**
  - `996` `pump_curve`: `if((pr=pump_row)>=0) PD.curve[pr]=v; else if((olr=outlet_row)>=0) OUT.curve[olr]=v;`.
  - `1013` `pump_init_state`: `if(pr>=0) PD.init_state[pr]=(on!=0)?1:0;`.
  - `1035` `crest_height`: `if(wr>=0) WD.crest_height[wr]=v; else if(olr>=0) OUT.crest_height[olr]=v;`.
  - `1053` `cd`: 3-way `orr`→`ORF.cd`, `wr`→`WD.cd`, `olr`→`OUT.coeff`.
  - `1070` `param2` (set_end_contractions): `if(wr>=0) WD.end_contractions[wr]=n;` (weir column ONLY — the side-table fixes the latent param2 collision with outlet expon; confirm no test calls set_end_contractions on an outlet).
  - `1092/1093/1094` `loss_inlet/outlet/avg` (set_loss_coeff): `if(cr>=0){ CD.loss_inlet[cr]=inlet; CD.loss_outlet[cr]=outlet; CD.loss_avg[cr]=avg; }`.
  - `1132` `seep_rate`: `if(cr>=0) CD.seep_rate[cr]=to_internal(...)`.
  - `1150` `culvert_code`: `if(cr>=0) CD.culvert_code[cr]=code`.
  - `1167` `barrels`: `if(cr>=0) CD.barrels[cr]=n`.
  - **`142` `swmm_link_set_length`** (anchored; **grep to confirm exact line**): `if(cr>=0) CD.length[cr]=to_internal(LENGTH,length)`.
- **Edit-API wiring (load-bearing for cutover):**
  - `72` `swmm_link_add`: replace `ctx.links.type[idx]=...` with `ctx.link_subtypes.set_link_type(ctx.links, idx, <TYPE>)` (mirror `swmm_node_add:82`); add a `c_to_internal_link_type` validity guard (mirror swmm_node_add) so an out-of-range type creates no row. **Keep wide write until D.**
  - `92` `swmm_link_pop_last`: after `ctx.links.erase_at(tail)` add `ctx.link_subtypes.erase_link(tail, ctx.links.count())` (mirror `swmm_node_pop_last:112`).

**`src/engine/core/SWMMEngine.cpp`** — repoint the pre-build reads deferred from A.2 here (now that writers are authoritative, or hoist build first — pick one and verify parity): `413/415/423` (q0), `512/513` (backwater), `567/568` (hotstart), `3813` (adverse-slope).

**`src/engine/edit/TypeConverter.cpp`** — make `convert_link` (174) the analogue of `convert_node` (47):
- Insert `ctx.link_subtypes.set_link_type(ctx.links, idx, new_type)` in place of the raw `ld.type[ui]=new_type` at **`236`**, moved to run BEFORE the new-default block (206-234). `set_link_type` erases the old subtype row + adds a default new row.
- Turn `clear_conduit_fields`/`clear_pump_fields`/`clear_structure_fields` into **name-list-only** (lines `119-145`, `154-158`, `167-171` become dead — the row erase drops the data). **KEEP** the `pump_curve_name` clear (`159`) — it's base, not erased by the row drop; keep the IRREGULAR/CUSTOM and `barrels>1` warnings (they read base `xsect_shape`/`barrels`).
- Repoint surviving new-default writes to the freshly-created row (all rows guaranteed ≥0 post `set_link_type`). **Only values that DIFFER from `add_default` seeds need writing**: `208` conduit `CD.roughness[cr]=0.013` (seed 0.01); `228` orifice `ORF.cd[orr]=0.65` (seed 0.0); `232` weir `WD.cd[wr]=3.33` (seed 0.0); `237` outlet `OUT.coeff[olr]=1.0` (seed 0.0); `212` conduit warning reads `CD.length[cr]`. Drop the redundant matches (`210` barrels=1, `219-224` pump curve=-1/init=0/startup=0/shutoff=0/curve_type=-1, `229` orifice_type=0, `233` weir_type=0).

**`src/engine/edit/ObjectDeleter.cpp`**:
- `delete_link` (343): after `ctx.links.erase_at(link_idx)` at **`373`**, add `ctx.link_subtypes.erase_link(link_idx, ctx.links.count())` (mirror `delete_node`'s `erase_node` at 325). Place it before the Step-4 cross-ref renumber block.
- `delete_node` link cascade (281-287, descending order): no change once `delete_link` is fixed — verify the descending sort (281) is preserved.

**DEAD CODE — `src/engine/hydraulics/Link.cpp`** `computeVelocity` (211), `computeFroude` (223), `computeAllConveyance` (252-266) have **no callers** repo-wide (grep clean). `computeAllConveyance` contains a parallel beta/rough_factor/q_full write path that would duplicate `Routing::init`'s authoritative derivation. **Recommend deleting these 3 batch funcs + `Link.hpp` decls (150/161) + `computeConveyance` if it loses its only caller — FLAG TO THE USER FIRST; do not delete unasked.** If revived, they must write the side-table.

### Stage B — move truly-mutable per-step conduit state
Add columns to `ConduitData` (and use `PumpData.curve_type` already present). **Per-step reset writes block deleting the wide array — repoint producers + readers + resets in the same stage.**

**Field decisions (audit-confirmed):**
- **MOVE to ConduitData**: `evap_loss_rate`, `seep_loss_rate`, `normal_flow_limited` (uint8), `inlet_control` (uint8), `full_state` (int8), `mod_length` (already a column; init-derived, not per-step — but its base array dies here).
- **MOVE to PumpData**: `pump_curve_type` → already exists as `PD.curve_type`. Cheapest win: repoint the sole reader, drop the mirror write, delete the base column.
- **KEEP on base**: `dqdh` (see STATUS), plus `setting`, `target_setting`, `direction`, `time_last_set`.

**`pump_curve_type` (do first):**
- `src/engine/hydraulics/HydStructures.cpp:97` — DELETE the mirror write `ctx.links.pump_curve_type[uj]=pumps_.curve_type[uk]` (PD.curve_type already authoritative here).
- `src/engine/core/SWMMEngine.cpp:2332` — repoint read: `int pr=pump_row(j); is_type4_pump=(type==PUMP && pr>=0 && PD.curve_type[pr]==4)`.
- `src/engine/edit/TypeConverter.cpp:157` (clear) and `:220` (init) — become PumpData row lifecycle ops via set_link_type (handled by the A.3 conversion).
- Then delete base `pump_curve_type` column (`LinkData.hpp:351`).

**`evap_loss_rate` / `seep_loss_rate`:**
- Producers (conduit-gated writes): `Routing.cpp:565` `CD.evap_loss_rate[cr]=evap_loss`, `:566` `CD.seep_loss_rate[cr]=seep_loss`.
- Readers: `Routing.cpp:642` (steadyflow, gated); `DynamicWave.cpp:1763`/`1852` (conduit-gated, tile uci context → `CD.*[conduit_row(uj)]`), `:1922` (`type==CONDUIT`); `KinematicWave.cpp:293`; `SWMMEngine.cpp:2740`/`2742` (`type==CONDUIT` 2737); `QualityRouting.cpp:407` **UNGATED** → `int cr=conduit_row(j); (cr>=0)?CD.evap_loss_rate[cr]:0.0`.

**`full_state`:**
- Producers: `KinematicWave.cpp:332` `CD.full_state[cr]=fs`; `Routing.cpp:690` `CD.full_state[cr]=(a>=a_full)?3:0`.
- Reader: `SWMMEngine.cpp:2530` (`type==CONDUIT && y_full>0` 2527) → `CD.full_state[conduit_row(j)]`.

**`normal_flow_limited`:**
- Producers (conduit-gated): `DynamicWave.cpp:1539` (init false), `:1579` (true).
- Reader/reset (`SWMMEngine.cpp` stats, **UNGATED**): `2563` read + `2565` reset → `int cr=conduit_row(j); if(cr>=0 && CD.normal_flow_limited[cr]){++stat; CD.normal_flow_limited[cr]=0;}`.

**`inlet_control`:**
- Producers (conduit-gated): `DynamicWave.cpp:1538` (init false), `:1548` (true); `Culvert.cpp:230` (true).
- Reader/reset (`SWMMEngine.cpp` stats, **UNGATED**): `2567` read + `2569` reset → `int cr=conduit_row(j); if(cr>=0 && CD.inlet_control[cr]){...; CD.inlet_control[cr]=0;}`.

**`mod_length` authority flip (trickiest):**
- `Router::init` (`Routing.cpp`) is the authoritative producer: `143` (lengthening-off, **ungated loop** — `int cr=conduit_row(j); if(cr>=0) CD.mod_length[cr]=CD.length[cr]`), `152` (non-conduit branch — **DELETE** at flip; non-conduits will have no row), `157`/`182`/`184` (conduit branch writes → `CD.mod_length[conduit_row(uj)]`), `201` (conveyance-recompute read). **You MUST repoint Router::init's writes when flipping authority**, else the Courant-lengthened value is lost (the mirror currently saves it only because build runs after Router::init).
- Reads already covered above (A.2/per-report/PostParse 355/357); `DefaultReportPlugin.cpp:1844` is Stage C IO.
- `DynamicWave.cpp:445`/`2501` non-tiled fallbacks → `CD.mod_length[conduit_row(uj)]`; verify the tile-build (`initHydraulics`/`refreshConduitTile`) sources mod_length/length from ConduitData when authority flips.

**`HydStructures.cpp` dqdh writes (298/310/506/519/597/641/665/747/794-798/801/908/945) and `Culvert.cpp:231` dqdh write — NO CHANGE (dqdh stays on base).** Likewise `SWMMEngine.cpp:2256/2280/2327` dqdh nc_idx reads — no change.

### Stage C — IO (.inp + GeoPackage read/write)
These all run BEFORE build at init (readers) or on a deep-copied ctx (writers), so they're parity-safe with no change during A/B; flip them to the side-table here. **The GPKG on-disk schema stays FLAT (column-per-field) — Phase 7 will redesign it.**

**Unit-converted moving fields** (handle in BOTH convert functions, lock-step): `CD.length` (×inv_len/×len, LENGTH), `CD.seep_rate` (×inv_rain/×rain, RAINFALL), `WD.crest_height`+`OUT.crest_height` (×inv_len/×len, LENGTH). **Everything else is stored-internal** (no conversion), incl. pump startup/shutoff (deliberately ctx-native/verbatim).

**`src/engine/input/PostParseResolver.cpp`** convert functions:
- `convert_inputs_to_internal`: `458` `CD.length[cr]*=inv_len` (conduit-gated), `461` `CD.seep_rate[cr]*=inv_rain`, `468` crest_height ungated dispatch (weir+outlet ×inv_len).
- `convert_internal_to_display` (runs on a COPY of ctx — `InpWriter.cpp:497-502`, GeoPackageWriter likewise; `SimulationContext` holds `LinkSubtypes` by value with only `std::vector` members → default copy ctor deep-copies it; **verify no shared pointers**): `555` `CD.length[cr]*=len`, `558` `CD.seep_rate[cr]*=rain`, `562` crest_height dispatch ×len. Keep field-for-field inverse of the input function.

**`src/engine/core/InpWriter.cpp`** (display-converted copy; gated reads):
- `1111` `length`+`roughness` ([CONDUITS], `type!=CONDUIT continue` 1109).
- `1129` `cd` ([ORIFICES], 1127) → `ORF.cd[orr]`.
- `1138` `crest_height`+`cd` ([WEIRS], 1136) → `WD.crest_height[wr]`/`WD.cd[wr]`.
- `1147` `cd`(=`OUT.coeff`)+`param2`(=`OUT.expon`) ([OUTLETS], 1145). (Pre-existing limitation: writer hardcodes FUNCTIONAL, ignores TABULAR — out of Phase-6 scope, do not fix.)
- `1167`/`1177` `barrels` ([XSECTIONS], loop skips only PUMP at 1155 → **ungated for orifice/weir/outlet**): `(cr>=0)?CD.barrels[cr]:1`. **DEFAULT 1.**
- `1183`/`1189`/`1192` `loss_inlet/outlet/avg`, `1193` `seep_rate` ([LOSSES], conduit-gated via `type==CONDUIT &&` / continue 1188).
- **Also flag** `1120` reads `ctx.links.pump_curve` (PUMP-gated) for the table NAME lookup — repoint `tN(ctx, PD.curve[pr])` (pump_curve_name string stays base).

**`src/engine/input/geopackage/GeoPackageWriter.cpp`** `write_links` (all-links ungated bind loop; **DEFAULTS must match wide resize defaults exactly**):
- `452` `barrels` → `(cr>=0)?CD.barrels[cr]:1` (**1**); `453` `culvert_code` → `:0`; `472` `roughness` → `:0.01` (**TRAP: 0.01, not 0.0**); `473` `length` → `:0.0`; `474/475/476` `loss_inlet/outlet/avg` → `:0.0`; `478` `seep_rate` → `:0.0`.
- `487` `pump_init_state` (PUMP-gated, `PD.init_state[pr]?1.0:0.0`); `488` `pump_startup`, `489` `pump_shutoff` (PUMP-gated, NO unit conv — ctx-native).
- `502` `crest_height` (ungated): `wr>=0?WD.crest_height[wr] : olr>=0?OUT.crest_height[olr] : 0.0`.
- `503` `cd` (ungated 3-way): `ORF.cd` / `WD.cd` / `OUT.coeff` / 0.0.
- `504` `param1` (ungated): `ORF.orifice_type` / `WD.weir_type` / `OUT.outlet_type` / 0.0.
- `505` `param2` (ungated): `WD.end_contractions` / `OUT.expon` / 0.0.
- `506` `orate` (orifice-only): `(orr>=0)?ORF.orate[orr]:0.0`.

**`src/engine/input/geopackage/GeoPackageReader.cpp`** `read_links`:
- **PREREQUISITE `420`**: replace the bare `ctx.links.type[idx]=ltype` with `ctx.link_subtypes.set_link_type(ctx.links, idx, ltype)` so the row exists before per-field writes (the reader currently never touches link_subtypes; `ensure_link_capacity` at 129 only grows base/spatial).
- GPKG load **SKIPS** the global convert pass (Reader:490 / Writer:1649) → round-trips display units; write display-unit `CD.length`/`CD.seep_rate`/crest_height with **no per-field conversion** (matches what the writer stored).
- Writes (guarded `if(cr>=0)` for non-conduit no-op): `513` `barrels`, `514` `culvert_code`, `532` `roughness`, `533` `length`, `534/535/536` `loss_inlet/outlet/avg`, `538` `seep_rate`.
- `545` `pump_init_state`, `546` `pump_startup`, `547` `pump_shutoff` (PUMP-gated, verbatim).
- `550` `crest_height` (ungated → weir/outlet row), `551` `cd` (ungated 3-way → ORF.cd/WD.cd/OUT.coeff), `552` `param1` (ungated → orifice_type/weir_type/outlet_type), `553` `param2` (ungated → end_contractions/expon), `554` `orate` (orifice-only).

**Mod_length in report IO**: `DefaultReportPlugin.cpp:1844` (conduit-gated) → `CD.mod_length[conduit_row(j)]/CD.length[cr]`.

### Stage D — delete wide arrays + remove build() mirror
Only after A.2+A.3+B+C are green with dual-writes.
1. In every A.3/B/C **writer**, drop the wide-array write (now side-table-only).
2. `LinkData.hpp` — delete the wide subtype field declarations (and their `resize` lines): conduit (`length`/`roughness`/`slope`/`mod_length`(212)/`barrels`/`beta`/`rough_factor`/`q_full`/`q_max`/`loss_inlet`/`loss_outlet`/`loss_avg`/`seep_rate`/`culvert_code`), pump (`pump_curve`/`pump_init_state`/`pump_startup`/`pump_shutoff`/`pump_curve_type`(351)), structure (`param1`/`param2`/`cd`/`orate`/`crest_height`), and the moved mutable state (`evap_loss_rate`(372)/`seep_loss_rate`(375)/`normal_flow_limited`(385)/`inlet_control`(391)/`full_state`(462)). **KEEP**: all `xsect_*`, `has_flap_gate`, `pump_curve_name`, `dqdh`(397), `setting`, `target_setting`, `direction`, `time_last_set`, node1/node2, offset1/offset2, q0/q_limit/volume/flow/depth/old_* state.
3. Remove the temporary `build(const LinkData&)` mirror from `LinkSubtypes.hpp` and its call at `SWMMEngine.cpp:3926`.
4. In `TypeConverter.cpp`, delete the now-dead `clear_*_fields` field-clear bodies (already name-list-only after A.3).
5. Update the `DynamicWave.hpp:270-273` doc comments to reference ConduitData.
6. Compile must fail on any lingering wide-array reference — fix each; then full gates.

---

## PITFALLS

1. **Non-conduit-default guard (the #1 parity trap).** The wide arrays return TYPE-DEFAULTS for non-subtype links; `<x>_row(idx)` returns -1. Every **ungated** read must become `int r=<x>_row(idx); v=(r>=0)?TBL.field[r]:DEFAULT;` with the DEFAULT matching `LinkData::resize`/`add_default` **exactly**: `barrels`→**1** (never 0), `roughness`→**0.01**, `culvert_code`→**0**, `pump_curve`/`outlet curve`→**-1**, `pump_init_state`→**0/false**, `normal_flow_limited`/`inlet_control`→**false**, everything else→**0.0**. Using 0.0 for roughness/barrels would change .gpkg/.inp bytes for every pump/orifice/weir/outlet row. Specific ungated DEFAULT sites: DynamicWave 1938 (barrels 1); Inlet 360 (roughness 0.01), 401 (slope 0.0); QualityRouting 406 (barrels 1), 407 (evap 0.0); Controls 487/491/493 (0.0); SWMMEngine 2180 (orate 0.0 → instantaneous), 2563/2567 (false); GPKGWriter 452/472 (1 / 0.01); all the openswmm_links_impl getters.

2. **Hot loops keep reading cached groups — DO NOT add per-step `conduit_row` lookups in DW kernels.** The DW conduit hot path consumes the per-conduit **tile** (`tile_links_length_`, `tile_beta_`, `tile_q_max_`, `tile_rough_factor_`, `tile_slope_`, `tile_culvert_code_`, `tile_loss_inlet_/outlet_`, `tile_barrels_d_`, `cached_length_`), gathered once in `refreshConduitTile`. For the per-step wide reads that bypass the tile (DynamicWave 1473/1822 roughness, 1753/1842 loss_avg, 1840/1841 loss_inlet/outlet), **add new tile columns** (`tile_roughness_`, `tile_loss_avg_`) in `refreshConduitTile` and read `tile_*[uci]` — never a per-step row lookup. Loops that aren't conduit-dense-tiled (Routing computeConduitLosses/executeSteadyFlow, KW, Culvert, Outfall, Inlet per-step) MAY take one `conduit_row` lookup per conduit; for Outfall/Inlet the §1-preferred optimization is to cache the conduit constants (beta/q_max into the outfall→link map; slope into the inlet SoA) at init.

3. **`build()` timing vs `Router::init`.** `build()` (SWMMEngine.cpp:3926) runs ONCE at init AFTER `router_.init` (~3763) and after resolve, so the mirror is final before any side-table consumer. Several SWMMEngine init reads (`413/415/423`, `512/513`, `567/568`, `3813`) run in `open()` **before** 3926 — at A.2 leave them on the wide arrays; only repoint when writers flip (A.3) or after hoisting build. Until Stage D, **all flipped writers must dual-write (wide + side-table)**: a side-table-only write before the next `build()` (or an edit-then-get with no re-init) gets silently clobbered.

4. **`pump_curve_type` is written at init, not parse.** `PumpData.curve_type` is authoritative (set at HydStructures init from `pumps_.curve_type`); base `pump_curve_type` is a pure mirror written only at `HydStructures.cpp:97` for the one reader at `SWMMEngine.cpp:2332`. Repoint that read, drop the mirror write, delete the base column.

5. **`mod_length` is derived in `Router::init`, not parse.** PostParse 357 writes only a placeholder (=length, since mod_length is still 0 at resolve). The authoritative Courant-lengthened value comes from `Router::init` (Routing.cpp 143-184), which runs AFTER resolve and BEFORE build. When flipping authority you MUST repoint Router::init's writes to `CD.mod_length` and DELETE the non-conduit write at 152 — else the lengthened value is lost.

6. **GPKG on-disk schema stays FLAT.** The .gpkg keeps its column-per-field layout; Stage C only substitutes the in-memory source/sink. DEFAULTS for in-range non-conduit links must reproduce wide defaults to keep .gpkg byte-identical. Redesigning the on-disk link schema to be relational is **Phase 7**.

7. **Public header `include/openswmm/engine/openswmm_links.h` must stay BYTE-UNCHANGED.** Every repoint is internal storage substitution; no signature/enum/struct/constant change. Its inaccurate doc comments (set_pump_curve "must be SWMM_LINK_PUMP", set_crest_height/set_discharge_coeff "must be SWMM_LINK_WEIR") are already wrong w.r.t. the ungated impl and need not be touched — the refactor does not make them worse.

8. **`dqdh` does NOT move** (overturns the original handoff). Shared across 4 structure types + a dead conduit write; read only via the nc_idx loop. Keep on base.

9. **Anchored-but-unconfirmed line numbers**: `swmm_link_set_length` (~142) and `swmm_link_get_length` (~657) in openswmm_links_impl.cpp were anchored by the audit to the nearest readable line — **grep to confirm the exact lines** before editing. Likewise if any audit lacks a needed detail, grep rather than guess.

---

## GATES (run at EVERY stage boundary; all must stay green)

```sh
# 1. Build (asserts live, NDEBUG undefined)
cmake --build /Users/calebbuahin/Documents/Projects/cbuahin_github/openswmm.engine/build-arm64-osx \
  --target openswmm_engine openswmm

# 2. ctest (baseline 86/86)
cd /Users/calebbuahin/Documents/Projects/cbuahin_github/openswmm.engine/build-arm64-osx && ctest -j8

# 3. .out byte-parity across 18 epaswmm5_qa models (CFS+CMS) — must print EXACT for all
zsh /Users/calebbuahin/Documents/Projects/cbuahin_github/openswmm.engine/docs/relational/qa_runs/phase_6/p6_parity.sh
#   (filter out 'ld: warning' lines)
```

Additional gates per stage:
- **INP round-trip** (Stage C): write the model back out and diff the emitted .inp against a baseline; bytes must match field-for-field.
- **C-API edit-then-get** (Stage A.3 / B): for each repointed setter/getter pair (orifice_type, weir_type, outlet_rating_type, outlet_expon, pump startup/shutoff, orate, pump_curve, crest_height, cd, end_contractions, loss_coeff, seep_rate, culvert_code, barrels, roughness, slope, length) — set by index then get by index in OPENED state, assert the round-trip is exact. The ctest suite already pins scalar `get_capacity` ≡ bulk `get_capacities_bulk`.
- **Header-unchanged check**: `git diff --exit-code include/openswmm/engine/openswmm_links.h` must report no changes.

---

## RESUME COMMANDS (appendix)

```sh
# Confirm checkpoint
cd /Users/calebbuahin/Documents/Projects/cbuahin_github/openswmm.engine
git log --oneline -3            # expect HEAD = 366d214f, Stage A.1 = 71ae12a5
git status --short              # expect clean for LinkSubtypes/HydStructures/SWMMEngine/SimulationContext

# Rebuild + re-verify the three gates
cmake --build build-arm64-osx --target openswmm_engine openswmm
( cd build-arm64-osx && ctest -j8 )                 # expect 86/86
zsh docs/relational/qa_runs/phase_6/p6_parity.sh    # expect EXACT x18 (ignore 'ld: warning')

# Header byte-unchanged check (run after every stage)
git diff --exit-code include/openswmm/engine/openswmm_links.h

# Measure RSS before/after the refactor (definition-of-done evidence):
#   run a large QA model under /usr/bin/time -l (macOS) and record 'maximum resident set size'
#   at HEAD=366d214f (baseline) and again after Stage D.
```

**Final squash** (after Stage D is green and RSS measured), collapse the two Phase-6 WIP commits into one clean commit:

```sh
git rebase --onto 71ae12a5~1 71ae12a5~1 swmm6_rel
# then squash 71ae12a5 (foundation+A.1) + 366d214f (resume doc) + all stage commits
# into a single "Phase 6: relational in-memory LINK side-tables" commit.
# Practically: git reset --soft 71ae12a5~1 && git commit  (interactive rebase is unavailable here).
```
End the commit message with:
`Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`

---

## DEFINITION OF DONE

Phase 6 is complete when: (1) **zero wide-array subtype references remain** — `LinkData.hpp` no longer declares any conduit/pump/orifice/weir/outlet config field or the moved mutable state (`evap_loss_rate`/`seep_loss_rate`/`normal_flow_limited`/`inlet_control`/`full_state`/`mod_length`/`pump_curve_type`), keeping only the shared/base fields (`xsect_*`, `has_flap_gate`, `pump_curve_name`, `dqdh`, `setting`/`target_setting`/`direction`/`time_last_set`, topology and flow/volume state), and the temporary `build()` mirror plus its 3926 call site are deleted; (2) **full QA parity holds** — build is clean with asserts live, ctest is 86/86, all 18 epaswmm5_qa models print EXACT byte-parity, the INP round-trip and C-API edit-then-get gates pass, and `openswmm_links.h` is byte-unchanged; (3) **peak RSS is measured before/after** on a large model and recorded in the commit message as the memory-density evidence; and (4) the two WIP commits are squashed into one Phase-6 commit. The next milestone, **Phase 7**, redesigns the **on-disk GeoPackage link schema** to a relational layout (the in-memory cutover delivered here is its prerequisite).
