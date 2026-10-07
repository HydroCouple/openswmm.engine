# FV tier-construction attribution and next target

Date: 2026-10-04. Status: **attribution checkpoint complete; no new production optimization adopted**. This follows the [census experiment](DW_FV_FV_CENSUS_REPORT_2026-10-04.md) under the approved P0/F4 program. P0/P1 and the broader performance/scaling gates remain open.

## Decision

Do not add a parallel region to tier construction yet. Its independent cell-bound loop is real work, but its share of total network runtime is too small to prioritize another fork/join optimization under the current measurement conditions. The nested update lists are particularly poor targets: they consume about 1% of tier CPU time on the retained large cases.

The next implementation candidate is the approved **F2 solve-local cache for invariant inputs to algebraic-junction residuals**, preceded by a focused cost measurement. This applies to junction-rich networks. The chain cases instead remain dominated by cell updates and flux evaluation; F2 must not be presented as a universal FV speedup. No method, timestep, LTS-fit option or thread default was changed.

Diagnostics, source snapshots, libraries, fixtures and results are in [the experiment directory](../../results/dw_fv_tier_2026-10-04). Production solver files are unchanged. The reusable isolated source/build was restored to the accepted baseline.

## What was measured

An isolated diagnostic splits `assignTiers` into twelve consecutive regions: scratch setup, cell bounds, node bounds, feedback bounds, initial classification/structure pinning, grading, maximum-level selection, dry/algebraic-node pinning, face tiers, per-tier buckets, nested due sets, and accumulator reset. It also counts calls, grading passes/face visits, bound evaluations, bucket/due-set entries and returned tier counts, including early returns.

The first variant measures elapsed wall time. A second measures both elapsed time and the calling thread's CPU time through `CLOCK_THREAD_CPUTIME_ID`. Tier construction is serial in this baseline, so calling-thread CPU time covers its computational work and excludes descheduling. It still includes cache/memory stalls while scheduled, frequency effects and instrumentation overhead; it is not a controlled-host benchmark. Totals are thread-local and printed once at thread exit. Multiple engines on one thread would aggregate; these workers run one solver each.

A retained calibration measured about 161 ns of CPU time per paired wall/CPU clock read over 100,000 pairs. At roughly thirteen pairs per tier assignment this suggests about 1.3 ms of clock-read cost for the 639-assignment case, before other instrumentation overhead. This is an estimate, not a subtracted correction. Very small phases and millisecond fixtures should be interpreted primarily through counts.

**56 completed runs** include 38 baseline/wall-profile runs and 18 baseline/CPU-profile runs. The initial matrix covers native LTS at configured 1/4/8 workers, graded 4/16-cell conduit networks at 1/4, wet/dry and TPA native cases, and branched controls. The three large four-worker configurations have three wall-profile observations and two CPU-profile observations each. Prior logs were reused when adding fixtures; they are not counted as new executions. [Machine-readable summary](../../results/dw_fv_tier_2026-10-04/summary.json).

## Findings

The percentages below are the median phase shares of **tier CPU time** over two diagnostic runs, at four configured workers. They are not shares of the whole solver and are not speedup estimates.

| Tier phase | Native 16,384 cells | Network, 4 cells/conduit | Network, 16 cells/conduit |
|---|---:|---:|---:|
| Cell bounds | 32.2% | 31.8% | 29.5% |
| Initial classification | 14.4% | 10.8% | 11.7% |
| Ordered grading | 26.8% | 30.5% | 32.9% |
| Per-tier buckets | 15.3% | 13.4% | 14.5% |
| Face tiers | 6.3% | 5.7% | 5.8% |
| Nested due sets | 0.8% | 1.1% | 0.8% |

The graded networks have 1024 authored conduits, simulate 300 seconds and preserve the prior fixtures' short stiff segment. In the wall-only repeats, tier construction occupies 2.5–5.6% of total profiled step time at four cells per conduit, and 7.7–12.4% at sixteen. The native channel's share is 12.7–17.2%. These are observed ranges under contention, not portable bounds.

For scale, even an ideal fourfold acceleration of a cell-bound phase occupying about 30% of tier time would remove only about 22.5% of tier time. Applied illustratively to the largest observed network tier fraction, that is under 3% of total step time before region overhead. Combining several independent loops could raise the ceiling, but grading, bucket order and synchronization remain. This does not justify a large persistent-team rewrite from the tier profile alone.

**The due-set duplication hypothesis is weak on the retained chain cases.** The four-cell case constructs 2,488,563 bucket entries and 2,546,640 nested entries in total; the sixteen-cell case constructs 22,248,063 and 22,707,504. Most work is at coarse levels, so nested lists add about 2% more entries here rather than multiplying the entire mesh by six tiers. Other tier distributions can differ. The native case adds about 9%; the short branched case about 2.35 times as many nested entries, but list construction is still small in absolute time.

**Grading is a substantial serial component.** The native case runs 1184 sweeps over 296 assignments (four per call), and both graded networks run five sweeps per call. The sixteen-cell network visits 55,618,560 faces during grading. The loop updates both incident tiers in place, so a direct parallel-for would race and alter propagation order. Any future alternative must demonstrate identical final tiers, list ordering and scheduling, including structure pinning, feedback bounds and cap overrides.

