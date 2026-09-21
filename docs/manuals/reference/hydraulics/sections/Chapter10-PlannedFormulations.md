@page hydraulics_ref_ch10_planned Chapter 10: Planned and Retired Formulations

@tableofcontents

\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_

**Nothing in this chapter is in this release.** \status{Planned} Chapters 2
through 9 describe formulations the shipped engine executes; this chapter
records the ones it does not — designed, prototyped or measured and set
aside, or once shipped and withdrawn — so that a plan file cited in a
commit, a retired option key met in an old model, or a limitation stated
in an earlier chapter can be followed to its record. *Planned* means
designed and not built, whatever the plan's own optimism; *Retired* means
explored or shipped and withdrawn, with the measurement that withdrew it.
Every section names its plan of record by path; the plans are not part of
the manual, and the code is the source of truth where they disagree.

| Section | Formulation | Plan of record | Status |
|---|---|---|---|
| 10.1 | Chebyshev spectral geometry for irregular sections | `plans/CHEBYSHEV_XSECT_GEOMETRY_PLAN.md`, `plans/CHEBYSHEV_XSECT_FEASIBILITY_2026-08-21.md` | \status{Planned} |
| 10.2 | An implicit finite-volume solver | `plans/FV_IMPLICIT_HYPRE_SOLVER_EXPLORATION.md` | \status{Planned} |
| 10.3 | The 1D finite-volume solver on Kokkos backends | `plans/EXPLICIT_FV_KOKKOS_1D_SOLVER_PLAN.md` §5, §7B.3; `ROADMAP.md` §7 | \status{Planned} |
| 10.4 | Dynamic wave on GPU | `plans/1d/1D_DYNWAVE_GPU_KOKKOS_STRATEGY.md` | \status{Retired} |
| 10.5 | The 2D solver on Metal | `plans/2d/2D_GPU_METAL_STRATEGY.md` | \status{Planned} |
| 10.6 | A bed-step treatment at finite-volume junction faces | none — `ROADMAP.md` §1.1 only | \status{Planned} |
| 10.7 | The Vasconcelos and Wright (2009) hybrid flux for the TPA filling front — no longer required: the divergence it answered was removed on 2026-09-12 | `plans/FV_SLOT_TPA_REVIEW_2026-08-31.md`, `plans/ISSUE_MIXED_FLOW_TPA_UF.md` | \status{Retired} |
| 10.8 | LID units as storage nodes, the hydraulic side | `plans/LID_StorageNode_Redesign.md`; `ROADMAP.md` §6 | \status{Planned} |
| 10.9 | Retired formulations | `plans/IMEX_LOCAL_INERTIAL_IMPLEMENTATION_PLAN.md`; `ROADMAP.md` §7; `src/engine/input/handlers/OptionsHandler.cpp` | \status{Retired} |

*Table 10-1 Formulations recorded in this chapter*

## 10.1 Chebyshev spectral geometry for irregular sections

The `IRREGULAR`, `STREET` and `CUSTOM` sections of Chapter 4 are evaluated
from 51-row tables of area, width and hydraulic radius against depth,
interpolated piecewise-linearly. The forward lookups are nearly free —
about 15 ns — but the solvers spend their time in the *inverse*
direction: Newton on \f$A(S)\f$ costs 45 to 300 forward lookups, and the
finite-volume depth inversion of §8.4.3 must use a derivative-free Brent
search because the tabulated width is not the derivative of the tabulated
area. On a transect-heavy dynamic-wave deck the irregular chain measured
56 % of the routing wall clock.

The first study (`plans/CHEBYSHEV_XSECT_FEASIBILITY_2026-08-21.md`) fitted
global Chebyshev series to the table interpolant against the 1e-8 parity
bar of the bit-exact tier and returned a no-go on replacement: the target
has fifty corners, the error frontier stalled five orders short, the fits
rang and were not monotone. It found a conditional go on a narrower use —
a degree-12 inverse *seed* feeding one or two Newton polishes against the
exact evaluators, 1.6–3.4× on the finite-volume inversion — inside the
tolerance-gated fast tier, never the default.

