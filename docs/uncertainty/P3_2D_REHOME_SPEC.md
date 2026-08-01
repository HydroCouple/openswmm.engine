# P3 (revised) — 2D ROM Re-Home onto the Explicit Local-Inertial Marcher

Status: draft 2026-07-31. Supersedes the 2D portions of
`HSYM_RESIDUALS_PR_CHECKLIST.md` §0.5.5 / PR P3 / PR P4, which assumed a
`CvodeSurfaceSolver` + `ArkodeSurfaceSolver` world with a consumable implicit
operator snapshot. That premise is obsolete (see "What changed"). The 1D
portions of P3/P4 are unaffected and proceed as originally written.

---

## What changed on `swmm6_rel` (tip `1e531a8a`, 2026-07-31)

- `7e65dffb feat(2d)!: retire the CVODE/ARKODE stack — the explicit marcher is
  the only 2D integrator`
- `3fd113a7 build!: drop SUNDIALS and hypre — the explicit marcher needs no
  linear solver`
- `133d4946` explicit local-inertial FV marcher (`INTEGRATOR EXPLICIT`);
  `7e757555` tiered local timestepping (LTS, `lts_tiers` 1..8);
  `695fbca3` windowless per-step co-advance coupling; `e2bff7da` couple 1D↔2D
  every routing step by default (`COUPLING_SYNC` opt-in).

Net: the 2D layer is now **one explicit backend family** —
`ExplicitInertialSolver` (CPU default) + `ExplicitKokkosSurfaceSolver` (GPU) —
behind the surviving `ISurfaceSolver` interface. **No linear solve, no Jacobian,
no preconditioner.** There is no operator to snapshot; `SWMM_OperatorSnapshot`
is now a **1D-only** concern.

## Design principles (unchanged, and the reason this is feasible)

The sidecar is a **read-only, deviation-form observer** that anchors on the
deterministic post-step state (`h_det`) and evolves member *deviations*
`δa = Pᵀ(h − h_det)` on a reduced eigenbasis. It consumes **state**, never
solver internals. Load-bearing invariant: the deterministic run is
**bit-identical with the ROM on vs off** (protects the Bellinge float32 parity
ladder). This is why a full 2D integrator replacement does not break the ROM's
contract — it only invalidates *where the eigenbasis came from* and *whether the
decay operator is still calibrated*.

The `ISurfaceSolver` interface already exposes everything the re-home needs:
mesh + `SurfaceStateData` by reference, `last_coupling_exchange()` (per-point
∫Q dt, m³), and advance telemetry. **No new virtuals should be added.**

---

## W1 — Standalone 2D eigenbasis  [Size S, mechanical]

**Problem.** `SpectralROM`'s basis came from `SpectralPrecond2D`
(`src/engine/2d/solver/SpectralPrecond2D.{hpp,cpp}`), which was built as a
**SUNDIALS Krylov preconditioner** (`psolve`/`psetup`/`SUN_PREC_LEFT`). With
SUNDIALS dropped, the preconditioner role is dead — but the part the ROM
actually used, a **Lanczos eigensolve of the geometric mesh graph-Laplacian**,
is SUNDIALS-independent.

**Action.** Extract that core into a standalone `MeshEigenBasis`, mirroring the
already-proven 1D `GraphEigenBasis` (`src/engine/uncertainty/GraphEigenBasis`):
- Build the symmetric geometric graph-Laplacian from mesh face-adjacency +
  centroid distances (this is exactly the operator `SpectralPrecond2D` already
  assembled — lift it verbatim, drop the `SUN*` wrappers).
- Lanczos + QL → `P` (n×k), `Λ` (k). Zero-mean ramp start vector for null-mode
  avoidance (same guard as `GraphEigenBasis` / `SpectralCoarse`; a constant
  start vector is the null eigenvector → immediate breakdown).
- `SpectralROM` depends on `MeshEigenBasis` only. Delete `SpectralPrecond2D`
  (its only surviving purpose was this basis).

**Test.** `MeshEigenBasis` eigenpairs reproduce the pre-retirement
`SpectralPrecond2D` `P`/`Λ` to 1e-10 on the Phase-8 fixtures (5-vertex/4-tri and
a structured 50×50), after the mean-1 weight normalization.

**Note.** The 2D ROM **never depended on the implicit operator** — its basis was
always the *geometric* Laplacian, merely borrowed from the preconditioner
object. So there is no lost fidelity here; this is a sourcing change, not a math
change. (Contrast the 1D ROM, whose weighted-Laplacian *refinement* genuinely
reads `dqdh` — that path is 1D-only and untouched.)

---

## W2 — Hoist ROM ownership to `SurfaceRouter2D` + re-home coupling  [Size M, design]

