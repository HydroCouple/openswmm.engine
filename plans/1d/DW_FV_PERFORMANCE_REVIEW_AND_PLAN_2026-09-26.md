# Dynamic-wave and 1D finite-volume performance: code review and proposed program

Original review date: 2026-09-26. Status updated 2026-10-04: **P0 correctness checkpoint and the first F4 due-cell threading change implemented; F1 sparse-LTS bookkeeping remains a separate experiment.**

See the [P0 implementation report](DW_FV_P0_IMPLEMENTATION_REPORT_2026-10-03.md) for the DW UF fix and corrected parallel FV counters, the [F1 experiment report](DW_FV_P1_LTS_REPORT_2026-10-04.md) for the unpromoted sparse-bookkeeping candidate, and the [LTS threading report](DW_FV_LTS_THREADING_REPORT_2026-10-04.md) for the latest integrated change. Separating the previously untimed LTS cell updates identified a better target: threading large due-cell batches reduced median engine-step time by about 13% at four/eight threads on the tested 16-cell conduit network, with exact sampled state and work counts. The four-cell case shows no reliable gain. Forty extended state checks and all six selected current-source integration suites pass. Broader scaling qualification, weak scaling and FV parity with optimized DW remain open. The historical review below retains its original source anchors and evidence limits; the checkpoint reports describe current execution.

The subsequent [DW D1/threading audit](DW_FV_DW_D1_REPORT_2026-10-04.md) tested the proposed dense CSR conduit index. It passed the sampled numerical gates but was not promoted: 90 clean timing runs did not separate a gain from identical-baseline variation on a heavily contended host. The audit also verified that the effective-thread API returns policy predictions rather than observed teams; separate in-team diagnostics confirmed 1/4/8 workers for the tested setup. Geometry traversal/synchronization and controlled scaling evidence now take priority over this small mapping change. No additional phase is complete.

The subsequent [geometry synchronization experiments](DW_FV_DW_SYNCHRONIZATION_REPORT_2026-10-04.md) tested full SLOT-width/classification fusion and a narrower same-schedule barrier removal. Both passed selected numerical gates, including per-Picard geometry snapshots and targeted bypass coverage, but neither cleared the performance gate across 255 timing runs. The fusion gain failed confirmation; barrier-only timings were dominated by large identical-baseline variation on a saturated host. Both remain isolated. The next work is representative DW profiling and FV face-census/tier scaling within the existing D4/F4 program; numerical timestep reuse remains separate. The isolated build was restored to the accepted baseline.

The subsequent [FV face-census threading experiment](DW_FV_FV_CENSUS_REPORT_2026-10-04.md) preserves sampled state and exact winning-face/timestep decisions across native and network checks, including forced 1/4/8-worker census teams, delayed tie winners, empty work and node bounds. It remains isolated: the 45-run pilot did not establish a total-runtime gain and several rows exceeded the regression guardrail amid substantial baseline variation. The next F4 step is finer attribution of tier construction before adding more parallel regions. P0/P1 remain open; the isolated build was restored to the accepted baseline.

The subsequent [tier-construction attribution](DW_FV_TIER_ATTRIBUTION_REPORT_2026-10-04.md) separates twelve phases and adds calling-thread CPU time to distinguish execution cost from descheduling. Across 56 matching baseline/diagnostic runs, cell bounds and ordered grading dominate tier work; nested due-set construction is about 1%. The primary four-cell network spends only 2.5–5.6% of profiled step time in all tier construction, so another isolated parallel region is deferred. The next candidate is F2 solve-local residual-input caching on junction-rich workloads, with focused cost and head-sequence validation first. LTS-window controls also distinguish built tiers from actually executed macro cycles. No production optimization or numerical-default change was adopted; P0/P1 remain open.

The subsequent [F2 residual-cache experiment](DW_FV_RESIDUAL_CACHE_REPORT_2026-10-04.md) implements a solve-local cache of ordinary faces’ bed and cell-side inputs. Sixty network runs, detailed trial/residual/final-face traces at observed 1/4/8-worker teams, and successive-engine reuse checks preserve the sampled numerical trajectory. A 108-run screen/confirmation supports about a 7% single-worker gain on the longer junction-rich fixture, but does not establish consistent multithread wall-time gains. The candidate remains isolated; production code is unchanged. Controlled scaling and missing feature/retry coverage precede integration, and no additional phase is complete.

Initial engine review: `swmm6_rel`, HEAD `c5d0b16362a17ebc8624e1244f5170ceae300c52`, plus the working tree. During the independent validation pass HEAD advanced to `e7c68b83a4f3a8eb0cbf99d0d17a3211da372f95`; this shared checkout is active. Benchmark repository initially inspected at `3058ca6`. Existing changes were preserved. The probe manifest records source and binary hashes; the recorded DW/FV solver and profiling-header hashes remained unchanged during the probes. This is a source review plus focused correctness experiments, **not a fresh performance baseline or full test-suite pass**. Source locations below refer to files inspected on this date.

The user confirmed that “subgrid” means **the 1D finite-volume cells within each conduit**. The 2D surface solvers are outside this program, except for regression protection of shared engine interfaces.

The subsequent [F2 qualification and node-team attribution](DW_FV_RESIDUAL_QUALIFICATION_REPORT_2026-10-04.md) adds 32 exact internal/output checks, including test-only global/RK2/TPA and LTS rejection paths plus culvert fallbacks. Source and runtime checks confirm that tier pinning keeps algebraic-node incident faces live together. Thirty-six diagnostic runs and three clean numerical controls separate lower node-loop CPU work from substantial waiting; host load reached 56.6–67.8 on ten logical CPUs. These are attribution measurements, not acceptance benchmarks. F2 remains isolated; controlled-host scaling, connected production coverage and broader integration gates remain open. No production change or phase completion is claimed.

## 1. Recommendation

Pursue two related but different programs:

1. **Dynamic wave (DW): keep the numerical method and configuration fixed; improve data access and execution.** Finish the conduit-dense working layout, reduce repeated shape-group packing and unnecessary whole-network work, and vectorize independent arithmetic under strict floating-point rules. Preserve the exact sequence of operations within each hydraulic calculation and each node accumulation.
2. **Finite volume (FV): first reduce the cost of advancing the existing mesh; then reduce unnecessary advances.** Retain the current section closure and conservation ledger. Concentrate on repeated boundary-face reconstruction, full-network work inside sparse local-time-step firings, and the serial portion of the tiered update. For stiff pressurized networks, complete the existing implicit-pressure direction before expecting refined FV to consistently compete with DW.
3. **Measure both against the same frozen base and compare FV against the improved DW as well.** A win against an old or poorly threaded DW build is insufficient.
4. **Treat multithread scaling as a primary design objective from the first phase.** Measure and reduce serial work, synchronization, imbalance, and memory traffic alongside single-thread cost. DW and FV must both scale within one simulation, including FV's refined and locally stepped meshes. Threading is not deferred until the serial optimization program is complete.

Start with a short measurement and correctness phase, followed by small, independently reversible optimization patches. Re-rank changes using current profiles. Existing closure and threading mechanisms were verified in the source; their historical speedups have not been independently reproduced and are not performance credits for this program.

**Independent follow-up changed the priority:** a frozen-library experiment reproduced DW unsteady-friction nondeterminism at two and four forced threads under both SLOT and EXTRAN. The matching UF-off controls were bitwise stable in the sampled API state. Establish a deterministic UF contract before reordering that momentum path. A separate probe compiled against the current FV headers validated mass-only flux identity on one million sampled state pairs. See §4.1 and the linked experiment record for limitations and artifacts.

