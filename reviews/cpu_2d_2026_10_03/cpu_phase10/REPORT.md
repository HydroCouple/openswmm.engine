# Phase 10: CPU face-kernel overhead and coupled-model qualification

## Outcome

Retain direct access to face geometry and force inlining of the SWE kernel chain in the CPU solver translation unit. The production change is confined to `ExplicitInertialSolver.cpp`. **Implemented and validated.** There are no new buffers, runtime settings, or changes to the numerical formulas. GPU performance was not tested.

All **140 analytical executions (70 paired comparisons)** produced exactly identical endpoint cell depth, volume and momentum, sampled histories, error metrics and step counts. All **24 complete-model executions (12 paired comparisons)** produced identical final state/ledger values, reports after removing wall-clock timestamps, and HDF5 dataset contents. The existing suite passed **169 tests with one skip**.

## What changed and why

Fresh baseline: `5c3b10bc97cf341a8b0516f0556d8bc1748086bc` plus the recorded working-tree hashes in `baseline_manifest.json`. The pre-existing rain-activation edit was held identical on both sides and remains untouched.

Three fresh instrumented bowl runs put approximately 47–48% of advance time in face evaluation excluding gradients, 19–24% in gradients, and 16–20% in cell updates. Instrumentation was removed from all speed measurements.

1. Each second-order face previously searched both incident cells' CSR rows for its cell-to-face arms. The replacement computes the same midpoint-minus-centroid values directly from existing geometry arrays. This is the identical expression used when the CSR arms are built, with no additional persistent storage.
2. The compiler retained separate calls to `faceFluxReconBed`, `faceFlux` and `hllcFlux` in the baseline's hottest loop. A local annotation override inlines the kernel chain in this CPU translation unit, then restores the existing annotation. Symbol inspection confirms those out-of-line kernel bodies disappear from the selected component object. Shared headers and other backends are unchanged.

A velocity-cache prototype was rejected. It added a per-stage pass and extra storage, and its gains were inconsistent; the four-thread screen regressed. The retained change includes none of that cache.

## Repeated component timings

One discarded warm-up pair, then five randomized baseline/optimized pairs per workload. CPU columns are medians; reduction is the median of paired CPU ratios, so it need not equal the ratio of the displayed medians. Negative reductions mean a slowdown. Native Apple ARM64, Clang, double precision. Component flags include `-O3 -fno-fast-math -ffp-contract=off -mcpu=native`. CPU timings use child-process user+system time, including setup and final error evaluation; advance-only wall times are also recorded. Host load is recorded for every confirmation run. Other machine activity remains a source of variability; these are local measurements, not cross-platform guarantees.

| Case | Baseline CPU (s) | Optimized CPU (s) | CPU reduction | Paired reduction range |
|---|---:|---:|---:|---:|
| radial 64, quads, order 2, 1 thread(s) | 0.316 | 0.268 | 13.8% | 9.9–22.1% |
| planar 64, quads, order 2, 1 thread(s) | 1.282 | 1.090 | 13.5% | 11.9–16.3% |
| planar 64, triangles, order 2, 1 thread(s) | 3.347 | 2.966 | 11.7% | 4.8–14.2% |
| planar 128, quads, order 2, 1 thread(s) | 8.288 | 7.299 | 12.0% | 8.8–15.6% |
| planar 64, quads, order 2, 4 thread(s) | 1.796 | 1.535 | 11.1% | 6.2–24.2% |
| planar 64, quads, order 1, 1 thread(s) | 0.343 | 0.291 | 15.1% | 12.0–15.8% |
| ritter 256, triangles, order 2, 1 thread(s) | 0.140 | 0.124 | 11.6% | 10.2–16.0% |

## Repeated complete-model timings

One discarded warm-up pair, then three randomized pairs per case. This measures the complete engine CPU time, including initialization and output. The larger surface model has 32,768 initially wet triangular cells, rain, infiltration, a drain/pipe connection, and HDF5 output. It is an authored qualification workload, not a calibrated catchment.

