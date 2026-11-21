# Python bindings review and implementation strategy

Date: 2026-09-26. Repository: `openswmm.engine`.

> Historical review baseline. Implementation and current validation are recorded in [the implementation report](PYTHON_BINDINGS_IMPLEMENTATION_2026-09-26.md).

## Scope and evidence

This is a review and implementation plan, not an implementation of the missing bindings. It examines the current working-tree public C headers, modern Cython bindings, Python enums and stubs, tests, packaging, Sphinx sources, and relevant CI workflows. Existing local edits are included in the review. HEAD at evidence capture is `2daef06bec8ac48172010e31e709bc15827853b8`; the companion JSON records hashes of reviewed inputs because a commit alone does not describe this working tree.

The modern API is the primary scope. The legacy solver/output bindings and compatibility import paths receive a separate maintenance assessment; the plugin SDK is not assumed to require a one-to-one Python wrapper.

Evidence files:

- `PYTHON_BINDINGS_REVIEW_2026-09-26_inventory.tsv`: every modern C export and its source-level binding status.
- `PYTHON_BINDINGS_REVIEW_2026-09-26_evidence.json`: counts, missing functions, stub findings, input hashes, and validation results.
- Appendix A below: the complete missing-function inventory grouped by header.

### Current state

| Measure | Result | Interpretation |
|---|---:|---|
| Modern public C functions extracted | 1,172 | Exported declarations in the engine headers |
| Functions referenced outside extern blocks in `.pyx` | 1,050 | Source-level reachability signal, not behavioral coverage |
| Entries in existing intentional-exclusion list | 4 | Requires review; one is actually used by a `.pxd` helper |
| Functions with no binding use and no exclusion | **118** | Current failing API-coverage assertion |
| Modern Cython extension modules checked | 29 | All translate and pass generated-C++ syntax checking |
| Engine Python test files / test definitions | 63 / 891 | Inventory only; not a claim that these tests ran or passed |

The older parity report, `plans/parity/parity_matrix.md`, reports 552 functions and zero Python gaps. It is stale relative to this inventory and should not be used as a release-readiness claim.

### Validation performed

| Check | Result |
|---|---|
| `python3 -m unittest discover -s python/tests -p test_api_coverage.py -v` | 5 tests: 4 pass, headline coverage test fails with 118 functions |
| `python3 -m unittest discover -s python/tests -p test_solver_pxd_attrs.py -v` | 2 pass |
| Cython translation of all 29 modern `.pyx` modules | Pass |
| Apple Clang C++20 syntax check of all 29 generated translation units against current public headers, Python 3.13 and NumPy headers | Pass; no linking or runtime execution implied |
| Existing mypy engine check | Pass: 35 source files |
| Existing strict typing smoke file | Pass: 1 source file |
| Sphinx HTML, warnings treated as errors | Pass with external intersphinx inventories disabled |

Sphinx used the existing Python virtual environment plus `sphinxcontrib-mermaid` installed into a temporary directory. No project dependency configuration was changed. External links/inventories were not validated. A clean native rebuild, full functional suite, installed-wheel checks, Linux/Windows verification, and runtime memory-safety tests remain acceptance work for implementation.

## Findings, ordered by risk

### R1 — The 2D view can outlive the engine handle (P0)

`python/openswmm/engine/_2d.pyx` stores only `void* _engine`, populated from an integer. `Solver.surface2d` constructs it from `self.handle`; `Solver.destroy()` destroys and nulls the solver's own handle without invalidating the retained pointer. In `src/engine/2d/api/Api2D.cpp`, the entry guard checks for null, then dereferences the engine. A retained surface/infiltration view can therefore reach a freed pointer.

This is a concrete ownership defect identified in source; a crash was not intentionally triggered. Retaining the solver alone is insufficient because explicit `destroy()` must also invalidate access.

**Action:** use an owner-backed handle accessor with destruction/generation checks, including nested infiltration views. Define the behavior of views across close, reopen, mesh edits, builder-to-solver ownership transfer, and explicit destruction. Deprecate or clearly isolate the raw-pointer constructor.

**Tests:** retained view after destroy; solver garbage collection; nested view lifetime; repeated close/destroy; reopen with a different mesh; access concurrent with destruction. Run unsafe-path regressions in subprocesses and use sanitizer builds where supported.

