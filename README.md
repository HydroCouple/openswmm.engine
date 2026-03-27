# OpenSWMM Engine

**Open Storm Water Management Model — Next-Generation Computational Engine**

[![Unit Testing](https://github.com/HydroCouple/openswmm.engine/actions/workflows/unit_testing.yml/badge.svg)](https://github.com/HydroCouple/openswmm.engine/actions/workflows/unit_testing.yml)
[![Regression Testing](https://github.com/HydroCouple/openswmm.engine/actions/workflows/regression_testing.yml/badge.svg)](https://github.com/HydroCouple/openswmm.engine/actions/workflows/regression_testing.yml)
[![Documentation](https://github.com/HydroCouple/openswmm.engine/actions/workflows/documentation.yml/badge.svg)](https://github.com/HydroCouple/openswmm.engine/actions/workflows/documentation.yml)
[![Deployment](https://github.com/HydroCouple/openswmm.engine/actions/workflows/deployment.yml/badge.svg)](https://github.com/HydroCouple/openswmm.engine/actions/workflows/deployment.yml)
[![Issues](https://img.shields.io/github/issues/HydroCouple/openswmm.engine)](https://github.com/HydroCouple/openswmm.engine/issues)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

[![PyPI](https://img.shields.io/pypi/v/openswmm.svg)](https://pypi.org/project/openswmm)
[![Downloads](https://pepy.tech/badge/openswmm)](https://pepy.tech/project/openswmm)
[![Python](https://img.shields.io/pypi/pyversions/openswmm.svg)](https://pypi.org/project/openswmm)
[![Wheel](https://img.shields.io/pypi/wheel/openswmm.svg)](https://pypi.org/project/openswmm)

---

## Overview

OpenSWMM Engine is a community-driven, open-source continuation of the EPA Storm Water Management Model (SWMM). SWMM is a dynamic hydrology–hydraulic–water quality simulation model used for single-event or long-term (continuous) simulation of runoff quantity and quality from primarily urban areas.

This project preserves and advances the rich legacy of SWMM by developing high-quality, QA/QC'd code while building an active community for sustainable maintenance and improvement. The community is actively working with organizations including ASCE's Environmental and Water Resources Institute (EWRI) and Water Environment Federation (WEF) to ensure the long-term sustainability of the SWMM codebase.

## What's New in v6.0.0

OpenSWMM Engine v6.0.0 is a major modernization of the SWMM computational engine. Key improvements include:

### Architecture & Performance

- **Data-Oriented Design** — Core data structures refactored to Structure of Arrays (SoA) layout for cache efficiency and SIMD-friendly computation.
- **Reentrant Engine** — Global state eliminated; all simulation state encapsulated in an opaque `SWMM_Engine` handle, enabling multiple independent simulations in the same process.
- **Plugin-Based I/O** — Output and report writing abstracted through a plugin interface. Plugins receive read-only simulation snapshots on a dedicated I/O thread.
- **C++20 Codebase** — New engine written in modern C++20; legacy EPA SWMM 5.x solver preserved unmodified in `src/legacy/`.

### New C API

A comprehensive, domain-split C API replaces the monolithic legacy interface:

| Header | Domain |
|---|---|
| `openswmm_engine.h` | Engine lifecycle, error codes, state machine |
| `openswmm_model.h` | Model building, validation, serialization, options |
| `openswmm_nodes.h` | Junctions, outfalls, storage, dividers |
| `openswmm_links.h` | Conduits, pumps, orifices, weirs, outlets |
| `openswmm_subcatchments.h` | Subcatchments, infiltration, coverage |
| `openswmm_gages.h` | Rain gages (timeseries and file sources) |
| `openswmm_pollutants.h` | Pollutant definitions and runtime injection |
| `openswmm_tables.h` | Time series, curves, and patterns |
| `openswmm_inflows.h` | External inflows, DWF, RDII |
| `openswmm_controls.h` | Control rules and direct link actions |
| `openswmm_infrastructure.h` | Transects, streets, inlets, LID controls |
| `openswmm_spatial.h` | CRS, coordinates, polylines, polygons |
| `openswmm_quality.h` | Landuse, buildup, washoff, treatment |
| `openswmm_massbalance.h` | Continuity errors and cumulative flux totals |
| `openswmm_callbacks.h` | Progress, warning, and step callbacks |
| `openswmm_hotstart.h` | Hot start file save/load/modify |
| `openswmm_statistics.h` | Node, link, and subcatchment statistics |

### Additional Features

- **Hot Start API** — Save, load, modify, and query hot start files through a transparent C ABI.
- **CRS Support** — Coordinate reference system specification via OPTIONS for spatial data.
- **User Flags** — Custom USER_FLAGS section for user-defined metadata on objects.
- **Plugin SDK** — Header-only development kit for building output/report plugins.
- **HEC-22 Inlet Analysis** — Street inlet capture, grate and curb inlets (from SWMM 5.2).
- **Variable Speed Pumps** — Type5 pump curves with speed scaling.
- **New Storage Shapes** — Conical and pyramidal shapes with elliptical/rectangular bases.

### Input File Extensions

The new engine extends the standard SWMM `.inp` format with several new sections while remaining fully backward-compatible with existing input files. Below is a complete `.inp` snippet demonstrating all new features:

```ini
;; EXTENSION OPTIONS — new and custom keys in [OPTIONS]
;; Standard SWMM options work as before.  Any unrecognized key is stored
;; in an extension map accessible at runtime via the C/Python API.

[OPTIONS]
FLOW_UNITS           CFS
INFILTRATION         HORTON
FLOW_ROUTING         DYNWAVE
START_DATE           01/01/2024
START_TIME           00:00:00
END_DATE             01/02/2024
END_TIME             00:00:00
ROUTING_STEP         00:00:30
REPORT_STEP          00:05:00

;; New built-in option (v6.0.0)
CRS                  EPSG:4326

;; Extension options — stored automatically, readable by plugins
MY_STABILITY_FACTOR  1.05
PLUGIN_LOG_LEVEL     DEBUG


;; USER FLAGS — typed custom metadata on any model object
;; Define a schema of flag names with type and default value.
;; Supported types: BOOLEAN, INTEGER, REAL, STRING.

[USER_FLAGS]
;;Name              Type      Default   Description
INSPECTED           BOOLEAN   NO        'Has the asset been field-inspected?'
PRIORITY            INTEGER   0         'Maintenance priority (0=none, 1=low, 5=critical)'
ROUGHNESS_ADJ       REAL      1.0       'Roughness calibration multiplier'
ASSET_ID            STRING    ""        'External asset management system ID'

;; Assign flag values to individual objects.
;; ObjectType can be NODE, LINK, SUBCATCHMENT, or GAGE.

[USER_FLAG_VALUES]
;;ObjectType   ObjectName   FlagName        Value
NODE           J1           INSPECTED       YES
NODE           J1           PRIORITY        3
NODE           J2           ASSET_ID        "AM-00412"
LINK           C1           ROUGHNESS_ADJ   1.12
LINK           C1           INSPECTED       YES
LINK           C2           PRIORITY        1
SUBCATCHMENT   S1           INSPECTED       NO
SUBCATCHMENT   S1           ASSET_ID        "AM-00501"


;; PLUGINS — load output/report plugins at runtime
;; Each line: <shared-library-path>  [arg1  arg2  ...]
;; The first token is the path to a .so / .dylib / .dll.
;; Remaining tokens are passed to the plugin's initialize() method.

[PLUGINS]
;; Write time-series results to HDF5 instead of (or alongside) the .out file
./plugins/hdf5_output.so      file="results.h5"  compress=9

;; Write a custom CSV summary report after simulation
./plugins/csv_report.dylib    file="summary.csv"  delimiter=","

;; Multiple plugins can be loaded — they run concurrently on the I/O thread
./plugins/netcdf_output.so    file="results.nc"   variables="depth,flow"
```

#### How Each Feature Works

**Extension Options** — Any keyword the parser does not recognize in `[OPTIONS]` is stored in `options.ext_options` and can be read at runtime:

```python
# Python
val = solver.get_option("MY_STABILITY_FACTOR")   # "1.05"
```

```c
/* C API */
char buf[256];
swmm_options_get(engine, "MY_STABILITY_FACTOR", buf, sizeof(buf));
```

**User Flags** — Flags attach structured metadata to objects for asset management, calibration tracking, or custom workflows. Plugins can read them during `validate()` or `prepare()` from the `SimulationContext`.

**Plugins** — Shared libraries loaded from the `[PLUGINS]` section implement one or both of:

| Interface | Purpose | Thread |
|---|---|---|
| `IOutputPlugin` | Write time-series results at each reporting step | I/O thread |
| `IReportPlugin` | Write summary statistics after simulation ends | Main thread |

Each plugin library exports a single entry point:

```cpp
extern "C" openswmm::IPluginComponentInfo* openswmm_plugin_info(void);
```

The engine calls the plugin through a managed lifecycle:

```
initialize(args) → validate(ctx) → prepare(ctx) → update(snapshot)... → finalize(ctx)
                                                   └─ write_summary(ctx)  [report plugins]
```

Output plugins receive a `SimulationSnapshot` — a read-only deep copy of simulation state safe to consume on the I/O thread while the main simulation advances.

**Custom Sections** — Plugins and embedders can also register handlers for entirely new `.inp` sections via the C++ `SectionRegistry` API:

```cpp
engine.registry().register_custom("MY_CUSTOM_DATA",
    [](SimulationContext& ctx, const std::vector<std::string>& lines) {
        for (const auto& line : lines) {
            // parse and store in ctx or plugin-managed storage
        }
    });
```

This allows the `.inp` to contain:

```ini
[MY_CUSTOM_DATA]
;;ID     Param1   Param2
OBJ_1    42.0     enabled
OBJ_2    17.5     disabled
```

The two built-in plugins (`DefaultOutputPlugin` for `.out` and `DefaultReportPlugin` for `.rpt`) are always available and require no `[PLUGINS]` entry. The Plugin SDK headers are in `include/openswmm/plugin_sdk/`.

### Testing & Quality

- **Google Test** — All unit tests migrated from Boost.Test to Google Test 1.15.2.
- **Comprehensive Test Suite** — Legacy engine (73+ tests), legacy output (41 tests), and new engine unit tests.
- **Multi-Platform CI** — GitHub Actions pipelines for Windows, Linux, and macOS (Intel + ARM64).
- **Doxygen API Docs** — All 19 public C API headers fully documented with Doxygen conventions.

---

## Project Structure

```
openswmm.engine/
├── include/openswmm/
│   ├── engine/           # New engine public C API headers (19 headers)
│   └── legacy/           # Legacy SWMM 5.x public headers
├── src/
│   ├── engine/           # New C++20 engine implementation
│   ├── legacy/engine/    # Original EPA SWMM 5.x solver (preserved unmodified)
│   ├── legacy/output/    # Original binary output reader
│   ├── plugin_sdk/       # Header-only plugin development kit
│   └── cli/              # Command-line interface
├── tests/
│   ├── unit/legacy/      # Legacy solver and output tests (Google Test)
│   ├── unit/engine/      # New engine unit tests
│   ├── regression/       # Regression tests (new vs. legacy)
│   └── benchmarks/       # Performance benchmarks (Google Benchmark)
├── python/               # Python bindings (Cython + scikit-build)
├── docs/                 # Doxygen config and technical manuals
├── cmake/                # CMake helper modules
└── .github/workflows/    # CI/CD pipelines
```

## Prerequisites

| Requirement | Version |
|---|---|
| CMake | 3.21 or higher |
| C compiler | C17 support (GCC 10+, Clang 12+, MSVC 19.29+) |
| C++ compiler | C++20 support (GCC 10+, Clang 14+, MSVC 19.29+) |
| vcpkg | 2025.02.14 (for test dependencies) |
| Python | 3.9–3.13 (optional, for bindings) |
| Ninja | Recommended for Linux/macOS builds |

## Build Instructions

### C/C++ Engine

```bash
# Clone the repository
git clone https://github.com/HydroCouple/openswmm.engine.git
cd openswmm.engine

# Bootstrap vcpkg (for test dependencies)
git clone https://github.com/microsoft/vcpkg.git
./vcpkg/bootstrap-vcpkg.sh   # or bootstrap-vcpkg.bat on Windows

# Configure and build using a platform preset
# Available presets: Windows, Windows-debug, Linux, Linux-debug, Darwin, Darwin-debug
export VCPKG_ROOT=$(pwd)/vcpkg

cmake --preset=<platform> -B build
cmake --build build --config Release

# Build with tests enabled
cmake --preset=<platform>-debug -B build-debug -DOPENSWMM_BUILD_TESTS=ON
cmake --build build-debug --config Debug
```

### Running Tests

```bash
# Run all C++ unit tests
ctest --test-dir build-debug -C Debug --output-on-failure

# Run with verbose output
ctest --test-dir build-debug -C Debug --output-on-failure -V
```

### Packaging

```bash
# Create distributable archives (ZIP + TGZ)
cmake --build build --target package
```

### Python Bindings

```bash
cd python

# Install requirements
python -m pip install -r requirements.txt

# Build and install
python -m pip install .

# Run Python tests
python -m pytest -v tests
```

## Python Usage

The `openswmm` package provides Cython bindings for the full C API.
Domain objects are constructed by passing an active `Solver` instance.

### Running a Simulation

```python
from openswmm.engine import Solver, Nodes, Links, Subcatchments, Gages

# Context manager handles open/initialize/start and end/report/close/destroy
with Solver("model.inp", "model.rpt", "model.out") as s:
    nodes = Nodes(s)
    links = Links(s)
    subcatchments = Subcatchments(s)
    gages = Gages(s)

    while s.step():
        # Per-element access by name or integer index
        depth  = nodes.get_depth("J1")
        flow   = links.get_flow("C1")
        runoff = subcatchments.get_runoff("S1")

        # NumPy bulk access (single memcpy — fast)
        all_depths = nodes.get_depths_bulk()       # shape (n_nodes,)
        all_flows  = links.get_flows_bulk()         # shape (n_links,)

        # Runtime forcing
        nodes.set_lateral_inflow("J2", 0.5)
        gages.set_rainfall(0, 25.4)
```

### Advanced Forcing with Persistence

```python
from openswmm.engine import Solver, Nodes, Forcing, ForcingMode, ForcingTarget

with Solver("model.inp", "model.rpt", "model.out") as s:
    nodes   = Nodes(s)
    forcing = Forcing(s)

    j1 = nodes.get_index("J1")

    # Apply a persistent lateral inflow (held every step until cleared)
    forcing.node_lat_inflow(j1, 1.5, ForcingMode.REPLACE, persist=True)

    # Additive forcing (added to model-computed value)
    forcing.gage_rainfall(0, 10.0, ForcingMode.ADD, persist=True)

    while s.step():
        pass

    # Clear specific object or all forcing
    forcing.clear(ForcingTarget.NODE, j1)
    forcing.clear_all()
```

### Programmatic Model Building (No .inp File)

```python
from openswmm.engine import (
    ModelBuilder, Nodes, Links,
    NodeType, LinkType, XSectShape,
)

m = ModelBuilder()

# Add objects
m.add_node("J1", NodeType.JUNCTION)
m.add_node("J2", NodeType.JUNCTION)
m.add_node("OUT1", NodeType.OUTFALL)
m.add_link("C1", LinkType.CONDUIT)
m.add_link("C2", LinkType.CONDUIT)

# Set geometry
m.set_node_invert(0, 10.0)
m.set_node_invert(1, 8.0)
m.set_node_invert(2, 5.0)
m.set_link_nodes(0, 0, 1)        # C1: J1 -> J2
m.set_link_nodes(1, 1, 2)        # C2: J2 -> OUT1
m.set_link_length(0, 400.0)
m.set_link_length(1, 400.0)
m.set_link_roughness(0, 0.013)
m.set_link_roughness(1, 0.013)
m.set_link_xsect(0, XSectShape.CIRCULAR, 1.0)
m.set_link_xsect(1, XSectShape.CIRCULAR, 1.0)

# Set simulation options
m.set_option("FLOW_UNITS", "CFS")
m.set_option("ROUTING_MODEL", "DYNWAVE")
m.set_option("START_DATE", "01/01/2024")
m.set_option("END_DATE", "01/02/2024")

# Validate, finalize, and simulate
m.validate()
m.finalize()

solver = m.to_solver()
solver.start()
while solver.step():
    pass
solver.end()

# Optionally write to .inp for inspection
m.write("generated_model.inp")

solver.destroy()
```

### Reading Binary Output Files

```python
from openswmm.engine import OutputReader, OutNodeVar, OutLinkVar

with OutputReader("model.out") as out:
    # Query metadata
    print(f"Nodes: {out.get_node_count()}")
    print(f"Links: {out.get_link_count()}")
    print(f"Periods: {out.get_period_count()}")
    print(f"Report step: {out.get_report_step()} sec")

    # List object IDs
    for i in range(out.get_node_count()):
        print(f"  Node {i}: {out.get_node_id(i)}")

    # Read all node depths at each reporting period
    for t in range(out.get_period_count()):
        depths = out.get_node_result(t, OutNodeVar.DEPTH)     # float32 array
        flows  = out.get_link_result(t, OutLinkVar.FLOW)      # float32 array
        print(f"  Period {t}: max depth = {depths.max():.3f}")

    # Time series for a single node
    node_depths = out.get_node_series(
        node_idx=0,
        var=OutNodeVar.DEPTH,
        start_period=0,
        end_period=out.get_period_count() - 1,
    )
```

### Hot Start Save and Restore

```python
from openswmm.engine import Solver, HotStart

# Run part of a simulation and save state
with Solver("model.inp", "model.rpt", "model.out") as s:
    for _ in range(100):
        if not s.step():
            break
    HotStart.save(s, "checkpoint.hsf")

# Later: restore from hot start
hs = HotStart.open("checkpoint.hsf")

# Optionally modify state before applying
hs.set_node_depth("J1", 2.5)

with Solver("model.inp", "model.rpt", "model2.out") as s2:
    hs.apply(s2)
    while s2.step():
        pass

hs.close()
```

### Mass Balance and Statistics

```python
from openswmm.engine import Solver, MassBalance, Statistics, RunoffTotal

with Solver("model.inp", "model.rpt", "model.out") as s:
    while s.step():
        pass

    mb = MassBalance(s)
    stats = Statistics(s)

    # Continuity errors
    print(f"Runoff error: {mb.get_runoff_continuity_error():.4f}%")
    print(f"Routing error: {mb.get_routing_continuity_error():.4f}%")

    # Cumulative totals
    precip = mb.get_runoff_total(RunoffTotal.RAINFALL)
    runoff = mb.get_runoff_total(RunoffTotal.RUNOFF)
    print(f"Total precip: {precip:.2f}, Total runoff: {runoff:.2f}")

    # Per-object statistics
    print(f"Node 0 max depth: {stats.node_max_depth(0):.3f}")
    print(f"Link 0 max flow:  {stats.link_max_flow(0):.3f}")
```

For the full API reference, see the [documentation](https://hydrocouple.github.io/openswmm.engine).



## Libraries Built

| Target | Description |
|---|---|
| `openswmm_legacy_engine` | Original EPA SWMM 5.x solver (shared library) |
| `openswmm_legacy_output` | Original SWMM binary output reader (shared library) |
| `openswmm_engine` | New refactored C++20 engine (shared library) |
| `openswmm_plugin_sdk` | Header-only plugin SDK (INTERFACE library) |
| `openswmm_cli` | Command-line executable |

## Documentation

API documentation is auto-generated with Doxygen and deployed to GitHub Pages:

**[OpenSWMM Engine API Documentation](https://hydrocouple.github.io/openswmm.engine)**

The documentation includes:
- Full C API reference with parameter descriptions and usage notes
- Technical reference manuals (Hydrology, Hydraulics, Water Quality)
- User manual with modeling capabilities and examples
- Architecture design decisions and implementation plan

## Contributing

Contributions are welcome. Please:

1. Fork the repository and create a feature branch.
2. Ensure all tests pass (`ctest` for C++, `pytest` for Python).
3. Follow existing code style and naming conventions.
4. Submit a pull request against the `develop` branch.

## License

This project is licensed under the **MIT License** — see [LICENSE](LICENSE) for details.

Copyright 2026 HydroCouple. Original EPA SWMM material is in the public domain under 17 USC § 105.

## Acknowledgements

OpenSWMM builds on the foundational work of the EPA Storm Water Management Model, originally developed by Lewis A. Rossman at the U.S. EPA Office of Research and Development. See [docs/authors.md](docs/authors.md) for the complete list of authors and contributors.
