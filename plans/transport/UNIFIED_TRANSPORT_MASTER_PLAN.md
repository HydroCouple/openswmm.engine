# Unified Transport Master Plan

**Status:** Approved direction, 2026-08-12 (decisions recorded from planning session with C. Buahin)
**Step-level tracking:** `plans/transport/IMPLEMENTATION_ROADMAP.md` (living
document — every step across all tracks with live status; created
2026-08-16 with E0/E1 validated and E2 in validation). The phase tables in
§5 below define scope; the roadmap orders and tracks execution per the
2026-08-16 sequencing decisions (1D transport → HydroCouple → 2D →
groundwater, G0 sign-off early, LARD parallel-eligible after R3).
**Owner docs in this suite:**

| Document | Scope |
|---|---|
| `plans/transport/UNIFIED_TRANSPORT_MASTER_PLAN.md` (this doc) | Architecture, HydroCouple 2.0 adoption, engine inventory, sequencing |
| `plans/transport/EULERIAN_ARD_TRANSPORT_PLAN.md` | Solver-agnostic Eulerian full-ARD engine for the 1D network |
| `plans/transport/MULTISPECIES_REACTIONS_MSX_PLAN.md` | Shared EPANET-MSX-convention reaction module + Python API |
| `plans/transport/WATER_AGE_TRACKING_PLAN.md` | Water age across network, watershed, LID, GW; per-source initial age |
| `plans/transport/HEAT_TRANSPORT_PLAN.md` | 1D heat transport per HydroCouple CSH/HTS/RHE implementations |
| `plans/transport/TWOD_TRANSPORT_PLAN.md` | 2D surface + planned groundwater ARD, reactions, and heat |
| `plans/transport/TRANSPORT_IO_PLUGIN_CONFIG_PLAN.md` | `[PROCESS_COMPONENTS]` registration + external per-component config files (D-UT8) |

**Companion existing docs (updated with cross-references, not superseded):**
`plans/LAGRANGIAN_QUALITY_STRATEGY.md` (LARD), `plans/LAGRANGIAN_QUALITY_API_STRATEGY.md`,
`plans/EXPLICIT_FV_KOKKOS_1D_SOLVER_PLAN.md` (FV in-solver AD), `plans/PLUGIN_SDK.md`,
`plans/2d/2dModelStrategy.md`, `plans/TWO_ZONE_GROUNDWATER_FV_INTEGRATION_PLAN.md`,
`plans/FLOW_TRACE_ENGINE_PLAN_2026-08-03.md`, `plans/PROCESS_MODULARIZATION_PLAN.md`.

---

## 1. Audit findings this plan responds to (2026-08-12)

An audit of `src/` confirmed the following. Every statement below was verified against
source, not inferred from plan documents.

1. **There is no unified Eulerian ARD implementation.** The only
   advection–dispersion code is embedded in the explicit FV hydraulic solver
   (`src/engine/hydraulics/fv/ExplicitFvSolver.cpp`: `reconstructScalars()` ~:619,
   Zalesak FCT `limitSpeciesFluxes()` ~:786, HLLC-consistent `speciesFlux()` ~:809,
   implicit dispersion `dispersionSolve()` ~:2173). It is **dead code in production**:
   `Router::initFv()` sizes species to zero (`src/engine/hydraulics/Routing.cpp:844`,
   `fv_state_.resize(nc, nn, 0)`) and emits `WARN_FV_OPTION_INERT` for
   `FV_DISPERSION` / `FV_SCALAR_SCHEME`. It has **no reaction term** (AD, not ARD),
   no junction mixing/BC for species, and is FV-mesh-specific.
2. **All four routing models (STEADY, KINWAVE, DYNWAVE, FV) run legacy CSTR
   quality routing** — `quality::QualitySolver` (`src/engine/quality/QualityRouting.cpp`),
   called unconditionally from `SWMMEngine::routingStep()`. First-order decay only.
3. **LARD does not exist in code.** `plans/LAGRANGIAN_QUALITY_STRATEGY.md` is a
   draft; the `QUALITY_SOLVER` option it proposes appears nowhere in `src/`,
   `include/`, or `python/`.
4. **No MSX-style multispecies machinery** anywhere. The only expression
   infrastructure is the node-treatment RPN VM (`src/engine/quality/Treatment.cpp`).
5. **No water age** (only storage-node HRT, `NodeData.hpp` `hrt`), **no heat/water
   temperature transport** (air temperature forcing only), **no 2D quality/heat**,
   **no LID-internal or groundwater transport**.
