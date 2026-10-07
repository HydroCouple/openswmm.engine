# DW and FV performance implementation checkpoint

Date: 2026-10-03. Scope: dynamic wave and the 1D FV conduit subgrid, including multithreading. This executes the first measurement and correctness stage of the [approved program](DW_FV_PERFORMANCE_REVIEW_AND_PLAN_2026-09-26.md).

**Follow-up, October 4:** the [F1 experiment report](DW_FV_P1_LTS_REPORT_2026-10-04.md) records the next frozen baseline and candidate. The report-table regression described below is resolved in that newer baseline; both new builds pass all seven selected suites. Sparse-LTS optimization remains a separate candidate because its timing evidence does not meet the performance gate. The original results below remain historical evidence.

The first deliverable makes the baseline trustworthy: deterministic DW unsteady friction and correct FV operation counts at multiple threads. It does **not** establish an acceleration milestone. Two small optimizations passed the sampled numerical checks but did not establish a reproducible timing improvement; they remain separate experiments. The larger SoA, LTS and threading changes in the approved plan are still ahead.

## Changes selected for integration

| Change | Reason and consequence |
|---|---|
| Order the DW momentum pass when unsteady friction is enabled with positive K3 | The stencil reads neighboring flows and areas that this same pass updates in place. Preserve the existing one-thread sequence, including slot overrides and flow publication. Keep the other DW phases threaded. |
| Synchronize initialization of the optional DW link trace | Function-static configuration was mutated by multiple conduit workers. C++ guarded initialization removes that race; only the selected conduit accesses the mutable trace stream. |
| Accumulate FV counters privately inside parallel phases | Each worker collects integer counts and merges once at the end of its batch. Nested batches merge into their parent. No contended counter increment is added to a hydraulic inner loop. |
| Add focused regression tests | Exercise the UF neighbor dependency with forced worker boundaries, and test parallel/nested counter batches, reset, and disabled profiling. |

The UF change intentionally uses the established **serial** computation as the reference. Existing threaded UF results were nondeterministic, so there is no stable threaded trajectory to preserve. A snapshot of all previous-iterate neighbor velocities would be a different numerical method and was not substituted. This fix can reduce UF momentum parallelism; it is a correctness prerequisite for the performance work.

The profiler still has process-global totals and timers. Profile one simulation at a time. This patch does not make concurrent independent profiled engines isolated. The debug trace also retains its existing process-global configuration.

## Independent baseline and numerical evidence

The tests use a fresh, isolated source snapshot of the dirty engine tree at `7550c0496bc3a32e858861d8487bba60c7849e1d`, rather than assuming that an existing binary represented the source. The source manifest records every copied file hash. Baseline and experimental candidate use identical Darwin Release options: Apple Clang, ARM64, OpenMP, LTO, `-O3`, `-fno-fast-math`, `-ffp-contract=off`, and `-fno-math-errno`. Both fast section lookup and fast Manning power options are OFF. CPU 2D support is present; the GPU plugin is OFF in both builds.

Frozen libraries, compiler commands, build caches, generated decks, traces and logs are retained in [the experiment directory](../../results/dw_fv_p0_2026-10-03/). The CLI comparison includes loader logs proving that the baseline and experimental libraries came from their respective frozen directories. The source snapshot is essential because this shared checkout continued changing during the work.

| Check | Result and scope |
|---|---|
| Fresh baseline existing tests | All five selected routing, DW UF and FV suites passed before changes. |
| New DW regression against baseline | Failed as expected, reproducing the threaded UF mismatch in the newly built engine. |
| Experimental DW matrix | 24 successful runs: SLOT/EXTRAN, UF off/on, 1/2/4 threads, two repeats. All 36 within-candidate comparisons were bitwise equal. The four one-thread configurations also matched the fresh baseline trace bytes. |
| Experimental FV component matrix | 32 unprofiled runs: open first order, open second order, graded LTS and closed SLOT, with 4096 cells at 1/2/4/8 workers in both builds. Cell A/Q/H and recorded step/tier statistics matched exactly. Finite state, nonnegative area, completion and walled-channel volume conservation checks passed. Four additional profiling runs preserved state. |
| Experimental engine corpus | 25/25 output files byte-identical with successful completion. Includes quality and two coupled 2D decks, but the separate broader 2D surface regression was not run. |
| Experimental network screen | 100 completed runs. For each fixed solver/mesh, output bytes and routing-step times matched across both builds, repeats and tested thread counts. Meshes remain distinct numerical configurations. |

The final selected patch was rebuilt **after removing both deferred optimizations** and independently rechecked:

| Final patch check | Result |
|---|---|
| Selected CTest suites | 7/7 passed, including the new UF and counter regressions. |
| DW matrix | 24/24 completed; 36/36 comparisons bitwise equal, plus all four serial configurations identical to the fresh baseline. |
| FV component and profiling matrix | 36/36 completed; one state/statistics hash per case across both builds and 1/2/4/8 threads. Maximum absolute relative volume error was below 1.7e-15. |
| Engine corpus | 25/25 output files byte-identical to baseline; loader logs identify the selected library. |
| Heavy-junction instrumentation | 128 independent Y junctions, each with three connected conduits. Baseline at one thread and final patch at 1/2/4/8 threads produced identical output and all recorded operation counts. Each run performed 2560 junction solves, 22,912 residual evaluations and 68,736 residual face evaluations. |

These are results from the isolated source/build pair. The production patch was applied surgically onto the newer shared checkout, preserving its concurrent statistics and 2D changes. The [integration manifest](../../results/dw_fv_p0_2026-10-03/integration_manifest.json) records before/after hashes; the [actual applied diff](../../results/dw_fv_p0_2026-10-03/integrated.patch) isolates this task's edits.

**Integration finding:** the newer shared source built successfully, but its rerun passed **6/7 suites**. The FV integration suite passed 33/34 cases and failed `FvEngine.ReportDescribesTheRunItActuallyMade`: C2 was missing from the flow-classification table. A concurrent change in `DefaultReportPlugin.cpp` now restricts that entire table to `RoutingModel::DYNWAVE`; the FV case therefore cannot print it. That file is absent from this performance patch. This is a source-level attribution, not a claim that an unpatched integration control build was run. The earlier frozen-base patch passes this test. The shared checkout is consequently **not a fully green integration baseline**. The report change was left for its own workstream rather than folded into the solver-performance patch. See [failure details](../../results/dw_fv_p0_2026-10-03/integration_report_regression.json), [integration test log](../../results/dw_fv_p0_2026-10-03/tests_integrated.log), and the retained [single-test reproducer](../../results/dw_fv_p0_2026-10-03/integrated/run_report_regression).

The FV component probe records state at each of eight advance-window boundaries; it does not instrument every internal substep. It has no junctions or transported species. Its full cell-state comparison is complemented by engine-level regression, not a claim to cover all internal network state. The DW probe captures binary64 node depths and link flows at every routing step, not every internal convergence/control decision. Full decision tracing, broader legacy comparison and the complete feature corpus remain release gates.

The original counter failure was directly measurable: a four-thread baseline run recorded only **18,915 of 40,976 area evaluations**, and 16,648 of 32,768 hydraulic-radius evaluations. The experimental candidate returned the exact one-thread counts at four threads while retaining identical cell state. Lost increments are therefore observed evidence, not an assumption based only on inspecting the code.

## Timing screen and promotion decision

The synthetic network has 1024 initially wet circular conduits, each 40 feet long and 4 feet in diameter, a uniform slope, an upstream inflow and a fixed downstream stage. The 300-second simulation uses the same authored network for both solvers. FV retains 4, 8 or 16 cells per conduit; no one-cell result is used to support a speed claim. There are five interleaved runs per build/configuration, with reversed pair order on alternating repeats. Profiling is disabled during these timings.

The table measures accumulated engine-step API time, including routing, statistics and report-output work performed during those calls. Setup and final reporting are excluded from that column and recorded separately as total API time. It is not a pure momentum-kernel timer. The machine reports 10 logical CPUs and 8 performance cores. Requested and resolved team counts are recorded; CPU placement and hardware counters were not measured. All runs explicitly use passive OpenMP waiting and zero block time, rather than testing the shipping automatic wait policy.

| Solver and cells per conduit | Threads | Baseline median seconds [min–max] | Experimental median seconds [min–max] |
|---|---:|---:|---:|
| DW | 1 | 1.074 [0.995–1.360] | 1.023 [0.927–1.293] |
| DW | 2 | 5.044 [4.139–21.699] | 4.170 [3.182–6.237] |
| DW | 4 | 7.375 [1.510–9.971] | 10.357 [1.274–15.027] |
| DW | 8 | 1.566 [1.331–11.090] | 1.535 [1.292–20.756] |
| FV 4 | 1 | 1.670 [1.549–4.657] | 1.877 [1.505–7.849] |
| FV 4 | 2 | 1.470 [1.279–1.705] | 1.656 [1.468–1.987] |
| FV 4 | 4 | 1.884 [1.701–2.526] | 1.849 [1.676–2.581] |
| FV 4 | 8 | 1.746 [1.429–3.516] | 1.550 [1.250–2.735] |
| FV 8 | 1 | 3.287 [2.787–5.108] | 2.819 [2.607–3.650] |
| FV 16 | 1 | 13.821 [11.708–14.498] | 14.516 [11.226–19.155] |

The experimental candidate includes the correctness fixes **plus** a full-radius Manning-power cache and removal of redundant first-order slope resets. Those two performance changes are excluded from the integrated patch. Large overlapping ranges, occasional order-of-magnitude latency spikes, and inconsistent signs of the differences prevent attributing an acceleration to them. The acceptance rule in the approved plan requires a reproducible gain beyond the measurement spread and no unexplained representative regression.