### R2 — CI omits the existing drift guards (P0)

`python/pyproject.toml` sets the wheel test command to `pytest {package}/tests/engine ... --ignore=.../test_integration.py`. It does not run top-level `test_api_coverage.py`, `test_solver_pxd_attrs.py`, the other top-level tests, or `tests/legacy`. This is why a substantial binding gap can coexist with passing wheel tests.

The typing workflow watches `.py`, `.pyi`, and selected configuration files, but not `.pyx`, `.pxd`, or public headers. Python/typing workflows target main/develop, whereas documentation also targets the active `swmm6_rel` branch. Align the actual branch/release policy instead of relying on comments about manual triggers.

**Action:** add an inexpensive source-contract job before wheel builds, and include it as a release dependency. Run legacy/top-level functional tests in explicit jobs. Review and either migrate or document the exclusion of `test_integration.py`.

**Tests:** prove the guard fails when a declaration is added without a wrapper, a wrapper is removed, an enum member changes, or a stub signature diverges. Comments and extern declarations must not satisfy reachability.

### R3 — Four entire API domains are absent (P1)

Groundwater authoring/results, groundwater transport, surface water quality, and ARD transport configuration account for **82 of the 118 gaps**. All exported functions in their four headers are absent from the modern binding call surface. Generic string options and INP editing do not replace these structured APIs.

Expose these as coherent domain views. Proposed names in this document are design suggestions, not existing Python features. Preserve the distinction between authoring data, runtime state, diagnostics, and supported engine capabilities.

### R4 — Enum and report drift persists inside already-bound domains (P1)

Confirmed missing named members include:

- `RefType`: 15 reference kinds with codes 7–21. `_edit.pyx` already has their string names, so the exported enum disagrees with the editor rather than the whole editor being absent.
- `FilePathRole`: `MESH_2D`, `OUTPUT_2D`, `LID_REPORT` (11–13).
- `RunoffTotal`: initial/final snow storage (7–8).
- `RoutingTotal`: 1D-to-2D coupling outflow (12), link groundwater inflow (13).
- `ForcingType`: four per-element climate channels and link seepage (13–17).

The name-based audit also finds nine C enum types without a same-named Python enum: `EvapType`, `TempSource`, `WindType`, `HumidityType`, `HumidityVar`, `HeatElemKind`, `InpProfile`, `TransportDispersionMode`, and `UnitSystem`. Some underlying operations already accept integers; classify these as typed-surface decisions, not nine entirely missing behaviors. Review the absent `ForcingMode.NONE` sentinel separately rather than automatically advertising it as a valid setter mode.

The enum audit ignores public `#define` selectors, including groundwater scope/zone/soil/closure/ledger constants and transport matrix state/domain/class constants. Those need explicit mappings too.

`_report.py` omits the new snow and coupling/groundwater totals. The `RoutingTotal.FORCING_INFLOW` Python docstring says it is distinct from EXTERNAL, while the C header now describes it as a subset. That distinction matters to consumers assembling water balances.

**Action:** compare names, numeric values, aliases, sentinels and macro constants against C; synchronize `.py`, `.pyi`, exports, report records, examples and accounting descriptions. Generic integer getters already make some numeric selectors callable; that does not provide named discovery or report completeness.

### R5 — Runtime members, stubs, and generated reference docs differ (P1)

Confirmed examples of members present in `.pyx` but absent from their `.pyi` surface:

| Runtime class | Missing stub members |
|---|---|
| `Pollutants` | `rename` |
| `Landuse` | `rename` |
| `Quality` | `validate_treatment_expression` |
| `Subcatchment` | `coverages`, `loadings`, `rain_scale_factor`, `snow_scale_factor` |

Some are in current uncommitted implementation changes. They should ship with their companion stubs and documentation. This member-name comparison is a lower-bound finding, not an exhaustive proof of signature/default/type parity.

There is no `py.typed` marker under `python/openswmm`, and the inspected packaging files do not create one. Source-tree mypy success does not establish that downstream installed-package consumers receive typing support.

