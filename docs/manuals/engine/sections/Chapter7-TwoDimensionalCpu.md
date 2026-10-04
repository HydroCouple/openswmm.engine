@page engine_manual_2d_cpu CHAPTER 7 — Two-Dimensional CPU Implementation and Qualification

## 7.1 Purpose and scope

The built-in explicit CPU marcher advances conservative surface volumes,
face transfers and, for `FULL_SWE`, cell momentum. The October 2026 review
corrected conservation, source accounting and shoreline reconstruction,
then reduced CPU work without weakening the accepted numerical checks.
This chapter explains the retained strategy and how to assess it on a
new model. The equations and spatial reconstruction are in
@ref hydraulics_ref_ch9_two_dimensional §9.5.10; option syntax is in
@ref engine_manual_ch2_input_file.

The optimized path does not introduce a new input keyword or a different
results-file format. Its performance depends on the momentum closure,
wet fraction, species count, mesh, output cadence and thread count.
The review measured the Apple ARM64 CPU implementation; GPU correctness
and speed were not tested. Existing backend eligibility rules still
apply. A CPU result is not evidence for an equivalent GPU implementation.

## 7.2 Selecting and confirming the formulation

A water-only second-order setup can use:

```
[2D_OPTIONS]
INTEGRATOR          EXPLICIT
BACKEND             CPU
MOMENTUM_EQUATION   FULL_SWE
RECONSTRUCTION_ORDER 2
LTS_TIERS          1
CFL_NUMBER         0.4
```

This is an option fragment, not a complete model. Supply the mesh,
initial/boundary conditions, forcing, duration and output settings needed
by the physical problem. Keep the original storage closure and coupling
physics when evaluating an optimization.

| Requested configuration | Actual route or restriction |
|---|---|
| `FULL_SWE`, order 1 | Piecewise-constant Godunov route; local time tiers available |
| `FULL_SWE`, order 2, water only | Limited surface/depth/velocity reconstruction and SSP-RK2; global one-tier stepping |
| Order 2 with multiple tiers | Tiers reduced to one with a diagnostic |
| Order 2 with active surface transport | Falls back to first order with a diagnostic; prior tier reduction is not undone |
| Active second-order RK2 with groundwater | Initialization rejects the combination because this route does not advance groundwater |
| Local inertial or diffusive momentum | Their own first-order face laws; order 2 does not enable MUSCL for these closures |
| `FULL_SWE` with CFL above 0.5 | CFL reduced to 0.5 with a diagnostic |

For transport or groundwater with local timestepping, explicitly request
order 1 and the intended tier count. Read initialization diagnostics and
inspect run statistics (`swmm_2d_get_run_stats`): backend, momentum mode,
tiers, internal steps, face evaluations and active-cell fraction. The
statistics do not independently prove that reconstruction order 2 was
used; check fallback diagnostics and the model's enabled features too.
A model with zero active surface faces does not exercise wet-surface
performance, even if its groundwater is active.

## 7.3 Conservation and source-accounting contract

Every optimization must preserve the following ordering and invariants.

1. A unique face evaluates one water transfer and books equal and opposite
   contributions to its two incident cells. Species use that same final
   mass flux, including its conveyance and positivity restrictions.
2. Local-time faces fire at the beginning of their interval; cells consume
   accumulated transfers at the end. Pending water, species and momentum
   must be settled before tier reassignment, publication and finalization.
   The current base-step bound divides each cell's allowable step by its
   frozen tier multiplier. A cell at tier 3 must not execute eight times
   its admissible local interval.
3. After face transfers land, inflows and existing storage form one
   available-water budget. Evaporation, infiltration and coupling withdrawal
   share insufficient water proportionally. Applied amounts, rather than
   requested demands, enter the ledgers. Complete exhaustion makes the
   cell exactly dry; a roundoff film must not carry residual solute away.
4. Gross source transfers remain meaningful when their net water change
   is zero. Equal rain and infiltration may change concentrations and
   ledgers even though depth stays constant. Solute retention under
   evaporation and signed temperature handling retain their distinct rules.
5. Source totals are booked in padded worker-private buffers and folded
   after the parallel loop. Groundwater receives the cell-local transferred
   mass. Cell-owned gathers avoid concurrent face scatter into runoff;
   front-breach detection uses a reduction.
6. RK2 averages its state and stage-dependent ledgers consistently. A
   two-stage calculation must not publish two intervals of exchange for
   one interval of elapsed time.

