# Changelog

All notable changes to the OpenSWMM Engine are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

The `6.0.0-alpha.3` sections below are **unreleased / pending** — no
`v6.0.0-alpha.3` git tag has been cut yet, but it is the version already
set in `CMakeLists.txt` (`OPENSWMM_PRERELEASE`), `vcpkg.json`, and
`python/pyproject.toml` for all work merged since the `v6.0.0-alpha.1`
tag, so it's used here instead of a generic "Unreleased" heading.

## [6.0.0-alpha.3] — Object deletion: complete referential integrity + new delete APIs

### Changed

- **2D surface routing: VFR is now the default cell closure.** `CELL_CLOSURE`
  defaults to `VFR` and `FACE_RECONSTRUCTION` to `VFR_FACE` (were `FLAT`/`MEAN`).
  The Begnudelli & Sanders (2006/2007) volume/free-surface closure restores the
  C-property at shorelines and removes the "water climbs uphill" artifact of the
  flat closure (which overstates a partially wet cell's free surface by up to
  two-thirds of its relief). Default on **all** backends — serial CVODE, serial
  ARKODE, and the Kokkos OpenMP/GPU path. **`CELL_CLOSURE FLAT` /
  `FACE_RECONSTRUCTION MEAN` restore the legacy behavior** and remain selectable.
  `VFR_MIN_WET_FRAC` (default `0.01`, range `(0, 0.5]`) tunes the wetted-area
  floor that keeps the closure C¹ for the implicit solvers. See
  `plans/2d/2D_VFR_SOLVER_CLOSURE_PLAN.md`.

### Added

- **Nine new delete + `analyze_impact` API pairs** covering every remaining
  data-object type: `swmm_pollutant_delete`, `swmm_pattern_delete`,
  `swmm_aquifer_delete`, `swmm_snowpack_delete`, `swmm_lid_delete`,
  `swmm_street_delete`, `swmm_inlet_delete`, `swmm_landuse_delete`, and
  `swmm_hydrograph_delete` (name-keyed). Each cascades or nullifies every
  cross-reference and reports the impact set; `analyze_impact` previews the
  same set without mutating. `SWMM_RefType` gained 15 additive values
  (`SWMM_REF_EXT_INFLOW` … `SWMM_REF_CONTROL_RULE`).
- **`swmm_control_find_references`** — read-only scan reporting which control
  rules reference an object by name (word-boundary match on NODE/LINK/CONDUIT/
  PUMP/ORIFICE/WEIR/OUTLET clauses, case-insensitive). Node/link
  `analyze_impact` and delete reports now include affected rules as
  `SWMM_REF_CONTROL_RULE` entries; **rule text is never edited by a delete**.
- **`swmm_control_remove_rule`** — remove a single rule by index (previously
  only `swmm_control_clear_rules` existed).

### Fixed

