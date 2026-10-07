# DW geometry synchronization experiments

Date: 2026-10-04. Status: **two candidates tested; neither promoted**. Production solver source is unchanged. P0/P1 remain in progress; the previously committed DW correctness and FV due-cell threading changes remain the accepted baseline.

This continues D1/D4 of the [approved performance program](DW_FV_PERFORMANCE_REVIEW_AND_PLAN_2026-09-26.md), following the [CSR/threading audit](DW_FV_DW_D1_REPORT_2026-10-04.md). It independently tests the next geometry/synchronization opportunity rather than assuming fewer passes or barriers must improve performance.

## Decisions

**Hold the full loop fusion.** Moving the SLOT width overrides into conduit classification preserved the sampled numerical results and eliminated a pass/barrier, but its initial timing improvement did not survive confirmation. The 8,192-conduit four-worker case lost four of five confirmation pairs. Code layout, register pressure and lost loop optimization are possible contributors, not measured explanations; contention prevents attributing the change reliably.

**Hold the narrower barrier-only patch for controlled timing.** Keeping both original loops and adding `nowait` passed expanded parity checks, including eight-worker cases with fewer conduits than workers. Its 90-run timing screen still had wide identical-baseline variation and apparent regressions above the 5% guardrail. Favorable rows are not accepted speedups.

Do not change thread defaults or replace the accepted solver from these results. The frozen candidates, diagnostic sources, inputs and results remain reviewable for a quiet-host qualification.

## Why the narrower change can preserve dependencies

