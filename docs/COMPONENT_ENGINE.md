# Native OpenSWMM engine component

The optional `openswmm_engine_component` module publishes component information
ID `org.hydrocouple.openswmm.engine`. It uses HydroCouple interfaces and the
public OpenSWMM C API; it does not link HydroCoupleSDK. The older
`openswmm.model` module remains the separate FVQual compatibility adapter.
Build with `OPENSWMM_WITH_HYDROCOUPLE=ON`. Qualified execution includes CPU 1D
and the bounded CPU spatial and managed transport profiles below. Independent network and surface roles also support the CPU node/surface water phase contract.
Mesh groundwater node/conduit water exchange, classical NODE LIDs and the reviewed water-only joint Richards profile are available in bounded split profiles. Classical mesh, sealed and current bottoms and mixed classical/Richards columns retain native preparation and hydraulic ownership. Reviewed source models still enforce native eligibility guards for generic groundwater beds and conduit seepage. See the version 8 scope in the water phase contract. Coordinated restart remains open.

## Configuration and inspection

Set `hydrocouple.instance_id` before initialization, then set the required
`configuration` argument using JSON or a local JSON file. JSON `{"@uri":"..."}`
also references a local file. Relative paths in inline configuration resolve
against the host's reference directory; paths in a file resolve against that
file's directory.

```json
{
  "schema": "openswmm.component/1",
  "role": "complete_model",
  "model": "model.inp",
  "output_directory": "out/native",
  "outfalls": ["O"],
  "exchange_times_s": [60, 120, 600]
}
```

`complete_model` retains native ownership of configured processes. Surface meshes
and MESH groundwater can run through the water-only profiles below. PER_SUBCATCH
groundwater remains refused. `network_only` requires no
subcatchments or surface mesh. Choose `split_network` and `integrated2d`
for the independent water-only execution arrangement. The component roles,
connection and units are specified in [the water phase contract](SPLIT_WATER_PACKET.md).
Native watershed, snow, LID and legacy groundwater processes in a complete model
are not replaced by a second component driver. Their detailed coupling and
parity matrices remain to be qualified.

The `native_cpu_surface_water/1` profile supports a complete model with triangle
and/or quad surface cells and the built-in HDF5 results writer. It requires
authored `BACKEND CPU`, `RECONSTRUCTION_ORDER 1`, `IGNORE_QUALITY YES`, heat
transport disabled, no reaction species or explicit groundwater, and all four
surface `TRANSPORT_POLLUTANTS`, `TRANSPORT_MSX`, `TRANSPORT_AGE` and
`TRANSPORT_TEMPERATURE` options set to NO, with `BED_HEAT_MODEL NONE`.
The wrapper preserves those settings and the native schedule. Other spatial
configurations and selected external outfall exports are refused. A non-CPU
`OPENSWMM_2D_BACKEND` environment override is refused before creating run outputs.

The `native_cpu_integrated_water/1` profile adds native MESH groundwater to that
surface-water profile. It requires `FLOW_ROUTING DYNWAVE`, at least one aquifer
row, `SOIL_CHAR GARDNER`, `CLOSURE CLOSED_FORM`, `NODE_ENROLMENT ROWS` and
`GW_ET NONE`. Any per-row soil or closure override must also select Gardner or
CLOSED_FORM respectively. All four groundwater `TRANSPORT_*` options must be NO,
with `THERMAL_MODEL WATER_ONLY` and `CONDUCTION NO`. Subcatchments, conventional
LID usage and node-attached LIDs remain refused in this profile.
The native integrated owner retains infiltration, node intake/release, conduit
losses/gains, groundwater pending inventory and SHARED or INDEPENDENT clocks.
The wrapper neither creates an additional groundwater scheduler nor exports
these internal exchanges as component transfers. Other closures, groundwater
evapotranspiration, spatial transport and watershed/LID source ownership need
separate qualification.

Set `"managed_session": true` to use the existing native managed-session builder
for reviewed source ownership and aquifer profiles. `native_cpu_managed_water/1`
keeps watershed source clocks, conventional LID deliveries, node LIDs and the
surface/aquifer owner inside one native engine. Current parity cases cover
CLOSED_FORM, ENSLAVED and SIGMA with SHARED/INDEPENDENT clocks, including joint
Richards `BOTTOM MESH_AQUIFER` acceptance. Ownership metadata comes from the
public authoring preview: revision, validation diagnostics, objects and SI area
shares. Managed execution requires `complete_model` without external outfall
ports; native preparation retains its own compatibility checks. This is a whole
engine component, not split execution.

