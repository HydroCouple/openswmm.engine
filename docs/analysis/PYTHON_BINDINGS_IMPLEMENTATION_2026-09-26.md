# Python bindings implementation and validation

Date: 2026-09-26. Repository: `openswmm.engine`.

The approved review's binding gaps are implemented and locally validated using
the **Conda `openswmm` environment, Python 3.12.13**. The original review and its
118-function inventory remain the historical baseline. This report records the
resulting code and separates local evidence from release checks still pending.

## Outcome

| Measure | Result |
|---|---:|
| Current modern native exports | 1,173 |
| Recognized executable binding uses | 1,170 |
| Documented compatibility exclusions | 3 |
| Unexplained binding gaps | **0** |
| Original missing exports implemented | 118 |
| Additional staged-writer export implemented during this work | 1 |
| Installed-wheel tests | **1,221 passed** |
| Minimal-build tests, optional native features disabled | **37 passed** |
| Standalone source-contract tests | **34 passed** |
| Engine type-checking | **40 source files passed** |
| Strict consumer typing, source and installed-wheel runs | **passed** |
| Sphinx HTML with warnings treated as errors | **passed** |

The exclusions are `swmm_get_current_time`, `swmm_get_last_error` and
`swmm_get_last_error_msg`: Python provides current time and typed exceptions.
`swmm_error_message` is recognized through its executable `.pxd` helper.
Reachability verifies that a wrapper calls an export; it is not numerical
validation of every native process.

## Delivered domains and regression coverage

| Area | Python surface | Principal evidence |
|---|---|---|
| Aquifer hydrology | `surface2d.groundwater`: rows, options, nodes, scalar/bulk state, sigma columns, ledgers and species | `test_groundwater.py`: mixed cells, authoring units, save/reopen, inactive/stale states, array ownership, water residual |
| Groundwater transport | `.groundwater.transport`: parameters, sorption, initial quality, boundaries, sources and source-species terms | `test_new_domain_tables.py`: all table round trips/removals, quad edge 3, sidecar reference, species snapshots/ledgers |
| Surface quality | `surface2d.quality`: coverage, initial loading, curb length, runtime buildup | `test_new_domain_tables.py`: scope precedence with nonzero buildup, bad coverage, round trip, independent arrays |
| ARD transport and capability | `solver.transport`, `transport_matrix`, thread diagnostics and typed records | `test_transport.py`: invalid coefficients, matrix axes, thread limits, US/SI persistence with registered component file |
| Existing metadata | Initial-quality file/is-file, heat series names, component registry, relative time metadata, authored link orientation, compatibility writer | `test_metadata_bindings.py`: round trips and all compatibility profiles |
| Editing, output and forcing | Batch deletions, live output refresh, local climate/temperature/age/seepage forcing, staged serialization | `test_binding_completion.py`: atomic invalid batches, duplicates, partial output records/footer adoption, callback failures and reentry rejection |
| 2D diagnostics | Rainfall, accumulated rain/coupling volumes, weights and output selections | Nonzero mixed triangle/quad rainfall fixtures in both US and SI units; scalar/bulk parity and volume increments |
| Safety | Owner retention, invalidation, callback error capture, per-owner access guards, file-reader guards | `test_native_safety.py`: destroy/GC/transfer, callbacks, independent and conflicting access, subprocess use-after-destroy regression |
| Packaging and typing | `py.typed`, public exports, new stubs and optional import behavior | Installed marker/source checks, stub/runtime signature checks, strict consumer typing outside the repository |
| Reports | Current routing diagnostics, percentage conversion, snow/forcing/coupling/groundwater totals | Snapshot values compared with the native-backed mass-balance view after a completed run |

The GeoPackage bulk writer now validates matching lengths, copies strided
arrays, handles empty writes and refuses reads after close. Native series reads
pin their file handles against concurrent close or refresh. The 2D/editor views
retain the Python engine owner rather than retaining an unowned native address.
Callbacks cannot unwind through the C ABI; the first exception is re-raised
when native execution returns.

## Documentation and CI

Sphinx now includes guides for native-call safety, transport capabilities,
groundwater/transport, surface quality, batch edits, live output and staged
serialization. They state lifecycle, indices, units, snapshot ownership and
persistence rules. Existing metadata and mass-balance guides, type stubs,
README, changelog and the application manual's Python chapter were updated.

Two details established by tests are explicit in the documentation:

- ARD persistence requires a registered process component and configuration-file
  path. Setting a dispersion value alone does not add that registration.
- Native continuity-error accessors return fractions; report fields named
  `continuity_error_pct` now convert to percentages. Report diagnostics use the
  current typed accessor instead of silently probing retired method names.

CI has a reusable source-contract gate and an actual optional-feature-disabled
build. The gate is a release dependency. Wheel testing includes modern, legacy,
top-level and integration tests; only typing fixtures are excluded from runtime
collection. Typing triggers include Cython and native headers, and the Python
workflows include `swmm6_rel`. The old fuzzy parity report is marked historical;
the regenerated provenance report shares the strict tokenizer and counts
1,173 exports. MCP-only exclusions do not exempt Python bindings.

The minimal build exposed native 2D references in core routing and reports that
prevented compilation/linking with 2D disabled. Feature guards fixed those
build failures without changing the enabled-feature behavior.

## Reproducible local validation

Interpreter: `/Users/calebbuahin/miniforge3/envs/openswmm/bin/python`.
NumPy 2.4.4, Cython 3.2.4 and pytest 9.0.2. Additional mypy/Sphinx dependencies
were loaded from a temporary directory; the Conda installation was not replaced.

The final local wheel was built with 2D, GeoPackage, HDF5 and the OpenMP GPU
plugin enabled, and **both experimental fast-kernel switches disabled** to use
the release-default numerical behavior. It was installed with `pip --no-deps`
into `/tmp/openswmm-conda-wheel-installed`, then tested with the Conda interpreter.
All 62 Python source, stub and typing-marker files matched the built wheel.

```sh
conda run -n openswmm python python/scripts/test_staged_package.py \
  /tmp/openswmm-conda-wheel-installed python/tests --ignore=python/tests/typing -q
```

`test_staged_package.py` removes only the OpenSWMM editable redirector for that
process and asserts that imported package modules come from the requested
prefix. This matters because the environment's editable install otherwise
selects older extensions despite `PYTHONPATH`. No installed environment files
are changed by this test runner.

For a plain CMake install, `python/scripts/stage_python_sources.py PREFIX`
copies the Python modules/stubs that scikit-build adds to wheels. The minimal
build disables 2D, GPU, GeoPackage and HDF5 and exercises omitted-feature imports,
core solver lifecycle and transport configuration.

The companion JSON preserves input hashes, audit output, wheel hash, feature
settings and complete final runtime test summaries.

## Remaining release qualification

Local implementation and macOS arm64 validation are complete. The following
remain external release checks, not claimed successes:

- Run the updated GitHub Linux/Windows/macOS and Python-version wheel matrix.
- Validate portable dependency repair and minimum-OS tags. The locally built
  `cp312-macosx_26_0_arm64` wheel is a local test artifact, not a published wheel.
- Run sanitizer builds where supported. The local safety regressions include a
  subprocess test, but no ASan/TSan run was performed.
- Validate external documentation links/intersphinx inventories. The successful
  Sphinx run disabled external inventories and retained the repository's
  existing warning suppression categories.

The earlier CI fix remains commit `4895765e`. The implementation commit includes the
bindings, documentation, contract gates and their native writer/build dependencies.
The validation above ran in the working tree alongside pre-existing work in
progress; the companion hashes identify the tested inputs. Unrelated changes
remain outside this commit.