This is the decision P3 flagged for a one-pager. With one solver family it is now
unambiguous: **the ROM lives on `SurfaceRouter2D`, not on any solver.**

**Ownership.** `SurfaceRouter2D` owns `rom_` (`SpectralROM`), `rom_basis_`
(`MeshEigenBasis`), the non-owning `rom1d_` pointer, ROM seeding, and quantile
computation. Rationale: it already owns the mesh, `SurfaceStateData`, and
`unique_ptr<ISurfaceSolver> solver_` — so **both** explicit backends get the ROM
for free with zero solver-side code.

**Lifecycle mapping onto `ISurfaceSolver`:**
| Router step | ROM action |
|---|---|
| after `solver_->initialize(mesh,state,opts)` | build `rom_basis_` from `mesh`; `rom_.initialize(...)` |
| after `solver_->advance(t0,t1)` | read post-step depth from `state` as `h_det`; `rom_.advance(dt, k_eff, rainfall)` |
| at report boundary (`output_due()`) | `rom_.computeQuantiles(h_det)` → HDF5 q05/q50/q95 + CSV |
| coupling | see below |

- `k_eff` is computed in `SurfaceRouter2D` from mesh Manning/slope/depth — a
  **physical** quantity, integrator-independent (unchanged from today).
- **Coupling re-home.** Replace `applyCouplingFluxToROM`'s old
  `COUPLING_INTERVAL` macro-window + coupling-queue smoothing with the
  **windowless per-step** path: take the deterministic per-point exchange from
  `solver_->last_coupling_exchange()` (∫Q dt, m³) and apply per-member
  `δQ_i = Q_i − Q_det` once per routing step. This matches `e2bff7da` (couple
  every step); the queue-smoothing mechanism is no longer needed. If
  `COUPLING_SYNC > 0`, the ROM batches on the same span as the deterministic
  co-advance, for consistency.

**Non-intrusiveness guard.** The ROM path reads `state`/`mesh`, never writes
them. Add/keep the test: deterministic 2D output **bit-identical** with ROM on
vs off (the invariant that protects float32 parity).

**Interface impact: none.** Everything needed is already on `ISurfaceSolver`. If
some quantity seems missing, compute it in `SurfaceRouter2D` from mesh+state
rather than widening the interface.

**The operator: one reduced `k×k` matrix `M` (DECIDED 2026-07-31 — go anisotropic
directly).** Rather than build two operators (isotropic then anisotropic), the
ROM's `advance()` carries the flow-aligned anisotropic advection–diffusion
operator of `LOCAL_INERTIAL_DEVIATION_OPERATOR.md` §5 as a **Galerkin projection
onto the fixed geometric eigenbasis** `P` (from W1, built once — *no*
flow-dependent re-eigensolve):

> `δa = Pᵀ(h − h_det)`;  `M = Pᵀ·L_op·P` (`k×k`, `k≈24`);
> `δa(t+Δt) = exp(−M·Δt)·δa(t) + forcing`  (a `k×k` matrix exponential, µs-cheap).

Every physics rung is just what is assembled into `M` — **one code path, a dial**:
- isotropic (old-ROM parity / validation baseline): `M = diag(D·λ_j)`, `D⊥/D∥=1`;
- **anisotropic (target)**: `M = Pᵀ(D∥∂²∥ + D⊥∂²⊥)P`, edge conductances weighted
  by the edge–flow angle (`u` from readable face velocity);
- **+ advection**: add the skew `Pᵀ(−c_k ∂∥)P`, `c_k=(5/3)u`.

`D∥`, `D⊥`, `c_k` are **parameters W3 calibrates** against the marcher MC — the
structured-grid values (`D∥≈0.31·K_eff`, `D⊥≈1.0·K_eff`, `c_k=(5/3)u`) are the
starting point, not hard-codes. `M` and `P` read only mesh + flow state, so
non-intrusiveness holds. `M` is reassembled only on the basis-update cadence
(when the flow field moves materially), not per step.

**Note (operator-snapshot, 1D):** for the **in-engine** full sidecar the 1D
weighted-Laplacian refinement reads DW's `dqdh`/`sumdqdh` **directly** via the
already-ported `HSnapshot` (1D DW is unchanged). The public `SWMM_OperatorSnapshot`
PR was the surface for *external* consumption only — **reference-only here**; no
rebase of that branch is required for the in-engine path.

---

## W3 — Re-validate bands vs brute-force MC under the marcher  [Size M–L, the real science]

**Why this is the crux, not the wiring.** The old decay operator `K_eff·λ`
modeled Manning **diffusion** — it fit the CVODE diffusion-wave solve because
that Jacobian *was* approximately a weighted graph-Laplacian. The marcher solves
**local-inertial** dynamics (momentum term retained; wave-like character). The
eigenbasis is still a valid *spatial* basis for smooth spread fields, but the
deviation-**decay-rate** model is now an approximation of *different physics*.
Coverage and band width must be **re-measured**, not assumed.