**The target is credible for selected workloads, but universal FV ≤ DW runtime at arbitrary subgrid resolution is not a defensible promise.** A finer explicit mesh increases both the number of cells and, when the CFL limit binds, the number of steps. SoA reduces the cost of the work; it does not remove this stability constraint.

## 2. Numerical and comparison contracts

### DW: identical configuration, no added numerical drift

Use two distinct references:

- **Optimization reference:** the frozen current refactored engine. For the same executable environment, compiler, floating-point settings, inputs, configuration, and effective thread count, require bitwise agreement of the hydraulic state at every routing step. Compare time-step selection, iteration counts, bypass decisions, flow classification, control actions, and continuity ledgers as well as output series.
- **Legacy reference:** the pinned legacy engine on configurations it supports. Preserve every established agreement and introduce no new discrepancy. Keep existing failures explicit. Experimental refactored features without a legacy counterpart are checked against the frozen refactored reference and their physical tests.

This avoids confusing “unchanged by this optimization” with “all legacy parity is already solved.” The September 25 parity report records remaining failures and weaknesses in the comparator.

DW restrictions:

- Keep `OPENSWMM_FAST_XSECT_LOOKUP=OFF` and `OPENSWMM_FAST_MANNING_POW=OFF`, with the shipping strict FP configuration. Record actual compilation commands, not only preset names.
- Preserve `std::pow(rWtd, 1.33333)`. The truncated legacy exponent is part of the calculation. `r*cbrt(r)` changes the exponent as well as the implementation.
- Preserve divisions, parentheses, library calls, and threshold comparisons. Replacing `x/L` with `x*(1/L)`, enabling FMA contraction, or changing interpolation can move convergence and the variable-step trajectory.
- Preserve each node's accumulation order, including conduit-end order, conduit losses, structures, and outfall updates. No unordered floating-point reductions or atomic scatter.
- Preserve configured Anderson acceleration, node continuity, inertial damping, surcharge closure, loss handling, controls, and routing limits. Enabling acceleration or changing tolerances is not a DW performance optimization under this contract.
- SIMD may execute **different conduits in parallel** while retaining each conduit’s expression tree. It must not silently substitute approximate vector transcendental functions or reassociate a node sum.
- Exactness is assessed on the same platform/build environment. Cross-platform numerical agreement is a separate gate; different libm implementations are not assumed bit-identical.

### FV: separate implementation changes from numerical changes

| Class | Changes | Acceptance |
|---|---|---|
| A — identical calculation | Data layout, scratch reuse, invariant caching, ordered parallel execution, removal of demonstrably redundant work | Bitwise hydraulic/state agreement against frozen FV, identical accepted-step/tier decisions, thread determinism; existing correctness gates unchanged |
| B — equivalent discretization with changed arithmetic | Polynomial coefficient reformulation, different inversion initialization, controlled layout/kernel changes that alter rounding | Explicitly declared result changes; closure identities, analytic errors, conservation, convergence and laboratory gates pass |
| C — numerical method or schedule | Partial LTS windows, implicit/explicit scheduling, new nonlinear pressure solve, changes to CFL/order/coupling cadence | Dedicated experiment and reviewed accuracy/runtime tradeoff before adoption |

Class A is an **acceptance target**, not a guarantee inferred from a code refactor. A changed result must be explained or the patch withdrawn. It must not be relabeled after the fact merely to pass.

FV does not need to reproduce DW’s trajectory exactly. It must solve the intended equations conservatively and accurately. DW agreement is a useful network-level diagnostic, not the truth reference for bores, wetting fronts, or pressure transients.

### What “FV matches or beats DW with subgrid” will mean

- Primary comparison: original network topology, identical forcing, duration, structures, controls, reporting, and hydraulic parameters; DW on its authored links, FV with **at least four cells per conduit**, plus 8- and 16-cell refinement studies. Report actual cell counts and min/median/max cell lengths because `FV_CELL_LENGTH` can create more cells than the floor.
- One-cell FV remains a diagnostic row, never the headline success criterion.
- Secondary comparison: runtime required by each solver to meet a stated accuracy target. If DW uses virtual junction refinement, report that topology and cost separately.
- Measure both single-thread operation and each solver's best validated setting within the same CPU budget. Also include the shipping automatic thread policy. Record the actual team size; requesting eight threads does not prove eight were used.
- Report each model's ratio `R = FV time / DW time`, the geometric mean, upper-tail behavior, and total elapsed time. Do not average successful and failed runs together.
- Proposed milestone: `R <= 1` on the agreed representative four-cell production set, with all accuracy gates passing. A per-model claim requires that model to pass; a corpus-level average must not conceal slow or invalid cases. Higher resolutions receive their own runtime/accuracy curves.
- Compare against both frozen DW and the optimized DW candidate. If the starting ratio is `R0`, with speedups `S_FV` and `S_DW`, the new ratio is `R0*S_DW/S_FV`. Closing a 4× gap while DW improves 1.5× requires a 6× FV improvement.

### Multithread scaling is a separate success criterion

The program has three performance objectives: lower one-thread latency, better scaling within a simulation, and FV/DW runtime parity at the selected mesh resolution. Report all three; a faster serial baseline can lower the apparent parallel speedup while still improving every absolute runtime.

For each fixed model, mesh, and numerical configuration, record:

- Strong-scaling speedup `S(p) = T(1)/T(p)` and efficiency `E(p) = S(p)/p`, using the **same candidate build** at both thread counts. Record improvement over the frozen build separately.
- Matched-budget solver ratio `R(p,m) = T_FV(p,m)/T_DW(p)` for each tested core budget `p` and FV refinement `m`. Show actual worker counts and core placement; if a size gate caps one solver, label that row rather than claiming equal effective teams.
- Best runtime attainable by each solver within the same core budget, and runtime under the shipping automatic policy. FV must compete with a properly configured threaded DW.
- Routing-only and end-to-end scaling, worker utilization, barrier/wait time, workload imbalance, memory bandwidth, and absolute elapsed time. Parallel ensemble throughput is a separate experiment, not evidence that a single simulation scales.

Initial engineering targets, subject to the P0 measured serial fraction and hardware limits: at least **3× routing speedup at four comparable physical cores and 5× at eight** on sufficiently large compute-heavy cases. These are investigation targets, not promised results for small, bandwidth-limited, or structure-dominated networks. A missed target requires attribution to measured serial work, memory saturation, scheduling, or insufficient active work, followed by a decision on the next change. Do not declare scaling solved from one favorable deck.

The automatic policy should choose a measured crossover region: proposed gate **within 10% of the best validated thread-count runtime** on the representative platform/model classes, without an unexplained >5% penalty against one thread on small cases. Avoid blindly choosing every logical CPU. Cross-thread DW runs must preserve the same numerical trajectory; FV class A threading must also preserve state, accepted steps, tiers, retries and conservation.

## 3. What is already present in the source

### DW execution and data flow

`Router::step` initializes routing state and calls `DWSolver::execute`. A single OpenMP team surrounds the Picard loop. Each iteration initializes nodes, derives conduit depths, evaluates widths, classifies flow, evaluates areas/radii, applies pressure/loss/momentum calculations, gathers conduit contributions at nodes, evaluates structures and outfalls, and updates node depths/convergence.

| Component | Current implementation | Implication |
|---|---|---|
| Public state | `LinkData`, `NodeData`, and subtype arrays | SoA already exists; a wholesale AoS-to-SoA conversion is not the task |
| Conduit invariants | Dense `tile_*` arrays with explicit link and conduit-row maps | Most static-property hoisting is complete |
| Hot working state | Area/depth/velocity/flow arrays still sized and indexed by all links | Mixed layouts still create mapping, gather/scatter, and memory-traffic costs |
| Geometry | Shape-grouped triple kernels, team-sliced; bypassed groups packed | Geometry parallelization and bypass masking already exist |
| Momentum | One fused conduit pass with per-element category dispatch | Classification exists, but it is not a contiguous category-vector kernel |
| Node contributions | CSR gather in original per-node link order | Deterministic parallel accumulation already exists; retain it |
| Threading | Persistent team per routing step, size gate and Darwin scheduling policy | “Remove OpenMP” and “add a persistent team” are stale starting recommendations |
| Losses and nodes | Zero-loss latch; ordinary-node fast path; per-step node tile | Several previously proposed small optimizations are complete |

