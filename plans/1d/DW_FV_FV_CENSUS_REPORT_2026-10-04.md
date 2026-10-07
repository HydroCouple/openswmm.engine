# FV face-census threading experiment

Date: 2026-10-04. Status: **sampled exactness gates pass; candidate retained separately, not promoted**. This is F4 implementation/scaling work under the [approved program](DW_FV_PERFORMANCE_REVIEW_AND_PLAN_2026-09-26.md). P0/P1 remain in progress. The accepted DW correctness and FV due-cell threading commits remain the production baseline.

## Decision

Parallel evaluation can preserve the exact selected CFL face and timestep, including equal-bound ties, node constraints, pressure editing and boundary ghosts. The implementation passed the selected state, diagnostic and network checks. However, the 45-run timing pilot did not establish a reproducible end-to-end gain. Four-worker medians were slower on the native LTS and both retained conduit networks, while identical-baseline controls and round-to-round times varied considerably. Do not change the production solver or thread defaults on this evidence.

The clean candidate, diagnostic variants, inputs, scripts and all observations are retained in [the experiment directory](../../results/dw_fv_census_2026-10-04). No new solver change or commit was made in the shared checkout. The reusable isolated source/build was restored to the accepted baseline after testing.

## Implementation and numerical contract

The [candidate patch](../../results/dw_fv_census_2026-10-04/candidate.patch) changes only `ExplicitFvSolver::censusDt`. Each worker evaluates the original per-face expressions in active-face order and retains a local tuple: timestep, active-list position, reference length and speed. A once-per-worker synchronized merge chooses the smaller timestep, then the earlier active-list position for a tie. This reproduces the serial strict-less-than scan without a floating-point sum or changed arithmetic inside a face.

The original node-constraint pass, pressure classification, telemetry, fallback bound and census cadence remain intact. Worker-local `perf::CounterBatch` objects collect geometry counts before merging them. Small face lists and one-worker execution use a direct serial loop. The candidate provisionally reuses the existing `OPENSWMM_FV_OMP_MIN_FACES` threshold (default 4096); that threshold is **not qualified for census work** merely because flux work already uses it. Worker scratch is constant-sized; no face-sized bound array is introduced.

The reduction preserves the earliest face even when its worker arrives last. A local minimum is recorded only when the original strict comparison accepts it, preserving the initial maximum/no-winner behavior. This supports the design; the tested finite hydraulic cases are not exhaustive special-value or cross-platform verification.

## Validation

| Gate | Result |
|---|---|
| Selected CTest suites | 6/6 pass on the candidate |
| Clean-library native state checks | 120 runs, exact against each case's serial baseline |
| Direct census diagnostics | 28 runs; 579 candidate census decisions match their references |
| Network diagnostics | 9 runs; 10,870 candidate census decisions match their references |
| Clean-library timing pilot | 45 completed runs with matching sampled results/work schedules |
| Restored baseline suites | 6/6 pass |

The six suites cover FV LTS, networks, integration, implicit pressure, and profiling enabled/disabled. [Candidate test log](../../results/dw_fv_census_2026-10-04/tests.log).

The [120-run matrix](../../results/dw_fv_census_2026-10-04/checks/results.json) uses 1/2/4/8 workers. It covers open and circular sections, resting water, dry cells/fronts, first and second order, LTS, SLOT, TPA, implicit pressure and UF, plus an 8192-cell case exercising the default threshold. State traces include cell area/discharge/head, node volume/head, transported arrays, and reported timestep, tier-count, macro-cycle, rejection and argmin statistics after each advance. All profiled integer work counts match. These advance-boundary samples do not expose every internal tier array or intermediate state.

The [direct diagnostic checks](../../results/dw_fv_census_2026-10-04/diagnostic_checks/results.json) record every face minimum and final node-adjusted bound in hexadecimal floating-point format. They cover:

- A uniform tied-bound case with a deliberate delay before the earliest face's worker merges; face zero remains the winner.
- All-dry faces and a genuinely empty compacted active-face list; the latter retains the no-face fallback and uses one worker.
- A storage-node fixture in which the node bound owns all 16 census decisions per run.
- Implicit-pressure editing, TPA LTS and a wet/dry LTS front.

Diagnostic candidate libraries observe the actual census team inside the region: 1/4/8 workers where forced, with the empty list remaining serial. The separate native-probe team and the engine's effective-thread API are not substitutes for this observation. Diagnostic sleeps and logging are absent from the timed libraries.

The [network checks](../../results/dw_fv_census_2026-10-04/network_checks/results.json) exercise a 1024-conduit graded network at 4 and 16 cells per conduit over 300 simulated seconds, and 128 branched Y junctions over 5 seconds. Baseline at one worker and candidate at one/four workers match binary output, routing times, work counts, selected face tuples and final bounds. The graded cases exercise pass-through junctions; the branched case exercises ordinary node ghosts. Their candidate census teams are observed directly.

