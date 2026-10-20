# Spatial groundwater transport

This development implementation extends the CPU mesh aquifer. Groundwater chemistry and heat use first-order coupling; the restricted water-only SSP-RK2 profile below has separate qualification. It does not apply to conventional subcatchment `[GROUNDWATER]` aquifers. Build from the current source; published packages may lag these interfaces.

## Supported profiles

| Profile | Authoring | Storage and execution |
|---|---|---|
| Bulk groundwater | Default `UNSAT_TRANSPORT BULK` | SAT and mixed UNSAT inventories; existing closures and defaults |
| Layer chemistry | `UNSAT_TRANSPORT LAYERED` | SIGMA on every covered cell; aqueous plus sorbed layer inventories; UNSAT is their derived sum |
| Exterior edges | `[2D_AQUIFER_BOUNDARIES]` and `[GW_BOUNDARY_QUALITY]` | HEAD or FLOW hydraulics; advective composition and independent mass/heat fluxes |
| Aquifer reactions | `[REACTION_SUBSURFACE]` | Explicit SAT/UNSAT/LAYER expressions using the shared reaction integrator |
| Porous soil heat | `THERMAL_MODEL POROUS_EQUILIBRIUM` | Fixed-depth thermal/porewater owner; boundary water, species and physical joules are booked once |

LAYERED chemistry can use `THERMAL_MODEL WATER_ONLY` or `POROUS_EQUILIBRIUM`. With porous heat, SIGMA owns solute/age mass and fixed physical layers own thermal energy; a sparse conservative overlap map connects their independently chosen layer counts. Bulk porous SAT/UNSAT reactions retain their fixed-depth inventory owner. LAYERED requires spatial `MODE MESH`. Coupled surface reconstruction order 2 remains unsupported for these transport/thermal profiles. Groundwater stores and time advancement use the built-in CPU owner. The optional OpenMP raw-recharge operator below is a separate partial profile; CUDA/HIP/SYCL requests retain CPU groundwater computation. `swmm_2d_get_run_stats` identifies the surface marcher, while `swmm_gw2d_get_execution_status` identifies the groundwater operator.

## Restricted water-only SSP-RK2

A standalone spatial `CLOSED_FORM` aquifer can advance with `MOMENTUM_EQUATION FULL_SWE` and `RECONSTRUCTION_ORDER 2` on the CPU. Set `NODE_ENROLMENT ROWS` without exchange rows and `LINK_SEEPAGE NONE`. This profile requires no active groundwater or surface species/age/heat, subcatchments, network exchange points, runtime providers, snow or borrowed common-source/channel owners. Wells and aquifer HEAD/FLOW edges may have constant or authored piecewise-linear water-only forcing. The marcher uses one global tier; unsupported profiles fail before execution.

Authored well/FLOW-edge knots and sign reversals split the shared surface/groundwater step. Both Euler evaluations use the exact mean prescribed water flow on the same interval, so the convex blend counts each forcing interval once. Earlier accepted subintervals are the starting point for subsequent blends; a later failure restores the complete original call. Gross boundary requested/applied volumes and domain ledgers receive the same accepted blend. HEAD stages integrate pressure-driven flux with their held cell head; no additional aquifer-only water substeps occur inside an Euler evaluation. Shared cuts also include exterior-head clipping levels. Runtime providers and quality terms remain guarded.

Both stages advance surface and groundwater water together. The accepted convex blend includes SAT/UNSAT water, pending surface returns, wetting-front state and cumulative transfer ledgers. A rejected trial or failed advance restores the complete owned numerical state, including momentum, clocks and receipts. Smooth stages evaluate instantaneous intake and drying recovery; reusing their held-interval finite subflows would reduce coupled temporal order. At the singular initial wetting front a finite Green-Ampt startup retains bounded intake, without claiming second-order accuracy across that event.

Tests measure approximately fourfold error reduction for analytical deep drainage and nonlinear lateral Darcy relaxation, and factors 3.85/3.91 for smooth coupled infiltration under timestep halving. Full-step/two-half-step differences approach third local order on four fine steps; the coarsest measurement is retained as pre-asymptotic evidence. Large injection/extraction and singular startup conserve water and stay admissible. An affine well plus linear deep drainage also converges at second order over four timestep refinements. Constant-head unconfined exchange matches an independent Riccati solution under both inflow and drainage. A clipped/reversing head series is checked against an independently refined scalar RK4 reference: the four finest timestep errors decrease by factors about 3.85, 3.69 and 4.00. All six measurements are retained; the two coarsest have cancellation and are not monotonic. This is an asymptotic result for the named fixture, not a monotonic-error or universal order claim across switching/saturation events. Asynchronous signed wells, bounded pumping, late-interval rollback and exact series-qualified checkpoint continuation have separate gross-water gates.