`native_cpu_managed_transport/1` additionally qualifies first-order CPU mesh-only
models with matched surface/GW pollutant, water-age and temperature layouts,
without nodes, links, subcatchments or reaction species. The porous thermal
profile uses `HEAT_MODEL COUPLED`, `BED_HEAT_MODEL GROUND_COLUMN` and groundwater
`THERMAL_MODEL POROUS_EQUILIBRIUM`. The six current thermal cases cover the same
three closures and two groundwater clocks. Tests require evolving tracer/age/
temperature state, native water/species continuity, byte-identical binary output
and equality of every HDF5 dataset/attribute through the direct component, CLI
and simultaneous Composer instances. The species temperature ledger is °C·m³,
not a substitute for the porous soil/water energy inventory. Second-order mesh
transport remains refused by this component profile. Spatial MSX exports and
arbitrary network/watershed transport combinations remain unqualified.

Native CLI `openswmm --managed model.inp model.rpt model.out` uses the same native
builder. Each run needs fresh private output paths; authored outputs are preserved
by component path isolation. Inspection creates no managed session or results.

Initialization parses the configured model without preparing or advancing a
simulation, loading runtime `[PLUGINS]`, or writing results. `model_metadata`
is a scalar String containing `openswmm.component-metadata/1` JSON: stable node
and link IDs/endpoints, pollutant units, reaction BULK/WALL kinds and declared
units, process registrations, transport policy, clocks, legacy aquifer count,
explicit groundwater mode/configuration and surface counts. The additive spatial
catalogue includes:

- `spatial`: declared CRS (null when absent), project length units, parsed-stage
  provenance, and zero-based mesh indices. Node/link/subcatchment coordinates
  remain as authored; a declared CRS is not inferred from the flow units.
- `nodes[].coordinates`, `links[].coordinates`, and `subcatchments[].polygon`:
  node locations, endpoint/interior polylines and watershed boundaries. An
  unlocated node or link endpoint is null, distinct from an authored `(0, 0)`.
  Incomplete polylines are inspection records, not valid GeoJSON geometries.
- `surface`: mixed triangle/quad connectivity, vertices normalized to metres,
  vertex-to-node connections and every cell-to-node row (including repeated
  cells), with exchange areas in square metres. `source_scale_to_m` accounts
  for project units and a mesh's SI declaration. Mesh indices are model-local,
  and must not be reused across a remesh.
  `boundaries` lists exterior cell/local-edge prescriptions, including default
  WALL edges, using native endpoint incidence rather than unresolved neighbour
  slots. Constant stages use `head_m`; stage-series references retain project
  length units even for an explicitly SI mesh. Constant flow uses `flow_native`
  and `<FLOW_UNITS>/m`; flow-series and rating-curve records identify their units
  without sampling a value. `boundary_quality` retains species IDs and native
  concentration values. These records do not validate manifold topology.
- `legacy_groundwater`: separate aquifer definitions and actual subcatchment /
  aquifer / receiving-node connections, owned by native watershed hydrology.
- `explicit_groundwater`: MESH or PER_SUBCATCH mode, authored scopes, node beds,
  conduit beds and enrollment options. PER_SUBCATCH remains explicit groundwater;
  its geometry source is the subcatchments. AUTO locations remain unresolved
  until native initialization. Automatic rows and resolved runtime mappings are
  not synthesized during inspection. Properties ending `_project` use project
  units: lengths ft/m, areas ft²/m², conductivity in/hr or mm/hr and alpha 1/ft
  or 1/m. The native integrated surface/groundwater owner retains internal exchange.
  `boundaries` retains HEAD in project ft/m and FLOW in m³/s, plus series names.
  `boundary_quality` retains CONC/MASSFLUX kinds and native species values.
  `sources` retains CELL/TAG/GLOBAL scope, authored XY where present, scale,
  m³/s flow or series references, and native CONC/MASS species terms. XY retains
  authored project map units. Clock options describe the configured
  shared/independent groundwater clock; they do not enable a separate component
  step. Options include the global closure and soil characteristic;
  `transport_options` records the groundwater transport and thermal switches.
