@page user_manual_chapter_13 CHAPTER 13 – PROGRAMMATIC C API

@tableofcontents

OpenSWMM Engine v6 provides a comprehensive C API for building, running, and querying SWMM models entirely through code, without requiring an input file. This is useful for embedding SWMM in larger simulation frameworks, coupling with other models, or building custom user interfaces.

## 13.1 Architecture Overview

The new engine uses an opaque handle (`SWMM_Engine`) that encapsulates all simulation state. This reentrant design allows multiple independent simulations to run within the same process. The engine progresses through a well-defined lifecycle:

```
CREATED → OPENED → INITIALIZED → STARTED → [RUNNING] → ENDED → CLOSED
```

When a model is constructed programmatically (no input file), the engine instead begins in a `BUILDING` state and `swmm_finalize_model()` transitions it directly to `INITIALIZED`:

```
[BUILDING] → INITIALIZED → STARTED → [RUNNING] → ENDED → CLOSED
```

All API functions return an integer error code (`SWMM_OK` on success) and are organized by domain into separate headers; the master header @ref openswmm_engine.h pulls in all of them:

| Header | Domain |
|---|---|
| `openswmm_engine.h` | Engine lifecycle, error codes, state machine, timing, master include |
| `openswmm_model.h` | Model building, validation, serialization, options, user flags, `[PLUGINS]`/`[FILES]` access |
| `openswmm_nodes.h` | Junction, outfall, storage, and divider nodes |
| `openswmm_links.h` | Conduit, pump, orifice, weir, and outlet links |
| `openswmm_subcatchments.h` | Subcatchments and infiltration |
| `openswmm_gages.h` | Rain gages |
| `openswmm_pollutants.h` | Pollutants and their properties |
| `openswmm_tables.h` | Time series, curves, and patterns |
| `openswmm_inflows.h` | External inflows, DWF, and RDII |
| `openswmm_controls.h` | Control rules |
| `openswmm_infrastructure.h` | Transects, streets, inlets, LID controls |
| `openswmm_spatial.h` | CRS, coordinates, polylines, polygons |
| `openswmm_quality.h` | Landuse, buildup/washoff, treatment |
| `openswmm_initial_quality.h` | `[INITIAL_QUALITY]` per-element initial concentrations |
| `openswmm_reactions.h` | Multi-species reaction system, expression validation, `.rxn` text |
| `openswmm_process_components.h` | `[PROCESS_COMPONENTS]` registrations and config paths |
| `openswmm_water_age.h` | `[WATER_AGE_SOURCES]` source ages and node overrides |
| `openswmm_heat.h` | Heat-transport fluxes, solar/cloud forcing, `[HEAT_SOURCES]` |
| `openswmm_infil2d.h` | 2-D surface infiltration cells, defaults, and totals |
| `openswmm_massbalance.h` | Continuity errors and flux totals |
| `openswmm_statistics.h` | Post-run node/link/pump statistics |
| `openswmm_forcing.h` | Runtime forcing overrides |
| `openswmm_climate.h` | Evaporation, temperature, snowmelt, adjustments |
| `openswmm_edit.h` | Object rename, delete, convert, impact analysis |
| `openswmm_datetime.h` | SWMM DateTime encode/decode utilities |
| `openswmm_xsect.h` | Cross-section geometry computations |
| `openswmm_output.h` | Binary output (`.out`) file reading |
| `openswmm_geopackage.h` | GeoPackage container queries |
| `openswmm_callbacks.h` | Progress, warning, and step callback typedefs |
| `openswmm_hotstart.h` | Hot start file operations |
| `openswmm_2d.h` | 2-D overland flow mesh (when built with `OPENSWMM_HAS_2D`) |

## 13.2 Basic Workflow

Every engine instance moves through the deterministic lifecycle of
Figure 13-1. The file-based path enters at `swmm_engine_open()`; the
programmatic path enters at `swmm_engine_new()` (BUILDING) and joins it
at `swmm_finalize_model()`.

<pre class="mermaid">
stateDiagram-v2
    direction LR
    [*] --> CREATED : swmm_engine_create
    CREATED --> OPENED : swmm_engine_open - parse inp, load plugins
    [*] --> BUILDING : swmm_engine_new
    BUILDING --> OPENED : swmm_finalize_model
    OPENED --> INITIALIZED : swmm_engine_initialize
    INITIALIZED --> STARTED : swmm_engine_start
    STARTED --> RUNNING : first swmm_engine_step
    RUNNING --> RUNNING : swmm_engine_step
    RUNNING --> ENDED : swmm_engine_end
    ENDED --> ENDED : swmm_engine_report
    ENDED --> CLOSED : swmm_engine_close
    CLOSED --> [*] : swmm_engine_delete