A zero water flux is not necessarily a zero momentum contribution.
Hydrostatic pressure corrections remain active on a blocked face when
both cut face depths vanish. The bed/source terms, wet connectivity,
positive face-depth polynomial and shoreline cell velocity described in
@ref hydraulics_ref_ch9_two_dimensional are part of the numerical method,
not dispensable work to remove from a profile.

## 7.4 CPU work and memory layout

**Remove repeated scalar overhead.** The current worker's source-ledger
pointer is obtained once per cell and reused across its species rows.
For the local-inertial face law, an exactly zero momentum numerator
skips the friction denominator: a zero numerator remains zero after
division. This is an exact branch, not a new small-flow tolerance.

**Avoid unused source work.** When inactive-cell source processing reports
that it performed no source bookkeeping or volume change, the previous
head/depth closure can be retained. The predicate accounts for gross
transfers; testing only a zero net source is insufficient. Fixed-rate
source work and live junction exchange remain distinct.

**Rebuild sparse lists proportionally to activity.** Active lists carry
only participating cells/faces. Below one-quarter cell occupancy, the
rebuild discovers faces from active cells and sorts them into canonical
edge order; denser sets use the full face scan. The first rebuild always
scans all faces because initial velocities can seed momentum before a
previous active-face list exists. Subsequent retirement considers the
previous active faces. These rules preserve activation and momentum
cleanup while avoiding full-mesh work in sparse cases.

**Keep species adjacent on a face.** The private pending-mass buffers use
`face * species_count + species`, with separate left/right arrays.
Gathering handles blocks of at most eight species with an eight-double
stack buffer while walking the cell's incident faces. Each species still
sums those faces in the same order. Advection, dispersion and settlement
share the private layout; the public cell-mass layout and total sizes of
these face buffers do not change. This improves locality without adding
per-cell heap allocation or a shared scatter operation.

**Fuse when there are enough species.** With at least eight transported
species, each gathered species row immediately applies its source update,
using a water budget prepared once for the cell. Smaller counts retain
separate gather and source passes. Shared helper routines keep the row
arithmetic consistent in both routes. The threshold is an empirical
choice from the reviewed workloads, not a hardware-independent optimum.

![CPU species gather and source update](figures/png/eng_2d_cpu_pipeline.png)

*Figure 7-1 Face-local species storage, cell-owned gathering and the conditional fused source pass. Both routes preserve the same physical budgets.*

**Restrict additional reconstruction work to where it is needed.** Wet
interiors use the Green–Gauss path. Only shoreline stencils attempt the
conditioned least-squares fit, and only for elevation and depth. A small
connectivity mask is reused across stencil passes. Fully connected wet
interiors already have positive bounded face depths; omitted shoreline
and boundary faces require the extra positivity check. Thin or poorly
conditioned stencils keep zero gradients.

**Reduce face-kernel overhead.** The second-order face loop computes its
centroid-to-midpoint arms directly from edge midpoint and cell centroid
coordinates, avoiding two searches through cell incidence lists. The
limiter still uses its existing incidence arms. CPU-local force-inlining
of the small SWE helpers removes measured call overhead; the macro is
restored after inclusion and does not change the shared GPU annotation.
Compiler fallbacks remain for unsupported force-inline attributes.

**Storage cost is explicit.** The elevation-gradient vectors each contain
two cell-count blocks: elevation then depth. Thus depth reconstruction
adds two arrays of doubles, approximately 16 bytes per cell, relative to
the earlier elevation/velocity gradient storage. Velocity gradients and
RK stage buffers remain separate. The layout/fusion optimizations do not
add another persistent species-buffer copy. OpenMP static cell/face loops
retain independent writes and explicit reductions.

The implementation is in `src/engine/2d/solver/ExplicitInertialSolver.cpp`
and `.hpp`; `SweKernels.hpp` defines the flux, pressure corrections and
friction. `InertialEdges` supplies geometry and incidence. Public options
remain in `SolverOptions2D.hpp`. In particular, `faceFluxReconBed` is the
current second-order kernel; `faceFluxRecon` is the constant-bed
compatibility entry point.

## 7.5 Measured benefits and their boundaries

The final cumulative comparison uses the corrected implementation before
CPU phase 1 as its reference, with both solvers compiled against the same
supporting engine build. Each authored model has 32,768 triangular cells,
four threads, four local-time tiers, local-inertial momentum, one minute
of rain/infiltration, a drain/pipe connection and HDF5 output. CPU time
includes initialization and output. Three randomized measured pairs
follow one discarded warm-up pair. States, ledgers, normalized reports
and all HDF5 contents match exactly in every pair.