6. **The plugin SDK is I/O-only** (`PluginType { INPUT, OUTPUT, REPORT, STATE_IO }`,
   `include/openswmm/plugin_sdk/IPluginComponentInfo.hpp`). It is "loosely inspired
   by" HydroCouple but implements none of its interfaces. There is no plugin type
   for process/transport/reaction components.

## 2. Approved architectural decisions

**D-UT1 (amended 2026-08-12) — Solver-agnostic Eulerian ARD engine built on
the existing 1D FV transport implementation.** The unified Eulerian engine is
a standalone component in the architectural sense (own lifecycle, HydroCouple
surface, selectable under any routing model), but its **numerical core reuses
the transport machinery already implemented and verified inside the FV
hydraulic solver** (`src/engine/hydraulics/fv/`): the cell mesh from
`NetworkMeshBuilder`, HLLC-contact-consistent species fluxes, UPWIND /
MUSCL+limiter / QUICKEST-ULTIMATE reconstruction, Zalesak FCT limiting, and
the implicit per-chain dispersion solve (D-FV1). Under `FLOW_ROUTING FV` the
engine consumes the solver's own face mass fluxes directly; under
STEADY/KINWAVE/DYNWAVE a projection layer maps the solver's link/node fields
onto the same cell mesh and reconstructs discretely-consistent face fluxes
(§4.2, and Eulerian plan §3.2). One transport code path serves all four
routing models.

**D-UT7 — Conflict precedence: existing FV implementation over HydroCouple
guidance.** The HydroCouple stack (CSHComponent et al.) remains the guide for
component architecture, exchange semantics, and physical flux formulations
(heat sources, Fischer dispersion coefficient model). Wherever its numerical
conventions conflict with the in-tree FV transport implementation, **the FV
implementation wins**. Recorded consequences: advection schemes are the FV
set (UPWIND / MUSCL / QUICKEST-ULTIMATE with FCT), not CSH's
upwind/central/hybrid/TVD menu; face upwinding follows the HLLC contact-speed
convention, not IDW central interpolation; dispersion is integrated implicitly
per D-FV1, and CSH's numerical-dispersion subtraction (its eq. 4.19 correction)
is **dropped** — higher-order reconstruction + FCT is the anti-diffusion
mechanism; explicit subcycled time marching per the FV substep controller, not
CSH's per-element ODE-solver menu (RK4/RKQS/ADAMS/BDF).

**D-UT2 — Adopt HydroCouple 2.0 interfaces directly.** HydroCouple 2.0
(header-only, MIT; `HydroCouple/HydroCouple/include/hydrocouple.h`,
`hydrocoupletemporal.h`, `hydrocouplespatial.h`, `hydrocouplespatiotemporal.h`;
version 2.0.0 per its `CMakeLists.txt` and `CITATION.cff`) becomes a build
dependency (vcpkg port or CMake `FetchContent`). New transport, reaction, heat,
and (later) groundwater modules implement `IModelComponent` /
`IModelComponentInfo` / `IArgument` / `IInput` / `IOutput` / `IAdaptedOutput`
so that the exact same binaries are loadable both (a) in-process by the
openswmm engine loop and (b) in HydroCoupleComposer compositions alongside
existing components (SWMMComponent, CSHComponent, GWComponent, ...).

**D-UT3 — One shared reaction module.** A single solver-agnostic multispecies
reaction system following EPANET MSX conventions serves the Eulerian ARD engine,
LARD, the legacy-parity `QualitySolver` (qualroute), the FV in-solver transport,
and 2D transport. No engine reimplements chemistry. Configurable from the Python
API. See `MULTISPECIES_REACTIONS_MSX_PLAN.md`. This supersedes the
LARD-plan assumption that the reaction system is owned by `quality/lard/`.

**D-UT4 — Water age in both Eulerian and Lagrangian engines** (and in the
legacy-parity qualroute path), as a reserved species with per-source initial
age, carried through subcatchments, LID layers, and groundwater. See
`WATER_AGE_TRACKING_PLAN.md`.

**D-UT5 — Heat is a transported state, not a pollutant hack.** Temperature is
transported by the same ARD/LARD machinery as species (units °C, energy
accounting in J), with source terms (radiative, latent, sensible, sediment,
GW) supplied by dedicated flux modules modeled on the HydroCouple
CSHComponent / HTSComponent / RHEComponent implementations (Buahin, Neilson &
Horsburgh 2019, *Channel and Sub-Surface Solute and Heat Transport Modeling
Using the HydroCouple Component-Based Modeling Framework*). See
`HEAT_TRANSPORT_PLAN.md`.

