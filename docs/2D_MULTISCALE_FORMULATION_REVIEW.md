# 2D Diffusive-Wave Formulation Review — Multiscale Correctness & the Newton-Corrector Failure Storm

**Date:** 2026-07-26
**Scope:** the 2D overland-flow formulation itself (`src/engine/2d/`), reviewed against the
requirement that the model be **multiscale** — coarse rural cells (10²–10³+ m²) coexisting
with fine urban cells (≤ 1 m², observed down to 0.04 m²) on strongly graded terrain — and
against the measured pathology: **128,081 CVODE "corrector convergence test failed repeatedly
or with |h| = hmin" errors** on a 13,116-cell rain-on-mesh 48 h run (no 1D coupling,
`MIN_TIMESTEP 0.1`), with essentially **zero error-test failures**, concentrated in storm
wetting.
**Companion:** `docs/2D_MODEL_AND_COUPLING_REVIEW.md` (2026-07-18) covers architecture and
1D↔2D coupling; this document deliberately does not repeat it.

Configuration assumed for the pathology run = the defaults:
`CELL_CLOSURE=FLAT`, `FACE_RECONSTRUCTION=MEAN`, `JACOBIAN=ANALYTIC`, `PRECONDITIONER=AMG`,
`REL_TOLERANCE=1e-4`, `ABS_TOLERANCE=1e-6 m`, `ATOL_AREA_REF=AUTO(median)`,
`FLUX_DH_EPS=4 mm`, `DRY_DEPTH=1 mm` (`data/SolverOptions2D.hpp:193-311`), plus the user's
`MIN_TIMESTEP 0.1` (passed straight to `CVodeSetMinStep`,
`solver/CvodeSurfaceSolver.cpp:586`).

Verdict legend: **CORRECT** (right as implemented), **SUSPECT** (defensible but with a
concrete failure mode in scope), **INAPPROPRIATE-FOR-MULTISCALE** (breaks or degrades
specifically across the required scale range).

---

## Findings (ranked, most severe first)

### F1. Per-cell non-smooth wetting events × a hard `hmin` floor: Newton *cannot* converge across a wetting front at h = 0.1 s — this is the failure storm

