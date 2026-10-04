# LTS cell threading results and next performance priorities

Date: 2026-10-04. This executes the next measurement checkpoint and the first due-cell threading step in F4 of the [approved DW/FV program](DW_FV_PERFORMANCE_REVIEW_AND_PLAN_2026-09-26.md).

**Integrated: parallel updates of sufficiently large 1D FV LTS due-cell batches.** On the retained 1024-conduit graded network with 16 cells per conduit, median engine-step time decreased by **13.4% at four threads and 13.6% at eight threads**. Both configurations improved in all five paired rounds. The four-thread baseline and candidate timing ranges do not overlap. The four-cell network shows no reliable gain. This is a targeted refined-LTS improvement, not proof that FV matches DW or that threading is complete.

The earlier [sparse positivity candidate](DW_FV_P1_LTS_REPORT_2026-10-04.md) remains separate. Diagnostic instrumentation developed for this checkpoint also remains in the experiment directory. The primary solver receives only the cell-threading change, its regression test and the test's optional OpenMP linkage.

## What the additional measurements established

The old profile did not time `fireCells` or `fireNodes`. Its `ltsfire` value combined face flux calculation, node discovery/solves, positivity and ledger booking. Existing timers already account for nested scopes as self time; the gap was missing phases and insufficient separation, not a need to add overlapping timers.

The diagnostic build separates those phases and counts due faces versus full-face capacity, firings below the existing face-threading threshold, due cells/nodes, flux evaluations, mass-only residual trials and bed/cell/ghost reconstruction calls. Clock reads occur at phase boundaries, never inside the cell or face kernels. Parallel counts use the existing private batches. The measured counts and hydraulic traces match the frozen baseline across the tested thread counts.

| Finding | Evidence and consequence |
|---|---|
| LTS cell updates become a substantial serial fraction | In the diagnostic 16-cell conduit case at four threads, they consumed 0.7530 of 3.7255 solver seconds, about 20%. The threaded diagnostic reduced that phase to 0.2932 seconds. This justified the narrow F4 experiment. |
| The full `ltsfire` value overstated the opportunity available to sparse positivity | On the 16k-cell component at four threads, positivity was 0.0526 of 0.8573 seconds, about 6%; cell updates were 0.1740 seconds, about 20%. This explains why eliminating full positivity scans could not capture the whole former firing timer. |
| Many firings are tiny | The component used 4208 face firings; 3945 were below the face worksharing threshold. Due-face visits were about 6% of the sum of full-face capacities. Large batches can benefit from threads, but small batches need a direct serial path. |
| Boundary refresh is not the leading cost in these networks | It took 0.0040/0.7015 seconds in the four-cell network and 0.0077/3.7255 in the sixteen-cell network at four threads, both below 1%. This does not exonerate structure-heavy or time-dependent-boundary workloads; it limits what F3 can deliver here. |
| Residual reconstruction is workload dependent | The 128-Y-junction fixture performed 68,736 mass-only trials out of 114,816 flux evaluations, about 60%. Node solving took about 42% of its one-thread diagnostic solver time. In the long graded chain, only about 7000 of millions of evaluations were residual trials. F2 remains compelling for junction-rich models, not a universal explanation for FV cost. |

These phase timings are exploratory attribution, not the performance acceptance experiment. Parts of the first profiling campaign overlapped preparation of the next build; small phase differences should not be interpreted as speedups. The accepted timing campaign ran separately, after builds and tests, with profiling disabled. Operation counts provide the more stable evidence for reconstruction and due-set activity.

## Change and dependency audit

`ExplicitFvSolver::fireCells` now executes its unchanged per-cell arithmetic through a local callable. It uses static OpenMP worksharing only when the **due-cell count** reaches the existing `OPENSWMM_FV_OMP_MIN_CELLS` threshold and more than one worker is available. Otherwise it directly traverses the due list without entering a serialized OpenMP region. No threshold default, mesh resolution, time step, closure, tolerance or compiler arithmetic policy changed.

The audit found:

- Each cell appears once in the due list, assembled from disjoint tiers. State, area/momentum accumulators, depth/velocity and closure-cache writes belong to that cell.
- Geometry, face corrections, forcing, roughness and tier durations are read-only during this phase. UF gradients are computed before the tier update and remain frozen during it.
- The apparent shared spill write in `injectDivertedLateral` is to a rimmed **node boundary face**, with one incident cell owner. Interior faces have no finite spill rim. `fireNodes` gathers those spills later in the original order.
- The face-to-cell/node ledger accumulation remains ordered and unchanged. The worksharing barrier completes cell publication before node updates. There is no floating-point reduction, atomic scatter or new neighbor snapshot.
- Profiling counts accumulate in worker-private batches and merge after worksharing. The clean candidate's counters and state match baseline at one and four threads.

