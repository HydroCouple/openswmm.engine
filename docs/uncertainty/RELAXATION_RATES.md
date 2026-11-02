# Per-mode relaxation rates (H18) — design note

Status: design fixed 2026-10-13; **the default since 2026-10-16** (A.5). K1d,
its scale knob and the advective hook are deleted. Rain and other additive
uncertainties do not use these modes: they are carried at full order
(`ADDITIVE_CHANNEL.md`, H19), after §3's placement of the kinematic term proved
wrong for them (correction box below). Validation: VALIDATION.md "H18", "H19", "A.5".

## 1. The problem in one paragraph

The 1D ROM gives every mode j the rate λ_j·K1d/mm, where λ_j is an eigenvalue of a
mean-1-normalized (dimensionless) Laplacian and K1d is one network-wide number,
mean(PHI·h^{5/3}/(2n√S)/L²). One number cannot serve a network that mixes steep
free-surface laterals (disturbances leave in seconds, carried by the flow) with deep
pools (disturbances drain in hours). Under dynamic wave a pool in the test chain pulls
K1d up and the cells pass; under FLOW_ROUTING FV the pool never forms, K1d falls 19×
(0.0038 against 0.073), and the surcharged and front cells read 2×–8× wide
(VALIDATION.md "H17 §12"). Where each mode settles, (mm − 1)·φ, is already right and
does not depend on the operator. Only the speed is wrong.

## 2. The fix

Give each mode its own rate in physical units (1/s) from the conductance and storage
of the nodes it covers, through the generalized eigenproblem

    L_c v = λ S v

- L_c: a symmetric conductance Laplacian (ft²/s, i.e. cfs per ft of head).
- S: a diagonal node storage (ft²).

λ = (conductance) / (storage) is the textbook rate at which a level bump fades. The
problem is solved in the symmetric form

    S^{-1/2} L_c S^{-1/2} w = λ w,     v = S^{-1/2} w,

so the existing Lanczos solver (`GraphEigenBasis`) is reused unchanged. The basis is
S-orthonormal: PᵀSP = I. To keep the solver's absolute `null_tol` (1e-8) meaningful,
the scaled matrix is divided by ρ = mean(c_e)/mean(S) before the solve, and the
returned eigenvalues are multiplied by ρ afterwards. The kept modes are the k slowest.
That is the right choice for mode acceleration: fast modes are quasi-static and are
carried by the residual term (H17 §10).

## 3. Conductance per edge (decided)

Each basis edge (conduit or structure, the same edge list `rom1dRegime()` returns)
contributes a symmetric head part and a one-sided part.

**Conduits.**

- **Symmetric head-gradient part:** c_h = relay · ½·dQ/dH_Picard. dQ/dH_Picard is the
  dynamic-wave conductance per barrel, g·dt·A·nb/(L(1 + dt·g·n²|v|/(PHI²R^{4/3}))).
  Under dynamic wave it is read from the snapshot (`off/dt`); under other routing
  methods `rom1dRegime()` derives it from state. Why this and not the flow-response
  q/(2|Δh|):
  - In the friction limit ½·dQ/dH_Picard equals q/(2|Δh|) exactly: Picard is the
    secant, which is twice the Manning tangent. So where Manning governs, the two
    agree.
  - It stays finite at zero flow (gAdt/(2L)) and at zero head drop. The flow-response
    form is 0/0 or unbounded there (it is floored at |Δh| = 1e-4 ft, which can give
    10³ ft²/s). An unbounded entry widens the spectrum and slows Lanczos convergence
    of the smallest modes.
  - It is the conductance the routing solver itself uses, so still water stays
    connected (water levels on two sides of a still pipe do equalize). **This is the
    no-flow floor the plan asked for, and no extra constant is introduced.**

  The relay is the Froude ramp already used by the flow response (1 − upstreamGate(Fr)).
  A supercritical reach has no head coupling, the same rule the target uses.