The plan of record (`plans/CHEBYSHEV_XSECT_GEOMETRY_PLAN.md`, 2026-08-22)
reframes the target and supersedes that verdict. The reference is the
exact continuous geometry between stations — width piecewise linear, area
piecewise quadratic — of which the table is itself a sampling with its
own error (normalized: area \f$10^{-3}\f$, width 0.3, radius 0.1), and the
yardstick becomes "as accurate as the table", demonstrated end to end on
deck outputs. Per transect, at resolve time: up to four panels with
breaks placed first at detected discontinuities of the effective
hydraulic radius (an interior station submerging switches the conveyance
grouping) and then at the strongest width kinks; per panel a degree-10
Chebyshev fit of \f$p \approx \sqrt{dA/dy}\f$, the area taken as the exact
antiderivative of \f$p^{2}\f$ — monotone by construction, pinned to
\f$A(y_{full})\f$ — the width as \f$p^{2}\f$, the hydrostatic moment as the
antiderivative of the area, the radius fitted per panel at degree 12,
plus a global seed for \f$y(\sqrt{a})\f$. On 43 real transects the area
error matches the table's budget, the seeded inverse beats the table on
43 of 43 with unconditional Newton convergence, and the radius fit beats
the table including the discontinuous transects. A per-transect
acceptance check falls back to the table, so the feature is safe on
geometry the study never saw. Selection is `XSECT_GEOMETRY TABLES`
(default) or `CHEBYSHEV`, behind a CMake switch, with phases from a
benchmark baseline through the fitter, the evaluators and parity harness,
the finite-volume and dynamic-wave integrations and an end-to-end
sign-off, and kill criteria that leave the fitter as a study artifact if
the deck-level gates fail.

No part of this is in the tree: there is no `XSECT_GEOMETRY` option, no
Chebyshev evaluator in `src/`, no `OPENSWMM_CHEB_XSECT` switch. \status{Planned}

## 10.2 An implicit finite-volume solver

`plans/FV_IMPLICIT_HYPRE_SOLVER_EXPLORATION.md` (2026-05-13) is an
exploration, by its own status line, of a fully implicit finite-volume
reformulation of dynamic-wave routing. Each conduit is cut into
\f$N_c \ge 1\f$ control volumes carrying wetted area, with discharge
staggered at faces; junctions are zero-length volumes with a single head.
Backward Euler (or BDF-2) with new-time face fluxes gives a nonlinear
residual solved by Newton, its sparse linear systems preconditioned by
hypre BoomerAMG under GMRES or CG, with an Eigen back end when hypre is
absent. The motivation is the stiffness the Picard solver meets in looped
surcharged systems and mixed flow, and the wish for steps set by accuracy
rather than stability; the recommendation is a parallel, opt-in solver
preceded by a one-trunk-main feasibility prototype.

The prototype was never built, and two premises have moved beneath the
plan. The explicit solver of @ref hydraulics_ref_ch8_finite_volume
"Chapter 8" took the `FLOW_ROUTING FV` slot and the `FV_` prefix — its
`FV_CELL_LENGTH` and `FV_MIN_CELLS` occupy the ground the plan's
`FV_CELLS_PER_CONDUIT` and `FV_TARGET_CFL` were drawn for, and
`FV_DYNWAVE` does not exist — and the hypre dependency the plan is named
for left the build with the implicit 2D integrators (§10.9). As a plan of
record it would need re-founding on the current tree. \status{Planned}

## 10.3 The 1D finite-volume solver on Kokkos backends

The explicit solver of Chapter 8 was planned with a device backend
(`plans/EXPLICIT_FV_KOKKOS_1D_SOLVER_PLAN.md` §5): reconstruction, HLLC
fluxes with the upwinded species flux, a per-chain tridiagonal dispersion
solve, cell and node updates (the face-to-node gather as a deterministic
two-pass CSR), a Courant reduction, wet–dry flags and a small host pass
for structures, with state device-resident across every substep of a
routing step in deliberate parallel with the 2D plugin, and a shared
device cross-section module gated by a bit-exact extraction test.