- **Node delete left dangling references** in ext-inflow / DWF / RDII rows
  (rows now cascade-deleted, survivors renumbered), the positional treatment
  expression matrix (the deleted node's stripe is now erased — previously every
  node after it silently read its neighbor's treatment), and subcatchment
  `gw_node` (now nullified, previously only renumbered).
- **Subcatchment delete** now cascades LID-usage rows, clears LID `drain_to`
  and snowpack `removal_subcatch` name references.
- **Rain-gage delete** now clears unit-hydrograph gage assignments.
- **Table/timeseries delete** now clears the gage `ts_name` mirror and
  ext-inflow `ts_name` references, and nullifies + renumbers the subcatchment
  adjustment-pattern indices (`n_perv`/`d_store`/`infil`), which index tables
  and were previously silently misaligned by any table delete.
- **Street delete** resets STREET cross-sections on referencing conduits to
  CIRCULAR (mirrors transect delete) instead of leaving a dangling name.
- `swmm_pattern_remove` and `swmm_hydrograph_remove_group` now delegate to the
  same deleters as the new APIs (one code path; pattern removal previously
  missed nothing but reported nothing either).

## [6.0.0-alpha.3] — Cross-section geometry API + SWMM_XSectShape renumbering

### Fixed

- **`SWMM_XSectShape` selected the wrong cross-section for every code from 8
  up.** `swmm_link_set_xsect`/`get_xsect` pass the shape straight through with a
  `static_cast` to the engine's storage enum (`openswmm::XsectShape`), but the
  published `SWMM_XSECT_*` constants followed a different ordering — so
  `SWMM_XSECT_IRREGULAR` (19) stored a vertical ellipse, `SWMM_XSECT_EGGSHAPED`
  (14) stored a baskethandle, and so on. Only codes 0–7 and `SWMM_XSECT_STREET`
  were correct. The constants are now the storage codes, and 26 `static_assert`s
  in `openswmm_links_impl.cpp` pin the two together so the cast is provably
  sound and any future drift breaks the build instead of the model.
  **Breaking for C code that hard-coded the old integers**; code that passed the
  constants symbolically is corrected by recompiling. Five shapes that had no
  constant at all (`BASKETHANDLE`, `SEMICIRCULAR`, `CUSTOM`, `FORCE_MAIN`,
  `DUMMY`) were added. `swmm_xsect_shape_name()` resolves a code at runtime.
- **Python `XSectShape.IRREGULAR` / `CUSTOM` / `FORCE_MAIN` were mismapped.**
  Their values (16/17/18) were read by the engine as `RECT_TRIANG` /
  `RECT_ROUND` / `HORIZ_ELLIPSE`, so assigning them silently produced the wrong
  cross-section. The enum now mirrors the engine's codes exactly and gained the
  seven shapes it was missing (`RECT_TRIANG`, `RECT_ROUND`, `HORIZ_ELLIPSE`,
  `VERT_ELLIPSE`, `ARCH`, `STREET_XSECT`, `DUMMY`) — all 26 are now nameable.
- **`_geometry._GEOM_LABELS_EXTRA` labelled the wrong shape codes.** The
  ellipse/arch labels sat on 19/20/21 rather than 18/19/20. The table is folded
  into `_GEOM_LABELS`, now keyed by enum member and covering every shape.

### Added

- **Standalone cross-section geometry API** (`openswmm_xsect.h`). Exposes the
  engine's own geometry kernels — area, top width, hydraulic radius, section
  factor, critical depth, and their inverses — as a reference implementation
  usable with no model open. Sections are built from shape + Geom1–Geom4, from
  transect / shape-curve / street data, or from a link of an open model
  (`swmm_link_create_xsect`), in which case the handle deep-copies the geometry
  the engine actually built and outlives the engine. Every scalar query has an
  `_array` counterpart. There is no geometry maths in the new code: it delegates
  to `xsect::setParams` and the `xsect::` kernels, so results match a simulation
  exactly.
- **`XSectionGeometry`** in `openswmm.engine` — the Python surface for the
  above, with scalar-or-NumPy dispatch on every query, `from_transect` /
  `from_curve` / `from_street` / `from_link` constructors, and a required
  keyword-only `units` argument (no default, so a unit system is never silently
  assumed). Also `shape_name()`, and a new `xsect_geometry` guide page.
- **`Links.get_xsect_info()`** — implements the method `_geometry` had
  documented since it was written but which never existed, returning the
  already-exported-but-never-constructed `CrossSection`. Also
  `link.xsect.info()` and `link.xsect.geometry()`, plus `CrossSection.from_raw`.

### Changed

- `link::applyTabulatedXSectParams` (new, in `Link.cpp`) consolidates the
  IRREGULAR / CUSTOM / STREET full-flow property derivation that previously
  lived only inside `PostParseResolver`, so the standalone constructors and the
  resolver share one legacy-parity implementation. Behaviour is unchanged —
  `ctest` is green including all transect/street parity tests.

## [6.0.0-alpha.3] — Precipitation scaling (gage SCF fix + per-subcatchment factors)

See `plans/PRECIP_SCALING_IMPLEMENTATION_PLAN.md` and
`plans/PRECIP_SCALING_TEST_HANDOFF.md`.

### Fixed

- **Refactored engine dropped the gage snow catch factor (SCF) at runtime.**
  The C++ engine parsed, stored, wrote and API-exposed the `[RAINGAGES]` SCF but
  never multiplied by it: `separatePrecip()` was dead code with zero call sites,
  and the live rain/snow split in `SWMMEngine.cpp` used the raw gage intensity.
  Legacy `gage.c` applies SCF. Snowfall volume now matches legacy. Replaced
  `separatePrecip()` with `gage::splitPrecip()`, the single source of truth for
  the split. **Behaviour change for any model with `SCF ≠ 1.0`** — every model
  in `tests/` and `examples/` uses `SCF = 1.0`, so no shipped baseline moves,
  but user snow models will.
- **Refactored engine applied no rain/snow split for snow-pack-less
  subcatchments.** The non-snow-pack runoff path read raw gage rainfall with no
  temperature test, so below freezing it received `rain × 1.0` where legacy
  gives `rain × SCF`. `RunoffSolver::execute()` now routes through
  `splitPrecip()` too. **Behaviour change for `SCF ≠ 1.0` snow-season models.**
- **`InpWriter` silently dropped the `[SUBCATCHMENTS]` snow-pack assignment.**
  Only 8 columns were emitted, so token 8 (Snowpack) was lost on every INP
  round-trip. The writer now emits it (and a `*` placeholder when a trailing
  scale factor must reach past an unassigned pack).
- **`swmm_gage_set_snow_factor` was not settable mid-run.** It carried a
  `CHECK_GEOMETRY` guard the sibling `swmm_gage_set_scale_factor` deliberately
  omits, so a calibration/RTC loop could not adjust the SCF after the model was
  opened. Removed the guard — SCF is a scalar precipitation multiplier, not
  geometry, and now matches the rainfall scale factor and the new subcatchment
  scale factors.

### Added

- **Optional per-subcatchment rainfall and snow scale factors** — trailing
  tokens 9 and 10 of `[SUBCATCHMENTS]`, both defaulting to `1.0` (a true
  no-op; default-valued models round-trip byte-identically). They compose
  multiplicatively with the gage factors:
  `rainfall = gage_rain × gage.scaleFactor × rainScaleFactor`,
  `snowfall = gage_rain × gage.scaleFactor × SCF × snowScaleFactor`.
  Applied to the gage-derived component only — API/forcing overrides are
  absolute and deliberately unscaled. Token 8 accepts `*` as a "no snow pack"
  placeholder so tokens 9/10 are reachable without a pack. Exposed across the
  full stack: C API (`swmm_subcatch_{get,set}_{rain,snow}_scale_factor`,
  settable mid-run), legacy property enum (`swmm_SUBCATCH_{RAIN,SNOW}_SCALE_FACTOR`),
  Python bindings (`Subcatchment.{rain,snow}_scale_factor` on both trees),
  GeoPackage subcatchments table (`rain_scale_factor`/`snow_scale_factor` REAL
  columns, reader guards a column-existence check so pre-existing `.gpkg` files
  still open), and the openswmm.mcp `editing_{get,set}_subcatch_{rain,snow}_scale_factor`
  tools (plus `editing_{get,set}_gage_snow_factor`, closing the last gage-SCF
  gap). The GUI surfaces all four factors in the Property Browser and the
  Attribute Table.

## [6.0.0-alpha.3] — Runtime forcing Phase 4 + §3 legacy quality sources

See `docs/RUNTIME_FORCING_PHASE4_HANDOFF.md`,
`docs/RUNTIME_FORCING_PHASE4_AUDIT.md` (per-group outcomes), and
`docs/RUNTIME_FORCING_API_GAP_PLAN.md` §7/§12.

### Fixed

- **`TIMESERIES` / `TIDAL` outfalls read the wrong stage table** — the
  `[OUTFALLS]` handler read the stage-data name and discarded it, deferring
  resolution to a post-parse pass that was never written, so
  `OutfallData::param` kept its default of `0`. Curves and timeseries share
  one index space (`ctx.tables`) and `Outfall.cpp` guarded only with `>= 0`,
  so the outfall silently drew its stage from whichever table came first in
  the model — an unrelated shape curve, in the reported case, pinning the
  outfall at that curve's y-value with no error. `OutfallData` now carries
  `param_name` for deferred resolution (mirroring `DividerData::link_name`),
  `PostParseResolver` resolves it against `ctx.table_names` and type-checks
  the referent (a `TIMESERIES` outfall may not point at a curve), and an
  unresolvable name is a fatal `ERR_NAME` as in legacy `outfall_readParams()`
  rather than a silent misread. Unresolved references now use `-1`, not `0`.
- **`InpWriter` corrupted `[OUTFALLS]` on save** — the section was emitted as
  `Name Elev Type Gated [Stage]`, but the canonical (and parsed) order is
  `Name Elev Type StageData Gated RouteTo`. A `FIXED` outfall's stage was
  written *after* the gate flag and re-parsed as `0`; `TIDAL`/`TIMESERIES`
  names and `RouteTo` were never written at all. The writer now emits the
  canonical column order, resolving the table *name* for `TIDAL`/`TIMESERIES`,
  and the parser accepts the EPA-GUI `*` stage-data placeholder on
  `FREE`/`NORMAL` rows without shifting the gate column.
- **`[FILES]` `USE/SAVE RUNOFF` and `USE/SAVE RDII` now work in the
  refactored engine** — previously parsed and written back but never
  consumed. `SAVE RUNOFF` auto-opens the existing binary runoff interface
  writer from the slot; `USE RUNOFF` replaces each runoff substep with the
  file's records (legacy `runoff_readFromFile`), driving the runoff clock
  from the recorded timesteps. New `RdiiInterfaceFile` implements the RDII
  slots: `SAVE RDII` exports computed flows in the legacy `SWMM5-RDII`
  binary format; `USE RDII` **bypasses the internal unit-hydrograph
  computation** (legacy `rdii_openRdii` semantics) and reads either the
  binary or the legacy text format with step-aligned (non-interpolated)
  lookup. Open/format failures fail `start()` with legacy errors
  323/325/343/345. `USE/SAVE RAINFALL` (collated binary rain file) remains
  unimplemented but now emits WARNING 103 instead of being silently
  ignored. Plan: `plans/FILES_INTERFACE_GAP_CLOSURE_PLAN_2026-07-02.md`.
  Tests: `tests/unit/engine/test_files_iface_gaps.cpp`,
  `python/tests/engine/test_files_iface_gaps.py`.

- **Routing interface files (`[FILES]` `USE INFLOWS` / `SAVE OUTFLOWS`) now
  work in the refactored engine** — the paths were parsed and stored but the
  `InterfaceManager` was never opened, so simulations silently ran without
  the upstream inflows (and wrote no outflows file). `swmm_engine_start()`
  now opens both files, reads/writes the legacy headers, and fails with
  legacy errors 351/353/355/357 on open/format problems. Outfall rows are
  written at reporting cadence (legacy `iface_saveOutletResults`), flows are
  converted to the declared units on write, interface pollutant loads flow
  into node quality mixing and the new "External Inflow" row of the quality
  routing continuity report, and interface flow volume is booked as external
  inflow in the routing mass balance. Wrong-mode rows (`SAVE INFLOWS` /
  `USE OUTFLOWS`) are rejected at parse time (legacy `ERR_ITEMS`). Plan:
  `plans/ROUTING_INTERFACE_FILE_INTEGRATION_PLAN_2026-07-01.md`. Tests:
  `tests/unit/engine/test_iface_routing.cpp`,
  `python/tests/engine/test_iface_routing.py`.

### Added

- **Python bindings for 2D vertex coupling CD/AREA** —
  `Surface2D.get/set_vertex_coupling_cd` and `get/set_vertex_coupling_area`
  wrap the recently added `swmm_2d_get/set_vertex_coupling_cd` / `_area` C
  API (the `[2D_VERTEX_NODE_MAP]` CD and AREA columns), closing the binding
  gap flagged by `python/tests/test_api_coverage.py`. Tests:
  `python/tests/engine/test_surface2d_view.py::TestVertexCouplingParams`.

- **Python GeoPackage model export** — `Solver.write_with_plugin(path,
  output_plugin_id)` and the convenience `Solver.write_geopackage(path,
  crs=...)` so a loaded model can be exported to a `.gpkg` from Python (the C
  API already had `swmm_model_write_with_plugin`; only `ModelBuilder` wrapped
  it before). `write_geopackage(crs="EPSG:2284")` applies the CRS via
  `solver.spatial.crs` first, so every feature layer is tagged with that SRS —
  without a CRS the geometries get an undefined SRS (`srs_id 0`) and GIS tools
  cannot place them. Added the `GEOPACKAGE_PLUGIN_ID` constant (the real id is
  `org.hydrocouple.openswmm.plugins.geopackage`; corrected the stale example in
  `openswmm_model.h` that omitted `.plugins.`). Tests:
  `python/tests/engine/test_geopackage_export.py`.

- **GUI-editor round-trip APIs** — getters/setters so the property and
  category editors can *load* an existing definition, not just write one
  (closes the gaps in `openswmm.gui/docs/HANDOFF_compile_verify_agent.md`
  §5.2–§5.2f):
  - Pollutant: `swmm_pollutant_set_units` (inverse of the existing
    `_get_units`; pre-start-only).
  - Aquifer: `swmm_aquifer_get_evap_pattern` / `_set_evap_pattern` — the one
    string column (`[ETupat]`) the param-code API didn't cover.
  - Snowpack (previously add/list-only): `swmm_snowpack_set/get_plowable`,
    `_impervious`, `_pervious` (the seven `[SNOWPACKS]` surface values),
    `_set/get_removal` (six values) and `_set/get_removal_subcatch`.
  - Inlet: `swmm_inlet_get_params` (inverse of `_set_params`) and
    `swmm_inlet_get_type`.
  - LID: `swmm_lid_get_surface` / `_soil` / `_storage` / `_drain` (inverse of
    the four layer setters), `swmm_lid_get_type`, and full set/get for the
    remaining two layers — `swmm_lid_set/get_pavement` (6 values: thick,
    void-ratio, frac-imperv, ksat, clog-factor, regen-days) and
    `swmm_lid_set/get_drainmat` (3 values: thick, void-frac, roughness) — so
    PERM_PAVEMENT and GREEN_ROOF controls now round-trip every layer.
  - Python bindings for all of the above (`Pollutant.units` setter,
    `Aquifers.get/set_evap_pattern`, `Snowpacks.set/get_*`,
    `Inlets.get_params/get_type`,
    `LIDs.get_surface/soil/storage/drain/type` + `set/get_pavement` +
    `set/get_drainmat`) with `.pyi` stubs. Tests:
    `tests/unit/engine/test_editor_roundtrip_api.cpp` (bit-exact C round-trips)
    and `python/tests/engine/test_editor_roundtrip.py`. All six LID layers are
    now covered end-to-end.

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
- **Phase 4 wave B8 — aquifer parameters (P10).** New setter in both engines
  (none existed before). Flux coefficients (conductivity, conductivity slope,
  tension slope, upper-evap fraction, lower-evap depth, lower-loss coefficient)
  are runtime-editable — refactored `SWMMEngine::refreshAquiferParams` re-derives
  the groundwater solver's per-subcatchment flux columns on each edit; legacy
  reads `Aquifer[]` live. Structural / initial-condition parameters (porosity,
  wilting point, field capacity, bottom/water-table elevation, upper moisture)
  seed GW state and are pre-start-only (`LifecycleError` / `ERR_API_IS_RUNNING`
  while running). Refactored `swmm_aquifer_get_param`/`_set_param` +
  `SWMM_AquiferParam` enum, binding `Aquifers.get_param`/`set_param` +
  `AquiferParam`; legacy `swmm_AquiferProperty` (800 block) via the existing
  `swmm_AQUIFER` object case, binding `SWMMAquiferProperties`. Enum coverage
  +12 (aquifer). Tests: `TestAquiferParamsRuntime`, `TestLegacyAquiferParams`.
- **Phase 4 wave B9 — adjustment arrays (P9): no setter, decision recorded.**
  The audit closes the gap matrix without new code: the monthly climate
  adjustment arrays are covered at runtime by the more direct Phase-1 forcing
  setters, and the per-subcatchment N-PERV/DSTORE/INFIL adjustment patterns are
  retunable mid-run via the P6 pattern-factor setter. See the audit doc for the
  rationale. This completes the Phase 4 parameter-surface audit — every
  §12.1 group now has a recorded disposition.

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

## [6.0.0-alpha.3] — 2D GPU acceleration, capped-pipe 1D/2D coupling & performance

Commits 2026-06-10 → 2026-07-05, surfaced via a `git-cliff` audit of
`v6.0.0-alpha.1..HEAD` against this file (see `cliff.toml`) — these landed
alongside the runtime-forcing work above but were not yet logged.

### Added

- **Portable Kokkos GPU surface solver + HIP/SYCL plugins (2D).** New
  performance-portable GPU path for the 2D surface router
  (`docs/2D_GPU_PORTABLE_CVODE_STRATEGY.md`): CVODE control flow,
  tolerances, and operator-splitting are unchanged — only the `N_Vector`
  and RHS move to Kokkos, numerically equivalent to the serial CPU
  reference. `ISurfaceSolver` interface with `SurfaceSolverFactory`
  resolving the backend at runtime (dlopen a GPU plugin, else fall back to
  the serial `CvodeSurfaceSolver`; `OPENSWMM_2D_BACKEND` override, auto
  order cuda → hip → sycl → omp → serial). GPU plugin is opt-in
  (`OPENSWMM_BUILD_GPU_PLUGIN`) via a stable C ABI (`GpuPluginAbi.h`); base
  build stays Kokkos-free. New vcpkg `gpu`/`gpu-cuda`/`gpu-hip`/`gpu-sycl`
  features.
- **Capped-pipe 1D/2D junction coupling model.** Replaced the two-regime
  free-inlet/surcharge blend with a bidirectional gradient orifice
  (`h_2d - h_1d`) gated by a C1 Hermite ramp at the pipe crown: below the
  crown the node pressurizes internally over its auto-sized ponded shaft
  with no exchange; the cover only connects the domains once water
  overtops it. `sur_depth` now solely sizes the 1D Preissmann-slot
  headroom above the crown. Doc:
  `docs/1D_2D_COUPLING_CONFIGURATION.md`.
- **Time-based 2D coupling window.** New `COUPLING_WINDOW` (s) in
  `[2D_OPTIONS]` (-1 AUTO / 0 every step / >0 s; env
  `OPENSWMM_2D_COUPLING_WINDOW`; INP + GeoPackage round-trip) decouples
  the 2D advance cadence from 1D variable-step shrinkage. Adds a
  quiescence short-circuit (windows with no water/rain/coupling sources
  and only WALL/NORMAL_FLOW boundaries skip the CVODE advance entirely)
  and a stencil-scoped CFL hint so a rain-wetted cell far from the network
  no longer pins the 1D routing step. Bellinge 3h smoke: 15.2s → 9.2s
  (omp).
- **Dry-cell active-set masking (wet-front tracking), opt-in.** Restricts
  every 2D RHS pipeline stage to `active = wet ∪ sourced ∪ halo` while the
  CVODE system stays full-size (frozen cells get `ydot = 0`, so the BDF
  history is never invalidated). Opt-in via `[2D_OPTIONS] ACTIVE_SET`
  (env `OPENSWMM_2D_ACTIVE_SET`); wall-guarded (mask errors are locally
  conservative, never a leak) with a breach-redo/halo-doubling safety
  net. `SurfaceStateData` gained an `active_set` pointer — GPU plugin ABI
  bumped to v2.

### Performance

- **1D dynamic-wave Picard loop:** persistent-team OpenMP threading
  (team spans the whole iteration loop; CSR node-centric gather replaces
  the serial link-order scatter, provably bit-identical FP order to
  legacy at any thread count), batch-geometry kernels parallelized with
  Apple-Silicon P-core clamp/QoS, and dead-work elimination
  (loss recompute / node-state init skipped when structurally a no-op).
  Bellinge: 204s → 56.5s (T=8, Apple Silicon); routing loop 95.8s → 80.1s
  in the separate bit-parity-preserving optimization pass (bypass-aware
  batch geometry kernels). All changes gated on a 20-model bit-parity
  scorecard / byte-identical `.out` files.
- **2D held-path coupling:** skip zero-exchange coupling stencils in the
  active set (the exchange is a per-window constant already scattered
  into `coupling_flux`, so a stencil with zero flux this window
  contributes nothing) — storm-peak active set drops from ~38% to
  ~wet+halo (~5%) of the mesh on held-path coupled models.

## [6.0.0-alpha.3] — User-flag schema bindings + 2D/MCP gap closure

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

## [6.0.0-alpha.3] — Runtime forcing verification pass (handoff build/test/fix)

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

## [6.0.0-alpha.3] — Runtime forcing API phases 1–3 complete (gap plan rows 5–12)

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

## [6.0.0-alpha.3] — Snowfall forcing + snow-path repair (gap plan row 4)

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

## [6.0.0-alpha.3] — Runtime climate forcing (gap plan rows 1–3)

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

## [6.0.0-alpha.3] — Subcatchment PET prescription

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

## [6.0.0-alpha.3] — Pythonic Python bindings (v1)

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

## Legacy EPA SWMM 5 engine history

The sections below cover the EPA-maintained `src/legacy` engine (`swmm5.c` /
`runoff.c` / `dynwave.c` / etc.), reconstructed from EPA's official update
notes (`epaswmm5_updates.txt`). Only **Engine Updates** are listed — the
original notes also describe Windows-GUI-only changes (EPA's separate
Delphi `epaswmm5.exe`), which are out of scope for this engine repository.
GUI-facing engine capabilities added here (e.g. Streets/Inlets, LID
practices) were later re-implemented for the OpenSWMM GUI/MVC layer in
`openswmm.gui`.

Builds `v5.0.22` through `v5.2.4` have matching `git` tags in this
repository. Builds `5.0.001`–`5.0.021` (SWMM 5's original 2004–2010
release run) predate this repo's tag history — no `v*` tag exists for
them here — and are listed below without version brackets for that
reason, oldest first.

## [5.2.4] — 2023-07-15

### Fixed

- Mismatch between reported pollutant Surface Runoff mass and conveyance
  system Wet Weather Inflow mass in a run's Status Report.
- Invalid-input-data test for an LID unit with an underdrain.
- Water-flux-rate calculations between layers in Bio-Retention, Permeable
  Pavement, and Infiltration Trench LID units.
- Hydraulic head seen by a storage-layer underdrain in a Permeable Pavement
  LID with a soil layer above it.
- Retrieval of the backing parameters for a Street cross-section.
- Generation of transect points for a Street cross-section with a
  depressed gutter, and the gutter-slope calculation for depressed-gutter
  Street links.
- Effective hydraulic head seen within a curb inlet with an inclined
  throat opening.

### Changed

- Conduit evaporation/seepage loss per time step is now limited to the
  conduit's current volume (was its flow rate) under dynamic wave routing,
  and is split evenly between both end nodes (was upstream node only).
- Default Inertial Damping and Variable Time Step option values now match
  the GUI's defaults.

## [5.2.3] — 2023-02-12

### Fixed

- Double counting of initial moisture volume in the drainage-mat layer of
  a green roof LID unit.

## [5.2.2] — 2022-12-01

### Added

- Dimension check for the Modified Basket Handle and Round-Rectangular
  cross sections (rounded-portion height cannot exceed total height).
- Additional performance statistics in the Street Flow Summary table.

### Changed

- Default number of dynamic-wave routing threads changed to 1, matching
  the User's Manual and the GUI.

### Fixed

- Long run times when the simulation duration exceeded the end of an
  externally applied time series.
- A bug (introduced in 5.2.0) causing the math-expression evaluator to
  compute `a*b^c` as `(a*b)^c` instead of `a*(b^c)`.
- Storage-unit evaporation/exfiltration loss reported as a percentage of
  total storage volume.
- Warning messages about raising a node's max depth and adjusting a
  conduit's elevation drop (removed in 5.2.1) restored.

## [5.2.1] — 2022-08-01

### Changed

- Use of the Normal Flow Limited feature for dynamic wave routing is now
  optional.
- For kinematic-wave storage routing, the reported depth after
  convergence is based on the last volume value rather than the next
  trial depth.
- Reduced excessive Status Report warnings: no message when a node's max
  depth is raised to the crown of the highest connecting conduit, or when
  a conduit's elevation drop/slope is adjusted to a minimum allowed value.

### Fixed

- A refactoring bug causing excessive execution times for projects with
  control rules.
- Egg-shaped cross-section geometry tables at the two lowest relative
  depth levels.
- Dry nodes no longer have their pollutant concentration forced to 0 when
  receiving non-zero pollutant inflow (a 5.2.0 regression); a non-storage
  node with no inflow now keeps its water-quality concentration unchanged
  instead of being zeroed.
- `F_OFF` definition in `output.c` for non-MS C/C++ compilers.

## [5.2.0] — 2021-11-01

Last EPA-maintained release before this project's refactor.

### Added

- **Street runoff capture by inlet drains** — new Street cross-section
  type (`[STREETS]`), Inlet object (`[INLETS]`), and conduit `[INLET_USAGE]`
  placement; HEC-22 (or custom capture-curve) inlet capture analysis
  interfaced with flow routing; new Street Summary table (peak flow depth
  and spread per Street conduit/Inlet).
- Type 5 variable-speed pump obeying the pump affinity laws (head/flow vs.
  speed).
- Pre-defined analytical Storage Curve shapes: cylinders, paraboloids,
  cones, pyramids.
- New control-rule condition-clause quantities, including past n-hour
  rainfall; condition clauses can now include named variables and math
  expressions.
- Listing of nodes with the highest flow-routing non-convergence frequency
  in the Status Report.
- Support for the latest NOAA Climate Data Online GHCN service (US or SI
  units).
- Additional validation check on the user-supplied Green-Ampt Initial
  Deficit value.
- New Rain Barrel LID parameter: covered or not.
- Command-line executable now supports binary output files larger than
  2 GB; number of open files increased to 8192.
- A number of new functions added to the SWMM 5 API.

### Changed

- Permeable Pavement LID effective permeability now accounts for the
  Impervious Surface Fraction parameter.
- Permeable Pavement LID depth values in the detailed report are now
  expressed in inches/mm (was feet).
- Math-expression parser now allows exponents to be expressions, not just
  constants.
- Time-step-average reporting option's average-flow computation changed.
- Shell sort replaces insertion sort for sorting event periods.

### Fixed

- Conversion of runon flow into an equivalent ponded depth for Curve
  Number infiltration.
- Total reporting time value used in several summary-table statistics.

## [5.1.15] — 2020-05-01

### Added

- A mix of infiltration methods can now be used within a single project.
- Status Report grouped frequency table of variable routing time steps
  used during a simulation.
- Fatal error now issued if a storage node's area curve produces a
  negative volume when extrapolated to the node's full depth.

### Fixed

- Average summary statistics for a reporting start date later than the
  simulation start date.
- Pollutant mass-balance error when very shallow storage units lost all
  inflow to flooding.

## [5.1.14] — 2020-03-01

### Fixed

- A refactoring bug producing incorrect rainfall when the same time
  series was used by an RDII-Unit-Hydrograph rain gage and another
  subcatchment gage.
- Skipping the first rain gage in a project when checking for duplicate
  station IDs with different data files.
- A crash running projects with LID units but no subcatchments.
- LID underdrain pollutant loads incorrectly added to mass-balance totals.
- The program hanging when an LID unit sent outflow back onto the
  pervious area of its own subcatchment.
- Failure to re-initialize layer volumes for each LID unit evaluated.
- Street sweeping being ignored when the sweeping period began with a
  higher day-of-year than its end.
- Incorrect adjustments for conduit evaporation/seepage losses under
  dynamic wave routing.
- Soil-moisture-deficit recovery being ignored for Green-Ampt
  exfiltration from storage units.
- Node/link ID names mistaken for option keywords in the `[REPORT]`
  section.
- A possible crash when reporting average (vs. point) values within each
  reporting time interval.

## [5.1.13] — 2018-05-10

### Added

- Monthly time patterns for a subcatchment's depression-storage depth,
  pervious Manning's n, and hydraulic conductivity.
- LID controls can now treat a designated portion of a subcatchment's
  pervious-area runoff (previously impervious-only).
- Permeable pavement LIDs subject to clogging can have permeability partly
  restored at periodic intervals.
- LID underdrain flow-control options: auto-open/auto-close depth
  thresholds and a head-based control curve for nominal drain flow.
- Pollutant removal percentages assignable to LID underdrain processes.
- Subcatchment Runoff Summary Report now includes pre-LID pervious and
  impervious total runoff volumes.
- Choice of dynamic-wave surcharge method: traditional EXTRAN Surcharge
  Algorithm, or a new SLOT (Preissmann Slot) option for closed conduits
  flowing >98.5% full.
- Storage-unit node can model a closed/pressurized vessel via a Surcharge
  Depth value.
- Weir discharge coefficient can vary with head via a Weir Curve.
- Periodic time step option for control-rule evaluation.
- Option to report node/link time-series results as reporting-step
  averages instead of interpolated point values.

### Changed

- A regulator link's upstream offset below its downstream node's invert
  is now auto-raised only under dynamic wave routing (with a warning);
  other routing methods only warn.

### Fixed

- Unused rain gages no longer examined when adjusting the wet-runoff time
  step.
- Permeable-pavement LID surface inflow rate capped at the pavement's
  permeability.
- Minimum Nodal Surface Area dynamic-wave option now applied only when a
  node's connecting-link surface area falls below it (was always-available
  surface area).