The new `FvLts.DueCellThreadsPreserveStateAndScheduleBits` test compares every sampled A/Q/H byte, work counts, macro counts and tier occupancy at 1/2/4 threads, with UF off and on. It requires actual worker teams and LTS activity, uses a mesh large enough for the default worksharing threshold, and restores the caller's OpenMP settings. Builds without OpenMP skip that specific test. This is an incremental F4 change; a shared global/LTS cell kernel and a persistent team are still future work.

## Unprofiled timing with baseline controls

Each of five rounds shuffles configuration order and runs two identical baseline controls plus the candidate in shuffled order. Thread counts are interleaved rather than measured in separate long campaigns. There are 105 successful runs: 70 baseline controls and 35 candidate runs. The table pools the ten baseline measurements per configuration and compares them with five candidate measurements. Every run retains its state/output hash, work counts, process CPU/wall time and host load.

The host reports 10 logical CPUs and 8 performance cores. Requested/resolved worker counts are checked. The network API counts are policy predictions, not observations inside the running kernels; the component probe and dedicated regression separately observe OpenMP teams. See the subsequent [DW threading measurement audit](DW_FV_DW_D1_REPORT_2026-10-04.md) for that distinction. Passive OpenMP waiting and zero block time are fixed across builds. Physical-core placement, hardware counters and a quiet exclusive host were not established. Baseline controls measure some of that uncertainty; they do not remove it.

| Workload | Threads | Baseline median seconds [min–max] | Candidate median seconds [min–max] | Median time reduction |
|---|---:|---:|---:|---:|
| Graded conduit network, 4 cells/conduit | 4 | 0.6053 [0.5750–0.6543] | 0.6031 [0.5618–0.6640] | 0.4%, inconclusive |
| Graded conduit network, 16 cells/conduit | 1 | 6.7927 [6.6500–6.9759] | 6.7501 [6.6690–6.7930] | 0.6%, inconclusive |
| Graded conduit network, 16 cells/conduit | 4 | 3.5787 [3.4213–3.9368] | 3.0993 [2.9451–3.2183] | **13.4%** |
| Graded conduit network, 16 cells/conduit | 8 | 3.3785 [3.1917–3.7964] | 2.9182 [2.7470–3.5261] | **13.6%** |
| Graded rectangular component, 16,384 cells | 1 | 1.3141 [1.2890–1.4338] | 1.3382 [1.3074–1.4173] | −1.8% |
| Graded rectangular component, 16,384 cells | 4 | 0.7867 [0.7782–1.7624] | 0.6981 [0.6908–0.8024] | 11.3% |
| Graded rectangular component, 16,384 cells | 8 | 0.7820 [0.7403–0.9148] | 0.7389 [0.6788–0.8194] | 5.5% |

Network time sums engine-step API calls, including statistics/output work performed there, excluding setup and final reporting. Component time sums 32 solver-advance calls, excluding trace writes. The 1024-conduit network and its 300-second duration, initial state, boundary forcing and fixed 4/16-cell meshes are the same retained fixtures used in the prior checkpoint. They actually enter LTS; this is not a speedup achieved by coarsening or disabling the subgrid.

The four-thread 16-cell paired speedups relative to each round's mean baseline are 1.182, 1.208, 1.149, 1.099 and 1.198. Baseline A/B ratios range from 0.982 to 1.080. The target gain is repeatable beyond the observed control spread, with no unexplained median regression above 5% among the tested configurations. That supports adoption of this small change. Eight-thread results are supportive but have more overlapping ranges.

All observations are retained, including a 1.7624-second baseline component outlier whose paired baseline control was much faster. It is not filtered out or used to claim an 82% speedup. The pooled medians and the stronger conduit-network result drive the decision.

Candidate 16-cell network scaling is approximately **2.18× at four threads and 2.31× at eight**, relative to its one-thread median. This improves the baseline but remains below the proposed 3×/5× investigation targets. Eight threads deliver only a small further improvement over four. No weak-scaling result or new DW/FV comparison is claimed.

## Correctness and integration evidence

