# Phase 12 — final cumulative CPU qualification

The planned CPU review is complete. This phase retains no new solver changes. It directly compares the final implementation with earlier references, reruns the matched full-engine regression suites, and consolidates the analytical and coupled evidence. GPU validation remains deferred.

## Direct cumulative CPU comparison

Reference: the corrected solver before CPU phase 1, recovered from the phase-6 start snapshot. Final: the phase-10 implementation, unchanged through phases 11 and 12. Both are freshly compiled against the same frozen phase-7 supporting engine objects and headers. The older solver header differs in private method declarations and comments, with no object-storage layout change. This isolates solver changes from unrelated concurrent engine work.

The three authored coupled workloads have 32,768 triangular cells, four threads, four local-time tiers, local-inertial momentum, one minute of rain/infiltration, a drain and pipe, and HDF5 output. Initialization and output are included in CPU time. Three randomized measured pairs follow one discarded warm-up pair per workload. Every pair has exactly matching API state/ledgers, normalized reports, and all HDF5 contents, including transported-species output.

| Species | Reference CPU median (s) | Final CPU median (s) | Median paired CPU reduction | Paired reduction range |
|---|---:|---:|---:|---:|
| 0 | 0.615 | 0.611 | -1.9% | -2.3 to 6.9% |
| 8 | 2.918 | 1.804 | 40.9% | 38.2 to 41.6% |
| 32 | 10.409 | 4.947 | 52.5% | 52.0 to 59.4% |

The water-only control shows no consistent improvement; its median paired CPU cost is slightly higher. These short runs do not establish a general water-only speedup. The species cases show substantial cumulative savings. Ratios are medians of paired ratios, which need not equal the ratio of the two reported time medians. These are new measurements, not compounded percentages from earlier phases.

## Accuracy per CPU time

The numerical reconstruction changed during phases 7–9, so an identical-grid speed comparison alone is insufficient. This separate reference is the phase-7 reconstruction, preserved as the phase-9 baseline. It is not the pre-CPU reference above. Both component solvers and identical harnesses were rebuilt. Each runs the same three-period Thacker analytical problem: reference at 128 divisions and final at 96 divisions, second order, one thread. The final mesh has 43.75% fewer cells.

Independent diagnostic runs record analytical depth error throughout the run; timing executables omit those history diagnostics. Three randomized measured pairs follow one discarded warm-up pair. Both endpoint and time-mean depth error must improve before accepting a cost comparison. This establishes lower cost at better **depth accuracy** for these four cases; it is not an equal-velocity-error or universal convergence claim.

| Bowl | Mesh | Reference endpoint / mean depth L1 | Final endpoint / mean depth L1 | CPU reduction | Paired reduction range |
|---|---|---:|---:|---:|---:|
| radial | triangles | 3.88% / 1.42% | 2.97% / 1.13% | 61.1% | 58.2 to 61.5% |
| radial | quads | 7.29% / 2.65% | 4.90% / 1.83% | 60.1% | 59.4 to 62.0% |
| planar | triangles | 7.84% / 4.38% | 6.30% / 3.42% | 59.9% | 59.0 to 61.9% |
| planar | quads | 10.70% / 5.95% | 9.51% / 5.26% | 60.9% | 59.3 to 61.3% |

All eight diagnostic runs keep nonnegative depth; maximum absolute relative volume error is 1.34e-14. Detailed velocity extrema are recorded, but the resolution comparison is selected by depth accuracy.

## Correctness and supported configurations

**Fresh this phase:** 169 passed, one skipped, zero failures across 15 suites. The skip is the unavailable Kokkos OpenMP comparison plugin. The selected solver library and focused correctness executable were rebuilt; other suite executables use the matching frozen support context. Loaded library paths are checked. Also fresh: 24 complete coupled runs (18 measured, six warm-ups), eight analytical diagnostic runs, and 32 analytical timing runs (24 measured, eight warm-ups).

**Evidence carried forward, not rerun:** source hashes confirm the phase-10 solver and kernel are still the implementation under test. Its 140 analytical executions cover bowls, wet/dry resting lakes, Ritter/Stoker dam breaks, regular/perturbed/mixed meshes, first/second order, one/four threads, and three/ten-period bowls. All 70 optimization pairs were exact. Its 24 full-engine executions cover FLAT/VFR storage, rain, infiltration, boundary conditions, groundwater, transport, drain/pipe coupling, and output; all 12 pairs matched. Phase-10 artifact hashes are verified by this phase’s final verification script.

The real Bellinge mesh loads 25,600 cells. The wet ten-minute cold-start interval advances 2,728 internal steps and 104,574,000 face evaluations, with 99.5% mean active cells. It has exact pre-/post-phase-10 state, ledgers, reports and HDF5 output. The separately timed early interval is dry. There is no repeated wet-Bellinge performance claim or calibrated physical-accuracy claim. Authored 32,768-cell wet coupled models previously showed 12–19% end-to-end CPU savings for phase 10 alone.

| Configuration | Qualification / remaining limit |
|---|---|
| CPU first order, local timestepping, surface transport | Regression and exact coupled-output coverage; cumulative species savings above |
| CPU FULL_SWE second order, water only | Analytical and wet coupled coverage; global one-tier stepping |
| FLAT / VFR storage | Lake-balance and coupled-model coverage |
| Groundwater with first order | Supported regression/coupled coverage |
| Groundwater with second-order RK2 | Explicitly rejected during initialization; not supported |
| Surface transport requested with second order | Falls back to first order; not second-order transport qualification |
| GPU / other CPU architectures | Unvalidated here |

## Remaining limitations and disposition

Host load was uncontrolled: the one-minute load average ranged from 7.4 to 13.5 during the new timings on this ten-logical-CPU Mac. Runs were serialized within this review; unrelated workloads were not stopped. CPU time is emphasized over wall time, and all pairing/load data are retained. OpenMP thread count is not a promise of speedup. Earlier larger water-only cases also showed small CPU regressions; those remain a reason to benchmark the intended deployment workload.

Long-period numerical damping remains, particularly on coarse meshes. General arbitrary-mesh stability, second-order convergence, production-catchment accuracy, GPU equivalence, and performance on other machines are not established. Full-engine qualification uses internally consistent frozen support code; it does not certify unrelated current 1D edits or a build of the entire dirty checkout. The preexisting pending rain-activation hunk was present in the final solver snapshots and remains untouched and uncommitted.

Phase 11 rejected all five extra hotspot prototypes because their gains were small or inconsistent. No additional CPU phase is required by this review plan. Future work should be driven by a concrete deployment workload, a new measured bottleneck, or available GPU hardware. No commit was made in phase 12.

## Reproduction

Run from the engine repository root. `prepare.py` assembles ignored snapshots from earlier local review snapshots and prepares the coupled decks; `build_engine.py start` and `build_engine.py selected` build matched full-engine variants. `build.py baseline`, `build_variant.py selected`, and `build_performance.py baseline selected` build component variants. Run `check_suites.py selected`, `cumulative.py`, `accuracy_cost.py`, then `finish_review.py`, serially. The local frozen sources/support objects and toolchain recorded in the command manifests are prerequisites; this is not a portable standalone benchmark package.

The initial cumulative warm-up stopped because the inherited deck disabled HDF5 output. Output was explicitly enabled, and the full comparison was restarted; the successful counts above exclude that setup attempt. Source snapshots, libraries, model decks/outputs and detailed history files are ignored local artifacts. Scripts, aggregate results, regression output, command records and source/evidence hashes are retained for review.