The internal standalone CPU component checkpoint has a separate versioned RK2 profile. It preserves surface water/momentum, aquifer stores, pending returns, scheduling and accepted numerical clocks. Restore validates geometry, options, complete well/edge-series identity and held weather before installation. Corrupt or incompatible state is refused atomically, with valid retry supported. This component checkpoint does not include the full engine calendar, reports or output writers; managed engine/source checkpoint attachment remains refused for RK2.

## Optional host OpenMP recharge

`BACKEND OMP` (or `OPENSWMM_2D_BACKEND=omp`) may select the Kokkos OpenMP extension for the **raw CLOSED_FORM recharge calculation only**. AUTO retains scalar groundwater recharge until an integrated workload qualifies an automatic selection policy. All four soil-characteristic laws call the same scalar soil function inside a host-parallel loop. Their existing physical-model limitations still apply. SIGMA and ENSLAVED recharge, water budgets/donor caps, pending receipts, transport/dispersion, reactions, heat and clocks remain CPU-owned. This also applies when CLOSED_FORM recharge accompanies bulk reactive or porous thermal owners. It is partial host execution; it does not establish full groundwater acceleration or device execution.

The optional ABI v1 advertises the operator, closure, soil laws, host-owned companions, time integrators and canonical host restart. It owns no layer or species arrays. Missing exports, incompatible capabilities or malformed/nonfinite results preserve scalar CPU computation. No CUDA/HIP/SYCL extension is supplied. Kokkos initializes once per process; the first initialization fixes the actual host thread count.

Query `swmm_gw2d_get_execution_status` with `struct_size=sizeof(SWMM_GwExecutionStatus)`, or Python `solver.surface2d.groundwater.execution_status`, after initialization. Availability and successful synchronous operator calls are separate: an available extension has not necessarily run. Batch/cell counters describe accepted raw evaluations, including values that can require scalar recomputation after a source knot changes their input. They are process-local diagnostics, reset on component restore, and excluded from rejected RK2 transactions. The returned status also names the CPU-owned operations and effective Kokkos threads. Surface backend status alone does not describe this partial profile.

Model-qualified groundwater and standalone RK2 checkpoints distinguish the selected operator profile. Same-profile restart restores canonical host inventories and recomputes transient operator inputs; changing the selected operator is refused before state installation. Historical bulk native hotstarts remain state-only carriers and do not certify exact session continuation. The full-engine/output and RK2 profile restrictions elsewhere in this document remain applicable.

## Hydraulic edges and quality

```text
[2D_AQUIFER_BOUNDARIES]
CELL 1 EDGE 0 HEAD CONST -0.5
CELL 2 EDGE 2 FLOW SERIES GW_FLOW

[GW_BOUNDARY_QUALITY]
1 0 TSS CONC 3
2 2 TSS TS GW_TSS
2 2 TSS MASSFLUX 2
```

Use cells and exterior local edges from the actual mesh. File cells are **1-based**; local edges are **0-based**. For triangles and quads, edge `e` joins local vertices `(e+1)%nv` and `(e+2)%nv`. Interior or duplicate hydraulic edges, unmatched quality rows and uncovered aquifer cells are refused.

HEAD is an elevation in project length units. FLOW is **total m³/s across the edge, positive inward**, including for US-unit projects. An unspecified edge is no-flow. HEAD and named FLOW series may reverse. Every enabled groundwater species needs an explicit exterior concentration, including zero, wherever inflow is possible. Extraction uses the interior donor. A concentration and one independent imposed flux may coexist for each species/edge.

CONC/TS uses native concentration, age in seconds, and signed temperature in °C. MASSFLUX is native amount/(s·m of edge); HEATFLUX is W/m. Physical edge length is applied once. Mass flux requires a pollutant or BULK reaction species with a recognized amount basis; age/temperature do not accept MASSFLUX. HEATFLUX requires temperature transport. A zero hydraulic FLOW can carry an independent imposed species/heat flux. A quality row alone never supplies water.

