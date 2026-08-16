# OpenSWMM Uncertainty Sidecar — User Guide

> **Status (2026-07-15):** Phases 0–9 complete; WQ uncertainty layer wired (PR 13a/b).  
> 45 unit tests + 6 regression tests + 2 engine integration tests + 2 WQ uncertainty tests pass.  
> The 2D spectral ROM (scalar + spatial), coupling uncertainty, runoff ensemble, WQ bounds,
> Fiedler diagnostics, the 1D spectral ROM struct, the 1D ROM engine lifecycle, and the
> water-quality uncertainty layer (`QUALITY` target with per-node concentration bounds) are all
> implemented and tested.  HDF5 quantile output is the one remaining gap.

---

## Contents

1. [What this is](#1-what-this-is)
2. [Quick start](#2-quick-start)
3. [Input file reference](#3-input-file-reference)
4. [How it works](#4-how-it-works)
5. [How to interpret outputs](#5-how-to-interpret-outputs)
6. [Advanced configuration](#6-advanced-configuration)
7. [Implementation inventory](#7-implementation-inventory)
8. [Known limitations](#8-known-limitations)
9. [Performance](#9-performance)
10. [Visualization](#10-visualization)

---

## 1. What this is

The uncertainty sidecar runs an M-member parameter ensemble **alongside** the deterministic CVODE
2D solver — not instead of it — at roughly 1000× lower cost than full Monte Carlo.  After each
routing step you get three additional per-cell depth fields:

| Field | Meaning |
|---|---|
| `q05` | 5th-percentile depth across the ensemble |
| `q50` | Median depth (best single estimate) |
| `q95` | 95th-percentile depth |

Together `[q05, q95]` is a **90th-percentile prediction interval**: if the true Manning's n and
rainfall intensity lie within the perturbation ranges you specify, there is roughly a 90 %
probability that the true depth at each cell lies inside the band.

**What parameters can be uncertain:**

| Layer | Parameter | Section |
|---|---|---|
| 2D surface | Manning's n (scalar or spatially-correlated) | `[2D_ROM]` + `[UNCERTAINTY]` |
| 2D surface | Rainfall intensity (scalar or spatially-correlated) | `[2D_ROM]` + `[UNCERTAINTY]` |
| 2D coupling | Discharge coefficient Cd (inlet grates) | `[UNCERTAINTY]` |
| 1D sewer | Manning's n (pipe roughness) | `[UNCERTAINTY]` |
| 1D sewer | Lateral inflow / DWF rate per node | `[UNCERTAINTY]` |
| Runoff | Soil hydraulic conductivity (Green-Ampt Ks, Horton f0/fmin, CN S) | `[UNCERTAINTY]` |
| Water quality | First-order decay rate k (per pollutant) | `[UNCERTAINTY]` |

---

## 2. Quick start

Add `[2D_ROM]` and `[UNCERTAINTY]` to your `.inp` file. The `[UNCERTAINTY]` section overrides
the perturbation values in `[2D_ROM]`, so you only need one of the two for a basic run.

```ini
[2D_ROM]
ENABLE        YES
MEMBERS       50
MODES         10

[UNCERTAINTY]
;;Layer  Parameter    Perturbation
2D       MANNINGS_N   0.20
2D       RAINFALL     0.10
```

That's it. After the simulation, read quantiles from the engine (see §2.1) or from the future
HDF5 output file (§7.1).

### 2.1 Reading quantiles (current API)

Quantiles are accessed via the C++ API after `swmm_engine_step`:

```cpp
#include "openswmm/engine/openswmm_engine.h"
#include "core/SWMMEngine.hpp"
#include "2d/uncertainty/SpectralROM.hpp"

SWMM_Engine handle = swmm_engine_create();
swmm_engine_open(handle, "model.inp", "model.rpt", nullptr, nullptr);
swmm_engine_initialize(handle);
swmm_engine_start(handle, 0);

double elapsed = 1.0;
while (elapsed > 0.0)
    swmm_engine_step(handle, &elapsed);

auto* eng = static_cast<openswmm::SWMMEngine*>(handle);
const openswmm::twoD::SpectralROM* rom =
    eng->surfaceRouter2D().rom();   // null if ROM inactive or not yet seeded

if (rom && rom->is_ready()) {
    // rom->q05, rom->q50, rom->q95 — length n_tri each
    for (int i = 0; i < n_tri; ++i)
        printf("cell %d: q50 = %.4f m  [%.4f, %.4f]\n",
               i, rom->q50[i], rom->q05[i], rom->q95[i]);
}
```

The cast is safe: `SWMM_Engine` is `typedef void*` and is always `new SWMMEngine()` stored
as a pointer. All C++ symbols are exported (`CXX_VISIBILITY_PRESET default`).

---

## 3. Input file reference

### 3.0  `[2D_ROM]` vs `[UNCERTAINTY]` — what each section controls

These two sections serve different purposes and can be used together or independently.

| Section | Controls | Does NOT control |
|---|---|---|
| `[2D_ROM]` | *How* the ROM runs: ensemble size M, modes k, K_eff, spatial correlation lengths, reseed thresholds, parametric tails | Perturbation levels; which parameters are uncertain |
| `[UNCERTAINTY]` | *What* is uncertain and by how much: which physical parameters carry a prior, the perturbation half-range, and which model layer (1D/2D) | ROM mechanics |

Think of `[2D_ROM]` as the **solver configuration** (accuracy, cost, spatial resolution) and `[UNCERTAINTY]` as the **experiment configuration** (what you don't know and by how much).

**How they interact:**
- A `2D MANNINGS_N 0.20` entry in `[UNCERTAINTY]` overrides `MANNINGS_PERT` in `[2D_ROM]` and also implicitly enables the ROM (`ENABLE YES`).
- If both sections are present, `[2D_ROM]` sets the ROM mechanics; `[UNCERTAINTY]` overrides only the perturbation levels.
- You do not need both. For a minimal run: `[UNCERTAINTY] 2D MANNINGS_N 0.20` is sufficient.

**1D sewer ROM**: there is no `[1D_ROM]` section. The 1D sewer ROM is activated exclusively by a `1D` layer entry in `[UNCERTAINTY]`. A pure 1D model (no 2D mesh) only needs `[UNCERTAINTY]`.

### 3.1 `[2D_ROM]`

Controls the ROM solver. All keywords are optional; defaults shown.

| Keyword | Default | Range | Description |
|---|---|---|---|
| `ENABLE` | `NO` | YES/NO | Activate the ROM sidecar. |
| `MEMBERS` | `50` | ≥ 2 | Ensemble size M. More members → smoother quantiles, O(M) cost. |
| `MODES` | `10` | ≥ 1 | Laplacian eigenmodes retained for the ROM basis. More modes → finer spatial resolution of spread. Capped at min(MEMBERS, n_tri−1). |
| `MANNINGS_PERT` | `0.20` | ≥ 0 | Half-range for Manning's n: each member's n ∈ [1−p, 1+p] × n_base. Overridden by `[UNCERTAINTY] 2D MANNINGS_N`. |
| `RAINFALL_PERT` | `0.20` | ≥ 0 | Half-range for rainfall intensity. Overridden by `[UNCERTAINTY] 2D RAINFALL`. |
| `K_EFF` | `≤ 0 → AUTO` | any | Effective diffusive conductance (m^(4/3)/s). Values ≤ 0 activate AUTO mode (see §4.8). |
| `MANNINGS_CORR_LEN` | `0.0` | ≥ 0 | Spatial correlation length (m) for Manning's n field. 0 = uniform scalar per member (fast). |
| `RAINFALL_CORR_LEN` | `0.0` | ≥ 0 | Spatial correlation length (m) for rainfall field. 0 = uniform scalar per member. |
| `WET_RESEED_FRACTION` | `0.05` | [0, 1] | 2D-only. Rebuild the ROM basis (and reset every member's deviation to zero) when the wet-cell count changes by more than this fraction of n_tri, so the ROM extends coverage to newly-wet cells. Not a periodic drift correction — see §4.5, §6.3. No 1D equivalent; the 1D ROM never reseeds. |
| `WET_RESEED_MIN_INTERVAL` | `60.0` | ≥ 0 | Minimum simulation time (s) between consecutive wet-domain rebuilds. Prevents excessive rebuilding on an oscillating wetting front. |
| `PARAMETRIC_TAILS` | `NO` | YES/NO | Fit a log-normal to the wet-member sub-population and use the analytic 95th percentile as q95 (reduces noise from top 1–2 samples when M is small). q05/q50 remain sort-based. |
| `MODE_DROP_THRESHOLD` | `1e-10` | ≥ 0 | Drop mode j from `advance()` when its energy E_j and rainfall forcing are both below this threshold. Automatically reactivated by rainfall. |

### 3.2 `[UNCERTAINTY]`

Format: `LAYER  PARAMETER  [DISTRIBUTION]  PERTURBATION`

- `DISTRIBUTION` is optional; defaults to `UNIFORM`.
- Supported distributions: `UNIFORM`, `NORMAL`, `LOGNORMAL`.
- `[UNCERTAINTY]` entries **override** the corresponding scalar fields in `[2D_ROM]`.

**Currently parsed parameters:**

```ini
[UNCERTAINTY]
;;Layer  Parameter    [Distribution]  Perturbation
2D       MANNINGS_N                   0.20    ;; overrides [2D_ROM] MANNINGS_PERT
2D       RAINFALL     UNIFORM         0.15    ;; same effect as RAINFALL_PERT
1D       MANNINGS_N                   0.20    ;; enables 1D sewer ROM (see §7.7)
1D       RAINFALL                     0.20    ;; lateral-inflow uncertainty for 1D
QUALITY  TSS                          0.30    ;; decay-rate uncertainty for TSS pollutant
```

`LAYER` is either `2D` (surface routing), `1D` (sewer network routing), or
`QUALITY` (water-quality decay rate).

Both `2D` and `1D` layers accept `MANNINGS_N` and `RAINFALL` with any supported
distribution.

The `QUALITY` layer is different — its `PARAMETER` token is a **pollutant name**
(from `[POLLUTANTS]`), not a hydraulic parameter name. It declares uncertainty in
that pollutant's first-order decay rate `k`. The name is resolved to a pollutant
index at engine initialization (after `[POLLUTANTS]` has been parsed), so the
`[UNCERTAINTY]` section may appear before or after `[POLLUTANTS]` in the file.

**QUALITY layer rules (v1):**
- `DIST` defaults to `UNIFORM`; `UNIFORM` is the only supported distribution in v1.
- The perturbation `p` is a relative half-range on the decay rate: each member's
  `k_i ∈ [k(1−p), k(1+p)]`.
- The `ENTRY` is fixed to `RATE_MULT` (the decay rate is a rate multiplier);
  an explicit `ENTRY` token is not accepted for `QUALITY` in v1.
- `QUALITY` entries do **not** activate the hydraulic ROM — they produce analytic
  concentration bounds independently (see §5.3).

A `2D` entry also enables and configures the 2D ROM (sets `enable_rom = true` in
`[2D_ROM]`).  A `1D` entry only populates the uncertainty config — the 1D ROM is
built automatically in `initialize()` whenever any active source is present, or
whenever the 2D ROM is enabled (so 2D→1D coupling always sees per-member 1D heads).

> **Tip — pure 1D (no 2D mesh):** adding `1D  MANNINGS_N  0.20` to a plain 1D `.inp`
> is sufficient; no `[2D_ROM]` or `[2D_MESH]` sections are needed.

> **Tip — water quality only:** adding `QUALITY  TSS  0.30` produces per-node
> concentration bounds without activating any ROM. A model with `[POLLUTANTS]`
> and `[UNCERTAINTY] QUALITY <pollutant> <p>` is sufficient.

Support for `SOIL` (runoff ensemble) and `CD` (coupling-flux uncertainty) are set
via C++ API directly (parsed by `UncertaintyEnsemble`, not by the `.inp` handler).

### 3.3 `[2D_OPTIONS]` — solver settings

These govern CVODE accuracy, not the ROM. Set tighter tolerances for small or stiff meshes.

| Keyword | Default | Description |
|---|---|---|
| `MAX_TIMESTEP` | `10.0` | Max CVODE internal step (s) |
| `MIN_TIMESTEP` | `0.001` | Min CVODE internal step (s) |
| `REL_TOLERANCE` | `1e-4` | Relative error tolerance |
| `ABS_TOLERANCE` | `1e-6` | Absolute error tolerance (m) |
| `DRY_DEPTH` | `0.001` | Cells below this depth (m) are treated as dry |
| `MAX_CVODE_STEPS` | `500` | Max CVODE steps per SWMM routing interval |
| `COUPLING_CD` | `0.65` | Default discharge coefficient for 2D↔1D inlets |

---

## 4. How it works

### 4.1 The operator's problem

An engineer has just finished calibrating a SWMM model for a city drainage review. The
deterministic run takes 4 minutes. The report is due tomorrow. Everything seems set — until
the question arises: *what is Manning's n for that mixed concrete-and-gravel surface in the
north-east catchment?*

The calibration data from 2019 suggest 0.015. The textbook range is 0.012–0.018. Field
inspection says "somewhere in there." If the true value is 20% higher than assumed, how much
deeper does flooding get at the critical junction? How confident should the engineer be in the
100-year return level?

**The traditional answer**: run 50 simulations with n perturbed across the range. Cost: 200
minutes. The engineer does not have 200 minutes.

**What most engineers do instead**: run one simulation and add a caveat — "Manning's n is
uncertain, results may vary." This is honest but quantifies nothing.

**What the ROM sidecar does**: run 50 virtual members as a lightweight parallel process alongside
the single deterministic solver. Extra cost: about 2 seconds on a 4-minute model. At every node
or cell, at every timestep, you get three additional fields: `q05` (optimistic scenario),
`q50` (best estimate), `q95` (pessimistic scenario). Together they form a **90th-percentile
prediction interval** — if the true Manning's n is anywhere within ±20% of the calibrated
value, the depth lies inside the band for roughly 90% of timesteps.

### 4.2 Why eigenmodes — the guitar string analogy

A guitar string vibrates in discrete modes: the fundamental, the octave, the fifth above that.
Each mode has a characteristic frequency and a characteristic spatial shape. To describe how the
string responds to any perturbation you do not need to track every atom — you only need the
amplitudes of the first few modes. Higher modes decay almost instantly; only the low-frequency
modes persist over timescales of interest.

Water surfaces in drainage networks behave the same way. The "shape modes" of a 2D mesh — or a
1D pipe-network graph — are the eigenvectors of the **graph Laplacian** L. The Laplacian
encodes which cells or nodes are connected and how strongly. Its eigenvectors are the natural
decay modes of any diffusing quantity on that network:

- **Mode 0 (null mode)**: a uniform depth shift — it does not spread and is excluded.
- **Mode 1 (Fiedler mode)**: the smoothest non-trivial mode; it identifies the network's
  tightest hydraulic bottleneck (see §7.4 for the diagnostic).
- **Higher modes**: increasingly fine-grained spatial patterns that decay faster and faster.

When Manning's n is uncertain, the **uncertainty in the water surface** is well-captured by
the first k eigenmodes. A member with high Manning's n evolves differently from a member with
low n — but both evolve in the same low-dimensional subspace spanned by those k modes.
Retaining k=10 modes out of n=10,000 cells reduces the per-member state from 10,000 numbers
to 10 — a 1000× compression of state space.

### 4.3 The key elegance — an exact solve, no timestep limit

After projecting onto the eigenbasis, the linearised diffusion-wave PDE becomes k **independent
scalar ODEs**, one per retained mode per ensemble member:

```
da[i,j]/dt = −rate[i,j] · a[i,j]  +  f[i,j]

rate[i,j] = λ_j · K_eff / mannings_mult[i]   (mode j decays faster for smoother channels)
f[i,j]    = r_coarse[j] · rainfall_mult[i]   (rainfall projected onto mode j)
```

Each ODE has an **exact analytical solution** — the exponential integrator:

```
a[i,j](t+dt) = (a[i,j](t) − f[i,j]/rate[i,j]) · exp(−rate[i,j] · dt)  +  f[i,j]/rate[i,j]
```

This is not an approximation. It is the exact solution for the linearised system at any `dt`.
There are no substep limits, no Newton iterations, no Krylov solves. Advancing the entire
M-member ensemble requires M×k multiplications and exponentials — roughly 500 floating-point
operations for M=50, k=10. A modern CPU executes this in under 1 microsecond.

Compare with the deterministic CVODE solver, which performs many nonlinear RHS evaluations on
all n cells per routing step. The ROM sidecar costs less than 0.1% of total simulation time.

### 4.4 Why this is new

SWMM is 50 years old. Uncertainty quantification in stormwater modelling has been studied for
decades. So why does this feel new?

**Previous methods run outside the solver.** Monte Carlo, FOSM (first-order second-moment), and
Morris sensitivity screening all require running the full solver N times and comparing outputs.
They are expensive, disconnected from the solver's internal state, and produce no per-timestep
uncertainty bands without storing complete simulation ensembles. The ROM sidecar runs *inside*
the solver, advancing its ensemble in lock-step with the deterministic solution, with access to
the current depth field at every routing step.

**ROM ideas existed but not for this problem.** Galerkin and POD-based ROMs have been applied to
river and coastal shallow-water equations since the early 2000s — but almost exclusively as
offline surrogates: build a surrogate from a library of full-model snapshots, then query it
instead of the full model for new inputs. What is different here is the specific combination:
(1) a *graph-Laplacian eigenbasis* computed once from mesh topology (no snapshot library
needed), (2) running as a *live sidecar* inside the solver rather than replacing it, and
(3) handling irregular urban networks with wet/dry transitions and coupled 1D/2D domains. That
combination has not been applied to SWMM-class urban drainage before.

**The engine architecture had to catch up first.** The ROM's inner loop iterates over all N
node depths for every ensemble member on every routing step — a tight numerical loop that is
sensitive to memory layout. SWMM5's object-oriented design stored node data as arrays of
structs (each node object holding all its fields). Iterating over depths meant pointer-chasing
through non-contiguous memory: cache-unfriendly at scale. SWMM6's data-oriented restructuring
(struct-of-arrays: `nodes.head[]`, `nodes.depth[]`, `links.roughness[]` as flat contiguous
arrays) was a prerequisite for the ROM sidecar to be fast enough to run at every timestep on
large networks. The data layout change that looks like an internal refactor is also what makes
the 1000× speedup practically achievable.

**The 1000× speedup crosses a practical threshold.** At 10× speedup, uncertainty quantification
is marginally better than Monte Carlo. At 100×, it is practical for research. At 1000×, it is
operationally free: a 4-minute simulation gains a 90th-percentile prediction band for under
0.25 seconds of additional compute. That threshold is what makes this useful in everyday
engineering practice, not just in papers.

---

*The remaining subsections give the precise mathematical definitions for implementers.*

### 4.5 ROM setup and lifecycle

The 2D and 1D ROMs are built and seeded independently at `initialize()`, then advance alongside
their respective solvers at every routing step.

#### 2D ROM — what runs when

**At `initialize()`** — mesh Laplacian built, eigenbasis computed, ROM allocated:

The 2D mesh Laplacian is assembled from triangle connectivity (shared-face lengths, centroid
distances, cell areas). The Lanczos+QL eigensolver extracts the k lowest non-trivial eigenvectors
P[:,0..k−1] and eigenvalues λ₀..λ_{k-1} from this Laplacian, and stores them in a
`MeshEigenBasis`. The basis is a function of mesh geometry alone, so it is built once and
reused for the whole run. A
`SpectralROM` struct is allocated with an M×k coefficient matrix (all zeros) and an LHS
parameter design for M members.

**At the first `CvodeSurfaceSolver::advance()` call** — seeding:

`seedROM()` zeroes every member's modal deviation and stores the current CVODE depth array `h`
as the deterministic reference `h_det`:
```
δa[i,j] = 0    for all members i = 0..M−1
h_det   = h
```
Every member starts exactly on the deterministic solution (deviation form — see §4.6), so
spread = 0 at seed time. The different LHS multipliers (`mannings_mult[i]`, `rainfall_mult[i]`)
give each member a different Manning-sensitivity and forcing-sensitivity term on the very next
`advance()` call, so spread grows immediately — and the nominal member (`mult = 1`) stays
exactly on `h_det` for the rest of the run.

**At each subsequent CVODE step** — advance and quantiles:

1. `applyCouplingFluxToROM()` injects per-member drainage-flux *deviations* at coupling points
   (if any; see §4.11) — only the difference from the deterministic exchange is applied.
2. `SpectralROM::advance(dt, K_eff, rainfall, h_cell, h_det)` updates all M×k deviations via the
   exponential integrator, using the live CVODE depth as `h_det`.
3. `computeQuantiles(h_det, parametric_tails)` reconstructs per-cell depths for all M members
   (`h_det + P·δa`, floored at 0) and sorts to get q05/q50/q95.

**Basis rebuild on wet-domain growth** — extending ROM coverage, not correcting drift:

If the wet-cell count changes by more than `WET_RESEED_FRACTION × n_tri` since the last seed,
and at least `WET_RESEED_MIN_INTERVAL` seconds have elapsed, the ROM rebuilds its depth-weighted
basis and re-seeds from the current CVODE depth (all deviations reset to zero). This exists
solely to extend the ROM's coverage to a newly-wet region that the previous basis didn't
represent — it is not a periodic correction for accumulated drift (the deviation formulation
has no drift to correct: the nominal member never leaves the deterministic trajectory). Spread
briefly collapses to zero at a rebuild and re-grows as members diverge again; the median is
unaffected either way, since it already tracks `h_det`.

#### 1D ROM — what runs when

**At `SWMMEngine::initialize()`** — network Laplacian built, eigenbasis computed, ROM seeded:

`buildROM1D()` collects all conduit node pairs from `ctx_.links`. Outfall nodes are identified
from `ctx_.nodes.type` and excluded from the active set — they are fixed-head boundaries and
cannot carry spreading uncertainty. `NetworkLaplacian1D::buildUniform()` assembles the graph
Laplacian in CSR format over the remaining active nodes. `GraphEigenBasis::build()` runs
Lanczos+QL to extract k eigenvectors and eigenvalues. The ROM is then seeded immediately from
the current hydraulic state — every member's modal deviation zeroed, and the current heads
stored as the deterministic reference:
```
δa[i,j] = 0                    for all members i = 0..M−1
h_det_active = h_active
```
where `h_active` contains only the heads of active (non-outfall) nodes in the ROM's index
space. The `full_to_active[]` map (indexed by SWMM node index) records which active ROM index
corresponds to each node, and returns −1 for outfalls.

**At each `stepRouting()` call** — advance and quantiles:

1. `computeK1d()` estimates mean diffusion-wave diffusivity from current conduit depths, slopes,
   and roughnesses, then normalises by conduit length² to yield K1d [1/s].
2. `SpectralROM1D::advance(dt, K1d, h_det_active, runoff)` updates all M×k deviations, using the
   current active-node heads as the live deterministic reference `h_det_active`.
3. `computeQuantiles(h_det_active, invert_active)` reconstructs per-active-node head quantiles
   (`h_det_active + P·δa`, clamped to each node's invert elevation).

The 1D ROM has **no reseed mechanism at all**. It seeds once at `initialize()` and, from then
on, only ever advances the deviation ODE — there is nothing to periodically re-anchor, because
the nominal member (all multipliers = 1) never leaves the deterministic trajectory by
construction (deviation form — see §4.6).

#### How the sidecar attaches — five hook points

The ROM is a **read-only observer** of the deterministic solver. It never writes to CVODE
state or the DYNWAVE solution; the solver drives, the sidecar observes and reports. But it
does not attach loosely — it reads from five specific hook points, each of which extracts a
quantity the deterministic solver computes anyway:

| Hook | What the sidecar reads | How it is used |
|---|---|---|
| `initialize()` | Mesh/network topology — triangle connectivity, conduit node pairs, cell areas, conduit lengths | Assembles the Laplacian eigenbasis: the same operator CVODE will invert at every Newton step |
| First `CvodeSurfaceSolver::advance()` | CVODE depth array `h[]` (current state after solver warmup) | Seeds all M members: zeroes every deviation, stores `h` as `h_det` |
| Each `advance()` | Current wet-cell depths, Manning's n values, bed slopes, and the live deterministic depth/head (`h_det`) | Computes K_eff (2D) or K1d (1D); `h_det` supplies the Manning-sensitivity forcing term (§4.6) |
| Coupling exchange | Deterministic DYNWAVE node heads from `computeCouplingExchange()` | Per-member orifice equation — M realisations of what the inlet head difference is, applied as a deviation from the deterministic exchange |
| Wet-domain check (2D only) | CVODE's current wet-cell count | Decides when the ROM basis needs rebuilding to cover newly-wet cells (not a drift correction — the 1D ROM has no equivalent hook at all) |

This is the barnacle design: the ROM attaches to the hull at initialization, moves with the
solver through every timestep, draws parameters from quantities the solver already computes,
and emits three extra depth fields per cell without altering the solver's own trajectory.

#### How the two ROMs differ in basis construction

| | 2D ROM (`SpectralROM`) | 1D ROM (`SpectralROM1D`) |
|---|---|---|
| Laplacian type | Geometric mesh Laplacian (area/distance-weighted) | Graph (topological) Laplacian (connectivity only, uniform weights) |
| Eigenvalue character | Carries spatial scale implicitly via mesh geometry | Dimensionless — counts graph connectivity, not metres |
| Diffusivity parameter | K_eff [m^(4/3)/s] — spatial scale baked into mesh Laplacian | K1d = D/L² [1/s] — must normalise by L² to give 1/s |
| When seeded | Deferred to first CVODE advance (solver warms up first) | Immediately at `initialize()` (from initial steady-state heads) |
| Automatic reseeding | Yes — triggered by wetting-front advance | No |
| Eigenbasis type | `MeshEigenBasis` (mesh geometry) | `GraphEigenBasis` (network connectivity) |
| Output quantiles sized | n_tri (triangles) | n_active_nodes (excluding outfalls) |

### 4.6 The ROM ODE (full definition — deviation form)

**This subsection describes the current deviation-form implementation (reform PRs 6–7).**
The full normative derivation lives in `docs/uncertainty/DEVIATION_FORM.md`; this is the
condensed version for this guide.

The real deterministic depth/head field `h_det` is whatever SWMM's ordinary DynWave/CVODE
solver computes at each routing step — the ROM never advances `h_det` itself. Instead, each
ensemble member `i` tracks only its **modal deviation** from that live reference:

```
δa_i = P^T (h_i − h_det)         (member i's deviation, projected onto the eigenbasis)
b_j  = P[:,j]^T · h_det          (projection of the real deterministic field itself)
```

Linearising the diffusion-wave equation about `h_det` and projecting the *difference* between
member `i`'s dynamics and the deterministic dynamics onto mode `j` gives:

```
d(δa[i,j])/dt = −rate[i,j] · δa[i,j]  +  g[i,j]

rate[i,j] = λ_j · keff[i,j]                                    (mode-j decay rate for member i)

g[i,j]    = −λ_j · (keff[i,j] − keff_nominal[j]) · b_j[j]       (Manning-sensitivity forcing)
            + (f[i,j] − r_coarse[j])                            (forcing-sensitivity forcing)

r_coarse[j] = P[:,j]^T · rainfall_field                (nominal, mult = 1, forcing projection)
```

`keff[i,j]` is member `i`'s per-mode effective conductance — `K_eff / mannings_mult[i]` on the
scalar path, or the per-cell Rayleigh-quotient value on the spatial Manning path (§4.10);
`keff_nominal[j]` is the same quantity at `mannings_mult = 1`. `f[i,j]` is member `i`'s rainfall
forcing (scalar `r_coarse[j] · rainfall_mult[i]`, spatial, or ensemble-runoff — see §4.10).

**The defining property**: for the nominal member (`mannings_mult[i] = 1`,
`rainfall_mult[i] = 1`), every term in `g[i,j]` is identically zero, so `δa[i,j] ≡ 0` for all
time — that member's reconstructed depth equals `h_det` exactly, not approximately. There is
nothing to drift and therefore nothing to periodically re-anchor (see §6.3).

Each `(member, mode)` pair is still solved **exactly** via the exponential integrator — no
substep limit, no Krylov solves:

```
steady[i,j]    = g[i,j] / rate[i,j]
δa[i,j](t+dt)  = (δa[i,j](t) − steady[i,j]) · exp(−rate[i,j] · dt)  +  steady[i,j]
```

When `rate[i,j]` is near zero (near-null mode or K_eff ≈ 0), the solver falls back to Euler.
Reconstructed cell depths are `h_det[t] + Σ_j P[j,t] · δa[i,j]`, clamped to ≥ 0 (2D; the ROM
works in depth space) or to the node invert elevation (1D; the ROM works in head space).

**Connection to the CVODE Jacobian:**  
For the linearised system `∂h/∂t = K_eff · L · h`, the Jacobian is `J = K_eff · L`. Its
eigenvalues are exactly `{λ_j · K_eff}` — the same values that appear as the ROM's nominal
per-mode decay rates. The ROM basis P is not arbitrary: it diagonalises the operator that
CVODE's Newton–Krylov solver is implicitly inverting at each timestep. The k retained modes are
the k slowest-decaying directions of the linearised dynamics — the most persistent patterns in
the solution space and therefore the ones that carry the most uncertainty at the timescales of
interest. Modes with high `λ_j` decay so fast (sub-second) that they contribute negligible
uncertainty by the time the CVODE step completes; they are safely discarded.

The same formulation applies to the 1D sewer ROM (`SpectralROM1D`), substituting the network
graph Laplacian for the 2D mesh Laplacian. The 1D K_eff is computed as the diffusion-wave
diffusivity `D = h^(5/3) / (2n√S)` [m²/s] averaged over active conduits, then normalised by
conduit length squared `L²` to yield units of 1/s. This normalisation is essential: graph
Laplacian eigenvalues are dimensionless (they count topological connectivity, not spatial
scale), so `λ_j × K1d` must have units of 1/s.

**Does this work with SWMM's dynamic wave routing?**

*2D surface domain*: The CVODE solver solves the diffusion-wave equation by design — the ROM's
basis is derived from the exact same equation, so the Jacobian connection above holds exactly.
"Dynamic wave" in the 2D context does not apply.

*1D sewer network*: SWMM's `DYNWAVE` solver integrates the full **Saint-Venant equations**
(continuity + momentum). The 1D ROM propagates uncertainty using a **diffusion-wave
approximation** — the momentum equation is dropped. This approximation is valid when inertial
terms are small compared to pressure-gradient and friction forces, which is true for:
- Subcritical flow in partially-full pipes (Fr ≪ 1)
- Gradually-varying, pressure-gradient-dominated events
- Most standard urban drainage design storms

It breaks down — and the ROM spread should be treated as a qualitative indicator only — for:
- Surcharging or fully-pressurized mains (the momentum balance changes fundamentally)
- Rapidly rising hydrographs where `∂Q/∂t` is large
- Tidal or strongly backwater-controlled reaches
- Pump stations, inline storage, or sluice gates (discontinuous momentum sources)

In these regimes the ROM correctly identifies *which nodes* carry the most Manning's n
sensitivity (the spread pattern is qualitatively right) but may mis-estimate the band width
by 30–50%. See also §8, limitation 2.

**Roadmap: Jacobian-informed 1D ROM basis**

SWMM's DYNWAVE Picard iteration already assembles a linearized operator H at each Newton
step. The existing `SpectralCoarse` infrastructure extracts it:

```cpp
active_diag[ai] = max(surf_area - 0.5*dt*sumdqdh, min_surf_area)
conduit_off[ci]  = 0.5*dt * dqdh_[tile_uj_[ci]]   // H = surfaceArea/dt − dQ/dh
```

H includes inertial terms, actual pipe depths, backwater, and surcharge effects through the
live `dqdh` values — it is the true dynamic-wave Jacobian, not a diffusion-wave approximation.
Using H's eigenvectors as the ROM basis would make the 1D ROM accurate for the full
Saint-Venant regime.

`SpectralCoarse` was closed as unsafe because it *wrote corrections back* to the DYNWAVE state,
contaminating `links.flow` across timesteps. A read-only ROM sidecar would avoid this entirely
— it reads H's eigenvectors to update its own coefficients and never touches the solver state.

Three open questions before this can be implemented:

**Gap 1 — Re-eigensolving cost per step**

H changes every routing step as pipe depths change (depth changes → `dqdh` changes → H
changes). Recomputing k=20 eigenvectors of a N=10,000 node network costs approximately:
```
O(N · k · iters) ≈ 10,000 × 20 × 60 iterations ≈ 12M flops ≈ 15 ms per solve
```
For a 6-hour storm at 30-second routing intervals (720 steps), re-solving every step adds
roughly 10.8 seconds — a ~36% overhead on a 30-second run, rising to 100%+ on faster models.

The practical fixes, in order of increasing complexity:

- **Amortised re-solving**: re-compute the eigenbasis every K=10 steps. Cost drops to ~1.5 s
  for the same storm. Appropriate when H evolves slowly (steady or slowly-rising events).
- **Change-triggered re-solving**: only re-solve when a trigger fires — e.g., when a
  pipe transitions between free-surface and pressurised (`depth/diameter` crosses 0.9), or
  when `max|H_new − H_old| / max|H_old| > threshold`. Most routing steps in a typical event
  trigger nothing; re-solves cluster around peak flow and surcharge onset.
- **Krylov recycling (warm start)**: pass the previous eigenvectors as starting vectors for
  the next Lanczos run. When H changes slowly, the new eigenvectors are close to the old ones;
  Lanczos converges in ~k iterations rather than ~3k. Cost estimate: 20 × 10,000 = 200k flops
  ≈ 0.25 ms per step — effectively negligible. This is the most promising approach and reuses
  the existing `GraphEigenBasis` infrastructure.

**Gap 2 — Time-varying basis: re-projecting the ensemble state**

This is the most subtle gap. At step t, each ROM member's head distribution is encoded as:
```
h_i(t) ≈ Σ_j P_t[:,j] · a_t[i,j]
```
where `P_t` is the eigenbasis computed from `H_t`. At step t+1, H changes and we compute a
new eigenbasis `P_{t+1}`. The old modal coordinates `a_t[i,j]` are now expressed on the wrong
basis. Before calling `advance()` with `P_{t+1}`, the ensemble state must be re-projected:
```
a_{t+1}[i,j] = Σ_k  (P_{t+1}[:,j]ᵀ · P_t[:,k])  · a_t[i,k]
             = Σ_k  R[j,k]  · a_t[i,k]

where  R = P_{t+1}ᵀ · P_t  is a k×k "rotation matrix" between the two bases.
```
Cost: forming R costs O(N·k²). At N=10,000 and k=20: 10,000 × 400 = 4M flops ≈ 4 ms.
For 720 steps at every step: ~2.9 s added per simulation run.

Two practical mitigations:

- **Skip when the basis is nearly the same**: check `‖R − I‖_F < tol` (cheap: O(k²)). If
  the rotation is small, the re-projection error is also small; skip it for that step. In
  practice, this fires for most steps during steady or slowly-varying flow.
- **Sign alignment**: Lanczos eigenvectors have arbitrary sign per run; `P_{t+1}[:,j]` may
  point in the opposite direction from `P_t[:,j]`. Before computing R, align signs via
  `sign(P_{t+1}[:,j]ᵀ · P_t[:,j])` — otherwise the rotation matrix contains −1 diagonal
  entries that invert modal coordinates spuriously.

**Gap 3 — Non-symmetry of H**

The Picard Jacobian is `H = surfaceArea/dt − dQ/dh`. In the linearised Saint-Venant equations,
`dQ/dh` at a conduit endpoint depends on the local velocity and depth:
```
dQ_ij/dh_i ≠ dQ_ij/dh_j   (in general, when Q ≠ 0)
```
The asymmetry comes from the convective acceleration term `∂(Q²/A)/∂x = (2Q/A)·∂Q/∂x`.
When flow velocity is significant, the upstream endpoint has higher sensitivity to a head
perturbation than the downstream endpoint — H is non-symmetric by O(Fr) × magnitude.

Standard Lanczos requires a symmetric matrix to guarantee real eigenvalues and a stable
tridiagonal recursion. For non-symmetric H, the correct algorithm is Arnoldi iteration
(ARPACK-style), which produces a Hessenberg matrix rather than tridiagonal — more memory and
~2× more flops per step, with less favourable numerical stability.

The practical solution for the urban drainage case is to **symmetrize H**:
```
H_sym = (H + H^T) / 2
```
The antisymmetric part `H_skew = (H − H^T) / 2` is the component that breaks Lanczos. Its
magnitude scales as O(Fr²) relative to the symmetric part (pressure gradient + friction
dominate at low Froude numbers). For subcritical urban drainage (Fr < 0.5 in most pipes):
```
‖H_skew‖ / ‖H_sym‖ ≈ Fr² < 0.25
```
Discarding `H_skew` and running Lanczos on `H_sym` reuses the existing `GraphEigenBasis`
solver with no algorithmic changes, and gives an eigenbasis that is accurate to O(Fr²) in the
dynamic wave regime — substantially better than the pure diffusion-wave Laplacian, which is
only accurate to O(Fr⁰) (it completely drops the inertial terms regardless of Froude number).
For surcharging pipes (Fr → 0 as pipes fill, since wave speed dominates velocity), `H_sym` is
effectively the exact Jacobian.

This enhancement would remove limitation 2 from §8 entirely for the 1D domain.

### 4.7 Latin-hypercube design

`UncertaintyEnsemble` generates four decorrelated LHS columns, each stratifying its parameter
uniformly across M strata (midpoint rule):

| Column | Ordering | Range | Correlation with Manning |
|---|---|---|---|
| `mannings_mult` | ascending | [1−p_n, 1+p_n] | — |
| `rainfall_mult` | descending | [1−p_r, 1+p_r] | near-zero (reversed order) |
| `soil_mult` | Fisher-Yates shuffle of strata | [1−p_s, 1+p_s] | < 0.05 (seed+2) |
| `cd_mult` | Fisher-Yates shuffle of strata | [1−p_cd, 1+p_cd] | < 0.05 (seed+3) |

The reversed-order trick for rainfall gives perfect anti-correlation between Manning and rainfall
columns within each realisation — members with high Manning's n (slow conveyance) get low
rainfall multipliers, which is physically conservative and prevents artificial spread inflation.

### 4.8 AUTO K_eff

At the first CVODE advance (and at each reseed), K_eff is estimated from the current wet state:

```
K_eff = h_mean^(5/3) / (2 · n_mean · √S_mean)
```

- `h_mean` — mean depth over cells with depth > 1e-6 m
- `n_mean` — area-weighted mean Manning's n over wet cells
- `S_mean` — mean bed slope magnitude (floor: 1e-6, prevents divide-by-zero on flat domains)

**Option A** (per-mode Rayleigh quotient): when the advance call receives the current depth
array, each mode j gets its own K_eff weighted by the depth distribution:

```
K_eff_j = K_eff · Σ_t φ_j[t]² · (h[t]/h̄)^(5/3)
```

Modes concentrated in deep cells decay faster. This is enabled automatically when h_cell is
passed to `advance()` (which `CvodeSurfaceSolver` does by default).

### 4.9 Quantile computation

After each `advance()` call, `computeQuantiles()` reconstructs per-cell depths for all M members
and computes the three percentiles by sorting:

```
h_i[t] = max(0,  Σ_j P[t,j] · a_{i,j})      for member i, cell t
```

The three output fields are the sorted 5th, 50th, and 95th percentile of `{h_i[t] : i=0..M-1}`
at each cell `t` independently.

With `PARAMETRIC_TAILS YES`: for cells where ≥ 4 members are wet, fits a log-normal to the
wet sub-population and replaces the sort-based q95 with the analytic 95th percentile of that
fit. This suppresses noise from the top 1–2 samples when M is small (< 30).

### 4.10 Spatial uncertainty fields

When `MANNINGS_CORR_LEN > 0` or `RAINFALL_CORR_LEN > 0`, each member gets a spatially-varying
multiplier field generated by `CorrelatedFieldGenerator`:

1. Draw i.i.d. N(0,1) samples at each triangle centroid.
2. Smooth with an exponential kernel: `Z_smooth[t] = Σ_{t'} exp(−d(t,t')/L) · Z[t']`
   (summed over neighbors within `3·L` metres of centroid `t`).
3. Centre the field: `Z_centred[t] = Z_smooth[t] − mean(Z_smooth)`.
4. Combine with the member's global LHS level: `W[i][t] = global_level[i] + pert · Z_centred[i][t]`

As `corr_len → ∞`, spatial variation collapses toward a uniform field (scalar limit).
When `corr_len = 0`, the spatial generator is bypassed for speed.

**In `advance()`**: `f_j^i = P[:,j]ᵀ · (rainfall ⊙ W_rain[i])` replaces the scalar
`r_coarse[j] · rainfall_mult[i]` when spatial rainfall is active.

### 4.11 Coupling uncertainty

When both a 2D ROM and a 1D ROM are active, the coupling exchange operates on the full
ensemble rather than on a single deterministic pair of heads. The sequence at each SWMM
routing step is:

**Step 1 — deterministic exchange (shared heads)**

`computeCouplingExchange()` computes a single inflow/outflow at each coupling point using the
current CVODE surface depth and the DYNWAVE node head. This updates `ctx_.nodes.lat_flow[]`
and drives the deterministic 1D DYNWAVE solve — exactly as in a run with no ROM.

**Step 2 — per-member ROM exchange (difference form)**

`applyCouplingFluxToROM()` re-runs the orifice equation independently for all M ensemble
members at each non-outfall coupling point, then applies only each member's *deviation* from
the deterministic flux — the deterministic exchange itself is already inside `h_det` via
Step 1, so applying the absolute per-member flux would double-count it:

- **2D depth per member**: reconstructed from the 2D ROM deviation:
  `h_2d[i][ci] = max(0, h_det[ci] + Σ_j P[ci,j] · δa[i,j])`
- **1D head per member**: if the 1D ROM is registered,
  `h_1d[i] = rom1d->reconstructHead(i, full_to_active[cp.node_idx])`; for outfall nodes
  (`full_to_active == −1`) the shared deterministic head is used as a fallback.
- **Deterministic reference flow**: `Q_det = Cd · A · sign(dh_det) · √(2g|dh_det|)`, computed
  once from `h_det` and the shared deterministic 1D head — nominal `Cd` (no `cd_mult`).
- **Orifice flow per member**:
  `Q_i = Cd · cd_mult[i] · A · sign(h_2d[i] − h_1d[i]) · √(2g|h_2d[i] − h_1d[i]|)`, capped by
  available depth in the 2D cell. Orifice discharge does **not** depend on Manning's n (that
  governs surface conveyance, not the inlet) — per-member spread here comes from `cd_mult`
  (§4.7), not from `mannings_mult`.
- **2D ROM deviation update** (for each retained mode j): only the difference is applied —
  `δa[i,j] += P[j,ci] · (−(Q_i − Q_det) · dt / tri_area)`

**Step 3 — independent ROM advances**

After coupling, `SpectralROM::advance()` and `SpectralROM1D::advance()` run independently for
the remainder of the routing step using their respective K_eff and K1d. The coupling
deviation injected in Step 2 is a one-shot impulse on the 2D ROM's modal deviations; the 2D
ROM then evolves freely until the next routing step.

**What the 1D ROM is not updated by:**

The 1D ROM coefficients are not modified by `applyCouplingFluxToROM()`. The 1D ROM sees the
coupling only indirectly — through the lateral flow term in DYNWAVE, which updates
`ctx_.nodes.head[]`, which in turn affects K1d and the initial seed state. This is an
operator-splitting approximation: the 2D ensemble absorbs per-member coupling uncertainty;
the 1D ensemble evolves under its own Manning uncertainty and contributes per-member 1D heads
to the coupling exchange in Step 2.

The per-coupling-point bounds `[q_min, q_max]` across all M members are accumulated in
`CouplingUncertaintyOutput` and available via `SurfaceRouter2D::couplingOutput()`.

---

## 5. How to interpret outputs

### 5.1 What q50, q05, q95 mean

- **q50** is the median of the ensemble — the best single-number estimate when Manning's n is
  uncertain. For small perturbations (≤ 20%) it is close to, but not identical to, the
  deterministic CVODE result (small systematic bias from the linearisation).
- **[q05, q95]** is a 90-percentile prediction interval. It answers: "if the true Manning's n
  and rainfall are anywhere within the specified ±p% range, what is the range of outcomes?"
- **q95 − q05** (the spread) quantifies where uncertainty matters most. Large spread = high
  sensitivity to the uncertain parameters at that cell and time.

### 5.2 Typical spatial patterns

| Observation | Likely cause |
|---|---|
| Spread peaks at inlet/outlet junctions | These are hydraulic bottlenecks where Manning's n most strongly controls conveyance. The Fiedler gradient (§7.4) confirms this. |
| Spread grows downstream along a pipe | Manning's n uncertainty compounds over flow path length — each cell passes its uncertainty to the next. |
| Spread is near zero early in the event, grows later | Deviation-form spread starts at exactly zero by construction and grows toward its parametric steady state with per-mode time constants `1/(λ_j·K1d)`. This is spin-up, not an error — see §8 limitation 1. |
| Spread is near zero everywhere despite non-zero perturbation | On a network with **no grounded boundary at all** (no outfall / open edge), the constant mode is unrepresentable and a spatially uniform field projects to nothing. With a normal outfall this no longer applies — the basis is grounded there and uniform inputs do produce spread. Check the model actually has an outfall; otherwise use a non-uniform IC. |
| High relative spread in dry cells | Relative spread = (q95−q05)/q50 blows up as q50 → 0. Use **absolute** spread (q95−q05 in metres) for dry-cell analysis. |

### 5.3 Choosing perturbation levels

The perturbation `p` represents the half-range of the assumed uniform prior for the parameter
multiplier. Physically:

- **Manning's n ± 20%**: matches typical calibration uncertainty in urban drainage (USEPA 1992
  SWMM manual reports ±15–25% for concrete and grass-lined channels). Use 0.15 for well-calibrated
  systems, 0.25–0.30 for uncalibrated design-phase models.
- **Rainfall ± 10%**: captures radar QPE error at typical urban scales. Use 0.15–0.20 for
  long-duration events where spatial variability is significant.
- **Soil Ks ± 25%**: typical coefficient of variation for saturated conductivity from point
  measurements (Rawls & Brakensiek 1989).

The output band width is approximately linear in `p` for small perturbations:
```
q95 − q05 ≈ 2 · p · |∂(depth)/∂(Manning's n)| · n_base
```

### 5.4 Ensemble size guidance

| M | Use case |
|---|---|
| 10–20 | Rapid screening runs, qualitative band shape only. q05/q95 noisy. |
| 50 | Default. Acceptable quantile accuracy (± ~5% of spread). |
| 100 | Required if using `PARAMETRIC_TAILS NO` and need reliable q95 tail. |
| 200+ | Diminishing returns; use if publishing or for regulatory submissions. |

Cost scales exactly as O(M). Doubling M doubles ROM overhead (which is already ~1% of total
simulation time), so M=100 adds roughly 2% overhead relative to M=0.

### 5.5 Modes guidance

| k | Effect |
|---|---|
| 5 | Only the coarsest spatial uncertainty patterns captured. Fast, good for large meshes. |
| 10 | Default. Captures spatial features down to ~3× the mean triangle edge length. |
| 20–30 | Fine-grained spatial uncertainty; diminishing returns for scalar Manning perturbation. More useful when `MANNINGS_CORR_LEN` is small (spatially heterogeneous uncertainty). |

The ROM cannot represent spread at spatial scales finer than the kth eigenmode. If the mesh has
10,000 triangles and k=10, the ROM can resolve spread patterns at scales of roughly
`domain_size / sqrt(k)` — about 30% of the domain width for a square domain.

### 5.6 Coupling uncertainty interpretation

`SurfaceRouter2D::couplingOutput()` returns per-coupling-point bounds `{q_min, q_max}` (m³/s).
These represent the range of inlet drainage flows across the ensemble:

- `q_max − q_min` large → the inlet flow is sensitive to discharge-coefficient (`cd_pert`)
  uncertainty and/or per-member 2D depth or 1D head spread. Orifice discharge does not depend
  on Manning's n (§4.11), so Manning perturbation alone produces no coupling-flux spread.
- Use the Fiedler gradient at the coupling cell (§7.4) to confirm whether the inlet lies in a
  high-connectivity region of the mesh

### 5.7 Water-quality uncertainty output

When a `QUALITY` layer entry is present in `[UNCERTAINTY]`, the engine produces a separate
CSV file alongside the report file:

```
<report_path>.wq_uncertainty.csv
```

The file is created by replacing the `.rpt` suffix of the report path with
`.wq_uncertainty.csv` (or appending if no `.rpt` suffix). Schema:

```
time_s,node_name,pollutant_index,q05,q50,q95
```

**What the columns mean:**

| Column | Description |
|---|---|
| `time_s` | Simulation time in seconds at the report boundary |
| `node_name` | SWMM node name |
| `pollutant_index` | 0-based index into the `[POLLUTANTS]` list |
| `q05` | 5th-percentile concentration (mass/volume) |
| `q50` | Median concentration — ≈ the deterministic value |
| `q95` | 95th-percentile concentration |

**What the bounds represent:** the spread in concentration attributable to ±`pert`
uncertainty in the first-order decay rate `k` **over the most recent report interval only**.
The bounds are analytic: `c₀ · exp(−k · kmᵢ · dt)` where `c₀` is the deterministic
concentration at the start of the report interval, `kmᵢ` are M ascending uniform
multipliers spanning `[1−p, 1+p]`, and `dt` is the report step in days.

**Key properties:**
- `q50 ≈ c_det` (the deterministic concentration at the report time) — the band is
  centred on the deterministic value.
- `q05 ≤ q50 ≤ q95` always holds (ascending strata).
- When `k_decay = 0` (no decay), `exp(0) = 1` for all members, so `q05 = q50 = q95`
  (zero-width band — no decay-rate uncertainty).
- Band width grows monotonically with `pert` and with `k · dt`.

> **Limitation (v1):** the bounds are **per-report-interval**, not cumulative since
> pollutant injection. Quality routing does not carry parcel age, so a fully
> cumulative band is not yet available. See §8, limitation 8.

### 5.8 Band-trust diagnostic (`fr_trust` / `surcharge_frac`)

Whenever the 1D ROM is active, the engine writes a second sidecar alongside
the report file:

```
<report_path>.rom_diag.csv
```

The file is created the same way as `.uncertainty.csv` (replacing the `.rpt`
suffix, or appending if there isn't one). Schema:

```
time_s,fr_trust,surcharge_frac,basis_age_s,basis_rebuilds,cold_restarts,cache_hits
```

**What the columns mean:**

| Column | Description |
|---|---|
| `time_s` | Simulation time in seconds at the report boundary |
| `fr_trust` | Flow-weighted mean squared Froude number over the network |
| `surcharge_frac` | Fraction of active nodes currently surcharged |
| `basis_age_s`, `basis_rebuilds`, `cold_restarts`, `cache_hits` | Reserved; always `0` until the H1 (cold-restart) and H2 (basis cache) features land |

**Why this file exists, separately from `.uncertainty.csv`:** the ROM's
weighted-Laplacian operator is built from the Picard solver's own dQ/dH
values, applied symmetrically to each conduit's two endpoints — so the
operator itself is exactly symmetric, with no directional bias to detect
from the operator alone. The real Saint-Venant Jacobian isn't symmetric once
advective (inertial) terms matter, and that gap grows with the square of the
Froude number. `fr_trust` is the honest, computable stand-in for "how much
does that gap matter right now" — it isn't (and can't be) zero/one certainty,
just a flow-weighted summary of how fast the network is moving.

**How to read `fr_trust`:**

| Range | Interpretation |
|---|---|
| `≲ 0.05` | Bands are trustworthy as reported |
| `0.05 – 0.15` | Mentally inflate reported widths by roughly 10% |
| `≳ 0.25` | Peak-flow regime; band widths may be off by 10–25% (the roadmap's known limitation, now with a number attached) |

`surcharge_frac` is the complementary signal, not a second trust concern in
the same direction: once a reach surcharges, velocities collapse toward zero
and `fr_trust` naturally falls with them — a high `surcharge_frac` alongside
a low `fr_trust` is the ROM's linear approximation becoming *more* valid, not
less (Fr → 0 makes the symmetric surrogate nearly exact). See §8 for the
underlying limitation this diagnostic reports on.

### 5.9 Threshold-crossing probability and the bimodality flag

Whenever the 1D ROM is active the engine writes a companion file alongside the
report:

```
<report_path>.rom_threshold.csv
```

Schema:

```
time_s,node_name,threshold_kind,threshold_value,p_exceed,p_ctrl,modality_flag
```

| Column | Description |
|---|---|
| `time_s` | Simulation time in seconds at the report boundary |
| `node_name` | SWMM node name |
| `threshold_kind` | `CROWN` (surcharge onset) or `MAX_DEPTH` (flooding) — which threshold this row measured against |
| `threshold_value` | The crossing level, as an absolute head in internal units |
| `p_exceed` | Fraction of ensemble members strictly above `threshold_value` |
| `p_ctrl` | Fraction of members above this node's `[CONTROLS]` setpoint, if it is named as a sensor; `0` otherwise |
| `modality_flag` | `1` when the ensemble looks bimodal at this node (see below), else `0` |

**Why this file exists.** The decision-relevant question at a crown or a control
setpoint is almost never "what is the 95th-percentile depth" — it is *what
fraction of the ensemble crosses this line*. That number was always available
from the member values the quantile sort already reconstructs; it simply was
never reported. Reading `p_exceed = 0.30` at a crown is a direct statement that
roughly 30% of the plausible parameter space surcharges this node.

**Threshold priority.** The crown is used when the node has one (it is the
earlier, more actionable event); `MaxDepth` is the fallback for nodes with no
connecting conduit crown. Nodes with neither emit no row at all rather than a
misleading zero.

**`p_ctrl` is reporting only.** It tells you how close the ensemble is to
disagreeing about a control rule — it does **not** re-evaluate or re-time
control actions per member. The deterministic run's control decisions are
unchanged. A `p_ctrl` near 0.5 means the members genuinely straddle the
setpoint, and the single deterministic answer is a coin flip in disguise.

**`modality_flag` — why a band can be the wrong summary.** When members split
across a threshold the ensemble becomes bimodal, and describing it with three
order statistics is actively misleading: the median sits between the two
clusters and is reported as "the central estimate" while a large share of the
ensemble is nowhere near it. The flag is a cheap gap statistic (largest gap
between consecutive sorted members ÷ interquartile range, plus a requirement
that at least 20% of members fall on each side, so a lone outlier does not
count as a second mode) — not a mixture fit, which would be overkill at
M ≈ 50. **When this flag is set, stop reading `q05/q50/q95` as a band** and
read `p_exceed` instead: the honest answer is a branch probability, not an
interval.

This is the sidecar's answer to the objection that a linear ROM cannot
represent branching behaviour. It cannot — so it reports the probability of
the branch rather than pretending to a single smooth band.

### 5.10 Front-passage timing (per-member phase coordinate)

Everything described in §4 is an *amplitude* method: each ensemble member
carries a deviation from the deterministic run's trajectory, evolved by a
decaying operator. That representation cannot capture a **timing** error — a
filling front arriving a few minutes earlier or later depending on Manning's
n is a phase shift, not an amplitude one, and prior validation runs measured
a real, unrepresented 2–4 m transient width right at front passage as a
result (see `docs/uncertainty/VALIDATION.md`, "Per-member phase coordinate
(PR H11)").

**What changed, and what you'll notice.** The 1D ROM now automatically
computes each member's own conveyance-driven timing offset
(`τ_i = (mm_i − 1)·T̄(x)`, where `T̄(x)` is the deterministic travel time from
the source and `mm_i` is that member's Manning multiplier — a slower member
sees the same shape it always would, just a bit later) and reconstructs its
band accordingly. This is **on by default and requires no new `.inp` keys** —
q05/q50/q95 simply become noticeably wider during a genuine wetting front and
identical to before everywhere else:

- **Steady-state / saturated regions**: no visible change. The mechanism adds
  exactly zero width once `h_det` stops moving — the median still tracks the
  deterministic run exactly (`mm_i = 1` always gives zero timing offset,
  regardless of how large the travel-time field is).
- **A node whose flow is essentially stagnant** (very low velocity — e.g. a
  ponded or backwater-controlled reach): also unaffected. A conveyance-driven
  timing offset only makes sense where there is a travelling signal to be
  early or late about; a stagnant reach's uncertainty is governed entirely by
  the amplitude channel (and, if surcharged, §4's attenuation), same as
  before.
- **A genuine filling front** (dry-to-wet transition, storm onset on a long
  reach): the band widens noticeably right around the transition and narrows
  again once the network settles — this is the mechanism working as intended,
  not noise.

**Nothing to configure.** Like §4's surcharge attenuation, the underlying
physical dials (conveyance-velocity floor, per-edge/whole-path travel-time
caps) are internal defaults, not `.inp`-exposed — this is deliberately the
same "ship a sane default, no new parser surface" choice §4 made.

**Scope**: 1D only. The 2D counterpart (the same timing gap shows up as the
drain-to-pond limitation in item 4 below) is not yet addressed.

---

## 6. Advanced configuration

### 6.1 Spatial Manning's n

Use a spatially-varying Manning's n field when the domain has heterogeneous roughness that you
expect to be uncertain at spatial scales comparable to the correlation length (e.g., patchy
vegetation or variable pavement condition):

```ini
[2D_ROM]
ENABLE              YES
MEMBERS             50
MODES               20
MANNINGS_PERT       0.20
MANNINGS_CORR_LEN   30.0    ; 30 m correlation length
```

The generated field has mean 1.0 and standard deviation ≈ `MANNINGS_PERT` (after normalisation).
The correlation length should match your best estimate of the spatial coherence of roughness
variation (e.g., land-use patch sizes, paving unit widths).

### 6.2 Auto vs explicit K_eff

`K_EFF AUTO` (the default, triggered by any value ≤ 0) is appropriate for most runs. Set it
explicitly if:
- The mesh is partially wet at start and the automatic estimate is computed over a non-
  representative wet region.
- You want bit-reproducible results independent of the initial depth distribution.
- The AUTO estimate produces unreasonably large spread (K_eff too large → fast decay → spread
  underestimated; K_eff too small → slow decay → spread overestimated).

To diagnose: enable verbose mode and check the logged K_eff at first seed. Then set it
explicitly if needed:

```ini
[2D_ROM]
K_EFF    8.5    ; m^(4/3)/s — set from logged AUTO value
```

### 6.3 Reseeding (2D only — basis coverage, not drift correction)

Under the deviation formulation, the ROM's median always tracks the live deterministic depth
field (§4.6) — there is no periodic drift to correct, and the 1D ROM has no reseed mechanism
at all. The one remaining reseed trigger is 2D-specific and purely about **basis coverage**:
the ROM's eigenbasis is built over the wet region at seed time, so as the wetting front
advances into previously-dry cells, the basis needs rebuilding to represent them.
`WET_RESEED_FRACTION` controls how aggressively this rebuild fires:

```ini
[2D_ROM]
WET_RESEED_FRACTION       0.02   ; rebuild when wet area changes by 2% of domain
WET_RESEED_MIN_INTERVAL   30.0   ; at most once every 30 s
```

After each rebuild, every member's deviation resets to zero (all members restart exactly on
the deterministic solution) and spread re-grows from there — but the median is unaffected,
since it already equalled the deterministic solution before and after the rebuild. If your
output frequency is coarser than the rebuild interval, the brief spread reset is not visible
in the output.

### 6.4 Parametric tails

With small M (< 30) the sort-based q95 is noisy because only 1–2 samples determine the top
percentile. `PARAMETRIC_TAILS YES` fits a log-normal to the wet-cell population and uses the
analytic 95th percentile:

```ini
[2D_ROM]
MEMBERS           20
PARAMETRIC_TAILS  YES
```

The log-normal fit uses method of moments on the log-transformed wet depths. It may
overestimate q95 if the true distribution has a lighter tail than log-normal — always verify
against a high-M run before relying on parametric tails for design decisions.

---

## 7. Implementation inventory

This section describes exactly what is implemented in the codebase as of 2026-05-25.

### 7.1 2D Spectral ROM (SpectralROM) — COMPLETE

**Files**: `src/engine/2d/uncertainty/SpectralROM.hpp/cpp`  
**Tests**: `tests/unit/engine/test_2d_spectral_rom.cpp` (47/47 pass)

The core ROM struct. Methods:

| Method | What it does |
|---|---|
| `initialize()` | Allocates `a_ensemble[M×k]` (modal deviations), builds LHS design (or uses external samples). |
| `seed(h_full)` | Zeroes every member's modal deviation and stores `h_full` as the deterministic reference `h_det_last_`. |
| `advance(dt, K_eff, rainfall, h_cell, h_det)` | Advances all members' deviations via exact exponential integrator, using the live deterministic depth `h_det` for the Manning-sensitivity term. Optionally uses per-mode Rayleigh K_eff via `h_cell`. |
| `computeQuantiles(h_det, parametric_tails)` | Reconstructs per-cell depths as `h_det + P·δa`, sorts, fills q05/q50/q95. |
| `applyCouplingFlux(cps, heads, mesh, dt, rom1d)` | Applies only each member's *deviation* from the deterministic orifice flux at each coupling point (§4.11); updates ROM deviations; fills `coupling_unc_output`. |
| `setEnsembleRainfall(rates)` | Replaces scalar rainfall_mult path with per-member runoff rates from RunoffEnsemble. |
| `setExternalSamples(mann, rain)` | Injects UncertaintyEnsemble's LHS columns instead of building internal ones. |
| `setCdSamples(cd)` | Injects discharge-coefficient LHS column. |
| `is_ready()` | True after initialize() succeeds with a valid basis. |

**Integration**: `CvodeSurfaceSolver` owns a `SpectralROM` instance, seeds it on first
advance, calls `advance()` + `computeQuantiles()` after each successful CVODE step, and calls
`applyCouplingFluxToROM()` after `computeCouplingExchange()`.

`SurfaceRouter2D::rom()` exposes a const pointer; `SurfaceRouter2D::couplingOutput()` exposes
the per-coupling-point bounds.

### 7.2 UncertaintyEnsemble — COMPLETE

**Files**: `src/engine/uncertainty/UncertaintyEnsemble.hpp/cpp`  
**Tests**: via spectral ROM tests

The single LHS owner. Call `generate()` once; then pass column views to consumers:

```cpp
UncertaintyEnsemble ens;
ens.n_members       = 50;
ens.mannings_pert_2d = 0.20;
ens.rainfall_pert_2d = 0.10;
ens.soil_pert        = 0.25;
ens.cd_pert          = 0.10;
ens.generate();

rom.setExternalSamples(ens.manningsSamples2D(), ens.rainfallSamples2D());
rom.setCdSamples(ens.cdSamples());
```

Four decorrelated LHS columns: Manning (ascending), rainfall (descending), soil
(Fisher-Yates shuffled), Cd (independently shuffled).

### 7.3 Spatial fields (CorrelatedFieldGenerator) — COMPLETE

**Files**: `src/engine/2d/uncertainty/CorrelatedFieldGenerator.hpp/cpp`,
`src/engine/2d/uncertainty/SpatialUncertaintyField.hpp`  
**Tests**: `tests/unit/engine/test_engine_2d_spatial_field.cpp` (13/13 pass)

Generates M×n_tri fields with exponential spatial correlation. Uses a grid-indexed
neighbourhood structure for O(n_tri × avg_neighbours) preprocessing, with `3·corr_len`
search radius. Fields are stored in `SpectralROM::spatial_mannings` and
`SpectralROM::spatial_rainfall`.

### 7.4 Fiedler diagnostic (2D + 1D) — COMPLETE

**Files**: `src/engine/2d/uncertainty/FiedlerDiagnostic.hpp` (header-only),
`src/engine/uncertainty/FiedlerDiagnostic1D.hpp` (header-only)  
**Tests**: 7 tests in `test_2d_spectral_rom` (FiedlerDiagnostic suite),
13 tests in `test_engine_spectral_rom1d` (FiedlerDiagnostic1D suite)

The Fiedler vector (second eigenvector of the Laplacian, or first retained mode after null
filtering) identifies bottleneck cells/nodes: those where a small perturbation in connectivity
most strongly divides the network into two sub-domains. Large Fiedler gradient = high
coupling sensitivity.

**2D usage:**
```cpp
#include "2d/uncertainty/FiedlerDiagnostic.hpp"
FiedlerDiagnostic fd_2d(precond_2d);    // uses P[:,0] (null already filtered)
// fd_2d.grad[t] = max |φ₂[t]−φ₂[t']| / dist over face-sharing neighbours of t
// fd_2d.rank[0] = index of cell with highest gradient (bottleneck)
double sensitivity = fd_2d.grad[coupling_point.cell_idx];
```

**1D usage:**
```cpp
#include "uncertainty/FiedlerDiagnostic1D.hpp"
FiedlerDiagnostic1D fd_1d(graph_eigen_basis);
// fd_1d.gradAtFullNode(node_idx, rom1d.full_to_active) → gradient at SWMM node
double sensitivity = fd_1d.gradAtFullNode(cp.node_idx, rom1d.full_to_active);
```

Cells/nodes with high Fiedler gradient are where coupling uncertainty (§5.6) is most
consequential: small changes in Manning's n there produce large changes in flow partition.

### 7.5 Runoff ensemble (RunoffEnsemble) — COMPLETE

**Files**: `src/engine/uncertainty/RunoffEnsemble.hpp/cpp`,
`src/engine/uncertainty/SoilParameterLHS.hpp`  
**Tests**: `tests/unit/engine/test_runoff_ensemble.cpp` (13/13 pass)

Runs M infiltration trajectories in parallel with independent soil multipliers from
`UncertaintyEnsemble::soilSamples()`:

| Infiltration model | Perturbed parameter |
|---|---|
| Green-Ampt | `Ks_i = Ks_base × soil_mult[i]` |
| Horton | `f0_i = f0_base × mult[i]`,  `fmin_i = fmin_base × mult[i]` |
| Curve Number | `S_i = S_base / soil_mult[i]` (higher mult → more infiltration → less runoff) |

The per-member surface runoff rates can be fed to `SpectralROM::setEnsembleRainfall()` to
propagate infiltration uncertainty through the 2D surface routing.

### 7.6 WQ uncertainty bounds (WQUncertaintyBounds) — COMPLETE

**File**: `src/engine/uncertainty/WQUncertaintyBounds.hpp` (header-only)  
**Tests**: in `test_runoff_ensemble.cpp`

Analytical bounds for first-order constituent decay `c(t) = c₀ exp(−k·t)` with uncertain k:

```
q05_conc = c₀ exp(−k·(1+p)·t)    (highest k → most decay → lowest concentration)
q95_conc = c₀ exp(−k·(1−p)·t)
```

No ODE solve needed. The `p` parameter is taken from `soil_pert` (as a proxy for rate
constant uncertainty).

### 7.7 1D Spectral ROM (SpectralROM1D + GraphEigenBasis) — COMPLETE

**Files**: `src/engine/uncertainty/GraphEigenBasis.hpp/cpp`,
`src/engine/uncertainty/NetworkLaplacian1D.hpp`,
`src/engine/uncertainty/SpectralROM1D.hpp/cpp`  
**Engine hooks**: `SWMMEngine::buildROM1D()`, `SWMMEngine::computeK1d()` in
`src/engine/core/SWMMEngine.cpp`  
**Tests**: `tests/unit/engine/test_engine_spectral_rom1d.cpp` (39/39) +
`tests/unit/engine/test_engine_1d_rom_integration.cpp` (5/5)

A 1D network spectral ROM mirroring the 2D design:

- `GraphEigenBasis` — Lanczos+QL eigensolver for arbitrary CSR-format graphs. Requires
  n_active_nodes ≥ 4. The first retained mode (after null mode filtering) is the Fiedler mode.
- `NetworkLaplacian1D` — builds the graph Laplacian from conduit-to-node connectivity.
  Outfall nodes are excluded from the active set.
- `SpectralROM1D` — seed/advance/computeQuantiles on the 1D network. Exposes
  `full_to_active[]` for mapping SWMM node indices to active ROM indices, and
  `reconstructHead(member, active_node)` for per-member head at a node (used in 2D coupling).
- `SWMMEngine::buildROM1D()` — called in `initialize()` when any active 1D uncertainty source
  is configured, or when the 2D ROM is enabled. Builds the Laplacian from `ctx_.links` conduit
  pairs, runs the eigensolver, seeds from current node heads, and registers with
  `surface_router_.setROM1D()`.
- `SWMMEngine::computeK1d()` — diffusion-wave diffusivity
  `D = h^(5/3) / (2n√S)` [m²/s], averaged over active conduits and normalised by mean
  conduit length squared `L²` to yield 1/s for the dimensionless graph Laplacian eigenvalues.

**Accessing 1D quantiles from C++:**

```cpp
// After swmm_engine_step() loop:
auto* eng = static_cast<openswmm::SWMMEngine*>(handle);
const auto* rom1d = eng->rom1d();
if (rom1d && rom1d->is_ready()) {
    // rom1d->q05 / q50 / q95 — length n_active_nodes each
    // rom1d->full_to_active[swmm_node_idx] — -1 if outfall
    for (int ai = 0; ai < rom1d->n_nodes; ++ai) {
        double spread = rom1d->q95[ai] - rom1d->q05[ai];
        // ...
    }
}
```

**Note — 1D ROM output not in HDF5.** The 1D quantiles are currently only accessible
via the C++ cast API above. Writing them to the binary output file is deferred (no
parser key or plugin hook exists yet).

### 7.8 2D↔1D Coupling ROM path — COMPLETE

**File**: `src/engine/2d/uncertainty/SpectralROM.cpp` — `applyCouplingFlux()`

When `SpectralROM1D` is registered via `SurfaceRouter2D::setROM1D()`, the coupling flux
computes per-member 1D head via `rom1d->reconstructHead(member, active_idx)` instead of using
the single shared deterministic head. This gives each ensemble member a physically consistent
pair of (2D depth, 1D head) for the orifice equation.

### 7.9 Regression and integration tests — COMPLETE

| Test | Description | Count |
|---|---|---|
| `test_engine_2d_spectral_rom` | ROM unit tests (all paths) | 47/47 |
| `test_engine_spectral_rom1d` | 1D ROM + GraphEigenBasis + FiedlerDiagnostic1D | 39/39 |
| `test_engine_2d_spatial_field` | CorrelatedFieldGenerator | 13/13 |
| `test_engine_runoff_ensemble` | RunoffEnsemble + WQ bounds | 13/13 |
| `test_engine_regression` | Cross-engine + self-consistency (Example1–3) | 6/6 |
| `test_engine_2d_rom_integration` | Full engine lifecycle with 2D ROM | 1/1 |
| `test_engine_1d_rom_integration` | Full engine lifecycle with 1D ROM | 5/5 |

### 7.10 HDF5 output

`Default2DOutputPlugin` (`src/engine/2d/output/Default2DOutputPlugin.cpp`) writes
CF-1.11/UGRID-1.0 HDF5 output when `[2D_OPTIONS] OUTPUT_FILE <path>` is specified
in the `.inp` file. The plugin is injected automatically by the engine — no
`[PLUGINS]` entry is required.

**Activation**: add `OUTPUT_FILE <path>` to `[2D_OPTIONS]`:

```ini
[2D_OPTIONS]
OUTPUT_FILE  results.h5
```

The path is resolved relative to the `.inp` file directory if relative. The
plugin creates the file at `prepare()` time (truncating any existing file),
writes static mesh topology in `prepareMeshAndDatasets()` (called from
`start()` after the mesh is built), and appends one time step per report
interval via `update()` on the IO thread.

**2D ROM quantile datasets** (written when the 2D ROM is active):

```
/Mesh2_face_depth_q05    [nTime, nFace]   — 5th-percentile depth (m)
/Mesh2_face_depth_q50    [nTime, nFace]   — Median depth (m)
/Mesh2_face_depth_q95    [nTime, nFace]   — 95th-percentile depth (m)
```

**1D ROM quantile datasets** (written when the 1D ROM is active, PR 12b):

```
/rom1d/node_head_q05     [nTime, nActiveNode]   — 5th-percentile head (m)
/rom1d/node_head_q50     [nTime, nActiveNode]   — Median head (m)
/rom1d/node_head_q95     [nTime, nActiveNode]   — 95th-percentile head (m)
/rom1d/node_names        [nActiveNode]          — Variable-length string node names
```

The `/rom1d` group is created lazily on the first snapshot that contains
non-empty 1D ROM quantile fields. The `node_names` dataset provides the
active-node index table so readers can map quantile rows back to SWMM node
names. The existing `.uncertainty.csv` file (§5) continues to be written
as the zero-dependency path — both outputs are produced simultaneously.

**Reading the file**: the HDF5 output follows CF-1.11 and UGRID-1.0
conventions for unstructured triangular meshes, making it directly readable
by ParaView, QGIS, or any CF/UGRID-aware tool. The `/rom1d` group is a
custom extension outside the CF/UGRID standard; read it with h5py or
xarray as a supplementary group.

---

## 8. Known limitations

1. **Linear ROM operator can go stale between basis rebuilds.** The Galerkin ROM linearises the
   diffusion-wave operator (the eigenbasis `P` and its per-mode conductances) at the moment of
   the last basis build; it does *not* re-linearise every step. Under the deviation formulation
   this no longer causes the median to diverge — the median always equals the live deterministic
   solution regardless of basis age (§4.6) — but the *spread* (the Manning- and
   forcing-sensitivity magnitude) is computed against that possibly-stale operator, so it can be
   under- or over-estimated if the true diffusive structure (wet-cell coverage, effective
   conductance) has moved significantly since the last rebuild. The `WET_RESEED_FRACTION` guard
   (2D) and periodic `updateBasis()` calls (1D) limit how stale the operator can get; neither
   guarantees perfect spread accuracy for rapidly evolving wetting fronts or hydraulic jumps.

2. **1D ROM is diffusion-wave; DYNWAVE is not.** SWMM's 1D solver integrates the full
   Saint-Venant equations (continuity + momentum). The 1D ROM propagates uncertainty using a
   diffusion-wave (Laplacian) approximation, which drops the momentum term. For subcritical,
   gradually-varying flow in partially-full pipes — the standard urban drainage design case —
   this is a good approximation. It breaks down for: surcharging or fully-pressurized mains
   (**partially addressed — see limitation 10**); rapidly rising events where `∂Q/∂t` dominates
   and, specifically, a filling front's *arrival timing* (**substantially addressed in 1D — see
   §5.10 and limitation 11**); tidal or strongly backwater-controlled reaches; pump stations and
   sluice gates. In those
   regimes the ROM identifies the right *spatial pattern* of sensitivity but may mis-estimate
   band width. This limitation applies to the 1D ROM specifically; the 2D ROM's own fidelity
   tradeoffs (it operates on the explicit local-inertial marcher's linearized advection-diffusion
   operator — CVODE/diffusion-wave were retired from the 2D solver, see
   `docs/uncertainty/P3_2D_REHOME_SPEC.md`) are tracked separately in VALIDATION.md's 2D
   solver-mode-compatibility section. See §4.6 for the full 1D discussion. **A live signal for
   when you're in the degraded part of this regime is now available**: `fr_trust` in
   `<rpt>.rom_diag.csv` (§5.8) is a flow-weighted mean squared Froude number, computable at
   every report boundary without re-solving anything — `fr_trust ≳ 0.25` flags the peak-flow
   condition where this limitation's 10–25% band-width error applies.

3. **Mean K_eff (not per-cell).** The scalar K_eff path uses a single effective conductance
   for the full domain. Per-mode Rayleigh-quotient K_eff (§4.8 Option A) partially corrects
   for this, but for highly non-uniform Manning's n distributions the spread may be
   systematically underestimated in rougher zones.

4. **Uniform inputs on an UNGROUNDED network → zero ROM spread.** *(Substantially narrowed —
   this limitation used to apply to every model.)* A pure-Neumann graph Laplacian has the
   constant vector as a null mode, so every retained eigenmode is zero-mean and a spatially
   uniform field — the dominant response to a domain-wide parameter perturbation — projects to
   nothing. Reform PR 4 fixed this in 1D by **grounding** outfall-adjacent rows (Dirichlet), and
   W3 fixed the 2D counterpart by grounding outlet-adjacent cells; both are the default, and
   both are covered by tests asserting that a uniform field now projects non-trivially
   (`GroundedLaplacian.UniformVectorProjectsNonzero`,
   `MeshEigenBasis.GroundedBasisCapturesUniformShift`). What remains is the genuinely
   ungrounded case: a network or domain with **no outfall and no open boundary anywhere**, where
   there is nothing to ground against and the constant mode really is unrepresentable. Any
   documentation or intuition that still says "a uniform storm produces zero spread" predates
   these fixes.

   Separately, and unchanged: deviation-form spread starts at exactly zero at seed time by
   construction and grows toward its parametric steady state. That is spin-up (limitation 1),
   not a null-mode problem.

5. **Fixed spatial field seeds.** Spatial Manning and rainfall fields use fixed internal seeds
   (`0xdeadbeef01`, `0xcafebabe02`). Full seed control from the shared ensemble seed is not
   yet implemented.

6. **1D ROM quantiles now written to HDF5.** The 1D ROM is fully wired into the
   engine (`buildROM1D()` / `computeK1d()` / advance per step). Quantiles are
   accessible via `SWMMEngine::rom1d()` (see §7.7) and are now also written to
   the `/rom1d` group in the HDF5 output file when `OUTPUT_FILE` is specified
   (§7.10). The `.uncertainty.csv` file continues as the zero-dependency path.
   Both outputs are produced simultaneously when both are configured.

7. **q50 vs deterministic output.** Under the deviation formulation (reform PRs 6–7), q50 is
   *provably* bounded close to the deterministic CVODE/DynWave result: at 0% perturbation the
   two are identical to machine precision (tested — `DeviationForm.ZeroPerturbationIsExact` /
   `DeviationForm2D.ZeroPerturbationIsExact`), and at nonzero perturbation the median stays
   within 25% of the local spread of the deterministic value (tested —
   `DeviationForm.MedianTracksDeterministic` / `DeviationForm2D.MedianTracksDeterministic`).
   This replaces the earlier, weaker "differs slightly due to linearisation" characterisation
   from the pre-reform total-head formulation.

8. **WQ uncertainty is per-report-interval, not cumulative.** The water-quality uncertainty
   layer (§5.7) computes concentration bounds attributable to decay-rate uncertainty `k` over
   the **most recent report interval only** — not the cumulative uncertainty since the
   pollutant was first injected. Quality routing does not carry parcel age (only storage nodes
   track hydraulic residence time), so a fully cumulative band would require extending the
   routing state. The per-interval bounds are still useful for understanding how sensitive
   the concentration field is to decay-rate assumptions at each reporting period. A future
   version may track parcel age to produce cumulative bands.

9. **WQ uncertainty supports only UNIFORM distribution (v1).** The `WQUncertaintyBounds`
   computer generates its own ascending uniform strata internally. `NORMAL` and `LOGNORMAL`
   distributions on the `QUALITY` layer are rejected with an error message. A future version
   (Option B in the design doc) will accept a precomputed multiplier column from
   `UncertaintyEnsemble` to support arbitrary distributions and cross-parameter decorrelation.

10. **Surcharged-band attenuation is validated for `NODE_CONTINUITY SEMI_IMPLICIT` only.**
    Once a pipe runs full, the free-surface conveyance law the 1D ROM's Manning-sensitivity term
    assumes no longer holds, and left uncorrected this used to over-predict surcharged band
    widths 50–190×. The sidecar now smoothly damps that source term as a node crosses its crown
    (never to exactly zero — a pressurized pipe still loses head to friction depending on n).
    Validated against brute-force Monte Carlo separately per `NODE_CONTINUITY` mode: it lands
    cleanly under `SEMI_IMPLICIT`. Under `EXPLICIT`, near a single-conduit chokepoint the
    discrete surcharge branch produces a genuinely steeper backwater-vs-roughness response than
    the attenuated ROM tracks — confirmed to be a property of the regime, not a fixable
    calibration constant (the gap does not narrow at gentler surcharge severities either). **If
    you expect sustained surcharge and want validated band widths, run with `NODE_CONTINUITY
    SEMI_IMPLICIT`.** See `docs/uncertainty/VALIDATION.md`, "Surcharged-regime sensitivity
    attenuation (PR H5)," for the full measurement and root-cause writeup.

11. **Front-passage timing is now 1D-only.** §5.10's per-member phase coordinate closes most of
    the front-arrival-timing gap (measured: median width ratio during front passage went from
    0.009 — effectively unrepresented — to 1.354, comfortably within the acceptance band) but is
    **1D only**. The 2D counterpart (the same mechanism showing up as the drain-to-pond
    limitation — coverage 0.55–0.60 in VALIDATION.md's 2D solver-mode-compatibility section) is
    not addressed by this fix; a travel-time *field* rather than a path integral would be needed
    there, and remains open work. See `docs/uncertainty/VALIDATION.md`, "Per-member phase
    coordinate (PR H11)," for the full measurement.

---

## 9. Performance

### 9.1 2D surface mesh (measured)

Benchmark setup: sloped domain, uniform initial depth 0.10 m, 3 cm Gaussian bump, 30 advance
steps × dt=1 s. Full MC: 20 perturbed CVODE runs each advancing t=0→30 s.

| Configuration | Time |
|---|---|
| ROM k=6, M=20, 50×50 mesh (5k cells) | 1.85 ms |
| ROM k=10, M=20, 50×50 mesh | 2.93 ms |
| ROM k=10, M=50, 50×50 mesh | 4.03 ms |
| Full MC M=1 (1 CVODE run), 50×50 | 177 ms |
| Full MC M=20, 50×50 | 3 705 ms |
| ROM k=10, M=20, 100×100 mesh (20k cells) | 11.5 ms |
| Full MC M=1, 100×100 | 610 ms |

**ROM vs full MC speedup**: ~1265× at 50×50, M=20; ~1060× implied at 100×100.

ROM advance cost scales as O(M · k · n_steps) — purely arithmetic, no ODE solves.
Doubling M doubles ROM overhead, which is ~1% of total simulation time at M=50.

### 9.2 1D sewer network (estimated, N=10,000 nodes)

For a pure 1D sewer network with N=10,000 active nodes, M=50 members, k=20 modes:

| Phase | Cost | Notes |
|---|---|---|
| Lanczos eigensolver (one-time at `initialize()`) | ~15 ms | O(N·k) sparse mat-vec; paid once regardless of simulation length |
| ROM `advance()` per routing step | < 1 μs | M×k = 1,000 scalar exponentials |
| `computeQuantiles()` per routing step | ~2 ms | M×N = 500,000 reconstructions + per-node sort |
| Per-step overhead vs deterministic DYNWAVE | < 1% | DYNWAVE Newton solve dominates at ≫ 1 ms/step |

For a 6-hour storm at 30-second routing intervals (720 steps):

| | Full Monte Carlo (50 DYNWAVE runs) | ROM sidecar |
|---|---|---|
| Total overhead | ~50× baseline runtime | ~0.5% of baseline |
| Typical wall time | hours | seconds |

**Quantile reconstruction** (M×N per step) is the dominant 1D ROM cost for large networks.
For N=10,000 and M=50, each step reconstructs 500,000 head values. At 30 s routing intervals
this amounts to roughly 700 ms of quantile work per simulated hour. To reduce it: lower M,
increase the routing interval, or compute quantiles only on output steps rather than every step.

**Lanczos build** requires n_active ≥ 4 conduit-endpoint nodes (outfalls excluded). Build time
scales as O(N·k·iters) where iters ≈ 3k for typical Lanczos convergence. At N=10,000 and k=20
this is approximately 15 ms — a one-time cost absorbed into `swmm_engine_initialize()`.

---

## 10. Visualization

### 10.1 Python scripts (scripts/uncertainty/)

| Script | Output |
|---|---|
| `plot_coupled_1d_2d.py` | 6-panel: 2D q50/spread maps + per-node flow hydrographs with q05/q50/q95 bands at 4 nodes along the 1D trunk |
| `plot_pipe_transect.py` | 4-panel: longitudinal flow profile at 4 time snapshots, line colored by relative spread (%) using `LineCollection` |
| `plot_wet_event_bands.py` | 1D KW confidence bands (Edinburgh trunk, ±25% Manning) |
| `plot_2d_uncertainty_maps.py` | 2D spatial maps of q50 depth and q95−q05 spread |
| `plot_coupling_uncertainty_5A_5B.py` | Per-coupling-point flux bounds over time |
| `fiedler_diagnostic.py` | Fiedler vector analysis — identifies bottleneck nodes |
| `mcmc_vs_rom.py` | ROM prior vs MCMC posterior comparison |

Run with the Anaconda Python:
```bash
MPLBACKEND=Agg /Users/corinnewiesner/anaconda3/bin/python3 \
    scripts/uncertainty/plot_pipe_transect.py --out transect.png
```

### 10.2 Colored pipe transect

The `plot_pipe_transect.py` script uses `matplotlib.collections.LineCollection` to draw
each segment of a pipe colored by its relative uncertainty:

```python
from matplotlib.collections import LineCollection
import matplotlib.colors as mcolors

norm = mcolors.Normalize(vmin=0, vmax=50)     # 0–50% relative spread
pts  = np.column_stack([x_station, q50_flow])
segs = np.stack([pts[:-1], pts[1:]], axis=1)
lc   = LineCollection(segs, cmap="YlOrRd", norm=norm, linewidth=2.5)
lc.set_array(0.5 * (spread_pct[:-1] + spread_pct[1:]))
ax.add_collection(lc)
fig.colorbar(lc, ax=ax, label="Relative spread (q95−q05)/q50 %")
```

Replace `x_station`, `q50_flow`, and `spread_pct` with arrays from your node extraction loop.

### 10.3 Future: HDF5 / ParaView / QGIS

Once the HDF5 output pipeline is wired (§7.10), the CF-1.11/UGRID-1.0 datasets will be
readable by any CF-aware tool:

```python
import xarray as xr
ds = xr.open_dataset("results.h5", engine="netcdf4")
spread = ds["Mesh2_face_depth_q95"] - ds["Mesh2_face_depth_q05"]
```

For ParaView: open the `.h5` file, select `Mesh2_face_depth_q95` and subtract
`Mesh2_face_depth_q05` via a Calculator filter to visualise the uncertainty band width.

---

## 11. Soft Rainfall — Supplying Your Own Rainfall Distributions

The soft-rainfall feature lets you supply per-gage or per-grid rainfall
uncertainty as a location-scale family (NORMAL, LOGNORMAL, UNIFORM) without
materializing an ensemble. The deterministic rain IS the location parameter;
the uncertainty supplies only the spread. The ROM propagates the spread through
its modal ODE via the two-projection form `f_ij = r_loc[j] + c_i · r_spread[j]`,
where `c_i` is the family-selected per-member coefficient.

### 11.1 `[SOFT_RAINGAGES]` — Per-Gage Soft Rainfall

```
[SOFT_RAINGAGES]
RG1  NORMAL   CV        0.30
RG2  LOGNORMAL SD        TIMESERIES SPREAD_TS
RG3  UNIFORM  HALFRANGE 1.5
```

**Grammar**: `Gage Family SpreadKind SpreadSource|TIMESERIES <name>`

- **Gage**: name of a gage defined in `[RAINGAGES]`.
- **Family**: `NORMAL`, `LOGNORMAL`, or `UNIFORM`.
- **SpreadKind**: `SD` (absolute standard deviation), `CV` (coefficient of
  variation, relative), or `HALFRANGE` (absolute uniform half-range; only
  valid with `UNIFORM`).
- **SpreadSource**: a non-negative constant, or `TIMESERIES <name>` for a
  time-varying spread.

**Per-member evaluation** (§4.3 of the design doc):
- NORMAL: `rain_i = loc + z_i · sd`
- LOGNORMAL: `rain_i = loc · exp(z_i · σ_log)` (delta-linearized as
  `loc + z_i · loc · σ_log` in the ROM; warn when CV > 0.5)
- UNIFORM: `rain_i = loc + (2u_i − 1) · halfrange`

where `u_i = shuffledStrata(M, seed+4)[i]` and `z_i = probit(u_i)`.

A `[SOFT_RAINGAGES]` entry activates the 1D network ROM on its own. The ROM's
forcing field is the dh/dt head-rate buffer, so the gage-level spread is
mapped as `spread_now[n] = dh/dt[n] · area-weighted relative spread`.

### 11.2 `[SOFT_RAINFALL_GRID]` — Gridded Soft Rainfall

```
[SOFT_RAINFALL_GRID]
2D     radar_grid.h5  CENTROID  FORCE_LOCATION
RUNOFF radar_grid.h5  CENTROID  FORCE_LOCATION
INFLOWS node_grid.h5  CENTROID  FORCE_LOCATION  NODES  nodes.txt
```

**Grammar**: `Target File Mapping [Options]`

- **Target**: `2D` (per-cell surface rainfall), `RUNOFF` (per-subcatchment),
  or `INFLOWS` (per-node lateral inflow).
- **File**: path to an HDF5 grid file (§11.3).
- **Mapping**: `CENTROID` (nearest cell center). `BILINEAR` and `AREA_MEAN`
  are planned (SR-4a).
- **Options**: `FORCE_LOCATION` (the grid's `/location` plane overrides the
  deterministic rainfall; without it, the model's existing rain is the location
  and the grid supplies spread only). `NODES <file>` (for INFLOWS target only:
  a text file listing node names, one per line).

### 11.3 HDF5 Grid File Layout

```
/               attrs: family ("NORMAL"|"LOGNORMAL"|"UNIFORM"|"MIXED"),
                       spread_kind ("SD"|"CV"|"HALFRANGE"), units, crs (optional)
/time           (T)        float64
/x, /y          (nx),(ny)  float64  grid coordinates (cell centers)
/location       (T,ny,nx)  float32  — optional (deterministic location parameter)
/spread         (T,ny,nx)  float32  — required (spread: SD, CV, or HALFRANGE)
/family_code    (ny,nx)    uint8    — ONLY when family == "MIXED"
```

- `float32` everywhere (rainfall precision does not warrant float64).
- `/location` is optional: when absent, the model's existing rain input is the
  location parameter and the file supplies spread only.
- `/spread` is required: `0` at a pixel/time means hard (no uncertainty) there.
- `/family_code` (MIXED only): per-cell distribution family
  (0=NORMAL, 1=LOGNORMAL, 2=UNIFORM). The ROM uses the NORMAL coefficient `z_i`
  for all cells as a v1 approximation; UNIFORM cells have their spread
  pre-scaled by the coefficient range ratio. Exact per-cell per-member dispatch
  is the design's deferred cold path.

### 11.4 pybme Round-Trip Example

The `scripts/uncertainty/pybme_soft_rain_example.py` script demonstrates the
full workflow: synthetic gages + radar → pybme BME posterior (mean, sd) →
§3.3-conformant HDF5 → engine run → q05–q95 band plot. The
`write_soft_rain_hdf5()` function in the script is a standalone schema-writer
helper that others can copy. If pybme is not installed, the script falls back
to a Gaussian posterior approximation.

### 11.5 Deprecation Note: Scalar `RAINFALL` in `[UNCERTAINTY]`

The scalar `RAINFALL` parameter in `[UNCERTAINTY]` (which applies a single
multiplier to all rainfall) is superseded by the soft-rainfall feature. For
new work, prefer `[SOFT_RAINGAGES]` (per-gage location-scale families) or
`[SOFT_RAINFALL_GRID]` (gridded) for spatially distributed rainfall uncertainty.
The scalar path remains functional for backward compatibility.

### 11.6 Spatial correlation — `COHERENCE` option (CL-1 + CL-2, v1 complete)

The default soft-rainfall behaviour (`COHERENCE FULL`) is **comonotone**: every
ensemble member uses the same scalar coefficient across all space, so member i
is uniformly wet or dry everywhere. This is conservative and fast, but can
produce artificially wide downstream bands because per-member perturbations
never cancel.

**Problem:** storm tracks in reality are spatially heterogeneous. A member that
is wet in region A may be dry in region B, allowing downstream hydrographs to
decorrelate rather than compound. The `COHERENCE CORR_LEN <meters>` option
introduces **spatially-varying per-member coefficients**, decorrelating space
and producing tighter, more physical bands.

**Syntax:**

```ini
[SOFT_RAINGAGES]
RG1  NORMAL  CV  0.30  COHERENCE FULL              ;; comonotone (default)
RG2  NORMAL  CV  0.30  COHERENCE CORR_LEN 500     ;; 500 m spatial correlation

[SOFT_RAINFALL_GRID]
2D    radar_grid.h5  CENTROID  FORCE_LOCATION  COHERENCE FULL          ;; comonotone
2D    radar_grid.h5  CENTROID  FORCE_LOCATION  COHERENCE CORR_LEN 1000 ;; 1 km
```

**Mechanics:** When `CORR_LEN` is set to a positive value, `CorrelatedFieldGenerator`
builds an M×n field (M members, n cells/nodes) by:

1. Drawing i.i.d. N(0,1) random samples at each spatial location (cell centroid or node
   coordinate).
2. Smoothing with an exponential kernel over a 3·ℓ neighborhood, where ℓ is the
   correlation length.
3. Ranking members by their smoothed field values at each location and assigning the
   global ensemble coefficients {c_i} by rank.

Result: **per-cell ensemble exactly matches the input coeff set {c_i}** (marginal
correctness), but members have **different signs/magnitudes in different cells**
(spatial decorrelation). As ℓ → ∞, the field collapses to a scalar (comonotone
limit); as ℓ → 0, each cell's ensemble becomes independent.

**Physical interpretation — "storm track uncertainty":** imagine a localized heavy
rainfall nucleus moving across a domain. Member i might catch the nucleus in cell
A (high rainfall, low coefficient) and miss it in cell B (low rainfall, high
coefficient). Downstream, the two cells' uncertainties partially cancel instead of
compounding.

**Cost and limitations:**

- **Runtime cost:** the comonotone path projects `Pᵀ·spread` once per step —
  O(k·n) work (k modes over n cells), then a scalar `c_i` scaling per member.
  The correlated path adds per-member projections: O(M·k·n) with the
  materialized field. When the **reduced spatial basis** is active (CL-2,
  automatically selected for large meshes when `K_s < M`), this drops to
  O(K_s·k·n) basis projections + O(M·K_s·k) reconstruction, `K_s ≪ M`. For
  M=50, k=10, n=10k cells the correlated overhead is a few ms/step — typically
  a small fraction of total simulation time.
- **Storage:** the correlated field (or its `K_s`-mode basis) is built **once at
  initialization and reused every advance** — it is not regenerated per step.
  The materialized path holds a persistent M×n array; the reduced-basis path
  stores only the K_s × n mode fields plus the M × K_s coefficients, avoiding
  the full M×n materialization on large meshes.
- **Comonotone bit-identical:** `COHERENCE FULL` or omitted `COHERENCE` reproduces
  the scalar path exactly (regression-locked).
- **Uniform prior assumption:** the rank/copula construction assumes a uniform
  ordering of members across space — for non-uniform families (e.g., LOGNORMAL),
  interpret the bands as a qualitative indicator, not a calibrated statistic.

**Typical settings:**

| Scale | ℓ (m) | Use case |
|---|---|---|
| 100–500 | Small domain (< 5 km) urban catchment | mesoscale storm variability |
| 500–2000 | Medium domain (5–20 km) regional model | synoptic-scale heterogeneity |
| > 5000 | Large domain or very heterogeneous terrain | landscape-scale patchiness |
| 0 or absent | | comonotone (default, fastest) |

**Example — coastal stormwater with approaching storm surge:**

```ini
[SOFT_RAINGAGES]
RG_NORTH   NORMAL  CV  0.25  COHERENCE CORR_LEN 800
RG_CENTRAL NORMAL  CV  0.25  COHERENCE CORR_LEN 800
RG_SOUTH   NORMAL  CV  0.25  COHERENCE CORR_LEN 800
```

At a single node near the coast, the q95 − q05 band is narrower than if all three
gages had independent coefficients (comonotone would give the widest band, since
all three members move together). At an inland node, members' uncertainties
partially cancel because the storm nucleus was off-center for that region.

**Deprecation note:** the related `MANNINGS_CORR_LEN` option (§6.1) applies
spatially-correlated Manning's n fields; `COHERENCE CORR_LEN` is the rainfall
equivalent.