Key source anchors: `DynamicWave.cpp:497`, `:787`, `:900`, `:1246`, `:1655`, `:2329`, `:3326`; `XSectBatch.cpp:1279`, `:1335`, `:1404`.

The per-iteration sequence is not freely interchangeable. For example, node flow gathering needs the newly calculated conduit flows; outfall depths are refreshed after link flow calculation; bypassed links retain previously calculated state and derivatives. Optimizing by “skipping converged nodes” or rearranging these phases requires more proof than independent conduit arithmetic.

### FV execution and data flow

`Router::stepFv` assembles forcing and calls `ExplicitFvSolver::advance`. The solver refreshes boundaries and active lists, evaluates stability limits, and chooses global substeps or LTS macro cycles. The global update reconstructs states, computes face fluxes, solves node coupling, optionally applies the implicit-pressure update, limits positivity, and updates cells/nodes. A post-step stability check can restore a saved state and retry. The router publishes link/node results at the routing boundary.

| Component | Current implementation | Implication |
|---|---|---|
| Mesh and state | SoA cells/faces/nodes, conduit chains, fixed two-face cell incidence | Good foundation for contiguous cell kernels and deterministic gathers |
| Section closure | Shared `FvGeometry` blocks; 128-panel monotone Hermite closure or polynomial open section | Replacing Brent and deduplicating geometry are already done |
| Forward geometry | Cell A/T/I1 cache; fused `closureEval` | More caching must target remaining repetition, not duplicate this cache |
| Inverse geometry | Safeguarded Newton on the same closure, analytic polynomial inverse | Current inverse should be profiled; old Brent timing is not current cost |
| Node residuals | Mass-only flux trials, final full flux; heavy/light node split | These earlier plan items are complete |
| Pass-through nodes | Far-cell ghost and census treatment; lateral diversion with activation/spill bookkeeping | Do not propose these as new wins or bypass their conditions |
| Global threading | Parallel faces, cells, selected node solves, and some refreshes | Multiple separate regions remain; small calls still encounter runtime overhead |
| LTS | Tiered face/cell/node schedules and conservative accumulators | Face flux calculation can parallelize, but much of the tiered path remains serial/full-network |
| Pressure solve | Optional CPU implicit acoustic solve, chains via Thomas, branched components via CG | Existing route to removing pressure-wave CFL cost; still has a nonlinear convergence caveat |

Key source anchors: `Routing.cpp:1376`, `:1507`, `:1552`; `ExplicitFvSolver.cpp:404`, `:643`, `:1320`, `:1616`, `:3024`, `:3125`, `:3426`, `:3695`, `:3850`, `:4088`; `FvClosureKernels.hpp:233`.

## 4. Measurements that can and cannot be carried forward

The retained September 11–13 program is substantially newer than the May DW review and September 2 FV profile. The earlier “FV is dominated by Brent” and “FV node solves are serial” diagnoses no longer describe the source.

| Retained result | Interpretation for this program |
|---|---|
| September 11 closure/fixed-work round: roughly 4–6× improvement on several reach cases | Historical reported gain, not independently reproduced. The replacement closure exists in the current source; do not promise the same gain again |
| September 12 shared sections: TwinOaks 5,086 conduits reduced to 11 distinct geometry blocks | Do not count geometry deduplication again; assess hot scalar access and current table locality |
| September 12 shared sections/node threading: TwinOaks 116.7 s at 1 thread, 103.0 s at 2, 125.1 s at 8; East Boston 112.3/104.7/127.3 s | Thread count has a crossover, not a monotonic speed benefit; these are historical machine-specific results |
| September 12 census consistency: East Boston FV 60.5 s versus DW 64.4 s | Reported candidate crossover, not independently reproduced. The FV deck used a one-cell floor and still reported 0.797% continuity error; not proof of the requested refined-mesh target |
| September 13 fixes: East Boston continuity recorded as approximately zero | Accuracy improved later; the 60.5 s timing cannot be assumed to describe that later fixed binary |
| Shortened LTS-cycle experiment reduced some network times but failed the Aureli filling case badly | Existing `OPENSWMM_FV_LTS_FIT=1` must remain experimental; do not promote it as a safe quick win |
| TwinOaks v2 historically used hundreds of FV substeps per routing step; some DW runs had very poor continuity | Both step count and solution validity matter; a fast invalid DW result is only a diagnostic timing reference |

All numbers above come from `plans/FV1D_PERF_BASELINE_2026-09-11.md`, especially its later Phase 3, remainder, experiment, and September 13 sections. They are not measurements made by this review.

There is no defensible single current “FV/DW factor” until the source, libraries, settings, decks, and machine load are frozen. The dedicated performance suite in the benchmark repository is still a scaffold (`suites/performance/suite.py`). The usable engine-side starting point is `tests/benchmarks/scripts/fv_perf_baseline.py`, supplemented by the benchmark repository's numerical suites.

### 4.1 Independent evidence and hypotheses to challenge

Fresh experiment record: [independent validation and reproducible artifacts](../../tests/benchmarks/generated/dw_fv_review_2026-09-26/README.md). These experiments test narrow claims; none establishes a production speedup.

| Claim / candidate | Fresh evidence | Consequence / next falsifier |
|---|---|---|
| DW results are independent of thread count | **Contradicted for UF-on in the frozen existing library.** The completed 24-run passive-wait matrix covered SLOT/EXTRAN, UF off/on, forced 1/2/4 threads, two repetitions. UF-on repeated 2/4-thread states differed; UF-off controls and UF-on serial repetitions matched | Reproduce on an isolated fresh build, trace the first differing neighbor read, and separate the correctness fix from optimization. This is not a blanket failure of default DW |
| Neighbor data are previous-iterate snapshots | **Contradicted by inspected code:** UF reads `links.flow[unb]` while the momentum loop commits that array; local slot overrides also write neighbor-readable areas | Source comments cannot justify arbitrary scheduling. Define which iteration supplies each stencil input |
| FV mass-only trials retain the full flux's mass exactly | **Supported in a fresh strict-FP current-header probe:** zero bit differences in 1,000,000 sampled pairs covering dry, free, pressurized and vented tags | Retain this completed mechanism. The probe does not validate every gate/culvert/node branch or physical accuracy |
| Exact DW full-radius power reuse can avoid libm work | **Supported at the arithmetic-kernel level:** zero bit differences in 1,000,000 finite positive full-radius cases. The `r*cbrt(r)` negative control differed in every sample | Prototype only for identical actual pow arguments, then test full-state parity and measured hit rate. Savings are unmeasured |
| Two pass-through FV faces can share a mirrored flux unchanged | **Naive bitwise claim rejected:** 5,865 signed-zero mass differences in 1,000,000 mirrored pairs, despite zero numerical-value differences and matching momentum bits | Preserve baseline signed-zero/branch semantics and prove full reconstructed-face identity before reuse. Do not equate algebraic symmetry with bit identity |
| More threads always improve end-user latency | **Unproven.** Current engine code requests active/infinite waiting for eligible DW runs. The default-wait probe had one incomplete run; passive-policy probes completed, but this was not a controlled timing experiment | Benchmark wait policy and CPU use separately on idle and shared hosts. Do not attribute the timeout or claim a speedup from these observations |

