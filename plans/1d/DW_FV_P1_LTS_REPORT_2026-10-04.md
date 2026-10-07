# Sparse LTS performance experiment: validation and adoption decision

Date: 2026-10-04. This checkpoint tests F1 in the [approved DW/FV program](DW_FV_PERFORMANCE_REVIEW_AND_PLAN_2026-09-26.md): reduce full-mesh bookkeeping during sparse 1D FV local-time-step firings. It follows the [P0 correctness checkpoint](DW_FV_P0_IMPLEMENTATION_REPORT_2026-10-03.md).

**Decision: retain the candidate separately; do not integrate it into the primary solver.** Sampled numerical results match exactly, but the timing evidence does not satisfy the program's reproducible-performance gate. Initial component medians improve; whole-network changes are small, and longer/eight-thread confirmation runs show mixed paired results and substantial host timing drift. This is a completed experiment with a deferred adoption decision, not an acceleration milestone.

**Later October 4 checkpoint:** the [LTS threading report](DW_FV_LTS_THREADING_REPORT_2026-10-04.md) records the follow-up attribution and a separate, accepted F4 cell-threading change. That change does not include this sparse-bookkeeping candidate. It improves the tested 16-cell network at four/eight threads while retaining sampled bitwise parity.

## What the code review led us to test

`ExplicitFvSolver::fireFaces` already restricts face evaluations to the due set, yet its positivity preparation clears and scans the full cell/node arrays on every firing. Node discovery also clears a full node bitmap. When a short reach fires frequently inside a large coarse mesh, this makes nominally local work depend on total mesh size.

A retained 16,384-cell graded-channel profile attributed 0.3900 seconds to LTS firing in a 2.0887-second advance total, roughly 19%. Census, flux evaluation and cell updates remained substantial. These timer scopes are not an exhaustive partition: their reported total differs from the outer advance time. The profile motivated a narrow experiment; it did not establish that this phase dominates a conduit network.

The candidate uses solver-owned scratch arrays and generation stamps to initialize only exporting cells/nodes, evaluate their positivity factors, and discover touched nodes. It preserves due-face order, each export sum, the positivity formula, scaled face flux and the order in which that flux enters the conservation ledgers. Unvisited positivity factors default to one, including the corner case where a nonzero flux times its time interval underflows to zero.

The first prototype used stamps for all firings. It showed overhead on some network configurations. The final candidate retains the original streaming full-array loops when the due-face count reaches one quarter of the cell count. This cutoff is an implementation experiment, not a numerical setting or a demonstrated universal crossover. Small due sets use compact exporter lists. Initialization resets the generations; wraparound clears every associated stamp array. Scratch allocation is lazy, so runs that never enter LTS avoid these buffers.

This reduces serial work without introducing a new parallel phase. It neither changes the LTS schedule nor solves the larger serial-update and synchronization constraints. It adds stamp/list memory and per-export bookkeeping; moving formerly thread-local buffers into each solver also changes their ownership and retained-capacity costs. Those costs matter when many solver instances coexist. Forced generation wrap and underflow were source-reviewed, not separately driven by dedicated runtime fixtures.

## Frozen source and validation

The fresh baseline source manifest records HEAD `6813158c2f66941e7d36cd330420d3f2a9554357` plus the dirty tree. The manifest, rather than HEAD alone, identifies the tested source. Both builds use the same Apple Clang ARM64 Release/OpenMP/LTO configuration, strict floating-point expressions, and disabled fast Manning/section options. The baseline and final candidate libraries have distinct SHA-256 hashes, recorded with probe compilation commands. The earlier P0 correctness fixes are present in both.

The P0 report-table regression has been corrected in the newer baseline: the report gate now includes FV. Fresh baseline and candidate builds both pass the affected integration suite. This supersedes that historical integration limitation, without rewriting its original failure record.

| Evidence | Result |
|---|---|
| Selected CTest suites | Baseline 7/7 and final candidate 7/7: FV geometry, network, LTS and integration; DW UF; enabled/disabled profiling counters. |
| FV state/profiling matrix | 36 completed runs; open first order, second order, graded LTS and SLOT at 1/2/4/8 threads, with profiling checks at 1/4. Identical sampled full-state/statistics traces between builds and across threads. |
| Storage-node LTS | 16 completed runs; two opposite initial-head arrangements with two closed storage nodes, at 1/2/4/8 threads in both builds. Identical state/statistics traces; finite state and cell-plus-node volume checks. |
| Solver reuse | New candidate test compares one reused solver with fresh solvers across different and repeated mesh sizes, reversing the initial front. Every sampled A/Q/H byte and work count matches; each scenario actually enters LTS. |
| Component timing gates | 80 final-candidate/baseline runs, including the two confirmation campaigns; state/statistics hashes, effective team sizes, finite state, nonnegative area and conservation are checked. |
| Authored-conduit network | 40 completed runs retaining 4/8/16 cells per conduit. For each fixed mesh, all output and routing-step hashes and aggregate work counts match across builds, repeats and tested thread counts. |
| Core engine corpus | 25/25 binary outputs identical, no failed deck exits, and actual library loader paths verified. Includes two coupled 2D decks. |

