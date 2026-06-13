# Changelog

All notable changes to the OpenSWMM Engine are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased] — Runtime forcing Phase 4 + §3 legacy quality sources

See `docs/RUNTIME_FORCING_PHASE4_HANDOFF.md`,
`docs/RUNTIME_FORCING_PHASE4_AUDIT.md` (per-group outcomes), and
`docs/RUNTIME_FORCING_API_GAP_PLAN.md` §7/§12.

### Added

- **Phase 4 wave B1 — runtime time-pattern factors (P6) & street sweeping
  (P4).** Audited the mid-run mutation semantics of both groups and added the
  legacy parity setters so both engines share the contract:
  - Legacy `swmm_TIME_PATTERN` object type + `swmm_PatternProperty`
    (`FACTOR` with the factor index in `subIndex`, read-only `COUNT`/`TYPE`)
    and `swmm_LANDUSE` + `swmm_LanduseProperty`
    (`SWEEP_INTERVAL`/`SWEEP_REMOVAL`) via new `set/getPatternValue` /
    `set/getLanduseValue` in `swmm5.c`; settable pre-start and while running
    (both are per-step lookups). Python `SWMMPatternProperties` /
    `SWMMLandUseProperties` enums (+ `.pyi`), enum coverage, and parity tests
    `python/tests/legacy/test_param_runtime.py`.
  - Refactored audit tests `python/tests/engine/test_param_runtime.py`.
- **Phase 4 wave B2 — runtime buildup/washoff function coefficients (P2).**
  Both groups SOUND mid-run; the accumulated buildup pool is preserved (an
  edit only changes how buildup evolves going forward). Legacy parity:
  `swmm_LanduseProperty` extended with `BUILDUP_FUNC`/`COEFF1..3`/`NORMALIZER`
  and `WASHOFF_FUNC`/`COEFF`/`EXPON`/`SWEEP_EFFIC`/`BMP_EFFIC` (pollutant index
  via `subIndex`); `set/getLanduseValue` gained `subIndex`; buildup edits
  recompute `maxDays` per `landuse_readBuildup`. Tests in
  `test_param_runtime.py` (engine + legacy).