`python/docs/conf.py` imports `.pyi` files in preference to the compiled implementation. Consequently, the claim in `api.rst` that stubs are always synchronized with compiled extensions is not enforced. A successful Sphinx build can publish an incomplete API reference.

**Action:** add installed-wheel typing checks, marker/package-content checks, and runtime-versus-stub comparisons for public names, signatures, keyword-only parameters, defaults, return types and read-only properties. Treat `.pyx` runtime docstrings as the implementation contract and `.pyi` as its checked typing projection. Publish reference docs from the tested wheel; retain a stub-based fast preview only with parity guards.

### R6 — Exception, callback, and concurrency contracts need a common design (P1)

Most modules use `_common.pxd`'s typed exception dispatch; `_2d.pyx` instead raises bare `RuntimeError` with a numeric code. The shared helper uses the generic `swmm_error_message(code)`, losing handle-specific details where those are available. Some older `ModelBuilder` mutations return raw status codes while newer facades raise exceptions.

Callback trampolines in `_solver.pyx` call user Python code inside `noexcept with gil` functions without capturing exceptions for the initiating Python call. Decide and test propagation at a safe `step`/`stride`/`run` boundary; do not allow a controller failure to be silently lost. Abort timing and whether the current native step can finish must be explicit.

`step`, `stride`, and bulk operations release the GIL. That supports independent solvers, but does not by itself serialize mutation/destruction of the same solver. Existing concurrency tests concentrate on speedup and concurrent reads, not the whole ownership contract.

**Action:** define one per-handle operation/reentrancy policy, including callback-safe operations. Add deterministic correctness/progress tests; keep wall-clock speedup thresholds in a performance lane. Preserve older raw-status API behavior until a documented deprecation/migration can be made.

### R7 — Documentation examples contain lifecycle and mesh drift (P1)

`guide/initial_quality.rst` mutates initial-quality rows inside `with Solver(...)`. `Solver.__enter__` opens, initializes and starts the run, while the same guide and C API require BUILDING/OPENED for those mutations. That example cannot exercise the advertised editable state.

The `_model.pyx` module example uses the old numeric step-return convention and tests RUNNING immediately after start. The current binding returns `timedelta`, with STARTED preceding RUNNING. These examples need executable fixtures.

`guide/index.rst` still introduces the 2D router as a triangular CVODE solver. Current bindings expose the explicit marcher and mixed triangle/quad mesh helpers. `get_edge_geometry_bulk` allocates using `edge_stride` but its docstring still describes a fixed stride of three. The C header explicitly aliases triangle count to cell count; this review does **not** identify that count alias as a buffer-size defect.

**Action:** fix concepts and executable examples together. Specify lifecycle, physical units, array layout, padding, indexing, ownership, optional capability behavior, and save/reload semantics at each public entry point.

### R8 — Optional-module and compatibility testing can hide broken installations (P2)

The package catches any `ImportError` from optional modules and reports `HAS_2D=False` or `HAS_GEOPACKAGE=False`. Several tests skip on import failure. Distinguish an intentionally disabled feature from a wheel that was meant to contain it but has a loader/dependency failure.

A separate legacy symbol scan finds two solver exports not referenced in the legacy `.pyx`: `swmm_getRunningMassBalErr` and `swmm_setWarningCallback`; all 34 legacy output exports have references. This is not a signature or behavioral parity proof. Decide whether the two omissions are intentional, expose them if required, and keep legacy compatibility tests active. Documentation that calls the legacy engine “verbatim” or says no development happens there should reflect the actual maintained extensions.

## Work packages for all 118 missing functions

Names below are proposed Python entry points. Every package includes declarations, wrappers, public imports, type stubs, tests, code documentation, Sphinx reference entries, a guide example and a changelog entry.