**D-UT6 — Quality engine selection.** `[OPTIONS] QUALITY_SOLVER` becomes a
three-way enum: `LEGACY` (default; current `QualitySolver`), `EULERIAN_ARD`
(the new component), `LAGRANGIAN` (LARD). This widens the two-way enum in
`LAGRANGIAN_QUALITY_STRATEGY.md` §3.2. Reactions, water age, and heat are
available under all three (legacy path gets reactions applied per
node/link CSTR volume, ARD/LARD get them per cell/segment).

**D-UT8 — External per-component configuration files (user decision
2026-08-12).** The legacy `.inp` is not polluted with transport
configuration bodies. Each process component — built-in or third-party
HydroCouple — is registered by one line in a new `[PROCESS_COMPONENTS]`
section (modeled on `[PLUGINS]`) whose `config="…"` argument names the
component's own `.inp`-dialect configuration file, delivered as its
HydroCouple `File`-type `IArgument`. The same file drives the component
under HydroCoupleComposer. Only engine-selection/coarse toggles
(`QUALITY_SOLVER`, `WATER_AGE`, `HEAT_TRANSPORT`, `2D_TRANSPORT`) stay in
`[OPTIONS]`. Path/override/error rules mirror the proven
`[2D_MESH_FILE]` pattern. Full design:
`plans/transport/TRANSPORT_IO_PLUGIN_CONFIG_PLAN.md` — its §3.2 table is
the source of truth for section placement; `[REACTION_*]`, `[HEAT_*]`,
`[WATER_AGE_*]`, `[TRANSPORT_*]`, `[2D_TRANSPORT_*]`, `[GW_TRANSPORT_*]`
sections referenced elsewhere in this suite live in the component files,
not the legacy input.

**D-UT9 (amended 2026-08-12) — one unified surface–subsurface 2D
HydroCouple component with transport and heat folded in.** The 2D surface
router and the two-zone groundwater model **share the same triangular mesh**
and are tightly coupled (infiltration, Dunne saturation-excess, return
flow, capillary-rise ET), so they are packaged as a **single**
`PROCESS_COMPONENT` — `org.hydrocouple.openswmm.integrated2d` — solving
surface local-inertial hydrodynamics, two-zone subsurface flow, and the
full solute/heat/age transport for all three domains (surface cells, unsat
columns, saturated zone) behind one component id and **one config file**
(`model.i2d`, D-UT8) that folds in the mesh, all parameterization,
transport, and heat. This is the Qu & Duffy (2007) integrated kernel as
one component, and the HydroCouple GWComponent precedent (flow + heat +
solute in one component) taken to its mesh-sharing conclusion. Internal
capability toggles (`SURFACE_ROUTING`, `GROUNDWATER`, `TRANSPORT`, `HEAT`
in its `[2D_OPTIONS]`-lineage section) let users run surface-only,
groundwater-only (including the `PER_SUBCATCH` degenerate mode, which
needs no surface mesh), or fully integrated configurations without
changing components. There are **no** separate 2D-transport, GW, or
GW-transport components. Chemistry remains shared: the component calls the
common reaction module (D-UT3) and species registry (§4.1). It exposes
HydroCouple exchange items (coupling fluxes, per-domain state) so the same
binary composes in HydroCoupleComposer; in-process, the engine drives it
through the same lifecycle as today's 2D stepping.

## 3. Component architecture (HydroCouple 2.0)

### 3.1 New plugin type and directory layout

Extend the plugin SDK with a fifth plugin type:

```
PluginType::PROCESS_COMPONENT   // HydroCouple IModelComponentInfo-backed
```

`src/engine/plugins/PluginFactory.cpp` gains discovery of libraries exporting
`hydrocouple_component_info()` (returning `IModelComponentInfo*`) in addition
to the existing `openswmm_plugin_info` export. Existing I/O plugin types are
untouched.

