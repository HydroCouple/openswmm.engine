# 2D rainfall: input formats, integration and saved output

## Scope

Audit the current engine's gage-to-cell rainfall conversion and its actual HDF5 output, independently of the GUI plot renderer. The affected project was not supplied; this is controlled model verification, not a diagnosis of that project's individual dry cells.

## Input and output contracts

- `INTENSITY` records are rates in the project's rain units: in/hr for US flow units, mm/hr for SI flow units.
- `VOLUME` records are **rainfall depths per gage recording interval** (inches or millimetres), not cubic metres. Conversion is `depth / interval_seconds * 3600`.
- `CUMULATIVE` records are cumulative depths. Consecutive differences give interval depths; a decreasing counter starts a new accumulation and the new reading is that interval's depth. Differencing happens before spatial interpolation.
- Each gage uses its own recording interval. Its converted rate is constant over that interval; gaps are dry. The runoff clock books changes, and each 2D synchronization window receives the integral of those rates divided by the window duration. The solver's smaller internal steps use that window mean.
- Spatial weights map gage values to each cell. Natural neighbour uses its existing inverse-distance fallback outside the gage hull. Nearest neighbour selects the closest located gage, including when its rainfall is zero.
- `Mesh2_face_rainfall` stores a sampled rate in **m/s**, not a report-period average. Multiply by 3,600,000 for mm/hr.
- `Mesh2_face_rain_cum` stores cumulative **volume in m³ per cell**. Divide by `Mesh2_face_area` for depth in metres; multiply by 1,000 for millimetres. Differences between cumulative records give the volume over a reporting interval.

For cell c, area A and static spatial weights w, the expected accumulated volume is:

`V_c(t) = A_c * sum_g(w_cg * integral(start, t, rain_g(t) dt))`.

A storm entirely between report instants can leave every sampled rate zero while cumulative volume increases correctly. Also, the last scheduled frame can precede simulation end: the final `mass_balance_2d/rainfall_in` includes the trailing interval, while that last frame stops at its own reporting time. `swmm_engine_end()` flushes a final partial 2D batch before writing final mass balance.

## Confirmed defects and correction

The old saved cumulative volume came directly from the completed 2D window, even when that window ended after the frame timestamp. A controlled 7-second routing case reported the accumulated volume through 21 seconds at the 17-second frame, 23.53% too high at that early frame. Across 36 cases / 1,080 cell records, maximum absolute saved-volume error was 0.0012072649572649578 m³. Final domain rainfall totals were already correct: maximum error against the independent input integral was 9.03e-17 m³.

The correction preserves the gage integral at a report boundary before the runoff clock passes it, then maps only the portion up to that boundary onto the cells. Added and overridden cell rainfall is included over the corresponding portion of the window. Runtime source rates, integration, and mass-balance totals are unchanged. Storage is bounded to one additional gage row and one cell row; the extra spatial mapping occurs at report cadence. This reconstructs source accumulation at the requested time: the hydraulic solver still applies a window-mean source, and reported depth/head still use linear state interpolation. Inside a window crossing a rain change, those interpolated hydraulic states need not imply exactly the same partial volume as the piecewise gage integral. Completed-window and final rainfall conservation are unchanged.

The saved rate also used the runoff cursor's current rain record. If a routing step crossed a report instant and then a rain-record change, the cursor could already point beyond the requested report time. The 2D report path now looks up the source interval directly, reusing the existing format conversion, scaling, co-gage and monthly-adjustment handling. The 1D legacy report path is preserved. Both paths retain SWMM's +1 second report-gage lookup convention, so a rate within one second of a boundary can show the next interval; cumulative volume integrates to the nominal report boundary without this look-ahead.

The HDF5 `time` coordinate contained absolute SWMM dates but was labelled `days since simulation start`. The label is corrected to `days since 1899-12-30 00:00:00`. Numeric timestamps are unchanged and retain SWMM's 1 ms report-date offset. The GUI already interprets these numbers as absolute SWMM dates.

## Regression matrix

`Output2DWriter.RainFormatsAndMismatchedClocks` writes and reopens real output files. Its expected values come from independently integrated depth records, not the production conversion or integration functions.

The matrix combines both interpolation modes, both unit systems and all three rain formats with these clock configurations:

| Routing maximum (s) | 2D maximum substep (s) | Base report interval (s) | Requested runoff interval (s) | Start offset (s) | Additional coverage |
|---:|---:|---:|---:|---:|---|
| 7 | 2 | 17 | 37 | 0 | Unaligned report and rain boundaries |
| 31 | 7 | 60 | 120 | 17 | Partial first rain interval and final simulation interval |
| 2.5 | 0.7 | 19 | 13 | 0 | Fractional routing and solver timesteps |
| 7 | 2 | 17 | 37 | 0 | 29 s synchronization, report start at 68 s, 34 s 2D reports, FLOAT32, persistent added rainfall |
| 31 | 7 | 53 | 120 | 17 | Routing step crosses a rain change after the report instant |

Three simultaneously active gages have 60-, 120- and 180-second recording intervals, distinct wet/dry patterns and cumulative counter resets. The engine correctly reduces a requested runoff step longer than the shortest recording interval. Other cases use FLOAT64. All authored report intervals exceed the routing maximum; sub-routing-step reporting is outside this matrix.

The assertions cover each saved rate, each saved cumulative cell volume, frame count, timestamps, unit attributes, final rainfall mass balance and consistency of the live cell-volume sum with the live ledger. Existing pulse, dry-cell, spatial interpolation and gage-file tests provide complementary coverage.

## Validation artifacts

Reviewable generated models, reports and HDF5 files: `tests/output/2d_output_options/rain_clocks_*`.

Audit logs and the independent Python/HDF5 checker: `tests/output/rainfall_temporal_audit_2026-09-30/`.

## Results

- All **60 model runs / 1,440 saved cell records** passed the analytic comparisons.
- All **45 tests** passed across `test_engine_gage_rain_series` (10), `test_engine_2d_rainfall_interp` (19) and `test_engine_2d_output_options` (16). The between-report pulse test now requires zero cumulative rain at the pulse's start instead of allowing an entire routing step's premature rainfall.
- The independent Python/HDF5 audit found a maximum FLOAT64 cumulative-volume error of **3.13e-17 m³** and maximum rate error of **1.70e-21 m/s**. For FLOAT32 these were **8.91e-10 m³** and **1.08e-13 m/s**, consistent with storage rounding.
- Maximum final domain rainfall error was **9.03e-17 m³**. Final rainfall totals on the original 36 baseline cases changed by **exactly zero** after the output corrections.
- All 60 files have the corrected time-axis units. Both US and SI models retain SI rainfall output units.

Validation used the rebuilt Release engine and the CPU explicit 2D marcher with one local-timestep tier. These changes are in the engine working tree; no GUI package was rebuilt or installed during this audit. Existing results need a simulation rerun with the updated engine to obtain corrected saved values. The input matrix uses model time series; external rain-file loading is covered separately by the passing gage-series suite, rather than every external file format being crossed with every clock configuration.

Reproduce the automated matrix and regression checks:

```sh
cmake --build build/darwin --target test_engine_2d_output_options test_engine_2d_rainfall_interp test_engine_gage_rain_series -j6
ctest --test-dir build/darwin -R '^test_engine_(2d_output_options|2d_rainfall_interp|gage_rain_series)$' --output-on-failure
```

The independent file-audit summaries are `before.json` and `after.json`; the test logs are `before.log`, `after.log`, and `regression.log` in the audit artifact directory above.