| Transported species | Median paired CPU reduction | Paired reduction range |
|---|---:|---:|
| 0 | -1.9% | -2.3% to 6.9% |
| 8 | 40.9% | 38.2% to 41.6% |
| 32 | 52.5% | 52.0% to 59.4% |

Positive reduction means less CPU time. The water-only control has no
consistent benefit. Larger water-only cases in earlier scaling runs also
showed small CPU regressions. Do not interpret the species savings as a
universal 2D speedup or add percentages from different phases.

The revised second-order reconstruction has a separate benefit: lower
cost at better analytical depth accuracy. The three-period Thacker
comparison in @ref hydraulics_ref_ch9_two_dimensional §9.10.1 uses a
96-division final mesh against the earlier reconstruction at 128
divisions and measures about 60% less CPU time. Those are different
resolutions and a different reference from the cumulative species test.
Phase 10 separately preserved numerical results while reducing CPU time
by about 11–15% in component cases and 12–19% in authored wet coupled
cases. Five further hotspot prototypes in phase 11 were rejected because
their gains were small or inconsistent; they are not in production.

These measurements were made on an Apple ARM64 Mac with ten logical
CPUs. The final run's one-minute load average ranged from 7.4 to 13.5.
Review workloads were serialized, but unrelated system work remained.
CPU time includes worker execution and OpenMP spinning; it is not an
instruction count. Wall time and optimal thread count can differ on an
idle host or another architecture. No claim is made for GPU speed.

## 7.6 Verification and reproduction strategy

Use three distinct comparisons rather than one aggregate score:

- **Numerical correctness:** analytical errors, phase, velocities,
  positivity, conservation and refinement. Include moving wet/dry fronts,
  sloping beds, blocked sills and perturbed/mixed meshes, not just resting
  lakes. Long-period damping must remain visible.
- **Optimization equivalence:** identical configurations and inputs,
  comparing full fields, sampled histories, ledgers and output contents.
  Match initialization, forcing, storage closure and step scheduling.
- **Accuracy per cost:** identify the error metric and reference clearly,
  require the intended accuracy before comparing different meshes, and
  include time-mean as well as endpoint error for oscillatory problems.

The final matched CPU regression build passed 169 tests with one
unavailable Kokkos OpenMP plugin test skipped. The new qualification has
24 complete coupled executions, eight analytical diagnostic executions
and 32 analytical timing executions. Prior hash-verified evidence adds
140 analytical executions and 24 coupled executions from phase 10; those
were not rerun or relabelled as fresh phase-12 results. The regression
suite includes source competition, macro-boundary settlement, thread
safety, groundwater transfers, wet-face flux consistency, pressure forces
and distorted shoreline velocities.

The evidence is under `reviews/cpu_2d_2026_10_03`:

| Directory | Purpose |
|---|---|
| `fixes` | Initial correctness fixes, analytical problems and regression evidence |
| `cpu_phase2`, `cpu_phase4`, `cpu_phase5`, `cpu_phase6` | Sparse work, species layout/fusion and scaling |
| `accuracy_phase7`, `accuracy_phase8`, `accuracy_phase9` | Reconstruction development, negative experiments and final numerical checks |
| `cpu_phase10` | Retained face-kernel optimization, analytical equivalence and wet coupled checks |
| `cpu_phase11` | Rejected hotspot prototypes and their measurements |
| `cpu_phase12` | Direct cumulative comparison, accuracy/cost comparison and final qualification |

Within phase 12, `prepare.py`, the build scripts, `check_suites.py`,
`cumulative.py`, `accuracy_cost.py` and `finish_review.py` record the
reproduction workflow. Command manifests record compiler/link flags;
source manifests identify the implementations. Frozen source trees,
matching supporting objects, external Bellinge data and generated binary
outputs are ignored local prerequisites. The scripts are review records,
not a standalone portable benchmark distribution. Do not mix solver and
supporting-engine objects from incompatible header/layout snapshots.

For a new deployment comparison, build both references with the same
compiler/settings and supporting code; keep original model physics;
record thread count, active fraction, step/face counts and output cadence;
run warm-ups followed by randomized adjacent pairs; serialize benchmark
jobs; then verify the intended numerical equivalence before reporting
timing ratios. The reviewed component builds use O3/native CPU with fast
math disabled and floating-point contraction disabled. Changing arithmetic
settings requires a new correctness comparison.

The remaining boundaries are groundwater/transport support for RK2,
long-period damping, mesh-dependent stability, production-catchment
accuracy and hardware portability. The planned CPU review is complete;
future optimization should follow a measured deployment bottleneck.