Forcing is integrated chronologically across series knots and sign changes. Outgoing water and nonnegative species sinks are availability-limited. Gross requested/accepted water and advective/imposed species receipts remain separate, including when net water exchange is zero. Boundary water and species enter the lateral net ledger once; they are not disguised as wells.

The new **authored** Python boundary API is `groundwater.set_authored_boundary(...)`, before initialization. Its inward FLOW convention differs from the existing runtime `set_flow_boundary(...)` API's outward-flow argument. Both document their units; overlapping authored/runtime prescriptions are refused.

## Layers and reactions

```text
[2D_AQUIFER_OPTIONS]
CLOSURE SIGMA
M_LAYERS 4

[GW_TRANSPORT_OPTIONS]
UNSAT_TRANSPORT LAYERED

[GW_INITIAL_QUALITY]
* UNSAT TSS 2
CELL 1 LAYER 2 TSS 7

[REACTION_SPECIES]
BULK A MG
BULK B MG

[REACTION_SUBSURFACE]
* SAT A RATE -0.001*A
* SAT B RATE 0.001*A
* UNSAT A RATE -0.001*A
* UNSAT B RATE 0.001*A
```

These are authoring fragments: the model must also enable the corresponding processes and define its mesh/aquifer, species and reaction options. Reaction rates follow `[REACTION_OPTIONS] RATE_UNITS`.

Authored LAYER indices start at **1**. Runtime C/Python layer access and HDF5 layer axes start at **0**, from surface downward. Precedence is GLOBAL < TAG < CELL; within a scope, UNSAT initializes all layers before explicit LAYER values. An invalid/non-SIGMA layer request is refused rather than mapped to a bulk store.

Each layer stores total aqueous plus equilibrium-sorbed amount. Dissolved concentration is `M / (Vwater + Kd × bulk_density × Vbulk)`; authored Kd in L/kg is converted to m³/kg once. Transport consumes only the accepted hydraulic layer transfers, including moving-table redistribution, compression and support. An implicit conservative chain solve handles internal layer transfers and vertical dispersion. Dry retained inventory stays visible. Layer sums exactly define the UNSAT inventory. In LAYERED mode the existing SAT/UNSAT concentration channels use total inventory divided by aqueous-plus-sorption capacity; legacy BULK keeps its historical inventory/water-volume reporting basis. Layered age production is a separately ledgered term; legacy BULK age semantics remain unchanged.

Subsurface expressions may use DT, TEMP, GW_THETA, GW_POROSITY, GW_SE and GW_DEPTH_M, along with supported species, coefficients and intermediate terms. UNSAT laws run independently in each active layer; scoped LAYER laws override them. No PIPE/TANK expression is reused implicitly. Pipe-only variables, reachable WALL species, disabled pollutant dependencies and duplicate first-order decay ownership are refused. Unrelated network WALL species do not disable groundwater BULK species. Aqueous RATE kinetics are scaled to aqueous volume rather than accidentally reacting sorbed inventory at the same rate. Reaction production can appear as negative net loss. Expressions can be edited through the existing C/Python staged reaction-text interface; failed edits preserve the previous system.

In LAYERED mode, node leakage above the table enters the SIGMA layer intersected by the bed base (node invert minus bed thickness). An unsaturated bed cannot drain the disconnected saturated zone upward. Conduit leakage is split along each clipped polyline segment at the actual layer/table intersections, using linearly interpolated endpoint invert elevations and authored bed thickness. Accepted water and producer composition share the same receiving slots. Slots remain fixed until the owner fires; the accepted ALE sweep then moves their inventory conservatively. Compression can pass excess downward into SAT, with saturation excess handled by the existing surface return. Default BULK retains its historical saturated-contact approximation.

Porous SAT/UNSAT reactions use each fixed physical layer's geometric table overlap, aqueous water and sorption capacity. They read the thermal owner's local temperature and phase depth/moisture. Accepted outgoing reservations remain protected from later reactions. All physical-layer reaction updates stage together before one canonical inventory is published and bulk views are refreshed. This adds no reaction heat source; heat-producing chemistry is not implied.

### SIGMA chemistry with porous soil heat

Enable `HEAT_TRANSPORT YES`, `UNSAT_TRANSPORT LAYERED`, `THERMAL_MODEL POROUS_EQUILIBRIUM`, and SIGMA on every active mesh aquifer cell. `M_LAYERS` controls chemistry; `THERMAL_LAYERS` controls the fixed-depth thermal grid. Their counts may differ. Authored LAYER values, including temperature, select SIGMA layers in this profile. Unspecified initial temperatures use the configured thermal initial state.

