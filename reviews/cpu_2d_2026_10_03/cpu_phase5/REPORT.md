# CPU phase 5: combine species gathering and source updates

## Delivered

The CPU solver now applies a species row's source terms immediately after gathering its pending face transfers when a cell transports at least eight species. This eliminates a second pass over that cell's species masses. Smaller species counts keep separate gather and source passes. Source-budget preparation and per-row arithmetic are shared helpers, with compiler-specific force-inlining confined to this implementation file.

The available-water calculation, proportional sharing between sinks, species arithmetic, signed-temperature rules, groundwater transfers and ledger additions retain their original per-row order. The final source-adjusted water volume is published after all rows consume the common budget. Face flux and runoff booking order is retained. The public data layout and object storage are unchanged. The optimization is CPU-only.

The selected implementation, two conservation tests and phase 5/6 reports are included in the CPU species/source optimization commit. The pre-existing four-line quiescent-reactivation change remains intact and is excluded from `selected.patch`. Unrelated checkout changes are preserved.

## Baseline and selection

A fresh source snapshot at phase-4 commit `8269dbc6`, including the recorded working-tree changes, is identified by `baseline_commit.txt`, `baseline.diff` and `baseline_manifest.json`. Old timing conclusions were not used as new evidence. Apple ARM64 host, 10 logical CPUs, Apple Clang and OpenMP. Component builds use O3/native CPU with no fast math and floating-point contraction disabled. Full-engine libraries use matched Release commands and frozen common objects; only the solver object differs.

Coarse profiling and sparsely sampled per-cell timers indicated source handling as a remaining multispecies cost. Sampling includes clock overhead and is qualitative, not a reliable absolute cost attribution. Separate, uninstrumented binaries supplied performance measurements.

Four approaches were considered: a specialized rain/infiltration path; fusion for all species counts; forcing the shared helpers inline; and limiting fusion to at least eight species. The narrow specialization had mixed results. Unconditional fusion penalized small species counts. The final hybrid retained the larger-species gains while avoiding most small-species overhead. Raw screen results are retained in `screen_timings.json`, `fused_screen_timings.json` and `inline_screen_timings.json`; original screen logs call the first simple prototype “selected”, while the normalized JSON labels it “simple”. `selected_origin.txt` records the final choice. Eight is a measured choice for this host, not a universal optimum.

## Component confirmation

Seven randomized adjacent pairs per workload, discarded warmups, 280 timed executions. The table gives medians; positive reduction means faster. Final-state hashing runs outside the timed advancement. No other review benchmark or build ran concurrently, but unrelated host work was active and load averages were high. Wall-clock results therefore need an idle-host replication, especially for short multithread runs. LI = local inertial; SWE = full shallow-water equations; DW = diffusive wave.

| Case / forcing | Cells | Mode | Species | Threads | Baseline wall s | Final wall s | Wall reduction | CPU reduction |
|---|---:|---|---:|---:|---:|---:|---:|---:|
| sparse / normal | 32,768 | LI | 8 | 4 | 0.0754 | 0.0717 | +4.9% | +2.0% |
| wave / coupling | 32,768 | LI | 8 | 4 | 0.1965 | 0.2035 | -3.6% | +5.6% |
| wave / evap | 32,768 | LI | 8 | 4 | 0.2388 | 0.2299 | +3.7% | +6.5% |
| wave / normal | 2,048 | DW | 8 | 4 | 0.9805 | 1.0015 | -2.1% | +3.6% |
| wave / normal | 8,192 | SWE | 8 | 4 | 0.0842 | 0.0901 | -6.9% | +5.4% |
| wave / normal | 32,768 | LI | 0 | 1 | 0.1138 | 0.1134 | +0.3% | +0.1% |
| wave / normal | 32,768 | LI | 0 | 4 | 0.0455 | 0.0484 | -6.4% | -2.5% |
| wave / normal | 32,768 | LI | 1 | 1 | 0.2050 | 0.2043 | +0.3% | +0.2% |
| wave / normal | 32,768 | LI | 1 | 4 | 0.0795 | 0.0857 | -7.8% | -2.6% |
| wave / normal | 32,768 | LI | 4 | 1 | 0.3218 | 0.3291 | -2.3% | -2.5% |
| wave / normal | 32,768 | LI | 4 | 4 | 0.1419 | 0.1301 | +8.3% | +3.8% |
| wave / normal | 32,768 | LI | 7 | 1 | 0.4915 | 0.4858 | +1.1% | -0.8% |
| wave / normal | 32,768 | LI | 7 | 4 | 0.2002 | 0.2220 | -10.9% | -4.1% |
| wave / normal | 32,768 | LI | 8 | 1 | 0.5393 | 0.5258 | +2.5% | +2.3% |
| wave / normal | 32,768 | LI | 8 | 4 | 0.2461 | 0.2152 | +12.6% | +6.4% |
| wave / normal | 32,768 | LI | 17 | 1 | 1.1067 | 0.8024 | +27.5% | +27.4% |
| wave / normal | 32,768 | LI | 17 | 4 | 0.4067 | 0.3533 | +13.1% | +23.6% |
| wave / normal | 32,768 | LI | 32 | 1 | 2.4822 | 1.6463 | +33.7% | +31.0% |
| wave / normal | 32,768 | LI | 32 | 4 | 0.6357 | 0.4963 | +21.9% | +26.7% |
| wave / rain_only | 32,768 | LI | 8 | 4 | 0.2465 | 0.2085 | +15.4% | +6.8% |