**Prototype finding (2026-07-31, `prototypes/local_inertial_decay.py`).** A
faithful 1D implementation of the exact `inertialFaceUpdate` kernel, linearized
about a sustained uniform Manning flow, shows the deviation operator **collapses
cleanly to advection–diffusion**:

> `d(δh)/dt = D·∇²(δh) − c_k·∇(δh)`, valid for `Λ = r_f/(c·k) ≳ 3`
> (friction-dominated / wetted / smooth low modes — i.e. PR-10's saturated regime).

- **Diffusion** `D` — *same functional form as the old ROM* (`h^{10/3}/(2n²q0)`,
  the `K1d` family). Measured `D` is **k-independent** (0.712, 0.712, 0.711, 0.710
  across modes m=1..4) — confirming a true `D·λ_j` operator on the same eigenbasis.
  It carries a **scheme-specific O(1) factor ≈ 0.71** on the naive `K_eff/2`
  (from the `hf = max(η)` face reconstruction + semi-implicit `|q|` lag — a
  constant textbook diffusion-wave theory cannot supply; it must be measured).
- **Advection** `c_k` — **NEW**: measured `= (5/3)·u` (the Manning kinematic-wave
  celerity) to 3–4 digits, also k-independent. The old ROM used a **symmetric**
  graph-Laplacian → pure diffusion → it **dropped this skew term**. It makes
  deviation bands *travel with the flow* rather than only spread. (This is the
  same "skew lives in the gap, not in H" observation as H3 §0.5.4, made concrete.)
- **Breakdown**: for `Λ ≲ 3` (thin films, dry fronts, high mode-k) the reduction
  fails and damped gravity waves appear — the *same* regime the old ROM was
  already untrustworthy in (front-timing spread, spin-up-from-zero).

**2D prototype finding (2026-07-31, `prototypes/local_inertial_decay_2d.py`).**
A faithful *structured-2D* implementation (Perot cell-velocity reconstruction,
vector-magnitude friction — the real kernel's `|q⃗|`, not `|q_n|`) shows the
operator is **not isotropic**. Because Manning friction uses the flow-vector
magnitude, the linearized friction rate is `2 r_f` **streamwise** but `r_f`
**transverse**, so the diffusion is a **flow-aligned tensor**:

> `∂δh/∂t = D_∥·∂²_∥δh + D_⊥·∂²_⊥δh − c_k·∂_∥δh`

Measured (96², n=0.12, Fr=0.32, extrapolated to `ε=g h k²/r_f² → 0`):
- `D_∥ ≈ 0.62·(K_eff/2) = 0.31·K_eff` (streamwise), k-independent;
- `D_⊥ ≈ 1.00·K_eff` (transverse), k-independent;
- **anisotropy `D_⊥/D_∥ ≈ 3.2`** — the physical factor-2 (vector friction),
  amplified by the scheme damping streamwise diffusion more than transverse;
- `c_k = (5/3)u` streamwise only, exact to <1%;
- diagonal mode reproduced by `D_∥ kx² + D_⊥ ky²` to 4% → the tensor form is right.

**Consequence for the re-home** (revised — bigger than the 1D view suggested,
but still clean and bounded): the old ROM's **isotropic** geometric Laplacian
mis-weights along- vs across-flow spread by ~3×. Options, by fidelity:
1. **Isotropic, recalibrated** (cheapest): keep the geometric Laplacian, scale by
   an effective scalar `D` between `D_∥` and `D_⊥`. This is what the old ROM did;
   PR-10 showed it adequate for diffuse/low-flow fixtures. Ship first, let MC judge.
2. **Flow-aligned anisotropic Laplacian** (faithful): assemble the graph Laplacian
   with **anisotropic edge conductances** — weight each mesh edge by `D_∥` or `D_⊥`
   per the angle between the edge and the local flow direction (`u` from readable
   face velocity). Still symmetric, sparse, same eigensolve machinery — just
   flow-dependent weights (rebuilt on the ROM's basis-update cadence, not every step).
3. **+ skew advection** `−c_k ∂_∥δh` (`c_k=(5/3)u`), a directed-edge difference,
   if MC shows bands are mis-positioned in channelized flow.

The functional forms are all clean and readable-state-only; the open question is
purely **how much fidelity MC coverage demands** (isotropic scalar vs. flow-aligned
tensor), which W3 answers with numbers — not a structural unknown.

**Protocol** (extends PR-10 / `VALIDATION.md`):
- Fixture: wet 2D domain, **off-centre** IC (centred Gaussian projects to zero
  on anti-symmetric low modes — documented symmetry pitfall), ±20% Manning's n.
- Both sides run the **same** solver (the marcher): M brute-force perturbed-n
  marcher runs vs the ROM M-ensemble.
- Assert: coverage (deterministic ∈ [q05,q95]) ≥ 0.95; width-ratio ROM/MC median
  ∈ [0.5, 2], ≥95% of samples ∈ [0.3, 3]; per-cell band monotonicity
  q05 ≤ q50 ≤ q95.
- **LTS caveat**: read `h_det` only at routing-step / coupling boundaries
  (macro-steps), never mid-tier. Assert the anchor cadence.

**Decision (2026-07-31): the operator is built anisotropic-capable from the
start** (the reduced `M` of W2 / `LOCAL_INERTIAL_DEVIATION_OPERATOR.md` §5). W3 is
therefore **calibration**, not an operator-choice ladder — **recalibrate the
constants in `M`, never loosen the band tests** (hard rule):
1. **Pin `D∥`, `D⊥`** in `M` against MC on the real unstructured/VFR mesh
   (structured-grid starts: `D∥≈0.31·K_eff`, `D⊥≈1.0·K_eff`). The isotropic
   setting (`D∥=D⊥`) is retained as the **sanity baseline** — it should reproduce
   old-ROM-style behavior and bounds the anisotropy's effect.
2. **Skew advection on/off**: keep `−c_k∂∥`, `c_k=(5/3)u` in `M` if the
   streamwise-vs-transverse width-ratio split (the harness reports it) shows bands
   mis-positioned in channelized flow; drop it for diffuse sheets. A calibration
   flag, not a new operator.
3. Only if constants won't hold across regimes: make them depth/Froude-dependent
   from readable state. (Prototypes show constants k-independent for `ε≲0.05`, so
   unlikely inside the ROM's validity envelope.)

**Deliverable.** A `VALIDATION.md` section "Solver-mode compatibility (explicit
marcher)" with measured coverage/width tables and a go/no-go: does the geometric
diffusion operator suffice, or is the inertial recalibration required.

**Harness sketch**: `tests/regression/test_2d_rom_marcher_coverage.cpp` (the 2D
analogue of PR-10's `test_rom_coverage.cpp`). Drives the real
`ExplicitInertialSolver` for M perturbed-n MC runs and `SpectralROM` for the
band, with `setExternalSamples()` feeding **identical** per-member multipliers to
both sides (like-for-like), and an inline **streamwise-vs-transverse width-ratio
split** that reports the anisotropy signal — the number that decides rung 1 vs.
rung 2 above. Keyed to the post-rebase tree (needs the marcher headers + W1's
`MeshEigenBasis`); not yet in CMake. Initial gates are the P4 floors
(coverage ≥ 0.90, width-ratio median ∈ [0.5, 2]) — tighten once the rung is chosen.

---

## Sequencing & effort

`W1 (S)` → `W2 (M)` → `W3 (M–L)`. W1+W2 = "compiles and runs correctly-wired
under the marcher"; W3 = "trustworthy under the new physics" (may spawn a
recalibration sub-task, which is where a Fable/premium-tier numerics pass earns
its keep).

**1D is out of scope here and unchanged**: DW is still Picard; the
operator-snapshot re-home and the P4 compatibility matrix proceed as originally
specified. The explicit retirement is a **2D-only** event.

## Open questions (human decision)

1. Name: `MeshEigenBasis` (recommended — the "preconditioner" identity is dead)
   vs. retaining `SpectralPrecond2D` as a misnomer.
2. ~~Does the local-inertial response admit a clean analytic deviation-decay
   rate?~~ **Answered (1D+2D prototypes)**: yes — a clean *flow-aligned
   anisotropic* advection–diffusion. Streamwise `D_∥≈0.31·K_eff`, transverse
   `D_⊥≈1.0·K_eff` (`D_⊥/D_∥≈3.2`), skew `c_k=(5/3)u`. Remaining call — set by MC
   coverage (W3): isotropic-scalar (like the old ROM) vs. flow-aligned anisotropic
   Laplacian, and whether to include the skew advection term.
3. Should ROM coupling always follow the deterministic `COUPLING_SYNC` batching,
   or advance every routing step regardless? (Default: follow the deterministic
   path for consistency.)

## Strategic note (re: in-engine vs. satellite)

The retirement makes the **in-engine** 2D re-home *cleaner* (one explicit
backend, a tidy `ISurfaceSolver` seam that already surfaces state + coupling
exchange). It makes an **external** 2D sidecar *harder*: with no operator to
exploit, an out-of-engine ROM would have to rebuild the mesh eigenbasis itself
and pull the full depth field across the API every routing step — losing the
O(M·k) advantage. This is a genuine data point favoring keeping the 2D
*computation* in-engine; it does not by itself decide the packaging or
visualization questions.
