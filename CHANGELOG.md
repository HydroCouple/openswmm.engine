# Changelog

All notable changes to the OpenSWMM Engine are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased] — Subcatchment PET prescription

See `docs/SUBCATCHMENT_PET_PRESCRIPTION_PLAN.md`.

### Added

- **Legacy engine:** new `swmm_SUBCATCH_API_PET` subcatchment property —
  prescribe a potential evapotranspiration rate (in/day or mm/day) per
  subcatchment at runtime. The prescribed rate replaces the climate-derived
  `Evap.rate` for surface, LID, and groundwater upper-zone evaporation
  (bypassing `DRY_ONLY` and monthly adjustments); a negative value clears
  it. New read-only `swmm_EVAPRATE` system property returns the current
  climate-derived rate. Python: `LegacySubcatchment.set_api_pet` /
  `get_api_pet` / `clear_api_pet`, `LegacySystem.get_evap_rate`.
- **Refactored engine:** new `swmm_climate_get_evap_rate()` C API getter
  and `Forcing.climate_evap_rate()` Python method for caller-side
  adjustment composition.

### Fixed

- **Refactored engine:** `swmm_forcing_subcatch_evap()` previously had no
  effect — it overwrote `evap_loss` before the runoff solver recomputed it.
  It now prescribes a PET *rate* (user units: in/day US, mm/day SI —
  previously documented as ft/sec) consumed by the runoff, LID, and
  groundwater solvers, so capping to available water and mass-balance
  accounting happen along the normal computation paths.

## [Unreleased] — Pythonic Python bindings (v1)

### Changed — **breaking** (Python bindings only; C API unchanged)

A full property-style rewrite of the `openswmm.engine` Python surface.
See `docs/PYTHONIC_BINDINGS_PLAN.md` and the
`docs/PYTHONIC_BINDINGS_DONE.md` wrap-up. The C API is untouched.

#### Solver lifecycle

- Lifecycle methods (`open`, `initialize`, `start`, `step`, `stride`,
  `end`, `report`, `close`) **raise on failure** instead of returning
  integer codes. `step()` and `stride()` return a
  `datetime.timedelta`; `timedelta(0)` is the end-of-simulation sentinel.
- `Solver.state` now returns the `EngineState` enum.
- `Solver.elapsed` and `Solver.routing_step` are `datetime.timedelta`.
- New `Solver.start_datetime`, `end_datetime`, `current_datetime`,
  `report_start_datetime` return `datetime.datetime`.
- New `Solver.steps()` iterator and `Solver.until(target)` (accepts
  `datetime` or `timedelta`).
- Every file argument accepts `pathlib.Path` / `os.PathLike`.

#### Views on the Solver

- `solver.options` — `MutableMapping` over `[OPTIONS]` plus typed
  shortcuts (`start_datetime`, `routing_step`, …).
- `solver.userflags` — `MutableMapping` with auto-typed bool/int/float.
- `solver.events` — `MutableSequence[Event]` (each entry carries
  `datetime` `start` / `end`).
- `solver.save_schedule` — `MutableSequence[SaveScheduleEntry]` for the
  `[SAVE HOTSTART]` block.

#### Domain collections + wrappers

Each `solver.<domain>` returns a collection that is **indexable by
`int | str`**, iterable, and `len`-able. Items are typed wrapper
objects with property-style access:

- `solver.nodes["J1"].depth = 1.2`
- `solver.links["C1"].xsect = (XSectShape.CIRCULAR, 1.0, 0, 0, 0)`
- `solver.subcatchments["S1"].infiltration.set_horton(...)`
- `solver.gages["RG1"].rainfall = 25.4`
- `solver.pollutants["TSS"].kdecay = 0.05`

Per-type sub-views raise `AttributeError` on wrong-type nodes/links:
`node.outfall` only on OUTFALL, `node.storage` only on STORAGE,
`link.pump` only on PUMP, etc.

Bulk numpy access is now a property pair:
`solver.nodes.depths` / `solver.links.flows` / `solver.subcatchments.runoffs`.

#### OutputReader

- Path-agnostic constructor (`str` / `Path`).
- Typed metadata: `start_datetime`, `report_step` (`timedelta`),
  `flow_units` (`FlowUnits` enum), `period_times`
  (`np.ndarray[datetime64[s]]`), `node_ids` / `link_ids` /
  `subcatchment_ids` lists.
- Variable-selector arguments require an enum (`OutNodeVar` etc.);
  object selectors accept `int | str`.
- `node_attributes(key, period)` returns `Dict[OutNodeVar, float]`.
- `node_stats(key)` returns a typed view with `max_depth`,
  `max_overflow`, `vol_flooded`, `time_flooded`.