| Package / C header | Missing | Proposed surface | Required behavioral tests and docs |
|---|---:|---|---|
| Groundwater: `openswmm_gw2d.h` | 26 | `solver.groundwater2d`: options, authored rows/node beds, state, species, ledgers | Global/tag/cell precedence; BUILDING/OPENED mutation; automatic node enrollment and exchange flags; state/column shapes; species order; continuity; project-unit authoring versus SI runtime results. New `groundwater2d.rst`. |
| Groundwater transport: `openswmm_gw_transport.h` | 29 | `solver.groundwater_transport`: options, parameters, sorption, sources/species, boundary/initial quality | CRUD/upsert/removal semantics; selectors; sidecars; INP round trips and supported container round trips; runtime rejection of authoring edits; pollutant/age/temperature semantics. New `groundwater_transport.rst`. |
| Surface quality: `openswmm_sq2d.h` | 14 | `solver.surface2d.quality`: coverage, curb lengths, loading, buildup | Row width versus land-use count, precedence, dry/wet cases, mixed-cell meshes, species/cell array order, nonzero buildup response. New `surface_quality.rst`. |
| Transport configuration: `openswmm_transport.h` | 13 | `solver.transport`: dispersion, target dx, conduit overrides, source/boundary rows | OFF/FISCHER/VALUE; zero/default handling; finite/nonnegative parameters; ft²/s versus m²/s; read-only row access; negative extraction rates. New `transport.rst`. |
| 2D diagnostics: `openswmm_2d.h` | 8 | Rainfall/volume arrays, interpolation weights, output-variable metadata | Scalar/bulk agreement, configured rainfall modes, empty arrays, cell order, weights, cumulative versus instantaneous units, output masks/names. Update `2d.rst`. |
| Engine diagnostics: `openswmm_engine.h` | 5 | `thread_info()`, `solver.effective_threads()`, `solver.transport_matrix` | OpenMP absent/present; auto/explicit requests; full 4×4 matrix, reasons and refresh after edits. Update solver guide and new capabilities section. |
| Forcing: `openswmm_forcing.h` | 5 | Per-element climate, node age/temperature, link seepage on `solver.forcing` | RESET/PERSIST; ADD/OVERRIDE; clear/fallback; signed values; project temperature units; isolation from global climate; effect after an actual step. Update forcing/heat/age guides. |
| Batch editing: `openswmm_edit.h` | 4 | Batch-delete methods on `solver.editor` | Duplicate/invalid selections, preflight/atomicity per C contract, cascade/restrict, order independence, stale views and identity remapping. Update editing guide. |
| Initial quality: `openswmm_initial_quality.h` | 3 | Sidecar path and per-row provenance on `initial_quality` | Original/resolved paths, set versus load-on-next-open behavior, file/inline rows, clear/save/reopen, missing file diagnostics. Update initial-quality guide. |
| Live output: `openswmm_output.h` | 3 | `OutputReader.open_live`, `is_live`, `refresh` | Zero complete periods, incremental flush, incomplete tail, finalization, repeated refresh, array snapshots and close semantics. Update output-reader guide. |
| Heat: `openswmm_heat.h` | 2 | Shortwave/cloud timeseries names | Missing/assigned series and save/reopen consistency. Update heat guide. |
| Process registry: `openswmm_process_components.h` | 2 | Known component vocabulary, separate from configured instances | Enumerate metadata; unknown IDs; registry versus model membership. Update process-components guide. |
| Timeseries: `openswmm_tables.h` | 2 | Relative-time metadata on a timeseries | Relative/absolute distinction, start-date changes, authored round trip. Update tables/datetime guides. |
| Link orientation: `openswmm_links.h` | 1 | Explicit authored-orientation restoration | Adverse-slope reversal, endpoints/offsets/vertices, idempotency and save/reopen. Update links/editing guides. |
| Compatibility writing: `openswmm_model.h` | 1 | `solver.write_compat(path, profile)` with `InpProfile` | FULL, legacy and stock profiles; warnings for dropped content; original model unchanged; stock-load compatibility; file failures. Update model-builder/migration guides. |
| **Total** | **118** | | |

Transport boundary/source rows are explicitly **read-only in the C API revision reviewed**. Their missing setters are an upstream API-design item, not something the Python wrapper should fake. Similarly, do not imply that adding wrappers implements a physical process absent from a selected transport backend. Expose the transport capability matrix and test meaningful supported configurations.

## Test design and code-documentation requirements

### Shared contract tests

Build reusable fixtures that cover:

