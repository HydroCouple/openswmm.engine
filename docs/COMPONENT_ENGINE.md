# Native OpenSWMM engine component

The optional `openswmm_engine_component` module publishes component information
ID `org.hydrocouple.openswmm.engine`. It uses HydroCouple interfaces and the
public OpenSWMM C API; it does not link HydroCoupleSDK. The older
`openswmm.model` module remains the separate FVQual compatibility adapter.
Build with `OPENSWMM_WITH_HYDROCOUPLE=ON`. Qualified execution includes CPU 1D
and the bounded CPU spatial and managed transport profiles below. Independent network and surface roles also support the CPU node/surface water phase contract.
Mesh groundwater node/conduit water exchange, classical NODE LIDs and the reviewed water-only joint Richards profile are available in bounded split profiles. Classical mesh, sealed and current bottoms and mixed classical/Richards columns retain native preparation and hydraulic ownership. Version 9 adds reviewed ordinary groundwater beds and conduit seepage, including layouts without a surface coupling port. Native complete-model restart retains their real queue ownership and conduit geometry; coordinated component-graph restart remains open.

Common-source water updates retain compensated rounding remainders for small stability-limited steps. Native source checkpoints use profiles 11–20 for these remainders and can read older profiles 1–10 with zero remainders. The precision correction can change final rounding bits relative to older runs; native/component parity is qualified against the same engine build. See versions 8/9 in the water phase contract for the bounded scope.

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

The opt-in `split_material_network` and `split_material_surface` roles expose
`openswmm.node-surface-material.v10`. They use the bounded native passive-storage
pollutant contract described in [the material packet contract](SPLIT_MATERIAL_PACKET.md),
with MG/L, UG/L or #/L basis and native concentration-volume amounts. They are
separate from the water-only roles and schemas. See
`examples/hydrocouple/split-material/composition.json` for a two-component graph.
No HydroCoupleSDK dependency is added to the engine or its component module.

Connect exactly one material surface output directly to one material network
input from this plugin, with distinct instance IDs. Both instances must prepare
before the network advances; either preparation order is supported. They must
have matching authored configuration, ordered geometry, pollutant basis and
initial native stocks. Optional `material_capacity` is an integer from 1 to
65536 (default 4096) and must match on both roles. It bounds the retained native
journal; it is not a packet-size guarantee. The example uses six slots and
then continues through real empty windows after those slots have been used.

The native network owns advancement. Surface `update()` performs lifecycle
scheduling only; native outer-phase callbacks drive its solver. The surface
publishes accepted time only after the network accepts the corresponding step.
Either finish order settles native pending windows before reporting. Public
output pulls cannot drive these callbacks. Prepared connections are frozen;
adapters, fan-out, replacement peers and lifecycle reentry during callbacks are
refused. Destroyed or reinitialized peers invalidate their private generation.
The opaque in-process session owns material authority; a copied JSON packet
does not create an executable endpoint or ownership.

`material_session` is scalar String observational JSON with schema
`openswmm.material-component-session/1`. It reports accepted time/boundaries,
observed OFFER/PROOF/ACK stages, applied drain/spill legs, empty windows and the
first terminal error. These counters do not authorize transfers. A snapshot is
written as `material-session.json` at preparation, finish or failure in the
component's fresh output directory; numerical callback dispatch does no disk
I/O. The network owns `model.out`, while the surface owns optional
`model.2d.h5` (`surface_results`, default true).

Material metadata inspection invokes the native read-only eligibility check,
without starting a session or creating output files. Runtime geometry import
and binding still enforce the executable contract. This profile excludes
general reaction/heat/age transport, arbitrary network or watershed ownership,
coordinated graph restart and remote authentication. Unsupported native
arithmetic or transaction failures stop the session with its original
obligations retained; the adapter does not retry or reconstruct authority.

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

Native pollutant preparation includes a bounded receiving correction: passive
wet storage with unchanged positive volume retains a positive coupling pollutant
amount when both incoming carrier water and coupling water are exactly zero.
The complete assembled signed amount, including ordinary co-loads, joins the
pool after the existing available-mass clamp. Storage LIDs, reactions and
treatment retain their existing mixing rules. This does not qualify split
pollutant packets: negative delivery, funded donor reservations and complete
bidirectional material conservation remain open. Public water packet schemas
remain 1–9.