Evidence labels for implementation decisions are **source verified**, **fresh kernel tested**, **frozen-library observed**, **historical/unreplicated**, and **hypothesis**. A profile from an older binary is not evidence that the same phase dominates now. For each proposed optimization, record the predicted work removed, a disconfirming test, measured absolute runtime at one and multiple threads, and whether it improves end-to-end runtime. A negative result should remove or demote a candidate.

## 5. Phase P0 — establish trustworthy evidence

Proposed effort: 3–5 engineering days, with corpus runtime dependent on the selected models. Deliver a baseline table and source/binary manifest before large kernel changes.

### P0.1 Freeze source, libraries, inputs, and effective options

- Use an isolated, managed checkout/build for implementation. Capture the reviewed working-tree differences deliberately rather than silently benchmarking clean HEAD while inspecting modified source. Preserve existing work in both repositories.
- Freeze both CLI **and the engine library actually loaded**, including dependency paths, hashes, compiler, architecture, optimization/LTO/FP flags, and runtime version. Copying only the thin CLI does not freeze the engine.
- Capture input/sidecar hashes, engine and benchmark commits, dirty diff, every relevant environment override, effective solver options, backend, and actual thread count. Include experimental switches such as `OPENSWMM_FV_LTS_FIT`, pressure secant passes, and DW bypass-mask overrides.
- Keep every run's inputs, reports, binary results, logs, and metadata in a persistent reviewable directory such as `tests/benchmarks/generated/dw_fv_perf_2026-09-26/`. Never overwrite earlier stage results.
- Make full builds and confirm test executables share the intended source/FP settings. The retained history contains several stale-library and shared-build confounds.

### P0.2 Correctness and instrumentation prerequisites

**DW optional unsteady-friction dependency — investigate before reordering momentum.**

`momentumKernels` is an OpenMP conduit loop and commits `links.flow[uj]` inside that loop (`DynamicWave.cpp:2476` vicinity). `processManningLink`'s UF neighbor stencil reads `links.flow[unb]` (`:2948` vicinity). The header describes previous-iterate/double-buffered input, but the inspected implementation reads the same flow array being updated. Neighbor area reads also need inspection because pressure overrides occur in the momentum pass. The independent frozen-library probe now supplies an observed nondeterministic result consistent with this dependency; it is not a ThreadSanitizer diagnosis or a fresh-build regression verdict.

The saved probe forces the team on a four-conduit fixture to expose cross-worker neighbors. Repeat it on a fresh build and add a larger case using the normal size gate, then trace the first divergence. A previous-iteration snapshot may change even the existing serial result, which currently observes some already-updated neighbors. Choose explicitly between preserving the serial UF ordering and correcting the stencil to its documented snapshot semantics; the latter needs a separate numerical baseline decision. A nondeterministic threaded run cannot serve as a unique exactness reference. The default UF-off path does not execute this stencil.

Also audit `traceLinkTerms`' mutable function-static initialization (`DynamicWave.cpp:3019` vicinity). Its `lf_target == -2` check/write can be reached by multiple workers even when tracing is disabled. Initialize diagnostic configuration before entering the team, and isolate per-engine trace state. This additional source-level race has not been isolated experimentally here.

**FV profiling counters — make parallel measurements well-defined.**

`PerfTimers.hpp:324` performs plain increments of shared `long` counters. `cacheClosure`, face flux, and algebraic node paths call it inside parallel loops. With profiling enabled these are C++ data races, not merely approximate counters. Use solver-owned per-thread counters reduced after phases, or loop-extent integer reductions. Avoid an atomic increment for every inner geometry operation. Check instrumented versus uninstrumented hydraulic results and measure instrumentation overhead separately.

**Strengthen the parity oracle.**

The benchmark comparator's subtraction/threshold checks can ignore finite/NaN mismatches (`harness/compare.py:248`). Validate return status, binary footer, expected end time, period count, entity/variable identity, and finite-value masks before numerical comparisons. Distinguish NaN/Inf/sign cases explicitly. A timeout, unreadable output, missing period, or failed binary is never a fast successful row.

Use byte identity where the binary format permits it and exact decoded results otherwise, documenting excluded non-numerical metadata. A float32 report stream alone can hide double-precision state drift: add an opt-in per-routing-step state digest and a first-difference dump. Diagnostic comparison should cover all state that influences the next step, not raw padded structs. Also compare cumulative mass/quality ledgers and routing decisions.

Retain a named baseline-failure register. The September 25 report's 216/220 unit result and remaining corpus failures are historical; rerun and classify them rather than inheriting either a universal-green or universal-exact claim.

### P0.3 Add the missing work measurements

DW phase timings: geometry preparation, mask packing, widths, classification, area/radius, momentum/losses, CSR gathering, structures/outfalls, node update, time-step selection, and outer router/reporting work. Record Picard iterations, live/bypassed conduits per iteration, shape/category populations, barrier time, and effective team size. Use coarse phase timing and sampling; do not put clock reads in each conduit calculation.

FV additions: separate global and LTS cell/node updates and positivity/bookkeeping; count **actual cell updates and face evaluations**, including node residual trials and rejected attempts. Record accepted physical steps separately from RK stages and LTS base ticks. Add retry reasons, macro cycles executed/rejected, closure inversion iterations, boundary callbacks, copied bytes, active/due fractions, and time spent in implicit classification/assembly/linear/nonlinear solves.

Existing `last_num_steps` and total substeps are insufficient as a work metric: RK2 counts two operator evaluations, and one LTS tick does not update every cell. Similarly, populated tier histograms do not establish that a macro cycle actually ran.

Report both end-to-end elapsed time and routing-only time. Keep output cadence and output selection identical. A faster hydraulic kernel can be hidden by rainfall, transport, or reporting; changing those to make a timing look good is not an engine-wide gain.

### P0.4 Benchmark matrix

| Family | Purpose | Required variations |
|---|---|---|
| Example1 and small ordinary networks | Fixed overhead and latency | DW, FV 1/4/8 cells; 1 thread; shipping automatic policy |
| Uniform and graded generated reaches | Scaling, CFL, LTS usefulness | 50/500/2,000 conduits, plus a larger practical case; 4/8/16 cells; uniform/heterogeneous lengths; LTS on/off |
| Mixed-shape and transect networks | DW gather/packing and geometry; FV closure diversity | Circular/open/tabulated mix; offsets; partial and full flow; repeated versus unique sections |
| East Boston 77 | Junction and boundary work | Authored case plus 4/8-cell variants; dry weather and storm; fixed effective thread settings |
| TwinOaks variants | Short pipes, pressure, real-network cost | Keep v2 and thinned cases distinct; reproducible short window plus full duration; 4/8-cell variants; explicit and existing implicit mode as separately labeled configurations |
| Structure/storage networks | Serial/coupling costs and conservation | Pumps, weirs, orifices, outlets, tidal/time-series/free outfalls, losses, flooding/ponding, DUMMY links |
| Analytic and laboratory problems | Physical accuracy | SWASHES, wet/dry, lake at rest, bores, width/bed steps, surcharge/pressure transitions, TPA/UF, mesh convergence |
| Restart/API and transport cases | Data-cache and publication contracts | Hotstart/reinitialize, permitted run-time edits, pollutants/age/heat; verify the intended LTS eligibility |

Do not take the complete Cartesian product. Use a small screening matrix for each patch, then the full representative/accuracy gates for promotion. Profile one thread first; sweep 1/2/4 and higher physical-core counts only on cases large enough to make them relevant. Include Apple Silicon and a representative x86 system before changing shipping thread thresholds.