- **Phase 4 wave B3 — infiltration parameters (P1): documented pre-start-only.**
  The audit found the refactored infiltration setters
  (`swmm_subcatch_set_infil_horton`/`_green_ampt`/`_curve_number`) are guarded
  to the editable states by `CHECK_GEOMETRY` and raise `LifecycleError` while
  running (correcting the gap-plan's "no running guard" note); the
  per-subcatchment infiltration state is built once at `start()`. P1 is
  therefore a pre-start edit in both engines (no legacy parity setter). Tests
  in `test_param_runtime.py::TestInfiltrationParams`.
- **Phase 4 wave B4 — pollutant kinetics (P5).** `kdecay` / co-pollutant /
  snow-only are read live each step (sound mid-run, no cache); legacy parity
  added via `swmm_PollutProperty` `KDECAY`/`CO_POLLUTANT`/`CO_FRACTION`/
  `SNOW_ONLY` (kdecay accepted in 1/day, stored as the legacy 1/sec). The
  initial network concentration (`INIT_CONCEN`) has no per-step consumer, so
  both engines now reject it mid-run (refactored `CHECK_GEOMETRY`
  → `LifecycleError`; legacy `ERR_API_IS_RUNNING`). `SWMMPollutantProperties`
  4→9 (+ `.pyi`, enum coverage). Tests in `test_param_runtime.py`.
- **Phase 4 wave B5 — external-inflow / DWF baselines & scale (P7/P8).** The
  inflow solver caches ext/DWF definitions at start (same cache class as P6).
  New direct setters `swmm_ext_inflow_set_scale` / `_set_baseline` and
  `swmm_dwf_set_baseline` (plus the add/remove paths) now refresh that cache so
  a mid-run edit takes effect on the next step; bindings
  `Inflows.set_external_scale` / `_baseline` / `set_dwf_baseline`. Legacy
  parity for node-keyed `[INFLOWS]`/`[DWF]` baseline editing is deferred (the
  legacy per-node linked-list inflow model has no flat-index API; runtime
  inflow control remains available via `swmm_NODE_LATFLOW`) — see the audit
  doc. Tests: `TestInflowBaselineRuntime`.
- **Phase 4 wave B6 — treatment expressions (P3).** SOUND mid-run with a
  cache-refresh fix (same class as P6/P2/P7): the step loop evaluates the
  compiled-expression cache built at start, so `swmm_treatment_set`/`_clear`
  now recompile the edited (node, pollutant) cell via
  `SWMMEngine::refreshTreatment` — an edit/replace/clear takes effect on the
  next step, and a failed parse is rejected (`BadParamError`) with the
  previous expression restored. Legacy parity via dedicated functions (an
  expression cannot ride `setNodeValue`): `swmm_setTreatment` /
  `swmm_clearTreatment` re-use the `[TREATMENT]` input parser, freeing any
  prior `MathExpr` so runtime replaces don't leak; Python
  `Solver.set_treatment`/`clear_treatment` (+ `.pyi`). Tests:
  `TestTreatmentRuntime` (engine), `TestLegacyTreatment` (legacy).
- **Phase 4 wave B7 — LID layer parameters (P11).** The four refactored LID
  setters were silent no-op stubs; they now write `ctx.lid_controls.*` for
  real. Split contract: surface/soil/storage are **pre-start-only** (they seed
  per-unit LID state at start; `LifecycleError` mid-run, physical bounds
  enforced) while the **drain** coefficients are runtime-editable
  (`SWMMEngine::refreshLIDDrainParams` re-copies the per-unit drain columns
  the step loop reads — the cistern/rain-barrel RTC knob). Legacy parity for
  the sound group: `lid_setDrainParams` (`lid.c`, input-file units matching
  `readDrainData`) + exported `swmm_setLidDrain` + `Solver.set_lid_drain`
  binding. Tests: `TestLidParamsRuntime` (paired deterministic runs diverge
  only after the mid-run drain edit), `TestLegacyLidDrain`. Flagged
  follow-up: the refactored LID module lacks unit conversion of layer params
  (consumes raw input values; legacy converts via UCF) — see the audit doc.

- **§3 legacy water-quality source setters — functional tests.** The legacy
  `setPollutValue` source concentrations (rain/wet-deposition `pptConcen`,
  groundwater `gwConcen`, RDII `rdiiConcen`, dry-weather `dwfConcen`) and the
  ponded-surface quality injection are now covered by real-solver tests:
  `python/tests/legacy/test_quality_sources.py` (Q1 rain → washoff, Q2 GW,
  Q3 RDII, Q5 DWF, Q6 ponded round-trip + washoff). Each source feeds an
  existing inflow term already counted in the quality mass balance. Fixtures
  derive a self-contained inline-storm copy of `legacy_small.inp` (quality
  routing enabled, orphan external stage timeseries dropped, two-day horizon)
  with one TSS pollutant whose source columns are set one at a time so each
  node signal is attributable to a single source; artifacts land in
  `python/tests/legacy/output/`.

### Fixed

- **Refactored DWF/external-inflow patterns ignored mid-run edits.**
  `InflowSolver::init` copies pattern factors into a per-step lookup cache, so
  `swmm_pattern_set_factors` (which mutates `ctx.patterns`) had no effect on
  DWF/external inflow mid-run (groundwater-evap patterns read the live context
  and were unaffected). Added `InflowSolver::refreshPatterns` +
  `SWMMEngine::inflowSolver()`; the setter now refreshes the cache so an edit
  takes effect on the next step.
- **Legacy subcatchment pollutant bindings** passed the pollutant index in
  the `sub_index` slot, but the C `getSubcatchValue`/`setSubcatchValue`
  pollutant cases read it from `pollutantIndex`. `LegacySubcatchment`
  `get_pollutant_buildup`, `set_external_pollutant_buildup`, and
  `set_ponded_concentration` now pass `pollutant_index=` and work at runtime
  (previously raised an object-index API error).

## [Unreleased] — User-flag schema bindings + 2D/MCP gap closure

See `docs/API_GAP_CLOSURE_PLAN_2026-06-10.md`.

### Added

- **Python bindings:** the user-flag schema C API (`swmm_userflag_define`
  / `undefine` / `def_count` / `def_get` / `value_get` / `value_set` /
  `value_clear`) is now bound — the last unbound block of the 702-function
  engine surface. `ModelBuilder` gains `define_userflag`,
  `undefine_userflag`, `userflag_def_count`, `get_userflag_def`, and
  `get/set/clear_userflag_value`; the `solver.userflags` view gains
  `define()`, `undefine()`, `definitions()` (returning `UserFlagDef`
  records), `get_value()` / `set_value()` / `clear_value()`, real
  `len()` / iteration over definitions, and STRING-flag support in the
  mapping interface. New `UserFlagType` enum (BOOLEAN / INTEGER / REAL /
  STRING). Tests: `python/tests/engine/test_userflags_schema.py`.
- **Python bindings:** new lazy `Solver.surface2d` property returning the
  cached `Surface2D` view, so 2D access no longer requires constructing
  `Surface2D(solver.handle)` by hand.

### Changed

- **Python bindings:** `del solver.userflags[name]` now removes the
  flag's schema definition and per-object values via
  `swmm_userflag_undefine` (previously raised `TypeError`); assigning a
  `str` value auto-defines a STRING flag, mirroring the scalar setters.
- `python/tests/test_api_coverage.py`: removed 54 stale `KNOWN_UNBOUND`
  allowlist entries for symbols that had since been bound; the allowlist
  is now empty and the coverage test enforces the full surface.

## [Unreleased] — Runtime forcing verification pass (handoff build/test/fix)

Builds, runs and verifies the runtime-forcing batch per
`docs/RUNTIME_FORCING_TESTING_HANDOFF.md`. Full Python suite green
(833 passed) and the C++ unit suite green (78/78, 2D enabled). Thin bindings
(§2) and functional tests (§3: M4, M5, S1, S2, T1, Q4, Q5 + GW quality) added.

### Fixed

- **`[POLLUTANTS]` concentrations silently zeroed:** `PostParseResolver`
  unconditionally re-ran `resize_pollutants()` (which zero-fills) *after*
  `handle_pollutants` had parsed the values, so every INP rain/GW/RDII/DWF/
  init concentration loaded as 0 (the DWF/GW/rain quality features did
  nothing for INP-driven models). Guarded the resize like the adjacent
  node/link resizes (`if count != n`).
- **Refactored snow never accumulated:** the `[SUBCATCHMENTS]` snow-pack
  column was never read (no deferred name resolution) and the snow solver's
  per-subarea `fArea` was never initialised, so `plowSnow`/melt treated
  every surface as zero-area. Added `snowpack_name` deferred resolution and
  `fArea` init (legacy `snow_initSnowpack`).
- **Climate temperature/wind forcing stuck after clear:** a one-shot or
  cleared prescription never reverted because the forcing overwrote the same
  `ClimateState` field it read as the broadcast base. Added
  `temperature_src`/`wind_speed_src` source bases resolved fresh each step.
- **`ForcingData::effective_rainfall`/`_snowfall` out-of-bounds:** lacked the
  size guard `effective_evap_rate` has, segfaulting direct-solver unit tests
  with unsized forcing arrays.
- **Legacy `get_value` misread valid negatives:** `swmm_getValueExpanded`'s
  return was validated by sign, so a sub-freezing air temperature or the
  −999 API-unset sentinel raised a spurious error. Now keys off the system
  ERROR_CODE and the API-error sentinel range.
- **Groundwater inflow quality (audit A5):** GW inflow pollutant mass
  (`q_gw × c_gw`) was never applied. Added `QualitySolver::addGwLoads()` and
  a `qual_routing_gw_in` bucket; the report's Groundwater Inflow quality row
  (previously hardcoded 0) and the quality continuity total now include it
  (and DWF).
- **2D mass-balance evaporation (§4.1):** moved the cumulative evap loss from
  the 2D state mirror into `MassBalance2D::evap_out`, folded into `error()`,
  and surfaced in the report's 2D continuity block.
- **`[POLLUTANTS]` writer round-trip:** `InpWriter` now writes the `Cdwf`
  and `Cinit` columns (§4.4).
- **numpy 1.x build:** `PatchNumpyPxd.cmake` only rewrites the Cython pxd on
  numpy ≥ 2.0 (it would break the numpy 1.x build it was meant to support).
- Deleted dead `SnowSolver::batchATIUpdate`/`batchAccumulate` (§4.3); added
  scalar `execute`/`plowSnow` convenience overloads (per-subcatchment array
  signatures broke `test_snow.cpp`).

## [Unreleased] — Runtime forcing API phases 1–3 complete (gap plan rows 5–12)

See `docs/RUNTIME_FORCING_API_GAP_PLAN.md` and
`docs/RUNTIME_FORCING_TESTING_HANDOFF.md` (functional verification pending).

### Added

- **Global evaporation prescription (M4):** legacy `swmm_API_EVAP` system
  property (replaces the post-adjustment `Evap.rate` for all consumers
  incl. conduits/storage; per-subcatchment PET still wins); refactored
  `swmm_forcing_climate_evap()` channel. Python:
  `LegacySystem.set/get/clear_api_evap_rate`, `Forcing.climate_evap`.
- **DRY_ONLY runtime toggle (M5):** legacy `swmm_EVAP_DRY_ONLY`;
  refactored `swmm_climate_set/get_dry_only()`. Python:
  `LegacySystem.set/get_evap_dry_only`, `Forcing.climate_dry_only`.
- **Legacy pollutant source setters (Q1–Q3, Q5):** new `swmm_POLLUTANT`
  dispatch with `swmm_POLLUT_RAIN/GW/RDII/DWF_CONCEN` (500 block),
  runtime-settable; `SWMMPollutantProperties` Python enum.
- **Refactored link quality forcing channel (Q4):**
  `swmm_forcing_link_quality()` (REPLACE = concentration, ADD = mass rate,
  mass-balanced); `Forcing.link_quality`; `ForcingType.LINK_QUALITY`.
- **Legacy ponded-quality injection (Q6):**
  `swmm_SUBCATCH_POLLUTANT_PONDED_CONCENTRATION` now settable while running.
- **Groundwater state injection (S1):** legacy `swmm_SUBCATCH_GW_MOISTURE`
  / `_GW_LOWER_DEPTH` (set/get via `gwater_get/setState`); refactored
  `swmm_subcatch_set/get_gw_state()` with porosity/thickness clamping.
- **Snowpack state injection (S2):** legacy `swmm_SUBCATCH_SNOW_SWE/_FW/
  _ATI/_COLDC` (per snow subarea via sub_index); refactored
  `swmm_subcatch_set/get_snow_state()`.
- **2D mesh evaporation (T1):** depth-limited evaporation sink inside the
  CVODE RHS; `swmm_2d_force_evap()` / `swmm_2d_force_evap_uniform()`
  (m/s, OVERRIDE/ADD, RESET/PERSIST); `swmm_2d_get_mass_balance()` gained
  an `evap_out` total; Python `Surface2D.force_evap/_uniform`.

### Fixed

- **Refactored DWF quality (audit A1):** the `[POLLUTANTS]` `Cdwf` column
  was parsed and discarded and dry weather quality inflow did not exist.
  Added `PollutantData.c_dwf`, `QualitySolver::addDwfLoads()` (mirroring
  RDII loads), a `qual_routing_dw_in` mass-balance bucket included in the
  continuity error, the report's Dry Weather Inflow row (previously
  hardcoded 0), and `swmm_pollutant_set/get_dwf_conc()`.