The internal `PassiveStorageExchange` input additionally qualifies staged
funded receiving through `QualitySolver::execute`. One named native pollutant
and isolated passive storage are supported. The book reserves gross spill
across points and outstanding receipts; future drain mass cannot fund it.
Signed slices enter the single native mixer separately from ordinary loads and
the shared legacy queue. Native interval/span checks, exact gross partitions,
representability checks and terminal failure handling prevent silent drops or
replay. Ordinary concentration divide/product roundoff has an explicit bound
of two machine epsilons of the target amount. No component species schema is enabled.

The staged owner can freeze a delivery cohort before lateral assembly and
observe its post-drain span. ADVANCE rows accepted afterward wait for the next
actual assembly. Historical sample, physical batch, phase and eligibility are
retained; REPORT/FINAL sampling is distinct, and final receipts remain pending
without an extra routing step. Overlapping physical point batches are refused
even under a changed receipt ID. This profile does not admit a producer-side
water-span reset.

Sealed owner archives preserve pending rows, samples and native delivery
history. Restore validates a detached historical input through the native
quality receiver before binding a pristine owner to matching current state.
Frozen/prepared/failed snapshots are refused. Archives are limited to 64 MiB,
4096 nodes/rows/receipts/events and one million validation-work units. This
owner-state archive alone does not restore native water/quality. Staged-only
archives retain their version 1 bytes. Version 2 retains authenticated live
proof, with at most 65,536 proof records and the same bounded replay budget.

An explicitly attached private C++ `LivePassiveStorageExchange` now owns the
sealed book and a bounded, preallocated journal in `SWMMEngine`. The actual
first-order serial CPU marcher authenticates each drain and admits each spill
before its native donor, cell, water and diagnostic writes. Gross spill funding
includes all sealed and open commitments across shared-node points; future drain
mass is unavailable. Zero-concentration carrier legs are recorded. The engine
freezes the prior cohort before lateral assembly, observes the real drain, and
receives through the existing quality solver. Live-owned points bypass the
legacy pollutant queue. Actual firing sites distinguish ADVANCE, REPORT and FINAL.

This is a restricted research attachment with no INP flag, public C API or
component activation. Only isolated, wet, unchanged passive STORAGE pools with
one native pollutant are eligible. RK2, parallel/plugin marchers, incident links,
other storage/source mobilization, event schedules, process hosts, head overrides,
runtime/source/split sessions remain refused. A separate private paired restart
profile supports physical state in a closed first-order CPU model: all network
nodes are mapped passive storage, with no links, catchments, gages, infiltration,
groundwater, heat, external boundary/forcing owners or output continuation. Attach
the live owner first and enable checkpointing at the initial accepted boundary.
Generic hotstart saving and component activation remain refused.

A window seals only when both authenticated gross water and actual native hydraulic
booking are exactly zero net. The private live input preserves each actual SI
donor operand, sample, firing order/duration and native spill withdrawal, then
converts that accepted amount once. It does not recompute aggregate spill mass
from converted carrier water. Staged unauthenticated inputs retain their original
strict carrier identity. Other unrepresentable aggregates and stored increments
still stop the owner. Capacity, funding and source-increment refusals precede the
rejected leg's effects; earlier applied legs survive terminal refusal. Later
`step` and `end` cannot retry failed work, and diagnostics survive until close.

Paired restart stages the existing hydraulic, mesh, marcher and router archives
without a source stage or additional physics. It checks receipt proof against the
incoming physical state before installation or output publication. Initial water
and surface pollutant anchors, per-node native receiver replay (including retired
and never-used pools), actual exchange/donor ledgers, clocks and pending mesh time
must agree. Resolved per-point laws, coefficients, sampling cells and equalization
policy are part of the model identity. Checksum-valid edits to sources, water, material, samples, topology,
conversion, proof and node concentrations are refused atomically. Native engine
profile 3003 is distinct; existing unrelated checkpoint formats stay unchanged.

