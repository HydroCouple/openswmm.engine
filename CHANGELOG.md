# Changelog

All notable changes to the OpenSWMM Engine are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [6.0.0-alpha.1] — 2026-03-25

### Added

#### New Engine Architecture
- **Data-oriented engine** — Refactored core data structures to Structure of Arrays (SoA) layout for cache efficiency and SIMD-friendly computation.
- **Reentrant design** — All simulation state encapsulated in an opaque `SWMM_Engine` handle, eliminating global state and enabling multiple independent simulations per process.
- **Plugin-based I/O** — Output and report writing abstracted through a plugin interface with a dedicated I/O thread and double-buffered snapshots.
- **Engine lifecycle state machine** — Explicit states: CREATED → OPENED → INITIALIZED → STARTED → RUNNING → ENDED → CLOSED.

#### Comprehensive C API (19 headers)
- `openswmm_engine.h` — Engine lifecycle, error codes, state machine.
- `openswmm_model.h` — Model building, validation, serialization, options.
- `openswmm_nodes.h` — Junctions, outfalls, storage nodes, dividers.
- `openswmm_links.h` — Conduits, pumps, orifices, weirs, outlets with 20 cross-section shapes.
- `openswmm_subcatchments.h` — Subcatchments, infiltration (Horton/Green-Ampt/Curve Number), landuse coverage.
- `openswmm_gages.h` — Rain gages with timeseries and file data sources.
- `openswmm_pollutants.h` — Pollutant definitions and runtime quality injection.
- `openswmm_tables.h` — Time series, curves, patterns, and cursor-optimized lookups.
- `openswmm_inflows.h` — External inflows, dry weather flow, RDII.
- `openswmm_controls.h` — Control rule expressions and direct link setting/status actions.
- `openswmm_infrastructure.h` — Transects, streets, inlets, LID controls and LID usage.
- `openswmm_spatial.h` — CRS, coordinates, polylines, polygons for all object types.
- `openswmm_quality.h` — Landuse, buildup/washoff functions, treatment expressions.
- `openswmm_massbalance.h` — Continuity errors and cumulative flux totals.
- `openswmm_callbacks.h` — Progress, warning, step-begin/end, plugin state, and hot-start-missing callbacks.
- `openswmm_hotstart.h` — Hot start file save/load/modify/query with workflow examples.
- `openswmm_statistics.h` — Node, link, and subcatchment simulation statistics.
- `openswmm_engine_export.h` — Auto-generated shared library export macros.

#### Features
- **Hot start API** — Save, open, modify, query, and close hot start files through a transparent C ABI.
- **CRS support** — Coordinate reference system specification via OPTIONS section.
- **User flags** — Custom USER_FLAGS section for user-defined metadata on objects.
- **Plugin SDK** — Header-only development kit for building output/report plugins.
- **HEC-22 inlet analysis** — Street inlet capture with grate, curb, slotted, and custom inlet types (from SWMM 5.2).
- **Variable speed pumps** — Type5 pump curves with speed scaling.
- **New storage shapes** — Conical and pyramidal shapes with elliptical/rectangular bases.
- **Python bindings** — Cython-based bindings with solver context manager, iterative stepping, and output reading.

#### Testing & CI
- **Google Test migration** — All unit tests converted from Boost.Test to Google Test 1.15.2.
- **Comprehensive test suite** — 73+ legacy engine tests, 41 legacy output tests, and new engine unit tests.
- **Reorganized test structure** — `tests/unit/legacy/{engine,output}` and `tests/unit/{engine,output}`.
- **Multi-platform CI** — GitHub Actions for Windows x64, Linux x64, macOS x64, and macOS ARM64.
- **Performance benchmarks** — Google Benchmark integration for critical-path profiling.

#### Documentation
- **Doxygen API documentation** — All 19 public C API headers thoroughly documented with `@brief`, `@details`, `@param`, `@returns`, `@see`, and `@note` tags.
- **Technical reference manuals** — Hydrology, Hydraulics, and Water Quality reference manuals updated for OpenSWMM.
- **User manual** — Comprehensive user manual with modeling capabilities, typical applications, and input/output descriptions.
- **Author/license metadata** — All new engine source files annotated with `@author`, `@copyright`, and `@license` Doxygen tags.

### Changed

- **Project renamed** from `OpenSWMMCore` to `openswmm` with `openswmm.engine` as the primary library output name.
- **CMake minimum version** raised to 3.21 (from 3.15).
- **C++ standard** set to C++20 (from C++11/14).
- **C standard** set to C17.
- **CMake options** namespaced to `OPENSWMM_*` prefix (legacy `OPENSWMMCORE_*` aliases preserved).
- **Version scheme** updated to SemVer 2.0.0 with pre-release tags.
- **vcpkg** adopted as the dependency manager (replacing NuGet-based Boost distribution).
- **CI/CD pipelines** cleaned up: updated to `actions/checkout@v4`, `actions/setup-python@v5`, `actions/upload-artifact@v4`; removed stale branch triggers; fixed CMake flag from `-DBUILD_TESTS=ON` to `-DOPENSWMM_BUILD_TESTS=ON`.

### Removed

- **Boost.Test dependency** — Replaced entirely by Google Test.
- **NuGet package dependency** — Regression testing no longer requires external NuGet-hosted Boost packages.
- **Global state** — Eliminated from the new engine (legacy solver globals preserved in `src/legacy/`).

### Fixed

- **CI CMake flag** — Unit testing workflow was passing `-DBUILD_TESTS=ON` which did not match the actual `OPENSWMM_BUILD_TESTS` option, preventing tests from being built in CI.
- **Documentation workflow** — Removed stale `bug_fixes` branch trigger; updated to `actions/checkout@v4`.
- **Export header** — Fixed misplaced `@author`/`@copyright` block that was injected inside a `#define` preprocessor directive in `openswmm_engine_export.h`.

## [5.2.0] — Legacy

Last EPA-maintained release. See [docs/SWMM_5.2.0.md](docs/SWMM_5.2.0.md) for details on HEC-22 inlet analysis, new storage shapes, variable speed pumps, and control rule enhancements.
