# Standalone multi-column series-file harness

Sandbox verification harness for
`plans/MULTICOLUMN_SERIES_SINGLE_READ_2026-08-17.md` /
`plans/HANDOFF_MULTICOLUMN_SERIES_ENGINE_2026-08-17.md` (Tier-2 fallback:
the sandbox cannot fetch GoogleTest, so the gtest targets in
`tests/unit/engine/` cannot be linked there — this harness runs the same
assertions without gtest). It is NOT registered with CMake/CTest; the
authoritative tests are the gtest files:

- `tests/unit/engine/test_multicolumn_series_file.cpp`
- `tests/unit/engine/test_gage_rain_series.cpp` (extended)
- `tests/unit/engine/test_timeseries_file_roundtrip.cpp` (extended)
- `tests/unit/engine/test_gage_format_preservation.cpp`

## main.cpp — parser-level (no engine library needed)

Compiles `src/engine/input/MultiColumnSeriesFile.cpp` alone and exercises:
wide-row CSV (>4096-byte rows/header), TSF AM/PM datetimes, delimiter
sniffing, the cache's parse-once guarantee, first-data-column default,
malformed-row accounting, and failure statuses.

Binaries and run artifacts go to `tests/standalone_multicolumn/output/`
(gitignored, in-repo and reviewable per CLAUDE.md §4.1). Nothing is written to
`/tmp`, and nothing is written into `tests/unit/engine/data/`, which holds
checked-in `.rpt`/`.out` fixtures a harness run must not be able to clobber.

```sh
cd <repo-root>
g++ -std=c++20 -Isrc/engine -o tests/standalone_multicolumn/output/mcsf_parser \
    tests/standalone_multicolumn/main.cpp \
    src/engine/input/MultiColumnSeriesFile.cpp
(cd tests/unit/engine/data && ../../../standalone_multicolumn/output/mcsf_parser)
```

## engine_capi_main.cpp — end-to-end through the built engine

Links the built `libopenswmm.engine` and opens the fixtures in
`tests/unit/engine/data/rain_series/` through the public C API: single-read
assertion (N gages + M timeseries on one file ⇒ one parse), B1 format
preservation, B2 empty-column default, TSF gage loading, `[TIMESERIES]`
column selection, and the loud open failures for missing/zero-row files.

```sh
cd <repo-root>
cmake -S . -B <builddir> -G Ninja -DOPENSWMM_BUILD_2D=OFF \
      -DOPENSWMM_BUILD_GPU_PLUGIN=OFF -DOPENSWMM_WITH_GEOPACKAGE=OFF
cmake --build <builddir> --target openswmm_engine
g++ -std=c++20 -Iinclude -Isrc/engine -o tests/standalone_multicolumn/output/mcsf_capi \
    tests/standalone_multicolumn/engine_capi_main.cpp \
    -L<builddir>/src/engine -lopenswmm.engine \
    -Wl,-rpath,<builddir>/src/engine
(cd tests/unit/engine/data && ../../../standalone_multicolumn/output/mcsf_capi)
```

The C-API harness writes its `.rpt`/`.out` files to `output/` itself (see
`art()` in `engine_capi_main.cpp`).

## fixture_open_main.cpp — the dangling-fixture acceptance check

`refactored_small.inp` and `legacy_small.inp` reference two `.dat` files whose
real data is not distributable; the placeholders now committed under
`tests/unit/engine/data/Rich_BC_CSO_FinalPlan_CRST_design_2A_common_files/`
(see the README there) are what lets them open under the stricter
missing-file error. This harness asserts both halves: the strict open
succeeds, **and** every gage series is still all zeros, so the ~18 python
engine tests that use these fixtures see the same numbers as before.

```sh
g++ -std=c++20 -Iinclude -Isrc/engine -o tests/standalone_multicolumn/output/fixture_open \
    tests/standalone_multicolumn/fixture_open_main.cpp \
    -L<builddir>/src/engine -lopenswmm.engine \
    -Wl,-rpath,<builddir>/src/engine
(cd tests/unit/engine/data && ../../../standalone_multicolumn/output/fixture_open)
```

All three binaries exit 0 on success and print one line per check.