1. **Lifecycle:** new/building/opened/initialized/started/running/ended/closed/destroyed, including reads versus authoring mutations and invalid handles.
2. **Ownership and identity:** retained views, deleted/renamed objects, structural generation changes, builder ownership transfer, garbage collection and callbacks holding references.
3. **Units and value semantics:** US/SI equivalents; seconds versus days/hours; project versus SI output quantities; concentration versus mass; signed age/temperature/extraction values; NaN/Inf and range validation.
4. **Array and ABI contracts:** exact dtype/shape/order; owned snapshots versus borrowed views; empty inputs, noncontiguous inputs, invalid sizes, undersized output buffers; platform C `long` width and integer overflow; triangle/quad padding. Compile C-derived enum/struct probes on each platform.
5. **Persistence:** author → write → reopen → compare both values and provenance. Cover relative paths, Unicode paths/identifiers, external sidecars, profiles and write failures. Apply GeoPackage/HDF5 expectations only where that serializer supports the feature; report unsupported behavior explicitly.
6. **Physical observables:** nonzero forcing changes the intended recipient, dry cells remain well-defined, conservative transfers balance within justified tolerances, and supported backend combinations agree on the wrapper contract. Use small analytical/reference fixtures; do not duplicate the entire numerical solver verification suite in Python.
7. **Failures:** correct exception class/code/context; unknown IDs; forbidden states; dangling references; callback exceptions; failed import of an expected native dependency.

Reuse relevant C++ fixture/test intent from `test_gwf_api.cpp`, `test_gw_transport_authoring.cpp`, `test_gw_transport_kernel.cpp`, `test_2d_surface_quality.cpp`, `test_transport_options_api.cpp`, `test_transport_policy.cpp`, `test_output_reader_live.cpp`, `test_2d_quad_mesh.cpp`, and `test_thread_info.cpp`. Give Python tests independent expected results rather than simply asserting that a call returns success. Every new binding must be exercised by at least one reachable public Python test; every mutable subsystem needs a round-trip test and every numerical subsystem an observable-effect test.

### Documentation contract for each public member

Record purpose and underlying C function; parameters and accepted enums; return type and shape; physical units and signs; zero-based API versus one-based file indexing; allowed lifecycle; ownership/copy behavior; mutation invalidation; threading/GIL restrictions; errors and warnings; optional build/runtime capability; persistence and path semantics; and a minimal usable example.

Use one consistent Sphinx-compatible docstring style for new work and normalize touched older epytext blocks. Do not invent documentation for units that the C implementation does not actually honor: resolve contradictory header/implementation contracts first.

### Sphinx and related deliverables

- Add the four new domain guides listed above and connect them through `guide/index.rst`, `api.rst`, and package/module maps.
- Add a units/lifecycle/capability reference shared by all guides; explain SI groundwater results alongside project-unit authoring and 2D conventions.
- Correct initial-quality and model-builder examples; execute guide examples against small checked-in fixture models in CI.
- Update quickstart, model editing, forcing, mass balance, live output, mixed meshes, compatibility migration and optional-installation troubleshooting.
- Update `.pyi`, `__init__.py`, `__init__.pyi`, `__all__`, any Cython re-export declarations and build module registration together.
- Add and verify the typing marker in built wheels; run downstream mypy from outside the source tree.
- Rebuild the parity inventory and retire stale claims/links; update README, CHANGELOG, package support tables and the application manual's Python chapter.
- Keep Sphinx `-W --keep-going`; inspect suppressed duplicate/reference warning categories and replace broad suppression with narrow documented exceptions where feasible. Add executable-example and periodic external-link checks. An HTML build alone is not a behavioral docs test.

## Ordered implementation strategy

