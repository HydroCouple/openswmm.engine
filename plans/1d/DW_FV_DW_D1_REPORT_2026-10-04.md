# DW D1 mapping experiment and threading measurement audit

Date: 2026-10-04. Status: **experiment complete; CSR candidate not promoted**. P0/P1 remain in progress. The two prior committed correctness/FV-threading changes remain in place.

This checkpoint follows D1 and D4 of the [approved program](DW_FV_PERFORMANCE_REVIEW_AND_PLAN_2026-09-26.md). It tests the proposed dense conduit index in node adjacency, profiles DW phases, and checks sensitivity to thread waiting. It does not change numerical options, mesh resolution, thread defaults, or production solver source.

## Decision

Do not adopt the additional CSR index yet. It preserves the sampled numerical results, but the 90-run timing campaign does not establish a repeatable benefit beyond identical-baseline variation. On the 8,192-conduit serial profile, the entire gather accounts for only about 4% of engine-step time; removing one lookup can recover only part of that cost. Prioritize geometry traversal/synchronization and measured thread-policy crossover before this small mapping change. Keep the candidate available for a quieter, representative mixed-link workload if later profiles justify it.

Thread waiting has material, workload-dependent effects. These runs do **not** justify switching the default wait policy. Background contention was substantial, and bounded active waiting was particularly expensive at eight requested workers. Worker placement and individual barrier wait were not measured.

## Source and build provenance

The frozen baseline is the preceding integrated build, including both the ordered DW unsteady-friction fix and the FV due-cell threading change. Its library SHA-256 is `4fa4454c6f5dbe83c351bbce609af7092c10780ce50a9989584b050beef540bf`. The working checkout HEAD when recorded was `f5345bf0`.

This is a dirty-source snapshot, not a claim to reproduce HEAD alone. Four files in the active checkout differed from that frozen source: `DefaultReportPlugin.cpp`, `SWMMEngine.cpp`, `SWMMEngine.hpp`, and `SimulationContext.hpp`. The current production DW header/source matched the frozen baseline throughout the experiment. No current-checkout integration was needed because the candidate failed the performance gate.

Apple Clang ARM64 Release, LTO, OpenMP, strict floating-point flags, and disabled fast Manning/section options match the previous checkpoint. Baseline and candidate use identical build settings. Source/library hashes, scripts, logs, decks, traces, output files and isolated patches are retained in [the experiment directory](../../results/dw_fv_dw_d1_2026-10-04). The [final audit](../../results/dw_fv_dw_d1_2026-10-04/final_validation.json) passed.

The reusable isolated build workspace currently contains the team-observation diagnostic candidate. Use the separately frozen clean baseline/candidate libraries for further comparisons, or restore the intended source snapshot and rebuild first.

## Candidate and numerical evidence

The [candidate patch](../../results/dw_fv_dw_d1_2026-10-04/candidate.patch) stores the dense conduit index alongside each CSR incident-link entry and reads it directly during node-flow gathering. Both ends are filled, including both entries of a self-link. Existing row order, link-indexed flow reads, loss calculations and floating-point accumulation order are preserved. Storage increases by four bytes per valid conduit end, normally eight bytes per conduit.

| Check | Result |
|---|---|
| Selected engine suites | 6/6 pass: routing, virtual junctions, DW TPA, DW unsteady friction, cross-section parity and cross-section kernel parity |
| Pressure/UF traces | 24 successful baseline/candidate runs, SLOT and EXTRAN, UF off/on, 1/2/4 forced threads; every sampled binary64 node-depth/link-flow value and elapsed time matches the corresponding serial reference |
| Uniform-network experiments | All 156 phase-profile, wait-policy and clean timing runs finish with matching output bytes and routing-step schedules for each network size; continuity checks pass |
| Core corpus | 25/25 output files byte-identical, with the distinct baseline/candidate loaded-library paths verified |
| Actual DW team diagnostic | 18 separate short runs, two network sizes × three thread counts × three wait policies; each of four routing steps observes the requested 1/4/8 workers inside the DW team |

The corpus runner exits 1 because its separate broader 2D regression script is not executable. The 25 core decks include two coupled 2D decks, but this is not a complete surface-solver validation. The API traces sample the stated public fields; they are not a dump of every private solver array. No new dedicated self-link/geometry-edit regression was added for this unpromoted candidate.

## Clean timing campaign

Two uniform wet networks contain 1,024 and 8,192 circular conduits and simulate 30 seconds at a 0.25-second routing limit. Each performs 120 routing steps and 240 Picard iterations in the diagnostic build. These are synthetic, mostly-conduit workloads, not a production qualification set.

Five rounds shuffle configurations and, within each configuration, shuffle baseline A, baseline B (the identical library) and candidate. There are 60 baseline controls and 30 candidate runs. The table pools ten baseline observations against five candidate observations. Times sum engine-step API calls, including output/statistics work during those calls; setup and final reporting are recorded separately. Waiting is passive with zero block time. All observations and outliers are retained.

