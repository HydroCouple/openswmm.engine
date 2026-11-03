# Native OpenSWMM engine component

The optional `openswmm_engine_component` module publishes component information
ID `org.hydrocouple.openswmm.engine`. It uses HydroCouple interfaces and the
public OpenSWMM C API; it does not link HydroCoupleSDK. The older
`openswmm.model` module remains the separate FVQual compatibility adapter.
Build with `OPENSWMM_WITH_HYDROCOUPLE=ON`. Qualified execution includes CPU 1D
and the bounded CPU surface-water profile below. Spatial transport, explicit
groundwater, bidirectional exchange and coordinated restart remain separate
qualifications.

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

`complete_model` retains native ownership of configured processes. Explicit
groundwater, including PER_SUBCATCH groundwater, remains refused. Surface meshes
can run through the water-only profile below. `network_only` requires no
subcatchments or surface mesh. Neither role is the future
external-1D/integrated2d split mode.
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
  shared/independent groundwater clock; they do not enable component stepping.
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
For the surface-water profile, `execution_profile` names the profile and
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
| Boundary configuration | `swmm_2d_get_edge_bc_type`, `swmm_gw2d_boundary_get`, `swmm_gw_boundary_quality_get`; native surface/GW boundary owners | Parsed catalogues are inspection records. Runtime boundary ports and split ownership are missing. |
| Hydraulic boundary state | `swmm_node_get_depth`, `swmm_coupling_set_sources`, `swmm_2d_set_runtime_boundary`, `swmm_gw2d_set_runtime_boundary` | Native calls exist; general component hydraulic input/output ports are missing. A head override does not itself transfer inventory. |
| External source/forcing | `swmm_coupling_apply_frame`, `swmm_gw_source_get`; native source staging and receipts | Native copied atomic frames and source inspection exist. No generic HydroCouple provider bridge or automatic physical-unit conversion is implemented. |
| Transfer offer/reservation | Native surface/GW/node/link and Richards owners | Native scheduling exists. The public external-1D trial/reservation/acceptance lifecycle and `Integrated2dComponent` are missing. No wrapper may recreate these owners or advance chemistry twice. |
| Accepted outfall transfer | `swmm_node_get_export_receipt`; native outfall debit | Implemented scalar `hydrocouple.interval-transfer/1`, one direct receiver, water/pollutants/enthalpy. MSX, fan-out and refund/retry are refused. |
| Accepted source/boundary transfer | `swmm_coupling_get_receipts`, `swmm_coupling_get_boundary_receipt`, `swmm_coupling_get_infiltration_receipt`; native receipt owners | Native receipts exist. General typed component ports and split phase equivalence remain missing. |
| Inventory and diagnostics | `swmm_2d_get_species_ledger`, `swmm_gw2d_boundary_species`, native `.out` / HDF5 owners | Native diagnostics and wrapper result files exist for qualified profiles. Diagnostics never constitute a second physical storage or debit. |
| Time | `swmm_engine_advance_to`, `swmm_coupling_advance_to`; native solver calendars | Qualified CPU 1D wrapper uses the recorded epoch and millisecond endpoints. External-1D R4/R5 phase traces and unequal-step component qualification remain missing. |
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