An authored ponded-storage model now demonstrates two actual transfer directions,
funded pending receipts, byte-identical restored checkpoints and native
continuation. Its live runs with and without checkpoint attachment have identical
clocks, water, material and transfer legs. A separate real marcher witness at
concentration 0.3 preserves a conversion-roundoff difference in the authenticated
archive. Full unequal-concentration signed delivery, broader layouts/RK2, output
continuation, GUI exposure and component species/schema 10 remain unqualified.

Two arithmetic-conditioned authored cases additionally demonstrate positive and
negative native delivery at unequal concentrations, with checkpoint recovery both
while pending and after settlement. They use the native two-step synchronization
window, matching native pond geometry and existing quality mixer. These bounded
witnesses do not establish general eligibility: ordinary 1/2 and 2/1 concentration
cases and later signed windows still refuse unrepresentable aggregates.
Arithmetic reservation refusals are distinguished from insufficient
donor inventory. This strict policy remains the default.

An explicitly selected private `RETAIN_ROUNDOFF` research policy additionally
retains the native receiver's exact rounding difference. For represented native
mass M, realized signed amount delta and owned remainder R, it preserves the
receiver-local identity `Mnext + Rnext = M + R + delta`. The single native mixer
and prepared physical amount remain unchanged; R is never fed back into mixing.
The exact local error must fit two machine epsilons of the native rounded target,
and a whole nonzero increment still cannot disappear. Negative R reduces gross
spill funding, while positive R and future drain cannot increase it. Nonzero R
keeps its pool bound after receipts retire. Receipt archive 3 and live archive 2
preserve policy identity and verify R through the existing native replay before
installation. Empty standalone bytes mean absence of a bound owner, rather than
a saved arithmetic policy. Strict archive bytes remain unchanged. This does not
establish exact global conservation: surface writes and unit conversions retain
their separate rounding behavior and the independent physical inventory check.
This policy keeps gross/net and partial partitions strict; corrective feedback, broader
owners and public/component activation are separate gates.

The separate private `RETAIN_AGGREGATES` policy retains exact node gross and net
material sums across represented rows and receipts. Exact gross accessors expose
the components; existing double getters still refuse a nonrepresentable result.
Live sealed/open spill budgets compare the complete exact amount against both
physical and accountable inventory before credit. Only the signed mixer input is
correctly rounded to binary64, with exact bracketing and ties to even. R retains
the difference against the exact realized net; the full native error is bounded
using the independently rounded exact target `RN(M+D)`. Prior R never joins the
mixer input. Receipt archive 4 and live archive 3 identify these semantics even
with zero R. Each individual point total, water balance and partial partition
remains strict; unsupported point aggregation keeps its applied journal on
terminal refusal. This qualifies node aggregation separately from broader point,
layout, ordinary-source and public/component profiles.

The further private `RETAIN_POINT_AGGREGATES` policy retains material totals
across repeated actual firings at one physical point. One receipt row still owns
one point; its individually converted native proof legs authenticate each exact
direction total. Zero/single-part amounts use the existing scalar fields. For
nonrepresentable totals a sparse canonical expansion is authoritative and the
scalar field is zero. Multipart input requires authenticated native proof. Water
remains strict. A whole native flush consumes the exact material total; a partial
multipart delivery refuses before mixing and retains the original pending proof.
Represented partial amounts retain the prior scalar operation and exact partition
checks. Node funding, the single correctly rounded mixer input and exact retained
roundoff use the same rules as `RETAIN_AGGREGATES`; no component is donor credit.

Receipt v5/live owner v4 identify this policy even at zero remainder. Earlier
policies retain their archive bytes and behavior. Component counts, exclusivity,
nonnegative totals and canonical form are checked under the bounded replay work
budget before native history replay and atomic installation. This remains private
C++ research functionality; no input option, public component species interface,
GUI activation or HydroCoupleSDK dependency is introduced.

The point qualification has two distinct witnesses: both signs of a full window
with two firings per point and pending/retired restart, and a separate nine-window
history with one firing per point per window. The latter reaches 19 native steps
and restarts after five windows. Its next window exceeds the unchanged replay
work budget after producer execution: the applied journal is retained and the
owner becomes terminal. This is not a rollback boundary. Strict water aggregation
also limits subsequent windows of the repeated-firing fixture. Broader water and
partial-delivery arithmetic remain separate qualification gates. Local evidence
is under `tests/verification/component_species_points_2026-11-04`.

