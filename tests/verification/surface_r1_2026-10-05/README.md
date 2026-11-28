# R1 / P0a acceptance evidence

Baseline: engine commit 85ebe251 archived into `isolated/source`, compiled in Release, numerical accelerators OFF, LTO ON, CPU 2D, HDF5 and GeoPackage ON. Only `isolated/r1-p0a.patch` was then applied and rebuilt. Staged baseline/p0a CLIs load their own libraries. Provenance records engine/patch hashes and the corpus confirms matching numerical build options.

`isolated/candidate_tests.log`: six suites pass; existing integration test requiring an unavailable backend remains skipped. `isolated/corpus.log`: 25 registered `.out` files byte-identical; separate mesh census 7 identical, 3 non-running fixtures skipped on both engines. `api_census/summary.json`: six methods × US/SI, each under common wet–dry–wet external mesh rainfall. Every recorded double is byte-identical: elapsed days, surface depth, held capacity, applied cumulative infiltration and total infiltration volume. This is a finite fixture set, not every possible project. The harness keeps output in this directory and loads only one engine per subprocess.

Earlier working-source baseline/refactor logs outside `isolated/` are superseded: concurrent outfall work invalidated that source comparison. GUI validation uses the shared working tree; its task-only patch, hashes, logs and screenshot are saved in `openswmm.gui/tests/gui/data/surface_r1_out/` and `surface_r1_gui_tests_final.log`.

P0b attribution uses the independently staged `isolated/p0a` engine. P0b intentional changes are recorded separately; the legacy zero-Horton-drying convention stays unchanged. Aquifer-owned capacities and UEB are later rounds.