| Case | Baseline CPU (s) | Optimized CPU (s) | CPU reduction | Paired reduction range |
|---|---:|---:|---:|---:|
| bellinge_10min_swe2 | 0.681 | 0.644 | 5.4% | 4.5–7.2% |
| parking_lot_swe2_flat | 0.370 | 0.327 | 12.4% | 11.6–13.8% |
| surface_wet_swe2_t1 | 28.335 | 23.446 | 19.0% | 10.8–19.4% |
| surface_wet_swe2_t4 | 33.064 | 28.963 | 12.4% | 5.9–19.0% |

Bellinge uses a fresh ten-minute interval (2012-06-29 04:15–04:25) with FULL_SWE, second order, one LTS tier, VFR storage, CPU backend and CFL 0.4. The external mesh and rainfall inputs were frozen and hashed before running. The historical source filename says “116k”; the mesh actually loaded here has **25,600 cells**, as verified through the engine API. See `bellinge_provenance.json`. That first interval is dry (zero surface water ledger), so its timing is a dry-control result. An additional cold-start storm interval, 06:13–06:23, covers the largest rolling ten-minute gauge-5425 rainfall total that day (9 mm); see `bellinge_storm_provenance.json`. Both intervals are qualified for numerical equivalence. The wet interval advances 2,728 internal steps and 104,574,000 face evaluations, with a mean active-cell fraction of 99.5% and nonzero rain and coupling ledgers. The storm pair is not used to claim a repeated performance gain. End-to-end timing includes substantial work outside the optimized face kernels.

## Correctness evidence

- Analytical matrix: radial and planar Thacker bowls; triangular, quadrilateral and mixed meshes; regular and perturbed vertices; first and second order; three- and ten-period runs; one and four threads; fully wet and emerged-bump lake-at-rest cases; Ritter and Stoker dam breaks. Every baseline/optimized pair is exact. One-/four-thread comparisons are exact too. Maximum absolute relative water-volume error: **5.48e-15**.
- Existing regression suites: 170 tests, 1 skip, no failures. The single skip is the unavailable Kokkos OpenMP comparison plugin. These include wet-face mass flux, pressure-source consistency, distorted shoreline velocity, conservation, local timestepping, coupling and transport.
- Complete models: original parking lot, two-way groundwater, Dunne/species-9, and surface/species-32 controls; parking-lot FULL_SWE order 2 with FLAT and VFR storage; wet 32,768-cell order-2 surface runs at one/four threads; groundwater FULL_SWE order 1 with FLAT/VFR; and both dry and storm Bellinge order-2 intervals. Final arrays/ledgers and normalized reports match, and native `h5diff` reports identical contents for all output-enabled cases.
- Model telemetry verifies FULL_SWE, one tier and positive face-evaluation counts for every second-order fixture. The dry two-way-groundwater control has zero active surface faces; it exercises groundwater coupling rather than surface performance.
- **Existing feature limits remain:** second-order RK2 rejects groundwater coupling; both builds reject both tested storage closures with the same diagnostic (four expected-rejection executions). Overland transport falls back to first order, so species-enabled controls are not claimed as second-order qualification. Water-only fixtures exercise the optimized second-order path.

Full-engine variants use the same frozen, internally consistent support-object/header set from the earlier review, with the current 2D solver compiled separately on each side. This avoids mixing unrelated concurrent 1D changes or incompatible object layouts. Full-engine compiler settings are recorded in `engine_commands_*.json`. No GPU or other CPU architecture was tested.

## Evidence and reproduction

`profiles.json`, `screen.json`, `inline_screen.json`, `performance_results.json`, `performance_summary.json`, `model_timings.json`, `model_timing_summary.json`, `analytical_results.json`, `regressions_selected.json`, `model_results.json`, `bellinge_results.json`, `unsupported_groundwater.json`, `retained.patch`, and `final_verification.json` preserve decisions and results. Review-local scripts build isolated variants, run the analytical/engine checks and collect timings. Generated binaries, full frozen source trees, per-cell dumps, model outputs and external-input snapshots remain ignored local artifacts. Existing baseline hashes and provenance identify their inputs.

The retained patch removes measurable CPU overhead while preserving the accuracy improvements from phase 9. Further work should follow the remaining measured costs—gradient reconstruction and cell updates—and any additional representative catchment evidence, rather than changing numerical tolerances to obtain speed.
