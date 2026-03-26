# OpenSWMM Engine

**Open Storm Water Management Model — Next-Generation Computational Engine**

[![Unit Testing](https://github.com/HydroCouple/OpenSWMMCore/actions/workflows/unit_testing.yml/badge.svg)](https://github.com/HydroCouple/OpenSWMMCore/actions/workflows/unit_testing.yml)
[![Regression Testing](https://github.com/HydroCouple/OpenSWMMCore/actions/workflows/regression_testing.yml/badge.svg)](https://github.com/HydroCouple/OpenSWMMCore/actions/workflows/regression_testing.yml)
[![Documentation](https://github.com/HydroCouple/OpenSWMMCore/actions/workflows/documentation.yml/badge.svg)](https://github.com/HydroCouple/OpenSWMMCore/actions/workflows/documentation.yml)
[![Deployment](https://github.com/HydroCouple/OpenSWMMCore/actions/workflows/deployment.yml/badge.svg)](https://github.com/HydroCouple/OpenSWMMCore/actions/workflows/deployment.yml)
[![Issues](https://img.shields.io/github/issues/HydroCouple/OpenSWMMCore)](https://github.com/HydroCouple/OpenSWMMCore/issues)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

| Python Bindings | |
|---|---|
| [![PyPI](https://img.shields.io/pypi/v/openswmm.svg)](https://pypi.org/project/openswmm) | [![Downloads](https://pepy.tech/badge/openswmm)](https://pepy.tech/project/openswmm) |
| [![Python](https://img.shields.io/pypi/pyversions/openswmm.svg)](https://pypi.org/project/openswmm) | [![Wheel](https://img.shields.io/pypi/wheel/openswmm.svg)](https://pypi.org/project/openswmm) |

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

### Testing & Quality

- **Google Test** — All unit tests migrated from Boost.Test to Google Test 1.15.2.
- **Comprehensive Test Suite** — Legacy engine (73+ tests), legacy output (41 tests), and new engine unit tests.
- **Multi-Platform CI** — GitHub Actions pipelines for Windows, Linux, and macOS (Intel + ARM64).
- **Doxygen API Docs** — All 19 public C API headers fully documented with Doxygen conventions.

---

## Project Structure

```
OpenSWMMCore/
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
git clone https://github.com/HydroCouple/OpenSWMMCore.git
cd OpenSWMMCore

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

## Python Usage Examples

End-to-end modeling workflows via a Python API

- Creating model instances
- Adding objects 
- Parameterizing model
- Runtime coupling and feedback
- Saving model instance to file
- Saving hotstart files
- etc.

Check out the documentation for the [API](python/index.html) 


```python
from openswmm.engine import Solver, Nodes, Links, Subcatchments

with Solver("model.inp", "model.rpt", "model.out") as solver:
    solver.start(save_results=True)

    nodes = Nodes(solver)
    links = Links(solver)
    subcatchments = Subcatchments(solver)

    while solver.step() > 0:
        # access current time-step values
        depth = nodes["Node1"].depth
        flow  = links["Conduit1"].flow

    solver.end()
    solver.report()
```



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

**[OpenSWMM Engine API Documentation](https://hydrocouple.github.io/OpenSWMMCore)**

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