**Verdict: INAPPROPRIATE-FOR-MULTISCALE** (formulation kinks whose magnitude scales as 1/A,
made fatal by using `MIN_TIMESTEP` as CVODE's hard `hmin`).

**Evidence.** The semi-discrete RHS is continuous (C⁰) everywhere but only *piecewise* C¹.
Two state-dependent kink families fire once per cell per wetting/drying event:

1. **The upwind flip.** `solver/SurfaceFluxCalculator.cpp:339` selects
   `upstream = (h_L >= h_R) ? i : nbr`, and under the default `MEAN` face depth the
   conveyance depth is the upwind **cell-mean** depth (`:340`,`:342`). At the flip surface
   `h_L = h_R` the flux is 0 (continuous), but its slope
   `∂F/∂Δη = −C·h_up^{5/3}·Φ'(0)` jumps by `C·Φ'(0)·|h_L^{5/3} − h_R^{5/3}|`
   (`:390-401`). Precisely at a wetting front one side is *deep* and the other *dry*, so
   the jump is maximal — proportional to the full wet depth^{5/3} — and `Φ'(0) = 1.5/√ε ≈
   23.7 m^{-1/2}` from the `regSqrt` regularization (`:95-99`, ε = 4 mm). Note the
   opt-in `VFR_FACE` reconstruction (`:354-363`) makes the conveyance depth *continuous*
   across the flip (both sides evaluate the same η at the crossing), i.e. the **default**
   configuration is the most kink-prone one.
2. **The V = 0 clamp.** `reconstructFromVolume` clamps `v = (V > 0) ? V : 0`
   (`solver/CvodeSurfaceSolver.cpp:69`); no positivity constraint is set on CVODE, and
   negative volumes are deliberately preserved (`resyncFromVolumes`, `:888-910`). So
   `∂η/∂V` jumps from 0 (V ≤ 0: the whole RHS is constant in V) to `1/A` (FLAT) at V = 0 —
   a kink crossed by every cell that wets, and by every over-drained cell that recovers.

Smooth-by-design constructs (correct, listed for completeness): the `h_up^{5/3}` dry-out
(C¹ at h = 0, no explicit wet/dry switch, `:404-408`), `regSqrt` (C¹, second derivative
jumps only, `:95-99`), `faceDepthFromEta` (piecewise C¹ in η, `:65-74`; the level-edge
branch `dz < 1e-9` has a 0→1 slope jump at η = z_lo but is measure-zero), the evaporation
Hermite ramp (C¹, `SurfaceFluxCalculator.hpp:81-86`), and the VFR closure with its ε-tail
(C¹, `mesh/VfrClosure.hpp:133-198`). Rain/coupling/BC forcings are held constant per window
(state-independent during a solve), so table lookups never enter the Newton residual.

**Why it kills the corrector, quantitatively.** CVODE's (modified, inexact) Newton iterates
with `M = I − γJ`, γ ≈ h. Across a kink the iteration matrix is built on one side while the
solution sits on the other; the contraction factor is ≈ `γ·|ΔJ|` where ΔJ is the Jacobian
jump. For a *receiving* cell of area `A` next to a wet cell of depth `h_w`:

```
ΔJ ≈ C · h_w^{5/3} · Φ'(0) / A,     C = ξ/(n·√Δx)
```

Urban numbers (ξ = Δx = 1 m, n = 0.02 → C = 50; h_w = 0.1 m → h^{5/3} = 0.0215;
Φ'(0) = 23.7): `ΔJ ≈ 25/A s⁻¹`. Convergence therefore needs

- A = 1 m²   → h ≲ 0.04 s
- A = 0.04 m² → h ≲ 1.6 ms  (and ≲ 0.4 ms for h_w = 0.2 m)

With `hmin = 0.1 s` **no step size that converges is reachable** for any fine cell on the
front: CVODE retries `MXNCF` times at the floor and returns `CV_CONV_FAILURE` — the exact
message observed, once per failed `CVode()` call, 128,081 times over 48 h of storm wetting.
Zero error-test failures is the expected signature: failures occur in the corrector before
the error test ever runs, and BDF's LTE at these h is tiny. The router then freezes the
window and halves it (`SurfaceRouter2D.cpp:989-1043`), so the storm becomes a
freeze/retry/re-freeze grind. This is the same mechanism behind the memory-banked Bellinge
finding that removing the 0.04 m² cells bought 3.34×: the kink amplitude is `∝ 1/A`, so
**the smallest cells set the convergence-critical step for the whole mesh**, and 0.1 s is
~2 orders of magnitude above their kink timescale. (Even the default
`MIN_TIMESTEP = 0.001` is marginal at 0.04 m²; note the earlier "MAX_CVODE_STEPS 60 advice
DANGEROUS at high rain" memory is the same trade seen from the other side.)

**Remedy (formulation-level, minimal, in order of leverage).**
1. **Never use `MIN_TIMESTEP` as CVODE's hard `hmin`.** Leave `CVodeSetMinStep` at 0 (or
   ~1e-6 s) and treat the input key as a *diagnostic* threshold (warn when h stays below
   it). A BDF integrator that wants 1 ms during front passage is doing its job; forbidding
   it converts slowness into 128 k failures plus dropped rain (F8).
2. **Smooth the upwind flip:** blend the conveyance depth over a small head band, e.g.
   `h_face = w(Δη)·h_up^{5/3} + (1−w)·h_dn^{5/3}` with a C¹ weight `w` transitioning over
   ~`flux_dh_eps` — one-line change in `computeEdgeFluxes` + exact mirror in
   `SurfaceTangent`. (Adopting `VFR_FACE` as default achieves most of this for free.)
3. **Smooth the V = 0 clamp:** replace `max(V,0)` with a C¹ softplus of area-scaled width
   (e.g. `V_ε = ε·A·dry_depth`), so `∂η/∂V` ramps 0 → 1/A instead of jumping; mirror in
   `dEtaDV`.

---

### F2. The analytic tangent is not the derivative of the RHS where it matters: clamp not mirrored, branches frozen, and lsetup-lagged — inconsistent exactly at wetting fronts

**Verdict: SUSPECT** (consistent in the smooth interior — verified against the flux term by
term — inconsistent on the kink set, which is where the corrector fails).

**Where tangent = true derivative (verified):** interior flux chain rule
(`solver/SurfaceTangent.cpp:210-246` matches `SurfaceFluxCalculator.cpp:382-422` including
`edge_conveyance`, upwind Manning n, both the Δη-path and the h_up-path), `regSqrtPrime`
(`:61-71`), `faceDepthPrime` (`:53-59`), `evapSinkPrime` (`:74-78`), boundary
NORMAL_FLOW/SPECIFIED_STAGE (`:97-155` — the header's claim of "local central difference"
at `SurfaceTangent.hpp:88-91` is doc drift; it is analytic), VFR `dEtaDV` via
`vfrDEtaDMeanDepth` (`:82-93`).

**Where it is not:**
1. **The V ≤ 0 clamp is not mirrored.** `dEtaDV` returns `1/A` under FLAT unconditionally
   (`SurfaceTangent.cpp:93`), but the true RHS is *constant* in V for V < 0
   (`CvodeSurfaceSolver.cpp:69`), i.e. the true row/column is exactly 0 there. The tangent
   thus carries spurious off-diagonals `∂F/∂V_nbr ≠ 0` for edges into clamped
   (negative-volume) cells — which is the state over-drained front cells sit in. FD J·v
   would have captured the clamp; the analytic path does not.
2. **Frozen branches** (`:195-197, 204-207, 226`): upwind selection, dry gate, face-depth
   branch and `sign(Δh)` are all evaluated once and held. That yields a valid *one-sided*
   generalized derivative (same as FD-Jv at a point), but during a front passage the
   corrector solution is on the *other* side of the branch, so the iteration matrix is the
   wrong element of the generalized Jacobian — with mismatch `∝ |h_L^{5/3} − h_R^{5/3}|`
   (F1), i.e. maximal at fronts.
3. **Staleness.** `jtsetup_fn` rebuilds the tangent only at linear-solver setups
   (`CvodeSurfaceSolver.cpp:696-716`), which CVODE lags (up to ~20 steps / until a
   convergence failure). The FD J·v it replaced was evaluated fresh at the current Newton
   iterate on every Krylov vector. During quasi-steady flow this is free performance; during
   front propagation the analytic path linearizes about a state up to many steps old.
   Recovery exists (failure → `jok=SUNFALSE` → rebuild) but each recovery *is* a failed
   corrector attempt — at `hmin` these become the logged errors.

**Multiscale angle:** all three gaps scale like the kinks they shadow — `1/A` on the fine
cells — so tangent error concentrates on exactly the cells that already bound convergence.

**Remedy.** (i) Mirror the clamp: pass V (or a `clamped` flag) into `dEtaDV` and return 0
for V ≤ 0 under FLAT (and the ε-tail slope under VFR — already bounded there). (ii) After
smoothing per F1, the branch-freeze issue mostly evaporates (one-sided = two-sided for C¹
constructs). (iii) During wetting-active periods, force tighter lsetup cadence (the
`OPENSWMM_2D_LSETUP_FREQ` hook already exists, `CvodeSurfaceSolver.cpp:600-601`) or A/B
`OPENSWMM_2D_JACOBIAN=fd` on the failing mesh — if FD markedly reduces `nncfails`, the
tangent gaps (not the RHS kinks alone) are confirmed as a primary driver.

---

### F3. Both preconditioners are built from a secant transmissivity that omits the dominant front-region Jacobian term (∂F/∂h_up) — GMRES quality collapses exactly during wetting

**Verdict: SUSPECT** (adequate in near-level ponding, structurally wrong at fronts and on
steep slopes).

**Evidence.** Jacobi (`CvodeSurfaceSolver.cpp:303-337`) and the AMG matrix
(`solver/SurfaceJacobian.cpp:70-91`, doc `:9-21`) both use only
`T_e = |F_e| / max(|Δη|, 1e-9)` — the secant of the Δη-dependence. The true Jacobian has a
second, *nonsymmetric* term through the upwind depth,
`∂F/∂V_up = C·(5/3)·h_up^{2/3}·Φ(Δη)/A_up`. Ratio of omitted to included term:

```
(∂F/∂h_up · dh/dV) / (T·dη/dV) = (5/3)·Δη / h_up
```

At a front or on a steep graded slope Δη is decimetres-to-metres while h_up is
centimetres → the preconditioner omits a term **10–100× larger** than the one it keeps, and
puts nothing in its (asymmetric, upwind-column) position. `M = I − γJ` is assembled
near-symmetric (`SurfaceJacobian.hpp:9-21`) while the true front-region J is strongly
nonsymmetric. With SPGMR capped at `MAX_KRYLOV_DIM = 30` (no restarts configured,
`CvodeSurfaceSolver.cpp:625`) and one BoomerAMG V-cycle per apply
(`HypreAmgPreconditioner.cpp:81-84`, hierarchy additionally lagged `:93-99`), the linear
solve goes inexact exactly when Newton is already fighting F1/F2 — linear-solve
non-convergence is folded into the same corrector-failure/step-reduction path and the same
logged error.

**Multiscale angle:** the `dη/dV = 1/A` scaling gives the matrix rows a 4–5
order-of-magnitude scale contrast between 0.04 m² and 10³ m² cells; BoomerAMG
(strong-threshold 0.25, aggressive coarsening) tolerates contrast, but only if the entries
are *right* — the missing upwind term is largest precisely on the fine-cell rows.

**Remedy.** Assemble the preconditioner matrix from the **already-computed analytic
tangents** (`SurfaceTangents.dfdvi/dfdvnbr/diag` have exactly the right sparsity — diagonal
+ 3 neighbours) instead of the secant heuristic: one source of truth for J·v *and* P, the
nonsymmetric front term included for free, and the FLAT/VFR chain-rule handling
(`CvodeSurfaceSolver.cpp:258-277`) becomes redundant. This is a small, contained change in
`psetup_fn`/`SurfaceJacobian::assemble`.

---### F4. Error control: a 1 µm depth-equivalent atol (1000× below DRY_DEPTH) and a median-based A_ref that is fragile on a bimodal urban/rural mesh

**Verdict: SUSPECT** (the *structure* atol_i = depth_tol × area is right; the default level
and the AUTO reference are wrong for this mesh class).

**Evidence.** `atol_i = ABS_TOLERANCE · max(A_i, √(A_i·A_ref))` with
`ABS_TOLERANCE = 1e-6 m` and A_ref = the **median** cell area
(`CvodeSurfaceSolver.cpp:549-576`; `SolverOptions2D.hpp:196-210`). Physical meaning per
cell: control the mean-depth error to `1e-6·max(1, √(A_ref/A_i))` metres, with
`rtol·|V_i|` adding a *relative-in-depth* term `1e-4·h̄_i` — that structure is sound and
the WRMS norm is well-conditioned in double precision.

Three problems at multiscale:
1. **Level:** 1 µm of depth is 1000× below `DRY_DEPTH` (1 mm) and far below any physical
   signal in overland flow. CVODE's nonlinear convergence test requires the corrector to
   beat ~0.1× the WRMS tolerance — so Newton must resolve a *kinked* residual (F1) with an
   inexact, mis-preconditioned linear solve (F3) down to sub-micron depth increments. This
   is how a run produces a corrector-failure storm with **zero error-test failures**: the
   step controller is perfectly happy; the corrector target is unreachable.
2. **Median A_ref on a bimodal mesh** sits inside one mode. If fine urban cells dominate
   the count (median ≈ 1 m²), a 0.04 m² cell gets `√(0.04·1) = 0.2` → depth-equivalent
   tolerance 5e-6 m — still micron-scale, i.e. the floor barely helps the cells that need
   it; the rural mode is irrelevant to it. If coarse cells dominate, the lift is large but
   arbitrary. The floor's effect is an accident of the area histogram, not a physical
   choice.
3. `rel_tolerance` means "relative depth error per cell" — defensible — but on a 10³ m²
   cell `rtol·V` corresponds to 1e-4·h̄ of depth while the same rtol on a 0.04 m² cell is
   numerically ~1e-8 m³: the *nonlinear-solve* target on tiny cells is dominated by the
   (micron) atol.

**Remedy.** Make the tolerance *depth-based with a physical floor*:
`atol_i = A_i · max(ABS_TOLERANCE, κ·dry_depth)` with κ ≈ 0.01–0.1 (i.e. 10–100 µm), or
simply change the `ABS_TOLERANCE` default to 1e-4 m. Keep the √(A·A_ref) bridge if desired
but define `A_ref` physically (e.g. geometric mean of min/max area, or an explicit value in
the INP for engineered meshes) rather than the median of a bimodal histogram. Optionally
loosen only the *nonlinear* test during wetting via `CVodeSetNonlinConvCoef` (hook already
exists: `OPENSWMM_2D_NLCONV`, `CvodeSurfaceSolver.cpp:602-603`).

---

### F5. Default closure pair (FLAT cell + MEAN face) has O(cell-relief) head error — metres of phantom head on coarse graded cells, discharged into the fine mesh

**Verdict: INAPPROPRIATE-FOR-MULTISCALE as the default** (correct for the
uniformly-fine, deep-water urban meshes it was tuned on).

**Evidence.** FLAT reconstructs `η = z̄_c + V/A` (`CvodeSurfaceSolver.cpp:77-79`); the code
itself documents the bias — up to **2/3 of the cell relief** for a partially wet cell — as
the driver of the uphill-creep artifact (`SolverOptions2D.hpp:101-124`,
`mesh/VfrClosure.hpp:7-13`). On a 1 m² urban cell relief is centimetres and the bias is
harmless. On a 10³ m² rural cell on a 5 % slope the relief is **metres**: a thin film on
that cell reads as a free surface up to ~2/3·relief above reality, creating phantom Δη of
metres against its downhill (often fine) neighbours; combined with the MEAN face depth
(blind to where the waterline sits relative to the shared edge, `SolverOptions2D.hpp:127-146`)
the model discharges water across edges the real surface never reaches. The wetting *timing*
and *direction* at coarse→fine transitions — exactly the urban-margin cells the multiscale
design cares about — are systematically wrong, and every spurious discharge event is also an
extra F1 kink crossing. The C¹ fix exists (VFR + VFR_FACE, validated per the 2026-07-19
memory) but is opt-in, and its reported 3–8× step-count penalty is partly *caused* by F2/F3
(the preconditioner mis-scaling at fronts is called out in the code itself,
`CvodeSurfaceSolver.cpp:254-258`).

**Remedy.** Tie the default to the mesh, not the build: per-cell closure selection
`relief_i = z3−z1 > k·dry_depth ⇒ VFR` (the closure is already evaluated per cell; a
per-cell flag costs one byte) with FLAT retained for genuinely flat cells — this gives the
urban interior FLAT's speed and the rural cells VFR's correctness. At minimum, document
that multiscale meshes must run `CELL_CLOSURE=VFR FACE_RECONSTRUCTION=VFR_FACE`, and fix
F2/F3 so the VFR step-count penalty shrinks.

---

### F6. Two-point flux with barycentric Δx and no non-orthogonality correction: inconsistent gradient on graded unstructured meshes (and the "second-order" reconstruction never feeds the flux)

**Verdict: SUSPECT** (monotone and conservative — but not consistent, and the
discretization error concentrates in the coarse-fine transition bands).

**Evidence.** The face gradient is `Δη / |x_ci − x_cj|` along the centroid line, multiplied
by the full edge length, with **no projection onto the face normal**
(`SurfaceFluxCalculator.cpp:370-401`). This TPFA-style flux is consistent only when the
centroid line is (nearly) orthogonal to the face — true for quality quasi-uniform
triangulations, systematically false in grading bands where a 10³ m² cell abuts ~1 m²
cells: the centroid line is dominated by the big cell's radius and can be far from the face
normal, giving an O(1) directional error in the face gradient (the effective slope is
smeared over the big cell's half-diameter). Meanwhile the Green-Gauss + Jawahar–Kamath
limited gradients that the header advertises as the flux reconstruction
(`SurfaceFluxCalculator.hpp:4-10, 54-64`) are computed **only for output/API**
(`SurfaceRouter2D.cpp:42-48`; grep confirms no flux-path caller): the scheme is first-order
in space, and the header/doc claim is drift. On the plus side: the two incident cells
compute identical `(Δη, Δx, ξ, n_up, h_up, conveyance)`, so the flux is exactly
antisymmetric — **conservation is airtight** (`:365-422`), and TPFA's M-matrix sign
structure keeps it monotone (no spurious extrema), which is why the inconsistency shows as
wrong *rates/paths*, not oscillations.

**Remedy (minimal).** Scale the flux by the orthogonality cosine
`cosθ = |(x_cj − x_ci)·n̂_e| / Δx` (both cells compute the same value → antisymmetry
preserved) — a one-line correction that removes the worst over-prediction on skewed
transition faces. Longer-term: face-midpoint η reconstruction using the already-computed
limited gradients (with the tangent updated accordingly), or mesh-generation guidance
enforcing gradual size transitions (which also fixes F1's 0.04 m² tail — the single highest
ROI change of all, per the Bellinge re-mesh result).

---

### F7. No steep-slope safeguard on the diffusive-wave flux, and no moderation of the coarse→fine face-flux stiffness — everything is delegated to the implicit integrator

**Verdict: SUSPECT** (a legitimate MOL design choice, but unguarded at the extremes this
mesh class actually contains).

**Evidence.** The Manning-DW flux `F ∝ h^{5/3}√(Δη/Δx)` grows without bound in the slope
(`SurfaceFluxCalculator.cpp:394-401`); there is no Froude/celerity cap — the only clamp in
the module (`kQMax = 10 m²/s`, `:547,570-572`) applies to the *diagnostic* velocity
reconstruction, never to the flux. On steep faces (S ≳ 0.05–0.1, where DW's zero-inertia
assumption is itself at its validity edge) the scheme silently over-predicts front celerity.
The coarse→fine stiffness is exactly as the review question frames it: a 1 m²/0.04 m² cell
fed through a face sized by a 10³ m² neighbour has `dV/dt` sensitivity `∝ C·h_up^{5/3}/A`,
a time constant of milliseconds (see F1 numbers); nothing in the formulation — no face-area
limiting, no sub-stepping, no upwind-volume rate limit — moderates it. Delegating stiffness
to BDF is *correct* for a smooth RHS (L-stable BDF + Newton handles hλ ≫ 1 fine); it fails
here only because the RHS is non-smooth (F1) and the Newton machinery inconsistent (F2/F3)
— which is why the fix priority is F1–F3 and mesh grading, not an explicit stiffness
limiter.

**Remedy.** (i) Optional C¹ Froude-limited conveyance,
`q ← q·min_smooth(1, α·h√(gh)/q)`, mirrored in the tangent — bounds both the physics error
on steep slopes and the front-region stiffness in one smooth construct. (ii) Mesh guidance:
cap the neighbour-area ratio (e.g. ≤ 4:1) and eliminate sub-0.1 m² slivers; the
formulation's per-cell kink/stiffness scales are all `1/A`, so mesh quality is a
formulation-level parameter here, not cosmetics.

---

### F8. Failed-window handling silently discards the window's rainfall from the 2D surface — ledger-consistent but hydrologically wrong under a failure storm

**Verdict: SUSPECT** as a rare-event safety net; **INAPPROPRIATE** as the steady-state
behaviour it becomes when F1 makes failures routine.

**Evidence.** On a failed advance the surface is frozen, the integrator resynced
volume-exact, held 1D *exchanges* are un-booked/re-queued — but the window's **rain is
simply never applied and never carried forward**: `accumulateMassBalance` is skipped
(`SurfaceRouter2D.cpp:1155-1159`), the state never received the volume, and unlike the
coupling accumulators (`:1015-1027`) there is no re-queue for rainfall. The ledger closes
(the rain is never booked as inflow), but the watershed physically received that rain and
the simulation loses it. With the F1 failure storm concentrated *in the storm peaks*, the
lost volume is biased to exactly the hydrologically critical intervals; only a first-failure
warning and a window count are reported (`:990-999, 1243-1250`) — the dropped *volume* is
never quantified. The window-halving guard (`:1033-1036`, floor 1 ms) makes many small
failed windows possible, compounding silently. Rainfall being **held constant per window**
(`fireAdvanceWindow` → `updateRainfall`, `:845-877`) is itself fine — the window is
~ROUTING_STEP (`:691-706`), well below gage resolution, and a constant source is
Newton-transparent; the defect is purely the failure path.

**Remedy (minimal).** Carry un-landed forcing forward: on a failed window, accumulate
`Σ A_i·rain_i·dt` (per cell) into a pending-source buffer added to the next fired window's
forcing — conservative, local, no solver change; report the cumulative
re-delivered/dropped volume in the run summary. The cleaner semi-discrete treatment — rain
as a time-continuous (piecewise-linear in t) term in the RHS with the window as a pure
reporting boundary — removes the concept of "un-landed" forcing entirely and is
CVODE-friendly (BDF handles time-dependent source terms natively; discontinuities at gage
breakpoints are handled today by the window restarts anyway). But with F1 fixed, failures
return to being rare and the existing net + carry-forward is acceptable.

---

### F9. Smaller formulation-level observations

1. **Doc drift, load-bearing:** `SurfaceFluxCalculator.hpp:4-10` (and the strategy-doc
   equation references) describe a second-order limited-reconstruction flux; the
   implemented flux is first-order two-point (F6). Anyone tuning `LIMITER_EPSILON`
   expecting flux impact is turning a dead knob (it affects output gradients and the
   active-set seed pass only).
2. `SurfaceTangent.hpp:88-91` says boundary tangents come from a "local central
   difference"; they are analytic (`SurfaceTangent.cpp:97-155`). Harmless, but confusing
   during Jacobian debugging.
3. **SPECIFIED_STAGE inflow depth under MEAN** uses `max(h_bc − z̄_c, 0)`
   (`SurfaceFluxCalculator.cpp:162-165`) vs the cell-mean depth on outflow — a kink of the
   F1 family at Δh = 0 on stage boundaries (tangent mirrors it one-sidedly,
   `SurfaceTangent.cpp:123-151`). Same remedy as F1-2.
4. **Conservation audit: clean.** Interior antisymmetry (`:365-422`), mirrored per-edge
   conveyance (`:406-420`, mirrored at `SurfaceRouter2D::drainPendingRows`), volume-state
   telescoping with no 1/A in the flux gather (`:442-460`), the active-set wall guard
   (`:325-334`), and the failed-window volume-exact resync
   (`CvodeSurfaceSolver.cpp:888-910`) are all correct; no sign errors or asymmetric
   treatments found in the 2D interior.
5. The IMEX split (`assembleImplicitRHS`/`assembleExplicitRHS`, `:464-504`) reproduces
   `assembleRHS` exactly, including the local depth reconstruction in the explicit half —
   correct.
6. `evapSink` at `state.depth` inside `assembleRHS` (`:456-459`) uses the *unpacked
   current* depth — consistent with the tangent's `evapSinkPrime` diagonal. Correct.

---

## Question-by-question coverage map

| Q | Answer (finding) |
|---|---|
| 1. RHS smoothness | F1 (kink inventory + which are scale-dependent: upwind flip and V=0 clamp scale as 1/A; regSqrt/faceDepth/evap ramp are C¹ and benign; forcings/table lookups are held constant → state-independent) |
| 2. Jacobian consistency | F2 (clamp not mirrored; frozen branches = one-sided generalized derivative; lsetup lag). Diagnosis: **both** apply — non-smooth RHS is the root, tangent+preconditioner inconsistency (F2/F3) removes the machinery that could have limped through it |
| 3. Scaling/tolerances | F4 (structure sound, level 1000× too tight, median A_ref fragile on bimodal meshes; depth-based per-cell tolerance with a `dry_depth`-anchored floor is the sounder formulation) |
| 4. DW closure on varying slopes | F6 (TPFA inconsistency, no non-orthogonality correction, limiter never feeds flux), F7 (no steep-slope safeguard — silently wrong at DW's validity edge), F5 (FLAT closure relief-scaled bias on coarse cells) |
| 5. Coarse↔fine face flux | F6/F9-4 (conservative and antisymmetric — yes), F7 (nothing moderates the small-cell time constant; stiffness fully delegated; acceptable only once F1–F3 are fixed) |
| 6. Wet/dry propagation | F1 (wetting = one discrete kink crossing per cell, magnitude ∝ 1/A, count ∝ front length; DRY_DEPTH/LIMITER_EPSILON are *not* in the flux path — the flux gate is `depth_up ≤ 0` + h^{5/3}, smooth; the non-smoothness lives in the upwind flip and the V=0 clamp) |
| 7. Macro-window forcing | F8 (held-per-window rain: acceptable; failure-path rain-dropping: not, under a failure storm; carry-forward or time-continuous forcing) |
| 8. Anything else wrong | F9 (doc drift, stage-BC kink; conservation audit clean) |

---

## Top 3 causes of the Newton-corrector failure storm (best-evidence diagnosis)

1. **Non-smooth wetting events amplified by 1/A on the fine cells, colliding with
   `MIN_TIMESTEP 0.1` used as CVODE's hard `hmin`.** Every cell that wets crosses the
   upwind-flip and V=0 kinks; the Newton contraction bound `γ·ΔJ < 1` requires
   h ≲ 1.6 ms on the 0.04 m² cells (h ≲ 0.04 s even at 1 m²), while the floor forbids
   anything below 0.1 s. The corrector *cannot* converge during front passage, CVODE
   retries at the floor and emits exactly the observed `CV_CONV_FAILURE` message —
   thousands of front-cell events × retries ⇒ 128,081. Zero error-test failures is the
   fingerprint: the failure is in the corrector, before error control ever runs.
   (`SurfaceFluxCalculator.cpp:339,390-401`; `CvodeSurfaceSolver.cpp:69,586`)

2. **The Newton machinery is inconsistent precisely on the kink set.** The analytic
   tangent (default-on) freezes branches, omits the V≤0 clamp (spurious 1/A entries on
   over-drained front cells), and is lagged to lsetup; the AMG/Jacobi preconditioner is
   built from a secant that omits the upwind-depth term — 10–100× the retained term at
   fronts — and symmetrizes a strongly nonsymmetric front-region Jacobian, so the
   30-vector GMRES solve also degrades there. Even at step sizes where a consistent
   Newton would converge, this one often doesn't — each miss is another logged failure.
   (`SurfaceTangent.cpp:93,195-197`; `SurfaceJacobian.cpp:78-88`;
   `CvodeSurfaceSolver.cpp:313-336,625`)

3. **A micron-depth nonlinear convergence target.** `ABS_TOLERANCE 1e-6 m`
   (depth-equivalent, 1000× below DRY_DEPTH), with the median-based `ATOL_AREA_REF` floor
   providing little relief in the fine-cell mode of a bimodal mesh, sets the corrector
   acceptance at ~0.1 µm-scale depth increments. Resolving a kinked residual through an
   inexact, mis-preconditioned linear solve to that target is what turns "slow
   convergence" into "declared failure" — and why the step-size/error controller shows
   nothing wrong. (`CvodeSurfaceSolver.cpp:549-576`; `SolverOptions2D.hpp:196-210`)

**Fastest-payoff sequence:** (1) stop passing `MIN_TIMESTEP` to `CVodeSetMinStep` (or set
it ≤ 1e-4 s) and raise `ABS_TOLERANCE` to 1e-4 m — configuration-level, should collapse the
failure count immediately; (2) build the preconditioner from the analytic tangents and
mirror the V≤0 clamp in `dEtaDV`; (3) smooth the upwind flip / adopt VFR_FACE+VFR (per-cell
by relief) as the multiscale default; (4) re-mesh away the sub-0.1 m² sliver tail and grade
coarse→fine transitions — every failure mechanism above scales with 1/A_min.