The initial network comparator stopped because an unanchored pattern mistook the suffix of a startup timer ending in `n.read` for an integer `n.*` counter. Output, schedules and every census line already matched. The corrected comparator anchors complete counter keys; the original failure log is retained. No engine change was needed. The first build command also named a CTest entry as a build target; the corrected build and test logs are retained.

These are selected same-platform gates. The full parity corpus, transport/runtime-edit matrix, non-OpenMP build, sanitizers and broad physical qualification were not rerun for this unpromoted candidate.

## Performance evidence

Three rounds shuffle configuration order and baseline A/baseline B/candidate order. A and B use the identical frozen library. Diagnostic instrumentation is absent and profiling is disabled. Timing sums solver advances for the native channel or engine-step API calls for networks. Native traces are written outside the timed advance. The network measure includes work inside the engine-step API. No builds or source copies overlapped this pilot.

| Workload | Workers | Baseline median (s) | Candidate median (s) | Apparent change |
|---|---:|---:|---:|---:|
| 16,384-cell native LTS, 32 advances | 1 | 1.427 | 1.430 | 0.2% slower |
| Same native LTS | 4 | 1.160 | 1.287 | 10.9% slower |
| Same native LTS | 8 | 1.834 | 1.632 | 11.0% faster |
| 1024 conduits, 4 cells/conduit | 4 | 1.266 | 1.403 | 10.8% slower |
| 1024 conduits, 16 cells/conduit | 4 | 3.760 | 4.991 | 32.7% slower |

These are screening observations, **not established intrinsic speedups or regressions**. The eight-worker baseline spans 0.960–3.098 seconds; one within-round A/B pair differs by 2.35×. The 16-cell network baseline spans 3.046–8.492 seconds. One-minute load averages range from 34.0 to 43.6 on the ten-logical-CPU host. No exclusive core allocation or verified placement was available. Every completed observation is retained in [raw timing results](../../results/dw_fv_census_2026-10-04/pilot/results.json) and the [paired summary](../../results/dw_fv_census_2026-10-04/pilot_summary.json). No longer confirmation campaign was justified.

A separate four-run [phase screen](../../results/dw_fv_census_2026-10-04/phase_screen/results.json), after timing, found that the census itself can become cheaper without a measured total-runtime win. At four workers, native census time changed from 0.1409 to 0.1161 seconds, and 16-cell network census time from 0.4811 to 0.3283 seconds. Total profiled step times were 0.7167/0.8136 and 3.1983/3.1986 seconds respectively. These single profiled pairs are diagnostic, not confirmation. Their counts match; the network still performs 3128 censuses over 54,452,224 face visits. Profiling overhead, scheduling and unmeasured contention prevent assigning the total-time difference to the new region alone.

This sets a useful scale for prioritization: census occupied about 15% of that baseline network step measurement. Eliminating a third of that phase would save only about 5% of the total if everything else stayed fixed. Threading this phase alone does not establish a route to universal FV/DW parity.

## Provenance and continuation

The frozen accepted library SHA-256 is `4fa4454c6f5dbe83c351bbce609af7092c10780ce50a9989584b050beef540bf`. The source manifest records checkout HEAD `8269dbc6` and 25 current-source differences from the accepted snapshot; relevant FV solver and profiling sources still matched. This experiment uses frozen source, not an asserted clean reconstruction of that commit. Compilation retains Apple Clang ARM64 Release/LTO, `-fno-fast-math`, `-ffp-contract=off`, and the prior strict section/Manning options. See [source provenance](../../results/dw_fv_census_2026-10-04/manifest.json), [binary hashes](../../results/dw_fv_census_2026-10-04/binaries.json), and [final validation/restoration record](../../results/dw_fv_census_2026-10-04/final_validation.json).

Next work stays inside the approved F4/P0 program:

1. Qualify the census crossover on a controlled host, with small-network regression controls and 4/8/16-cell network curves. Reusing a flux threshold is a hypothesis, not a policy decision. Keep this patch separate until the total-runtime gate passes.
2. Split tier-construction attribution before adding another region. `assignTiers` mixes independent cell/node bound evaluation, ordered minimum selection, algebraic-node feedback, tier grading and schedule construction. Measure their shares first; do not parallelize the entire routine from its aggregate timer.
3. Preserve the specific dependencies in a future tier candidate: retain cell-before-node tie precedence and per-node face accumulation order; keep algebraic feedback writes and grading ordered unless separately proven. The current bound vectors are `static thread_local`; a worker loop must receive a pointer/view captured from the calling thread, rather than directly naming those TLS vectors and accessing each worker's unrelated storage.
4. Complete representative DW profiling and broader P0 scaling controls alongside FV work. No FV-versus-optimized-DW parity, weak-scaling result or additional completed phase is claimed here.