Use a warmup and at least five interleaved base/candidate repetitions for meaningful timing rows; publish median and spread, retaining raw samples. Batch very short cases so launch overhead does not dominate. Run timing trials sequentially on a quiet host; separate correctness concurrency from timing. Use uninstrumented time for headline speed and instrumented/sampled runs for attribution.

### P0.5 Establish strong and weak scaling baselines

Thread matrix: 1/2/4/8, then 16 and the remaining physical-core range where available. Measure SMT/hyperthreads separately. On heterogeneous CPUs, distinguish performance-core-only from mixed-core runs; document what affinity the platform actually enforces rather than assuming a scheduling hint pins a thread. On multi-socket systems, compare one socket first, then multiple sockets with memory placement recorded.

Strong scaling holds the entire network, mesh, duration, and numerical configuration fixed. Include dense wet flow, sparse wetting fronts, mixed-shape DW, high-degree junctions, pressure transitions, and FV LTS with small due sets. Repeat at 4/8/16 cells per conduit. Verify that the resulting step/iteration/work counts are invariant across threads before interpreting a timing change as parallel acceleration.

Weak scaling holds approximately constant **active hydraulic work per worker**, not merely total cell count. Use replicated independent network components with identical cell lengths, forcing, and hydraulic time scales as a controlled diagnostic, then larger connected networks for realism. Do not refine cell lengths while adding threads and call that weak scaling: it can change the CFL step and total work. Report time per simulated second plus actual cell/face/iteration counts, so workload changes remain visible.

Collect per-phase worker-time distributions, synchronization cost, effective bandwidth, cache misses where supported, and active/due elements per worker. Diagnose the maximum worker time as well as its average: one expensive shape group or junction can hold the whole team at a barrier. Profiling counters and scratch must be per worker or reduced safely; instrumentation must not create the contention being measured.

Fit a simple measured scaling budget: `T(p) ≈ Tserial + Tparallel/p + Tsync(p) + Tmemory(p) + Tinefficiency(p)`. Use it to select work immediately. If thread overhead or a serial LTS phase dominates, pull D4/F4 forward alongside layout changes instead of waiting for all single-thread tuning.

## 6. DW plan: preserve the calculation, improve locality

### D1. Remove redundant preparation and mapping work

**Priority:** first implementation candidates after P0. **Effort:** 2–4 days. **Gate:** exact.

- Add the dense conduit index to CSR entries, avoiding `csr_link -> tile_uj_to_ci` in every incident-link gather. Preserve row order and both entries of self-links.
- Cache complete scalar `XSectParams` for fallback/critical-depth paths after cross-section setup. Include `yw_max`, every shape parameter, and stable transect pointers. Verify table lifetime and invalidate on permitted geometry edits/reinitialization.
- Test exact full-radius friction caching: precompute the **same** `std::pow(r_full, 1.33333)` and reuse it only when the actual `rWtd` input matches that finite positive radius exactly. Keep the original weighted-radius expression and a fallback for every other input; do not infer eligibility from the category name alone. Full SLOT overrides provide a concrete repeated-input case. The fresh million-input arithmetic probe passed, but full solver parity, geometry-edit invalidation, cache hit rate, and net timing remain gates. This removes expensive calls without changing the exponent or libm implementation.
- Separate static node-tile fields from fields that can change. Avoid recopying immutable fields every routing step only after auditing API/hotstart/2D consumers and establishing explicit invalidation. Do not assume all cached fields are immutable merely because their names suggest geometry.
- Evaluate smaller bypass-mask changes before redesigning geometry: reuse a packed group only when its mask content and group geometry version are unchanged; skip repeated all-active packing; measure compact-index versus copied-parameter approaches. Masks can change in either direction between iterations and reset each step.
- Consider a serial direct-loop path for tiny networks if the inactive OpenMP team overhead is measured. Preserve the same kernels and phase order to avoid two drifting implementations.

Measure each candidate independently. Many networks have almost all links as conduits, so reducing array size alone may save little.

### D2. Make conduit working state genuinely dense

**Priority:** main DW architecture change. **Effort:** 5–8 days. **Gate:** exact.

Convert conduit-only scratch—areas, radii, depths, velocities, momentum terms, and historical areas—to one dense conduit index. Keep explicit maps among link ID, conduit subtype row, and solver row. **DUMMY conduits make these indices unequal.** Public link/node IDs and their externally visible ordering remain stable.

Adapt the batch-geometry interface so the solver can gather/compute/store against dense working state without scattering every intermediate back to sparse link arrays. Keep required publication points, including the flow data consumed by structures and diagnostics. Avoid a second complete shadow representation copied on every phase; otherwise packing can consume the gain.

Stage this by small groups of fields and profile after each stage. Preserve the previous-step area snapshot, held bypass state, virtual-junction relationships, and exact node CSR accumulation order. Include mixed structures/DUMMY links, UF neighbor stencils, and hotstart in the gate.

The practical objective is fewer indirections and bytes per active conduit per Picard iteration, not simply more arrays named SoA.

### D3. Vectorize the common momentum path without changing scalar math

**Priority:** after locality, and only for a measured hot phase. **Effort:** 5–10 days including target-platform verification. **Gate:** exact.

Prototype small batches of ordinary Manning conduits with identical structural features. Keep force mains, offsets/critical-depth paths, controls/gates, culverts, losses, virtual-junction coupling, and UF in explicitly covered fallback/specialized paths. Dynamic dry/full/Froude decisions still use the original predicates and update the same diagnostics.

Use compiler vectorization diagnostics and assembly to establish what actually vectorizes. SIMD multiply/divide/sqrt may help while `std::pow` remains scalar per lane. Do not replace the pow exponent, use a reciprocal approximation, or enable the fast-build options. Grouping costs and extra passes count in the benchmark.

Compare a contiguous SoA microkernel with a small blocked layout only if profiling supports it. A universal AoSoA rewrite is not a prerequisite. Retain the current fused loop when partitioning costs more than it saves.

### D4. Scale the persistent DW team alongside layout work

**Priority:** measured large-network cases. **Effort:** 2–4 days. **Gate:** exact across effective team sizes.

Measure imbalance as bypassing shrinks the live set, shape-group tails, mask-packing serialization, and time spent waiting on structure/outfall work. Tune coarse work partitioning using the existing persistent team. Keep stable per-node reduction order and avoid parallelizing structure updates until their reads/writes have been audited.

- Build stable work ranges over active conduits and shape groups, with more chunks than workers when measured cost variation warrants it. Different workers may own different conduits without changing their arithmetic; the UF dependency must be resolved first. Keep each node's incident contributions in their established order.
- Parallelize mask preparation with deterministic counts/offsets and disjoint output ranges if its serial fraction is material. Reuse existing worksharing regions and remove only barriers proven unnecessary by producer/consumer dependencies.
- Give node depth and CSR-gather work partitions a cost estimate based on incident degree/storage work, not just node count. Avoid placing all high-degree or tabulated-storage nodes in one worker's range.
- Audit the remaining serial structures/outfall phase. Parallelize only independent dependency groups, preserving existing order within a dependent group. Keep feedback and control semantics unchanged.
- Preserve deterministic CFL argmin and tie-breaking, including critical-element statistics, if parallelizing time-step selection. For floating-point sums, preserve the original per-node sequence; generic parallel reductions are not permitted by the DW contract.

Use contiguous output ownership and cache-line-aware chunk boundaries to limit false sharing. Keep per-worker counters/scratch on separate cache lines. For large multi-socket cases, examine first-touch placement of dense arrays; merely parallelizing a loop after all pages were initialized on one thread does not establish local memory placement. Prioritize NUMA work only once measured model sizes exceed cache/socket-local capacity.

Do not change team size on every Picard iteration by adding nested teams. For small live sets, a designated worker can execute a phase within the team if measured beneficial and correctly synchronized. Thread-policy changes must also be validated on small models and on the target hardware.