What shipped is the option surface and the loader. `FV_BACKEND` accepts
`CPU`, `AUTO`, `OMP`, `CUDA`, `HIP` and `SYCL`, `FV_MIN_PARALLEL_CELLS`
gates `AUTO`, and `NetworkSolverFactory` searches the plugin path for
`openswmm_gpu_<backend>` and requires two symbols — `openswmm_gpu_probe`
and `openswmm_make_gpu_network_solver` — before accepting a plugin,
falling through to the CPU solver otherwise. **No plugin in the tree
exports the network-solver symbol**; the 2D plugins export the surface
and inertial-marcher factories only, so every device request resolves to
the CPU solver with a notice, as `ROADMAP.md` §7 records: "the same loader
hooks and option surface but no plugin implementation yet".

The benchmark campaign that was to gate the port (§7B.3) returned a
no-go at the sizes measured: 2000 conduits at four cells each does not
reach the 20 000-cell gate, and at that size the solver runs 189× serial
dynamic wave, so no parallel efficiency makes it competitive there. The
plan is careful about scope — the dynamic-wave prototype of §10.4
measured a Picard iteration, the wrong shape for a device, whereas the
explicit solver is local-stencil work of the right shape — and names the
undecided regime, \f$10^{5}\f$ to \f$10^{6}\f$ cells, together with the cheap
way to settle it: the solver's OpenMP thread scaling at those counts. The
Kokkos implementation is on hold pending that measurement (owner ruling
2026-08-08). \status{Planned}

## 10.4 Dynamic wave on GPU

`plans/1d/1D_DYNWAVE_GPU_KOKKOS_STRATEGY.md` (2026-06-13) asked whether
the link–node solver of Chapter 3 could be offloaded through Kokkos, and
answered with a working prototype (`plans/prototypes/dynwave_kokkos/`)
reproducing the solver's data flow — the structure-of-arrays conduit
tile, a conveyance kernel, the node-inflow scatter as atomics, the Picard
depth update and the global convergence reduction — with the same kernel
bodies called from a serial reference and the Kokkos loops. The port is
faithful: bit-exact on one thread, within \f$1.5 \times 10^{-8}\f$ ft on
four. The performance result set the direction aside: on the launch-cheap
OpenMP backend the parallel path was slower than serial at every size to
a million nodes (0.06× at 200, 0.81× at \f$10^{6}\f$), because three to
twenty Picard sweeps per step, each with a global reduction and a host
synchronization, are latency-bound until the element count is enormous;
a discrete GPU's crossover lies further right, at sizes that do not occur
in 1D practice.

The recommendation, adopted, is not to pursue single-model offload of the
dynamic-wave solver. Two reframings survived — Kokkos as a portability
layer for the already-threaded CPU loops, and ensemble parallelism (many
models of one topology in one launch), which the plan identifies as the
credible payoff and for which no design exists. `ROADMAP.md` §7 lists GPU
solvers as no longer deferred on the strength of the 2D plugins and the
hooks of §10.3; the dynamic-wave offload itself is retired on
measurement. \status{Retired}

## 10.5 The 2D solver on Metal

The Kokkos backends of §9.11.1 have no Apple-GPU target, because Kokkos
has no production Metal backend. `plans/2d/2D_GPU_METAL_STRATEGY.md`
(2026-07-10) scopes the one path that closes the gap: a native Metal
plugin, `libopenswmm_gpu_metal.dylib`, implementing the same
`ISurfaceSolver` interface and C plugin ABI as the Kokkos plugins and
loaded through the existing factory with three additive lines. The hard
constraint is precision: Apple Silicon GPUs implement no `double`, so the
plan proposes mixed precision by default (fluxes in `float`, the conserved
accumulation and coupling ledger in higher precision), double-single
emulation where validation demands it, and an acceptance bar of parity
within a documented tolerance rather than bit-identity; unified memory
makes the host–device hand-off a coherency matter rather than a copy.

