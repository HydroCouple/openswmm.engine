@page quality_ref_ch7_ard_transport Chapter 7: Advection–Reaction–Dispersion Transport

@tableofcontents

## 7.1 Introduction

Chapter 5 describes the transport model SWMM has always used: each conduit is
treated as a continuously stirred tank reactor (CSTR), or optionally as a
short series of them. That model is robust and inexpensive, and it is exact
for the quantity it tracks — the mass leaving a link over a routing step — but
it carries no information about *where inside the link* mass is. A slug
entering a long conduit appears at the outlet immediately, diluted across the
whole link volume, rather than travelling down it as a front.

OpenSWMM adds two transport engines that resolve position along a conduit, and
retains the original as the default. They are selected per model with the
`[OPTIONS] QUALITY_SOLVER` key:

| `QUALITY_SOLVER` | Engine | Representation |
|---|---|---|
| `LEGACY` (default) | Tanks-in-series | One or more CSTRs per link (Chapter 5) |
| `EULERIAN_ARD` | Eulerian finite volume | Fixed cells along each conduit |
| `LAGRANGIAN` | Lagrangian (LARD) | Water parcels that move with the flow |

All three engines share the same external loads, the same treatment
expressions, the same mass-balance ledger and the same reporting surfaces. A
model can be switched between them without editing anything but the one key,
which is what makes the comparison meaningful: differences in results are
differences in the transport representation, not in the problem being solved.

**The default is unchanged.** A model that does not set `QUALITY_SOLVER`
behaves exactly as it did before, bit for bit.

## 7.2 Governing Equation

Both new engines solve the one-dimensional advection–reaction–dispersion
equation, which is Chapter 5's Equation 5.1 with the dispersion term retained
rather than absorbed into the tank model:

\f[\frac{\partial c}{\partial t} = -\frac{\partial(uc)}{\partial x} + \frac{\partial}{\partial x}\left(D\frac{\partial c}{\partial x}\right) + r(c)\f]

where \f$c\f$ is concentration, \f$u\f$ the cross-sectionally averaged
velocity, \f$D\f$ the longitudinal dispersion coefficient, and \f$r(c)\f$ the
reaction rate. The three terms are advection, dispersion and reaction, and the
two engines differ chiefly in how they treat the first.

The equation is solved by **operator splitting**: within a transport step,
advection is applied first and dispersion second, each as a complete step
(a Lie, or first-order, split). Reaction is applied afterwards. The splitting
error is therefore \f$O(\Delta t)\f$ rather than the \f$O(\Delta t^2)\f$ a
symmetric Strang split would give. This is a deliberate choice rather than an
accuracy claim: the advective substep is already limited by a Courant
condition, and the dispersion solve is unconditionally stable, so the
first-order split costs little and keeps the sequence simple to reason about.

## 7.3 The Eulerian Engine (ARD)

### 7.3.1 Spatial discretization

Each conduit is divided into a fixed number of cells of length
\f$\Delta x\f$, sized at initialization from the conduit length and the
`[TRANSPORT_OPTIONS] TARGET_DX` key. Node storage is represented separately;
cells hold only conduit water. Species are stored species-major, so all cells
of one species are contiguous — the layout the flux kernels sweep.

Cell wetted area is taken from the hydraulic solution each routing step, so
the transport mesh always describes the same water the hydraulic engine
routed. A cell whose area falls below a dryness threshold is skipped rather
than divided by, and its mass is held rather than discarded.

### 7.3.2 Advection

Face fluxes are computed with a **flux-limited high-resolution scheme**. A
first-order upwind flux is computed for stability, a higher-order correction
is computed for accuracy, and the correction is limited so that the result
introduces no new extrema — the flux-corrected transport idea, applied here in
the form Leonard describes for conservative advection schemes
(Leonard, 1991). Without limiting, a second-order scheme produces oscillations
ahead of and behind a sharp front, which for a concentration means values
below zero.

The scheme is selected with `[TRANSPORT_OPTIONS] SCALAR_SCHEME` and the
limiter with `LIMITER`:

| Key | Value | Behaviour |
|---|---|---|
| `SCALAR_SCHEME` | `UPWIND` | First-order upwind alone — monotone, strongly diffusive |
| | `MUSCL` | Second-order reconstruction with limiting (default) |
| `LIMITER` | `MINMOD` | Most diffusive of the limited options; safest on rough data |
| | `VANLEER` | Smooth, symmetric; the usual default |
| | `SUPERBEE` | Least diffusive; sharpens fronts, can over-steepen smooth profiles |

A useful way to read the table: `UPWIND` never produces a value outside the
range of its neighbours but smears a sharp front over many cells;
`SUPERBEE` keeps the front nearly intact but will convert a smooth gradient
into a staircase. `VANLEER` is between them and is what a model should use
unless a specific reason argues otherwise.

The advective step is subcycled to satisfy a Courant condition

\f[C = \frac{u\,\Delta t_{sub}}{\Delta x} \le C_{max}\f]

