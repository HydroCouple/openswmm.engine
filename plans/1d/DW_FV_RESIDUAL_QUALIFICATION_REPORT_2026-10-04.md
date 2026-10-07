# FV residual-cache qualification and node-team attribution

Date: 2026-10-04. Continuation of the [F2 residual-cache experiment](DW_FV_RESIDUAL_CACHE_REPORT_2026-10-04.md). The production solver is unchanged. The candidate remains isolated pending a reproducible multithread performance gain and broader integration gates.

## Retry and feature evidence

[Reproducible runner](../../results/dw_fv_residual_qualification_2026-10-04/checks.py), [instrumentation generator](../../results/dw_fv_residual_qualification_2026-10-04/prepare.py), [results](../../results/dw_fv_residual_qualification_2026-10-04/checks/results.json).

Thirty-two runs cover eight configurations, each using baseline at one worker and candidate at observed teams of one, four and eight workers. The diagnostic builds extend the previously retained internal trial/residual/final-face audit with complete cell area, discharge, depth and velocity at each restore and routing-step publication. Per-node traces retain their local ordering; cell/rejection traces retain global ordering. Values are finite and hexadecimal floating-point records match exactly. Sampled binary output, routing-step times and unchanged-work counters also match.

| Configuration | Required path observed per run |
|---|---|
| Free flow, Euler | One forced global rejection and restore |
| Free flow, RK2 | One forced global rejection and restore after the two-stage operator |
| Crown transition, TPA | One forced global rejection and restore |
| LTS | One forced macro rejection and restore; five subsequently accepted macros |
| One culvert at each junction | Special-face fallback alongside cache-eligible ordinary faces |
| All three incident faces culverts | All-special fallback control |
| LTS with a culvert | Special-face fallback and five accepted macro cycles |
| LTS without injection | Five accepted macro cycles; no rollback |

Across candidate comparisons this adds 306,792 trial-state pairs, 102,264 residuals, 26,640 final-face records and 36,288 full-cell records. Twelve candidate runs exercise a rejection; each has exactly one `n.rollback`, including three with `n.macrorej = 1`. Each restore emits all 96 conduit cells. These are comparison counts, not distinct physical scenarios.

**Fault injection is explicit and test-only.** The global probe replaces one post-step stability bound with at most one quarter of the attempted step, exercising the existing restore/shrink/retry logic. The macro probe rejects one completed macro cycle after evaluating its ordinary post-cycle census, exercising existing accumulator clearing and global fallback. Both versions receive identical injection, once per fresh process. No fault-injection code or environment option is added to production or to clean timing libraries. This proves cache independence across the exercised restore paths; it does not establish a naturally triggered hydraulic instability or validate the physical rejection criterion. The existing acoustic TPA test explicitly reports zero rollbacks and is not counted as retry evidence.

The culvert diagnostics prove that the marked faces reach the special-face residual path. They do not assert that inlet-control caps bind in these gentle-flow fixtures. The existing culvert physical integration test remains separate evidence.

## Held-face invariant

The earlier report listed held-face coverage as missing. Source inspection narrows that requirement: `assignTiers` pins each algebraic node to its finest incident cell; each face uses the minimum tier of its cell and node. Consequently all incident faces of an algebraic junction have the same tier and fire together. Global stepping marks all faces live. The residual still supports a held flux defensively, and the cache remains behind the unchanged live/active predicates.

A diagnostic at every residual records any incident face for which `faceIsLive` is false. All 32 runs, including accepted and rejected LTS cycles, produce **zero held-face records**, consistent with that invariant. We did not perturb tier pinning just to manufacture coverage. A future scheduler that allows mixed live/held incident faces needs its own regression before adoption; these results do not test such a scheduler.

## Node-team attribution

Attribution results are recorded separately from clean timing measurements. Instrumentation is never an acceptance benchmark.

The [attribution runner](../../results/dw_fv_residual_qualification_2026-10-04/attribution.py) executes 36 runs: baseline/candidate, two shuffled rounds, 1/4/8 observed workers, and free flow at 4/16 cells plus a four-cell at-rest control. Each simulates 60 seconds on 256 independent Y junctions. [Per-run records](../../results/dw_fv_residual_qualification_2026-10-04/attribution/results.json), [row medians](../../results/dw_fv_residual_qualification_2026-10-04/attribution/summary.json).