### Audits

- A3: refactored ponded-quality and buildup setters are runtime-callable
  (no running guards) — functional verification handed off.
- A4: the 2D solver has **no infiltration sink** (T2 remains out of scope).
- Phase 4 parameter-surface audits are execution-gated — protocol in
  `docs/RUNTIME_FORCING_TESTING_HANDOFF.md` §6.

## [Unreleased] — Snowfall forcing + snow-path repair (gap plan row 4)

See `docs/RUNTIME_FORCING_API_GAP_PLAN.md` (item M3).

### Added

- **Per-subcatchment snowfall forcing (M3):** refactored
  `swmm_forcing_subcatch_snowfall()` (in/hr US, mm/hr SI as SWE;
  OVERRIDE/ADD, RESET/PERSIST) with `Forcing.subcatchment_snowfall()` and
  `ForcingType.SUBCATCH_SNOWFALL`; resolves on the temperature-split gage
  snowfall before accumulation, plowing, and melt. Legacy already had
  `swmm_SUBCATCH_API_SNOWFALL`.
- New checked-in snow fixture `python/tests/data/solver/site_drainage_snow.inp`
  (snow pack on S1, constant 25 °F temperature series).

### Fixed

- **Refactored snow path:** snowfall never accumulated — `plowSnow()` was
  never called from the step pipeline, so packs could melt but never grow.
  Accumulation + plowing now runs each runoff step before melt (matching
  legacy `runoff.c` order), and the snow solver takes per-subcatchment
  rain/snow inputs (previously a single area-weighted broadcast), with
  per-subcatchment rain-on-snow vs. degree-day melt selection (matching
  legacy `snow_getSnowMelt`/`meltSnowpack`).