The explicit private `preflight_history` attachment option selects live wrapper
v5 with the same point arithmetic and receipt v5. Before a normal engine step
changes readiness, clocks or physics, it reserves the unchanged replay-work,
record and proof limits for one next delivery and a possible surface batch. It
bounds future applied legs by the global journal's remaining capacity, including
potential point components in both accepted and remaining rows. This conservative
estimate can refuse earlier than the actual limit, including at initial state
for a large journal capacity; it does not infer firing count from MAX_TIMESTEP.
A work-limit refusal is recoverable: the owner remains accepted, its transient
diagnostic is available through `historyPreflightError()`, and the preceding
checkpoint can still save, restore and refuse a retry identically. A pending
FINAL flush is independently checked before teardown effects.

This option qualifies a closed engine path. Callbacks are excluded, and one-shot
forcing/boundary edits refuse before they can flush a batch. Earlier wrappers
retain their admission behavior and bytes. Actual journal exhaustion remains a
separate terminal leg failure with its applied journal retained. This admission
check does not relax water arithmetic, partial-delivery eligibility, native
receiving/funding rules or any numerical tolerance. Local evidence is under
`tests/verification/component_species_preflight_2026-11-05`.

The private `RETAIN_EXACT_HALVES` receiver policy additionally permits one
exact half delivery of authenticated point amounts. It evaluates each original
once-converted native leg in the existing `(amount/span)*dt` order and requires
finite, nonnegative, equal slice and remainder with an exact partition. Exact
point water must agree with the scalar water schedule, and each delivered and
remaining node-water sum must remain zero. Material slices retain their original
point/direction attribution. The second half flushes whole; quarter requests,
repeated partial requests and subnormal unequal halves refuse before mixing.
The SI donor proof remains immutable and surface transfers are never replayed.

Receipt v6 retains the first-half marker through settlement. Live v6 and v7
identify this policy without and with history preflight. Native replay derives
both progress and remaining rows before installation. History work includes
per-leg reconstruction and a bound on component growth in pending historical
receipts; all prior policy formats and limits retain their rules.

This qualifies the authenticated marcher/native receiver seam with a supplied
pre-existing span and fixture-driven lateral-drain and clock observations; the
half-delivery fixture does not call the engine's lateral-inflow assembler.
A fresh authored closed model has zero net native water and
does not create a positive partial-delivery span. Whole-engine witnesses of this
policy qualify full delivery only. Water diagnostics separately expose the raw
solver point volumes; a failed seal precedes posting to the network, so an
ordered reconstructed node sum is labeled what would be booked. No water join
is relaxed by this policy, and no public/GUI/component species activation or
HydroCoupleSDK dependency is introduced.

The separate private `enableCoupledWaterCheckpointing()` opt-in supports native
water-only restart for isolated mapped wet storage and a closed first-order
serial CPU surface. It retains the ordinary native marcher, point-volume
booking, lateral-inflow assembly and dynamic-wave routing. Engine archive 3004
and a distinct paired-water surface/model identity separate this physical
snapshot from the passive-material receipt profiles. External source/process
owners, links, groundwater, LID storage, forcing, controls, material, output
continuation and public hotstarts remain excluded. Read-only phase observation
is permitted. Inspection and restore validate detached state before publication;
the snapshot establishes physical consistency and continuation, not historical
reachability or authenticity. For this closed profile, current inflow/outflow
must match the signed coupling rate, node losses must be zero, and the saved
net and incoming flows must agree with the saved lateral rate. Finite but
inconsistent histories are refused before either inspection or restore publishes
state. Ponded overflow remains a diagnostic and need not be zero.