#### MassBalance, Statistics, HotStart, Tables

- `solver.mass_balance.routing_diagnostics` returns the
  `RoutingDiagnostics` dataclass.
- `solver.statistics.<domain>_<stat>` are all bulk numpy properties.
- `HotStart.open(path)` classmethod / `HotStart.save_from(solver, path)`
  static; `sim_datetime` (`datetime`), `warnings` (`list[str]`),
  `apply(solver)` on the hot-start.
- `solver.tables` exposes `TimeSeries.points` as a structured numpy
  array `(time: datetime64[s], value: float64)`. `solver.patterns` is a
  separate indexable collection.

#### Exceptions

New `EngineError` hierarchy in `openswmm.engine._exceptions`. Every
subclass **also** inherits from a standard-library exception:

- `BadIndexError(EngineError, IndexError)`
- `BadParamError(EngineError, ValueError)`
- `LifecycleError(EngineError, RuntimeError)`
- `HotStartError(EngineError, RuntimeError)`
- `FileError(EngineError, IOError)`
- `ParseError(EngineError, ValueError)`
- `NumericalError(EngineError, RuntimeError)`
- `CRSError(EngineError, ValueError)`
- `DependencyError(EngineError, RuntimeError)`
- `PluginError(EngineError, RuntimeError)`
- `BadHandleError(EngineError, RuntimeError)`
- `StaleObjectError(LifecycleError)` — raised when a wrapper's
  generation counter no longer matches the solver's after a
  rename/delete.

Every `EngineError` carries `.code` (raw int), `.code_enum`
(`ErrorCode` member), `.message` (filled by the C API).

#### New enums

- `OrificeType`, `WeirType`, `OutletRatingType` (in `openswmm_links.h`).
- `ErrorCode.DEPENDENCY = 15` (was missing from the Python side).

#### DateTime conversion C API

- New `include/openswmm/engine/openswmm_datetime.h` exposes
  encode/decode/`add_seconds`/`time_diff` primitives matching the
  legacy `datetime.c` bit-for-bit.
- Reached from Python through `openswmm.engine.datetime_api` (the
  Cython binding) plus the high-level `oadate_to_datetime` /
  `datetime_to_oadate` helpers.
- All "Julian date" wording removed from the C API header
  documentation; the convention is documented as the OLE Automation /
  Delphi TDateTime epoch (1899-12-30) — **not** astronomical Julian.

### Documentation

- Sphinx CI gate (`sphinx-build -W --keep-going`) was already in place
  and is kept; every guide page renders warning-free against the new
  `.pyi` stubs.
- New `guide/datetime.rst`, `guide/plotting.rst` pages.
- `guide/concepts.rst`, `guide/error_handling.rst`, every domain
  guide and the migration page all rewritten for the v1 surface.
- v0 → v1 cheat sheet appended to `migration/swmm5_to_swmm6.rst`.

### Test-suite migration (now landed)

The legacy per-domain `test_*.py` files have been processed:

- **Duplicated coverage neutralised** — `test_nodes.py`,
  `test_links.py`, `test_subcatchments.py`, `test_gages.py`,
  `test_massbalance.py`, `test_output_reader.py`, `test_hotstart.py`,
  `test_spatial.py`, `test_infrastructure.py`,
  `test_quality_pollutants.py`, `test_tables.py`, `test_new_api.py`,
  `test_new_modules.py`, and all `*_expanded.py` files are now
  module-level `pytest.skip()` stubs (each names its replacement
  `*_pythonic.py` file in the docstring). They can be `git rm`-ed in
  a subsequent sweep without changing CI behaviour.
- **Unique-scenario tests migrated to v1** — `test_integration.py`,
  `test_callbacks_and_xsect.py`, `test_workflow.py`,
  `test_opened_state_editing.py`, `test_concurrent_simulation.py`,
  `test_controls_advancement.py`, `test_controls_inflows.py`,
  `test_rdii_advancement.py`, `test_solver.py`. Each uses the v1
  surface (`solver.nodes["J1"].depth`, `for elapsed in solver.steps()`,
  `solver.links[0].xsect = (...)`, etc.).

### mypy gate

- `python/pyproject.toml` ships a `[tool.mypy]` block: default-mode
  check across the whole `openswmm.engine` package plus strict-mode on
  the pure-Python modules (`_enums`, `_exceptions`, `_dates`).
- `python/tests/typing/test_surface.py` exercises every public symbol
  with explicit type annotations — runs under strict mode.
- New CI workflow `.github/workflows/typing.yml` runs both passes on
  every PR touching the bindings or the typing test.

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
