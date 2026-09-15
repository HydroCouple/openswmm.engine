# Water Age Tracking Plan (Network + Watershed + LID + GW)

**Status:** Approved direction, 2026-08-12
**Parent:** `plans/transport/UNIFIED_TRANSPORT_MASTER_PLAN.md` (Phase T3; decision D-UT4)
**Extends:** `plans/LAGRANGIAN_QUALITY_STRATEGY.md` §8 (which covers the routing
network only, in ~8 lines) to the full water pathway, with per-source initial age.
**Distinct from:** `plans/FLOW_TRACE_ENGINE_PLAN_2026-08-03.md` — that is an
offline, steady-approximation travel-time post-processor on the `.out` file.
This plan is in-simulation transported age. Cross-references added to both.

---

## 1. Concept

**Packaging note (2026-08-12):** water-age *transport* is embedded in every
engine — a reserved species slot in the ARD FV-kernel cell state, a reserved
species on LARD segments, a CSTR-mixed quantity under LEGACY, per-zone
species in integrated2d. The separately registered `waterage` component
(D-UT8) is a thin **coordinator**, not a transport engine: it registers the
reserved species, seeds per-source initial ages into the §2 loaders, and
owns the age states that live outside the network engines (subcatchment
ponded/snow/GW, LID layers). This split exists because (a) age must work
under `QUALITY_SOLVER LEGACY` with no ARD/LARD registered, (b) most age
state is hydrology/2D-side, not 1D-engine-side, and (c) one
`[WATER_AGE_SOURCES]` table keeps all engines agreeing on source ages when
`QUALITY_SOLVER` is flipped — no config duplication across engine files.

Water age is a reserved species `__WATER_AGE__` (registry kind
`RESERVED_AGE`, master plan §4.1) with:

- **Zero-order growth**: `d(age)/dt = 1` (rate 1.0 s/s) in every water
  parcel/element/CSTR — the MSX-standard formulation the LARD plan already
  adopts. Under the legacy qualroute engine the equivalent is exact:
  age advances by dt then mixes volume-weighted.
- **Volume-weighted mixing** everywhere water mixes (junctions, storage,
  LID layers, aquifer, 2D cells): `age_mix = Σ V_i·age_i / Σ V_i`. The
  transported quantity is age-volume (see master plan §4.3 tuple), so
  mixing is conservative by construction.
- Enabled by `[OPTIONS] WATER_AGE ON` (LARD plan option, now global across
  engines: LEGACY, EULERIAN_ARD, LAGRANGIAN). Reported as a pseudo-pollutant
  column (units: hours) in `.rpt`/`.out` unless `[REACTION_REPORT]` says
  otherwise.

## 2. Per-source initial age (headline requirement)

Every source pathway gets a configurable initial age (default 0). One
section + API surface — placed in the water-age component's external config
file (`model.age`, registered via `[PROCESS_COMPONENTS]`; only
`WATER_AGE ON|OFF` stays in `[OPTIONS]` — D-UT8, see
`TRANSPORT_IO_PLUGIN_CONFIG_PLAN.md` §3.2):

```
[WATER_AGE_SOURCES]
;;Source           Scope            InitialAge (hours) | TIMESERIES name
RAINFALL           GLOBAL           0.0
DWF                NODE  N12        6.0        ; e.g., upstream WWTP residence
GW                 GLOBAL           720.0      ; old groundwater
GW                 SUBCATCH S3      2400.0     ; per-subcatchment override
RDII               GLOBAL           24.0
EXTERNAL_INFLOW    NODE  N4         TIMESERIES age_ts
IFACE              GLOBAL           0.0
INITIAL_STATE      GLOBAL           0.0        ; water in network at t=0
BOUNDARY_2D        EDGE_BC B1       0.0        ; 2D inflow BCs (Phase T6)
```

Wiring **as delivered** (A1a, `7c322a6c`): each loader in `QualitySolver`
(`addWetWeatherLoads / addRdiiLoads / addDwfLoads / addGwLoads /
addIfaceLoads`, `src/engine/quality/QualityRouting.cpp:100-130`) and the
external-inflow path contributes `age_volume = V_source · age_source(t)` —
seven pathways in all — into `ctx.water_age_state.node_age_vol_in`, a RATE
(age·ft³/s) that is the age analogue of `qual_mass_in` and sits **beside**
`nodes.qual_mass_in / qual_vol_in` rather than inside a widened tuple. This
shape is now the contract (master plan §4.3, D-UT10); heat's enthalpy
channel mirrors it. Hotstart files persist age state (native V3 bump,
`f704b83d`, through `IStateIOPlugin` so plugins stay compatible).