- `snow` and `lids`: effective degree-day/UEB pack selections, conventional LID
  usage, node assignments, classical/Richards flow choice and bottom ownership.
  `lids.node_outlets` identifies link IDs, one-based layer anchors and TOP/BOTTOM.
  These describe authoring; the native parser's compatibility gates still apply.

These records support connection inspection, without advertising new execution,
remapping, species conversion, or restart capabilities. Broader weather/forcing
catalogues and resolved connection validation remain separate work. Check `runnable` and
`execution_restrictions`; metadata inspection can succeed for a model whose
execution is not qualified. Runtime plugins and model-authored additional output
channels are refused until their metadata/output isolation is qualified.
For the spatial profiles, `execution_profile` names the profile and
`result_files.surface` names `model.2d.h5` under the instance output directory.
The authored 2D destination is preserved during inspection and replaced on the
private runtime handle. Other authored output channels retain their restrictions.

The same inspection path is public independently of HydroCouple:

```python
from openswmm.engine import Solver

solver = Solver("model.inp")
try:
    solver.open_metadata()       # C API: swmm_engine_open_metadata
    print([node.id for node in solver.nodes])
    print(solver.spatial.node_has_coord("J"))  # preserves a valid origin
    print([species.name for species in solver.reactions.species])
finally:
    solver.destroy()
```

Metadata handles are OPENED and cannot initialize/start a run. Close and open
normally to simulate. The parser uses strict validation even if lenient editor
opening was selected. Inspection may read referenced inputs/configuration files;
it does not evaluate runtime chemistry or prove that all runtime data are valid.

For parsed edge inspection through C, call `swmm_2d_prepare_for_edit()` then
`swmm_2d_edge_is_exterior()`. Python exposes the same operations through
`solver.surface2d.prepare_for_edit()` and `edge_is_exterior(cell, edge)`.
Preparation materializes authored boundary storage; it does not start a solver
or resolve neighbour topology. Indices follow the native local-edge convention,
including quads; invalid cell/edge indices are rejected.

Executable OPENED handles can bind a fresh absolute 2D results path with
`swmm_2d_set_results_file()` or Python `surface2d.set_results_file(path)`.
This updates the unprepared writer as well as its stored path, adding the writer
when absent. Start opens the destination exclusively. Binding creates no file;
metadata-only and initialized/running handles are refused. The generic
`swmm_file_path_set` edits the stored authoring slot and does not rebind a writer.

## Execution and outputs

Each run requires a **fresh, nonexisting** `output_directory`. Preparation creates
it and refuses an existing directory without overwriting its contents. It checks
that the main model and directly registered process configuration files have not
changed since inspection. Keep other referenced forcing inputs immutable for
the duration of inspection/run; nested input snapshotting is not implemented.

Without selected outfalls, the component follows native `step()` scheduling and
supports the existing reaction engine. Native `.out` results retain ordinary
pollutant columns, native age/temperature columns where enabled, and appended
MSX species in native order. The wrapper does not remap or synthesize species
values. A zero-volume flow-through junction does not execute tank reactions;
storage and pipe values follow the native chemistry owners. Full WALL/BULK unit,
solver, report-boundary and output-continuation qualification remains open.

`model.rpt` and `model.out` are under the run directory. Uncoupled native and
component `.out` files are byte-identical for the tested 1D pollutant and MSX
models. Independent instances have separate engine handles, clocks and result
directories. Cloneability and checkpointing are not advertised.
Surface-water runs additionally write isolated `model.2d.h5` results. The
mixed-mesh native/component fixture retains `.out` byte identity and exact HDF5
dataset/attribute equality, including nonempty evolving report values. Two
interleaved instances retain independent files with matching scientific results.
Integrated-water fixtures cover both groundwater clocks, measurable infiltration,
both directions of node and conduit exchange, US project units and an SI mesh
inside a US project. Each configuration is compared against its own native
schedule, with native groundwater continuity checked separately. The direct
component, native command line and Composer results match exactly in `.out`
and all HDF5 datasets/attributes. These fixtures qualify the bounded profile;
they do not close the complete whole-engine qualification matrix.

## Typed external-outfall transfers

Selected IDs must identify external native outfalls, without native subcatchment
rerouting. This initial transfer profile requires LEGACY quality, enabled native
heat transport, and mass pollutants in MG/L or UG/L. Exported amounts are kg,
m3 and J from committed native outfall receipts. MSX exports, count-based
pollutants and reverse outfall flow are refused. The native enthalpy reference
is water relative to 0 degrees C (the current native heat receipt convention);
receivers must use that convention.