Authored cases with both drain and spill points now qualify positive and negative
net native water booking, genuine two-step queue halves, and fresh-engine restart
at initial, pending, first-half and later refill boundaries. Ordinary and opted-in
runs have identical phase traces; resumed archives and continued state are
byte-identical. The queue consumes the current delivery rate, while isolated
dynamic-wave storage integrates the previous/current net-flow average. Its
endpoint flow memory is therefore required even after a queue drains. The tests
observe empty queues during routing and preserve that memory through later
refills. Two additional authored near-equilibrium cases qualify idle-boundary
restart after the queue empties. Six bidirectional points first drain and spill;
their head differences then fall below the existing ORIFICE cutoff without any
runtime head, queue or clock edits. Both signs recover the zero-queue boundary,
the following hydraulic tail and its settled continuation. With no newly
delivered water, that following step still changes storage by the retained
previous-flow contribution. This is a bounded near-equilibrium witness, not a
general convergence claim. Its phase and exact arithmetic records are in
`tests/verification/component_species_variable_volume_2026-11-06`.

The exact audit distinguishes per-window SI point totals, their rounded native
conversions, ordered node booking and queue consumption from hydraulic volume
change. Depth and volume round independently during repeated native updates;
their geometric residual is accounted for from the observed operations without
adding a tolerance. This does not relax the passive receiver's zero-water gate
or by itself qualify changing-volume pollutant delivery. Earlier
water-booking evidence is retained in
`tests/verification/component_species_water_booking_2026-11-05`.

The private `attachLiveChangingVolumeExchange()` attachment selects a distinct
changing-volume passive receiving law. Each mapped storage retains settled
`M = RN(C * V)` and exact accounting remainder `R`. The existing quality writer
computes `Cnew = RN(RN(M + RN(deltaM)) / Vnew)` using the actual hydraulic volume,
including steps without material delivery. It retains
`Rnew = R + M + deltaM - RN(Cnew * Vnew)` without adding that remainder to the
physical mixer. Gross spill admission uses both settled physical and accountable
inventory; the prepared result must still fund all remaining reservations.
Nonzero material increments are compared with the same-step zero-material mix.
The existing local two-epsilon roundoff bound remains in force.

This law requires exclusively mapped wet storage and a closed flat first-order
serial CPU surface, with no other source, forcing, boundary, thermal, reaction,
network or storage-loss owner. Eligibility is checked at attachment, at step
entry and before producer effects even without checkpointing. Water remains on
the ordinary native path. Physical per-firing water, ordered signed SI point
totals, point conversions, node bookings, queue consumption and hydraulic volume
changes remain separate observations. The exact difference between native
booking and gross native leg water is diagnostic, never a balancing receipt.
By default, per-point directional gross water totals must still be exactly
representable;
unsupported accumulation or partitioning is refused. Whole deliveries and the
existing authenticated exact halves are the only qualified material partitions.

The separate `PassiveStorageWaterArithmetic::RETAIN_AGGREGATES` option on
`attachLiveChangingVolumeExchange(water, capacity, preflight)` retains canonical
sparse directional water totals at each point. It authenticates sums of the
actual once-converted native legs; signed SI point accumulation and native
queue booking keep their original arithmetic. Water supplements never balance
or modify the hydraulic queue. Whole delivery consumes the stored exact amount;
one equal-half delivery regroups individually authenticated native leg halves
and proves the exact partition. Existing scalar half-schedule checks remain.
Other fractions still refuse. Water-only rows remain pending even with zero
pollutant mass. Receipt archive 8 and live archives 10/11 distinguish this option,
without/with conservative history admission, and reject cross-policy restores.
Decoding, replay and preflight include the additional water components and their
possible future half remainders. Earlier modes and archive bytes are unchanged.

`attachLiveChangingVolumeSlicedExchange(capacity, preflight)` selects
`RETAIN_EXACT_SLICES` with retained gross water. Two columns in immutable
native-leg order own the current water and material remainders. Each partial
uses `(before / frozen_span) * dt` and `before - slice`; both must stay positive
for a positive leg and their exact sum must equal `before`. Thus successive
partitions qualify only when the represented native operations conserve each
leg exactly. This does not promise exact rational proportionality. Whole delivery
retires every remainder. Unsupported arithmetic refuses before material mixing.

Receipt 9 and live 12/13 identify this pairing. Detached replay reconstructs every
leg remainder from the original transfers and delivery history, including settled
zero remainders. Counts are bounded before allocation; history admission and
replay include the extra per-leg validation/copy/regrouping work. Earlier
attachments and archive formats retain their previous behavior.