Include the OpenMP wait policy in that work. `SWMMEngine.cpp:7858–7911` requests active waiting and infinite block time for eligible DW runs, subject to user overrides and the resolved-team oversubscription check. Core count alone does not detect competing applications, other engine instances, or a host that pauses between API steps. Measure bounded spinning and passive waits against the current default for standalone simulations and embedded/interactive use. Report CPU time and responsiveness alongside routing time. Honor explicit host settings; do not mistake persistent spinning for useful parallel work.

**Not in the DW parity program:** new section closures/Chebyshev approximations, changed tolerances, fewer Picard iterations, new convergence tests, float32 state, changed AA defaults, relaxed FP, or altered routing steps.

## 7. FV plan: reduce cost per update first

### F1. Remove full-network floors from sparse LTS work

**Priority:** first FV implementation track. **Effort:** 3–5 days. **Gate:** class A.

`fireFaces` already stamps its live faces, but it still clears the full touched-node bitmap, clears full `out_cell`/`out_node` arrays, and scans them to calculate positivity factors (`ExplicitFvSolver.cpp:3695–3848`). These costs are incurred even when the due-face list is small.

- Apply generation stamps/touched lists to node discovery and positivity scratch. Accumulate exports in the current due-face order, evaluate only touched cells/nodes, and return the exact default factor 1 for untouched entities.
- Keep the identical limited flux in both incident ledgers. Preserve the order of repeated contributions to `acc_a`, `acc_q`, `cell_q_int`, and node exchange/in/out ledgers. Handle generation wrap and rejection/reinitialization explicitly.
- On first-order runs, initialize zero reconstruction slopes/flags once rather than clearing them on every `reconstructState` call, after proving no other writer or option-change path requires reset.
- Build stable active-cell/node lists where they eliminate repeated full scans without changing membership, halo, re-tier cadence, or the dry-lateral activation rule. The full mesh still participates where the time-step contract requires it.

Promote only if current LTS or sparse-wetting profiles show a gain. Earlier measurements exonerated active-list rebuild as the dominant cost on some reach cases; do not assume all O(N) loops are equally expensive.

### F2. Make a node residual pay only for the changing ghost

**Priority:** junction-rich networks. **Effort:** 4–7 days. **Gate:** class A initially.

Mass-only trials already exist, but `computeFaceFlux` still obtains bed information and reconstructs both sides through `faceSide` for each trial. Cache invariant per-incident-face inputs for the duration of a single algebraic node solve: cell-side reconstructed state, bed terms, orientation, geometry identity, and raw pressure correction. Recompute the head-dependent ghost and required wave-speed/flux terms for each residual.

The cache key is the **current substep/stage/reconstruction state**, not just the conduit or section. Invalidate across RK stages, cell updates, regime changes, and rollback. Preserve gates, culverts, pass-through fallback, final face-state writes, head clamps, and the exact residual evaluation sequence. Measure residuals/solve, face reconstructions/residual, and wall time—not only closure-call count.

Also evaluate a static topology descriptor for ordinary interior faces versus node, gate, culvert, and width-step faces. The common internal conduit faces can use a compact kernel without repeatedly discovering topology. Dynamic pressure/dry state remains a runtime decision.

**Additional candidate: paired pass-through face reuse.** The current mesh retains two faces at a degree-two junction, and eligible pass-through reconstruction presents the far cell at both. Investigate calculating the common Riemann problem once when the full reconstructed states, section, due time and orientation mapping prove it is the same problem. Keep distinct per-side hydrostatic corrections, ledgers and published face state. Exclude gates, culverts and any dynamically ineligible junction until separately proved. LTS faces with different due times cannot share an old result. The mirrored-kernel probe found signed-zero differences, so blindly negating/copying the mass flux fails the exact gate even before mesh-level issues are considered.

Quantify the ceiling before implementing: a long uniform chain of C conduits with m cells each has C(m+1) retained faces; eliminating one evaluation at each of C−1 interior junctions removes at most (C−1)/(C(m+1)) of face evaluations, approaching 20% at m=4 and 5.9% at m=16. Whole-solver savings are smaller and depend on face cost. Keep this behind residual caching unless profiles and duplicate-state counts justify it; it cannot by itself close a several-fold FV/DW gap.

### F3. Narrow boundary refresh and unnecessary memory traffic

**Priority:** large substep-count cases. **Effort:** 3–5 days. **Gate:** class A with a dependency audit.

`Router::refreshFvBoundaryFlows` and its structure-evaluation lambda traverse all nodes/links. `refreshStructFlows` clears full lateral/structure arrays and revisits node coupling. Compile the boundary dependencies once: outfall nodes and their incident conduits, structure endpoints, storage/head inputs, DUMMY-link controls, and lateral-diversion candidates. Update only the required entries and clear only entries written previously.

A deck without pumps or weirs can still have time-dependent/free outfalls, so “no structures” does not justify skipping the entire callback. Preserve the exact refresh time, order, integrated discharge, control cadence, and live API writes. A change to `FV_STRUCTURE_COUPLING` is a class C configuration experiment, not this optimization.

Audit `saveState`/RK snapshots and implicit scratch allocations using measured bytes. Reuse solver-owned capacity first. Consider active-set snapshots only if every possible writer, wetting activation, and ledger rollback is accounted for. Never remove rollback because its measured incidence is low.

### F4. Share cell kernels and make LTS threading compose

**Priority:** primary scaling track for larger meshes; start alongside F1/F2 if P0 identifies serial LTS work or region overhead. **Effort:** 6–10 days. **Gate:** class A.

The global `updateCells` loop is parallel; `fireCells` is serial. Extract the common per-cell closure/friction/state-update work into one tested kernel with explicit inputs and output ownership. Keep each path's distinct flux integration and source cadence intact. Audit diverted-lateral spill writes and diagnostic counters before parallel execution.

Parallelize independent due cells and nodes, then evaluate a persistent team around an `advance`/macro-cycle segment. This requires team-aware variants of existing `parallel for` loops to avoid nested teams. Keep callbacks, tier assignment, and schedule decisions in ordered single-thread phases; all workers must reach barriers and retry decisions consistently.

For face-to-cell/node accumulator booking, use an ordered gather that reproduces the **actual due-face traversal order**, not a floating-point atomic scatter. Do not assume sorting by global face index reproduces the existing per-tier order. Include self-connections and reversed chain directions.

Cover the whole update, not just the Riemann-flux loop: reconstruction, census, positivity, accumulator booking, cell updates, node updates, and required publication. Parallelize independent cell/node ownership and give every stage an explicit dependency/barrier contract. A parallel face loop followed by several serial O(N) sweeps will not scale on a refined mesh.

Keep contiguous blocks within conduit chains where possible; schedule heavy algebraic junctions separately from cheap pass-through nodes. For LTS, choose worksharing according to **the currently due set**, with a serial-within-team fallback for small firings. Measure whether task scheduling improves irregular workloads enough to pay its overhead before adding it. Global cell count alone is not a suitable threshold for every tier.

Use per-worker counters and persistent scratch, avoid nested OpenMP regions, and preserve independent writes in rollback/retry paths. Review cache-line ownership of adjacent state arrays and first-touch placement for large meshes. Shared geometry blocks are read-only and should remain shared; duplicating whole closure tables per thread can increase bandwidth and cache pressure.

The implicit-pressure path also needs a scaling plan: parallelize disconnected components and independent assembly/matrix-vector rows while retaining each row's summation order. Thomas chains are sequential internally; many chains can run concurrently, but one long chain or a single large branched component is a different scaling case. CG dot products and stopping decisions must retain the baseline reduction order for class A work. A new reduction tree or parallel chain algorithm belongs to a separately validated class B/C experiment if it changes arithmetic. Record component sizes and linear iterations so an implicit speedup is not assumed to scale automatically.