- Full closed rectangular cross-section top width now set to 0.
- Mitered Corrugated Metal Arch culvert "C" parameter value corrected.
- Flow-continuity-error reporting for systems with backflow through
  outfall nodes.

## [5.1.12] — 2017-03-14

### Changed

- `direct.h` now only `#include`d when compiled for Windows.
- Redacted the 5.1.011 update that internally aligned the wet time step
  with the reporting time step — it caused problems for certain time-step
  combinations.
- Subcatchment's bottom elevation (not its parent aquifer's) now used
  when saving a water-table value to the binary results file.
- Conduit seepage-rate conversion (per-area → per-length) now uses top
  width instead of wetted perimeter (only vertical seepage is assumed).
- Crest-length reductions for end contractions no longer used for
  trapezoidal weirs.
- `NO`/`YES` no longer accepted as `NORMAL_FLOW_LIMITED` attributes (only
  `SLOPE`/`FROUDE`/`BOTH`).
- User-supplied minimum-slope option now initialized to 0.0 (none).
- Routing Events and Skip Steady Flow options now work correctly
  together; steady-state periods with no flow routing no longer skew
  routing-time-step statistics.
- MS exception-handling statements now only enabled for the Microsoft C
  compiler.

### Fixed

- Failure to limit surface infiltration into a saturated rain-garden LID
  unit.