The 17- and 32-species workloads reduce CPU time by approximately 24–31%. Eight-species cases improve modestly. Zero/one/four/seven-species controls range from small gains to regressions; these are retained above. The phase is justified by the larger multispecies workloads, not by a claim that all 2D runs accelerate.

## Complete-engine confirmation

Four synthetic 32,768-cell input models combine rain, infiltration, eight/32 pollutants and a 2D drain connected to a 1D pipe. They run three simulated minutes with surface-file output disabled. Five randomized pairs and one discarded warmup pair per case give 40 timed executions, 48 total. Total time includes initialization, advancement and report generation, but excludes dynamic-library loading.

| Species | Threads | Baseline total s | Final total s | Elapsed reduction | CPU reduction |
|---:|---:|---:|---:|---:|---:|
| 32 | 1 | 21.213 | 17.979 | +15.2% | +29.3% |
| 32 | 4 | 5.883 | 4.247 | +27.8% | +27.5% |
| 8 | 1 | 5.638 | 4.851 | +14.0% | +4.1% |
| 8 | 4 | 5.028 | 4.845 | +3.6% | +2.8% |

All repeated runs match final depth hashes, routing-step counts, water budgets, infiltration and normalized reports exactly. Report normalization removes only the analysis start/end and elapsed-time lines. These are constructed workloads, not a representative production-catchment sample.

## Correctness evidence

- **164 regression tests passed; one unavailable Kokkos backend test skipped.** The selected full-engine library was verified as loaded for each executable. All 15 regression suites passed.
- **336 species comparison executions:** baseline/final histories and final fields identical, covering all momentum modes, triangular/quadrilateral meshes, one/four threads, 1/7/8/9/17/32 species, advection, dispersion, signed temperature and age, first/second order, boundaries and switching source combinations.
- **144 activation/source-transition executions:** identical histories and fields for dry, pulse, spill, edited-water, balanced and competing-sink cases.
- **72 analytical executions:** identical to the baseline for wet/dry lakes, Ritter, Stoker and radial/planar Thacker cases across mesh shapes and selected order/tier combinations. Equivalence does not mean zero analytical error.
- **280 component timings and 40 full-engine timings:** exact matched state checks in addition to timings.
- Two independent regressions exercise surface/groundwater species conservation and changing rain/coupling/evaporation budgets, including partial species blocks and intensive/signed rows.

Phase 6 adds larger meshes, cross-thread reproducibility, ten storm cycles, long analytical runs, coupled groundwater and HDF5 history comparisons. See `../cpu_phase6/REPORT.md` for the final qualification and its accuracy limits.

## Files and reproduction

`src_baseline` and `src_selected` are ignored frozen snapshots; `selected.patch` is the reviewable change. Build commands and common-object hashes are recorded in the JSON manifests. `build_engine_pair_frozen.py`, `check_regressions.py`, `check_species.py`, `check_transitions.py`, `check_analytical.py`, `confirm.py` and `compare_engine.py` record the workflows. Original model inputs are copied before execution. Generated binaries, common objects and run directories are ignored. `final_verification.json` records applied source hashes and validation counts. Supporting snapshots, scripts, raw results and manifests remain local review artifacts; this commit includes the production changes, tests and the two reports.