- **Legacy `apiSnowfall` continuity hole:** prescribed snowfall influenced
  melt computations but never accumulated in the pack (only gage snow did,
  via `snow_plowSnow`), while still counting as rainfall inflow in the
  runoff mass balance. `snow_plowSnow()` now includes `apiSnowfall`.
- **Refactored `swmm_subcatch_get_snow_depth()`** was a stub returning 0;
  it now returns the area-weighted pack SWE in user depth units via the
  new `SWMMEngine::subcatchSnowDepth()`.

## [Unreleased] — Runtime climate forcing (gap plan rows 1–3)

See `docs/RUNTIME_FORCING_API_GAP_PLAN.md` (items A2, A5, M1, M2).

### Added

- **Air temperature forcing (M1):** legacy `swmm_API_TEMPERATURE` system
  property (set while running; `<= -999` clears) with read-only
  `swmm_TEMPERATURE`; refactored `swmm_forcing_climate_temperature()`
  channel (OVERRIDE/ADD, RESET/PERSIST) with `swmm_climate_get_temperature()`.
  Applied before derived climate quantities (saturation vapor pressure,
  psychrometric constant, Hargreaves moving average) so snowmelt and
  temperature-evap consumers stay consistent. User units (°F US, °C SI).
- **Wind speed forcing (M2):** legacy `swmm_API_WINDSPEED` (negative
  clears) with read-only `swmm_WINDSPEED`; refactored
  `swmm_forcing_climate_wind()` with `swmm_climate_get_wind_speed()`.
  User units (mph US, km/hr SI).
- Python: `LegacySystem.set/get/clear_api_temperature` and
  `…_api_wind_speed`; refactored `Forcing.climate_temperature/_wind`
  setters, `get_climate_temperature/_wind_speed` getters, and
  `ForcingTarget.CLIMATE` for `Forcing.clear`.

### Fixed

- **Subcatchment rainfall forcing (A2):** `swmm_forcing_subcatch_rainfall()`
  previously had no effect — `applyForcings()` pre-wrote
  `subcatches.rainfall`, which the runoff solver then overwrote from the
  gage. The forcing now resolves inside the runoff solver's rainfall
  assembly (same pattern as the PET forcing fix).
- **`Forcing.clear()` channel mapping (A5):** the Python binding passed
  `ForcingTarget` object-kind codes where C `SWMM_ForcingType` channel
  codes were expected, so clearing a SUBCATCH actually cleared node
  quality. It now clears every channel belonging to the requested object.

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