- Maximum-limit calculation on LID drain flows, for smoother results at
  low depths above the drain offset.
- A variable used for detailed LID reporting is now properly initialized.
- Occasional duplicate lines written to the detailed LID results file.
- Coefficient of the evaporation/seepage term in the dynamic-wave flow
  equation (corrected 1.5 → 2.5).
- Engels flow equation for side-flow weirs (incorrect since SWMM 3/4).
- Slope Correction Factor for culverts with mitered inlets.
- An entry in the gravel-roadway weir-coefficient table.
- Number of barrels now accounted for when compiling full-conduit-flow
  frequency statistics.
- Water level in storage nodes with no outflow links under kinematic
  wave/steady flow routing.
- Depth-at-max-width formula for the Modified Basket Handle cross
  section.

## [5.1.11] — 2016-08-22

### Added

- Detailed flow routing can be restricted to pre-defined event periods
  (`[EVENTS]` section: start/end date and time).
- New API functions `swmm_getError()` and `swmm_getWarnings()`.
- Recognizes the new NCDC Climate Data Online precipitation file format.
- Check that subcatchment imperviousness does not exceed 100%.
- Rule premises can include `SIMULATION DAYOFYEAR`.

### Changed

- Error codes returned by the API functions (`swmm_open`, `swmm_start`,
  `swmm_step`, etc.) corrected.
- Runoff time steps adjusted to stay aligned with the Report time step.
- LID native-soil infiltration now satisfied first when it occurs
  alongside underdrain flow.
- LID underdrain offset no longer limited to the top of the storage layer
  (allows upturned drains).
- Detailed LID report file now lists results by date/time and elapsed
  hours, and reports water level (not moisture content) for permeable
  pavement.
- A regulator link opening below its downstream node invert is now
  auto-raised to invert level (with a warning, was warning-only).
- Node surcharging now only reported for dynamic wave routing; storage
  nodes are never classified as surcharged.
- Status Report no longer lists modulated-control actions (continuous,
  produced an enormous number of entries).

### Fixed

- Monthly conductivity adjustments now also applied to the internal
  Green-Ampt "Lu" parameter.
- Time-step correction for outfall outflow returned to a subcatchment.
- A weir with an open rectangular shape and non-zero slope no longer
  raises a spurious input error (slope is ignored).
- Illegal array-index bug checking the pump-curve type for an Ideal Pump
  under dynamic wave routing.
- Redundant unit conversion of max. reported depth in the Node Depth
  Summary table.
- Storage unit surface-area-curve metric→internal conversion for bottom
  exfiltration.
- A bug resetting a link's `TIMEOPEN` control-rule variable when its
  setting changed between partly-open states.