The corpus runner itself exits **1**, because its separate broader 2D surface script is not executable. That suite was not run. The 25-deck identity result is valid but must not be described as an entirely passing corpus runner or a full 2D gate.

The reuse test initially failed its LTS-activity assertion for a short advance window, with no state mismatch. Lengthening the window to engage LTS fixed the fixture; both logs are retained. This was not treated as a hydraulic failure or silently omitted.

Trace comparisons record all cell fields written by the native probe, storage heads/volumes where present, and cumulative step/tier statistics at advance boundaries. They do not record every internal decision or intermediate substep. Existing network/integration tests complement them; complete release feature coverage, cross-platform parity and independent legacy parity remain open. No tolerances, closures, CFL settings, resolution or arithmetic rules were relaxed.

## Performance results and their limits

Timings are five interleaved baseline/candidate pairs per row, reversing pair order on alternate repeats. Profiling is disabled. OpenMP uses passive waiting and zero block time. The host reports 10 logical CPUs and 8 performance cores. Actual team sizes are checked; CPU placement and hardware counters were not measured.

The component table measures complete solver `advance` calls, excluding state-trace writes. These are graded rectangular closed channels, not authored circular conduit networks. The global-step open-channel controls exercise code that should be unaffected by F1.

| Component / cells / advances | Threads | Baseline median seconds [min–max] | Candidate median seconds [min–max] | Median ratio B/C |
|---|---:|---:|---:|---:|
| LTS / 16,384 / 32 | 1 | 1.3117 [1.2960–1.3863] | 1.2597 [1.2462–1.4236] | 1.041× |
| LTS / 16,384 / 32 | 4 | 0.9288 [0.8062–0.9976] | 0.8513 [0.7845–0.9126] | 1.091× |
| LTS / 65,536 / 16 | 1 | 2.5080 [2.3521–2.7137] | 2.4286 [2.3133–2.5400] | 1.033× |
| LTS / 65,536 / 16 | 4 | 1.3771 [1.3309–1.5919] | 1.2977 [1.2798–1.3412] | 1.061× |
| Global / 16,384 / 128 | 1 | 0.2272 [0.2270–0.2316] | 0.2286 [0.2280–0.2317] | 0.994× |
| Global / 16,384 / 128 | 4 | 0.1047 [0.1005–0.1070] | 0.1030 [0.0998–0.1044] | 1.017× |
| Confirmation: LTS / 65,536 / 64 | 4 | 14.9050 [12.4420–22.8853] | 14.4245 [12.8343–23.5990] | 1.033× |
| Confirmation: LTS / 65,536 / 16 | 8 | 3.1137 [2.5945–3.8980] | 2.8984 [2.6697–3.1432] | 1.074× |

Both confirmation rows improved in only three of five pairs. The longer-run paired ratios are 1.086, 1.033, 0.969, 1.087 and 0.970. This reverses the sign often enough, against large range/drift, that the positive median is insufficient for promotion. The eight-thread campaign occurred under different host load and cannot be combined with the earlier four-thread campaign into a valid scaling curve. Recorded one-minute load averages ranged approximately 15–21 in the initial component campaign, 18–44 in the longer confirmation and 33–39 in the eight-thread confirmation. Load average is context, not a measurement of CPU utilization or proof of the source of delay.

The network has 1024 authored circular conduits, mostly 400 feet long with four 5-foot conduits in the middle; diameter 4 feet, slope 0.001, initially wet with matching upstream inflow/downstream stage. It simulates 300 seconds. Four, eight and sixteen cells per conduit remain separate fixed configurations. Actual LTS macro counts are 79, 160 and 400; this network genuinely exercises LTS, unlike the earlier uniform P0 screen.

| Cells/conduit | Threads | Baseline median engine-step seconds [min–max] | Candidate median engine-step seconds [min–max] | Median ratio B/C |
|---|---:|---:|---:|---:|
| 4 | 1 | 1.2121 [1.1587–1.2586] | 1.1997 [1.1575–1.3168] | 1.010× |
| 4 | 4 | 0.6403 [0.6023–0.6792] | 0.6563 [0.6124–0.7559] | 0.976× |
| 8 | 1 | 3.9456 [3.9287–4.2467] | 3.9619 [3.8753–4.1216] | 0.996× |
| 16 | 1 | 6.7294 [6.6670–6.9177] | 6.6184 [6.5657–6.8215] | 1.017× |