- **One-sided kinematic part:** c_k = q · K′(y_up)/K(y_up). This is the flow-response
  kinematic term, so a steep free-surface pipe relaxes at (5/3)·v/L. It goes on the
  **upstream node's diagonal only**. The true Jacobian has it in the upstream row and,
  with the opposite sign, as a transfer into the downstream row. Keeping the upstream
  row exactly and dropping the transfer keeps the operator symmetric (so stable). It
  also keeps the H14 rule that a downstream deviation never reaches upstream through a
  free-flowing reach. What is lost is the transient arrival of an upstream deviation
  downstream. The target φ already carries the steady propagation, so only timing is
  affected.

> **Correction (2026-10-13, measured).** The last sentence holds for the Manning
> channel only, because it has a target (φ). Rain and the other additive forcings
> have no target: their steady state comes from the operator itself. With the
> transfer dropped, a node receives only its own extra inflow, not the extra flow
> from every node upstream. SR-5's J5 (next to the outfall) fell to 0.3–2 % of the
> Monte Carlo spread. Putting the kinematic term on a symmetric edge instead fixes
> rain (SR-5 0.879 / 0.949, CL-1e 0.880 / 0.960). It breaks the Manning cells,
> though: DW `EXPLICIT` diverges (1e216), and `SEMI_IMPLICIT` and FV surcharged turn
> conservative. The fix is to give additive forcings a flow-response target too
> (VALIDATION.md "H18" §10).

**Structures** (pump, orifice, weir, outlet). While a structure passes flow, its
|dQ/dH| goes on the upstream node's diagonal. This is the same "upstream only" rule
the flow-response target uses for free structures and H14 uses for pumps. A structure
passing no flow couples nothing.

