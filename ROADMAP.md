# OpenSWMM Engine Roadmap

This document records the development direction and current implementation of the OpenSWMM Engine (formerly OpenSWMMCore). It is maintained by the Technical Manager, [Caleb Buahin](https://github.com/cbuahin).

**Implementation snapshot: October 21, 2026 — 6.0.0-alpha.4 development source.** The milestone table distinguishes tagged baselines from later source work; current code may be ahead of published packages. Release history is recorded in the [release notes](docs/releases/RELEASE_NOTES_6.0.0-alpha.4.md) and [changelog](CHANGELOG.md).

Items are organized by theme rather than release dates. Remaining work depends on numerical validation, supported process combinations, platform qualification and community bandwidth. Community members can discuss priorities in [GitHub Discussions](https://github.com/HydroCouple/openswmm.engine/discussions).

## Status Key

| Symbol | Meaning |
|---|---|
| ✅ | Implemented baseline in the development source; remaining extensions are listed separately |
| 🔧 | Implemented in part or experimental; integration or qualification remains active |
| 📋 | Planned; no operational implementation claimed |
| 🔬 | Under exploration or recorded design study |
| ⏸ | Deferred or retired |

Implementation does not imply that every hydraulic, transport, restart and backend combination is supported. The restrictions below and the runtime capability diagnostics define those boundaries. Section 8 separates released milestones from later source work.

## 1. Flow Routing

### 1.1 Explicit Finite Volume 1D Flow Routing — Full Saint-Venant Equations ✅

**Implemented:** `FLOW_ROUTING FV` selects conservative Saint-Venant routing alongside the existing routing models. The solver includes hydrostatic reconstruction, HLL fluxes, semi-implicit Manning friction, MUSCL reconstruction, SSP-RK2, local time stepping, algebraic junction handling and mixed free-surface/pressurized cross-section closures. Virtual junctions, the two-component pressure approach (TPA), signed-head reporting and unsteady friction are available in their supported configurations. Dynamic wave remains the default.

The built-in CPU solver includes OpenMP work regions. `FV_BACKEND` and the plugin loader exist, but the supplied surface plugins do not provide a 1D network-solver implementation. Loader availability is not a claim of 1D GPU execution.

**Remaining work:**

- Improve runtime using measured production bottlenecks, preserving the accepted solution and retry behavior. Earlier timing ratios and cache experiments describe their recorded models and builds, not current universal FV/dynamic-wave speed ratios.
- Continue mesh-refinement, junction bed-step, mixed-flow and short/steep-reach studies; establish peak and timing accuracy as well as continuity.
- Broaden published laboratory and engineering benchmark coverage, including pressurization, flow reversal and long-duration runs.
- Implement and qualify 1D device backends. The experimental implicit pressurized update is a CPU capability, not a general implicit FV solver.

See the [hydraulics reference](docs/manuals/reference/hydraulics.md) and [FV implementation](src/engine/hydraulics/fv/ExplicitFvSolver.cpp).

### 1.2 2D Overland Flow and 1D/2D Exchange ✅

**Implemented:** the explicit finite-volume marcher supports `LOCAL_INERTIAL`, `FULL_SWE` and `DIFFUSIVE_WAVE` momentum closures, mixed triangle/quad meshes, local time stepping, boundary forcing and conservative exchange with the 1D network. Terrain meshing and results visualization are provided by SWMMVis.

The exchange configuration now includes per-node `[2D_NODE_COUPLING]` types, automatic coupling areas and the regime-aware `COUPLING_LAW WEIR_ORIFICE` option. Authored coupling rows and overrides persist through GeoPackage I/O.

OpenMP, CUDA, HIP and SYCL surface plugins load separately from the core. Active groundwater, transported species/age/temperature and porous-ground heat can require the built-in CPU marcher. The CPU implementation and numerical/performance review are documented in the [2D CPU qualification chapter](docs/manuals/engine/sections/Chapter7-TwoDimensionalCpu.md); its results do not establish GPU performance or correctness.

**Remaining work:**

- Published inundation benchmarks and measured catchment case studies beyond the analytic SWASHES suite.
- Long-period damping, mesh-dependent stability and production-scale accuracy studies.
- Qualification of additional process/backend combinations, including groundwater transport under the RK2 surface route. Standalone CLOSED_FORM/ENSLAVED/SIGMA water, pollutants/MSX, age and transported water temperature now support CPU SSP-RK2, including SIGMA layer chemistry and symmetric groundwater reactions; porous heat, chemical/age inventories and reactions also support restricted RK2 under constant, time-series or held air thermal boundaries, including latent soil evaporation. Finite surface/soil RK2 contacts now include pollutants/MSX and age with complete aquifer coverage; nonlinear radiative, sensible and accepted surface latent atmospheric exchange are also qualified; copied surface and groundwater runtime sources, aquifer HEAD/FLOW frames, copied constant surface STAGE/FLOW overrides with wall fallback and rain/ET/paired infiltration now share bounded stages and expiry/restart ownership; further heat and borrowed source/network owners retain first-order requirements.
- Further large-mesh performance work where deployment measurements justify it.

The implicit CVODE/ARKODE 2D integrators are retired; they are not an alternative operational route.

### 1.3 Spatially Explicit Inlets — Mode-Switching Junction Nodes ✅

**Implemented:** `[INLET_JUNCTIONS]` promotes inlet capture to a hydraulic junction with its own head, HEC-22 grate/curb capture, bypass, ponding and sewer-to-street backflow. The existing conduit-attribute `[INLET_USAGE]` form remains supported. The engine rejects double capture when both forms are assigned to the same attached conduit.

**Remaining work:** broader engineering case validation and continued parity checks for the conventional HEC-22 capture path.

## 2. Water Quality Transport

### 2.1 Pipe Advection–Reaction–Dispersion ✅

**Implemented:** `QUALITY_SOLVER` selects legacy tanks-in-series, Eulerian ARD (`EULERIAN_ARD`/`ARD`) or Lagrangian parcel transport (`LAGRANGIAN`/`LARD`). The operational ARD and LARD engines provide conduit transport, dispersion, external loading, node mixing/treatment and mass accounting, with reaction, water-age and heat bindings where supported.

The hydraulic FV solver also contains conservative scalar kernels on its own mesh. These scheme-level kernels are distinct from selecting the operational ARD quality engine; their existence does not establish complete model-load, treatment and output integration on the hydraulic FV mesh. Its scalar FCT sweep currently requires synchronous stepping.

**Remaining work:** complete and qualify that hydraulic-FV scalar integration, broaden transport/reaction/structure combinations, and document resolution, parcel-merging and timestep sensitivity for engineering applications.

See the [transport reference](docs/manuals/reference/quality/sections/Chapter7-AdvectionReactionDispersion.md).

### 2.2 Overland Transport and Surface Quality ✅

**Implemented:** the 2D marcher carries species, age and temperature on the surface mesh according to the transport policy. Rainfall quality, infiltration, saturation-excess return, 1D/2D transfers, cell coverages, buildup, washoff, sweeping and dispersion have operational paths. Multispecies surface processes share the model's reaction-species definitions.

**Remaining work:** broader catchment-scale transport validation, mixed surface ownership and wet/dry transition qualification, and backend/order combinations beyond the supported CPU profile. A transported particulate pollutant is not a sediment-bed or morphodynamic model (Section 3).

See the [surface-quality reference](docs/manuals/reference/quality/sections/Chapter10-MeshSurfaceQuality.md) and [surface-quality tests](tests/unit/engine/test_2d_surface_quality.cpp).

### 2.3 Groundwater Transport 🔧

**Implemented:** the CPU mesh aquifer carries pollutant/reaction-species mass, age and temperature through saturated/unsaturated stores. Exterior hydraulic HEAD/FLOW edges now bind `[GW_BOUNDARY_QUALITY]` composition and independent native mass/physical heat fluxes, with gross requested/accepted receipts and availability limits. Explicit `UNSAT_TRANSPORT LAYERED` enables canonical SIGMA layer inventories and LAYER initial conditions. `[REACTION_SUBSURFACE]` binds scoped SAT/UNSAT/LAYER expressions to the shared reaction integrator. LAYERED node/conduit leakage enters the actual receiving layer; sloping reaches split at layer/table intersections. Porous SAT/UNSAT reactors use the existing fixed-depth inventory, thermal temperature and protected outgoing reservations. SIGMA chemistry can also share porous soil heat through a conservative overlap map: chemistry and thermal layer counts remain independent, layer reactions use projected temperature, and fixed physical layers retain sole energy ownership. C/Python, INP/GeoPackage, native V17 state and HDF5 layer/edge results support these profiles; the GUI adds staged boundary editing, layer assignment and layer-result selection.

**Qualification and remaining work:**

- CPU integration/regression and heterogeneous/plume refinement qualification are in progress. Tensor dispersion now uses transverse dispersivity, skew correction, harmonic normal resistance and the solute `C_DIFF` bound; passing local flux/conservation tests does not establish universal field validity.
- Attached mesh-only CPU bulk or SIGMA engine checkpoints now qualify matched optional BULK MSX/pollutant chemistry, age-only and heat-only layouts, WATER_ONLY or paired porous heat, and groundwater HDF5 continuation, including pending contacts, accepted cell clocks and independent grids. Managed sessions support numerical state or mesh output for this profile; reactive hydraulic/quality HEAD/FLOW edge continuation now also qualifies bulk and independent SIGMA/thermal grids, signed water forcing, imposed mass fluxes and atomic forcing-identity refusal. Full-session network chemistry and surface-species/buildup output continuation retain their guards.
- Joint conduit/node/well/edge donor tests, unequal-clock pending cold-contact/heat restart and a heterogeneous skewed-mesh diffusion refinement case now pass on CPU. Deliberate boundary-debit and compression-transfer faults are detected. Complete the broader coupled network-chemistry/output restart and application qualification matrix. The SIGMA/porous overlap mapping is implemented and selected CPU profiles are tested; default BULK retains its historical saturated-contact approximation.
- **Coupled CPU SSP-RK2:** standalone FULL_SWE with one global tier advances actual CLOSED_FORM, ENSLAVED or SIGMA aquifers. Conserved equilibrium/layer-water blends, starting-state ALE motion, beginning donors and whole-call rollback qualify smooth refinements and bounded wet/dry transitions. Authored wells and aquifer HEAD/FLOW edges split the shared stages at forcing knots and reversals. Switching events have no blanket second-order claim.
- **RK2 chemistry and age:** bulk or SIGMA inventories, sorption, dispersion, explicit age production, pollutant decay and surface/SAT/UNSAT/LAYER MSX kinetics share the accepted transaction. Symmetric reaction halves use actual aqueous capacity and local temperature, with bounded work-limit retries and signed production/loss ledgers. EUL, algebraic and non-smooth laws retain their declared accuracy limits.
- **RK2 thermal owners:** WATER_ONLY temperature advection and porous energy/conduction support cold forcing, independent chemistry/thermal grids, phase-aware reactions, sensible or latent soil ET and constant, project-series or held AIR boundaries. Complete aquifer coverage also supports reciprocal finite surface/soil heat, chemical and age contacts over physical VFR footprints. Nonlinear radiation, sensible atmosphere and surface latent cooling qualify. Latent joules follow actual accepted evaporation, with separate sensible water export and bounded whole-interval retries for thermal admissibility. Channel/LID/snow owners remain outstanding under RK2.
- **Copied runtime inputs:** surface/groundwater sources, aquifer HEAD/FLOW, rain/quality, ET and paired infiltration share bounded stages, chronological expiry and accepted water/species/heat receipts. Authored constant or in-memory series STAGE/FLOW boundaries and copied overrides qualify with exterior binding and wall or authored fallback, including held beginning-state Riemann evaluations, competing-edge budgets, clear history and atomic provider frames. Immutable series use actual Euler times, shared knot/reversal cuts and completed-clock restart validation. Normal-flow and in-memory rating curves now evaluate each Euler state, with accepted-state curve diagnostics and atomic restart/override fallback. Borrowed network/catchment owners remain guarded. Accepted rain and evaporation reach public/cell/HDF5 reports once.
- **Managed standalone RK2 restart:** numerical-only and mesh-HDF5 sessions preserve engine/native-weather calendars, cursors, pending batches, copied prescriptions and actual numerical/receipt state. Exact split runs and read-only inspection cover reactions, age and qualified thermal grids; corrupt or incompatible state is refused before installation/output publication, with valid retry. Scientific HDF5 values, attributes and schema match exactly; allocation addresses may differ. Earlier managed RK2 router snapshots lacking boundary receipt/fallback state are refused. Component format 4 and first-order formats retain their documented scope. Network/catchment owners, source journals, binary/combined output and file-based radiation remain outside this managed profile.
- **Backend qualification:** surface plugin ABI 7 protects the shared receipt layout; matching build contracts are also required. An optional versioned host OpenMP operator computes raw CLOSED_FORM recharge, with C/Python availability/use/thread diagnostics. Water, receipts, transport, reactions and heat remain CPU-owned. Full groundwater/transport acceleration and CUDA/HIP/SYCL execution remain outstanding; CPU fallback and surface alignment do not qualify device groundwater execution.

See the [transport contracts, examples and limits](docs/design/groundwater-transport.md), [kernel tests](tests/unit/engine/test_gw_transport_kernel.cpp) and [SIGMA conservation tests](tests/unit/engine/test_sigma_conservation.cpp). The new work is a development implementation, not a declaration that every groundwater completion gate has passed.

### 2.4 Multispecies Reactions Across Domains 🔧

**Implemented:** shared reaction-species definitions, bulk/wall declarations,
named coefficients and expressions, rate/equilibrium/formula evaluation and
selectable integrators. Bindings exist for legacy, ARD and LARD network
transport and supported surface processes. Groundwater has explicit scoped
`[REACTION_SUBSURFACE]` reactors for the supported CPU profiles described in
Section 2.3. Standard native binary output includes reaction-species columns.

The 2026-10-19 increment implements four formerly reserved sections:

- `[REACTION_SOURCES]`: additive CONC/MASS node loads through the existing external species-load seam; MASS uses declared mass units per second.
- `[REACTION_PATTERNS]`: cyclic nonnegative source multipliers, with PATTERN_TIMESTEP in seconds and a simulation-start clock origin.
- `[REACTION_PARAMETERS]`: node/link PARAMETER overrides used by terms and all expression forms, including ARD parent links and LARD segments.
- `[REACTION_REPORT]`: species-column selection in native output without altering internal species indexing. Ordinary `[REPORT]` retains element selection.

The 2026-10-20 increment adds `[REACTION_SUBCATCHMENTS]` for BULK wet pools
on non-LID subcatchments. It mixes rain/run-on with retained mass, integrates
its own expressions, then allocates mass to infiltration, runoff and storage.
It uses conservative residual-mass storage, records reaction/clamp amounts,
and exposes the scope through C/Python and File editing. Constants, global
parameters, terms, DT and beginning-of-step runoff TEMP are supported;
WALL/pollutant dependencies and other hydraulic inputs are refused. Existing
surface-quality closure remains unchanged without explicit subcatchment laws.

LEGACY now keeps WALL state attached rather than advecting or evaporatively
concentrating it as dissolved mass. The dissolved routing ledger excludes
WALL densities. ARD retains its LEGACY fallback for WALL systems; LARD refuses
them. LEGACY/LARD tank kinetics skip zero and sub-litre storage volumes.
Selected output and parameter overrides are checked across all three network
quality solvers. Reusable [reaction networks](examples/reactions/README.md)
include parent/product conversion, temperature-dependent decay and attached
wall decay. The companion benchmark repository adds a `quality/reactions`
suite with 24 analytic cells across quality solvers, rate units and overrides,
plus three rain-fed wet-pool conservation checks (self-consistency only).

The next increment adds native checkpoint/output continuation for the qualified
1D chemistry profile, geometry-aware WALL exchange, selective wet laws,
dry land-use reactors, porous LID inventories and FLOWPACED/SETPOINT boosters.
The editor has dedicated source, pattern, parameter, wall-area and report tables.
The companion suite now has 38 cells, including analytic wall, dry-buildup and
setpoint cases and a rain-fed bioretention conservation case.

**Remaining work and restrictions:**

- Full native reaction continuation currently qualifies CPU 1D network, wet/dry surface and supported LID inventories, with inline rainfall and native output. That 1D profile excludes heat/age combinations, active 2D surface chemistry, subsurface laws, pollutants and detailed ARD/LID output. A separate mesh-only first-order profile supports matched optional BULK MSX/pollutant inventories, age and porous heat, including age-only and heat-only layouts, without live network or private catchment owners (Section 2.3). State-only hotstarts cannot carry porous LID reaction inventories.
- `WALL_GEOMETRY YES` supplies AV in AREA_UNITS/litre. Conduits use actual wetted perimeter, physical length and barrels; storage requires explicit contact area. WALL density uses fixed reference area, kinetics scale by wetted fraction, and dry inventory is retained. WALL algebraic projections remain refused; ARD uses LEGACY fallback and LARD refuses WALL.
- `[REACTION_LIDS]` explicitly opts controls into BULK layer transport. Storage-node bucket/Richards reactors require LEGACY quality and a non-Mesh bottom. Conventional units require full subcatchment coverage and all units to opt in; bioretention and permeable-pavement stacks have conservation/restart checks. Partial capture, covered rain barrels, WALL chemistry and 2D-owned conventional units remain refused.
- Wet-pool mixing and dry buildup accrual/washoff remain first-order splits. Selective subcatchment laws and separate land-use buildup laws are implemented; exact continuously fed CSTR integration and wider timestep/domain qualification remain future work.
- Broaden independent chemistry and field benchmarks. Current analytic and conservation cases are numerical qualification, not calibrated or universal chemistry validation. Changes are committed locally; publication requires pushing the repositories.

The [reaction component](src/engine/transport/components/ReactionModule/ReactionsComponent.cpp),
[configuration/qualification tests](tests/unit/engine/test_reactions_config.cpp),
[output-quality tests](tests/unit/engine/test_output_quality.cpp) and
[multispecies tests](tests/unit/engine/test_msx_parity.cpp) record these distinct
capabilities. Source patterns are sampled on the inflow-assembly clock;
routing steps must resolve their changes. The GUI's Sources tab opens the
validated File editor for the extended sections; dedicated structured tables
are not yet provided.

## 3. Sediment Transport 📋

**Planned:** overland erosion/detachment, suspended and bedload routing, particle-size fractions, deposition and re-entrainment, sediment-associated constituents, and validation against flume and field data.

There is no operational sediment-bed evolution or morphodynamic solver claimed here. Existing pollutant transport and thermal `SEDIMENT_EXCHANGE` do not provide those processes.

## 4. Heat Transport 🔧

**Implemented baseline:** network/watershed temperature transport, surface sensible/latent exchange, radiation, solar forcing, supported LID layer heat and prescribed bed/hyporheic exchange. Per-element and tag-based heat overrides and runtime climate forcing are implemented; they are no longer a wholly planned feature.

**Experimental coupled mesh implementation:** `HEAT_MODEL COUPLED` and the required transport switches enable finite surface/soil heat exchange. `THERMAL_MODEL POROUS_EQUILIBRIUM` adds soil-plus-water storage and thermal transport beneath the spatial aquifer. Supported paths include lateral saturated heat transfer, routed-channel mesh-ground contacts, reciprocal UEB snow/soil exchange and mapped Richards LID layer heat. Accepted water and energy transfers share explicit ownership and budgets. Groundwater's default `WATER_ONLY` thermal representation remains distinct.

**Remaining work:**

- Broader geometry, spatial/time refinement and material-contrast qualification for the coupled model, especially channel/LID/soil contacts and UEB melt, drying and rewetting transitions.
- Complete tracer/age partitioning and atmospheric/footprint ownership for the experimental ground/LID paths; heat support does not establish support for every pollutant, reaction or age combination.
- Expand supported restart, backend and GUI release profiles. Native coupled thermal restart exists for qualified configurations (Section 9), while the wider combination matrix remains open.
- Field-temperature validation and detention-basin stratification remain development goals; neither is asserted as a delivered validation result or general operational capability here.

See the [thermal implementation and limits](docs/design/mesh-thermal-model.md), [mesh heat tests](tests/unit/engine/test_mesh_heat.cpp), [porous heat tests](tests/unit/engine/test_porous_heat.cpp) and [per-element heat tests](tests/unit/engine/test_heat_per_element.cpp).

## 5. Groundwater

### 5.1 Conventional Two-Zone Subcatchment Aquifers ✅

**Implemented:** the SWMM per-subcatchment aquifer path retains `[AQUIFERS]`, `[GROUNDWATER]` and `[GWF]`, with unsaturated/saturated moisture accounting, percolation, evapotranspiration and user-defined lateral flow to the drainage network. C and Python interfaces expose aquifer parameters.

**Remaining work:** continued hydrology/legacy-parity verification and explicit documentation of transport support for this path. Mesh-aquifer transport in Section 2.3 does not automatically apply to conventional subcatchment aquifers.

### 5.2 Spatially Explicit Groundwater — FV Mesh Integration 🔧

**Implemented:** opt-in `[2D_AQUIFER]` cells with a moving water table, saturated lateral Darcy exchange, surface infiltration/return flow, deep losses, node/conduit beds, evapotranspiration, soil-characteristic choices and CLOSED_FORM/ENSLAVED/SIGMA unsaturated closures. Groundwater transport and porous thermal work are described in Sections 2.3 and 4.

**Remaining work and restrictions:**

- Continue closure-ladder, column-reference and catchment-scale validation; the non-Gardner recharge generalizations remain experimental.
- Complete a true subcatchment-cell/outlet implementation for `MODE PER_SUBCATCH`. The current selector removes lateral topology but still runs on mesh cells; it is not the completed mesh-free aquifer representation originally proposed.
- Extend supported boundary, reconstruction-order and backend combinations without changing native ownership or double-counting subcatchment infiltration and aquifer exchanges.

See the [groundwater reference](docs/manuals/reference/hydrology/sections/Chapter9-MeshGroundwater.md) and [kernel implementation](src/engine/2d/subsurface/SubsurfaceSolver.cpp).

## 6. LID Hydraulic Integration — LID as Storage Nodes 🔧

**Implemented:** `[LID_NODES]` assigns layered controls to ordinary storage nodes. Multiple links and independent control rules supply head-dependent outlets, backwater and reversal. Classical layered flow, layer-specific pollutant treatment, outlet anchors, explicit bottom ownership and an optional adaptive 1D Richards column are operational. Conventional subcatchment LIDs remain supported.

Mapped Richards LID/aquifer water exchange and coupled thermal layers have supported experimental paths; these are no longer design-only work.

**Remaining work:**

- Broaden hydraulic, routing-step and porous-cell convergence studies and independent/field validation.
- Extend transport/reaction/age support across classical and Richards layers and qualify the combined mesh-aquifer/thermal configurations.
- Complete profile-history output and the remaining GUI/restart combinations. Classical and Richards LIDs do not share an unrestricted support matrix.

See the [storage-node LID manual](docs/manuals/engine/sections/Chapter6-LidStorageNodes.md), [classical hydrology implementation note](docs/design/lid-node-legacy-hydrology.md), [LID tests](tests/unit/engine/test_lid_nodes.cpp) and [thermal limits](docs/design/mesh-thermal-model.md).

## 7. Deferred Items

| Item | Current disposition |
|---|---|
| Implicit 2D CVODE/ARKODE integrators | Retired; the explicit marcher replaced this route and its SUNDIALS/hypre dependencies |
| Real-time data assimilation and sensor fusion | Deferred; external telemetry infrastructure is outside the current implementation |
| Machine-learning surrogate models | Deferred research direction; no operational engine module claimed |

Full shallow-water 2D momentum and Kokkos surface plugins are implemented (Section 1.2). 1D device execution remains outstanding (Section 1.1).

## 8. Completed Items

The version column distinguishes released baselines from later source milestones. **Unreleased** identifies development work beyond the recorded `v6.0.0-alpha.3` release baseline; it does not assign a future release or claim that every related extension is complete.

| Implemented baseline or milestone | Release/source status |
|---|---|
| Data-oriented, reentrant core, public C API and Python interfaces | 6.0.0-alpha.1; interfaces continue to evolve |
| Conventional two-zone groundwater | 6.0.0-alpha.1 |
| Coupled explicit local-inertial 1D/2D surface routing | 6.0.0-alpha.1; hardened through alpha.3 |
| Kokkos OpenMP/CUDA/HIP/SYCL surface plugins | 6.0.0-alpha.2 |
| Decoupled 1D/2D timesteps and conservative exchange windows | 6.0.0-alpha.3 |
| FV 1D routing, virtual junctions, TPA and unsteady friction | Unreleased source |
| Full-SWE/diffusive-wave 2D closures, mixed meshes and parameter surfaces | Unreleased source |
| Inlet junctions and revised per-node 1D/2D exchange laws | Unreleased source |
| Operational ARD/LARD, multispecies network bindings and native species output | Unreleased source |
| Mesh surface quality and bulk aquifer species/age/temperature transport | Unreleased source; restrictions in Section 2 |
| Spatial aquifer flow and common-source ownership | Unreleased source; restrictions in Section 5 |
| Storage-node LIDs, Richards columns and supported aquifer beds | Unreleased source; restrictions in Section 6 |
| Network/watershed heat and per-element heat overrides | Unreleased source |
| Coupled mesh, porous-ground, channel, UEB and Richards LID thermal paths | Unreleased experimental source; restrictions in Section 4 |
| Native hot-start extensions, dated saves and qualified coupled thermal checkpoints | Unreleased source; restrictions in Section 9 |
| HydroCouple metadata inspection, CPU 1D component execution and qualified outfall transfers | Unreleased source; restrictions in Section 9 |

## 9. Interfaces, Persistence and Model Coupling 🔧

### 9.1 C and Python Interfaces

**Implemented:** domain APIs, programmatic model construction/editing, runtime forcing and controls, bulk results, capability diagnostics, live output reading and metadata-only opening. Schema/catalog generation and API drift checks support binding maintenance.

**Remaining work:** close authoring/diagnostic gaps as new features arrive, retain strict lifecycle and unit contracts, and keep C/Python/GUI documentation aligned with actual execution support. Parsed configuration and metadata inspection must not advertise unqualified execution.

### 9.2 Model I/O and Restart

**Implemented:** input-file, GeoPackage and optional HDF5 model persistence; binary and 2D HDF5 results; native/legacy hot-start handling with per-save format selection and dated saves. Legacy hot starts with subcatchments can be embedded in GeoPackage without losing their runoff/trailer bytes. Native save/use includes reaction state and qualified coupled thermal engine state at accepted step boundaries.

Mesh-output checkpoint capture now settles HDF5's unused allocation tail before
recording a result prefix. Exact fresh-header identity checks, corruption rejection
and atomic multi-output publication remain intact. An isolated macOS CPU build
with HDF5 2.1.1 passes **174 engine checkpoint checks and 31 mesh-output checks**,
including initial/resumed reports, compression, groundwater/thermal/Richards LID
stores, source histories, damaged companions and relocated bundles. See the
[mesh-checkpoint verification record](tests/verification/mesh_checkpoint_2026-10-20/README.md).

**Remaining work:** extend qualification beyond the tested CPU surface/GW/snow/LID/thermal configurations to additional physical cases, platforms/backends and transport combinations. Managed water-only sessions, direct component checkpoints and coupled thermal engine checkpoints have different contracts; they are not interchangeable. Coordinated multi-component restart remains open.

See the [file reference](docs/manuals/engine/sections/Chapter3-Files.md), [hot-start implementation](src/engine/core/HotStartManager.cpp) and [checkpoint tests](tests/unit/engine/test_engine_checkpoint.cpp).

### 9.3 HydroCouple Components

**Implemented:** optional native components built with `OPENSWMM_WITH_HYDROCOUPLE`. The generic engine component provides model metadata, qualified CPU 1D execution, native results including MSX columns, and conservative external-outfall transfers with receiver acknowledgments for its supported LEGACY-quality/heat profile. The FVQual compatibility adapter remains separate.

**Remaining work:** spatial/integrated surface-groundwater execution, bidirectional hydraulic feedback, broader species and receiver profiles, coordinated restart and wider platform/package qualification. Inspection of spatial configuration is not spatial component execution.

See the [component execution and qualification contract](docs/COMPONENT_ENGINE.md).

---

*Last updated: October 21, 2026 — Caleb Buahin, Technical Manager*