The live changing-volume profile accepts one declared native pollutant in `MG/L`,
`UG/L` or `#/L`. Numeric concentration is preserved: surface amounts are
concentration times m3 and receiving amounts are concentration times ft3. These
are not kg amounts merely because a field includes `si` in its internal name.
Physical surface-amount factors are 0.001 kg for mg/L, 0.000001 kg for ug/L,
and 1000 counts for #/L. The corresponding native factor also includes
0.028316846592 m3/ft3. Counts may represent fractional expected values.
Conversion rounding is an observation, not a balancing correction.

Conservative enrollment additionally requires a recognized unit declaration.
Unknown registered INP unit tokens retain the ordinary native mg/L fallback,
with validity metadata that refuses this optional profile. GeoPackage rows
must contain the writer's explicit unit codes 0, 1 or 2; text coercion or enum
wraparound cannot establish a valid declaration. The metadata follows row
creation/deletion, and a valid pre-start units setter intentionally repairs it.
Rows skipped entirely by the native parser are outside this declaration check.
Existing archive formats retain their identities and bytes.

The experimental `openswmm_passive_exchange.h` C facade selects the qualified
integrated exact-slice owner with `swmm_passive_exchange_enable`. Call it after
ordinary initialize/start(0), before advancing. Capacity and history admission
remain explicit. `swmm_passive_exchange_enable_checkpoint` separately attaches
the native numerical checkpoint profile; existing C checkpoint inspection,
exclusive save and restore then require matching fresh enrollment.

Version1 getters copy status, the book's settled pool, sealed pending gross
amounts, receipt rows, original journal legs and current per-leg remainders.
Amounts are exact canonical expansions: count0 means zero and every returned
component participates in the sum. They carry no funding or application
authority. Versioned record sizes are checked before writing. Reads require
exclusive idle engine access and preserve outputs and engine diagnostics on
refusal. Terminal observations may include an open/unsealed producer window;
settled and current hydraulic volumes are separate. Unapplied attempt after-values
are proposals. These records are neither portable packets nor checkpoint files.

The pure C11 reference harness compares all three bases and both flow signs with
the same native C++/Fraction reference, including exact checkpoints, intermediate
restart, callback refusal, unsupported arithmetic, an open terminal journal and
recoverable history refusal. This facade does not activate species in the water
split API. The native exact-slice owner now reserves an exclusive network-book
allowance for each actual ADVANCE, REPORT or FINAL window. Per-node authority is
the exact minimum of settled physical and accountable inventory, less sealed
gross spill. Each spill consumes before surface credit; drains cannot replenish
the grant. Ordered applied proof prepares a candidate receipt. The old sealed
book remains unchanged until actual native water queue/span confirmation commits
both records and releases unused authority. Failed or missing confirmation keeps
the grant, spend, applied journal and prepared seal unresolved; quality and
checkpoints refuse outstanding work. Accepted-boundary archives retain their
existing format and replay reconstructs these grants, including empty windows.
This private integrated authority has no remote peer/session identity. Schema10
still needs the actual split post-quality REPORT/final handoffs and a qualified
peer funding protocol before any material grant can leave the native owner.

A separate private C++ water-only scheduling mode qualifies that report seam
using two native participants. It defers the surface's entire due output action
until one zero-duration REPORT request at the settled network endpoint. Peer
identities, sequence, report-grid time and mode must match. A pending report
blocks further phases; timer-only slots before REPORT_START still consume one
native tick. FINISH uses the existing peer end() after that tick completes.
The mode is restricted to simple water without source, aquifer, mapped-outfall
or node-LID owners. Schemas1-9 refuse all of its frames, including geometry;
the public C configure call and component metadata keep their existing profile.
This establishes scheduling only. Species transport, session-bound material
authority and schema10 remain separate gates.

The private `PeerPassiveStorageFunding`/`PeerPassiveStorageProducer` protocol
qualifies one native two-way producer window with distinct network and surface
contexts. The network alone owns settled inventory and the full in-flight grant;
the surface owns exact local spend and its actual applied-leg journal. A registered
recipient capability and noncopyable single-use offer/reply/acknowledgement bind
the process session, point map, native node columns, basis and physical window.
The same native admission port and operand checks serve integrated and peer
producers. Admission and application allocate nothing. Returned evidence is
marked peer-reported; the receiver does not replay admission as a local witness.
Unused funding is released after material/water commit. Missing replies retain
the full network encumbrance and the independently held surface journal. Peer
book checkpoints refuse even at settled boundaries until paired recovery exists.