| Phase | Work and dependencies | Completion gate |
|---|---|---|
| 0 — Reproducible baseline | Capture source hashes and feature configuration; reconcile the two audit tools and old parity matrix; map functions → Python entry points → tests → docs. Add a source-only contract job. | No newly introduced unexplained drift. The current 118 are explicit tracked implementation debt, not silently added to an intentional-exclusion list. |
| 1 — Safety and shared contracts | Fix 2D ownership/invalidation, callback exception capture, same-handle concurrency policy and diagnostic propagation. Synchronize enum members, known stub gaps and report semantics. Depends on baseline. | Regression tests fail on old behavior and pass on fixes; strict typing and public imports agree; retained views cannot dereference a destroyed engine. |
| 2 — Complete existing domain views | Compatibility writer, batch edits, initial-quality sidecars, relative timeseries, heat names, component registry, link orientation, live output and new forcing. | All corresponding gap rows closed with behavior/round-trip/error tests; guides and stubs ship in the same changes. |
| 3 — Transport and capability views | Bind the 13 transport-config functions and 5 engine diagnostics; introduce typed records and constants. | Matrix matches configured engine behavior; read-only limits documented; US/SI and configuration round-trip tests pass. |
| 4 — Groundwater | Bind 26 groundwater and 29 groundwater-transport functions. Split authoring and runtime/results into reviewable changes. Reuse Phase 1 ownership and Phase 3 capability contracts. | Explicit 55-function closure; state and species array tests; authoring round trips; meaningful water/species ledger checks; no false capability claims. |
| 5 — Surface quality and remaining 2D | Bind 14 surface-quality functions and 8 2D diagnostics; finalize mixed-mesh examples and naming. | Explicit 22-function closure; nonzero quality/rainfall fixtures, weights/masks, shape/stride and scalar/bulk parity tests. |
| 6 — Release qualification | Clean wheel builds; installed-package typing and examples; full modern, legacy and top-level test lanes; optional-feature build matrix; Sphinx from tested artifacts. Documentation is developed throughout, not postponed to this phase. | Zero unexplained binding gaps; correct intentionally unsupported inventory; supported wheel matrix green; reproducible docs and no unreviewed missing-feature skips. |

Use one focused PR per domain or safety contract, with code, tests, stubs and documentation together. Avoid a single large wrapper-generation change that obscures ownership, units, or physical meaning. Preserve established Python names and calling conventions; add aliases/deprecations where API consistency requires a migration. Assign owners and estimates after the shared safety contract and upstream-only requirements are agreed.

## Definition of done

- Every modern public C function is classified as supported, intentionally replaced by a Python idiom, intentionally native-only, or explicitly deferred with an owner and issue. Release qualification has no unexplained or silently allowlisted backlog.
- Supported functions have consistent declarations, callable public wrappers, verified enum/struct/array contracts, stubs, exports, meaningful tests and current reference documentation.
- New domains include runnable examples, unit/lifecycle tables, persistence behavior and capability limitations.
- Source-contract, runtime, installed-wheel typing, documentation-example and supported-platform gates run on the branches and tags that actually release the package.
- Legacy compatibility remains tested, and proposed breaking changes carry a migration path.
- Validation results distinguish static checks, syntax compilation, linking, runtime tests and cross-platform results; none stands in for another.


## Appendix A — Complete missing-function inventory

Generated from the existing source-level API coverage guard. These functions occur in public engine headers but have no recognized `.pyx` use and are not in its intentional-exclusion list.

### `openswmm_2d.h` (8)

- `swmm_2d_get_coupling_volume_bulk`
- `swmm_2d_get_rain_volume_bulk`
- `swmm_2d_get_rainfall_bulk`
- `swmm_2d_get_rainfall_weights`
- `swmm_2d_output_variable_count`
- `swmm_2d_output_variable_mask`
- `swmm_2d_output_variable_name`
- `swmm_2d_output_variable_text`

### `openswmm_edit.h` (4)

- `swmm_gage_delete_many`
- `swmm_link_delete_many`
- `swmm_node_delete_many`
- `swmm_subcatch_delete_many`

### `openswmm_engine.h` (5)

- `swmm_get_effective_threads`
- `swmm_get_thread_info`
- `swmm_get_transport_matrix`
- `swmm_transport_class_name`
- `swmm_transport_domain_name`

### `openswmm_forcing.h` (5)

- `swmm_forcing_element_climate`
- `swmm_forcing_element_climate_get`
- `swmm_forcing_link_seepage`
- `swmm_forcing_node_age`
- `swmm_forcing_node_temperature`

### `openswmm_gw2d.h` (26)

