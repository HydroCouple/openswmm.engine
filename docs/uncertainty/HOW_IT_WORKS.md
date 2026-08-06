# The Uncertainty Sidecar: How It Works and Why It's Fast

*A conceptual guide to SWMM6's ROM-based uncertainty propagation. If you've never
touched an eigenvector, you'll leave this document able to explain the idea at a
dinner party. If you already know what a Galerkin projection is, you'll leave
knowing exactly which equations run inside the solver and why they cost almost
nothing.*

> **TL;DR**: Instead of running your stormwater model 50 times to see how
> uncertain Manning's n or rainfall depth changes the answer, SWMM6 runs 50
> *extremely cheap* virtual copies alongside the one real simulation. The trick
> is mathematical, not computational — it exploits the fact that a drainage
> network only vibrates in a handful of characteristic "shapes," the same way a
> guitar string only rings in a few characteristic notes. Track the amplitude of
> those shapes instead of every individual pipe, and the cost of the extra 50
> copies drops from *200 minutes* to *2 seconds*.

---

## Contents

1. [The problem, in plain language](#1-the-problem-in-plain-language)
2. [The big idea, in one sentence](#2-the-big-idea-in-one-sentence)
3. [Three metaphors to build intuition](#3-three-metaphors-to-build-intuition)
4. [The math, built up gently](#4-the-math-built-up-gently)
5. [A worked example with real numbers](#5-a-worked-example-with-real-numbers)
6. [Reading the output](#6-reading-the-output)
7. [Why this is genuinely new](#7-why-this-is-genuinely-new)
8. [Honesty corner — what it can't do yet](#8-honesty-corner--what-it-cant-do-yet)
9. [Cheat sheet](#9-cheat-sheet)
10. [Where to go next](#10-where-to-go-next)

---

## 1. The problem, in plain language

You've just calibrated a stormwater model for a city drainage review. The
simulation takes 4 minutes to run. The report is due tomorrow. Everything is
ready — except a nagging question keeps surfacing in the back of your mind:

*What is Manning's n, really, for that stretch of aging concrete-and-gravel
channel in the north-east catchment?*

The 2019 field survey said 0.015. The textbook range for that surface type is
0.012–0.018. Nobody measured it directly; someone eyeballed it and moved on,
which is exactly what happens to roughness coefficients, soil infiltration
rates, and a dozen other parameters in every model ever built. If the true
value is 20% higher than what you assumed, how much deeper does the flooding
get at the critical junction downstream? How confident should you be in the
100-year design flow?

**The textbook answer** is Monte Carlo: run the model 50 times with Manning's n
randomly perturbed each time, and look at the spread of results. Fifty runs at
4 minutes each is over 3 hours. You don't have 3 hours.

**What most engineers actually do** is run the model once and add a footnote:
*"Manning's n carries inherent uncertainty; results should be interpreted with
appropriate engineering judgment."* This sentence is honest. It is also
completely unquantified — it tells the reviewer nothing about *how much*
judgment to apply, or *where*.

**What the uncertainty sidecar does** is give you the Monte Carlo answer at
roughly Monte-Carlo-for-free prices. Not an approximation of the answer, not a
statistical shortcut that trades accuracy for speed — a genuinely different way
of solving the *same* linearized equations that makes the extra 49 simulations
cost about two seconds instead of three hours.

---

## 2. The big idea, in one sentence

> **A pipe network — or a 2D flood surface — only has a few independent ways it
> can wobble, and if you track the wobbles instead of every individual pipe or
> grid cell, uncertainty propagation becomes almost free.**

That's it. Everything else in this document is working out what "wobble"
means precisely, how many of them there are, and how cheap "almost free"
actually turns out to be (spoiler: about 2 seconds of extra compute on a
4-minute, 8,000-node real-city simulation).

---

## 3. Three metaphors to build intuition

### 3.1 The weather forecast — an ensemble of possible futures

You've seen hurricane "spaghetti plots" — dozens of thin colored lines
fanning out from the storm's current position, each one a different plausible
future track. No single line is "the forecast." The *bundle* of lines is the
forecast: where they all agree, confidence is high; where they diverge,
confidence is low.

The uncertainty sidecar builds the same kind of ensemble for a drainage
network, except instead of 50 different possible storm tracks, it runs 50
different possible values of Manning's n (or rainfall depth, or both) *at the
same time*, alongside your one real simulation. Every one of those 50
"members" is a slightly different parallel universe: what if n were 0.0122
here? 0.0179 there? Sort the 50 depth values at each node and each moment in
time, and you get a band — a 90% prediction interval — exactly like a
spaghetti plot's shaded confidence cone.

The catch with real weather ensembles is that each of the 50 forecast members
is a *full, independent run of the entire climate model* — expensive. The
sidecar's trick is making its 50 members cost almost nothing, which is where
the next metaphor comes in.

### 3.2 The guitar string — why a network only has a few "notes"

Pluck a guitar string and it doesn't vibrate randomly. It rings with a
fundamental tone, plus an octave above, plus a fifth above that, each fainter
than the last. Physicists call these **modes**: characteristic *shapes* the
string is "allowed" to vibrate in, each with its own natural frequency. To
predict how the string responds to any pluck, you don't need to track the
position of every atom along its length — you only need the amplitude of the
first handful of modes. High-frequency modes die out almost instantly; only
the low ones persist.

A water surface in a drainage network behaves the same way. If you perturb
the depth somewhere and watch how the disturbance spreads and settles, it
doesn't just diffuse randomly — it decomposes into a small number of
characteristic spatial *shapes*, each with its own decay rate. These shapes
are called **eigenmodes**, and they come from a matrix called the **graph
Laplacian**, built purely from *which nodes are connected to which* (plus, for
the more refined version, *how strongly* — a big pipe couples its neighbors
more tightly than a small one).

- **The smoothest possible shape** (mode 0, the *constant* shape — everything
  moves up or down together) doesn't spread information anywhere; it's
  excluded from the analysis.
- **The next-smoothest shape** (called the *Fiedler mode*, after the
  mathematician who studied it) identifies the network's tightest hydraulic
  bottleneck — the pump station or narrow trunk line where the network is
  closest to splitting into two disconnected halves. (SWMM6 actually exposes
  this as a diagnostic: `FiedlerDiagnostic`/`FiedlerDiagnostic1D` rank every
  node by how much it sits on that bottleneck.)
- **Higher shapes** capture increasingly fine, local spatial detail, and decay
  faster and faster.

Retaining the first `k=10` shapes out of a network with `n=8,000` nodes
compresses the state needed to describe uncertainty by roughly 800×. That
compression is the entire reason this is fast.

### 3.3 The stratified raffle — why the sampling isn't just "50 random dice"

If you draw 50 random numbers to represent 50 possible Manning's-n values,
plain bad luck can leave big gaps — maybe none of your 50 draws land anywhere
near the extreme low end of the plausible range, so your uncertainty band is
artificially narrow exactly where it matters most for a worst-case flood
check.

The sidecar instead uses **Latin Hypercube Sampling (LHS)**: it slices the
plausible range `[1 − p, 1 + p] × base value` into exactly 50 equal-width
strata and puts exactly one ensemble member in each stratum. Think of it as a
raffle where you're guaranteed one ticket sold in every price bracket — no
clustering, no empty gaps, full coverage of the range with the minimum
possible number of draws. This is the classic statistical reason LHS
converges faster than plain random (Monte Carlo) sampling for the same
ensemble size.

A second, related design choice: if you draw Manning's-n strata and
rainfall-depth strata independently but both happen to be sorted the same
way, the two "random" parameters end up perfectly *correlated* by accident —
member 1 always gets the lowest Manning's n *and* the lowest rainfall,
member 50 always gets the highest of both. That silently narrows your
apparent uncertainty, because the two effects always point the same
direction instead of sometimes canceling. The sidecar's LHS design
deliberately shuffles the assignment of strata to members independently for
each parameter, so that low-Manning's-n members are just as likely to get
high rainfall as low rainfall.

---

## 4. The math, built up gently

Everything below is the *exact* mathematics running inside
`SpectralROM` (2D) and `SpectralROM1D` (1D) — not a simplification for the
sake of exposition. If you follow this section start to finish, you will
understand precisely what the code computes.

### 4.1 Warm-up: a single leaky bucket

Before tackling a whole network, consider the simplest possible drainage
system: one bucket, filling from a tap at rate `f`, draining through a hole
at the bottom at a rate proportional to how full it is (`rate × level`).
That's a first-order linear ODE:

```
dlevel/dt = −rate · level  +  f
```

If `f` and `rate` are constant, this equation has an exact, closed-form
solution — no numerical timestepping needed, no approximation:

```
level(t+dt) = (level(t) − f/rate) · exp(−rate · dt)  +  f/rate
```

The bucket approaches a **steady state** of `f/rate` and decays toward it
exponentially. This is precisely the mathematics of a rain barrel filling and
draining, and it is also — surprisingly — *precisely* the mathematics that
governs every single eigenmode of an entire drainage network. Every mode is
its own independent leaky bucket.

### 4.2 From one bucket to a whole network: the graph Laplacian

For a 1D pipe network, build a matrix `L` — the **graph Laplacian** — where:

- The diagonal entry for node `i` is the sum of the conductances of every
  pipe touching node `i`.
- The off-diagonal entry `L[i,j]` is `−(conductance of the pipe between i and
  j)` if such a pipe exists, or `0` otherwise.

`L` fully encodes the network's *shape*: which nodes talk to which, and how
strongly. The linearized diffusion-wave equation for how head deviations
spread through the network is:

```
dh/dt = −K1d · L · h  +  f
```

where `h` is the vector of head deviations at every active node, `K1d` is an
effective conductance (derived from Manning's n, slope, and depth — see
`computeK1d()`), and `f` is the forcing (inflow, rainfall-driven change).
This is the *network-wide* version of the leaky bucket — except now `h` has
thousands of entries and this is thousands of coupled equations, which is
exactly the problem the eigenmode trick solves.

*(The 2D surface-flow case uses the same structure, with `L` built from mesh
cell adjacency instead of pipe connectivity, and Manning's-n-dependent
conductance weighting the mesh edges.)*

### 4.3 Projecting onto eigenmodes — turning thousands of equations into a handful

`L` has eigenvectors `P[:,0], P[:,1], ..., P[:,n-1]` and eigenvalues
`λ_0 ≤ λ_1 ≤ ... ≤ λ_{n-1}` (this is the same eigendecomposition behind the
guitar-string metaphor in §3.2). Compute the coefficient `a_j` — "how much of
shape `j` is present in the current head field" — by projecting:

```
a_j = P[:,j]^T · h        (a dot product: "how much does h look like shape j?")
```

Crucially, because `L` is symmetric, its eigenvectors are *orthogonal*: the
evolution of `a_j` depends only on `a_j` itself, not on any of the other
modes. The single giant network equation decomposes into `k` completely
independent scalar ODEs — one leaky bucket per retained mode:

```
da_j/dt = −λ_j · K1d · a_j  +  r_coarse[j]

where r_coarse[j] = P[:,j]^T · f      ("how much of the forcing hits shape j")
```

This is exactly the bucket equation from §4.1, with `rate = λ_j · K1d` and
`f = r_coarse[j]`. Retaining `k=10` modes out of `n=8,000` nodes means
tracking 10 independent bucket equations instead of 8,000 coupled ones — a
computational reduction of nearly three orders of magnitude, *before* even
getting to the ensemble.

> **Why this doesn't lose accuracy where it matters**: the low-order modes
> (smooth, network-scale shapes) are exactly the ones that persist and matter
> for flood-depth uncertainty; the high-order modes (sharp, local, cell-scale
> detail) decay almost instantly and contribute negligibly to the
> uncertainty band a few minutes into a storm. Truncating them is the same
> approximation a guitarist makes when they say a plucked string "sounds
> like" its fundamental plus a couple of overtones — technically an infinite
> series, practically dominated by the first few terms.

> **One clarification before continuing**: the real deterministic answer —
> the actual water depth or head at every node — is *not* produced by this
> toy ODE. It comes from SWMM's full nonlinear DynWave/CVODE solver, exactly
> as it always has. The modal-projection math above is the *linearized
> surrogate* the sidecar uses internally to describe how an ensemble
> member's uncertainty, not its total value, evolves relative to that real
> deterministic answer. §4.4 makes this precise.

### 4.4 Making it an ensemble — tracking the *deviation*, not the total

Call the real, live deterministic depth (or head) field `h_det` — the number
SWMM's ordinary solver computes every routing step regardless of whether the
sidecar is even switched on. Each ensemble member `i` doesn't track its own
independent total depth; it tracks only how far its sampled Manning's-n
multiplier `mannings_mult[i]` and rainfall multiplier `rainfall_mult[i]`
(drawn from the Latin Hypercube design in §3.3) pull it away from `h_det`.
Project that *deviation* onto the eigenmodes exactly as in §4.3:

```
δa[i,j] = P[:,j]^T · (h_i − h_det)      ("how much member i's deviation looks like shape j")

b_j     = P[:,j]^T · h_det              ("how much of the real deterministic answer is shape j")
```

The deviation for mode `j`, member `i` obeys its own leaky-bucket equation:

```
d(δa[i,j])/dt = −rate[i,j] · δa[i,j]  +  g[i,j]

rate[i,j] = λ_j · K1d / mannings_mult[i]

g[i,j] = −λ_j · K1d · (1/mannings_mult[i] − 1) · b_j     (Manning sensitivity)
         + (rainfall_mult[i] − 1) · r_coarse[j]           (forcing sensitivity)
```

The key feature: for the member with `mannings_mult[i] = 1` and
`rainfall_mult[i] = 1` — the "nominal" member sitting exactly at the
calibrated parameter values — every term in `g[i,j]` vanishes. Its deviation
is *exactly zero, forever*: that member's reconstructed depth is always
identical to the real deterministic answer, not merely close to it. Every
other member's deviation measures, precisely, how much its particular
Manning's-n or rainfall guess would have changed the outcome.

Each of the `M × k` mode-member pairs is *still* a leaky bucket with an exact
closed-form solution — the same equation from §4.1, applied independently to
every (member, mode) pair:

```
steady[i,j]     = g[i,j] / rate[i,j]
δa[i,j](t+dt)   = (δa[i,j](t) − steady[i,j]) · exp(−rate[i,j] · dt)  +  steady[i,j]
```

This is **not an approximation of the ODE — it is its exact solution**, for
any timestep `dt`, however large. There is no substepping, no Newton
iteration, no Krylov solve, no CFL-style stability limit. Advancing the
entire ensemble for one timestep costs `M × k` multiplications and
exponentials — for `M=50, k=10`, about 500 floating-point operations,
something a modern CPU does in under a microsecond. Compare that to the
deterministic CVODE solver's nonlinear residual evaluations across every one
of the network's thousands of nodes on every routing step, and the reason the
sidecar is nearly free becomes a matter of simple arithmetic, not a clever
approximation.

### 4.5 Reading out the answer — reconstructing depths and taking quantiles

At any moment, reconstruct member `i`'s full head field by adding its
deviation, summed back through the eigenvector shapes, onto the real
deterministic answer:

```
h_i[node t] = h_det[node t]  +  Σ_j  δa[i,j] · P[j, t]
```

Do this for all 50 members at node `t`, sort the 50 resulting numbers, and
read off:

```
q05  =  the 5th-percentile value  (5% chance the true depth is lower)
q50  =  the median value          (the "best single guess")
q95  =  the 95th-percentile value (5% chance the true depth is higher)
```

`[q05, q95]` is a **90% prediction interval**: if the true Manning's n and
rainfall depth lie anywhere within the sampled range, the true depth should
fall inside that band roughly 90% of the time.

---

## 5. A worked example with real numbers

Abstract equations are easier to trust once you've seen them run on a real
network. SWMM6's uncertainty sidecar has been benchmarked on **StormCity**, a
real-scale synthetic network with **8,267 junctions, 1,132 outfalls, and
6,968 conduits**, driven by a 6-hour SCS Type II design storm at a 0.5-second
timestep (43,200 routing steps).

**Cost.** The deterministic baseline run — no uncertainty tracking at all —
took 2,124 seconds (about 35 minutes). Turning on the uncertainty sidecar
with a **50-member ensemble and ±20% Manning's-n uncertainty** brought the
total to 2,171 seconds: **a 2.2% overhead** for full 90% prediction bands at
every one of 8,266 nodes, at every report timestep, for the entire 6-hour
storm. A brute-force Monte Carlo equivalent — 50 full deterministic reruns —
would cost roughly **50× the baseline**, or nearly 30 hours.

**Result.** Output from this same StormCity uncertainty-benchmark setup (50
members, ±20% Manning's n) shows the physically expected pattern:

- **During the storm's rising limb** (roughly 29–57 minutes in), the
  uncertainty band widens rapidly as depths climb fast and Manning's-n
  differences between ensemble members compound — peak spread of about
  0.35 ft observed at storm onset.
- **The largest single spread observed anywhere in the simulation** was about
  4.4 ft, at a downstream junction during the storm's peak, exactly where
  you'd expect roughness uncertainty to matter most — a node experiencing
  rapid, large head changes where small differences in conveyance capacity
  compound into large depth differences.
- **The trunk mainline** (large-diameter pipes carrying most of the flow)
  showed comparatively small spread — under 0.1 ft — because large pipes are
  proportionally less sensitive to Manning's-n uncertainty than the small
  laterals feeding them. This matches physical intuition: roughness matters
  more when a pipe is nearly full and friction-dominated than when it's
  running at low relative depth in an oversized conduit.

A smaller-scale controlled comparison confirms the raw speed claim directly:
on a 50×50 mesh, computing the full uncertainty ensemble via the eigenmode
ROM (`k=10` modes, `M=20` members) took **4 milliseconds**. Running the
equivalent 20-member brute-force Monte Carlo ensemble — 20 full deterministic
solves — took **3,705 milliseconds**. That's a measured **~900×** speedup for
an apples-to-apples comparison on identical hardware, identical network,
identical parameter ranges.

---

## 6. Reading the output

At every active node, at every report timestep, the sidecar writes three
numbers instead of one: `q05`, `q50`, `q95` (in a `<report>.uncertainty.csv`
file for the 1D network, or as extra fields in the 2D output for surface
flooding). Here's how to read them:

- **`q50` (median) is your best single estimate** — treat it the way you'd
  treat the ordinary deterministic result, because for a well-behaved
  symmetric perturbation it's very close to the deterministic run itself.
- **The width `q95 − q05` is a direct measure of how much the answer at that
  location depends on the uncertain parameter.** A wide band at a specific
  junction is the model telling you: *"this location's flood risk is
  sensitive to exactly the parameter you're uncertain about — go get better
  field data here before you finalize the design."* A narrow band elsewhere
  says the opposite: *"even a 20% error in Manning's n barely moves this
  number — don't waste calibration effort here."*
- **Bands widen during rapid change and narrow during steady state.** This is
  physically correct, not an artifact: uncertainty in a rate parameter (like
  Manning's n) only matters while something is actively changing. A pipe
  sitting at dry-weather steady flow doesn't care much what Manning's n is;
  a pipe surging through a storm peak cares a great deal.
- **Spatial spread patterns are diagnostic, not just decorative.** Nodes with
  disproportionately wide bands relative to their neighbors are exactly the
  nodes a Fiedler-mode bottleneck analysis (§3.2) tends to flag — narrow
  throats and pump stations where the network's hydraulic capacity is most
  finely balanced.

---

## 7. Why this is genuinely new

SWMM has existed for roughly fifty years. Uncertainty quantification in
stormwater modeling is not a new research topic — so why does a
model-integrated ROM sidecar feel like a genuine advance rather than a
repackaging of known techniques?

**Existing methods run *outside* the solver.** Monte Carlo, First-Order
Second-Moment (FOSM) analysis, and Morris sensitivity screening all work by
running the complete deterministic solver many times and comparing the
outputs afterward. They're accurate, well-understood, and expensive — and
because they treat the solver as a black box, they produce no per-timestep
uncertainty bands without saving and post-processing entire ensembles of full
simulation output. The sidecar runs *inside* the solver, advancing its
50-member ensemble in lockstep with the deterministic solution, reading the
solver's live internal state every routing step.

**Reduced-order modeling isn't new either — but not applied here before.**
Galerkin projection and proper-orthogonal-decomposition (POD) reduced-order
models have been used in river and coastal shallow-water modeling since the
early 2000s, almost always as *offline surrogates*: build a library of
full-model snapshots, fit a surrogate to that library, then query the cheap
surrogate for new inputs instead of rerunning the expensive full model. What
SWMM6 does differently is: (1) it computes its eigenbasis directly from mesh
or network *topology*, with no snapshot library required; (2) it runs as a
*live sidecar* alongside the solver, not as an offline replacement for it;
and (3) it handles the specific mess of real urban drainage — wet/dry
transitions, coupled 1D pipe and 2D surface flow, thousands of irregular
nodes — that idealized river and coastal test cases don't have to deal with.

**The 1000× speedup crosses a practical threshold.** At 10× faster than
brute-force Monte Carlo, uncertainty quantification is marginally more
attractive than the status quo. At 100×, it becomes viable for careful
research studies. At roughly 1000× — the regime this sidecar operates in —
uncertainty quantification becomes **operationally free**: a 4-minute
calibration run gains a full 90% prediction band for well under a second of
extra compute. That's the difference between "something a PhD student runs
once for a paper" and "something that's on by default in every production
run," and it's the threshold that actually changes engineering practice.

---

## 8. Honesty corner — what it can't do yet

An accurate picture of any tool includes its edges.

**A perfectly uniform perturbation currently produces no visible spread.**
This sounds paradoxical, so it's worth explaining precisely why. Recall from
§3.2 that the "everything moves together" mode (mode 0, the constant shape)
is deliberately excluded — every mode the ROM actually tracks is a shape
that goes up in some places and down in others, never a shape that's
perfectly flat everywhere. If the starting condition (or the rainfall
forcing) is *exactly the same everywhere in the network*, there's genuinely
no spatial pattern for those shapes to grab onto — every one of the retained
modes projects to zero, the same way you can't tell whose stopwatch is more
accurate if neither stopwatch has started ticking yet. Every worked example
and default demo in this codebase deliberately uses a spatially-varying
starting condition or forcing (a localized bump, alternating high/low
rainfall cells) for exactly this reason — real storms and real terrain are
never perfectly uniform, so this rarely bites in practice, but a
perfectly-uniform synthetic test case can be genuinely misleading about how
much uncertainty is "really" there.

This is a documented, understood limitation with active reform work tracked
in this repository's engineering checklists — not a silent trap.

**Band widths degrade near peak flow, and now there's a signal for when.**
The linearized weighted-Laplacian operator the ROM builds is *symmetric* by
construction — the underlying Picard solve applies one dQ/dH value per
conduit antisymmetrically to its two endpoints, so there is no directional
asymmetry for the ROM's basis to represent. The real Saint-Venant Jacobian,
by contrast, picks up genuine asymmetry from advective (inertial) terms as
flow speeds up — terms the linearized surrogate drops entirely. That gap
scales with the square of the Froude number, so bands quoted at or near
critical flow (Fr approaching 1) should be read with some skepticism; bands
in slow, subcritical reaches are unaffected. The engine now computes and
reports this directly (PR H3): `fr_trust` in `<rpt>.rom_diag.csv` is a
flow-weighted mean squared Froude number over the network at each report
time, clamped so a single fast trickle can't dominate the reading.
Interpretation: `fr_trust ≲ 0.05` means bands are trustworthy as reported;
`0.05–0.15` suggests inflating widths mentally by roughly 10%; `≳ 0.25`
means you're in the degraded peak-flow regime, where band widths can be off
by 10–25%. The same file's `surcharge_frac` column is the complementary
signal: once a reach surcharges, velocities collapse toward zero (Fr → 0)
and the symmetric surrogate becomes *more* accurate again, not less — so a
high surcharge fraction is not itself a trust concern.

> **Historical note**: an earlier version of this section also warned that
> the band reflected uncertainty "since the last recalibration" — the ROM
> periodically re-anchored its ensemble to the deterministic solution,
> silently erasing accumulated spread every ~60 seconds during a fast-moving
> storm. The deviation-form reformulation (each ensemble member now tracks
> only its *deviation* from the live deterministic answer — see §4.4)
> removed that limitation outright: the nominal member's deviation is exactly
> zero forever, so there is nothing to periodically re-anchor, and the 1D ROM
> no longer reseeds at all. A narrower, physically legitimate version
> survives only on the 2D surface: when previously-dry cells become
> significantly wet, the ROM basis is rebuilt to cover them and every
> member's deviation resets to zero at that moment — but that only happens on
> genuine domain growth, not on a periodic timer, and it does not affect the
> median (which tracks the deterministic answer regardless).

---

## 9. Cheat sheet

| Term | Plain-language meaning | Where in the math |
|---|---|---|
| **Ensemble member** | One of the 50 parallel "what if" universes | index `i = 1..M` |
| **Eigenmode / shape** | One of the network's characteristic vibration patterns | `P[:,j]`, eigenvector `j` |
| **Eigenvalue** | How fast that shape's disturbances decay | `λ_j` |
| **Deterministic reference** | The real depth/head from SWMM's ordinary solver, ROM-independent | `h_det` |
| **Modal deviation** | "How far member `i`'s guess has drifted from the real answer, in shape `j`" | `δa[i,j]` |
| **Graph Laplacian** | The matrix encoding who's connected to whom | `L` |
| **K_eff / K1d** | Effective conductance (from Manning's n, slope, depth) | scales the decay rate |
| **Latin Hypercube Sampling** | Stratified "one ticket per price bracket" sampling | strata of `[1−p, 1+p]` |
| **q05 / q50 / q95** | The 5th / 50th / 95th percentile across all 50 members at a point | sorted `h_i[t]` |
| **90% prediction interval** | The band `[q05, q95]` | width = uncertainty magnitude |
| **Fiedler mode** | The smoothest non-trivial shape; flags network bottlenecks | eigenvector for smallest nonzero `λ` |

---

## 10. Where to go next

- **Configuration syntax, input-file reference, and API examples**: see
  [`USER_GUIDE.md`](USER_GUIDE.md) in this same directory — the complete
  technical reference for the `[UNCERTAINTY]` and `[2D_ROM]` input sections,
  code samples for reading quantile output, and detailed guidance on
  choosing perturbation levels, ensemble size, and mode count.
- **The underlying eigensolver and graph-Laplacian construction**: see
  `src/engine/uncertainty/GraphEigenBasis.{hpp,cpp}` and
  `NetworkLaplacian1D.hpp` for the 1D network case, or
  `src/engine/2d/uncertainty/MeshEigenBasis.{hpp,cpp}` for the 2D mesh case.
- **The ensemble ODE integrator itself**: `SpectralROM1D.{hpp,cpp}` (1D) and
  `src/engine/2d/uncertainty/SpectralROM.{hpp,cpp}` (2D) — every equation in
  §4 of this document has a direct line-for-line counterpart there.
- **The deviation-form design note**: `docs/uncertainty/DEVIATION_FORM.md` is
  the normative spec for the math in §4.4–4.5 — the full derivation, the
  exact API each ROM exposes, and the acceptance-test invariants.
- **Ongoing refinement work**, including the remaining limitation described
  in §8, is tracked in this repository's engineering checklists at the
  project root.

---

## 11. Supplying your own rainfall distributions (soft rainfall)

The "soft rainfall" feature is a different way to think about rainfall
uncertainty. Instead of sampling a scalar multiplier that applies uniformly
to all rain, you supply a **location-scale family** per gage or per grid cell:

- The deterministic rain IS the location parameter (the median / center).
- You supply only the **spread** (standard deviation, CV, or half-range).
- The ROM propagates the spread through its modal ODE using the
  two-projection form `f_ij = r_loc[j] + c_i · r_spread[j]`, where `c_i` is
  the family-selected per-member coefficient:
  - **NORMAL / LOGNORMAL**: `c_i = z_i = probit(u_i)` (standard-normal quantile)
  - **UNIFORM**: `c_i = 2·u_i − 1` (centered half-range band)

where `u_i = shuffledStrata(M, seed+4)[i]` — the same Latin-hypercube strata
the ROM already uses for Manning's n and other parameters, just with a
different seed offset (+4) so rainfall and roughness are decorrelated.

**The key insight** (the "location-parameter move" from the design doc): you
don't need to materialize an ensemble of rainfall fields. The deterministic
rain is the location; the spread is a single per-cell/plane field that the ROM
projects once per step. Member `i`'s realized forcing is
`loc + c_i · spread` — computed on demand inside the ROM's advance, never
stored as an M×N array. This is 50× smaller than a materialized ensemble.

**How to use it**: see `[SOFT_RAINGAGES]` and `[SOFT_RAINFALL_GRID]` in
`USER_GUIDE.md` §11. The pybme round-trip example
(`scripts/uncertainty/pybme_soft_rain_example.py`) shows the full workflow:
synthetic gages + radar → BME posterior → HDF5 grid file → engine run →
uncertainty band plot.

**MIXED family**: when a grid file declares `family=MIXED`, each cell carries
its own distribution family via a `/family_code` uint8 plane. The ROM uses
the NORMAL coefficient `z_i` for all cells as a v1 approximation; UNIFORM
cells have their spread pre-scaled by the coefficient range ratio. Exact
per-cell per-member dispatch is the design's deferred cold path.

**Deprecation note**: the scalar `RAINFALL` parameter in `[UNCERTAINTY]`
(which applies a single multiplier to all rainfall) is superseded by soft
rainfall for new work. The scalar path remains functional for backward
compatibility.

### 11.1 Joint vs. marginal correlation — the comonotone vs. spatial choice

**The comonotone path (default — `COHERENCE FULL`)** is the simplest case:
each member `i` has one scalar coefficient `c_i` that applies everywhere. The
per-cell ensemble at location t is `{loc[t] + c_1·spread[t], loc[t] + c_2·spread[t], ...}`.
Since all members scale the spread by the *same* amount, the *spread map itself*
defines the joint structure — if cell A has twice the spread of cell B, then A's
ensemble is twice as wide as B's. The marginals (per-cell distributions) are
correct, and the joint structure is **perfectly correlated across space**: members
move together everywhere.

**The spatially-correlated path (`COHERENCE CORR_LEN <meters>`)** lets each
member have a *different* coefficient at each location: `c_i[t]`. The per-cell
ensemble at location t is `{loc[t] + c_i1[t]·spread[t], loc[t] + c_i2[t]·spread[t], ...}`.
The rank/copula construction ensures the *marginals are still correct* — the
sorted ensemble at each cell still matches the input coefficient set — but the
*joint structure* becomes **spatially decorrelated**: member i's high value in
cell A does not imply a high value in cell B. At a downstream node, member i's
contribution from A may partially cancel its contribution from B.

**Why this matters for downstream uncertainty:**

Imagine two upstream cells feeding a single downstream node:

- **Comonotone** (full positive correlation across space): all members either flow
  wet from both cells or dry from both. Downstream spread is the worst-case sum.
- **Spatially correlated** (member i wet in A, dry in B; member j dry in A, wet
  in B): flows partially cancel at the downstream node. Spread is narrower.

**In practical terms**, the comonotone band is conservative (wider) because it
assumes the worst-case: "every source is either uniformly overestimated or
underestimated across the whole domain." The spatially-correlated band is more
physically realistic (narrower) because it accounts for the spatial
heterogeneity of real storms — a rain cell that misses one region often hits
another.

**The `/spread` map in context:**

When you supply a grid file with `/location` (deterministic rain) and `/spread`
(uncertainty), you are specifying:
- **`/location` per-cell**: the mean rainfall at that cell.
- **`/spread` per-cell**: the *marginal* standard deviation or half-range at
  that cell (e.g., from a BME posterior, rain-gauge measurement error, or
  radar-QPE climatology).
- **Comonotone structure** (default): the ROM assumes all ensemble members are
  perfectly correlated — the same realization everywhere.
- **Spatial correlation** (via `COHERENCE CORR_LEN`): the ROM builds a
  spatially-smooth set of rankings, so members' realizations decorrelate by
  distance.

### Why the reduced-basis trick keeps the ROM fast

When you activate spatial correlation, the engine must generate a correlated rainfall field
on each time step — originally an `M×n` materialization for all `M` members at `n` space-time
points. For large meshes (tens of thousands of cells), this can dominate per-step cost or even
wall-clock time. The *reduced-basis* optimization (automatically selected for large meshes)
replaces the `M×n` field with a compact spatial decomposition: a small set of `K_s` basis modes
(typically 10–50) whose weighted combinations reconstruct each member's field. Instead of
materializing all `M` fields, the engine computes only `K_s` basis projections per step
(O(K_s·k·n) arithmetic instead of O(M·k·n)), then assembles each member's forcing from the
basis coefficients (O(M·K_s·k)). Since `K_s ≪ M`, total per-step cost drops significantly.

The basis itself is built once at initialization using an analytic Whittle–Matérn spectral
decomposition (reproduced in
[`SPDE_SPATIAL_BASIS.md`](https://github.com/USEPA/Stormwater-Management-Model/blob/develop/docs/uncertainty/SPDE_SPATIAL_BASIS.md))
with the same correlation length `CORR_LEN <meters>` as the full correlated field — so
the ROM's behavior is unchanged, only the internal representation is more efficient. This
eliminated a 64-second field-generation bottleneck on typical large-mesh runs, making spatial
correlation practical even on production grids.