| Conduits | Requested workers | Baseline median seconds | Candidate median seconds | Apparent time reduction |
|---:|---:|---:|---:|---:|
| 1,024 | 1 | 0.06065 | 0.06131 | −1.1% |
| 1,024 | 4 | 0.07270 | 0.07652 | −5.3% |
| 1,024 | 8 | 0.11276 | 0.10674 | 5.3% |
| 8,192 | 1 | 0.60190 | 0.60082 | 0.2% |
| 8,192 | 4 | 0.34873 | 0.33343 | 4.4% |
| 8,192 | 8 | 0.37140 | 0.35777 | 3.7% |

**None is an accepted speedup.** Every row has overlapping baseline/candidate ranges. For 8,192 conduits at four workers, identical-baseline A/B ratios span 0.262–2.078; the candidate's apparent 4.4% median gain cannot be separated from that variation. The small four-worker case also crosses the proposed 5% regression screen. Its noisy result is not proof of a real regression, but it provides no basis for promotion. See [full timing summary](../../results/dw_fv_dw_d1_2026-10-04/timing_summary.json).

The host reports 10 logical CPUs and eight performance cores. One-minute load averages during timing were roughly 45–55; during the earlier wait-policy campaign they were roughly 54–103. Load average is a contention indicator, not measured core utilization. These experiments did not have exclusive CPU access.

## Phase and wait-policy findings

Diagnostic timers read the master thread's elapsed time between existing phase boundaries, without adding barriers. These intervals include existing waits; node initialization overlaps geometry. They cannot isolate worker imbalance, CPU execution, or barrier self time, and logging adds overhead. Interpret them as attribution clues, not performance acceptance measurements.

In the one-thread 8,192-conduit profile, engine-step time was 0.688 seconds: momentum 0.407, geometry 0.101, node-depth updates 0.042, and conduit gathering 0.027. This ranks momentum and geometry ahead of the CSR gather on that fixture. In the initial multithread profile, geometry and surrounding synchronization intervals grew sharply; later identical-binary timings were much lower. That change rules out treating the initial negative scaling as an intrinsic solver limit.

The separate 54-run campaign compares passive/zero block time, active/1 ms block time, and no wait-policy overrides, across both sizes and 1/4/8 workers. For 8,192 conduits, four-worker engine-step medians were 1.276, 0.661 and 0.949 seconds respectively. At eight workers they were 1.179, 5.632 and 1.419 seconds. The active eight-worker runs also consumed much more process CPU time. These results expose policy sensitivity under contention; three repeats and unstable host conditions are insufficient to choose a shipping policy. See [wait-policy measurements](../../results/dw_fv_dw_d1_2026-10-04/wait_summary.json). “No overrides” does not establish the runtime's internal default policy.

## Thread-count evidence correction

Inspection of `swmm_get_effective_threads` found that it recomputes policy-resolved counts. It does not observe a running team and does not include the `SWMM_DW_THREADS` override. The network harness's `effective_dw` and `effective_global` fields therefore must be read as policy predictions, not observed worker counts. The original timed runs did not record an in-team count.

The added diagnostic calls `omp_get_num_threads()` from the DW master inside its actual parallel region. All 18 separate checks observed the requested teams at every step. This corroborates the tested setup without retroactively converting the clean timing metadata into observations. Physical-core placement remains unknown; the Darwin QoS call is a scheduling hint, not pinning evidence. Prior FV component probes independently observe an OpenMP team; network API counts alone are insufficient for per-kernel team claims.

## Next work within the approved program

1. **D4/D1 geometry synchronization:** audit pass fusion and repeated full-network traversal. One concrete candidate is folding SLOT width overrides into the following conduit-classification pass, removing a traversal and closing barrier. Preserve width updates for bypassed conduits, current shape-slice dependencies and expression order. Validate before promotion; this checkpoint does not implement it.
2. **P0 scaling evidence:** add direct, diagnostic-only team observations to future qualification, separate useful worker time from barrier wait, and repeat controlled strong/weak scaling on a quiet host with baseline controls. Keep requested, resolved and observed counts distinct. Measure automatic settings as well as explicit budgets.
3. **D1/D2 layout:** defer the extra CSR index unless a mixed-link or cache-sensitive workload exposes material gather cost. Continue dense-state/geometry work where current profiles support it; do not count existing optimizations as new gains.
4. **FV F4/F2:** retain the accepted due-cell threading change. Target census, tier construction and ordered accumulation next; use junction-rich fixtures for boundary reconstruction/cache work. Keep the retained 4/8/16-cell meshes and compare against the best validated DW configuration.

No further phase is declared complete. Refined FV versus optimized DW runtime parity, broader scaling qualification, and the final release gate remain open.
