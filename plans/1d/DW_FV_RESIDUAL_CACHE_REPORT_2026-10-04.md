# FV junction residual cache: implementation and qualification

Date: 2026-10-04. Status: **candidate implemented and validated in isolation; retained for further qualification, not integrated**. This continues F2 from the [tier attribution checkpoint](DW_FV_TIER_ATTRIBUTION_REPORT_2026-10-04.md) and [approved performance program](DW_FV_PERFORMANCE_REVIEW_AND_PLAN_2026-09-26.md).

## Decision

The solve-local cache preserves the tested residual trajectory and produces a repeatable single-worker improvement on the junction-rich fixture: 10.7% lower median step time in the initial screen and 7.4% in the longer confirmation. The confirmation's three single-worker candidate/control ratios all improve, by about 5.7–9.2%; identical-baseline pairs differ by 0.5–4.9%.

The **multithread runtime gate remains open**. Four-worker free-flow results move from 3.9% faster in the screen to 1.9% slower in confirmation; eight-worker screening shows no clear gain. A 16-cell slowdown in the screen does not reproduce consistently. Reduced process CPU work is encouraging but does not establish lower elapsed time for one multithreaded simulation. Under the program's requirement to demonstrate benefits at one and multiple workers, retain the candidate separately.

Production solver files and thread/numerical defaults are unchanged. No commit was created. The isolated build was restored to the accepted baseline. The [candidate and all evidence](../../results/dw_fv_residual_2026-10-04) remain available for controlled qualification.

## Cost verification and implementation

A longer baseline screen confirms that the small earlier fixture identified a real cost: on 256 independent Y junctions with four cells per conduit, algebraic-node work occupies about 38% of profiled step time at one worker and 25% at four workers in the 120-second free-flow case. A 10-second crown-transition case gives about 46% and 28%. These phase measurements are targeting evidence, not an allocation of the entire node cost to cacheable work. [Baseline profiles](../../results/dw_fv_residual_2026-10-04/baseline_cost/results.json).

The [patch](../../results/dw_fv_residual_2026-10-04/candidate.patch) changes only `ExplicitFvSolver::solveAlgebraicNode`:

- Each ordinary live incident face lazily prepares its original left/right bed maximum, oriented interior velocity and reconstructed cell-side `FaceState` on the first residual evaluation.
- Each subsequent trial recomputes the head-dependent ghost with the original `faceSide` function and calls the original mass Riemann kernel in the original left/right order.
- Gates, culverts and unexpected face topology use the existing `computeFaceFlux(f, true)` path. Pass-through and prescribed-head nodes return through their existing early paths.
- Residual accumulation order, trial-head selection, convergence/clamp logic, degree-one fallback, and final full-flux publication remain unchanged.

Scratch is a thread-local vector sized by node degree. Readiness is reset on **every solve invocation**; it is not a cache across cells changing, RK stages, tier firings, rollback or engine instances. Each cell-side record also starts from a fresh `FaceState{}` before preparation. That reset matters: `faceSide` can leave default fields untouched when a feature is disabled, so retaining an old pressure flag would be wrong. No public API, mesh-sized cache or extra parallel region is introduced.

The current engine already caches centred cell closure values and already uses mass-only trials. Accordingly, the free-flow case's area/width/I1 counters need not fall even when bed reconstruction, field loads, velocity preparation and cell-side celerity calculation are avoided. The second-order case does reduce geometry work. The cache's benefit must be assessed by real runtime and unchanged numerical work, rather than claiming the existing closure cache as a new optimization.

## Exactness evidence

| Check | Result |
|---|---|
| Selected existing suites | 6/6 pass |
| Network comparisons | 60 runs at 1/4/8 configured workers match sampled output and scheduling |
| Internal diagnostic matrix | 40 runs; candidate trial inputs, residuals and final face values match exactly |
| Successive engine reuse | Two processes, four engines each; matching internal traces through changed settings and team sizes |
| Clean timing runs | 108 completed runs; sampled outputs and hydraulic work counts remain matched |

The existing suites cover FV LTS, networks, integration, implicit pressure, and profiling enabled/disabled. [Test log](../../results/dw_fv_residual_2026-10-04/tests.log).

The [network matrix](../../results/dw_fv_residual_2026-10-04/checks/results.json) covers free flow, crown transitions, pressurized flow, TPA, second order, reversed orientation with mixed section sizes, wetting/drying, gates, LTS and LTS with transitions. Binary output, routing times and substep/flux/macro counts match their serial references. All unchanged-work counters match. Area/width/I1 counts are allowed to decrease when the cached cell-side closure replaces repeated evaluation; for example, second-order area evaluations fall from 815,616 to 694,272. Other hydraulic work and accepted trajectories remain unchanged.

The [internal diagnostic matrix](../../results/dw_fv_residual_2026-10-04/audit_checks/results.json) uses eight Y junctions with forced node-thread thresholds and observes actual node-solve teams of 1/4/8 workers. It records hexadecimal floating-point values for both reconstructed trial states, every trial head/residual, and final head, mass/momentum flux, corrections and published face states. Per-node ordering is retained while independent nodes may execute in different orders. Candidate runs match **1,221,192 trial-state pairs, 407,064 residuals and 88,776 final face records** against their corresponding baseline references. These are comparison counts across runs, not distinct physical configurations.

The [reuse test](../../results/dw_fv_residual_2026-10-04/reuse_checks/results.json) executes pressure, free-flow, reversed/mixed-section and TPA cases successively in each process. It changes mixed-wave handling, cell count and requested teams 1→4→8→1, observing those teams in the node solves. Each side produces 47,712 matching internal records. This specifically checks the thread-local scratch lifetime across engine destruction/recreation and reuse by different nodes/settings.