with the substep count chosen so the condition holds in every wet cell. The
subcycling is internal to the transport step; the hydraulic solution is frozen
across it, which means refining the transport step refines transport alone.

**A worked example.** A 400 ft conduit at `TARGET_DX = 50` ft has eight
cells. Water moving at 4 ft/s over a 60 s routing step covers 240 ft — nearly
five cells — so a single explicit step would be violently unstable. The
engine computes \f$C = 4 \times 60 / 50 = 4.8\f$, and with
\f$C_{max} = 0.9\f$ takes \f$\lceil 4.8/0.9 \rceil = 6\f$ substeps of
10 s each. Nothing in the deck changes; the cost is six flux sweeps instead
of one. Halving `TARGET_DX` doubles both the cell count and the substep
count, so refinement is quadratic in cost — which is why `TARGET_DX` is the
key worth thinking about before reaching for a shorter routing step.

### 7.3.3 Dispersion

The dispersion term is solved implicitly along each conduit chain with a
tridiagonal (Thomas) solve, which is unconditionally stable and so places no
additional restriction on the step size. Chains span splice junctions, so a
conduit interrupted by a virtual junction disperses as one length rather than
as two.

The coefficient \f$D\f$ is set by `[TRANSPORT_OPTIONS] DISPERSION`:

| Mode | Coefficient |
|---|---|
| `OFF` (default) | \f$D = 0\f$; the dispersion solve is skipped entirely |
| `VALUE` | A single user-supplied \f$D\f$, in ft²/s or m²/s |
| `FISCHER` | Computed per conduit from the local hydraulics |

The `FISCHER` closure follows the shear-flow dispersion relations collected by
Fischer et al. (1979), which build on Taylor's analysis of dispersion in a
tube (Taylor, 1953) and Elder's extension to open-channel shear:

\f[D = 0.011\,\frac{u^2 B^2}{Y\,u_*}, \qquad u_* = \sqrt{g Y S}\f]

with \f$B\f$ the surface width, \f$Y\f$ the mean depth, \f$S\f$ the
friction slope and \f$u_*\f$ the shear velocity. The coefficient is
recomputed each step from the current hydraulics, so it responds to a
rising limb rather than describing one design condition.

In a shear-dominated open channel, mechanical dispersion arising from the
velocity profile exceeds molecular diffusion by several orders of magnitude,
so the coefficient is a property of the flow field rather than of the
transported substance — which is why the same coefficient is applied to every
species, including temperature (§9.4).

**Magnitudes worth recognising.** For the 1.5 ft pipe at 4 ft/s used above,
flowing half full, \f$D\f$ from the Fischer closure lands in the range of a
few ft²/s. Molecular diffusion of a dissolved solute is around
\f$10^{-8}\f$ ft²/s — eight orders of magnitude smaller, which is why no
mode of this key offers it. A user-supplied `VALUE` far below 0.1 ft²/s is
almost always a unit error rather than a modelling choice.

When `DISPERSION OFF` is in effect the dispersion kernel is not merely passed
a zero coefficient; it is not entered at all, so results are identical to a
build without the dispersion path.

## 7.4 The Lagrangian Engine (LARD)

The Lagrangian engine represents each conduit as an ordered slab of water
**parcels** — segments with a volume and a concentration — that move with the
flow rather than past a fixed mesh. Numerical diffusion of a front is
therefore not introduced by the advection scheme at all: a parcel carries its
concentration unchanged until it mixes.

Each transport step proceeds in five phases:

1. **Drain.** Every conduit sends the volume that left it over the step, taken
   from its downstream end, into its receiving node's inflow ledger, carrying
   the corresponding mass. A link whose volume has fallen below its remaining
   parcel volume sheds the difference through its upstream face, so the parcel
   volumes always sum exactly to the link volume the hydraulic engine reports.
2. **Mix.** Nodes are processed in flow-aware topological order. Each node
   mixes its own held volume with everything that arrived, as a CSTR — the
   same formula junctions, dividers, storage units and outfalls all reduce to.
   External loads join here, through the same loader seam the other two
   engines use. Zero-volume links (pumps, orifices, weirs, outlets) pass the
   node's *new* concentration through within the same step, which is why the
   ordering must be topological.
3. **Release.** Each conduit gains a new parcel at its upstream node's new
   concentration, sized so the slab total again equals the link volume.
   Adjacent parcels whose concentrations agree within a tolerance are merged,
   which is what keeps plug flow from growing an unbounded number of parcels.
4. **Decay.** First-order decay is applied to parcels and node stores by exact
   exponential, and the removed mass is booked to the reacted ledger row.
5. **Publish.** Link concentration is reported as the volume-weighted mean of
   the parcels, so outfall loads and final-storage accounting see the same
   mass the parcels hold.

Cycles in the node ordering are broken in index order, and the residue is
carried in the ledger to the next step rather than dropped.