Network timing sums engine-step API calls, including work performed there for statistics/output; setup and final reporting are excluded and separately recorded. The 16-cell row improves in all five pairs, but the gain is only around 1–2% and is not reproduced across refinement/thread configurations. No final network median regression exceeds 5%, but that alone is not an adoption criterion.

Four-thread scaling is still modest: candidate graded-component medians give about 1.48× at 16k cells and 1.87× at 65k; the four-cell conduit network gives about 1.83×. These sampled ratios fall below the program's proposed 3× target. They do not identify how much is serialization, imbalance, bandwidth, runtime overhead or host contention. Weak scaling and physical-core placement remain unmeasured. No new DW/FV runtime comparison was made in this checkpoint; FV ≤ optimized DW remains unproven.

## Plan adjustments supported by this experiment

1. **Keep the primary F1 implementation unchanged for now.** Preserve the final three-file patch, binaries, full source snapshots and evidence so that qualification can resume without reconstructing the experiment. Do not tune the cutoff repeatedly against these noisy samples.
2. **Make the next performance qualification controlled and targeted.** Interleave thread counts as well as binaries on an otherwise quiet host; record effective placement and CPU time; retain a baseline-versus-baseline control. Recheck the same sparse/dense workloads and a large branched network before adopting F1. Compare both absolute engine-step time and phase work. No new numerical options are needed.
3. **Split the LTS firing measurement before a larger rewrite.** Measure due-set density, scratch initialization/scan cost, node solves, positivity and ledger booking separately, using per-phase timing and private counters rather than hot-loop atomics. The current aggregate timer cannot prove which portion of the network benefits. Measure allocations/retained capacity as well as time.
4. **Continue the approved F3 and F4 investigations.** Quantify repeated boundary reconstruction and the serial cell-update/ledger/census work before adding caches or a persistent team. Preserve face accumulation order and assign disjoint work to workers. Use dense and sparse workloads, explicit small-set serial cutoffs and matched-core strong/weak scaling gates. Require a measured whole-advance opportunity before a large architectural patch.
5. **Keep DW locality work and numerical constraints intact.** The P0 ordered UF dependency fix remains the baseline. Continue D1/D2 dense conduit data and D4 synchronization work under the same-configuration exactness contract; this FV experiment supplies no evidence to change the UF stencil, compiler arithmetic, node sum order or solver defaults.

The new perspective from validation is that eliminating an obvious full-mesh operation is not enough: bookkeeping crossover, real conduit boundaries, refinement and host variability can outweigh a local saving. The next decision needs phase attribution and repeatable end-to-end evidence, not another favorable isolated median.

## Review and reproduction artifacts

All experiments live in [results/dw_fv_p1_2026-10-03](../../results/dw_fv_p1_2026-10-03/). The directory date reflects when this checkpoint began.

- [Final candidate patch](../../results/dw_fv_p1_2026-10-03/hybrid.patch): FV source/header plus the solver-reuse test. It is **not applied** to the primary checkout.
- [Baseline source manifest](../../results/dw_fv_p1_2026-10-03/source_manifest.json), [baseline library manifest](../../results/dw_fv_p1_2026-10-03/baseline_manifest.json), and [candidate library manifest](../../results/dw_fv_p1_2026-10-03/hybrid_manifest.json).
- [Artifact audit](../../results/dw_fv_p1_2026-10-03/final_validation.json) and [paired timing summary](../../results/dw_fv_p1_2026-10-03/timing_summary.json); raw results, traces and logs remain in each campaign directory.
- [Component runner](../../results/dw_fv_p1_2026-10-03/benchmark.py), [network generator/runner](../../results/dw_fv_p1_2026-10-03/network_graded.py), [storage-node checks](../../results/dw_fv_p1_2026-10-03/node_checks.py), [test log](../../results/dw_fv_p1_2026-10-03/tests_hybrid.log), and [corpus log](../../results/dw_fv_p1_2026-10-03/corpus_hybrid.log).

Use the paired frozen source/library directories (`source_baseline`/`baseline`, `source_hybrid`/`hybrid`) for reproduction. The reused P0 build directory is mutable and is not a frozen binary reference. Preserve the exact compile flags and loader paths. Existing output directories deliberately reject timing reruns; use a new retained directory name. No source changes, staging or commits were made to the primary solver for this candidate.