The four-cell FV run did not establish parity with the best observed DW setting. Do not present a favorable FV/DW comparison at a slow DW thread count as meeting that target. These observations also do not justify changing automatic thread policy from one loaded host and one network.

A separate five-second pressurized startup pilot was retained but excluded from performance qualification: DW reported a **−17.512%** routing continuity error in both builds. FV reported rounded zero continuity error. That fixture needs a valid initial state and independent accuracy checks before it can support a pressure-workload speed comparison. It is not evidence that the candidate introduced a new DW error.

Refinement changes the amount of work substantially. At four and eight cells, the screen used 1200 FV substeps and approximately 6.14 and 11.06 million face evaluations. At sixteen cells it used 3199 substeps and 55.69 million face evaluations. No LTS macro cycles fired in this uniform network even though LTS was enabled. The separate graded component case did exercise LTS. This reinforces the need to measure actual tier activity and CFL work before crediting LTS with a benefit.

## Next steps in the approved program

1. **Finish P0 performance qualification.** Repeat the retained matrix on a quiet host, include automatic and bounded/passive wait policies, record CPU placement and CPU time, and extend to large branched production networks and valid pressure transients. Complete weak scaling and the 8/16-cell multithread curves. Existing host thread reservations must be honored; the current engine already implements `OPENSWMM_HOST_RESERVED_THREADS`.
2. **Profile the whole FV advance with the corrected counters.** Rank cell closure, node residuals, census, active-list work, positivity, rollback and synchronization. Proceed with F1 sparse-LTS scratch and F3 boundary reconstruction reuse only where their measured runtime shares justify them. The uniform screen supplies no evidence for a sparse-LTS speedup.
3. **Address concurrency costs alongside SoA work.** Use the existing persistent DW team; measure barrier and active-mask costs before changing scheduling. For FV, measure the benefit of a persistent team around the full advance, including currently serial phases. Keep a serial path for small due sets and preserve each node/face accumulation order.
4. **Continue DW dense conduit state in small groups.** Preserve DUMMY/subtype/link mapping, lifecycle refresh and publication points. For UF, investigate independent dependency components while retaining the old in-place order within each component; one connected dependency chain remains a separate scaling constraint. Do not use a new stencil under the same parity label.
5. **Keep pressure-method work separate.** F8 implicit-pressure improvements may be needed where acoustic CFL dominates. They require their own conservation, transient-accuracy and nonlinear-convergence review before adoption.

No numerical defaults, closure panels, tolerances, CFL values, mesh resolution, or compiler arithmetic rules were relaxed. Universal refined-FV runtime parity with DW remains unproven.

## Reproduction and review files

- [Source manifest](../../results/dw_fv_p0_2026-10-03/source_manifest.json) and [baseline binary manifest](../../results/dw_fv_p0_2026-10-03/baseline_manifest.json).
- [Selected patch](../../results/dw_fv_p0_2026-10-03/accepted.patch) and [deferred optimization patch](../../results/dw_fv_p0_2026-10-03/deferred_optimizations.patch). Apply the latter only to the selected patch's corresponding source state when revisiting the experiment.
- [Network generator and runner](../../results/dw_fv_p0_2026-10-03/network_screen.py), [100 raw run records](../../results/dw_fv_p0_2026-10-03/network_screen/results.json), and [timing summary](../../results/dw_fv_p0_2026-10-03/network_screen/summary.json).
- [FV state probe](../../results/dw_fv_p0_2026-10-03/fv_state_probe.cpp), [FV runner](../../results/dw_fv_p0_2026-10-03/run_fv_checks.py), and [heavy-junction counter fixture](../../results/dw_fv_p0_2026-10-03/junction_counts.py).
- [Baseline failure of the new regression](../../results/dw_fv_p0_2026-10-03/regression_red.log) and [experimental corpus with loader verification](../../results/dw_fv_p0_2026-10-03/corpus_loader_verified.log).

Final patch logs: [CTest](../../results/dw_fv_p0_2026-10-03/tests_accepted.log), [DW matrix](../../results/dw_fv_p0_2026-10-03/accepted_probe/passive_results.json), [FV matrix](../../results/dw_fv_p0_2026-10-03/fv_checks_accepted/results.json), [junction counts](../../results/dw_fv_p0_2026-10-03/junction_checks_accepted_closed/results.json), [corpus](../../results/dw_fv_p0_2026-10-03/corpus_accepted.log), and [selected binary manifest](../../results/dw_fv_p0_2026-10-03/accepted_manifest.json).

The [final artifact audit](../../results/dw_fv_p0_2026-10-03/final_validation.json) checks saved state finiteness, hashes, loader paths, exact counts and these separate test verdicts.