```
src/engine/transport/                     # new top-level module
  HydraulicsFieldProvider.{hpp,cpp}       # §4.2 adapter over solver state
  fvkernels/                              # species advection/FCT/dispersion kernels
                                          #   promoted from hydraulics/fv/ (D-UT1 amended;
                                          #   ExplicitFvSolver re-points to these)
  components/
    EulerianArdComponent/                 # plans/transport/EULERIAN_ARD_TRANSPORT_PLAN.md
    ReactionModule/                       # plans/transport/MULTISPECIES_REACTIONS_MSX_PLAN.md
    HeatFluxModules/                      #   RadiativeExchange, SurfaceExchange (latent+sensible),
                                          #   SedimentExchange   (plans/transport/HEAT_TRANSPORT_PLAN.md)
    WaterAge/                             # plans/transport/WATER_AGE_TRACKING_PLAN.md
    Integrated2dComponent/                # D-UT9 unified: 2D surface hydrodynamics + two-zone
                                          #   subsurface flow + solute/heat/age transport for all
                                          #   domains on the shared mesh (wraps engine/2d/ solvers
                                          #   + two-zone kernel; TWOD_TRANSPORT_PLAN.md)
src/engine/quality/lard/                  # unchanged home for LARD (existing plan)
external/hydrocouple/                     # FetchContent/vcpkg: HydroCouple 2.0 headers
```

### 3.2 Exchange items

Each component exposes HydroCouple exchange items; the engine's in-process
composition wires them the same way HydroCoupleComposer would:

- `EulerianArdComponent` **inputs:** network geometry
  (`INetworkComponentDataItem`), per-element hydraulics time series
  (flow, area, depth, width, velocity — `ITimeSeriesComponentDataItem` per
  element), lateral mass/heat sources, boundary conditions.
  **outputs:** per-element concentration/temperature/age series, mass-balance
  ledger items.
- `ReactionModule` is consumed as a library-style sub-component (direct call
  into the bytecode VM from transport kernels — the HydroCouple pull loop is
  NOT in the per-cell integrator hot path; see D-R6 in the reactions plan).
- Heat flux modules **inputs:** met forcing (air temp, RH, wind, pressure,
  shortwave), water temperature, geometry (width, depth); **outputs:** J_sn,
  J_atm, J_br, J_lc, J_e, J_c per element (W/m²) — mirroring RHEComponent.
- MPI hooks (`IModelComponent::mpi*`) are stubbed (single process) in v1.

### 3.3 Lifecycle mapping

Engine loop ↔ HydroCouple state machine: engine `open` → `initialize()`
(Created→Initialized), engine start → `validate()` + `prepare()`
(→Updated), each routing step → `update({required outputs})`, engine `close`
→ `finish()`. In-process the engine is the driver; under Composer the pull
mechanism drives `update()`. The component must not assume call ordering
beyond the HydroCouple contract.

## 4. Unification contracts

### 4.1 The species registry (single source of truth)

One registry owns all transported constituents, replacing the implicit
"pollutant index" convention:

```
SpeciesKind { POLLUTANT, RESERVED_AGE, RESERVED_TEMPERATURE, MSX_BULK, MSX_WALL }
```