The profile is `hydrocouple.interval-transfer/1`. Each scalar output is named
`transfer_id`, `epoch_julian_day`, `interval_start_s`, `interval_end_s`, `water_m3`,
`enthalpy_j` or `species.<native-pollutant-ID>`. Coordinates are intensive, amounts
extensive; epoch days have SI factor 86400. Species identity is the stable native
ID, not its row number. Mass basis is kg of the declared pollutant; the wrapper
does not infer elemental nitrogen from an ID. All selected outfalls aggregate
into one donor interval tuple.

`exchange_times_s` must increase from zero and end at native model duration.
The first interval starts at zero; `advance_to` uses the native millisecond
clock. Use endpoints representable on that clock. Equal requested schedules
produce byte-identical native/component `.out` results in the qualified fixture;
changing the physical step schedule is a separate numerical comparison.

Connect every interval field once to one direct receiver. Its scalar outputs
must include `accepted_transfer_id` and `accepted.<amount-field>` with matching
units and dimensions. Adapters, fan-out and splitting among receiving owners
are not qualified in this profile. The reference sink in HydroCoupleComponents
provides these outputs and accepts species declared in a different order.

The physical export is **already debited by the native engine**. The component
only publishes the receipt; it must never debit the donor again. Python can inspect the same native ledger through
`solver.nodes["O"].outfall.export_receipt`: a copied mapping with cumulative
`exported_m3`, `reversed_m3`, `exported_heat_j`, capability `flags`, and
`pollutant_amounts` / `pollutant_amount_units` dictionaries keyed by native ID.
Before the next
advance, and at finish for the final interval, it checks the receiver's token and
accepted amounts. Full acceptance clears `unreceived.<amount-field>`; partial or
missing acknowledgment stops advancement. A proportional partial acceptance is
recorded, with the residual remaining explicitly unreceived. This profile cannot
refund/retry a native export or return rejected water to the network. It is not
the trial/acceptance protocol required for surface/GW/LID hydraulic coupling.
With no connections, the selected outfall remains a normal external boundary.

The run directory records `configuration.json`, `metadata.json`, per-interval
`native_boundary_N.json`, `export_N.json` and validated `accepted_N.json`. Native
cumulative receipts, exported totals and per-transfer receiver acceptance are
distinct. Keep these ledgers when diagnosing failure. A missing acknowledgment
has no accepted ledger; its last export remains outstanding.

## Runnable example and tests

Copy [the example](../examples/hydrocouple/native-outfall/composition.json) and
its `model.inp` to a run folder. Start Composer with both the engine module
directory and the HydroCoupleComponents reference-library directory:

```text
HydroCoupleComposer --run composition.json --components <engine-modules> --components <reference-modules>
```

The saved composition uses three intervals and a reversed receiver species
declaration order. It writes native results, a receipt CSV, recorded outputs and
a run manifest below `out/`. Choose a new run folder/output directory for reruns.
The reference sink is `org.hydrocouple.openswmm.test.sink`, with scale 1.

Native acceptance tests are `test_engine_component_metadata`,
`test_engine_component`, and `test_engine_output_quality`. Configure
`OPENSWMM_COMPONENT_RECEIVER_LIBRARY` to the built reference sink to include
the actual receiver tests. Python covers metadata inspection and normal reopen.
Composer's optional `COMPOSER_OPENSWMM_ENGINE_COMPONENT_LIBRARY` and
`COMPOSER_ENGINE_COMPONENT_FIXTURE`, alongside its existing custom-library
paths, enable configured metadata and actual native-engine/receiver workflow
tests. Current qualification is macOS arm64 Release, CPU; these tests do not
establish relocated-package, other-platform or coordinated-resume compatibility.

## Component contract inventory

This inventory distinguishes native operations from their HydroCouple exposure.
It is a source inventory, not a claim that the complete component gates pass.
Surface and explicit groundwater retain one native integrated owner. A metadata
catalogue describes authored configuration; it does not grant execution capability.