The caller must maintain one exclusive issuer per network context throughout
the session, including unresolved work. This internal protocol does not arbitrate
two independently constructed books over the same inventory. Engine-owned
attachment must enforce that exclusion when the no-link material role, native
quality hooks and successive REPORT/FINAL scheduling are added. This first
qualification uses the existing manufactured closed wet-storage marcher fixture
with zero-net water and both signs of material change. It does not enable full
engine material split scheduling, a portable packet, schema10 or GUI activation.

Receipt archive 7, live owner archive 8 and engine archive 3005 distinguish the
new law. Bounded replay records every quality-step volume transition and every
producer water publication, including empty windows, separate drained and
refreshed spans, and old/current flow memory. Replay uses the existing quality
writer, validates water booking/assembly and continuity of the observations,
and compares all mapped pools against detached incoming state before publishing.
Observed hydraulic transitions are inputs to material replay; this does not
prove that a unique historical hydraulic run produced them. Earlier formats
remain unchanged. Other delivery arithmetic, other owners and
public/component/GUI activation remain unqualified.

`attachLiveChangingVolumeExchange(capacity, true)` additionally selects
conservative history admission (live owner archive 9; receipt 7 and engine 3005
stay unchanged). Before stepping, it reserves one quality delivery and one
possible producer window against the existing record and replay-work limits.
Water publications and all mapped points count even in empty windows after the
material journal fills. The remaining global leg capacity bounds possible new
receipt rows, proofs and material expansions; this is deliberately conservative
and does not predict the native substep count. Pending REPORT/FINAL flushes
reserve one publication without an extra quality delivery. Refusal leaves the
accepted checkpoint, clocks and physical state unchanged, with a transient
owner diagnostic. Registered callbacks and forcing APIs that can flush or alter
that boundary remain excluded. A large journal capacity can refuse initially;
native leg exhaustion remains an independent terminal condition. Earlier live
owner 8 behavior and bytes are preserved, and cross-policy restores refuse.

Authored positive and negative cases cover native half delivery, pending/refill
states and zero-material hydraulic tails with fresh restart at each boundary.
Evidence and exact arithmetic checks are retained under
`tests/verification/component_species_changing_volume_2026-11-06`. The law stays
opt-in and introduces no HydroCoupleSDK dependency.


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


Private C++ material pairing uses `configureMaterialSplit` and
`bindMaterialPeer` for the existing closed, serial CPU, first-order,
changing-volume passive storage profile. Both engines start from the same
native model and geometry. The network exclusively owns the receiving stock,
funding allowance and quality solver; the surface owns its native mesh solve
and applied-leg journal. Native ADVANCE, post-quality REPORT and FINISH windows
transfer single-use funding/proof/acknowledgement capsules. Material proof and
water booking are checked before publishing the network queue. Surface step
acceptance follows network quality, output and the master step-end callback.
Successful finalization is tracked separately from natural clock completion.
A failed or closed partner makes the pair terminal; no physical rollback or
paired restart is provided.

This private path retains the existing SoA hydraulic and quality arrays and
native solver. It has no HydroCoupleSDK dependency. It does not enable species
in the public C split API, portable packets, installed components or the GUI.
Public water profiles and packet schemas remain unchanged. Unsupported exact
arithmetic still refuses. Qualification and limitations are recorded under
`tests/verification/component_material_pair_2026-11-07`.


An experimental C material adapter and separate schema10 codec are described in
`SPLIT_MATERIAL_PACKET.md`. This adapter retains native authority in one explicitly
bound trusted in-process pair and validates complete serialized offers before
credit, proofs before preparation, and acknowledgements after commitment. The
existing water codec and component/Composer/GUI profiles remain unchanged.

## Paired material recovery ownership

