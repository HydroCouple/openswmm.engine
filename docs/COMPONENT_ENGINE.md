# Native OpenSWMM engine component

The optional `openswmm_engine_component` module publishes component information
ID `org.hydrocouple.openswmm.engine`. It uses HydroCouple interfaces and the
public OpenSWMM C API; it does not link HydroCoupleSDK. The older
`openswmm.model` module remains the separate FVQual compatibility adapter.
Build with `OPENSWMM_WITH_HYDROCOUPLE=ON`. This initial execution profile is
CPU 1D. Spatial execution, bidirectional exchange and coordinated restart are
separate qualifications, not capabilities of this module yet.

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

`complete_model` retains native ownership of configured processes. The initial
profile refuses explicit surface meshes and explicit groundwater, including
PER_SUBCATCH groundwater; it never turns off these processes to make a model
appear supported. `network_only` requires no subcatchments and the same spatial
restrictions. Neither role is the future external-1D/integrated2d split mode.
Native watershed, snow, LID and legacy groundwater processes in a complete model
are not replaced by a second component driver. Their detailed coupling and
parity matrices remain to be qualified.

Initialization parses the configured model without preparing or advancing a
simulation, loading runtime `[PLUGINS]`, or writing results. `model_metadata`
is a scalar String containing `openswmm.component-metadata/1` JSON: stable node
and link IDs/endpoints, pollutant units, reaction BULK/WALL kinds and declared
units, process registrations, transport policy, clocks, legacy aquifer count,
explicit groundwater mode/configuration and surface counts. This is an initial
catalogue, not a complete spatial geometry interface. Check `runnable` and
`execution_restrictions`; metadata inspection can succeed for a model whose
execution is not qualified. Runtime plugins and model-authored additional output
channels are refused until their metadata/output isolation is qualified.

The same inspection path is public independently of HydroCouple:

```python
from openswmm.engine import Solver

solver = Solver("model.inp")
try:
    solver.open_metadata()       # C API: swmm_engine_open_metadata
    print([node.id for node in solver.nodes])
    print([species.name for species in solver.reactions.species])
finally:
    solver.destroy()
```

Metadata handles are OPENED and cannot initialize/start a run. Close and open
normally to simulate. The parser uses strict validation even if lenient editor
opening was selected. Inspection may read referenced inputs/configuration files;
it does not evaluate runtime chemistry or prove that all runtime data are valid.

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
