# OpenSWMM Engine

<p align="center">
  <img src="https://raw.githubusercontent.com/HydroCouple/openswmm.engine/develop/docs/images/hydrocouple_logo.png" alt="OpenSWMM" width="120">
</p>

**Open Storm Water Management Model — Next-Generation Computational Engine**

[![Unit Testing](https://github.com/HydroCouple/openswmm.engine/actions/workflows/unit_testing.yml/badge.svg)](https://github.com/HydroCouple/openswmm.engine/actions/workflows/unit_testing.yml)
[![Unit Testing Python](https://github.com/HydroCouple/openswmm.engine/actions/workflows/unit_testing_python.yml/badge.svg)](https://github.com/HydroCouple/openswmm.engine/actions/workflows/unit_testing_python.yml)
[![Documentation](https://github.com/HydroCouple/openswmm.engine/actions/workflows/documentation.yml/badge.svg)](https://github.com/HydroCouple/openswmm.engine/actions/workflows/documentation.yml)
[![CodeQL](https://github.com/HydroCouple/openswmm.engine/actions/workflows/codeql.yml/badge.svg)](https://github.com/HydroCouple/openswmm.engine/actions/workflows/codeql.yml)
[![OpenSSF Scorecard](https://api.securityscorecards.dev/projects/github.com/HydroCouple/openswmm.engine/badge)](https://securityscorecards.dev/viewer/?uri=github.com/HydroCouple/openswmm.engine)
[![Issues](https://img.shields.io/github/issues/HydroCouple/openswmm.engine)](https://github.com/HydroCouple/openswmm.engine/issues)
[![License: Apache 2.0](https://img.shields.io/badge/License-Apache_2.0-blue.svg)](https://github.com/HydroCouple/openswmm.engine/blob/HEAD/LICENSE)
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

OpenSWMM Engine is a community-driven, open-source continuation of the EPA Storm Water Management Model — a dynamic hydrology, hydraulic, and water-quality simulator for urban runoff. The project preserves the SWMM legacy under QA/QC and builds the community needed for long-term maintenance, working with ASCE/EWRI and the Water Environment Federation. Planned development direction and recorded design studies live in the [Roadmap](ROADMAP.md).

## What's New in v6.0.0

### Architecture & Performance

- **Data-Oriented Design** — Core state refactored to Structure-of-Arrays for cache efficiency and SIMD-friendly batches.
- **Reentrant Engine** — All simulation state lives behind an opaque `SWMM_Engine` handle; multiple independent simulations can run in the same process.
- **Plugin-Based I/O** — Output and report writing dispatch through plugin interfaces on a dedicated I/O thread.
- **Portable Parallel Backends** — Kokkos-based GPU/threaded plugin backends (OpenMP, CUDA, HIP, SYCL) for the 2D solver, shipped as separately loaded plugins so the base build carries no GPU dependency.
- **C++20 Codebase** — Modern C++20 implementation; the legacy EPA SWMM 5.x solver is preserved unmodified in `src/legacy/`.

### Process Formulation Enhancements

#### Hydrology

- **Two-Zone Groundwater Model** — Each subcatchment carries an independent aquifer with an upper unsaturated and lower saturated zone integrated as batched ODE systems (moisture accounting, percolation, deep percolation, evapotranspiration from both zones), with user-definable lateral flow to drainage nodes and channels. Aquifer parameters are readable and writable at runtime through the C API and Python bindings.
- **Physics-Based RDII Initial Abstraction Recovery** — RDII initial abstraction evolves as an exponential depletion/recovery process with additive base + thermal recovery rates and frozen-ground suppression. Seasonal RDII variation emerges from temperature dynamics on a single RTK set per sewershed — no monthly parameter tables required. Configured via the new `[RDII_DECAY]` input section.

  $$IA_{avail}(t+\Delta t) = IA_{max} - \bigl(IA_{max} - IA_{avail}(t)\bigr) \cdot e^{-k_{rec}(T)\,\Delta t}, \quad k_{rec}(T) = k_0 + k_T \cdot e^{\,\theta(T - T_{ref})}$$

- **Runtime Climate Forcing & Per-Subcatchment PET** — Air temperature, wind speed, evaporation, and rainfall can be prescribed while a simulation is running, and potential evapotranspiration can be prescribed per subcatchment, overriding climate-derived rates for surface, LID, and groundwater losses.
- **Consistent Snow / Rain Partitioning** — A single precipitation-split path applies the gage snow catch factor everywhere, correcting legacy inconsistencies in snow-season models.
- *Work plan:* **Spatially Explicit Groundwater** — a fully coupled two-layer subsurface kernel on the 2D finite-volume mesh (unsaturated and saturated depths joined at a moving water table, selectable soil characteristics, lateral Darcy exchange between cells), enabling saturation-excess overland flow, return flow, head-driven pipe ↔ aquifer exchange, and capillary rise — processes the per-subcatchment aquifer cannot represent ([Roadmap §5.2](ROADMAP.md)).

#### Hydraulics

- **Semi-Implicit Node Continuity** — Single-equation free-surface/surcharge formulation that removes the legacy two-branch discontinuity. Enabled via `NODE_CONTINUITY SEMI_IMPLICIT` (default).
- **Anderson Acceleration for Picard Iteration** — Depth-2 mixing of residual history cuts iteration counts 25–50% on stiff surcharge transitions with safe fall-back to standard Picard. Enabled via `ANDERSON_ACCEL YES`.
- **Dynamic Preissmann Slot** — Geometry-dependent slot width replaces the fixed-width slot at the free-surface / pressurized transition, improving stability for rapidly filling or draining conduits.
- **Explicit Finite-Volume 1D Routing** — A Godunov-type conservation-form Saint-Venant solver (`FLOW_ROUTING FV`) alongside dynamic wave: hydrostatic reconstruction, HLL fluxes, optional MUSCL/SSP-RK2, local time stepping, and a slot folded into the cross-section closure so mixed free-surface/pressurized flow needs no regime switching. Routing continuity error of 0.000% on the EPA reference model at every mesh resolution.
- **1D/2D Coupled Overland Flow** — Explicit local-inertial finite-volume 2D solver with mass-conservative 1D↔2D exchange, mesh generation from digital terrain data, per-cell parameter surfaces, and validation against the SWASHES analytic benchmark set. Surcharge re-routes over terrain, and lateral groundwater exchanges are tracked explicitly.
- **HEC-22 Inlet Analysis, Variable-Speed Pumps, New Storage Shapes** — Street inlet capture with grate and curb inlets, Type 5 pump curves with speed scaling, and conical/pyramidal storage shapes with elliptical and rectangular bases.
- *In development:* **Spatially Explicit Inlets** — promotes inlets to mode-switching junction nodes that capture street flow when gutter spread exceeds a threshold and revert to passive junctions otherwise ([Roadmap §1.3](ROADMAP.md)).

#### Water Quality

- **Eulerian ADE Transport on the FV Mesh** — Cell-resolved advection-dispersion transport riding the finite-volume hydraulic mesh: species fluxes upwinded on the contact speed for exact mass conservation, first-order upwind / MUSCL / QUICKEST-ULTIMATE reconstructions with flux-corrected transport limiting, and implicit longitudinal dispersion (`FV_DISPERSION`).
- **Treatment Expression Engine** — Node treatment expressions compiled once at start and evaluated from a cached expression VM.
- *Work plan:* **Multispecies Lagrangian Advection–Reaction–Dispersion (LARD)** — a selectable quality engine combining EPANET-style Lagrangian time-driven parcel transport, an EPANET-MSX-equivalent multispecies reaction system (bulk and wall species, user-defined rate ODEs and equilibrium DAEs, EUL/RK5/ROS2 integrators with adaptive error control), random-walk particle-tracking dispersion, and built-in water age. Independent of the hydraulic discretization, so it runs under dynamic wave and kinematic wave as well; the reaction system is written once and shared across pipe, overland, and groundwater transport ([Roadmap §2](ROADMAP.md)).
- *Work plan:* **Heat Transport** — a thermal module for stormwater temperature dynamics: surface energy balance for catchment-scale water temperature (solar radiation, long-wave exchange, evaporation, conduction), 1D longitudinal heat transport in pipes and channels coupled to the ADE solver, groundwater coupling for subsurface heat exchange and baseflow temperature, and simplified thermal stratification in detention basins ([Roadmap §4](ROADMAP.md)).

#### LID

- **Full SWMM 5.x LID Controls** — Bio-retention cells, permeable pavement, green roofs, infiltration trenches, rain gardens, and the rest of the SWMM 5.x LID set, with LID controls and usage fully editable through the C API and Python bindings (including delete cascades that keep LID usage, drain-to, and snowpack references consistent).
- *Work plan:* **LID as Storage Nodes** — Maps LID layers (surface, media, gravel) onto extended storage nodes using a reduced-physics kinematic Richards ODE, replacing the one-way hydrological cascade with hydraulically mediated LID trains: head-dependent flow between units, backpressure from saturated downstream units, and control structures between LIDs ([Roadmap §6](ROADMAP.md)).

### New C API

A domain-split C API replaces the monolithic legacy interface — separate headers per domain (engine lifecycle, model building, nodes, links, subcatchments, gages, pollutants, tables, inflows, controls, infrastructure, spatial data, quality, mass balance, callbacks, hot start, statistics, and optional GeoPackage I/O) under `include/openswmm/engine/`. Full reference at the [C engine documentation site](https://hydrocouple.org/openswmm.engine).

### Python Bindings — Full Modeling Workflows

The Python API is not just a solver wrapper — it supports the complete modeling lifecycle in code:

- **Domain Delineation & Model Building** — `ModelBuilder` constructs a complete model from scratch (no `.inp` required): network topology (nodes, links, subcatchments, gages), spatial geometry (coordinates, vertices, polygons, CRS), then `validate()` and `write()` an `.inp` or hand off directly `to_solver()`. Generate models from databases or GeoJSON, build synthetic test networks, or treat topology as a decision variable in optimization loops.
- **Parameterization** — Every domain is readable and writable programmatically: cross-sections, infiltration, aquifers and groundwater, LID controls and usage, land uses, buildup/washoff and treatment, inflows, RDII, transects, streets and inlets, control rules, climate, and simulation options.
- **Runtime Interaction** — Step-by-step simulation control with runtime forcing (rainfall, climate, PET, inflows, boundary heads), live control of pumps, orifices, and weirs, state inspection at every step, and hot-start save/load for checkpointing and operational forecasting.
- **Results Analysis** — Bulk NumPy access to time series, a binary output reader, node/link/subcatchment statistics, mass balance and continuity accounting, scenario comparison, and plotting utilities.

```python
from openswmm.engine import ModelBuilder, NodeType, LinkType, XSectShape

m = ModelBuilder()                              # no .inp required

j1  = m.add_node("J1",  NodeType.JUNCTION)      # delineate the domain
out = m.add_node("OUT", NodeType.OUTFALL)
c1  = m.add_link("C1",  LinkType.CONDUIT)
m.set_link_nodes(c1, j1, out)

m.set_node_invert(j1, 100.0)                    # parameterize
m.set_node_max_depth(j1, 5.0)
m.set_node_invert(out, 99.0)
m.set_link_length(c1, 300.0)
m.set_link_roughness(c1, 0.013)
m.set_link_xsect(c1, XSectShape.CIRCULAR, 1.0)

m.validate()
m.finalize()
m.write("model.inp")                            # dump for inspection, or…
solver = m.to_solver()                          # …run it directly
```

The solver returned by `to_solver()` behaves identically to one constructed from a parsed `.inp` — every domain class, forcing hook, control rule, and output reader works. See the [per-domain Python guide](https://hydrocouple.org/openswmm.engine/python) for all of the above.

### Additional Features

- **Hot Start API** — Save, load, modify, and query hot-start files through a stable C ABI.
- **CRS Support** — Coordinate reference systems specified in `[OPTIONS]`.
- **User Flags** — Typed `[USER_FLAGS]` / `[USER_FLAG_VALUES]` sections attach custom metadata (boolean, integer, real, string) to nodes, links, subcatchments, or gages.
- **Extension Options** — Unrecognized `[OPTIONS]` keys are preserved and exposed to plugins at runtime.
- **Plugin SDK** — Header-only SDK for input, output, and report plugins; the `IPluginComponentInfo` entry point advertises capabilities and supports custom `.inp` section handlers via `SectionRegistry`.
- **GeoPackage I/O** — Optional SQLite-based spatial backing store for inputs, results, observed series, and topology in a single `.gpkg` file.

## Quick Start

```bash
# C / C++ engine
git clone https://github.com/HydroCouple/openswmm.engine.git
cd openswmm.engine
git clone https://github.com/microsoft/vcpkg.git && ./vcpkg/bootstrap-vcpkg.sh
export VCPKG_ROOT=$(pwd)/vcpkg

cmake --preset=Linux            # or Windows / Darwin (see Building from Source)
cmake --build build --config Release

# Python bindings (PyPI)
pip install openswmm
```

```python
from openswmm.engine import Solver, Nodes, Links

with Solver("model.inp", "model.rpt", "model.out") as s:
    nodes, links = Nodes(s), Links(s)
    while s.step():
        depth = nodes.get_depth("J1")
        flow  = links.get_flow("C1")
```

Forcing, model building, hot-start, bulk NumPy access, mass balance, and statistics are covered in the [Python docs](https://hydrocouple.org/openswmm.engine/python).

## Building from Source

### Prerequisites

| Requirement | Version |
|---|---|
| CMake | 3.21+ |
| C compiler | C17 (GCC 10+, Clang 12+, MSVC 19.29+) |
| C++ compiler | C++20 (GCC 10+, Clang 14+, MSVC 19.29+) |
| vcpkg | 2025.02.14 |
| Ninja | recommended on Linux/macOS |
| Python | 3.10 – 3.13 (bindings only) |

### Dependencies

All third-party libraries are resolved through the vcpkg manifest ([`vcpkg.json`](vcpkg.json)) — nothing needs to be installed by hand. What gets pulled in depends on which features and CMake options are active:

| Dependency | Needed for | Default build | Controlled by |
|---|---|---|---|
| SQLite3 (rtree) | GeoPackage I/O | included | `-DOPENSWMM_WITH_GEOPACKAGE=ON` (default) / vcpkg feature `geopackage` |
| HDF5 | 2D module output (CF-1.11 / UGRID-1.0) | included | `-DOPENSWMM_BUILD_2D=ON` (default) / vcpkg feature `2d` |
| Kokkos (OpenMP) | GPU/threaded 2D surface-solver plugin | included | `-DOPENSWMM_BUILD_GPU_PLUGIN=ON` (default) / vcpkg feature `gpu` |
| GoogleTest | unit + regression tests | optional | `-DOPENSWMM_BUILD_TESTS=ON` / `*-tests` presets / vcpkg feature `tests` |
| Google Benchmark | performance benchmarks | optional | `-DOPENSWMM_BUILD_BENCHMARKS=ON` / vcpkg feature `benchmarks` |
| Kokkos (CUDA / HIP / SYCL) | GPU device backends for the 2D plugin | optional | vcpkg features `gpu-cuda` / `gpu-hip` / `gpu-sycl` + `-DOPENSWMM_GPU_BACKEND=cuda\|hip\|sycl` (requires the matching CUDA / ROCm / oneAPI toolkit) |
| Python + Cython + NumPy | Python bindings | optional | `-DOPENSWMM_BUILD_PYTHON=ON` or `pip install ./python` (NumPy ≥ 1.21 at runtime) |

The GPU plugin is a standalone shared library loaded at runtime (`openswmm_gpu_omp` / `_cuda` / `_hip` / `_sycl`) — it is never linked into the core engine, so builds and wheels without it stay Kokkos-free.

### Configure, Build, Test

```bash
cmake --preset=Linux                       # release build (Windows / Linux / Darwin)
cmake --build build --config Release

cmake --preset=Linux-tests                 # debug build with unit + regression tests
cmake --build build
ctest --test-dir build --output-on-failure
```

Available presets: `Windows`, `Linux`, `Darwin` (+ `-debug`, `-tests`, `-tests-release` variants) and `Windows-cuda` for a CUDA-backend GPU plugin build. See [`CMakePresets.json`](CMakePresets.json) for the full matrix.

### Python Bindings from Source

```bash
pip install ./python            # scikit-build-core + Cython; builds the engine automatically
python -m pytest python/tests   # optional
```

## Glossary

Brief definitions of the domain terms used throughout this README. Full treatment lives in the [reference manuals](https://hydrocouple.org/openswmm.engine).

- **RDII** — Rainfall-Dependent Inflow & Infiltration. Stormwater that enters sanitary or combined sewers through cracks, joints, defective laterals, and roof / foundation drains during and after rainfall.
- **RTK** — The triplet `(R, T, K)` that parameterises a SWMM synthetic unit hydrograph for RDII: `R` is the long-term fraction of rainfall that becomes RDII, `T` is the time to peak (hours), and `K` is the ratio of base time to peak time.
- **IA (Initial Abstraction)** — Rainfall depth absorbed by the catchment before any RDII response begins (interception, surface storage, soil wetting). Recovers between events.
- **DWF** — Dry-Weather Flow. Base sanitary flow plus infiltration unrelated to rainfall, typically specified as an average value with diurnal / day-of-week / monthly patterns.
- **LID** — Low-Impact Development. Distributed green-infrastructure controls (bio-retention cells, permeable pavement, green roofs, infiltration trenches, rain gardens) that intercept, store, and infiltrate runoff at the source.
- **ADE** — Advection-Dispersion Equation. The transport equation governing constituent movement by bulk flow (advection) and spreading by velocity shear and turbulence (dispersion).
- **MSX** — EPANET Multi-Species Extension. The reference specification for user-defined multispecies reaction networks (rate ODEs, equilibrium DAEs, bulk and wall species) that the planned LARD reaction system is equivalent to.
- **PET** — Potential Evapotranspiration. The evaporative demand applied to surface, LID, and groundwater moisture stores.
- **CRS** — Coordinate Reference System. The geodetic / projected coordinate frame (e.g. `EPSG:4326`) the model's spatial data is expressed in.
- **Dynamic Wave Routing** — Full Saint-Venant momentum solver for link flow, used for backwater, surcharge, and pressurized conditions.
- **Preissmann Slot** — A narrow virtual slot added to a closed conduit's cross-section so that pressurized flow can be solved with the same free-surface equations. The dynamic slot adjusts width with geometry to smooth the surface ↔ pressure transition.
- **Surcharge** — A pipe flowing full and under pressure (HGL above the crown), typically caused by downstream backwater or capacity exceedance.
- **Picard Iteration** — Fixed-point iteration used inside the dynamic-wave timestep to converge implicit node depths. Anderson Acceleration is a residual-history accelerator on top of Picard.
- **Hot Start** — A saved end-of-run state (depths, volumes, IA, snow, GW) that initialises a subsequent simulation, letting long runs be split into checkpoints or warm runs into operational forecasts.
- **HEC-22** — FHWA Hydraulic Engineering Circular No. 22, the design reference whose grate and curb-opening capture equations are used by the inlet-analysis module.
- **GeoPackage** — OGC standard for a SQLite-based, single-file container holding spatial features and tabular data with full CRS metadata.

## Project Structure

```
openswmm.engine/
├── include/openswmm/
│   ├── engine/           # New engine public C API headers
│   └── legacy/           # Legacy SWMM 5.x public headers
├── src/
│   ├── engine/           # New C++20 engine implementation
│   │   ├── input/geopackage/  # Optional GeoPackage I/O
│   │   └── 2d/                # 2D overland-flow solver, 1D/2D coupling, GPU plugin backends
│   ├── legacy/           # Original EPA SWMM 5.x solver and output reader
│   ├── plugin_sdk/       # Header-only plugin SDK
│   └── cli/              # Command-line interface
├── tests/                # Unit, regression, and benchmark suites
├── python/               # Cython bindings (scikit-build-core)
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
| `openswmm_gpu_omp` / `_cuda` / `_hip` / `_sycl` | Kokkos 2D surface-solver plugins (shared, optional, loaded at runtime) |
| `openswmm_plugin_sdk` | Header-only plugin SDK (INTERFACE) |
| `openswmm` | Command-line executable |

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

Apache License, Version 2.0 — see [LICENSE](LICENSE) and [NOTICE](NOTICE). Original EPA SWMM material is in the public domain under 17 USC § 105 and is not subject to the Apache license grant.

## Acknowledgements

OpenSWMM builds on the EPA Storm Water Management Model. See [docs/authors.md](docs/authors.md) for the full contributor list.