</pre>

*Figure 13-1 Engine lifecycle states (SWMM_EngineState in @ref openswmm_engine.h)*

The typical workflow for building and running a model programmatically (no input file):

```c
#include <openswmm/engine/openswmm_engine.h>

// 1. Create an empty engine in BUILDING state (no .inp file required)
SWMM_Engine engine = swmm_engine_new();

// 2. Set model options (string key/value pairs, same vocabulary as [OPTIONS])
swmm_options_set(engine, "FLOW_UNITS", "CFS");
swmm_options_set(engine, "FLOW_ROUTING", "DYNWAVE");

// 3. Build the network
swmm_node_add(engine, "J1", SWMM_NODE_JUNCTION);
swmm_node_set_invert_elev(engine, 0, 100.0);
swmm_node_set_max_depth(engine, 0, 6.0);

swmm_node_add(engine, "Out1", SWMM_NODE_OUTFALL);
swmm_node_set_invert_elev(engine, 1, 95.0);

swmm_link_add(engine, "C1", SWMM_LINK_CONDUIT);
swmm_link_set_nodes(engine, 0, 0, 1);  // from J1 to Out1
swmm_link_set_length(engine, 0, 500.0);
swmm_link_set_roughness(engine, 0, 0.013);
swmm_link_set_xsect(engine, 0, SWMM_XSECT_CIRCULAR, 2.0, 0, 0, 0);

// 4. Finalize the built model (validates topology → INITIALIZED)
swmm_finalize_model(engine);

// 5. Start the simulation (1 = save results)
swmm_engine_start(engine, 1);

// 6. Step through the simulation
double elapsed = 0.0;
while (swmm_engine_step(engine, &elapsed) == SWMM_OK && elapsed > 0.0) {
    // Query results during simulation
    double depth;
    swmm_node_get_depth(engine, 0, &depth);
}

// 7. End, close, and destroy
swmm_engine_end(engine);
swmm_engine_close(engine);
swmm_engine_destroy(engine);
```

To run an existing input file instead, use `swmm_engine_create()` followed by `swmm_engine_open()` and `swmm_engine_initialize()`:

```c
SWMM_Engine engine = swmm_engine_create();
swmm_engine_open(engine, "model.inp", "model.rpt", "model.out", NULL);
swmm_engine_initialize(engine);
swmm_engine_start(engine, 1);

double elapsed = 0.0;
while (swmm_engine_step(engine, &elapsed) == SWMM_OK && elapsed > 0.0) {
    /* ... */
}

swmm_engine_end(engine);
swmm_engine_report(engine);
swmm_engine_close(engine);
swmm_engine_destroy(engine);
```

The final `NULL` argument to `swmm_engine_open()` selects the built-in `.inp` reader; passing the path of an input-plugin shared library reads the model through that plugin instead (see Section 13.8). The single-call helpers `swmm_engine_run()` and `swmm_engine_run_with_callback()` chain the entire lifecycle for batch runs.

## 13.3 Callbacks

The callback system allows applications to receive notifications during simulation execution:

- **Progress callback** — Receives periodic updates on simulation progress (0–100%).
- **Warning callback** — Receives warning messages generated during simulation.
- **Step-begin / step-end callbacks** — Called before and after each computational time step.
- **Plugin state callback** — Notifies plugins of engine state transitions.

Register callbacks before calling `swmm_engine_initialize()`. See @ref openswmm_callbacks.h for details and examples.

## 13.4 Hot Start Files

The hot start API enables saving and restoring simulation state for warm-start scenarios:

```c
#include <openswmm/engine/openswmm_hotstart.h>

// Save state to a hot start file (engine must be RUNNING or ENDED)
swmm_hotstart_save(engine, "warmup.hsf");

// Later, open a hot start file and apply it to an INITIALIZED engine
SWMM_HotStart hs;
swmm_hotstart_open("warmup.hsf", &hs);
swmm_hotstart_apply(engine, hs);
swmm_hotstart_close(hs);
```

`swmm_hotstart_apply()` must be called after `swmm_engine_initialize()` but before `swmm_engine_start()`. Objects present in the file but missing from the model (or vice versa) generate warnings rather than errors; they can be enumerated with `swmm_hotstart_warning_count()` / `swmm_hotstart_warning()`. Saving dispatches through any registered state-IO plugins, so alternative hot-start file formats can be provided by plugins (see Section 13.8).