The engine's experimental material C interface now provides collective recovery
of an explicitly bound network/surface pair, with optional built-in binary and
pollutant HDF5 continuation. See [paired material recovery](SPLIT_MATERIAL_PACKET.md#paired-c-recovery).
Its backend capability flags are separate from HydroCouple component capabilities.
Both component roles continue to omit `Capability::Checkpointing` and
`ICheckpointableModelComponent`: that interface permits independent restoration
of an Updated/Done component, while this backend restores two fresh running
participants together. Composer/GUI resume requires collective host grouping,
fresh pair/output preparation and coordinated token ownership before activation.

The engine component module also exports the optional versioned service
`openswmm_component_material_recovery`, declared in
`openswmm/components/openswmm_component_recovery.h`. Discover it from the exact
already-loaded module after checking its HydroCouple component ABI stamp. It is
not an export of the native engine library and introduces no SDK dependency or
HydroCouple interface/vtable change. Unsupported component builds return no
service table. The V2 extension supports the explicit Composer CLI integration
described below; GUI resume remains disabled.

The host gives `enroll` the canonical `IModelComponent*` base addresses of one
exclusive material network and surface pair, in that order, while both are Valid
and before either prepares. Enrollment freezes the connection and rejects a
second owner. Writer mode is explicit: `outputs=0` suppresses native result
writers; `outputs=1` uses network `model.out` and surface `model.2d.h5`, requires
surface results, and intentionally omits text reports on both roles. Updated
metadata records this policy while per-component checkpoint capabilities remain
false. The chosen writer mode persists until reinitialization; releasing a group
does not finish its components.

Prepare both components in either order, then call the service's `prepare(group)`
once to start/bind/enroll at zero before the first workflow update. At an idle
synchronized Updated barrier, `capture` produces **one** immutable owned state for
both stable member IDs. The host indexes both IDs to that group and persists its
bytes once. `import_state` copies bounded caller storage; `data` borrows bytes
until `destroy_state`. The module must remain loaded until all groups and states
are released. State survives destruction of the original components and may seed
multiple fresh pairs. Release is exactly once; active group/state release is
refused and must be retried after the call returns.

Restore requires the same logical members/model/capacity/output policy, freshly
prepared and bound at zero. A bounded checksummed envelope retains accepted
component clocks, observation counts and last-window history around the native
paired binary state. Import rejects duplicate JSON keys and excessive nesting;
restore checks both observer histories and joins their accepted seconds to the
native `elapsed_ms/1000` before any numerical/output publication. It stages both
views before the native transaction, installs both clocks/views and statuses
before notifications, and holds the pair lease throughout. Invalid state leaves
the fresh views/prefixes available for valid retry. Historical session/receipt
observations are marked as history and never supply callback or funding
authority; the native restore establishes fresh identities. Notification failure
does not undo committed physics. Restore does not write diagnostic JSON files;
the host can persist the returned live views after success.

The service covers exactly this pair, fresh recovery and the existing native
save bounds. It does not cover text reports, arbitrary graphs/adapters, live
rollback or authenticated remote state. Checksums detect corruption, not hostile
rewriting. Factory/module and fresh executable qualification is under
`tests/verification/component_group_recovery_2026-11-08`.

### V2 state inspection and Composer CLI

The same discovery symbol accepts version 2 and
`sizeof(OpenSWMM_MaterialRecoveryV2)`. Its unchanged V1 function prefix has
`version=2`/the V2 size, followed by `describe_state`. Version-1 requests still
receive the original version/size/table. The added read-only operation borrows an
immutable `openswmm.component-material-state/1` JSON summary until state release:
`group`, `accepted_steps` and exact `accepted_time_s`. It remains valid after the
original components/group close. These are captured observations, not restored
native authority or certification of model compatibility.

A collective-aware host can now compare its logical members, output policy,
absolute step and accepted seconds to the opaque state before restore publishes
any native prefix. It must not parse the private binary wrapper itself. The
native/component transaction still performs full history/model/clock validation.
Composer's explicit command-line path uses this V2 join and one atomically
published binary state/manifest package for the entire pair. Its separate run
catalogs/writers and GUI resume remain outside this profile. See Composer
`docs/MATERIAL_RECOVERY.md`; qualification is retained under
`tests/verification/composer_collective_recovery_2026-11-08` and the Composer
checkout's `verification/component_collective_recovery_2026-11-08`.