- Roadway Weir road-width metric-unit conversion.
- Saved link settings read from a hot-start file, for models containing
  pollutants.
- A refactoring bug affecting water-quality mass-balance results for
  Steady Flow routing.
- Date/time fractional-part decoding could round to 24:00:00.

## [5.1.10] — 2015-08-05

### Added

- Modified Green-Ampt infiltration option (no upper-zone moisture-deficit
  redistribution during low-rainfall events; more infiltration for storms
  beginning with low intensity, e.g. SCS design storms).
- ROADWAY weir type (FHWA HDS-5 overtopping method), typically used in
  parallel with a culvert conduit.
- Rule premises can test whether a link has been open/closed for a
  specific duration.
- Unsaturated hydraulic conductivity ("K") usable in custom groundwater
  flow equations.
- Daily potential evapotranspiration (PET) added as a system output
  variable.

### Changed

- Hargreaves evaporation formula now uses a 7-day running average of
  daily temperatures (was single-day values).
- `qualrout.c` refactored to be more compact.
- Storage seepage/evaporation losses now based on end-, not start-,
  of-prior-time-step storage volume.

### Fixed

- A 5.1.008 regression that excluded LID infiltration from the
  groundwater routine.
- Failure to properly initialize the "initially wet" LID flag.
- Duplicate printing of the first line of an LID detailed report file.
- `makefile` for the GNU C/C++ compiler now correctly links the OpenMP
  libraries.

## [5.1.9] — 2015-04-30

### Added

- New warning for a control-rule premise comparing two different
  variable types.

### Fixed

- A refactoring bug preventing simulations longer than 68 years.
- Input-parsing error preventing recognition of a two-variable comparison
  in a control-rule premise.
- Runon to a subcatchment fully occupied by LIDs missing from its Summary
  Report (5.1.008 update 12 regression).
- LID units returning outflow to a subcatchment's pervious area even when
  LIDs occupied the entire subcatchment.
- Units label for Total Inflow Volume in the Node Inflow Summary table.

### Changed

- Dry conduit/storage-node definition for quality routing changed to
  ≤1 mm depth (avoids concentrations blowing up from evaporation losses).

## [5.1.8] — 2015-04-02

### Added

- Monthly adjustment patterns for hydraulic conductivity (rainfall
  infiltration and storage/conduit exfiltration).
- LID drains can send outflow to a different node/subcatchment than their
  parent subcatchment.
- Outfall nodes can send outflow onto a subcatchment (irrigation / complex
  LID treatment).
- New Rooftop Disconnection LID practice, with an optional downspout
  flow-capacity limit.
- Optional soil layer for Permeable Pavement LIDs (sand filter/bedding).
- New groundwater-equation variables: porosity, unsaturated hydraulic
  conductivity, infiltration rate, percolation rate; new Groundwater
  Summary table.
- Minimum Variable Time Step option for dynamic wave routing (down to
  0.001 s, was fixed at 0.5 s).
- Dynamic-wave routing parallelized across multiple processors (new
  `THREADS` option, default 1).
- Node Depth Summary column for max depth at the Reporting Time Step.
- Control-rule premises can compare a node/link variable's value at two
  different locations (e.g. `NODE 123 HEAD > NODE 456 HEAD`); node volume
  added as a condition variable.

### Changed

- LID runon from another source is now distributed across the
  subcatchment's non-LID area only (unless a single LID occupies the full
  area).
- Non-zero-runoff reporting threshold changed from 0.001 cfs to
  0.001 in/hr.
- Overall flow-routing mass-balance calculation now accounts for negative
  flow streams (e.g. total external inflow).
- Report labels renamed: "Surface Runoff" → "Total Runoff" (Runoff
  Continuity), "Internal Outflow" → "Flooding Losses" (Flow Routing
  Continuity).
- Pollutant washoff routines moved to a new module (`surfqual.c`),
  revised to account for LID runoff reduction.
- Steady Flow routing's initial flows are now ignored (removes their
  mass-balance contribution).
- Lateral inflows to conveyance nodes now evaluated at the start (was end)
  of the routing time step.
- Final runoff/routing time steps adjusted so total simulation duration
  is not exceeded.
- Storage node HRT added to Hot Start file state.

### Fixed

- Evaporation rates read from a time series only updated on new days,
  occasionally stopping a run prematurely.
- Hot Start runoff value assigned to the wrong internal property
  (`newRunoff` vs. `oldRunoff`).
- Indexing bug reading Hot Start files with snowmelt parameters.
- Non-conduit link setting from a Hot Start file not used to initialize
  the link.
- Snowmelt adjustment for snow-covered area derived from an areal
  depletion curve.
- Snowmelt double-counted in total subcatchment precipitation.
- Green Roof LID drainage-mat flow calculation applied void ratio to
  depth instead of area.
- LID wet/dry runoff time-step choice ignored LID unit state, causing
  excessive LID continuity errors.
- Refactoring bug leaving LID detailed-report time in minutes instead of
  hours; results now written at each runoff step where LID state changes.
- Groundwater evaporation loss not initialized to 0 for subcatchments
  with no pervious area.
- Excessive continuity errors for systems with high conduit seepage
  rates.
- Pollutant loss through conduit/storage-node seepage not included in
  mass balance.
- Conduit/storage-node concentrations not increased to account for
  evaporation volume loss.
- Capacity-limited-links check exiting prematurely on a non-conduit link.
- Bug identifying the percent of time a conduit has either end full.
- A refactoring bug that prevented surcharged weirs (5.1.007) from
  passing any flow.
- Bug evaluating recursive nodal water-quality treatment function calls.

## [5.1.7] — 2014-09-15

### Added

- Monthly adjustments for temperature, evaporation rate, and rainfall.
- Support for reading the new GHCN-Daily climate data files (NCDC Climate
  Data Online).
- Custom equation support extended to deep-groundwater-aquifer seepage
  flow (previously lateral flow only); `[GW_FLOW]` renamed to `[GWF]` with
  a format change to accommodate both.
- New Weir parameter: whether the weir can surcharge via an orifice
  equation.
- Storage-unit seepage can now use Green-Ampt infiltration (head-dependent
  seepage rate); constant-rate option remains via a zero initial moisture
  deficit.

### Changed

- Modified Horton method's dry-period infiltration-capacity-recovery
  formula revised.
- Green-Ampt infiltration functions refactored for clarity.
- Most LID simulation routines modified for more accurate results under
  flooded conditions; detailed LID results now always correspond to a
  full reporting time step.

### Fixed

- Green-Ampt initial cumulative infiltration into the upper soil zone was
  incorrectly set to the maximum value instead of zero.
- Infiltration out of the bottom of a Bio-Retention Cell or Permeable
  Pavement LID with a zero-depth storage layer.
- Groundwater flow-equation variable name for receiving channel bottom
  height corrected to match the GUI (`Hcb`).
- Crash when a climate file supplied evaporation rates with no
  subcatchments in the project.
- Flow/pollutant routing mass-balance accounting for negative external
  inflows.
- Area-available-for-seepage calculation for a storage node with a
  tabular storage curve.
- Depth-from-volume function for a storage curve where depth falls within
  a constant-area (vertical-wall) section.

## [5.1.6] — 2014-05-19

### Fixed

- Off-by-one error updating the next scheduled write time for detailed
  LID results.
- Soil-water-available-for-evaporation in LID soil layers wasn't limited
  by the wilting point.
- Misplaced parenthesis in the permeable-pavement infiltration-rate
  equation.
- Units-conversion error computing a pollutant's contribution from direct
  precipitation to subcatchment water quality.

### Changed

- Increased decimal places for hourly evaporation in the detailed LID
  report.

## [5.1.5] — 2014-04-23

### Fixed

- A problem reading hydraulic results from a hot-start file.

## [5.1.4] — 2014-04-14

### Added

- Support for the Ignore RDII analysis option.

## [5.1.3] — 2014-04-08

### Added

- New Upper Zone Evap. Pattern property on the Aquifer object (monthly
  adjustment of upper-zone evaporation fraction).

### Fixed

- Bug writing/reading RDII flows to the binary RDII file.

## [5.1.2] — 2014-03-31

### Fixed

- Bug preventing hotstart files with the latest format from being read.

### Changed

- Only non-ponded surface area is now saved for use in the dynamic-wave
  surcharge algorithm.

## [5.1.1] — 2014-03-24

### Added

- Support for the new NOAA-NCDC online precipitation file format.
- Modified Horton infiltration method (uses cumulative infiltration in
  excess of the minimum rate as its state variable).
- RDII interface files now saved in a binary format (ASCII still
  supported for externally-created files).
- Green Roof and Rain Garden LID categories (previously configured only
  via Bio-Retention Cell).
- Custom groundwater outflow equation per subcatchment.
- Evaporation of water from open channels.
- New conduit Seepage Rate property (uniform seepage along bottom/sloped
  sides).
- New Dynamic Wave options: maximum iterations and head tolerance per
  time step, plus reporting of the percentage of non-convergent time
  steps.
- User-settable flow tolerances for steady-state-skip determination.
- Control rules can use a conduit's OPEN/CLOSED status in premises and
  actions.
- New Node Inflows Summary column: mass-balance error in volume units.
- New Link Pollutant Load summary table.

### Changed

- Storage-unit infiltration renamed to seepage (single seepage-rate
  parameter; legacy Green-Ampt parameter sets still recognized).
- Meaning of the link "Capacity" view variable: fraction of full
  cross-section area filled (conduits) vs. control setting (other links).