## 13.5 Python Bindings {#user_manual_chapter_13_python}

OpenSWMM 6.0 ships a first-class Python package (`openswmm`) that provides
Pythonic, type-annotated access to the full engine feature set.  Install from
PyPI:

```bash
pip install openswmm
```

### 13.5.1 Core Classes (`openswmm.engine`)

All simulation functionality lives in the `openswmm.engine` sub-package.

| Class | Purpose |
|---|---|
| `Solver` | Engine lifecycle — open, start, step, end, report |
| `Nodes` | Query and set node attributes and results |
| `Links` | Query and set link attributes and results |
| `Subcatchments` | Query and set subcatchment attributes and results |
| `Gages` | Query rain-gage attributes and recorded rainfall |
| `HotStart` | Save and restore simulation state |
| `MassBalance` | Retrieve continuity error and mass-balance statistics |
| `ModelBuilder` | Construct a SWMM model programmatically without an input file |

### 13.5.2 Running a Simulation from an Input File

```python
from openswmm.engine import Solver, Nodes, Links, Subcatchments

with Solver("model.inp", "model.rpt", "model.out") as solver:
    solver.start(save_results=True)

    nodes = Nodes(solver)
    links = Links(solver)
    subcatchments = Subcatchments(solver)

    while solver.step() > 0:
        # Access current time-step values by object name
        depth  = nodes["J1"].depth
        head   = nodes["J1"].head
        flow   = links["C1"].flow
        runoff = subcatchments["S1"].runoff

    solver.end()
    solver.report()
```

### 13.5.3 Accessing Rain Gages

```python
from openswmm.engine import Solver, Gages

with Solver("model.inp", "model.rpt", "model.out") as solver:
    solver.start(save_results=True)
    gages = Gages(solver)

    while solver.step() > 0:
        rain = gages["RG1"].rainfall   # current rainfall rate (in/hr or mm/hr)
```

### 13.5.4 Hot Start Files

Hot start files allow a long-term simulation to be split into segments, each
beginning from the hydraulic state left by the previous run.

```python
from openswmm.engine import Solver, HotStart

# --- Warm-up run: save state at end ---
with Solver("warmup.inp", "warmup.rpt", "warmup.out") as solver:
    solver.start(save_results=True)
    while solver.step() > 0:
        pass
    solver.save_hotstart("state.hsf")
    solver.end()
    solver.report()

# --- Main run: restore state from hot start file ---
with Solver("main.inp", "main.rpt", "main.out") as solver:
    solver.use_hotstart("state.hsf")
    solver.start(save_results=True)
    while solver.step() > 0:
        pass
    solver.end()
    solver.report()
```

### 13.5.5 Mass Balance and Continuity

```python
from openswmm.engine import Solver, MassBalance

with Solver("model.inp", "model.rpt", "model.out") as solver:
    solver.start(save_results=True)
    while solver.step() > 0:
        pass
    solver.end()

    mb = MassBalance(solver)
    print(f"Runoff continuity error : {mb.runoff_error:.4f} %")
    print(f"Routing continuity error: {mb.routing_error:.4f} %")
```

### 13.5.6 Building a Model Programmatically

`ModelBuilder` constructs a complete SWMM network without an input file.  The
finished model is passed directly to `Solver`.

```python
from openswmm.engine import ModelBuilder, Solver, Nodes, Links
from openswmm.engine import NodeType, LinkType, FlowUnits, RoutingModel

# Build the network
mb = ModelBuilder()
mb.flow_units = FlowUnits.CFS
mb.routing_model = RoutingModel.DYNWAVE

j1 = mb.add_node("J1", NodeType.JUNCTION)
j1.invert_elev = 100.0
j1.max_depth = 6.0

out1 = mb.add_node("Out1", NodeType.OUTFALL)
out1.invert_elev = 95.0

c1 = mb.add_link("C1", LinkType.CONDUIT, from_node="J1", to_node="Out1")
c1.length = 500.0
c1.roughness = 0.013
c1.set_circular_xsect(diameter=2.0)

# Simulate the built model
with Solver(mb, "model.rpt", "model.out") as solver:
    solver.start(save_results=True)
    nodes = Nodes(solver)
    while solver.step() > 0:
        print(nodes["J1"].depth)
    solver.end()
    solver.report()
```