Compare against the existing simpler implementation: persistent teams can lose on tiny networks. Recalibrate thresholds using due work and node-solve cost rather than total mesh size alone, with explicit user thread settings still documented and honored according to policy.

### F5. Further closure and section-local kernel work, only if still dominant

**Priority:** profile-dependent. **Effort:** 3–6 days per accepted experiment.

- Keep geometry sharing. Separate compact frequently read section scalars from cold table/build data if cache measurements justify it. Group cell work by chain/section without changing cross-face/node accumulation order.
- Measure inversion iteration distributions and table-cache misses, especially near invert/crown. The current solver already uses safeguarded Newton; the historical Brent cost is irrelevant to this choice.
- Reuse the same per-cell derived closure state through flux/CFL/friction where arguments are exactly the same. Avoid conflating `cell_a` with re-evaluated `A(cell_h)`—the current cache deliberately distinguishes them.
- Test precomputed polynomial coefficients/Horner evaluation, coherent inverse seeds, or a fused inverse-plus-evaluation API as class B unless exactness is demonstrated. A Horner rewrite changes rounding; returning the last Newton evaluation may not equal evaluating the final rounded depth.
- Preserve monotonicity, `T = dA/dh`, `I1 = integral A dh`, nonnegative area/width, and accurate A→h→A round trips. Independent approximations of these quantities can break lake-at-rest balance and pressure behavior.

Do not reduce closure panels or lower precision to obtain an unqualified speed win. Measure accuracy and memory savings explicitly if a later experiment proposes either.

## 8. FV plan: reduce unnecessary updates and pressure stiffness

### F6. Reuse a stability census only when its inputs are unchanged

**Priority:** after profiling current census share. **Effort:** 2–4 days. **Class:** A where proven, otherwise B/C.

The accepted global substep computes a post-step CFL census, but normally the next substep computes a fresh pre-step census. A cached result can be reused only if cell/node state, boundary forcing/ghosts, active faces, pressure classification, and relevant options are identical. The next boundary callback can invalidate it even though the cells have not advanced.

Add explicit versions/dependency checks and retain the accepted-step value separate from the admissible CFL bound. Invalidate after restore, settling, tier changes, active-list rebuild, forcing/head updates, and regime changes. If those changes occur every step, abandon the cache rather than weakening its validity rule. Define how diagnostic census/argmin counts change; they must not masquerade as identical workload statistics.

### F7. Replace the unsafe short-cycle experiment with conservative window handling

**Priority:** only for a measured large tail/global-fallback cost. **Effort:** 1–2 weeks of prototype and validation. **Class:** C.

The current fit experiment reassigns tiers to force a shorter cycle and has a recorded pressure-front failure. Revisit the existing plan's proposal to shorten the synchronization window while retaining admissible local time scales.

A design must account for the exact duration of every face flux, every cell/node source, and every partially open coarse window. Closing a window early requires a flux evaluated for that shorter duration; booked full-window fluxes cannot simply be retained. Boundary changes, positivity budgets, nonlinear friction, and rejected cycles must all see consistent time intervals. Start with a conservative two-tier prototype and volume-ledger tests at each synchronization point.

This changes the integration schedule even when it preserves mass. Require front-arrival, wave-amplitude, nonlinear-source, and mesh/time refinement gates. Do not enable `OPENSWMM_FV_LTS_FIT=1` as a shipping optimization based on the East Boston timing alone.

### F8. Finish the implicit-pressure route for persistently stiff networks

**Priority:** bring forward if pressure/short-cell CFL dominates. **Effort:** 2–4+ weeks including numerical validation. **Class:** C; build on the existing solver.

The existing `PressurizedHeadSolver` already folds pressurized junctions, uses Thomas for chains and CG for branched components, and keeps transition faces explicit. It is the highest-potential path when most work is caused by acoustic CFL restrictions.

First address its recorded nonlinear limitation: `PressurizedHeadSolver.cpp:478–500` states that three secant passes are a measured compromise, not a converged storage solve. Implement and validate a residual-controlled nonlinear storage treatment with a safeguarded/monotone step strategy, explicit failure handling, and conservation-based acceptance. Do not assume implicit stability permits an arbitrarily large step or guarantees transient accuracy.

Then reduce its own cost: reuse scratch buffers and cache graph/component ordering while the active pressure/fixed-head topology is unchanged. Rebuild numerical coefficients whenever depth, friction, time step, or boundary data require it. Symbolic reuse does not imply reusing an old factorization.

Finally evaluate conservative synchronization of the implicit pressure components with explicitly advancing free-surface/transition regions. Today pressurized implicit updates force the global path; RK2 and transported species also exclude LTS (`ExplicitFvSolver.hpp:421`). Combining these paths is a new numerical scheme, not a threading change. Keep it behind separate validation until transition fronts, TPA state, structures, positivity and quality fluxes agree with the intended method.

The target is to confine fine explicit work to places where it is physically required while preserving all selected subgrid cells. Lowering slot celerity, raising CFL, switching to one cell, or freezing structures are not substitutes for demonstrating this result.

## 9. Why mesh resolution changes the performance problem

For `m` cells per conduit, `Ncell` is approximately `m*Nconduit` and a cell's explicit stability bound is proportional to `dx/(|u|+c)`. With `dx ~ L/m`, a uniformly wet, globally CFL-limited reach can approach **m² work growth**: m times as many cells and roughly m times as many steps. This is a scaling argument, not a prediction for every network; routing-step caps, dry compaction, geometry, junctions, and LTS can change it substantially.

LTS helps when stiffness is localized. It does little when nearly the whole refined network shares the same small admissible step. Implicit pressure treatment helps acoustic stiffness but does not erase explicit transition-front limits or temporal accuracy requirements.

Use the measured decomposition:

`T = Tshared + Tcell + Tface + Tjunction + Tscheduling + Tboundary + Trollback + Tpressure`.

For each proposed patch, show its fraction of runtime and achievable local reduction. If a phase is 40% of runtime, eliminating it entirely caps the speedup at 1/0.6 ≈ 1.67×. Do not multiply optimistic microbenchmark factors as if they applied independently to the whole run.

An optional later study may compare second-order FV on fewer cells against first-order FV at the same error. That is an accuracy/cost comparison, not a fixed-mesh implementation speedup, and is outside the initial parity-preserving work.

## 10. Validation and promotion gates

| Gate | Required evidence |
|---|---|
| Build provenance | Source/library hashes, compile flags, resolved runtime/backend, effective options and threads |
| DW exactness | Same step times, full-precision state, convergence/bypass/classification/control decisions, result payload and mass/quality ledgers against frozen DW; no new legacy discrepancy |
| FV class A exactness | Same mesh and configuration; state/timestep/tier/rollback decisions and output agreement; repeatability across supported thread counts |
| FV physical validity | Closure identities; lake at rest; wet/dry positivity; conservation at cell/node/tier interfaces; analytic error norms and convergence; pressure/TPA/UF laboratory responses |
| Feature coverage | Structures, DUMMY links, virtual junctions, offsets, multiple barrels, US/SI units, flooding/ponding, seepage/groundwater exchange, controls, hotstart and runtime edits |
| Transport compatibility | Pollutant/age/heat flux and ledger regression, including the cases where transport disables LTS |
| Performance | Interleaved median wall/routing times and spread, work counts, memory/copy metrics, no failed or incomplete row treated as success |
| Multithread scaling | Strong/weak scaling curves, speedup and efficiency, actual worker/core placement, measured serial/wait/bandwidth limits, automatic-policy crossover and small-model regression checks |