The SLOT override loop and the following classification loop both use `schedule(static)`, the same conduit iteration count, the same enclosing team and no SIMD construct. Under those conditions OpenMP guarantees matching iteration ownership. Classification reads only its own conduit's working widths; the preceding shape-kernel barrier and the closing classification barrier remain. The candidate therefore removes the intermediate wait while preserving each worker's producer-before-consumer sequence. Future changes to either schedule or iteration space must recheck this dependency. [OpenMP worksharing-loop specification](https://www.openmp.org/spec-html/5.0/openmpsu41.html).

Full fusion instead moves the width calculations before the existing bypass check in classification. That position matters: the old width loop also updated bypassed links. Neither candidate changes width formulas, accumulation order, routing configuration, iteration limits, CFL rules or mesh resolution. All compilation keeps the prior strict floating-point settings.

## Numerical validation

| Gate | Full fusion | Barrier only |
|---|---|---|
| Selected suites | 6/6 pass | 6/6 pass |
| Public binary64 state traces | 48 exact runs | 48 exact runs |
| Core corpus | 25/25 identical output files | 25/25 identical output files |
| Internal geometry comparisons | 53,552 candidate Picard snapshots exact | 80,328 candidate Picard snapshots exact |
| In-team worker observations in geometry checks | 1/4 | 1/4/8 |

The six suites cover routing, virtual junctions, DW TPA, DW unsteady friction and both cross-section parity suites. Public traces cover SLOT, EXTRAN, dynamic slot and TPA, UF on/off, and forced 1/2/4 workers. Every sampled elapsed time, node depth and link flow matches its frozen serial reference.

The new diagnostic snapshots record all working widths, conduit depths, surface-area contributions, heads, `fasnh`, bypass flags and flow classifications immediately after classification. They compare baseline and candidate across valve closure, mixed open/closed sections, near-crown/dry initial states, and a targeted surcharged bypass fixture. That fixture forces additional iterations while a quiet branch converges: each run has 6,983 Picard snapshots and 7,084 bypassed-link visits. All values remain finite and every recorded byte matches. The barrier-only eight-worker cases include empty work assignments and uneven conduit distribution.

The full-fusion geometry campaign has 28 baseline and 28 candidate runs. The barrier-only campaign reuses the same 28 frozen baseline traces and adds 42 candidate runs; reused references are not new baseline executions. These diagnostics add a snapshot synchronization point after an existing classification barrier and are never used for timing. The clean timed libraries contain no snapshot instrumentation.

Both corpus runners exit 1 because the separate broader 2D script is not executable. The 25 core decks include two coupled 2D cases, but neither run qualifies the complete surface solver. These are selected same-platform gates, not exhaustive private-state, feature or cross-platform validation. No new production test or solver change was adopted.

## Performance evidence and limits

Each timing round shuffles configurations and three labels: baseline A, baseline B (the same binary) and candidate. Five rounds produce ten baseline controls and five candidate observations per row. All observations are retained. Measurements sum engine-step API calls, including output/statistics work within those calls, with passive waiting and zero block time.

The initial fusion screen has 90 runs at 1,024/8,192 conduits and 1/4/8 workers, simulating 120 seconds. A 75-run confirmation doubles the simulated duration and adds EXTRAN controls. The barrier-only screen has another 90 runs using the initial matrix: **255 clean-library timing runs in total**.

Selected results illustrate why the first favorable medians were insufficient:

| Candidate/campaign | Workload | Baseline median seconds | Candidate median seconds | Apparent change |
|---|---|---:|---:|---:|
| Fusion, initial | SLOT, 8,192 conduits, 4 workers, 120 s | 1.829 | 1.508 | 17.6% faster |
| Fusion, confirmation | Same topology/workers, 240 s | 2.443 | 2.535 | 3.8% slower |
| Fusion, confirmation | SLOT, 1,024 conduits, 4 workers, 240 s | 0.649 | 0.686 | 5.7% slower |
| Barrier only | SLOT, 1,024 conduits, 1 worker, 120 s | 0.248 | 0.266 | 7.3% slower |
| Barrier only | SLOT, 8,192 conduits, 4 workers, 120 s | 4.653 | 5.104 | 9.7% slower |

**These are screening observations, not established speedups or intrinsic regressions.** For the last row, baseline times span 1.212–25.982 seconds and candidate times 1.163–9.379 seconds. Another barrier-only row appears almost 49% faster by median; that does not support a promotion when controls vary so widely and other gates fail.

The machine reports ten logical CPUs and eight performance cores. During the barrier-only campaign, one-minute load averages span roughly 34–95. A late aggregate system observation records zero idle CPU, load 68.47 and 14 GB of compressed memory. This corroborates contention at that instant; it does not measure per-trial utilization or establish ongoing swapping. No exclusive CPU allocation, verified core placement or hardware-counter attribution was available. Preparing the next source snapshot also overlapped part of the late fusion confirmation; that activity is disclosed, and those observations are retained. The later barrier-only timing campaign did not overlap our builds or source copying.

Thread counts from the engine API remain policy predictions. The separate diagnostic observations confirm actual 1/4/8-worker DW teams for the tested configurations; the timed runs do not contain in-team instrumentation. Neither strong/weak scaling targets nor FV/DW runtime parity are established here.

## Provenance and artifacts

Both experiments use the preceding accepted frozen baseline, library SHA-256 `4fa4454c6f5dbe83c351bbce609af7092c10780ce50a9989584b050beef540bf`. The checkout HEAD recorded at preparation was `99440dd0`; this is a dirty-source experiment, not a claim to rebuild that commit. Eight non-DW source files differed between the active checkout and frozen baseline. Their names and hashes are recorded; production DW source/header matched the reference. No integration into those concurrent edits was attempted.

- [Fusion patch, scripts and evidence](../../results/dw_fv_dw_fusion_2026-10-04): [audit](../../results/dw_fv_dw_fusion_2026-10-04/final_validation.json), [confirmation summary](../../results/dw_fv_dw_fusion_2026-10-04/confirmation_summary.json), [decision](../../results/dw_fv_dw_fusion_2026-10-04/promotion_decision.json).
- [Barrier-only patch, scripts and evidence](../../results/dw_fv_dw_nowait_2026-10-04): [audit](../../results/dw_fv_dw_nowait_2026-10-04/final_validation.json), [timing summary](../../results/dw_fv_dw_nowait_2026-10-04/timing_summary.json), [decision](../../results/dw_fv_dw_nowait_2026-10-04/promotion_decision.json), [host observation](../../results/dw_fv_dw_nowait_2026-10-04/host_snapshot.txt).

The reusable isolated source/build was restored to the accepted frozen baseline after the experiments, and its CLI and six selected test targets were rebuilt. See [restoration verification](../../results/dw_fv_dw_nowait_2026-10-04/restored_build.json). Candidate binaries and diagnostics remain separate.

## Next work within the approved program

1. Stop spending additional qualification runs on these micro-optimizations under saturation. Revisit the barrier-only candidate on a quiet host and a representative branched/mixed-shape network, with baseline controls. Do not promote it from dependency reasoning alone.
2. Continue P0 DW profiles on representative networks, including critical-depth/structure-heavy work, and distinguish useful CPU work from wait/imbalance. Uniform wet chains are useful controls, not the complete optimization target.
3. Resume FV F4 with the face-based CFL census and tier construction. `censusDt` currently computes face bounds serially and retains the first strict minimum in active-face order. Investigate parallel face-bound evaluation while preserving that exact winning face, its diagnostics, node bounds, ghost/pass-through treatment, counters and census cadence. This is an implementation/scaling experiment; timestep reuse or altered schedules remain separate F6 work. Validate before adopting.

P0/P1 remain open. P2–P4 and the release qualification are unchanged.