### 13.5.7 Full API Reference

The complete class and method documentation, including all attributes,
enumerations, and error types, is published in the
[Python Bindings API Reference](python/index.html).

See the `python/` directory in the source tree for the Cython source (`.pyx`),
type stubs (`.pyi`), and test suite.

## 13.6 Building with the API

To use the OpenSWMM Engine C API in your own project, link against `openswmm_engine` using CMake:

```cmake
find_package(OpenSWMMCore REQUIRED)
target_link_libraries(my_app PRIVATE OpenSWMMCore::openswmm_engine)
```

All public headers are installed under `include/openswmm/engine/`.

## 13.7 User-Defined Flags {#user_manual_chapter_13_user_flags}

OpenSWMM Engine v6 introduces **user-defined flags** (inspired by InfoWorks ICM custom attributes) that allow metadata to be attached to any model object—nodes, links, or subcatchments. Flags are defined with a name, data type, and optional description and then assigned values per object.

### Input File Syntax

Two new sections are recognised in the input file:

```ini
[USER_FLAGS]
;;Name            Type      Description
INSPECTED         BOOLEAN   "Has the object been field-inspected?"
PRIORITY          INTEGER   "Maintenance priority (1 = highest)"
ROUGHNESS_ADJ     REAL      "Site-specific roughness multiplier"
ASSET_ID          STRING    "External asset-management system ID"

[USER_FLAG_VALUES]
;;ObjectType   ObjectName   FlagName        Value
NODE           J1           INSPECTED       YES
NODE           J1           PRIORITY        2
LINK           C_MAIN       ROUGHNESS_ADJ   1.05
LINK           C_MAIN       ASSET_ID        "AM-00341"
SUBCATCHMENT   S_WEST       INSPECTED       NO
```

Supported types are **BOOLEAN** (`YES`/`NO`/`TRUE`/`FALSE`/`1`/`0`), **INTEGER**, **REAL**, and **STRING**.

### C API

Flag values can be read or written at runtime through the C API:

```c
int swmm_userflag_get_bool(SWMM_Engine engine, const char* name, int* value);
int swmm_userflag_set_bool(SWMM_Engine engine, const char* name, int  value);

int swmm_userflag_get_int (SWMM_Engine engine, const char* name, int* value);
int swmm_userflag_set_int (SWMM_Engine engine, const char* name, int  value);

int swmm_userflag_get_real(SWMM_Engine engine, const char* name, double* value);
int swmm_userflag_set_real(SWMM_Engine engine, const char* name, double  value);
```

See `openswmm_model.h` for the complete set of flag functions.

## 13.8 Plugin Interface for Output and Reporting {#user_manual_chapter_13_plugins}

The **plugin SDK** enables third-party shared libraries to replace or supplement the engine's built-in file I/O — the model reader/writer, the binary output (`.out`) file, the text-based status report, and the hot-start (state) file. Four abstract C++ interfaces are provided:

| Interface | Header | Purpose |
|---|---|---|
| `IInputPlugin` | `IInputPlugin.hpp` | Reads model data into the engine (and writes it back out) in alternative container formats |
| `IOutputPlugin` | `IOutputPlugin.hpp` | Writes time-series results at each output time step |
| `IReportPlugin` | `IReportPlugin.hpp` | Writes summary statistics at simulation end |
| `IStateIOPlugin` | `IStateIOPlugin.hpp` | Reads and writes simulation state (hot-start) files |

All interfaces share a common lifecycle that mirrors the engine's own state machine (enumerated by `PluginState` in `PluginState.hpp`):

```
LOADED → initialize() → INITIALIZED → validate() → VALIDATED
       → prepare()    → PREPARED     → update() [N times]
       → finalize()   → FINALIZED    → CLOSED
```

Each plugin is compiled as a shared library (`.so` / `.dylib` / `.dll`) that exports a single C factory function:

```cpp
extern "C" openswmm::IPluginComponentInfo* openswmm_plugin_info(void);
```

The `IPluginComponentInfo` class provides metadata (id in reverse-DNS notation, caption, description, version, vendor, license), capability queries (`has_input()`, `has_output()`, `has_report()`, `has_state_io()` — a single plugin may support several roles), optional registration/licensing hooks, and factory methods for creating instances of each interface. Plugins also advertise the file formats they handle through `file_filters()`, one entry per (role, glob-pattern) pair; hosts such as the GUI use these to build file-picker dialogs without hard-coding format lists.

### Plugin Discovery