| Exchange group | Native public operation / owner | Component exposure and remaining work |
| --- | --- | --- |
| Identity and topology | `swmm_engine_open_metadata`, `swmm_spatial_node_has_coord`, `swmm_2d_cell_get_vertices`; native parsed model | Implemented inspection and portable `hydrocouple.instance_id`. General geometry revision/remapping validation remains missing. |
| Boundary configuration | `swmm_2d_get_edge_bc_type`, `swmm_gw2d_boundary_get`, `swmm_gw_boundary_quality_get`; native surface/GW boundary owners | Mapped-outfall water phases are exposed through the split transaction. General edge-boundary component ports remain open. |
| Hydraulic boundary state | `swmm_node_get_depth`, `swmm_coupling_set_sources`, `swmm_2d_set_runtime_boundary`, `swmm_gw2d_set_runtime_boundary` | Native calls exist; general component hydraulic input/output ports are missing. A head override does not itself transfer inventory. |
| External source/forcing | `swmm_coupling_apply_frame`, `swmm_gw_source_get`; native source staging and receipts | Native copied atomic frames and source inspection exist. No generic HydroCouple provider bridge or automatic physical-unit conversion is implemented. |
| Transfer offer/reservation | Native surface/GW/node/link and Richards owners | The integrated2d role handles qualified water-only node/surface and mapped-outfall phases. Version 4 carries generic groundwater node/conduit water, version 5 reviewed source pools, and version 6 reviewed joint Richards trial/admission. No wrapper recreates these owners or advances chemistry twice. Broader return and reactive profiles remain open. |
| Accepted outfall transfer | `swmm_node_get_export_receipt`; native outfall debit | Implemented scalar `hydrocouple.interval-transfer/1`, one direct receiver, water/pollutants/enthalpy. MSX, fan-out and refund/retry are refused. |
| Accepted source/boundary transfer | `swmm_coupling_get_receipts`, `swmm_coupling_get_boundary_receipt`, `swmm_coupling_get_infiltration_receipt`; native receipt owners | Native receipts exist. General typed component ports and split phase equivalence remain missing. |
| Inventory and diagnostics | `swmm_2d_get_species_ledger`, `swmm_gw2d_boundary_species`, native `.out` / HDF5 owners | Native diagnostics and wrapper result files exist for qualified profiles. Diagnostics never constitute a second physical storage or debit. |
| Time | `swmm_engine_advance_to`, `swmm_coupling_advance_to`; native solver calendars | Qualified split roles retain native calendars and accepted-boundary phase traces, including reviewed sources and joint Richards. Broader unequal-step and reactive scheduling remain open. |
| Recovery | `swmm_checkpoint_save`, `swmm_checkpoint_restore`; supported native session owner | Native profile-specific recovery exists. Wrapper/graph recovery is missing; `checkpointing` remains false. |
| Native hosting | `swmm_component_library_count`, `swmm_component_search_path_add`; library discovery | Discovery is implemented. Library-backed `[PROCESS_COMPONENTS]` lifecycle, connections and scheduling remain missing. |

The matching executable witnesses are `test_engine_component_metadata`,
`test_engine_component`, `test_engine_process_component_loader`,
`test_engine_output_quality`, `test_engine_runtime_coupling_api` and
`test_engine_checkpoint`. Their profile-specific results must be recorded for
the source being qualified; a symbol in this table alone is not acceptance.

## Runtime forcing semantics at a component boundary

The legacy forcing API is not a uniform conservative amount API. The following
classification follows `openswmm_forcing.h` and its current native consumers.
New component connections must convert explicitly and use accepted receipts for
physical inventory. Never treat a concentration assignment as delivered mass.