- `swmm_gw2d_get_cell`
- `swmm_gw2d_get_cell_bulk`
- `swmm_gw2d_get_cell_conc`
- `swmm_gw2d_get_column`
- `swmm_gw2d_get_continuity_error`
- `swmm_gw2d_get_dimensions`
- `swmm_gw2d_get_ledger`
- `swmm_gw2d_get_species_ledger`
- `swmm_gw2d_get_tier_histogram`
- `swmm_gw2d_is_active`
- `swmm_gw2d_node_add`
- `swmm_gw2d_node_count`
- `swmm_gw2d_node_get`
- `swmm_gw2d_node_get_flags`
- `swmm_gw2d_node_remove`
- `swmm_gw2d_node_set_exchange`
- `swmm_gw2d_option_get`
- `swmm_gw2d_option_set`
- `swmm_gw2d_row_add`
- `swmm_gw2d_row_count`
- `swmm_gw2d_row_get`
- `swmm_gw2d_row_get_property`
- `swmm_gw2d_row_remove`
- `swmm_gw2d_row_set_property`
- `swmm_gw2d_species_count`
- `swmm_gw2d_species_name`

### `openswmm_gw_transport.h` (29)

- `swmm_gw_boundary_quality_count`
- `swmm_gw_boundary_quality_get`
- `swmm_gw_boundary_quality_remove`
- `swmm_gw_boundary_quality_set`
- `swmm_gw_init_quality_count`
- `swmm_gw_init_quality_file_get`
- `swmm_gw_init_quality_file_set`
- `swmm_gw_init_quality_get`
- `swmm_gw_init_quality_remove`
- `swmm_gw_init_quality_set`
- `swmm_gw_params_count`
- `swmm_gw_params_get`
- `swmm_gw_params_remove`
- `swmm_gw_params_set`
- `swmm_gw_sorption_count`
- `swmm_gw_sorption_get`
- `swmm_gw_sorption_remove`
- `swmm_gw_sorption_set`
- `swmm_gw_source_count`
- `swmm_gw_source_get`
- `swmm_gw_source_remove`
- `swmm_gw_source_set`
- `swmm_gw_source_species_count`
- `swmm_gw_source_species_get`
- `swmm_gw_source_species_remove`
- `swmm_gw_source_species_set`
- `swmm_gw_transport_authored`
- `swmm_gw_transport_option_get`
- `swmm_gw_transport_option_set`

### `openswmm_heat.h` (2)

- `swmm_heat_get_cloud_timeseries`
- `swmm_heat_get_shortwave_timeseries`

### `openswmm_initial_quality.h` (3)

- `swmm_init_quality_file_get`
- `swmm_init_quality_file_set`
- `swmm_init_quality_is_file`

### `openswmm_links.h` (1)

- `swmm_links_restore_authored_orientation`

### `openswmm_model.h` (1)

- `swmm_model_write_compat`

### `openswmm_output.h` (3)

- `swmm_output_is_live`
- `swmm_output_open_live`
- `swmm_output_refresh`

### `openswmm_process_components.h` (2)

- `swmm_process_component_known_count`
- `swmm_process_component_known_get`

### `openswmm_sq2d.h` (14)

- `swmm_2d_coverage_count`
- `swmm_2d_coverage_get`
- `swmm_2d_coverage_remove`
- `swmm_2d_coverage_row_size`
- `swmm_2d_coverage_set`
- `swmm_2d_curb_length_count`
- `swmm_2d_curb_length_get`
- `swmm_2d_curb_length_remove`
- `swmm_2d_curb_length_set`
- `swmm_2d_get_buildup_bulk`
- `swmm_2d_loading_count`
- `swmm_2d_loading_get`
- `swmm_2d_loading_remove`
- `swmm_2d_loading_set`

### `openswmm_tables.h` (2)

- `swmm_timeseries_get_relative_info`
- `swmm_timeseries_set_relative_info`

### `openswmm_transport.h` (13)

- `swmm_transport_boundary_count`
- `swmm_transport_conduit_disp_count`
- `swmm_transport_get_boundary`
- `swmm_transport_get_conduit_disp`
- `swmm_transport_get_configured`
- `swmm_transport_get_dispersion_mode`
- `swmm_transport_get_dispersion_value`
- `swmm_transport_get_source`
- `swmm_transport_get_target_dx`
- `swmm_transport_set_dispersion_mode`
- `swmm_transport_set_dispersion_value`
- `swmm_transport_set_target_dx`
- `swmm_transport_source_count`
