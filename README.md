# OpenSWMM Engine

<p align="center">
  <img src="docs/images/hydrocouple_logo.png" alt="OpenSWMM" width="120">
</p>

**Open Storm Water Management Model — Next-Generation Computational Engine**

[![Unit Testing](https://github.com/HydroCouple/openswmm.engine/actions/workflows/unit_testing.yml/badge.svg)](https://github.com/HydroCouple/openswmm.engine/actions/workflows/unit_testing.yml)
[![Unit Testing Python](https://github.com/HydroCouple/openswmm.engine/actions/workflows/unit_testing_python.yml/badge.svg)](https://github.com/HydroCouple/openswmm.engine/actions/workflows/unit_testing_python.yml)
[![Documentation](https://github.com/HydroCouple/openswmm.engine/actions/workflows/documentation.yml/badge.svg)](https://github.com/HydroCouple/openswmm.engine/actions/workflows/documentation.yml)
[![Issues](https://img.shields.io/github/issues/HydroCouple/openswmm.engine)](https://github.com/HydroCouple/openswmm.engine/issues)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![PyPI](https://img.shields.io/pypi/v/openswmm.svg)](https://pypi.org/project/openswmm)
[![Downloads](https://pepy.tech/badge/openswmm)](https://pepy.tech/project/openswmm)
[![Python](https://img.shields.io/pypi/pyversions/openswmm.svg)](https://pypi.org/project/openswmm)
[![Wheel](https://img.shields.io/pypi/wheel/openswmm.svg)](https://pypi.org/project/openswmm)

## Documentation

| | Site | Contents |
|---|---|---|
| **C / C++ Engine** | **[hydrocouple.org/openswmm.engine](https://hydrocouple.org/openswmm.engine)** | Full C API reference, hydrology / hydraulics / water-quality reference manuals, user manual, architecture notes. |
| **Python Bindings** | **[hydrocouple.org/openswmm.engine/python](https://hydrocouple.org/openswmm.engine/python)** | Quickstart, per-domain user guide, Cython API reference, SWMM 5 → v6 migration. |

Both sites cross-link from their top navigation.

---

## Overview

OpenSWMM Engine is a community-driven, open-source continuation of the EPA Storm Water Management Model — a dynamic hydrology, hydraulic, and water-quality simulator for urban runoff. The project preserves the SWMM legacy under QA/QC and builds the community needed for long-term maintenance, working with ASCE/EWRI and the Water Environment Federation.

## What's New in v6.0.0

### Architecture & Performance

- **Data-Oriented Design** — Core state refactored to Structure-of-Arrays for cache efficiency and SIMD-friendly batches.
- **Reentrant Engine** — All simulation state lives behind an opaque `SWMM_Engine` handle; multiple independent simulations can run in the same process.
- **Plugin-Based I/O** — Output and report writing dispatch through plugin interfaces on a dedicated I/O thread.
- **C++20 Codebase** — Modern C++20 implementation; the legacy EPA SWMM 5.x solver is preserved unmodified in `src/legacy/`.

### Process Formulation Enhancements

#### Implemented

- **Semi-Implicit Node Continuity** — Single-equation free-surface/surcharge formulation that removes the legacy two-branch discontinuity. Enabled via `NODE_CONTINUITY SEMI_IMPLICIT` (default). [Reference »](https://hydrocouple.org/openswmm.engine)
- **Anderson Acceleration for Picard Iteration** — Depth-2 mixing of residual history cuts iteration counts 25–50% on stiff surcharge transitions with safe fall-back to standard Picard. Enabled via `ANDERSON_ACCEL YES`.
- **Spatially Explicit Overland Flow & Groundwater (2D)** — 2D overland-flow grid coupled to the 1D pipe network. Surcharge re-routes over terrain to downstream nodes, lateral groundwater exchanges are tracked explicitly, and green-infrastructure placement is spatially resolved.
- **Dynamic Preissmann Slot** — Geometry-dependent slot width replaces the fixed-width slot at the free-surface / pressurized transition, improving stability for rapidly filling or draining conduits.
- **Physics-Based Initial Abstraction Recovery** — RDII initial abstraction now evolves as an exponential depletion/recovery process with additive base + thermal recovery rates and frozen-ground suppression. Seasonal RDII variation emerges from temperature dynamics on a single RTK set per sewershed — no monthly parameter tables required. Configured via the new `[RDII_DECAY]` input section.

  $$IA_{avail}(t+\Delta t) = IA_{max} - \bigl(IA_{max} - IA_{avail}(t)\bigr) \cdot e^{-k_{rec}(T)\,\Delta t}, \quad k_{rec}(T) = k_0 + k_T \cdot e^{\,\theta(T - T_{ref})}$$

#### In Development

- **Spatially Explicit Inlets** — Promotes inlets to mode-switching junction nodes that capture street flow when gutter spread exceeds a threshold and revert to passive junctions otherwise.
- **LID as Storage Nodes** — Maps LID layers (surface, media, gravel) onto extended storage nodes using a reduced-physics kinematic Richards ODE for two-way hydraulic feedback.

### New C API

A domain-split C API replaces the monolithic legacy interface. Full reference at the [C engine documentation site](https://hydrocouple.org/openswmm.engine).

| Header | Domain |
|---|---|
| `openswmm_engine.h` | Engine lifecycle, error codes, state machine |
| `openswmm_model.h` | Model building, validation, serialization, options |
| `openswmm_nodes.h` | Junctions, outfalls, storage, dividers |
| `openswmm_links.h` | Conduits, pumps, orifices, weirs, outlets |
| `openswmm_subcatchments.h` | Subcatchments, infiltration, coverage |
| `openswmm_gages.h` | Rain gages |
| `openswmm_pollutants.h` | Pollutant definitions and runtime injection |
| `openswmm_tables.h` | Time series, curves, patterns |
| `openswmm_inflows.h` | External inflows, DWF, RDII (incl. `[RDII_DECAY]`) |
| `openswmm_controls.h` | Control rules and direct link actions |
| `openswmm_infrastructure.h` | Transects, streets, inlets, LID controls |
| `openswmm_spatial.h` | CRS, coordinates, polylines, polygons |
| `openswmm_quality.h` | Landuse, buildup, washoff, treatment |
| `openswmm_massbalance.h` | Continuity errors and cumulative flux totals |
| `openswmm_callbacks.h` | Progress, warning, and step callbacks |
| `openswmm_hotstart.h` | Hot start file save / load / modify |
| `openswmm_statistics.h` | Node, link, and subcatchment statistics |
| `openswmm_geopackage.h` | Optional GeoPackage I/O |

### Additional Features

- **Hot Start API** — Save, load, modify, and query hot-start files through a stable C ABI.
- **CRS Support** — Coordinate reference systems specified in `[OPTIONS]`.
- **User Flags** — Typed `[USER_FLAGS]` / `[USER_FLAG_VALUES]` sections attach custom metadata (boolean, integer, real, string) to nodes, links, subcatchments, or gages.
- **Extension Options** — Unrecognized `[OPTIONS]` keys are preserved and exposed to plugins at runtime.
- **Plugin SDK** — Header-only SDK in `include/openswmm/plugin_sdk/` for input, output, and report plugins; the `IPluginComponentInfo` entry point advertises capabilities and supports custom `.inp` section handlers via `SectionRegistry`.
- **GeoPackage I/O** — Optional SQLite + spatial backing store for inputs, results, observed series, and topology in a single `.gpkg` file (`-DOPENSWMM_WITH_GEOPACKAGE=ON`).
- **HEC-22 Inlet Analysis** — Street inlet capture with grate and curb inlets (SWMM 5.2).
- **Variable Speed Pumps** — Type 5 pump curves with speed scaling.
- **New Storage Shapes** — Conical and pyramidal shapes with elliptical and rectangular bases.

## Quick Start

```bash
# C / C++ engine
git clone https://github.com/HydroCouple/openswmm.engine.git
cd openswmm.engine
git clone https://github.com/microsoft/vcpkg.git && ./vcpkg/bootstrap-vcpkg.sh
export VCPKG_ROOT=$(pwd)/vcpkg

cmake --preset=<platform> -B build -DOPENSWMM_WITH_GEOPACKAGE=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure

# Python bindings (PyPI)
pip install openswmm
```

Presets: `Windows`, `Windows-debug`, `Linux`, `Linux-debug`, `Darwin`, `Darwin-debug`. Full build, test, and packaging instructions are in the [C engine docs](https://hydrocouple.org/openswmm.engine).

```python
from openswmm.engine import Solver, Nodes, Links

with Solver("model.inp", "model.rpt", "model.out") as s:
    nodes, links = Nodes(s), Links(s)
    while s.step():
        depth = nodes.get_depth("J1")
        flow  = links.get_flow("C1")
```

Forcing, model building, hot-start, bulk NumPy access, mass balance, and statistics are covered in the [Python docs](https://hydrocouple.org/openswmm.engine/python).

## Glossary

Brief definitions of the domain terms used throughout this README. Full treatment lives in the [reference manuals](https://hydrocouple.org/openswmm.engine).

- **RDII** — Rainfall-Dependent Inflow & Infiltration. Stormwater that enters sanitary or combined sewers through cracks, joints, defective laterals, and roof / foundation drains during and after rainfall.
- **RTK** — The triplet `(R, T, K)` that parameterises a SWMM synthetic unit hydrograph for RDII: `R` is the long-term fraction of rainfall that becomes RDII, `T` is the time to peak (hours), and `K` is the ratio of base time to peak time.
- **IA (Initial Abstraction)** — Rainfall depth absorbed by the catchment before any RDII response begins (interception, surface storage, soil wetting). Recovers between events.
- **DWF** — Dry-Weather Flow. Base sanitary flow plus infiltration unrelated to rainfall, typically specified as an average value with diurnal / day-of-week / monthly patterns.
- **LID** — Low-Impact Development. Distributed green-infrastructure controls (bio-retention cells, permeable pavement, green roofs, infiltration trenches, rain gardens) that intercept, store, and infiltrate runoff at the source.
- **CRS** — Coordinate Reference System. The geodetic / projected coordinate frame (e.g. `EPSG:4326`) the model's spatial data is expressed in.
- **CFS / CMS** — Cubic feet per second / cubic metres per second. The two flow-unit conventions exposed via `FLOW_UNITS`.
- **Dynamic Wave Routing** — Full Saint-Venant momentum solver for link flow, used for backwater, surcharge, and pressurized conditions.
- **Preissmann Slot** — A narrow virtual slot added to a closed conduit's cross-section so that pressurized flow can be solved with the same free-surface equations. The dynamic slot adjusts width with geometry to smooth the surface ↔ pressure transition.
- **Surcharge** — A pipe flowing full and under pressure (HGL above the crown), typically caused by downstream backwater or capacity exceedance.
- **Picard Iteration** — Fixed-point iteration used inside the dynamic-wave timestep to converge implicit node depths. Anderson Acceleration is a residual-history accelerator on top of Picard.
- **Hot Start** — A saved end-of-run state (depths, volumes, IA, snow, GW) that initialises a subsequent simulation, letting long runs be split into checkpoints or warm runs into operational forecasts.
- **HEC-22** — FHWA Hydraulic Engineering Circular No. 22, the design reference whose grate and curb-opening capture equations are used by the inlet-analysis module.
- **GeoPackage** — OGC standard for a SQLite-based, single-file container holding spatial features and tabular data with full CRS metadata.

## Prerequisites

| Requirement | Version |
|---|---|
| CMake | 3.21+ |
| C compiler | C17 (GCC 10+, Clang 12+, MSVC 19.29+) |
| C++ compiler | C++20 (GCC 10+, Clang 14+, MSVC 19.29+) |
| vcpkg | 2025.02.14 |
| Python | 3.9 – 3.13 (optional) |
| Ninja | recommended on Linux/macOS |

## Project Structure

```
openswmm.engine/
├── include/openswmm/
│   ├── engine/           # New engine public C API headers
│   └── legacy/           # Legacy SWMM 5.x public headers
├── src/
│   ├── engine/           # New C++20 engine implementation
│   │   ├── input/geopackage/  # Optional GeoPackage I/O
│   │   └── 2d/                # 2D overland-flow & groundwater coupling
│   ├── legacy/           # Original EPA SWMM 5.x solver and output reader
│   ├── plugin_sdk/       # Header-only plugin SDK
│   └── cli/              # Command-line interface
├── tests/
│   ├── unit/legacy/      # Legacy solver & output tests
│   ├── unit/engine/      # New engine unit tests
│   ├── regression/       # New-vs-legacy regression tests
│   └── benchmarks/       # Performance benchmarks
├── python/               # Cython bindings (scikit-build)
├── docs/                 # Doxygen config and technical manuals
└── .github/workflows/    # CI/CD pipelines
```

## Libraries Built

| Target | Description |
|---|---|
| `openswmm_legacy_engine` | Original EPA SWMM 5.x solver (shared) |
| `openswmm_legacy_output` | Original SWMM binary output reader (shared) |
| `openswmm_engine` | New refactored C++20 engine (shared) |
| `openswmm_geopackage` | GeoPackage I/O (static, optional — requires SQLite3) |
| `openswmm_plugin_sdk` | Header-only plugin SDK (INTERFACE) |
| `openswmm_cli` | Command-line executable |

## Contributing

Contributions are welcome — bug reports, fixes, new features, docs, tests, and benchmarks.

1. Read [CONTRIBUTING.md](CONTRIBUTING.md) for the development workflow and the [Code of Conduct](CODE_OF_CONDUCT.md).
2. Fork the repo and create a feature branch.
3. Ensure C++ (`ctest`) and Python (`pytest`) tests pass.
4. Follow existing style and naming.
5. Open a PR against `develop`.

### Contributor License Agreement

First-time contributors must sign the project [CLA](CLA.md) before a pull request can be merged. The CLA grants the project a perpetual, royalty-free copyright and patent license to your contributions and preserves the project's ability to relicense in the future; **you retain full copyright ownership** of your work.

Signing is automated through [CLA Assistant](https://cla-assistant.io) — when you open your first PR, a bot comments with a one-click sign-in link. The CLA covers all subsequent contributions, so you only sign once. Corporate contributors should additionally submit a CCLA per [CLA §6](CLA.md#6-corporate-contributors).

## License

MIT — see [LICENSE](LICENSE). Original EPA SWMM material is in the public domain under 17 USC § 105.

## Acknowledgements

OpenSWMM builds on the EPA Storm Water Management Model originally developed by Lewis A. Rossman at the U.S. EPA Office of Research and Development. See [docs/authors.md](docs/authors.md) for the full contributor list.