Each thermal/chemical interval intersection borrows its actual aqueous and grain capacity. Surface/channel contact increments return to the intersected canonical chemical stores; accepted export reservations remain unavailable to later contacts. The fixed-grid view and its receipts are synchronized at the existing accepted coupling boundaries. Rebuilding the view cannot remix, duplicate or discard canonical layer mass. Zero-capacity retained inventories remain with their chemical owner and cannot supply a wet contact.

Thermal energy advances once on the fixed physical grid using the accepted external water/heat receipts and overlap-projected moisture. Water-temperature quantities on SIGMA layers are derived from aqueous overlap with the thermal field; they are not separate soil-energy stores. Reactions read that projected temperature and the actual chemical-layer aqueous/sorption capacity. Porous solid energy is never advected as solute or produced implicitly by reaction expressions. Coarse thermal layers mix temperatures within each physical control volume; refining chemistry alone does not refine the thermal solution.

Groundwater transport state checkpoints include overlap geometry, protected exports and pending contact increments. Incompatible thermal/chemical grids and unpaired mapping receipts fail before changing the target. Standalone thermal hotstart save/apply remains refused by the existing carrier guard. The attached mesh-only engine checkpoint profile below also qualifies calendar and groundwater-output continuation; wider owner combinations retain their separate restrictions.

## Results and restart

C/Python expose canonical per-layer water volume, total inventory and dissolved concentration, along with gross boundary receipts. INP and GeoPackage preserve the new sections at full double precision. The GUI provides staged edge/quality rows, BULK/LAYERED selection, valid layer initial assignments, undo, and species/layer result selection.

With groundwater reporting enabled, HDF5 adds:

- `Mesh2_face_gw_layer_mass` and `Mesh2_face_gw_layer_conc`: `[time,species,layer,face]`, with species identity, units and explicit axes.
- `groundwater_boundary_water`: `[time,boundary,term]`, gross requested/accepted inward/outward m³.
- `groundwater_boundary_species`: `[time,species,boundary,term]`, advective and imposed inward/outward amounts.
- `groundwater_transport_ledger`: `[time,species,term]`, the extended public ledger order.

Existing bulk datasets and the legacy 15-term species ledger retain their dimensions. Internal solute amounts use native concentration × m³: for mg/L, multiply by 1000 to obtain mg. Thermal forcing uses the configured physical water heat capacity; porous solid energy remains owned by the thermal model.

New transport profiles use native V17 hot-start state with layer inventory, pending receipts (including receiving layers and gross outgoing water reservations), clocks and boundary diagnostics. Model geometry, species/units, forcing and reaction identity are checked before publication. Legacy hot starts refuse these profiles. A native hot start restores hydraulic/quality state; it is not a promise of complete engine-calendar, control and output continuation for every coupled configuration. Existing engine checkpoint profiles retain their own capability restrictions.

The attached **mesh-only CPU first-order SIGMA/LAYERED + POROUS_EQUILIBRIUM** engine checkpoint profile is qualified for passive pollutants, water age, finite surface/soil contacts and independent chemistry/thermal grids. Exact full-session continuation covers moving-table state, a pending cold conduit-contact receipt and midnight crossing. HDF5 continuation with `GROUNDWATER HEAT` reporting covers 5/3 and 3/7 chemistry/thermal layers, hydraulic/quality groundwater edges, the report calendar and every numeric dataset. Species units/order and layer/boundary dimensions are validated before output publication; restoration adopts the corresponding writer counts. Infiltration capacity/refresh and ET stress may report NaN before their first applicable refresh or while disabled; infinities and nonfinite chemistry are refused. Corrupt output prefixes and incompatible grids leave the fresh engine and existing output unchanged, with a valid retry supported.