| Operation (`swmm_forcing_` prefix) | Meaning and native value units |
| --- | --- |
| `node_lat_inflow` | Overlay on user lateral flow, ft³/s in every project. OVERRIDE replaces that contribution; other authored inflows remain independently assembled. |
| `node_head_boundary` | Outfall stage override in datum ft. ADD is accepted by the setter but not consumed as a stage addition. No physical amount receipt. |
| `node_quality`, `link_quality` | Direct concentration overlay in pollutant-native concentration units. ADD currently adds concentration; it is not a conservative mass-rate injection. Use `swmm_node_set_quality_mass_flux` for the supported node amount-rate path. |
| `node_temperature` | OVERRIDE assigns °C; ADD supplies °C·ft³/s through the loader accumulator. Requires heat enabled. |
| `node_age` | OVERRIDE assigns hours; ADD supplies hours·ft³/s, converted internally to seconds. Requires age enabled. |
| `link_flow` | Pre-physics flow-state overlay in ft³/s. Routing may subsequently change it; it is not a sustained solver boundary. |
| `link_setting` | Current/target opening or speed overlay, dimensionless. Native controls remain active. |
| `link_seepage` | Conduit exchange in ft³/s, positive out of the conduit. Internal TWO_WAY/AUTO aquifer exchange also debits/credits its aquifer. An external owner must not debit that same aquifer again. |
| `subcatch_rainfall`, `subcatch_evap`, `subcatch_snowfall`, `gage_rainfall` | Weather-rate overlays in project precipitation units (in/hr or mm/hr), converted by native hydrology. |
| `climate_temperature`, `climate_wind`, `climate_evap` | Global weather overlays: °F/°C, mph/km/hr, and in/day/mm/day for US/SI respectively. Channel consumption follows the native weather owner. |
| `element_climate` | Per-element heat-climate override; values and parameter units follow the heat-climate parameter code. This is environmental forcing, not transferred water/energy. |
| `clear`, `clear_all` | Remove prescriptions. RESET expires after the consuming physical step; PERSIST lasts until replaced or cleared. Clearing is not a refund of an already accepted amount. |

The SI `swmm_coupling_*` frame/source/boundary family is a separate contract:
water m³/s, signed receipts m³, species in the target domain's declared order
and units, and heat J where supported. Its capability flags and profile guards
must be checked before attaching a provider. Neither this inventory nor native
forcing tests qualify a new HydroCouple connection or close the split scheduling
and double-debit tests required by the component finalization plan.

## Experimental native split phase harness

`openswmm_split.h` supplies an in-process native-peer interface for the first
independent network/surface test. The component adapters layer the versioned
JSON water phase contract on this interface using `openswmm_split_packet.h`.
The test evidence for the component and Composer runs is in
`tests/verification/component_split_adapters_2026-11-03`.

Each participant is a separate engine created and opened from the same fixed
original INP and configuration. Select its role in OPENED before binding fresh
output paths. The network disables embedded surface advancement. The surface
disables 1D routing and binary result writing, retaining the existing surface
solver, rainfall history, batching and mesh writer. Its copied node state is a boundary
view; it never advances a second 1D inventory. Input must omit `OUTPUT_FILE`;
bind the surface destination with `swmm_2d_set_results_file` after role selection.
The network refuses a mesh writer and the surface refuses a binary destination.

Initialize the surface, export its native footprint geometry, initialize the
network and import that geometry before start. Install the synchronous network
phase callback. Start the surface with save-results 0, then start the network.
The callback forwards each borrowed request to `swmm_split_evaluate` on the
matching surface and returns its newly owned reply. The network consumes replies.
`test_engine_split.cpp` uses only the public C API for engine execution and
provides the executable lifecycle example.

Conductance is evaluated by the existing native law inside every hydraulic
iteration without advancing surface time. ADVANCE calls the existing surface
marcher at the native post-routing boundary with the exact routing interval.
Native queue amounts and remaining delivery spans return to the network without
an SI round trip. Report-boundary flushes are retained with and without a mesh
writer. FINISH invokes native finalization, including any partial pending batch.
Inspection returns head/depth in m, queued/direct pending amounts in m3 and time
in seconds. Amount signs retain the native queue convention: positive credits
the network and negative debits it. These snapshots are not generic gross
water/species/energy offers or independent transit inventories.

Frames are immutable owned snapshots from the same loaded engine binary; they
share no engine context. Configuration, participant identities, phase, sequence
and time must match. Borrowed requests cannot be returned as owned replies.
Failures stop without retry or rollback; physics already performed is not undone.
Callback exceptions are caught inside the hydraulic team's callback boundary
and reported after that boundary, preserving the first failure on subsequent
steps. Caller access is exclusive. The first profile requires `THREADS 1` and
refuses a multi-thread `SWMM_DW_THREADS` override.

The bounded profile is CPU first-order water-only DYNWAVE with explicit node
continuity and footprint ponds. Ordinary watershed and conventional LID outlets
use the network as source owner. Unsupported reviewed source profiles, node LIDs, snow,
RDII, transport/heat/age, unsupported groundwater profiles,
tracking ponds, event skipping, report averaging, plugins,
process rows, file interfaces and restart are refused. Geometry validation
precedes advancement. Ordinary surface stepping and both roles' `advance_to`
are unavailable. Managed checkpoint attachment is also unavailable: graph
recovery must qualify every participant and pending exchange together.