The plan is stale in one structural respect: its port surface is the
implicit CVODE solver — nine right-hand-side kernels, a Jacobi
preconditioner and a matrix-free SPGMR loop, with a single-precision
SUNDIALS linked into the plugin — and that solver was retired on
2026-07-29 (§10.9). The engine a plugin must now reproduce is the
explicit marcher of §9.5, for which the ABI already reserves the
inertial-solver factory the Kokkos plugins implement. Packaging,
discovery, fallback and the precision analysis carry over; the kernel
inventory, milestones and validation cases do not. \status{Planned}

## 10.6 A bed-step treatment at finite-volume junction faces

`ROADMAP.md` §1.1 records, under "Peak attenuation at the default mesh",
that one cell per conduit attenuates the reference model's peaks by 37 %
on average (7 % at \f$\Delta x = 20\f$ ft), and that the cause is geometric
rather than diffusive: a cell-centred scheme places a single cell's bed at
the conduit mid-point, so every manhole presents an artificial bed step —
§8.3 and §8.10 describe the mechanism and its measured cost.
Higher-order reconstruction does not rescue it, because the error is in
where the bed is, not in how the state varies across the cell.
`FV_CELL_LENGTH` is the present workaround; the roadmap names "a bed-step
treatment at junction faces" as the fix.

**No plan file exists for this item.** It is one line in the roadmap's
remaining-scope list. What such a treatment must do follows from
Chapter 8: the junction face of §8.6.1 would see, on each side, the
conduit's true invert at the node rather than the cell's mid-point bed,
so that the hydrostatic reconstruction of §8.5.1 is performed against the
real step at the manhole and not a fictitious one — the reconstruction a
resolved mesh's interior faces already perform, which is why refinement
removes the attenuation. Whether by extrapolating the bed to the face, a
junction-face reconstruction, or a two-sided invert per cell, is
undecided. \status{Planned}

## 10.7 The TPA high-celerity filling divergence and the Vasconcelos and Wright (2009) hybrid flux

The two-component pressure approach of §8.4.5 shipped with one pinned
defect: on filling and reflection decks at high acoustic celerity — the
laboratory fixture at \f$a = 150\f$ m/s — explicit Euler diverged at the
reflected surge within seconds, a *temporal* odd–even pressure–vacuum
oscillation inside the flagged region that a local conservative filter,
tried and measured, could not damp. `plans/ISSUE_MIXED_FLOW_TPA_UF.md`
records the hybrid flux of Vasconcelos and Wright (2009) — the paper's
own post-shock treatment — as the documented contingency and an explicit
non-goal; `ROADMAP.md` §1.1 names it the designated fix path, with the
gate `FvTpa.KnownIssueHighCelerityFillingDiverges` pinning the divergence.

The review in `plans/FV_SLOT_TPA_REVIEW_2026-08-31.md` §5 then found a
family of divergences attributed to the closure to be an artefact of the
dynamic-wave step floor: the finite-volume substep was clamped *up* to the
legacy 1 ms `MIN_TIMESTEP`, discarding the Courant bound on short cells at
every `FV_CFL`, and the retry loop accepted the violating step silently.
A \f$10^{-6}\f$ s loop guard removed those, but the \f$a = 150\f$ m/s Euler
mode survived and the pin stood, correctly attributed. It fell on
2026-09-12 with the slot/free-surface wave-speed bound of the
closure-kernel program: Davis's symmetric estimate had carried the
acoustic celerity into the wave entering the *free-surface* side of a
pressurization front, and bounding that wave by the Rankine–Hugoniot bore
speed removes the mode. The fixture completes at 0.000 % continuity at
\f$a\f$ = 150, 300, 600, 1000 and 3000 m/s, and the gate is now the positive
test `FvTpa.HighCelerityFillingCompletes`
(`tests/unit/engine/test_fv_tpa_closure.cpp`). The roadmap entry, §8.4.5
and one test comment still cite the old gate name.