## 3. Watershed (subcatchment) age

Runoff age is tracked on each subcatchment so the network receives aged
runoff rather than age-0 water:

- **Ponded/depression storage:** per-subcatchment age state advancing at
  1 s/s, volume-mixed with incoming rainfall (age = RAINFALL initial age)
  and run-on from upstream subcatchments
  (`subcatchments_set_outlet_subcatchment` routing).
- **Snowpack:** snowmelt inherits snowpack age (pack age advances while
  snow is held; default can be disabled via `WATER_AGE_SNOW OFF` to treat
  melt as fresh).
- **Groundwater/soil:** infiltrated water entering `Groundwater.cpp`
  storage mixes into a per-subcatchment aquifer age state; GW outflow to
  nodes carries it (replaces the fixed `GW GLOBAL` value when the
  subcatchment aquifer age state is enabled: `WATER_AGE_GW_STATE ON`).
- Implementation site: alongside `SWMMEngine::stepSurfaceQuality()`
  (`SWMMEngine.cpp:2136`) in the runoff substep loop; state SoA parallel to
  `surface_quality_`. Washoff handoff extends
  `RunoffInterface.hpp` per-subcatchment vectors so routing interface files
  round-trip age.

## 4. LID modules

Each LID unit tracks age per layer (surface, soil, storage, drain-mat —
state arrays parallel to the moisture states in
`src/engine/hydrology/LID.hpp`):

- Layer age advances at 1 s/s; inter-layer fluxes carry age-volume;
  volume-weighted mixing within each layer store.
- Drain outflow and surface overflow deliver aged water to the receiving
  node/subcatchment; infiltration to native soil delivers age to the
  subcatchment GW age state (§3).
- Note: this is the first constituent state inside LID layers (today only
  `drain_rmvl` removal fractions exist). The state layout is designed so
  Phase T4 heat and future full LID pollutant transport reuse it
  (generic per-layer species-state block sized by the registry).

## 5. Engine-specific behavior

| Engine | Mechanism |
|---|---|
| LEGACY qualroute | age-volume in the load tuples; CSTR mixing at nodes/links; +dt per step; STEADY variant ages by link travel time |
| Eulerian ARD | reserved species slot in the FV-kernel cell state (`cell_phi` lineage) with unit zero-order source via the reaction module; junction complete-mix — one path under all routing models (engine plan rev. 2) |
| LARD | per existing plan §8 (reserved species, r = 1.0); gate G6 applies |
| 2D / GW | Phase T6/T7 — cell age species (see `TWOD_TRANSPORT_PLAN.md` §4.4, §5.4) |

## 6. API

```python
sim.options.water_age = True
wa = sim.water_age
wa.set_source_age("GW", hours=720)
wa.set_source_age("DWF", node="N12", hours=6)
wa.set_source_age("EXTERNAL_INFLOW", node="N4", timeseries="age_ts")
wa.node_age("N12")       # current mean age, hours
wa.link_age("C7")
wa.subcatch_age("S3")    # ponded-water age
wa.lid_age("S3", unit=0, layer="storage")
```

C API `openswmm_water_age.h`; MCP `water_age_*`; parity registries updated
(gate G-UT6). Age columns appear in `analysis_get_time_series` like any
pollutant.

**GUI:** the `[WATER_AGE_SOURCES]` table is edited by `WaterAgeSourcesDialog`
and the options checkboxes live on the "Quality & Transport" page — see
`openswmm.gui/workplans/TRANSPORT_QUALITY_GUI_PLAN_2026-08-12.md` §1, §3.4.
Source-table CRUD needs `*_count` companions and caller-allocated buffers per
the C-API house rules.

## 7. Implementation phases