Each engine instance owns a `PluginFactory` that automatically scans the engine library directory and its `plugins/` and `components/` subdirectories for shared libraries exporting `openswmm_plugin_info`. Discovered libraries are registered in a component registry keyed by `id:version`; the built-in default input, output, report, and state-IO plugins are registered alongside them (flagged as built-ins). Hosts that only need to enumerate available formats can call the framework-free discovery facade in `PluginDiscovery.hpp`:

```cpp
#include <openswmm/plugin_sdk/PluginDiscovery.hpp>

auto filters = openswmm::discover_all_filters();     // every (plugin, filter) pair
auto plugins = openswmm::discover_plugins_by_id();   // grouped per plugin id
```

### Registering Plugins

Plugins are loaded from the `[PLUGINS]` input-file section:

```ini
[PLUGINS]
./plugins/hdf5_output.dylib     file="results.h5"  compress=9
./plugins/csv_report.dylib      file="report.csv"   delimiter=","
```

Each line begins with the plugin to load — a shared-library path, a plugin id, or an `id:version` pair (ids are resolved against the auto-discovery registry) — followed by initialisation arguments that are forwarded verbatim to the plugin's `initialize()` method.

Figure 13-2 traces the full resolution path from engine open to an
initialized plugin.

<pre class="mermaid">
flowchart TD
    A[swmm_engine_open] --> B[PluginFactory scans engine lib dir plus plugins and components subdirectories]
    B --> C[Libraries exporting openswmm_plugin_info registered by id and version]
    C --> D[Built-in input, output, report and state-IO plugins registered as built-ins]
    D --> E[Parse PLUGINS section entries]
    E --> F{Entry form}
    F -- shared-library path --> G[Load library directly]
    F -- plugin id --> H[Resolve newest version in registry]
    F -- id colon version --> I[Resolve exact version in registry]
    G --> J[Instantiate plugin]
    H --> J
    I --> J
    J --> K[Forward remaining arguments to initialize]
    K --> L[Plugin receives host callbacks during the run]
</pre>

*Figure 13-2 Plugin discovery, resolution and loading workflow (rendered diagram)*

The `[PLUGINS]` section can also be inspected and edited through the C API without re-parsing the input file: `swmm_plugins_count()`, `swmm_plugin_get()`, `swmm_plugin_set()`, and `swmm_plugin_remove()` (declared in @ref openswmm_model.h) read and mutate the in-memory plugin list, which is re-serialised on the next model write.

### Input Plugins and Model Serialisation

An `IInputPlugin` reads a model file into the engine's simulation context and can also write the current model back out. The reader is selected per-open through the final argument of `swmm_engine_open()` (`NULL` selects the built-in `.inp` reader). To save the in-memory model through a specific writer plugin — for example a GeoPackage container instead of `.inp` — use:

```c
// NULL/empty plugin id is equivalent to swmm_model_write() (.inp writer)
swmm_model_write_with_plugin(engine, "model.gpkg",
                             "org.hydrocouple.openswmm.plugins.geopackage");
```

The plugin id is resolved with the same path / `id` / `id:version` logic as the `[PLUGINS]` section, and the resolved plugin must advertise input capability.

### State-IO Plugins

An `IStateIOPlugin` persists and restores simulation state (hot-start). On save, `swmm_hotstart_save()` dispatches through the registered state-IO plugins in order — the first plugin whose `write_state()` succeeds wins. On read, the engine calls each plugin's `can_read()` (a cheap extension/magic-number sniff) to pick the plugin that recognises the file. A built-in `DefaultStateIOPlugin` is always registered as a fallback, so the native binary hot-start format keeps working when no external plugin claims the file. Missing-object mismatches between the state file and the model are reported through the plugin's `warnings()` rather than as hard errors.

### Data Available to Plugins

At every output time step the engine passes a read-only **SimulationSnapshot** to `update()`. The snapshot exposes per-object results:

- **NodeSnapshot** — depth, head, volume, lateral inflow, total inflow, overflow
- **LinkSnapshot** — flow, depth, velocity, capacity fraction
- **SubcatchSnapshot** — rainfall, evaporation, infiltration, runoff, groundwater flow, groundwater elevation, soil moisture
- **GageSnapshot** — current rainfall rate

See the headers in `include/openswmm/plugin_sdk/` for full details. The engine-side loader, lifecycle dispatcher, and the built-in default plugins live in `src/engine/plugins/` (`PluginFactory.cpp`, `DefaultInputPlugin.cpp`, `DefaultOutputPlugin.cpp`, `DefaultReportPlugin.cpp`, `DefaultStateIOPlugin.cpp`).