The hybrid flux is therefore no longer a required fix and has not been
implemented. It remains what the issue called it — a contingency should a
future deck reproduce the post-shock oscillation the paper describes —
with no plan to build it. \status{Planned}

## 10.8 LID units as storage nodes, the hydraulic side

`plans/LID_StorageNode_Redesign.md` and `ROADMAP.md` §6 record the design
for moving LID units out of the subcatchment and into the network, on the
argument that a green-infrastructure train's interactions — head-dependent
flow between units, backpressure from a saturated downstream unit, control
structures between units, an underdrain that sees the network's grade
line — are hydraulic phenomena a one-way runon cascade cannot mediate. The
hydraulic side uses machinery this manual already documents. A storage
node (Chapter 5) gains a `[LID_LAYERS]` attribute giving each layer a
fractional depth, porosity, conductivity and moisture limits; its storage
curve is corrected for the void fraction,

\f[V(d) = A\left\lbrack h_{surface} + \varphi_{media}\,h_{media} + \varphi_{gravel}\,h_{gravel} \right\rbrack\f]

so the head the node reports is the water surface accounting for media
occupancy, and that head is what the connected links see. The underdrain
is an ordinary orifice at the gravel-base offset — its discharge follows
the orifice law of Chapter 6 on the head difference to the receiving
node and reverses when the network surcharges above it, with no special
casing; surface overflow is a weir at the surface elevation; mid-media
outlets are further offset orifices; a chain of units is ordinary
node–link topology with control rules on the links between them.
Contributing areas drain to the node through the existing runon
mechanism. What is new — a kinematic Richards system for the unsaturated
media, its moisture-to-head mapping and a flux limiter — is hydrology.

The status is "design recorded, no implementation". The plan's code
sketches target the legacy C sources (`objects.h`, `lid.c`, `node.c`)
rather than the current C++ engine, so an implementation would begin by
re-siting them. \status{Planned}

## 10.9 Retired formulations

**The IMEX local-inertial integrator.** \status{Retired}
`plans/IMEX_LOCAL_INERTIAL_IMPLEMENTATION_PLAN.md` proposed advancing the
2D surface with SUNDIALS ARKStep as an implicit–explicit integrator: a
diffusive-wave Phase 1 with the lateral diffusion implicit, a
local-inertial Phase 2 with the state enlarged to cell volumes and edge
discharges, and a Froude-gated blend selected by `MOMENTUM` and
`FROUDE_BLEND`, preconditioned by Casulli's elliptic free-surface operator
under hypre BoomerAMG. Phase 1 was implemented as `ArkodeSurfaceSolver`
and measured on 2026-06-26: correct and conservative, and structurally
slower than CVODE — 2.7× on the road-culvert case, 4.9× to 31× on a
synthetic basin, worsening with scale, at twenty times the Newton and
fifty-nine times the Krylov work — because in the diffusive wave the only
stiff term is the one the split made implicit, so the split merely
exchanged an adaptive-order BDF for a fixed-order DIRK on the same global
solve. The plan's header records the premise as refuted; Phases 2 and 3
were never built, and the integrator went with the CVODE stack.

**The CVODE and ARKODE integrators.** \status{Retired} The implicit
integrators were retired on 2026-07-29: the explicit marcher of §9.5
outperformed them on the benchmark models and removed the SUNDIALS and
hypre dependencies (`ROADMAP.md` §7). `INTEGRATOR` accepts `EXPLICIT`
only; the fourteen keys that configured the retired stack, and any other
`INTEGRATOR` value, are accepted with WARNING 104 and ignored on file
load so legacy models still open, and are errors on the programmatic
option-set path (§9.11.2).