Diagnostic logging and exit guards are absent from the clean timed libraries. Policy values returned by the effective-thread API are not described as observed teams; the in-solve diagnostics provide that evidence.

These gates are substantial but not exhaustive. The retained new matrix has **zero rollbacks**; it does not prove a rejection/retry case was exercised. Held-face and culvert-specific internal coverage, the complete physical/transport/runtime-edit corpus, non-OpenMP builds and cross-platform checks remain open. Maximum reported continuity error in the new short matrix is 0.093% and is unchanged; that rounded report is not a machine-precision conservation proof. No new production regression test was installed for this unpromoted patch.

## Performance screen and confirmation

Both campaigns shuffle workload order and baseline A/baseline B/candidate order within each round. A/B use the identical baseline library. Three rounds give six baseline controls and three candidate observations per row. Measurements sum engine-step API calls; process CPU time is collected separately and includes initialization/reporting and all worker CPU usage. Profiling and diagnostic logging are disabled. No builds or source copying overlapped these timings.

| Workload | Workers | Screen change | Longer confirmation change |
|---|---:|---:|---:|
| Free flow, 4 cells/conduit | 1 | 10.7% faster | 7.4% faster |
| Free flow, 4 cells/conduit | 4 | 3.9% faster | 1.9% slower |
| Free flow, 4 cells/conduit | 8 | 0.8% slower | Not repeated |
| Free flow, 16 cells/conduit | 4 | 7.2% slower | 1.2% faster |
| Crown transition, 4 cells/conduit | 4 | 9.9% faster | 2.9% faster |
| At rest, 4 cells/conduit | 4 | Not in screen | 0.5% slower |
| Gated, 4 cells/conduit | 4 | Not in screen | 6.7% faster |
| Pass-through chain control, 4 cells/conduit | 4 | 11.6% faster | Not repeated |

The screen uses 120 simulated seconds for free flow, 30 for the transition, and 300 for the chain. Confirmation doubles the free/transition durations and adds 240-second at-rest/gated cases. Comparisons are always within identical configurations; results across different durations are not pooled. [Screen pairs](../../results/dw_fv_residual_2026-10-04/pilot/summary.json), [confirmation pairs](../../results/dw_fv_residual_2026-10-04/confirmation/summary.json).

In confirmation, single-worker median step time changes from 1.1449 to 1.0602 seconds. The four-worker free-flow median changes from 0.9529 to 0.9715 seconds, including one candidate/control pair that is 20.6% slower despite close A/B controls. The 16-cell median is 3.1289 versus 3.0905 seconds, but two of three paired candidate/control comparisons are slower. A favorable median alone does not settle those rows.

Screen process-CPU medians are about 7–10% lower for four-cell junction workloads, including the eight-worker case with no wall-time win. At sixteen cells they are essentially unchanged. These measurements suggest useful work reduction and a separate scaling limit; they do not establish its cause. Scheduler delays, other phases and region overhead require attribution before assigning blame.

The pass-through chain does not execute the new cache, yet its screen median appears 11.6% faster. This warns against attributing every binary-level timing change to reconstruction savings. Code layout and host variation are possible contributors, not measured explanations. The host remains shared: one-minute load averages span 17.5–28.0 in the screen and 14.7–19.9 in confirmation on ten logical CPUs. There is no verified core placement or exclusive CPU allocation.

## Next qualification step

1. Retain this cache as a numerically promising, single-worker performance candidate. Do not add it to production or change the acceptance rule merely to count the observed serial gain as multithread completion.
2. On a controlled host, repeat the same clean-library pairs at 1/2/4/8 workers, including four- and sixteen-cell workloads, at-rest and all-special-face controls. Observe actual teams separately and record wall/process CPU time. Expand to connected junction-rich production networks; independent Y branches are useful controls, not that coverage.
3. Distinguish worker compute savings from waiting and other-phase cost before adding more caching or parallel regions. If setup overhead matters for one-trial solves, test lazy activation after the first residual as a separate candidate; do not assume it caused the noisy rows here.
4. Before integration, complete the missing retry/held-face/culvert and broader feature gates, add a durable regression for solve-local/thread-local reuse, and validate against a fresh current-source baseline. The active shared checkout has advanced independently.

P0/P1 remain open, F2 has an isolated implementation candidate, and FV-versus-optimized-DW parity and strong/weak scaling targets remain unproved.

## Provenance and restoration

Accepted frozen library: SHA-256 `4fa4454c6f5dbe83c351bbce609af7092c10780ce50a9989584b050beef540bf`. Preparation recorded checkout HEAD `d4e4fef8` and 35 source differences from that snapshot; the FV implementation/header and profiling header matched. This is a frozen-source comparison, not a clean reconstruction of that commit. Apple Clang ARM64 Release/LTO and strict floating-point settings are unchanged.

[Source manifest](../../results/dw_fv_residual_2026-10-04/manifest.json), [clean binary hashes](../../results/dw_fv_residual_2026-10-04/binaries.json), [final validation/restoration](../../results/dw_fv_residual_2026-10-04/final_validation.json). Clean candidate and diagnostic libraries remain separate. The reusable source/build was restored and its library hash rechecked against the accepted baseline.

## Subsequent qualification

The [qualification and node-team report](DW_FV_RESIDUAL_QUALIFICATION_REPORT_2026-10-04.md) adds explicit synthetic rollback/retry checks and culvert fallback coverage, verifies the current held-face invariant, and measures worker CPU and barrier waiting separately. It preserves the decision to keep this candidate isolated; the original zero-rollback matrix above remains correctly labeled.