- Froude Number link view variable replaced by flow volume; subcatchment
  "Losses" replaced by separate Evaporation/Infiltration variables; upper
  groundwater-zone Soil Moisture added.
- Rain Barrel Drain Delay of 0 now allows continuous draining while
  filling.
- Dropped the requirement that an impervious surface be dry before street
  sweeping.
- Remaining pollutant mass after a surface goes dry is now treated as
  unavailable for future washoff ("Remaining Buildup" in the mass-balance
  report).
- Wet-weather washoff inflow-load interpolation across a routing time
  step modified for better runoff/quality-routing agreement.
- RDII unit-hydrograph time-step selection modified for K < 1.0 (ratio of
  rising- to falling-limb duration).
- Upper groundwater zone reaching saturation now sets the lower saturated
  zone depth to the full aquifer depth.
- Conduits with small negative slopes are auto-corrected to the positive
  minimum slope (enables Steady Flow / Kinematic Wave routing).
- Avg. Froude Number / Avg. Flow Change columns replaced with
  normal-flow-limited and inlet-controlled time fractions in the Flow
  Classification Summary.
- Weirs no longer operate as an orifice when surcharged; excess flow
  floods the upstream node instead.
- Pump flow at a reporting time falling mid-transition now uses the
  nearest (start or end) value rather than interpolating.
- Binary results file no longer stores zero-valued pollutant results when
  Water Quality analysis is disabled.
- Hot Start files now contain the complete watershed + conveyance-system
  state.

### Fixed

- Fully-flowing open-channel flow can no longer exceed the full normal
  flow; Normal Flow Limit (slope + Froude) criteria unified.
- A check preventing outflow from a dry node.
- Control-rule elapsed-time/time-of-day equality tests made more
  accurate; such conditions now also accept decimal hours.
- Error 319 renumbered to 320 (new Error 319: unrecognized rainfall file
  format); external time-series format errors now use Error 363 (was
  173).

## [5.0.22] — 2011-04-21

### Added

- New validation errors: LID surface-layer vegetation volume fraction
  less than 1, total LID area exceeding subcatchment area, or total LID
  capture area exceeding subcatchment impervious area.
- New error 318 for a user rainfall file with dates out of sequence.
- New error 110 if a subcatchment's ground elevation is below its
  groundwater aquifer's initial water-table elevation.
- Checks added to the groundwater mass-balance solution (lower-zone depth
  vs. total depth; upper-zone moisture vs. porosity).
- Pump Summary Report expanded: number of startups, minimum flow, time
  off at both ends of the pump curve.

### Changed

- LID Storage-layer Conductivity now means the native soil's saturated
  hydraulic conductivity below the layer (was the layer's own
  conductivity).
- Storage layers are now optional (zero height) for Bio-Retention Cells
  and Permeable Pavement LIDs.
- A zero-top-width LID overland-flow surface now spills excess water
  above the surface storage depth instantaneously.
- Water initially stored in LID units is now reported in the Runoff
  Continuity table.

### Fixed

- Rain Barrel LID Drain Delay time conversion (hours → seconds).
- Vegetative Swale infiltration calculation, so a fully pervious swale
  with vertical sides matches an equivalent pervious subcatchment.
- Missing values for accumulation periods within an NWS rain file.
- Evaporation during wet periods incorrectly including rainfall/runon as
  available moisture (should be current ponded depth only).
- Curve Number infiltration now uses only direct precipitation (was
  including runon/internally-routed flow).
- Tailwater term in the groundwater flow equation now zero when no
  tailwater depth exists.
- Divide-by-zero for an empty Filled Circular pipe, and for an empty
  trapezoidal channel with zero bottom width.
- Critical/normal depth adjustment for a conduit no longer allowed to set
  depth to exactly zero.
- Orifice/weir flow depth not reported as 0 when its setting was changed
  to 0 (reporting only, no effect on routing).
- Node Surcharge Summary not reporting a ponded node as surcharged
  (reporting only, no effect on routing).

---

The releases below (SWMM 5.0.001–5.0.021, 2004–2010) predate this
repository's tag history; there is no corresponding `v*` git tag, so they
are headed by build number rather than `[x.y.z]`.

## Build 5.0.021 — 2010-09-30

### Changed

- Rainfall + runon used to compute infiltration no longer pre-adjusted by
  subtracting evaporation loss.
- Green-Ampt infiltration rate no longer allowed below the smaller of
  saturated hydraulic conductivity and available surface moisture
  (moisture below a small tolerance is now treated as 0).
- Pollutant Loading summary tables now list all pollutants in a single
  table (was 5 pollutants per table).

### Fixed

- A code-refactoring error in 5.0.019 that prevented recovery of
  infiltration capacity during dry periods.
- Pervious-area adjustment (5.0.019) for evaporation/infiltration to a
  subcatchment's groundwater zone.
- Accounting of evaporation loss from just a subcatchment's pervious
  area.
- Evaporation/infiltration losses from Storage nodes under Kinematic Wave
  and Steady Flow routing.

## Build 5.0.020 — 2010-08-23

### Fixed

- A refactoring bug preventing SWMM from reading rainfall data from
  external rainfall files.

## Build 5.0.019 — 2010-07-30

### Added

- Explicit modeling of five Low Impact Development (LID) practices at the
  subcatchment level.
- Pollutant buildup over a landuse can now be specified by a time series
  instead of just a buildup function.
- Option to evaporate standing water only during periods with no
  precipitation.
- Controls based on flow rates now account for flow direction.

### Changed

- Storage-node evaporation/infiltration losses now computed directly
  within the flow-routing routines for better mass conservation.
- Normal-flow check now uses only the upstream Froude number (was both
  up- and downstream).
- Maximum trials for dynamic-wave flow/head equations increased 4 → 8.
- Ponding calculation revised again for continuity: a surcharged/ponded
  node's depth change per time step is now bounded near full depth,
  governed by ponded area (dynamic wave); for Kinematic Wave/Steady Flow,
  ponded area is now just a pond/no-pond indicator and flooded depth is
  set to the node's maximum depth. Node Flooding Summary now reports
  ponded depth (dynamic wave) or ponded volume (other routing), not
  acre-inches.
- Groundwater mass-balance equations reverted to their 5.0.013 form.
- Villemonte correction for downstream submergence extended to partly
  filled orifices (previously weirs only).
- A non-conduit link connected to a storage node no longer contributes to
  the node's surface area.
- Auto max-depth adjustment to match a connected link's crown no longer
  applies to bottom orifices.
- Internal routing of runoff between impervious/pervious sub-areas is
  ignored when a subcatchment has only one type of sub-area.
- The Ignore Snowmelt switch is now automatically set true when no snow
  pack objects are defined.

### Fixed

- A missing term in the submerged-inlet-control check for Culvert
  conduits.
- Min/max daily temperatures from a climate file are now swapped if
  min > max; Hargreaves-derived evaporation rates can no longer be
  negative; several bugs reading Canadian DLY02/04 climate files.
- Zero rainfall values in a rain file/time series are now skipped
  (treated as a dry period) instead of desynchronizing the record.
- A bug desynchronizing evaporation time-series data from the simulation
  clock.
- Water-quality mass balance now correctly accounts for initial mass
  introduced via a hot-start file.
- For runoff-only models, the wet runoff time step is now capped at the
  reporting time step when the latter is smaller.

### Removed

- Fatal error is now raised for a negative conduit entrance/exit/average
  loss coefficient (previously silently accepted).

## Build 5.0.018 — 2009-11-18

### Added

- Storage Volume Summary table now reports total infiltration +
  evaporation loss as a percentage of total inflow, per storage unit.
- Warning message when a Rain Gage's recording interval is less than the
  smallest interval in its rainfall time series.
- Hot Start files now include each subcatchment's final groundwater-zone
  state.

### Changed

- Link Summary table now lists the actual conduit slope rather than the
  slope adjusted by conduit lengthening.
- Status Report now displays only the summary tables for which results
  were obtained.
- Engine version number corrected to 50018 (had been overlooked since
  5.0.010).

### Fixed

- Double counting of final stored volume when finding nodes with the
  highest mass-balance errors.

## Build 5.0.017 — 2009-10-07

### Added

- A default dry-weather-flow concentration property on the Pollutant
  object (overridable per node).

### Changed

- Ponding routine for dynamic wave routing further modified to handle a
  node transitioning between surcharged and ponded conditions within one
  time step (fixing large 5.0.016 ponding continuity errors).
- Error 112 (conduit elevation drop exceeds length) downgraded from fatal
  error to warning; slope computed the pre-5.0.014 way (elevation
  drop/length) in this case.
- Inflow interface files no longer need to contain every pollutant
  defined in the current project.
- RDII unit-hydrograph time step now uses the smaller of the wet runoff
  time step and the shortest hydrograph's time-to-peak (was the rain
  gage's recording interval), permitting hydrographs that peak faster
  than the gage interval.
- Curve Number infiltration now stops once the maximum capacity is fully
  used.
- CSTR mixing equation for water-quality routing replaced with a more
  robust finite-difference approximation (avoids numerical problems at
  high decay rates); first-order decay is now applied under Steady Flow
  routing via a dedicated routine.

### Fixed

- Water-quality mass-balance errors in systems with node treatment,
  by correctly accounting for both inflow mass and mass in storage.

### Removed

- The small ponded-depth tolerance before runoff initiation was removed
  for a smoother runoff response.

## Build 5.0.016 — 2009-06-22

### Added

- Option to compute daily evaporation from climate-file daily
  temperatures using Hargreaves' method.