**`ADVECTION YES`.** \status{Retired} The local-inertial law with a
staggered upwind convective difference added to (9-7) on wet–wet faces
(Stelling and Duinmeijer, 2003) was the first attempt to supply the term
§9.2.1 drops. It was superseded on 2026-09-06 by
`MOMENTUM_EQUATION FULL_SWE`, which supplies the term conservatively with
shock capturing. The key is deprecated rather than removed: a deck that
carries it is warned and keeps the physics it asked for (§9.2.3,
Table 9-2).

**`VIRTUAL_JUNCTION_MOMENTUM FULL`.** \status{Retired} The cross-junction
momentum term `FULL` added at a virtual junction under dynamic-wave
routing was measured defective, not merely inaccurate: it is
sign-inverted with respect to the per-link convective term it supplements
(at constant discharge \f$\Delta(v^{2}A) = -v^{2}\Delta A\f$) and applied
to both adjacent links on top of each link's own term. On the SWASHES
MacDonald periodic case it destroyed 224–325 % of the routed volume;
negating it restored conservation but left 5.24 % \f$L^{1}\f$ error against
0.163 % for `BASIC`, so no term-level correction was worth keeping.
Retired 2026-08-14: `FULL` is accepted, warned and treated as `BASIC`
(`src/engine/input/handlers/OptionsHandler.cpp`). Under finite-volume
routing the question does not arise — a virtual junction is one interior
face, and conservation across it is a property of the scheme (§8.6.2).

**The `FV_NODE_*` keys.** \status{Retired} Five keys are retired in two
tiers, on purpose. `FV_NODE_COUPLING`, `FV_NODE_DT` and `FV_NODE_PICARD`
(2026-08-29) once selected explicit versus semi-implicit storage-node
coupling, whether the node bound (8-32) was armed, and the number of
correction sweeps; storage nodes are now always semi-implicit, the bound
always armed, the correction one sweep — each the former default. A deck
spelling the former default is not warned; one asking for the retired
behaviour (`EXPLICIT`, `NONE`, sweeps above one) is warned at open, since
a calibrated model would otherwise change answers silently.
`FV_NODE_CELL_COUPLING` and `FV_JUNCTION_MODEL` were already no-ops — the
bucket-junction model and the coupled-star correction were superseded by
the pass-through interface of §8.6.1 — and stay silent
(`src/engine/input/handlers/OptionsHandler.cpp`; §8.9).

The works cited above are listed in §8.12 (Vasconcelos, Wright and Roe,
2009) and §9.13 (Stelling and Duinmeijer, 2003).
<!-- source: plans/CHEBYSHEV_XSECT_FEASIBILITY_2026-08-21.md:1-60,§2-§5; plans/CHEBYSHEV_XSECT_GEOMETRY_PLAN.md:1-35,66-137,160-271; plans/FV_IMPLICIT_HYPRE_SOLVER_EXPLORATION.md:1-27,30-88,539-558,656-660; plans/EXPLICIT_FV_KOKKOS_1D_SOLVER_PLAN.md:289-336,785-835; src/engine/hydraulics/fv/NetworkSolverFactory.cpp:135-172; src/engine/input/handlers/OptionsHandler.cpp:522-535,541-548,566-593,599-626; ROADMAP.md:28-51,193-225; plans/1d/1D_DYNWAVE_GPU_KOKKOS_STRATEGY.md §0-§3,§6,§8; plans/2d/2D_GPU_METAL_STRATEGY.md §0-§3,§7,§9; src/engine/2d/input/SectionHandlers2D.cpp:175-182; plans/FV_SLOT_TPA_REVIEW_2026-08-31.md:273-335; plans/ISSUE_MIXED_FLOW_TPA_UF.md:1-80; tests/unit/engine/test_fv_tpa_closure.cpp:380-390,638-660; docs/manuals/reference/hydraulics/sections/Chapter8-FiniteVolume.md:522-532; plans/LID_StorageNode_Redesign.md:1-60,89-165,322-372; plans/IMEX_LOCAL_INERTIAL_IMPLEMENTATION_PLAN.md:1-50,99-140,336-367 -->