Use existing tests rather than building a parallel test framework: `test_engine_routing`, `test_engine_virtual_junction`, `test_engine_dw_tpa`, `test_engine_dw_unsteady_friction`, `test_engine_xsect_parity`, `test_engine_xsect_kernels_parity`, and the `test_engine_fv_*` targets, including LTS, section geometry, integration, and implicit pressure. Add focused cases only for missing dependencies such as UF cross-thread neighbors, stamped positivity scratch, boundary-cache invalidation, and partial-window conservation.

Use `tests/parity/run_corpus.sh` for the in-tree two-build identity gate, but inspect the actual manifest; its README's old deck counts are not a coverage guarantee. Use the companion benchmark repository for wider legacy regression and SWASHES/transitions. Retain the mixed-flow laboratory matrix used in the existing closure program.

For class B/C changes, record numerical thresholds **before** evaluating the candidate. Reuse current analytic/laboratory tolerances and strengthen with unrounded conservation metrics and pressure-front checks as needed. A rounded “0.000%” continuity row is not proof of machine-precision conservation. A retained failing physical case remains a failing case; unchanged verdicts alone do not make it an acceptable performance reference.

Proposed performance promotion rule: a reproducible improvement beyond measurement spread on the intended class, no unexplained >5% regression on other representative classes, and all applicable correctness gates met. Expensive architecture changes should have a measured projection of at least roughly 10% end-to-end benefit on their target workloads before being undertaken. These are planning thresholds, not promised gains.

## 11. Delivery sequence and review checkpoints

| Stage | Deliverable | Decision |
|---|---|---|
| P0 | Frozen source/binary baseline, corrected instrumentation, explicit UF/parity findings, refinement and strong/weak scaling matrices | Choose the dominant costs and scaling limits per workload; establish accepted numerical gates |
| P1 | D1 plus F1/F3 small exact changes; early D4/F4 work where synchronization/serial phases dominate | Keep only demonstrated wins at one and multiple threads; no numerical-method defaults changed |
| P2 | D2 dense DW state, F2 residual caching, and continued D4/F4 scaling work | Review exactness, index/lifecycle handling, locality gains, and per-phase parallel efficiency |
| P3 | Complete D4/F4 scaling gates; D3/F5 where justified; calibrate automatic thread policy | Review absolute runtimes, strong/weak scaling, SIMD evidence and platform crossover |
| P4 | F6 safe census reuse; F7 and F8 as separate numerical experiments | Decide which methods warrant adoption based on accuracy/runtime curves |
| Release gate | Complete numerical/feature corpus, matched-core-budget 4-cell FV comparison against optimized DW, 8/16-cell curves, determinism and automatic-thread-policy gates | Make only the performance and scaling claims supported by valid completed cases |

The effort ranges are engineering estimates for planning, not a schedule commitment. A first baseline and low-risk round is roughly one to two weeks of work; dense layout/threading and pressure-method completion are subsequent milestones. F8 can move ahead of F2/F4 on a pressure-dominated workload; profiling determines that order.

Every implementation patch should include its before/after timings, work counts, exactness/accuracy results, and rollback path. Keep numerical fixes and answer-changing experiments separate from data-layout changes so each result is attributable.

## 12. Scope boundaries and decisions proposed for review

Recommended approval scope:

1. Adopt exact same-configuration DW behavior as the hard constraint and use the current refactored engine plus pinned legacy as the two references.
2. Use four cells per conduit as the primary FV speed target, with 8/16-cell refinement curves and one-cell rows labeled diagnostic.
3. Begin P0 and class A optimization work; require a separate review of the measured class B/C experiments before changing numerical defaults.
4. Use a representative production set drawn from the families above, retaining both small interactive workloads and large pressurized networks.
5. Treat one-simulation multithread scaling as a required deliverable for both solvers, with thread-count determinism, matched-core-budget FV/DW comparisons, and hardware-specific crossover policies.

Deferred: a new 1D GPU implementation, adaptive remeshing, mixed-precision state, new DW geometry, and any 2D performance program. The tree has a 1D backend factory and a plugin symbol lookup, but this review found no in-tree implementation exporting `openswmm_make_gpu_network_solver`. A factory option is not evidence of a working faster 1D GPU backend. The factory also forces CPU for implicit pressure, and non-CPU structure coupling has different support. CPU work and a validated numerical contract should come first.

## 13. Source and prior-plan index

Paths below are relative to `openswmm.engine` unless otherwise stated. Function names are included because line numbers will move.

| Evidence | Location at review |
|---|---|
| DW arrays and conduit invariants | `src/engine/hydraulics/DynamicWave.cpp:497`, `:787`; `DynamicWave.hpp` |
| Ordered DW node CSR | `DynamicWave.cpp:900`, `:3326` — `buildConduitNodeCSR`, `gatherConduitNodeFlows` |
| Persistent DW team and phase ordering | `DynamicWave.cpp:1246` — `execute` |
| DW bypass/group packing | `DynamicWave.cpp:1655`; `XSectBatch.cpp:1335` — `setBypassMask` |
| Momentum fusion, flow commit, UF neighbor reads | `DynamicWave.cpp:2329`, `:2843`, `:2948` |
| FP build policy | `CMakePresets.json`; `src/engine/CMakeLists.txt:227` |
| Thread policy | `src/engine/core/ThreadInfo.cpp:134` |
| FV shared geometry and SoA mesh | `src/engine/hydraulics/fv/NetworkMeshData.hpp`; `NetworkMeshBuilder.cpp` |
| FV closure and inverse | `FvClosureKernels.hpp:139`, `:233`; `SectionGeometry.hpp` |
| FV face and node hot paths | `ExplicitFvSolver.cpp:1067`, `:1320`, `:1616`, `:2177` |
| FV LTS full-array work | `ExplicitFvSolver.cpp:3695`, `:3755`, `:3850`, `:3985` |
| FV schedule, retries and census | `ExplicitFvSolver.cpp:4088`; `ExplicitFvSolver.hpp:421` |
| Boundary and result publication | `src/engine/hydraulics/Routing.cpp:1376`, `:1507`, `:1552` |
| Nonlinear pressure caveat | `src/engine/hydraulics/fv/PressurizedHeadSolver.cpp:478` |
| Profiling-counter synchronization gap | `src/engine/core/PerfTimers.hpp:324`; `ExplicitFvSolver.cpp:404`, `:1616` |
| Existing performance driver | `tests/benchmarks/scripts/fv_perf_baseline.py`; `bench_fv_closure.cpp`, `bench_xsect_eval.cpp` |
| Identity runner and provenance pitfalls | `tests/parity/README.md`, `MANIFEST`, `run_corpus.sh` |
| Companion performance stub and comparator | `openswmm.engine.benchmarks/suites/performance/suite.py`; `harness/compare.py:248` |

This proposal continues the useful directions of `DWSOLVER_DATA_OPTIMIZATION.md`, `DYNAMIC_WAVE_EFFICIENCY_REVIEW.md`, `FV1D_PERF_PLAN_REVISED_2026-08-20.md`, `FV1D_CLOSURE_KERNEL_PERF_PLAN_2026-09-11.md`, and `FV_SLOT_STORAGE_PROGRAM_2026-08-24.md`. Their earlier implementation-status claims must be read against the current source. The later entries of `FV1D_PERF_BASELINE_2026-09-11.md` and `PARITY_IMPLEMENTATION_RESULTS_2026-09-25.md` supply the most relevant retained measurements and validation caveats.

**Review outcome:** the next gains should come from finishing data locality, avoiding repeated full-network work and reconstruction, and making local updates scale. The existing FV pressure solver and a correctly synchronized LTS design are the route to larger gains on stiff refined networks. Both require stronger numerical evidence than a faster timing alone.