- Recognition of comma-delimited NCDC rainfall files (with/without
  station name) and space-delimited NCDC files with empty condition-code
  fields.
- Error check for an RDII unit hydrograph whose time base is less than
  its rain gage's recording interval.

### Changed

- Nodes that can pond are no longer always treated as non-surcharging
  storage nodes — only once ponding actually occurs.
- Extrapolated storage-curve surface area above the table's highest depth
  is now only used if the curve slopes outward; otherwise the last
  tabulated area is used.

### Fixed

- A small full/not-full storage-node tolerance that could keep a full
  unit "full" despite small net outflow, removed.
- Spurious negative-elevation-offset warnings for `*`-offset or
  near-invert offset values.
- A 5.0.015 regression producing incorrect RDII inflows when the gage
  recording interval was less than the wet time step.

## Build 5.0.015 — 2009-04-10

### Added

- Optional Green-Ampt infiltration parameters on Storage nodes (infiltration
  basin support), now explicitly accounting for ponded-water-depth effect
  on infiltration rate.
- Separate Initial Abstraction parameters (max depth, initial depth,
  recovery rate) for each of the three RDII unit hydrographs (short/
  medium/long term) in a group.
- Meander Modifier transect parameter (ratio of meandering main-channel
  length to overbank length).
- Recognition of space-delimited NWS TD 3240/3260 files with a station
  name field.

### Changed

- Normal-flow limitation based on Froude number now requires the
  criterion hold for both upstream and downstream depths (was either).
- Computed top surface width for dynamic wave routing is no longer
  floored at the width-at-4%-depth value; the actual width is used no
  matter how small.

### Fixed

- A 5.0.014 regression that inadvertently removed the 2 GB binary
  output-file size limit for GUI runs.
- Backflow into an outfall node is now correctly counted in the node's
  Total Inflow result.
- Reporting error for overflow rate into ponded volume at a flooding
  node under dynamic wave routing.

### Removed

- Rainfall time-series/rain-gage recording-interval mismatches are now a
  fatal error instead of a silently auto-adjusted gage interval.

## Build 5.0.014 — 2009-01-21

Large feature release (culverts, custom cross-sections, minimum slope,
baseline inflow patterns).

### Added

- Culvert Inlet Control flow computation under dynamic wave routing for
  designated Culvert conduits.
- Minimum Slope option — a computed conduit slope is never allowed below
  this value.
- Optional Baseline Time Pattern for external inflows at nodes (monthly/
  weekly periodic adjustment).
- Outlet rating curve can be based on either freeboard depth (as before)
  or the upstream/downstream head difference.
- "SIMULATION MONTH"/"SIMULATION DAY" added as control-rule time
  conditions; conduit OPEN/CLOSED status usable in premises/actions.
- Time Series data can now be imported from an external file.
- Option to ignore any combination of Rainfall/Runoff, Snowmelt,
  Groundwater, Flow Routing, and Water Quality process models.
- A user-defined groundwater outflow equation per subcatchment.
- Modified Baskethandle cross section extended to any circular-top radius
  ≥ half the section width.

### Changed

- Rain gage recording interval auto-adjusted to the smallest interval in
  its time-series data (with a warning); fatal error if gages sharing a
  time series don't share the same Rainfall Format.
- Curve Number infiltration regeneration rate now simply the reciprocal
  of the user-supplied drying time (no longer needs saturated
  conductivity); optional monthly adjustment pattern for the recovery
  rate.
- Under-relaxation of pump flows between DW-routing iterations dropped
  (could violate the pump curve); upstream-weighted area now used in the
  dQ/dH term for conduits; Froude numbers for the normal-flow check now
  use hydraulic depth.
- Ponded volume under dynamic wave routing now computed from computed
  nodal depth (reverting to pre-5.0.010 behavior) for consistency with
  storage-node treatment; orifice head now measured from opening midpoint
  (not bottom), and orifices no longer contribute end-node surface area.
- Orifice partial-open setting now interpreted as fraction of opening
  height (was fraction of area); equivalent discharge coefficient
  recomputed on every setting change.
- Washoff of user-specified initial buildup with no buildup function now
  works correctly; runoff/runon/rainfall concentration mixing revised for
  more consistent results, especially with BMP removal.
- Storage-unit quality routing switched to the analytical CSTR solution;
  HRT update formula revised; Steady Flow quality routing now treats
  conduit concentration as equal to the upstream node's.
- Reverse (backflow) inflow at an Outfall is now treated as an external
  inflow for water-quality purposes (models saltwater/contaminant
  intrusion).
- Snow removal now begins once the removal-depth threshold is reached,
  correctly converted to internal feet.
- "Total Flooding"/ponded-volume Node Depth Summary column relabeled "Max
  Vol. Ponded"; MGD/CMS flow values now report to 3 decimal places.

### Fixed

- Green-Ampt infiltration rate at the point of surface saturation
  mid-time-step.
- A crash with the No Routing option combined with Save Outflows
  Interface File.
- Under Steady Flow/Kinematic Wave, a Dummy conduit connecting to a
  higher-elevation node no longer requires an inlet offset.
- Possible closing of tide gates on outfalls directly connected to
  orifice/weir/outlet links.
- A bug preventing RDII from being computed for hydrographs sharing a
  rain gage with another hydrograph; a groundwater bug allowing
  infiltration to continue once the water table fully saturated; a
  metric-units conversion error for computed groundwater flow.
- The flow contribution of the triangular ends of a trapezoidal weir.
- A roundoff error under kinematic wave/steady flow that occasionally
  mis-reported nodes as ponded.

## Build 5.0.013 — 2008-03-11

### Changed

- PID controller definition and implementation revised.
- Dynamic-wave routing: new method weights upstream conduit geometry more
  as the Froude number approaches 1; Normal Flow Limit (slope + Froude)
  now applies both criteria together; flow in a fully-flowing open
  channel capped at full normal flow; a dry node can no longer have
  outflow; ponding computation reverted to the 5.0.009 approach (depth
  from volume); max-depth-change time-step criterion restored.

### Fixed

- Acceptable site-latitude value check.
- A code-refactoring error in the dynamic-wave momentum equation's
  inertial term.
- A node's crown elevation now considers connecting non-conduit links.
- Possible incorrect initial orifice setting.
- Error checks added for invalid numbers in a hot-start file.

## Build 5.0.012 — 2008-02-04

### Added

- PID-type modulated control rule.
- User-assigned maximum conduit flow limit now applies to all routing
  options (was Dynamic Wave only).
- Possibility of ponding at a Type I pump's inlet (wet well) node.

### Changed

- Conduit/orifice/weir/outlet offsets can now be an absolute elevation or
  a relative depth above the node invert (`LINK_OFFSETS` option).
- "Flooding" now recorded whenever water level exceeds a node's top,
  whether or not ponding occurs (previously only when there was no
  ponding).
- Green-Ampt upper-soil-zone drying-time calculation moved from time 0 to
  the first rainfall period (removes a start-date-shift artifact).
- Steady-state-flow detection criteria realigned with SWMM 4.
- Minimum flow-area/hydraulic-radius floor (0.0001) for dynamic wave
  routing removed (redundant with the depth floor); flow-direction test
  for UPSTREAM/DOWNSTREAM CRITICAL conditions removed (could stall
  solutions); max-depth-change time-step criterion dropped again.
- Head-loss calculation from flap gates extended to orifices.
- "Snow Only" pollutant-buildup option, previously unimplemented, now
  works.

### Fixed

- SI unit-conversion bugs for pump on/off depth settings and pump-curve
  slope values; Hazen-Williams head-loss formula for force mains.
- A 5.0.010 regression preventing RDII computation for hydrographs
  sharing a rain gage.
- Pollutant loading from RDII now based on RDII quality (was rainfall
  quality).
- System outflow/flooding values saved to the binary results file now
  match the values used for the flow-continuity-error calculation.
- Command-line version's default `END_TIME` corrected from 24 days to 0.

## Build 5.0.011 — 2007-07-16

### Fixed

- Weir/Outlet settings not being updated after a control-rule change.
- Weir control setting not accounted for in the equivalent orifice
  coefficient for surcharged flow, in V-notch weir flow, or in reported
  weir flow depth.
- A 5.0.010 change to ponded depth/volume computation under dynamic wave
  routing.
- Runon/rainfall/ponded-water quality-mixing equations, to prevent
  numerical instability at very low volumes.
- NCDC rainfall-file missing values ('M' flag) now counted in the
  reported missing-record total.

## Build 5.0.010 — 2007-06-19

Major release: engine recompiled with all `float`s as `double`s (except
binary-interface-file fields) under VC++ 2005.

### Added

- NO ROUTING analysis option (runoff-only runs).
- Ideal Pump type (pumps at inlet inflow rate, no pump curve).
- Custom Shape conduit cross section (via a new Shape Curve) and Circular
  Force Main shape (Hazen-Williams or Darcy-Weisbach for pressurized
  flow).
- Pump startup/shutoff inlet-node depths as direct pump properties
  (previously control-rule only).
- Timed orifice gate open/close rate (SWMM 4 `ORATE` parity).
- Initial-abstraction loss on RDII unit hydrographs.
- Combined slope + Froude-number criterion for supercritical/normal flow.
- Flow Instability Index per non-pump link, with the five highest listed
  in the Status Report.
- Node volumes now initialized from hot-start depth to reflect implied
  initial ponding.

### Changed

- Orifice head now measured to the opening's midpoint (not bottom);
  orifices no longer contribute end-node surface area; partial-open
  setting reinterpreted as fraction of opening height with the discharge
  coefficient recomputed on each change.