```
A1  SPLIT: A1a ✅ 2026-08-17 (validated, `7c322a6c`; see
    A1A_VALIDATION_HANDOFF_2026-08-17.md §5) = registry species +
    [WATER_AGE_SOURCES] (GLOBAL + NODE, constant hours) + source tuples
    through all seven loaders + ARD-mesh age (pulled forward from A5 —
    the substrate was ready). Verified: residence-time tracking,
    EXACT 6-h source shift (21600.000 s at ROUTING_STEP ≤ 2), exact
    level-pool aging, symmetric-row bitwise razor. A1a validation also
    exposed the PRIORITY pre-existing ARD external-inflow routing-step
    defect (see roadmap ⚠ row) via the identical-carrier diagnostic —
    the gate decks pin ROUTING_STEP 1 until it lands.
    A1b ✅ 2026-08-17 (validated, `d2f003e6`) = the LEGACY CSTR age
    mirror (routeLegacyAge, faithful line-by-line; no evap factor — §8
    resolved, and validation found the STRONGER reason: legacy's factor
    fires on every flowing node at steady state, hidden by the pollutant
    clamp). Validation contributed the instrument epistemics (roadmap
    lessons 33–35): differencing gates are blind to shared bias; the
    absolute-bias slope (n·dt per element per splitting stage) is the
    ordering observer; dt→0 extrapolation asserts the exact
    residence-time theorem. A5 inputs recorded: ARD's 2.25%
    dt-independent bias residual; cross-engine comparisons must use the
    outfall NODE (link ages differ by DEFINITION). A1a decks unpinned to
    ROUTING_STEP 5.
A2a ✅ 2026-08-17 (validated, `f704b83d`). Hotstart persistence: native
    format V3 carries per-node/per-link age; both engines seed from the
    loaded state. Restart continuity MEASURED rather than assumed: ARD
    −4.4%, LEGACY −1.6% over one step, and the ARD gap is STRUCTURAL —
    the record stores one age per link, the mesh one per cell, so a save
    collapses the within-link profile (roadmap lesson 37: an ARD restart
    is continuous, not bit-continuous). Carries: pollutant hotstart
    restore (absent in both formats — a parity decision) and a
    pre-existing UBSan misaligned CRC load blamed to 4e29c8869.
A2b ✅ 2026-08-17 (validated, `d4889329`). Age reports as a trailing
    `__WATER_AGE__` `.out` species column in HOURS (measured 2.166667 h
    against an analytic 2 h + 10 min). Reported stride (nr) named apart
    from the transport stride (np). **Blocking follow-up before this is
    usable:** no `swmm_output_*` entry point returns species IDs, so a
    consumer cannot tell the hours column from a concentration — the
    name-keyed unit decision needs `swmm_output_get_pollut_id` (roadmap
    carry (a)). Also owed: a manual note on the legitimate LEGACY-vs-ARD
    link-age definition difference (ARD publishes a between-nodes value,
    LEGACY the downstream node's age; nodes agree to <1%).
A2c Age-volume balance row — split out: the `.rpt` continuity table is
    mass-shaped and age neither is a mass nor conserves, so the row needs
    its own definition first.
A3  Subcatchment age (ponded + snow + GW state) + run-on + iface files.
    GW state rides the two-zone PER_SUBCATCH column (master plan G-track,
    G1) if landed; otherwise the GW sub-item is deferred to G1 — no interim
    aquifer-age structure is built twice.
    → verify: two-subcatchment cascade analytical mixing; GW state
      converges to input age under constant recharge.
A4  LID layer age.
    → verify: bioretention column test — drain age ≈ layer HRT chain under
      constant inflow.
A5  Eulerian ARD + FV bindings (with T2), LARD binding (with T5).
    → verify: engine cross-check — same model, 3 engines, ages agree
      within scheme tolerance (G-UT2 analogue); LARD gate G6.
A6  Python/C/MCP surfaces + docs.
```

## 8. Open items

- Default for `WATER_AGE_SNOW` (proposal: ON, pack ages).
- Whether evaporation should concentrate age (it removes volume but not
  age-volume → mean age rises; this is physically defensible — confirm) —
  proposal: evaporation removes volume at the parcel's current age
  (mean age unchanged), consistent with age being a volume-intensive mean.
- Interaction with `FLOW_TRACE_ENGINE` outputs — add a validation notebook
  comparing steady trace travel times vs transported age on a steady model.