| Gate | Result |
|---|---|
| Diagnostic profiles | 27 completed runs covering 1/2/4/8-thread components, graded conduit networks and heavy junctions. Same-configuration traces/output and common operation counts agree across the three builds. |
| Extended LTS exactness | 40 runs: second order, dry front, SLOT, TPA and UF; baseline/candidate at 1/2/4/8 threads. Small-batch cell worksharing is forced for this gate. All sampled full-state/statistics traces match; finite state, nonnegative area, conservation and LTS activity checks pass. |
| Performance run validity | All 105 runs complete. For each fixed numerical configuration, all state/output, routing schedules and recorded aggregate work counts match across builds, repeats and tested threads. |
| Candidate regression suites | 6/6 pass with the small-cell threshold forced to one. Includes FV network/LTS/integration, DW UF and enabled/disabled profiling counters. |
| New permanent LTS test | All 8 LTS tests pass, including the new UF-off/on thread-parity case. |
| Core corpus | 25/25 outputs byte-identical; actual loaded baseline/candidate library paths verified; no nonzero deck exits. |
| Current-source integration | Rebuilt with concurrent source changes preserved; 6/6 selected suites pass. |

The corpus runner's overall exit is **1** because its separate broader 2D surface script is not executable. That suite is unrun. The core corpus compares the frozen baseline and candidate; the current-source integration tests are a distinct check, not a claim that all concurrent 2D changes passed a full identity corpus.

The initial diagnostic test run exposed stale objects in the reused build directory after restoring the earlier solver header. Rebuilding all header consumers resolved the crashes; subsequent diagnostic, candidate and integration suites pass. The failed and corrected logs remain available. Frozen binaries are stored independently of that mutable build directory.

State traces sample advance boundaries and cumulative scheduling statistics, not every internal substep decision. Full release coverage, race-sanitizer coverage, other platforms, physical-core placement, weak scaling and broader production networks remain open. The integration manifest identifies the three applied files and before/after hashes; unrelated edits were preserved. No commits or staging were performed.

## Next priorities

1. Keep this small due-cell improvement and the strict numerical contract. Extend its timing qualification to branched and production networks, 8-cell refinement, default wait policy and controlled CPU placement before making broader scaling claims.
2. Measure and reduce the remaining serial census, tier construction and ordered ledger work. In the threaded component profile, census and tier construction together still consumed about 36% of the four-thread advance. A persistent team alone cannot remove that serial work. Preserve the census cadence and due-face accumulation order in class A changes.
3. Pursue F2 residual reconstruction reuse on a representative junction-rich workload, where the new counts establish repeated cell-side work. Keep boundary refresh work F3 targeted to models where its measured cost is material.
4. Continue the approved DW dense-state and synchronization work independently, retaining the P0 ordered UF contract. The measured FV gain does not justify changing DW numerics or using a poorly threaded DW run as a comparison target.

## Reproduction and review files

- [Applied three-file patch](../../results/dw_fv_attribution_2026-10-04/candidate.patch), [integration manifest](../../results/dw_fv_attribution_2026-10-04/integration_manifest.json), and [integration source manifest](../../results/dw_fv_attribution_2026-10-04/integration_source_manifest.json).
- [Timing runner](../../results/dw_fv_attribution_2026-10-04/balanced_timing.py), [105 raw measurements](../../results/dw_fv_attribution_2026-10-04/timing/results.json), and [timing summary](../../results/dw_fv_attribution_2026-10-04/timing_summary.json).
- [Final artifact audit](../../results/dw_fv_attribution_2026-10-04/final_validation.json), [extended checks](../../results/dw_fv_attribution_2026-10-04/extended_checks/results.json), [integration test log](../../results/dw_fv_attribution_2026-10-04/tests_integrated.log), and [core corpus log](../../results/dw_fv_attribution_2026-10-04/corpus.log).
- [Diagnostic-only patch](../../results/dw_fv_attribution_2026-10-04/instrumentation.patch), [profile records](../../results/dw_fv_attribution_2026-10-04/profiles/results.json), and [threaded diagnostic records](../../results/dw_fv_attribution_2026-10-04/profiles_parallel/results.json).

Baseline provenance remains the frozen P1 source manifest and library. `source_candidate`/`candidate` identify the clean optimization build; `source_integrated`/`integrated` identify the later integration snapshot. Their manifests retain strict Release/OpenMP/LTO compiler settings and library hashes. Diagnostic builds have separate source/library directories and must not be substituted into an unprofiled timing comparison.