The diagnostic replaces the combined node-loop pragma with an explicit team, the same dynamic chunk size of two, and an explicit terminal barrier. Padded per-worker samples record thread CPU through the work loop, wall time inside individual node solves, and time between a worker finishing its loop and leaving the barrier. The caller records elapsed time around the team. Logging happens after that interval. The one-worker control deliberately enters this diagnostic team too, unlike the clean production serial branch. Allocation, clocks, the changed region structure and logging mean these are **attribution measurements, not production speedups**. Thread CPU includes scheduling and clock overhead; solve wall time includes descheduling. The barrier measurement includes delayed arrival and wake-up, not just intrinsic barrier cost.

All 36 instrumented runs match sampled output, routing times and substep/flux/macro counts against three corresponding clean-library controls. Actual node teams are checked in every recorded region. No compilation overlaps the attribution campaign.

| Workload | Workers | Baseline / candidate node-loop CPU (s) | Baseline / candidate team elapsed (s) | Candidate maximum barrier wait / team elapsed |
|---|---:|---:|---:|---:|
| Free, 4 cells | 1 | 0.143 / 0.130 | 0.324 / 0.360 | 0% |
| Free, 4 cells | 4 | 0.149 / 0.119 | 0.721 / 0.970 | 55% |
| Free, 4 cells | 8 | 0.153 / 0.118 | 0.861 / 0.945 | 62% |
| Free, 16 cells | 4 | 0.260 / 0.199 | 1.271 / 0.723 | 50% |
| Free, 16 cells | 8 | 0.251 / 0.196 | 1.577 / 1.487 | 58% |
| Rest, 4 cells | 8 | 0.034 / 0.036 | 1.247 / 1.003 | 67% |

Times are medians of two per-run sums. The wait column sums the longest observed worker wait in each region, then divides the row medians. It is a diagnostic of delayed completion, not a disjoint decomposition of total time or a speedup opportunity of that size.

In every free-flow row, node-loop CPU decreases, while elapsed time remains inconsistent. In the eight-worker four-cell candidate, the summed longest worker solve chains account for only about 15% of team elapsed time; the longest barrier waits account for about 62%. The at-rest control has little computational work and no convincing CPU gain. This supports separating useful computation from waiting rather than adding workers or assuming the cache's arithmetic savings translate directly into latency.

Host load averages range from **56.6 to 67.8 on ten logical CPUs**. Even the one-worker free-flow baseline spends 0.324 seconds of team wall time for 0.143 seconds of loop CPU. The instrumentation measures substantial off-CPU time, but cannot distinguish external scheduler contention, worker wake-up and intrinsic OpenMP overhead sufficiently to select a production scheduling fix. A controlled-host replay is required. These runs do not justify changing thread thresholds, adopting a persistent team, or rejecting the cache based on their elapsed ratios.

## Decision and next work

- Retain F2 as an isolated candidate. Retry/reuse evidence is stronger; synthetic rejection is now tested explicitly, and culvert fallback paths are observed.
- Treat held incident faces as a scheduler invariant to preserve, rather than a missing naturally reachable test for the current pinned algebraic-node schedule.
- Replay clean baseline A/B/candidate pairs on a controlled host at 1/2/4/8 workers, with observed teams in separate diagnostics. Keep four-cell production-network performance primary, with sixteen-cell and at-rest/all-special controls.
- Before further parallel changes, use the same worker CPU and barrier attribution on that controlled replay. Persistent teams and crossover-policy changes require evidence there; this host's waiting is not sufficient evidence for either.
- Complete connected production-network coverage, broader feature/transport/runtime-edit gates, non-OpenMP/platform checks, a durable integrated reuse/retry regression, and a fresh current-source comparison before promotion. Inlet-control cap activation remains a separate physical check. No new performance phase is declared complete, and FV versus optimized DW parity remains unproved.

## Reproducibility and restoration

All new inputs, logs, patches, frozen diagnostic libraries and scripts are under `results/dw_fv_residual_qualification_2026-10-04`. The original clean cache libraries are untouched. Both qualification and attribution derive from the same frozen baseline/candidate pair as the prior report; fault injection is absent from attribution. The reusable source/build is restored to the accepted baseline after the experiments. [Final validation and hashes](../../results/dw_fv_residual_qualification_2026-10-04/final_validation.json).

Production solver files and numerical defaults were not changed. No commit or push was created.