Evidence in `tests/verification/component_split_2026-11-03` covers native versus
independent split `.out` equality, per-step water ledgers and full HDF5 datasets
and attributes. Cases include draining, flow reversal/spilling, competing
withdrawals, dry donors, adaptive routing, report boundaries, binary-only output
and early finish with a pending batch. This initial witness does not qualify `.rpt` identity,
portable HydroCouple orchestration, R4 sources, R5 groundwater/joint Richards,
transport, checkpoint/retry, other backends or the platform/install matrix.

Mapped outfalls additionally use the version 2 water-phase contract.
A pre-routing query retains native tailwater caches and withdrawal
reservations, followed by the accepted native exchange at the post-routing
boundary. The outfall increment covers pure outfalls, mixed node/outfall
connections, both flow directions, exhausted and overlapping donors, flap
gates, spread, relaxation, multiple barrels and early finish. Its native,
direct-component and Composer evidence is under
`tests/verification/component_split_outfalls_2026-11-03`.


Version 3 extends independent splitting to ordinary watershed and conventional
LID outlets through native network routing. The network alone runs runoff and
LID clocks. It sends each native gage history sample, report cursor and climate
evaporation to the surface, which reuses native rainfall history without
advancing another watershed. Native/direct component binary results, every
surface and routing ledger, and full HDF5 datasets/attributes are checked in
`tests/verification/component_split_sources_2026-11-03`. Direct reviewed R4
footprint delivery and R5 joint aquifer transactions remain closed.


Version 4 carries the native mesh groundwater node ownership flags, conduit
budgets and accepted signed seepage between the network and surface owners.
The surface alone runs groundwater, including shared or independent clocks and
pending cohorts. Gardner CLOSED_FORM, ENSLAVED and SIGMA water profiles with
GW_ET NONE are supported. The native node law, storage exfiltration suppression,
ONE_WAY/TWO_WAY conduit laws and internal surface/groundwater exchanges are
reused. No new groundwater solver or exchange law is introduced. Tests and
qualification are retained under
`tests/verification/component_split_groundwater_2026-11-03`.
Version 4 does not carry reviewed R4 source pools or R5 joint Richards
trial/admission. Version 5 adds the bounded R4 profile described below;
joint Richards still requires a separate paired transaction.


Version 5 keeps reviewed source pools, the common source/aquifer stage and their
native delivery bridge in `integrated2d`. The receiving network suppresses owned
ordinary runoff/LID kernels and requests the existing native staged outlet
rates before its hydraulic solve. It retains native source-rate corrections,
accepted balances and inventory diagnostics without a second source driver or
queue. Mesh results are required for this bounded reviewed profile. Groundwater
node/link beds, mapped outfalls, owned outfall returns and joint node-LID bottoms
remain refused until their additional phase contracts are qualified. See
`SPLIT_WATER_PACKET.md` and
`tests/verification/component_split_footprints_2026-11-03` for the current scope.

Version 6 adds explicit joint Richards trial and acceptance phases for reviewed
water-only source models with MESH_AQUIFER NODE LID bottoms. The existing native
node-admission helper builds private drafts against the surface owner's actual
aquifer budgets. After matching acceptance, the network routes and finishes
native layer ports once; accepted column state returns before native surface/
aquifer advancement. Directional reservations, pending receipts, groundwater
clocks and source pools retain their native owners. All three closures, shared/
independent clocks, competing columns, upward/downward exchange, evaporation
and early finish are qualified. Other node-LID formulations and broader bed/
outfall/return profiles still require separate qualification. See
`examples/hydrocouple/split-richards` and
`tests/verification/component_split_richards_2026-11-03` for the example and evidence.

Reviewed source models additionally support mapped outfall exchange through the
existing native preparation/acceptance phases. Schema version 7 carries the
native-booked outfall return inventory into reviewed source groups, preserving
native return availability and credit-before-bridge-commit ordering. Ordinary
unowned returns stay with network runoff. Accepted hydraulic flows are retained
before native outfall balance bookkeeping rewrites backflow diagnostics. No
second return calculation or source driver is added. A mapped outfall cannot
also send the same discharge into a reviewed source. The native INP reader now
resolves forward catchment references in outfall destinations and refuses unknown
names. See `SPLIT_WATER_PACKET.md` and
`tests/verification/component_split_returns_2026-11-03` for qualification.
