# Spatial groundwater transport

This development implementation extends the CPU, first-order mesh aquifer. It does not apply to conventional subcatchment `[GROUNDWATER]` aquifers. Build from the current source; published packages may lag these interfaces.

## Supported profiles

| Profile | Authoring | Storage and execution |
|---|---|---|
| Bulk groundwater | Default `UNSAT_TRANSPORT BULK` | SAT and mixed UNSAT inventories; existing closures and defaults |
| Layer chemistry | `UNSAT_TRANSPORT LAYERED` | SIGMA on every covered cell; aqueous plus sorbed layer inventories; UNSAT is their derived sum |
| Exterior edges | `[2D_AQUIFER_BOUNDARIES]` and `[GW_BOUNDARY_QUALITY]` | HEAD or FLOW hydraulics; advective composition and independent mass/heat fluxes |
| Aquifer reactions | `[REACTION_SUBSURFACE]` | Explicit SAT/UNSAT/LAYER expressions using the shared reaction integrator |
| Porous soil heat | `THERMAL_MODEL POROUS_EQUILIBRIUM` | Fixed-depth thermal/porewater owner; boundary water, species and physical joules are booked once |

LAYERED chemistry can use `THERMAL_MODEL WATER_ONLY` or `POROUS_EQUILIBRIUM`. With porous heat, SIGMA owns solute/age mass and fixed physical layers own thermal energy; a sparse conservative overlap map connects their independently chosen layer counts. Bulk porous SAT/UNSAT reactions retain their fixed-depth inventory owner. LAYERED requires spatial `MODE MESH`. Coupled surface reconstruction order 2 and accelerated groundwater execution remain unsupported. An automatic backend request can select CPU; an explicitly incompatible backend is refused.

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

Groundwater transport state checkpoints include overlap geometry, protected exports and pending contact increments. Incompatible thermal/chemical grids and unpaired mapping receipts fail before changing the target. This qualification covers the groundwater state serializer. Standalone thermal hotstart save/apply remains refused by the existing carrier guard; qualified engine checkpoints retain their separate owner restrictions. General engine-calendar/control/output continuation remains a separate gate.

## Results and restart

C/Python expose canonical per-layer water volume, total inventory and dissolved concentration, along with gross boundary receipts. INP and GeoPackage preserve the new sections at full double precision. The GUI provides staged edge/quality rows, BULK/LAYERED selection, valid layer initial assignments, undo, and species/layer result selection.

With groundwater reporting enabled, HDF5 adds:

- `Mesh2_face_gw_layer_mass` and `Mesh2_face_gw_layer_conc`: `[time,species,layer,face]`, with species identity, units and explicit axes.
- `groundwater_boundary_water`: `[time,boundary,term]`, gross requested/accepted inward/outward m³.
- `groundwater_boundary_species`: `[time,species,boundary,term]`, advective and imposed inward/outward amounts.
- `groundwater_transport_ledger`: `[time,species,term]`, the extended public ledger order.

Existing bulk datasets and the legacy 15-term species ledger retain their dimensions. Internal solute amounts use native concentration × m³: for mg/L, multiply by 1000 to obtain mg. Thermal forcing uses the configured physical water heat capacity; porous solid energy remains owned by the thermal model.

New transport profiles use native V17 hot-start state with layer inventory, pending receipts (including receiving layers and gross outgoing water reservations), clocks and boundary diagnostics. Model geometry, species/units, forcing and reaction identity are checked before publication. Legacy hot starts refuse these profiles. A native hot start restores hydraulic/quality state; it is not a promise of complete engine-calendar, control and output continuation for every coupled configuration. Existing engine checkpoint profiles retain their own capability restrictions.

## Qualification and remaining work

Tests cover exterior edge forcing on triangles/quads/mixed meshes, reversal, donor exhaustion, molecular diffusion at rest, layered initialization/sorption, moving-table uniformity, collapse/rewet, analytical retarded-column breakthrough refinement, A→B reaction closure, full-precision persistence and porous boundary energy. Solute tensor dispersion includes transverse dispersivity, harmonic normal material resistance and a deferred nonorthogonal correction, with an explicit solute diffusion timestep bound. The porous temperature row retains its separately qualified mechanical mixing/conduction operators. Linear skewed-field, material-interface and coordinate rotation/reflection tests exercise its flux contract. Unequal-grid SIGMA/porous tests cover conservative initialization, table motion, finite surface/channel contacts, cold inflow, age, local temperature/sorption kinetics, protected exports and native restart. A smooth Neumann diffusion fixture converges over three spatial/time refinements; a separate runtime tracer case responds to transverse dispersivity. A bounded stencil limits explicit exchanges; this is not a general second-order claim for distorted or discontinuous field models.

CPU integrated qualification remains in progress. The complete contact/donor and heterogeneous/plume qualification matrix, general coupled exact continuation, coupled RK2 stage/rollback/order verification and accelerated groundwater profiles remain open. Synthetic conservation tests do not establish universal field validity.

See the [kernel tests](../../tests/unit/engine/test_gw_transport_kernel.cpp), [SIGMA conservation tests](../../tests/unit/engine/test_sigma_conservation.cpp), [authoring tests](../../tests/unit/engine/test_gw_transport_authoring.cpp), and [roadmap](../../ROADMAP.md).