- Ponded depth under dynamic wave routing always set equal to computed
  ponded depth (was the smaller of ponded/dynamic depth).
- Width-vs-depth tables for circular and irregular cross sections
  expanded to 51 entries.
- Treatment-function math-expression evaluation made more efficient.
- Node Depth Summary's ponded-volume column relabeled "Max Vol. Ponded".

### Fixed

- Area corrections to dynamic-wave inlet/outlet loss terms (introduced in
  5.0.008) removed — reverted a regression.
- Kinematic-wave inflow-area normalization when flow is capped at maximum
  normal flow.
- Dynamic-wave variable-time-step node-fullness check (avoided
  excessively small steps).
- Divider-node check now examines both diversion-link end nodes.
- Outlet-link conditions now recognized in control rules; error raised
  for multiple rule clauses on one line.
- Ignore Rainfall option now zeroes rain-gage rainfall (prevented a
  spurious reported value).
- New Error 108 when a subcatchment outlet ID collides with both a node
  and a subcatchment name.
- Groundwater bug allowing infiltration to continue once the water table
  fully saturated, plus a metric-units conversion error on computed flow.
- Flow contribution of a trapezoidal weir's triangular ends.
- A roundoff error occasionally mis-reporting ponded nodes under
  kinematic wave/steady flow.

## Build 5.0.009 — 2006-09-19

### Changed

- Minimum runoff able to generate pollutant washoff changed from
  0.001 in/hr to 0.001 cfs.
- A new RDII event now begins once continuous dry weather exceeds the
  longest unit hydrograph's base time (was a fixed 12 hours).

### Fixed

- User-prepared climate files no longer confused with the Canadian
  format.
- Dynamic-wave routing through long force mains connected to Type 3/4
  pumps.

## Build 5.0.008 — 2006-07-05

### Added

- Constant value + scaling factor for Direct External inflows.
- Total pollutant washoff-load listing per subcatchment; new Node
  Inflows/Flooding and Outfall flows/pollutant-loads summary tables.
- Checks for non-negative conduit offsets and orifice/weir/outlet
  heights.

### Changed

- Pipe invert elevations at outfalls now measured relative to the
  outfall stage elevation (was the outfall's own invert).
- Entrance/exit minor-loss terms for dynamic wave routing adjusted by the
  mid-point-to-entrance/exit area ratio.
- Equivalent length cap for orifices/weirs changed from a 200 ft minimum
  to a 200 ft maximum.
- Subcatchment pollutant washoff reprogrammed for more rigorous mass
  balance when runoff is routed across subcatchments or with direct
  rainfall deposition.
- Revoked Engine Update #12 from 5.0.006.

### Fixed

- Horton infiltration drying-time → regeneration-curve-constant
  conversion.
- Flow-depth-from-head error in the dynamic-wave Froude-number
  normal-flow check.
- Rainfall-unit conversion when reading from an external file.
- Display of washoff mass-balance results for Counts/Liter pollutants.
- Reporting of total system maximum runoff rate in the Subcatchment
  Runoff Summary table.

## Build 5.0.007 — 2006-03-10

### Added

- Ignore Rainfall analysis option (external inflows/DWF only, no
  rainfall-driven runoff).
- Peak runoff flow added to the Subcatchment Summary table; non-conduit
  links now included in the Link Flow Summary table.

### Changed

- Hydraulic-radius calculations for Rectangular-Closed,
  Rectangular-Triangular, and Rectangular-Round shapes now account for
  wetted-perimeter increase under full flow.
- Full-Flow vs. Maximum-Flow distinction refined in several closed-conduit
  code paths; irregular cross sections where max-normal-flow depth is
  less than full depth now handled correctly.

### Fixed

- Final ponded-water volume from node flooding now included in the
  reported flow-continuity error.

## Build 5.0.006a — 2005-10-19

### Fixed

- Snowmelt-during-rainfall formula returned ft/sec instead of in/hr.
- Routing-interface-file generation for systems with nodes but no links.

## Build 5.0.006 — 2005-09-05

### Added

- Storage Unit maximum-volume/outflow-rate summary table.
- Optional SWMM 4 `BC` parameter (minimum groundwater table elevation for
  flow) on the groundwater flow equation.
- Control-rule Action clause can set pump/orifice/weir/outlet control via
  a curve (vs. node depth) or a time series ("Modulated Controls").
- Geometry tables for standard-size elliptical pipes.

### Changed

- Storage curves (area vs. depth) now linearly extrapolated beyond the
  table limit (SWMM 4 behavior), not held constant.
- Evaporation no longer computed for a dry storage unit; storage-unit
  water-quality concentrations now adjusted for evaporation loss each
  step.
- A climate file now positions to the simulation start (not file start)
  unless the user specifies a starting date; reaching end-of-file during
  a run is now a fatal error (was silently held at last value).
- Pollutant treatment functions using storage-node concentration now use
  inflow concentration (matching non-storage nodes); global first-order
  decay no longer applied to a storage unit that has its own treatment
  function.
- Total moisture available for infiltration each runoff step now has
  evaporation subtracted first.
- Node/Conduit flow statistics in the Status Report now collected only
  over the reporting period (not the full simulation period).

### Fixed

- Interior nodes mistaken for outfall nodes (depending on connecting-link
  orientation) during water-quality analysis.
- Water-quality routing through dummy conduits.
- Standard-size elliptical-pipe code number mistaken for an actual
  dimension.
- Upper-soil-zone moisture depletion during dry periods under Green-Ampt
  infiltration.
- Initial/final groundwater storage volumes in the Groundwater Continuity
  table (reporting only; did not affect computed flows or water-table
  levels).
- Climate files can now supply evaporation during runoff-free runs (was
  ignored with no subcatchments present).

## Build 5.0.005b — 2005-06-15

### Fixed

- End-node offsets for partly-filled circular cross sections weren't
  increased to account for fill depth.
- Weir flow wasn't necessarily zero when the high-head side's water level
  was zero.

### Changed

- Bottom Orifice "crest height" now interpreted as a horizontal plane
  above the upstream node's invert (supports storage-unit riser
  outlets).

## Build 5.0.005a — 2005-05-25

### Fixed

- An erroneous error message for a node with multiple outflow links
  including an Outlet link.

## Build 5.0.005 — 2005-05-20

### Added

- Maximum-allowable-flow property on the Conduit object (default 0 = no
  limit).
- New dynamic-wave routing option selecting the normal-flow-limit
  criterion (SWMM 4 `KSUPER` parity); new option to skip routing during
  steady-flow periods (reduces continuous-simulation run time).
- New, more robust water-quality routing algorithm for dynamic wave
  routing.

### Changed

- Conversion factor for external pollutant mass inflows must now convert
  to mass-concentration-per-second (flow units no longer part of the
  conversion).
- Minimum elevation change for a flat conduit changed to 0.001 ft (SWMM 4
  parity).
- Irregular cross-section max depth now based on the highest station
  elevation (was first/last station), with vertical walls added at the
  ends if needed; nominal width now the top width at full depth (was max
  width over all depths).
- Head over a non-surcharged, submerged weir now based on height above
  the weir crest (was head difference across the weir); side-contraction
  weir-length-reduction equation fixed (SWMM 4 bug).
- Depths at outfalls under Steady/Kinematic Wave routing now reported as
  the connecting conduit's depth.

### Fixed

- Ponded-depth computation at flooded nodes under dynamic wave routing.
- Wrong lookup function for Time-Series outfall water elevations.
- Interpolation of values from a routing interface file.
- Rainfall-file reader confusing the standard space-delimited format with
  other formats; a reporting error for rainfall series with no ending
  zero value.
- A missing snowmelt-coefficient computation for pervious areas.
- Max-to-design flow ratio per conduit, now accounting for barrel count.

### Removed

- The Compatibility Mode dynamic-wave option was removed in favor of a
  single method designed for SWMM 4 compatibility with more stable
  results.

## Build 5.0.004 — 2004-11-24

### Added

- Pollutant concentration-unit codes added to the binary output file.

### Changed

- Curve Number infiltration's regeneration-rate-from-drying-time
  calculation corrected to use a constant (not continuously declining)
  infiltration capacity per rain event.
- Surcharged/high-Froude-number conduits are now included when computing
  a dynamic-wave variable time step (previously excluded).

### Fixed

- NCDC-formatted external rain-file identification/reading.
- Reported-velocity sign for links with adverse slope.
- Reading results from previously saved Runoff Interface files.
- Dynamic-wave routine for SWMM3/SWMM4 compatibility modes (better match
  to Extran results).
- Zero-sloped-conduit check widened to elevation differences below
  0.01 ft.
- Ponded-depth computation at flooded nodes under dynamic wave routing.

## Build 5.0.003 — 2004-11-10

### Added

- Error 405 for a binary results file that would exceed the 2.1 GB system
  limit.
- Support for Canadian DLY02/DLY04 temperature files.

### Fixed

- Full-depth width-table entries for closed rounded cross sections
  (numerical stability under dynamic wave routing).
- A units problem for RDII inflows under metric flow units.
- Reading the `TEMPDIR` option when it contained spaces.
- Rule-based control of weir crest height (control setting previously
  adjusted flow instead of the crest-to-crown distance).

## Build 5.0.002 — 2004-11-01

### Changed

- Modifications to the Picard method used for dynamic-wave flow routing.

## Build 5.0.001 — 2004-10-29

First official release of SWMM 5.