Legacy `[POLLUTANTS]` entries auto-register as `POLLUTANT` (with kdecay
translated to an implicit first-order rate expression). The registry feeds
the Eulerian ARD engine, LARD, qualroute, FV `n_species`, 2D transport, and
the output/reporting layer. Lives in `src/engine/data/` beside
`ReactionData.hpp` (per the LARD plan's layout).

### 4.2 HydraulicsFieldProvider

A read-only adapter over `SimulationContext` publishing, per link and node
(and per FV cell when `FLOW_ROUTING FV`): `Q, v, A, y, B, V, V_old, slope`,
plus decomposed node inflows from `PROCESS_MODULARIZATION_PLAN.md`
(`runoff_inflow, gw_inflow, dwf_inflow, rdii_inflow, ext_inflow,
iface_inflow`) each **with an attached concentration/temperature/age vector**
(see §4.3). All hydraulic solvers already publish these quantities
(`publishFv` writes the same link/node arrays DW does), which is what makes
the standalone component solver-agnostic. Exposed both as plain SoA spans
(hot path) and as HydroCouple exchange items (composition path).

### 4.3 Source attribution contract

Every inflow pathway that today carries `(mass, volume)` pairs into
`nodes.qual_mass_in / qual_vol_in` (see `QualitySolver::addWetWeatherLoads`
etc., `QualityRouting.cpp:100-130`) also carries an age-volume and (with
heat) an enthalpy channel. This is the single seam that lets water age and
heat ride every existing source: wet weather, RDII, DWF, GW, interface
files, external inflows. Details in the age and heat plans.

**D-UT10 (decided 2026-08-17, user): the channels are PARALLEL per-capability
accumulators, not a widened tuple.** Rev. 1 of this section specified a
`(mass, volume, age_volume, enthalpy)` tuple. A1a (`7c322a6c`) shipped the
age channel differently and the decision follows what shipped:
`water_age_state.node_age_vol_in` is a rate (age·ft³/s) — the age analogue
of `qual_mass_in` — filled by the same seven loader pathways and living
*beside* `nodes.qual_mass_in / qual_vol_in`, not inside them.

Why the shipped shape wins:

- It is validated and in service; widening the tuple now would rewrite
  working, gated age code for form alone (CLAUDE.md §3).
- Each capability's state stays in its own struct (`WaterAgeData.hpp`), so a
  build without heat carries no enthalpy array and a deck without
  `WATER_AGE` carries no age array.
- **The guarantee §4.3 exists to make is untouched.** What matters is that
  every inflow pathway contributes its channel *at the same seam*; the seam
  is the LOADER SET, not the C++ type. A parallel accumulator filled in the
  same five loaders plus external inflows satisfies the contract exactly.

**Consequence for heat:** enthalpy follows the same shape — a
`node_temp_vol_in` rate accumulator in the heat state struct, filled by the
same loaders — and landed **with H1** (`4767aabb`) rather than as a separate
preparatory step. See the roadmap's T0b row.

**Addendum (H2, `221c5dac`): the accumulator was NOT rescaled to J/s, and
will not be.** H1's handoff §3.1 promised that H2 would convert it once
ρw·cp became load-bearing. H2 converted the FLUX into the accumulator's
units instead: the constants set the flux's weight against thermal mass
either way, so they are equally observable (H2 gate 4 measures the cp law to
2e-4), while one function changes rather than seven loader sites, the mixing
stage and nine H1 gates. Recorded here because the promise is otherwise a
conversion someone will go looking for and not find.

**Scope note:** this decision is about the *loader* seam only. The 1D↔2D
exchange tuple in `TWOD_TRANSPORT_PLAN.md` (§4, §5) is a different object —
a coupling payload, not a loader accumulator — and is unaffected. Revisit it
on its own terms when Phase 3 starts.

## 5. Sequencing and dependencies

```
Phase T0  HydroCouple 2.0 dependency + PROCESS_COMPONENT plugin type
          + species registry (§4.1) + HydraulicsFieldProvider (§4.2)
   → verify: engine builds with headers; registry unit tests; provider
     parity test — fields identical to solver-internal values across all
     4 routing models on tests/parity models.
Phase T1  Shared reaction module (MSX conventions) + Python API
          [MULTISPECIES_REACTIONS_MSX_PLAN.md]                 ── blocks T2–T5
Phase T2  Eulerian ARD component (advection+dispersion+reactions, junction
          mixing, BCs) wired for STEADY/KW/DW/FV
          [EULERIAN_ARD_TRANSPORT_PLAN.md]
Phase T3  Water age (registry species + per-source initial age + watershed/
          LID/GW propagation) on qualroute + Eulerian ARD
          [WATER_AGE_TRACKING_PLAN.md]
Phase T4  Heat transport (1D) — flux modules + TEMPERATURE species
          [HEAT_TRANSPORT_PLAN.md]
Phase T5  LARD implementation per existing plan, now consuming the shared
          reaction module and species registry; age + heat in LARD
          [LAGRANGIAN_QUALITY_STRATEGY.md as amended]
Phase T6  2D surface transport (ARD + reactions + heat) + 1D↔2D species/
          enthalpy coupling channel                            ── after T2, T4
Phase T7  Groundwater transport on the spatially explicit two-zone aquifer
          (per-triangle unsat + saturated species/heat/age state, inter-zone
          and channel exchange tuples; saturated-zone discretization per
          GWComponent) — lands with TWO_ZONE_GROUNDWATER_FV work
          [TWOD_TRANSPORT_PLAN.md §5; TWO_ZONE_GROUNDWATER_FV_INTEGRATION_PLAN.md]
```

T1 before T2 because the ARD component's cell update calls the reaction VM;
T5 (LARD) is independent of T2 once T1 exists and may proceed in parallel.

**Groundwater track (decision 2026-08-12): start the spatially explicit
two-zone module early, as a parallel track — not as a blocker ahead of
T0–T2.** Rationale: it is the longest-lead item (16 open sign-off
decisions, explicit-marcher restatement pending), D-UT9 means the
integrated2d component is designed around both domains at once (landing
subsurface flow before the 2D transport phases avoids retrofitting), its
flow feedbacks (Dunne, return flow, physically-based RDII/exfiltration)
deliver value independent of transport, and the age/heat subcatchment GW
states are its PER_SUBCATCH degenerate mode — built once on the real
kernel if it lands first.

```
G0  ✅ DELIVERED 2026-08-15 — detailed restated plan:
    `plans/TWO_ZONE_GROUNDWATER_EXPLICIT_LTS_PLAN_2026-08-15.md`
    (fully discrete explicit LTS on the surface-marcher pattern; two
    production closures: closed-form quasi-steady + σ-based explicit
    vertical discretization of the unsaturated column; runtime tier-count
    extension; integral with integrated2d). Remaining before code:
    sign-off on its §10 open decisions (D-N1..N5 + carried-over items).
G1  Steps 1–8 of that plan: closures A+B standalone, σ-column ALE gates,
    ET boundary, PER_SUBCATCH mode + node Darcy exchange (one two-zone
    column per subcatchment, selectable alternative to legacy GWSolver).
    → verify: that plan's gates 1–8; age/heat plans' §3 GW states ride
      this (no interim aquifer age/temperature structures built twice).
G2  Steps 9–22: lateral Darcy + LTS tier integration on the shared mesh
    inside integrated2d, feedback mechanisms, benchmarks, API/GUI/docs.
    → verify: that plan's gates 9–22 incl. tier-boundary conservation and
      the αL closure-crossover benchmark.
T7  GW transport rides G2 (per-σ-layer species state under closure B).
```

Main track T0→T4 proceeds concurrently; A3 (subcatchment GW age) and H5
(GW source temperatures) consume G1 if it has landed, else defer their GW
sub-items rather than building interim structures twice.

## 6. Cross-cutting verification gates

- **G-UT1 (no-regression):** with `QUALITY_SOLVER LEGACY`, bit-identical
  quality results vs current `swmm6_rel` on the full parity suite.
- **G-UT2 (solver uniformity):** Eulerian ARD produces mesh-converged,
  mutually consistent results for the same model run under STEADY, KINWAVE,
  DYNWAVE, and FV hydraulics (tolerance band documented per test).
- **G-UT3 (analytical):** advection–dispersion vs Taylor (1953) /
  Chapra analytical solutions; reaction vs MSX reference examples (as5,
  nh2cl, dbp, lead — reusing LARD plan gate G3's cases); heat vs
  CSHComponent published validation cases from the 2019 HydroCouple report.
- **G-UT4 (conservation):** exact species mass / energy / age-volume
  conservation in closed systems, including flow reversal, across every
  engine.
- **G-UT5 (composition):** `EulerianArdComponent` loads in
  HydroCoupleComposer and reproduces the in-process result when coupled to
  SWMMComponent on a benchmark network.
- **G-UT6 (API parity):** every new C API lands in
  `plans/parity/{c_funcs,py_methods,mcp_tools}.tsv` with Python + MCP
  coverage per the established gap-fill process.

## 7. GUI configuration (companion plan)

All user-facing configuration for this suite is planned in
`openswmm.gui/workplans/TRANSPORT_QUALITY_GUI_PLAN_2026-08-12.md`: the
Simulation Options "Quality & Transport" page (three-way `QUALITY_SOLVER`
combo gating per-engine parameter groups, water-age and heat option groups,
2D transport group), a Reaction System comprehensive editor mirroring the
`[REACTION_*]` sections with engine-validated expression editing, Water Age
Sources and Heat configuration editors, and dynamic result descriptors so
species/age/temperature appear in map themes, plots, tabular results, and 2D
scalar rendering. GUI-facing engine obligations recorded there (§6) and
mirrored in the affected sub-plans:

- `swmm_reaction_validate_expression` + vocabulary discovery getters
  (reactions plan §4) — the GUI performs **no client-side parsing**.
- Species-kind query on the registry (§4.1) so reserved species are filtered
  from pollutant editors but labeled in results pickers.
- Dynamic per-run quality-variable enumeration in `openswmm_output.h` and 2D
  sidecar variable metadata (2D plan §3.1) for result descriptors.
- Water-age / heat source-table CRUD with `*_count` companions per C-API
  house rules.

GUI phases G1–G7 are gated on engine phases as tabulated in that plan; the
GUI may ship disabled placeholder categories ahead of engine availability.

## 8. Out of scope (recorded)

- GPU execution of the standalone Eulerian ARD component (FV in-solver
  transport remains the GPU path; revisit after T6).
- `.msx` file reader (Python translator instead, per LARD plan open question
  resolution).
- MPI multi-process composition (interfaces stubbed).
- CHANGELOG.md entries land with each phase's release, per repo convention.
