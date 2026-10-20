# OpenSWMM Engine

**The Open-Source Storm Water Management Model (SWMM2D) — Next-Generation Computational Engine**

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

OpenSWMM is an open-source hydrology, hydraulic, and water-quality engine built on EPA SWMM. It provides a reentrant C++20 implementation, a public C API, Python bindings, and a command-line solver. The legacy SWMM 5.x solver and output reader remain available alongside the new engine.

[Engine documentation](https://hydrocouple.org/openswmm.engine) · [Python documentation](https://hydrocouple.org/openswmm.engine/python) · [SWMMVis GUI](https://github.com/HydroCouple/openswmm.gui)

## Current implementation

The source version is **6.0.0-alpha.4**. The following summarizes the implementation as of **October 19, 2026**; feature availability depends on the build and selected solver.

- **Engine and interfaces:** independent simulation handles, structure-of-arrays state, domain-specific C APIs, Python model building and editing, runtime forcing and controls, and plugin-based input/output.
- **Hydrology:** rainfall-runoff, infiltration, conventional LID controls, two-zone subcatchment groundwater, degree-day and UEB energy-balance snow, temperature-dependent RDII abstraction recovery, and runtime climate and potential evapotranspiration forcing.
- **1D hydraulics:** steady, kinematic-wave and dynamic-wave routing, plus explicit finite-volume Saint-Venant routing (`FLOW_ROUTING FV`). Dynamic-wave extensions include semi-implicit node continuity, Anderson acceleration, and dynamic Preissmann slots. Street/inlet capture, variable-speed pumps and additional storage geometries are supported.
- **2D surface flow and groundwater:** a local-inertial finite-volume surface solver with local time stepping, conservative exchange with the 1D network, per-node coupling types and a weir/orifice exchange law. Explicit two-zone groundwater runs on the mesh with surface/network exchange, hydraulic/quality edge boundaries, bulk or SIGMA layer chemistry, contact-depth routing of layered leakage, explicit aquifer reactions, and SIGMA chemistry coupled to porous soil heat through conservative layer mapping. Selected mesh-only pollutant/MSX chemistry and porous-heat checkpoints preserve age/contact ledgers, the engine calendar and groundwater HDF5 reports. Groundwater transport uses the CPU first-order marcher; a restricted standalone CLOSED_FORM water-only profile also supports coupled SSP-RK2 and authored water-only wells and aquifer HEAD/FLOW edges; see the [transport profiles and limits](docs/design/groundwater-transport.md).
- **Water quality:** legacy routing, Eulerian advection-reaction-dispersion (ARD), Lagrangian parcel transport (LARD), multispecies reaction expressions and integrators, treatment expressions, and water age. Solver and process combinations have different support limits.
- **Heat transport:** network and watershed temperature calculations, surface energy exchange, radiation and sediment exchange. The **experimental coupled mesh thermal model** adds porous-ground heat storage and transport, channel/ground contact, reciprocal UEB snow/soil exchange, and supported mesh-connected Richards LID heat exchange. See the [thermal implementation notes](docs/design/mesh-thermal-model.md) for activation and qualification limits.
- **LIDs as storage nodes:** layered controls connected through ordinary hydraulic links, independent outlet controls, layer treatment, and classical or Richards-column flow. See the [storage-node LID manual](docs/manuals/engine/sections/Chapter6-LidStorageNodes.md) for routing and transport restrictions.
- **Persistence and coupling:** SWMM input files, optional GeoPackage and HDF5 model I/O, binary and 2D HDF5 results, and hot-start/checkpoint support, including native coupled thermal state for supported configurations. Optional [HydroCouple components](docs/COMPONENT_ENGINE.md) provide metadata inspection and qualified CPU 1D execution and outfall transfers.

Kokkos OpenMP, CUDA, HIP and SYCL surface-solver plugins load separately from the core engine. The default source build includes the OpenMP plugin; device backends need their matching toolchains. An optional host OpenMP operator computes raw CLOSED_FORM groundwater recharge; water, transport, reactions and heat retain CPU ownership. Other groundwater acceleration remains outstanding; backend support is not interchangeable. See the [2D CPU implementation and qualification](docs/manuals/engine/sections/Chapter7-TwoDimensionalCpu.md).

## Quick start with Python

```sh
pip install openswmm
```

```python
from openswmm.engine import Solver, Nodes

with Solver("model.inp", "model.rpt", "model.out") as solver:
    nodes = Nodes(solver)
    while solver.step():
        depth = nodes.get_depth("J1")
```

Use a node ID from your model in place of `J1`. The [Python guide](https://hydrocouple.org/openswmm.engine/python) covers model construction, parameter editing, runtime forcing, results and checkpointing. Published packages may lag the source implementation described above.

## Build from source

Requires **CMake 3.21+**, **C17/C++20 compilers**, **Ninja** for the Linux/macOS presets, and **vcpkg**. Dependencies and optional features are declared in [vcpkg.json](vcpkg.json).

```sh
git clone https://github.com/HydroCouple/openswmm.engine.git
cd openswmm.engine
git clone https://github.com/microsoft/vcpkg.git vcpkg
./vcpkg/bootstrap-vcpkg.sh
export VCPKG_ROOT="$PWD/vcpkg"

cmake --preset=Linux
cmake --build --preset=Linux
./build/linux/bin/Release/openswmm model.inp model.rpt model.out
```

Use `Darwin` on macOS or `Windows` from a Visual Studio Developer Prompt; on Windows, bootstrap vcpkg with `bootstrap-vcpkg.bat` and set `VCPKG_ROOT` in that shell. Build folders follow the platform preset. The legacy executable is `openswmm-legacy`.

To build and run the test suite:

```sh
cmake --preset=Linux-tests
cmake --build --preset=Linux-tests
ctest --test-dir build/linux-tests --output-on-failure
```

Platform presets also provide `-debug` and `-tests-release` variants. See [CMakePresets.json](CMakePresets.json) and [CMake options](CMakeLists.txt) for optional 2D, GPU, GeoPackage, HDF5, Python and HydroCouple builds. For Python source builds, use `pip install ./python`.

## Documentation

- [Engine manual](docs/manuals/engine/engine_manual.md): input sections, files, reports and programmatic interfaces.
- [Reference manuals](https://hydrocouple.org/openswmm.engine): process formulations, numerical methods and API reference.
- [Release notes](docs/releases/RELEASE_NOTES_6.0.0-alpha.4.md) and [roadmap](ROADMAP.md): release history and development direction.

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