**Grounding.** An edge whose other end is an outfall adds its total (c_h + c_k, or the
structure's |dQ/dH|) to the active end's diagonal, as `NetworkLaplacian1D` already does.

**Floor.** c_e ≥ 1e-6 · mean(c_e) on every conduit edge. This is the same relative
floor `updateBasis()` applies today. Only a dry pipe (A = 0) reaches it, and it keeps
the graph connected so dry nodes form slow modes instead of null ones.

**Per-member scaling.** Every conductance above is ∝ 1/n except the inertial cap of
c_h. The member rates scale as 1/mm, the same structure as today. The fixed point
(mm − 1)·φ is therefore unchanged and still independent of the operator
(unit-tested).

## 3.1 Port-Hamiltonian reading (2026-11-01)

Write the 1D operator as Q dψ/dt = (J − R) ψ, where J is the skew part
(conservative transport) and R is the dissipative symmetric part. For the Froude-gated
edge (H14), per edge w·[[1, −(1−g)], [−1, 1]] has symmetric part with eigenvalues
w(1 ± (1−g/2)) ≥ w·g/2 ≥ 0 and skew part w·g/2·[[0, 1], [−1, 0]]. R = symmetric
conductance + kinematic upstream diagonal + grounding, all ⪰ 0. With the S-orthonormal
basis, sym(PᵀLP) = Pᵀ·sym(L)·P ⪰ 0, so the reduced operator is energy-stable by
construction. See `test_spectral_rom1d.cpp` (`DirectionalOperator.ReducedDirectionalOperatorIsEnergyStable`)
and PORT_HAMILTONIAN.md §2.1.

## 4. Storage per node (decided)

    S_i = max( node::getSurfArea(i, depth_i)
               + Σ_{attached conduits} ½ · W(y_e) · L_e · nb_e ,
               options.min_surf_area )

- getSurfArea is the storage curve (0 for plain junctions).
- W(y_e) is the top width at the conduit's published depth, so a full closed pipe
  contributes 0 and a surcharged region relaxes at pressure speed.
- This is the dynamic-wave solver's own surface-area rule (legacy `findSurfArea`),
  evaluated from published state so FV gets the same.

## 5. Consequences for every projection (decided)

Every field the ROM receives is in head or head-rate units:
- φ, the Manning target (ft);
- dh/dt runoff, soft-rain location and spread, and FORCING_VECTOR fields (ft/s).

Galerkin on S·dh/dt = … with h = h_det + P·a therefore gives one rule: **projection is
PᵀS·field**. This applies to:
- b (Manning target);
- r (runoff);
- both soft-rain planes;
- the CL-1b/CL-2c per-member projections (S folded into the spread field);
- FORCING_VECTOR fields.

The quasi-static residual becomes r = φ − P·PᵀSφ, the S-orthogonal complement.
Reconstruction stays h = h_det + P·a, and the phase coordinate (H11) is untouched. A
basis rebuild re-projects coefficients with R = P_newᵀ S P_old. The warm start passes
S^{1/2}·P_old, because the solver works in w-coordinates.

Implementation: keep P for reconstruction and add a projection matrix Q = S·P (each
column scaled by S). With S absent, Q = P, which is bit-identical to today.

## 6. Rates between rebuilds (decided)

The basis is rebuilt at most every 60 s (existing cadence and the H1 cold-restart
triggers). Today K1d is recomputed every step. To keep rates equally current, each
step uses the generalized Rayleigh quotient on the frozen basis:

    λ_j(t) = P_jᵀ L_c(t) P_j / P_jᵀ S(t) P_j

This costs O(edges·k) per step, about 2·10⁴ flops on Bellinge. Right after a rebuild
it equals the eigenvalue. Projections keep S frozen at its rebuild value
(S_basis), so PᵀS_basisP = I holds exactly between rebuilds. The engine passes these
rates through `SpectralROM1D::setModeRates()` and calls advance() with K1d = 1. The
diagonal integrator, the mode-activation test and the Manning forcing −λ(1/mm − 1)b
all read the per-mode rates.

## 7. The one-way operator (H14)

When per-mode rates are on, H14's reduced operator becomes
M = PᵀL_c,dir(t)P, in 1/s with K1d = 1. Here L_c,dir is the same conductance Laplacian
with the upstream row's coupling removed on gated conduits. Ungated, M = diag(λ(t)) up
to the Ritz residual, so the two paths agree. Whether H14 still helps once the
kinematic part sits on the upstream diagonal is decided by A.4 (a run with
`rom1dDirectionalConfig().enabled = false`), not here.

## 8. Risks to measure in A.4

1. **Lanczos convergence.** S spans 12 ft² (manhole) to 10⁴ ft² (pond), and c spans
   inertial caps of 10 ft²/s to 10⁻³ for a nearly dry pipe. The fixed Krylov budget
   (3k + 15) may leave the slowest Ritz values unconverged. Report the Ritz residual
   ‖L_c v − λSv‖/‖λSv‖ and the off-diagonal weight of PᵀL_cP on Bellinge. The
   per-step Rayleigh rates are consistent for any S-orthonormal P even when
   unconverged; the off-diagonal part is what the diagonal path drops. Fallback if
   poor: a larger budget, or shift-invert with a sparse Cholesky of L_c + σS
   (tree-like networks factor cheaply). Not built unless needed.
2. **Mode waste on dry or still nodes.** These nodes are slow, so they are selected
   first, but they carry no target and no forcing. Report how many kept modes are
   localized on nodes with zero φ.
3. **Rebuild churn.** Storage changes with depth, but H1's triggers read the snapshot
   conductances and surcharge flips, not S. Report rebuilds per hour under each
   routing method against today.
4. **The inertial cap depends on dt.** It only binds where flow is slow, which is where
   φ is small. Report whether any calibrated cell moves when the routing step is
   halved.

## 9. Not changed

- The target φ, the quasi-static residual's role, the floor at min(invert, h_det), the
  validity flags, the phase coordinate, soft-rain coherence, and the 2D ROM.
- `computeK1d()` stays in use while the hook is off. If A.4 accepts H18, K1d, the
  advective hook and `rom1dK1dScale()` are deleted in A.5.