**The parcel budget is what makes this tractable.** Left alone, a conduit
under steady flow gains one parcel per step forever. Two mechanisms bound it:
adjacent parcels whose concentrations agree within a tolerance are merged at
the RELEASE phase, and `[OPTIONS] MAX_SEGMENTS_PER_LINK` (default 100) caps
the slab, with the oldest parcels merged when the cap is reached. Under plug
flow with a constant inlet concentration, merging collapses the whole link
back to a single parcel every step and the cost is trivial; under a rapidly
varying inlet, the cap is what stops the memory growing without limit — at
the price of blending the oldest resolution first, which is the right end to
lose.

`[OPTIONS] QUALITY_STEP` substeps the whole five-phase sequence within a
routing step. Unlike the ARD subcycling above, this is user-chosen rather
than derived from a stability condition: LARD has no Courant limit, because
parcels move with the flow rather than past a mesh. The key exists for
accuracy of the node mixing, not for stability, and omitting it (or setting
it at or above the routing step) degenerates to a single substep and is
bit-identical to a model that never set it.

### 7.4.1 Dispersion under LARD

Because parcels do not exchange mass by construction, dispersion under LARD is
introduced explicitly, by a **random-walk particle method**: particles sample
the vertical shear profile to estimate inter-parcel exchange, and the estimated
exchange is applied to the parcel field. Particles carry no mass themselves —
they are an instrument for estimating the exchange coefficient, not a second
representation of the solute. The random draw is keyed by a deterministic seed
(`[OPTIONS] RWPT_SEED`), so a model gives the same answer on every run and on
every machine.

## 7.5 Boundaries, Sources and Extraction

Two sections supply species mass directly to the transport domain, independent
of the hydraulic inflows:

- **`[TRANSPORT_BOUNDARIES]`** sets the concentration carried by water
  entering at a node.
- **`[TRANSPORT_SOURCES]`** applies a distributed mass rate along a conduit,
  spread over its cells in proportion to cell length.

A **negative** rate denotes *extraction* rather than an error. Extraction is
clamped to the mass the element actually holds, so a request larger than the
available mass removes what is there and no more; the unmet portion is counted
and summarized at the end of the run rather than silently satisfied by driving
a concentration negative. Extraction clamps routinely while a system fills —
a near-empty element cannot satisfy any withdrawal — so a nonzero clamp count
is not by itself a modelling error.

Both sections address **multi-species reaction species** (Chapter 8) rather
than `[POLLUTANTS]` rows, which take their loads through the ordinary
`[INFLOWS]` and washoff pathways. A worked pair:

```
[PROCESS_COMPONENTS]
org.hydrocouple.openswmm.transport.ard  config="model.ard"
```

and in `model.ard`:

```
[TRANSPORT_OPTIONS]
DISPERSION        FISCHER
TARGET_DX         50
SCALAR_SCHEME     MUSCL
LIMITER           VANLEER

[TRANSPORT_BOUNDARIES]
J0   CL2   VALUE       1.2          ; mg/L on water entering at J0
J4   CL2   TIMESERIES  TS_PLANT     ; a measured plant residual

[TRANSPORT_SOURCES]
C7   CL2   VALUE      -0.004        ; a withdrawal, 4 mg/s along C7
```

A boundary sets the concentration carried by water **entering** at a node; a
source injects (or, negative, extracts) mass along a conduit independent of
its flow. Per-conduit dispersion overrides live in the same file and win over
the global model on the conduits they name.

## 7.6 Mass Balance

All three engines report through the same continuity table described in
Chapter 5. Two caveats apply to the newer engines:

- Species declared through the multi-species reaction system (Chapter 8) are
  carried on the transport mesh but do **not** yet have per-species
  mass-balance rows; the continuity table is indexed by pollutant. Mass
  delivered by `[TRANSPORT_SOURCES]`, which addresses reaction species,
  therefore appears in the continuity *error* rather than as a labelled inflow
  term.
All three engines now apply treatment expressions, the multi-species reaction
system, and the bed zone of §9.3.11; there are no remaining silent bypasses,
and any configuration an engine cannot honour is refused or warned by name at
model open rather than quietly ignored.

## 7.7 Implementation

The Eulerian engine is implemented in
`src/engine/transport/components/EulerianArdComponent/`, with the shared flux
and dispersion kernels in `src/engine/transport/fvkernels/`. The Lagrangian
engine is in `src/engine/quality/lard/` — `SegmentStore.hpp` for the parcel
slabs, `LagrangianSolver.hpp` for the five-phase step, and
`RwptDispersion.hpp` for the random-walk closure. Engine selection is resolved
in `src/engine/core/SWMMEngine.cpp`, and the shared clamp bookkeeping for
negative sources is in `src/engine/quality/NegativeSources.hpp`.

Input keys are documented in the User Manual's `[TRANSPORT_OPTIONS]`,
`[TRANSPORT_BOUNDARIES]` and `[TRANSPORT_SOURCES]` sections, and the same
configuration is readable and editable through the C API declared in
@ref openswmm_transport.h.