A separate **mesh-only first-order bulk or SIGMA/LAYERED shared groundwater quality** profile now admits matched surface/groundwater optional BULK MSX species, explicit subsurface reactions, passive pollutants and water age. Age-only and heat-only layouts also qualify; no dummy chemical species is required. Managed C/Python sessions support numerical state (`OUTPUT_NONE`, Python `0`) or mesh HDF5 output (`OUTPUT_MESH`, Python `2`). They require matching complete species layouts. Use either `THERMAL_MODEL WATER_ONLY` with heat disabled, or `POROUS_EQUILIBRIUM` with coupled surface heat and a ground-column bed. Bulk finite-porewater contact/generation ledgers now have a versioned transport payload, even without chemical layers, edge boundaries or reactions; old files missing this history are refused. Bulk shared-source transport also persists accepted cell clocks with a versioned payload, including species-only models without layers, boundaries or reaction laws. Older exact-session snapshots missing these clocks are refused; raw default bulk state carriers without a shared-source owner retain their existing behavior. WATER_ONLY layered-age bookkeeping does not allocate or refresh porous thermal cells. Temperature-only SIGMA columns initialize overlap geometry for thermal views and restart while keeping their mapped dissolved masses zero; energy retains its thermal owner. Exact split-run tests cover MSX-only, mixed and passive-only models, optional age/heat, cold or water-only time-series wells, bulk and 5/3 or 3/7 grids, pending contacts, midnight/report crossings and reaction/unit/grid mismatch refusal. HDF5 histories match every value, attribute and schema exactly; file allocation addresses may differ after reopening. Surface-species report continuation, binary/combined companions and WALL species remain guarded for this managed mesh profile.

This profile owns no private subcatchment sources or live network chemistry. LEGACY, EULERIAN_ARD and LAGRANGIAN network pollutant/age sessions remain refused because their routing quality state is not included in the hydraulic checkpoint owner. Surface-species/buildup output continuation and reactive continuation with live network or private catchment owners remain unqualified. Component contact tests do not remove these full-session restrictions. See the [session checkpoint tests](../../tests/unit/engine/test_engine_checkpoint.cpp).

## Qualification and remaining work

Tests cover exterior edge forcing on triangles/quads/mixed meshes, reversal, donor exhaustion, molecular diffusion at rest, layered initialization/sorption, moving-table uniformity, collapse/rewet, analytical retarded-column breakthrough refinement, A→B reaction closure, full-precision persistence and porous boundary energy. Solute tensor dispersion includes transverse dispersivity, harmonic normal material resistance and a deferred nonorthogonal correction, with an explicit solute diffusion timestep bound. The porous temperature row retains its separately qualified mechanical mixing/conduction operators. Linear skewed-field, material-interface and coordinate rotation/reflection tests exercise its flux contract. Unequal-grid SIGMA/porous tests cover conservative initialization, table motion, finite surface/channel contacts, cold inflow, age, local temperature/sorption kinetics, protected exports and native restart. A smooth Neumann diffusion fixture converges over three spatial/time refinements; a separate runtime tracer case responds to transverse dispersivity. A bounded stencil limits explicit exchanges; this is not a general second-order claim for distorted or discontinuous field models.

Integrated CPU gates additionally cover 20 combinations of conduit/node/well/edge withdrawals sharing one finite donor (three hydraulic closures, bulk/layered chemistry, sorption and porous heat). Unequal groundwater clocks on triangle, quad and mixed meshes preserve pending cold contacts and lateral heat through an exact component restart. Interior-layer injections exercise compression cascades before full saturation. Deliberately omitting a boundary water debit or a compression species transfer makes the corresponding tests fail.

A matched-interface diffusion eigenmode uses discontinuous porosity and diffusivity on a smoothly skewed mesh. Refining from 8 to 16 to 32 cells per axis reduces area-weighted concentration errors from `4.14e-4` to `1.12e-4` to `2.81e-5`; the closed species residual stays below `4e-16`. This specific smooth, aligned-interface case does not establish the same order for arbitrary distorted meshes or tensor discontinuities. The seven targeted groundwater/heat/source/ET suites pass 238 tests in the strict-floating-point CPU build.

CPU integrated qualification remains in progress. Broader contact/application and heterogeneous-plume cases, full-session network chemistry/reactive continuation, RK2 transport/heat/layered and borrowed-owner transactions, and other accelerated groundwater kernels remain open. The Kokkos OpenMP surface comparison passes at one and four host threads. Fifteen groundwater/backend-request combinations preserve canonical host state with a CPU surface marcher; the optional raw-recharge execution status is reported separately. Only the named host OpenMP raw-recharge operator is implemented; other accelerated groundwater operators and CUDA/HIP/SYCL execution remain unqualified; no matching device runner is configured in the inspected repository workflows. Synthetic conservation tests do not establish universal field validity.

See the [kernel tests](../../tests/unit/engine/test_gw_transport_kernel.cpp), [SIGMA conservation tests](../../tests/unit/engine/test_sigma_conservation.cpp), [authoring tests](../../tests/unit/engine/test_gw_transport_authoring.cpp), and [roadmap](../../ROADMAP.md).