**Wall and CPU measurements differ materially.** One sixteen-cell diagnostic spends 0.876 seconds in the measured tier regions but only 0.429 seconds of calling-thread CPU time. Its repeat spends 0.334/0.330 seconds. The CPU breakdown supports the broad ranking despite this variation; neither pair is a speedup result. Network worker metadata and retained system load observations remain available in each result. Original load observations were unavailable for some reused native logs and are explicitly null rather than replaced by the load at re-analysis time.

## LTS eligibility and routing-window controls

The branched family consists of 128 independent Y junctions, with ordinary node ghosts rather than pass-through nodes:

- With LTS disabled, no tier constructor runs. Absence of a tier-profile record is the expected control result.
- With equal conduit lengths and LTS enabled, 20 tier assignments all return one tier; there are no macro cycles.
- Shortening one leg in each Y produces 27 four-tier assignments, but still no macro cycles with the 0.25-second routing window. Schedule construction alone does not prove LTS was used.
- A separately configured five-second routing-window control produces six four-tier assignments and five accepted macro cycles. It validates the active scheduling path.

Baseline and instrumented libraries match **within each configuration**. The longer-window case is not a same-configuration performance optimization or authorization to change shipping numerical defaults. The short branched runs are coverage controls, not statistically qualified performance workloads.

## Numerical and instrumentation validation

All 56 runs match their corresponding frozen baseline's sampled state/output, routing times, substep/flux/macro counts and integer FV profiling counters. Tier work counts and returned-tier histograms also match between diagnostic variants, thread configurations and repeats for each unchanged case. Region sums fit inside the existing tier timer to its printed precision. Native traces retain the prior full-cell/node arrays and reported scheduling statistics at advance boundaries; they do not expose every intermediate private tier array.

The six selected suites pass on **both** the wall-only and CPU-instrumented libraries: FV LTS, network, integration, implicit pressure, and profiling enabled/disabled. See [wall-profile tests](../../results/dw_fv_tier_2026-10-04/tests_profile.log) and [CPU-profile tests](../../results/dw_fv_tier_2026-10-04/tests_cpu.log). No full corpus, sanitizer, non-OpenMP or cross-platform qualification is claimed.

The first campaign stopped when it expected a tier record from the deliberately LTS-disabled branched fixture. The harness was corrected to accept the absence only for that named control; the original log is retained. No numerical mismatch or engine fix resulted. All profiled workloads were run without overlapping our builds or source copying.

## Next candidate: bounded F2 work

Independent source inspection reconfirms that mass-only residual trials and fused/cached cell closure values already exist. The remaining repeated work is in `computeFaceFlux`: it still calls `faceSide` twice to obtain bed positions, then reconstructs the cell and ghost sides for every live-face trial. A cache must therefore demonstrate savings in this remaining work; it cannot claim to introduce the existing closure cache or skip momentum that is already skipped.

The short graded Y fixture records 6912 algebraic solves, 75,776 residual evaluations (10.96 per solve), and 227,328 trial face-flux evaluations. Its wall-only node-solve timer accounts for roughly a quarter of step time. The tiny runtime and host variation make this a targeting signal, not an accepted performance projection.

Next steps within F2:

1. Measure invariant bed/cell-side reconstruction versus changing-ghost reconstruction and Riemann evaluation on a longer junction-rich workload, including pressure transitions. Use coarse aggregate or sampled diagnostics; do not put a clock around every cheap side call and treat that inflated cost as the opportunity.
2. Prepare immutable per-incident-face inputs once **within a single node solve**. Begin with ordinary node faces and preserve the existing path for gates, culverts and pass-through special cases. Keep scratch private to the solving worker; no shared cache keyed only by section or conduit.
3. Preserve left/right orientation, the original `max(zl,zr)` order, ghost velocity, geometry identity, TPA/pressure flags, dry rules, face liveness, residual accumulation order and every head trial. Leave final full-flux publication and degree-one fallback behavior intact.
4. Compare trial-head/residual sequences and final face state directly, then state/schedules across worker counts. Retain wet/dry, RK2, TPA, mixed sections, reversed orientation, held LTS faces and rollback coverage before promotion. Expected reductions in reconstruction counters must be distinguished from unchanged hydraulic work and accepted trajectories.
5. Run clean interleaved controls only after that gate. Require an actual whole-solver benefit on the target class without unexplained regressions; do not promote from call counts alone.

For F4, the independent-bound and classification opportunities remain documented, including the caller-owned views needed for the current `static thread_local` scratch vectors. Persistent-team work should be justified by phase and waiting costs across the complete advance, rather than this routine in isolation. The primary four-cell FV versus optimized-DW target and broad strong/weak scaling qualification remain open.

## Provenance

The experiment starts from the accepted frozen library SHA-256 `4fa4454c6f5dbe83c351bbce609af7092c10780ce50a9989584b050beef540bf`. The active checkout HEAD at preparation was `e830b754`; current-source drift is recorded rather than folded into the comparison. Strict Apple Clang ARM64 Release/LTO arithmetic settings are unchanged. The two diagnostic snapshots each change only the FV implementation file; production FV/header/profiling source hashes remain those of the accepted baseline. [Manifest](../../results/dw_fv_tier_2026-10-04/manifest.json), [binary/probe provenance](../../results/dw_fv_tier_2026-10-04/binaries.json), [final restoration and validation](../../results/dw_fv_tier_2026-10-04/final_validation.json).