## 13.9 Multi-Column Series-File Inputs (CSV/TSV/TSF) {#user_manual_chapter_13_csv}

A new rain-file format, **USER_CSV**, allows rain gage data to be read from multi-column series files. A single file can serve multiple rain gages by specifying a column name after the file path:

```ini
[RAINGAGES]
;;Name   Format   Interval  SCF   Source
RG1      VOLUME   0:15      1.0   TIMESERIES RAIN1
RG2      VOLUME   0:15      1.0   FILE "rain.csv:EAST_GAGE"
RG3      VOLUME   0:15      1.0   FILE "rain.csv:WEST_GAGE"
```

The syntax `"filename.csv:COLUMN_NAME"` tells the engine to open `filename.csv` and read the column whose header matches `COLUMN_NAME` (case-insensitive). An empty column name selects the file's first data column. The file is expected to have a header row whose first column holds a full date-time; the remaining columns hold values.

Three file layouts are recognised, detected automatically from the file's contents (the extension does not matter):

- **CSV** — comma-separated, header row, e.g. `DateTime,EAST_GAGE,WEST_GAGE`.
- **TSV** — the same layout, tab-separated.
- **PCSWMM TSF** — tab-separated with a three-row header: an `IDs:` row naming the columns, a parameter row, and a units row. Date-times use the 12-hour `MM/DD/YYYY hh:mm:ss AM/PM` form.

Date-times in CSV/TSV files may be ISO-8601 (`YYYY-MM-DD HH:MM[:SS]`) or US (`MM/DD/YYYY HH:MM[:SS]`), with an optional trailing AM/PM token for 12-hour clocks.

The same `"path:COLUMN_NAME"` reference works for **named time series**: `[TIMESERIES] TS_E FILE "rain.csv:EAST_GAGE"` binds the series `TS_E` to that column, so inflows, outfall stages, and other objects that reference a time series by name can depend on one column of a shared file.

Each multi-column file is parsed **once** per model open (or per `swmm_gage_reload_rain_files()` call), no matter how many rain gages and time series reference it, and there is no limit on the number of columns or the width of a row. A referenced file that cannot be opened, or that yields no readable rows, fails the model open with an explicit error rather than silently supplying zero values.

The `RainFileFormat` enumeration now includes:

| Value | Constant | Description |
|---|---|---|
| 0 | `NWS_15` | NWS 15-minute data |
| 1 | `NWS_HOURLY` | NWS hourly data |
| 2 | `DSI_3240` | NCDC DSI 3240 hourly |
| 3 | `DSI_3260` | NCDC DSI 3260 15-minute |
| 4 | `HLY_PRCP` | HLY_PRCP format |
| 5 | `STAN_PRCP` | Standard SWMM rain file |
| 6 | `USER_CSV` | User-supplied multi-column CSV (**new in v6**) |

## 13.10 Extension Options (Optional Tags) {#user_manual_chapter_13_ext_options}

The `[OPTIONS]` section now tolerates **extension option keys** that are not part of the standard SWMM vocabulary. Any key the parser does not recognise is stored in an extension-options map as a string key-value pair rather than producing a fatal error (a non-fatal warning is issued). This mechanism allows plugins and coupled models to pass configuration through the familiar `[OPTIONS]` section.

### Input File Syntax

```ini
[OPTIONS]
FLOW_UNITS           CFS
FLOW_ROUTING         DYNWAVE
START_DATE           01/01/2020
END_DATE             01/02/2020

;; Standard new option — Coordinate Reference System (EPSG or PROJ string)
CRS                  EPSG:4326

;; Extension options — stored as-is, available to plugins via API
TURBULENCE_DAMP      0.85
PLUGIN_TIMEOUT       30
MY_VENDOR_SETTING    value123
```

Extension option keys are **upper-cased** for storage. Plugins (or any code using the C API) can retrieve and set these values at runtime:

```c
// Retrieve an extension option
char buffer[256];
swmm_options_get_ext(engine, "TURBULENCE_DAMP", buffer, sizeof(buffer));
double damp = atof(buffer);

// Create or update an extension option
swmm_options_set_ext(engine, "MY_VENDOR_SETTING", "new_value");
```

Note that the **CRS** key is a standard option new to v6 and is stored separately in `SimulationOptions::crs`. All other unrecognised keys go into the extension map.



