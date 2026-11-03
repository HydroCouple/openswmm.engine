# Per-member stores and ports (task card O-1, PORT_HAMILTONIAN.md phase 5)

> F-1 (2026-11-01): accepted with the ℓ1 contraction as the stability notion for the per-member solve, (b1) frozen
> pump setting, monotone envelopes and the regularised raised-end law; (b2) deferred. See PORT_HAMILTONIAN.md §2.4.

Status: DESIGN, 2026-11-01. Nothing here is implemented. O-2 implements it behind
`rom1dStorageMembersEnabled()`; the hook is deleted when phase 5 lands.
Replaces: `SWMMEngine::stepRom1dStorageMembers` (ADDITIVE_CHANNEL.md §7).
Read with: PORT_HAMILTONIAN.md, ADDITIVE_CHANNEL.md, RomAdditiveChannel.hpp.

**Headline.** The plan's argument ("each port has (h_up − h_dn)·Q ≥ 0, so a quadratic
energy decreases") does not hold as stated. In absolute heads every gravity port is passive and
a pump is a bounded active port. In deviation form, ½ yᵀ S y is NOT monotone for one-way ports
(weir, free outlet, supercritical pipe, pump; counterexample §2.2). What holds for every port
below is **ℓ1 volume contraction** (order-preserving + conservative; the same property the shared
planes already rely on). On that reading: (a) the two-way raised-end pipe with a per-member far
node **closes**, after correcting a phantom flow in today's law (§1.3); (b) rule-switched pumps
**close within a step** with a frozen per-member setting but **not across switching** (§2.5).
Two further monotonicity breaks are named (non-monotone flow brakes, circular conveyance near
full, §2.3). The change of storage function and (b2) go to **F-1** (§7).

---

## 1. Formulation

### 1.1 Unknowns and stores
For member i, write z_i for the head deviation from the main run h* (ft, internal
units) on every active node, so the member's head is h* + z_i. Today the ROM is
handed z_i = κ_i·Ψ + C_i (planes plus correction, `setMemberAdditive`). Keep that
contract. What changes is how C_i is obtained (§5).

The node set splits into:
- **A, admitted stores.** Storage basins with a threshold outlet, nodes at the
  raised end of such a basin's pipe, and coupled manholes near spill (the same
  admission as today, §4).
- **N, linear nodes.** Everything else.

Each node a ∈ A is a store with a nonlinear volume

    V_a(h) = ∫_{invert}^{h} A_a(s) ds,
    A_a(s) = node surface area (storage curve, or MIN_SURFAREA for a junction)
             + ½ Σ_{attached conduits} W(s − z_end)·L_c·n_barrels

This is SWMM's own surface-area function: the one DW uses, the same S as H18
uses at the main run, but evaluated at the member's level and not frozen at
h*. Floor: A_a(s) ≥ MIN_SURFAREA, so V_a is strictly increasing on
[invert, rim + sur].

Each node in N keeps the linear store S_t·z (`rom1d_h18_S_`).

### 1.2 Ports (links)
Each link l is a two-port with flow Q_l(h_up, h_dn), counted positive from the
port's "up" end. Every port used at an admitted node must have three properties:

    (P1) ∂Q/∂h_up ≥ 0, ∂Q/∂h_dn ≤ 0   (monotone; as a graph where it jumps)
    (P2) what leaves one end enters the other (conservation)
    (P3) Q(h*) is the anchor: the member uses δQ_l = Q_l(h*+z) − Q_l(h*)

| Port | Law Q(h_up, h_dn) | Source today |
|---|---|---|
| Structure: weir, orifice, outlet | Q_s(h_up), from a table of `flowAtShiftedDepth` at 17 levels y = Y·t\|t\| about h*, linear in between; downstream held at h* (a known limit: no member-level submergence) | kind 1 |
| Conduit, free end | q0 · K̃(y_up)/K̃(y0) · sqrt(H/H0), with H = h_up − max(h_dn, z_dn,end) and K̃ the monotone envelope of conveyance (§2.3). Supercritical (relay off): q0·K̃/K̃0 | kind 0 |
| Conduit, drowned or no threshold | the plane's line ju·z_up + jd·z_dn, with ju ≥ 0 ≥ jd | kind 3 |
| Raised-end pipe, two-way (basin B ↔ far node F) | §1.3 | kind 6 / 5 |
| Dry pipe out of a store | normal-flow rating Q_n(y_up), one-way downhill (exact here; the plane keeps its secant) | dry edge |
| Depth-rated pump (PUMP2/4 with y_on/y_off) | `pumpFlowAtDepth`(d_up)·r(d_up), with r a dry ramp from 0 to 1 over the bottom δ_dry = 0.01 ft | kind 4 + `pump_bal` |
| Rule-switched pump | §2.5 | excluded today |
| Street exchange (coupled manhole) | the orifice c·sqrt(2g)·sign(Δ)·s(\|Δ\|), s regularised below δ0, between the manhole store and its member pond store a_pond·p (§1.5, O-3) | `orificePortFlow` + `PondStore` (was `couplingOrificeQ`, deleted) |
| Rim overflow | Q_ovf(h) = 0 below rim + sur; above it, flooding loss (or ponding) | the clamp `y_ceil` |

### 1.3 Raised-end pipe (corrected)
B has its pipe end at z_B; F has its pipe end at z_F > z_B (raised). With
s(x) = sqrt(max(x, 0)) and K = PHI/n · A_f · R_f^{2/3} · nbar / sqrt(L):

    Q_BF = K · [ s(h_B − max(h_F, z_F)) · 1{h_B > z_B}  −  s(h_F − max(h_B, z_B)) · 1{h_F > z_F} ]

Water leaves a node only if it stands above that node's pipe end, and drives
against the higher of the far water and the far end.

Today's kind 6 law, K·sign(Δ)·sqrt|Δ| with Δ = max(h_B, z_B) − max(h_F, z_F),
is a function of the two clamped heads. When both nodes are below their ends it
gives a constant ±K·sqrt|z_B − z_F|. The anchor (P3) cancels that constant
while the main run and the member sit in the same regime. But a member below
z_B whose main run is above z_B keeps draining at K·sqrt(z_B − max(h_F, z_F)),
water the member does not have. That is a phantom outflow, and the bound
y_floor is what hides it. (It matters only where the basin's own pipe end sits
above its floor; with z_B = invert the floor clamp coincides with the law.) The corrected law is monotone in each argument and
jumps at h_B = z_B (and at h_F = z_F). Treat the jump as a monotone graph (§3).

### 1.4 Per-member balance (backward Euler)
For every node t (store or linear), with δq_i,t = Σ_p κ_i,p q_p,t (the planes'
sources):

    V_t(h*_t + z_t^{n+1}) − V_t(h*_t + z_t^n) − [V_t(h*_t^{n+1}) − V_t(h*_t^n)]
        = Δt · ( Σ_{l into t} δQ_l − Σ_{l out of t} δQ_l + δq_i,t )

For t ∈ N the left side is S_t (z^{n+1} − z^n). The bracket removes the main
run's own volume change, because z is a deviation. At a store it makes the
member's balance exact in volume, not in the frozen-area approximation of
today. Each link appears exactly once, as either a nonlinear port or a plane
edge (§5). Write the whole system as

    F(z) = 𝒱(z) + Δt·G(z) − b = 0,

where 𝒱 is the vector of volume deviations (strictly increasing in each
component) and G_t is the net extra outflow at t.

### 1.5 Street pond and orifice (O-3, 2026-11-02)
The street pond over a coupled manhole is a store of the per-member solve, not
integrated inside the port law. Unknown: the pond depth's deviation z_p from the
main run's pond depth p* (ft), volume a_pond·z_p over the 2D ROM's footprint
(`SpectralROM::pondArea`). The orifice is an ordinary two-port manhole -> pond:

    Q = flooredFlow( c·sqrt(2g)·sign(Δ)·s(|Δ|),  ramp(d_mh),  ramp(p) ),
    Δ = h_mh − (z_or + max(p, 0)),   s = sqrtReg with δ0 = 0.03 ft,

c = Cd·cd_mult_i·A. Monotone in both ends, passive (Q·Δ ≥ 0), slope ≤ c·sqrt(2g)/sqrt(δ0)
(finite Jacobian), an empty pond gives nothing back. The main run's reference is the
same law at (h*, p*) (no outside q*), so the dry factors are absolute (ramp of the
pond's own depth), not relative like `dryFactor`. Per member and step: z_p^n is read
from the 2D ROM (`memberCouplingDepth` − p*); the pond's only port in the step is the
orifice; its volume change −a_pond·(z_p − z_p^n) is booked to the 2D ROM
(`setExternalCouplingVolume`) and held there per cell (`coupling_resid_`/coupling
volume over `pond_cells_`). It equals dt·(port flow) to the Newton tolerance on the
pond equation (1e-10 ft of pond level), not to 1e-10 relative.

Where the deviation form shows: p* + z_p can end below 0 for a member whose pond gains
less than the main run's over the step (the main run's pond itself rose); its own
water is still ≥ 0. Hence the clamp max(p, 0) in Δ (the street level is never below
the ground).

What stays on the 2D side and why: the coupling residual (fix 8) is the member pond's
state between steps (the pond store lives one step); `reconstructHeadUnpinned` and the
2D ROM's own implicit orifice in `applyCouplingFlux` serve coupling points whose manhole
is not admitted (no external volume, NaN entries), and the whole non-per-member path;
the ROM pin mask (`setPinnedNodes`) keeps spilling non-admitted nodes at the main run.
Removed in the per-member path: the nested 60-step bisection (`couplingOrificeQ`) and
its "empties the pond" floor, replaced by the pond store's dry factor.

## 2. Passivity

### 2.1 Absolute hydraulic energy
Take H = ρg Σ_t ∫ s dV_t(s) (potential energy). Then dH/dt = −Σ_l Q_l (h_up − h_dn)
+ (ports to outfalls and sources). Every gravity port has sign Q = sign(h_up − h_dn),
or Q = 0: weir, orifice, outlet, conduit, the corrected raised-end pipe
(Q_BF > 0 needs h_B > max(h_F, z_F) ≥ h_F), and the orifice exchange. So each
term is ≥ 0 and the gravity network is passive.

The kind 6 phantom of §1.3 violates this: it gives Q_BF > 0 with h_B < h_F. A
pump has Q ≥ 0 at any lift, so Q·(h_up − h_dn) < 0 when it lifts. It is an
active port, bounded by Q_max·(rim_dn − invert_up). This settles absolute
stability (bounded energy input). It is not the property the deviation step
needs.

### 2.2 Deviation form: the quadratic energy does NOT close
Take the one-way port δQ = d·z_up (weir, free outlet, supercritical pipe, pump)
between two linear stores S. Then S ż = −L z with L = d·[[1, 0], [−1, 0]], and
sym(L) = d·[[1, −½], [−½, 0]] has the eigenvalue d(1 − √2)/2 < 0. So
½ zᵀ S z can grow: the downstream node receives energy it has no diagonal to
dissipate.

PORT_HAMILTONIAN §2.1 closes for the 1D modal operator only because H14 keeps
the downstream node's own diagonal. A one-way structure edge in the additive
channel has none. **So "monotone ⇒ (Δz)·δQ ≥ 0" is false for ports whose flow
does not depend on h_dn.** Say this in the code comments. Do not test it.

### 2.3 What does close: ℓ1 volume contraction
Under (P1) and (P2):
- G is off-diagonally antitone: raising z_k (k ≠ t) can only raise inflow to t
  or cut its outflow.
- G is diagonally isotone, and Σ_t G_t = (net loss to outfalls), which is
  isotone.
- 𝒱 is strictly isotone.

So F is an M-function (Rheinboldt), which gives:
1. a unique solution z^{n+1} for any Δt > 0;
2. order preservation: b ≤ b' ⇒ z ≤ z';
3. with conservation (Crandall–Tartar), ‖𝒱(z) − 𝒱(z')‖₁ ≤ ‖𝒱(z^n) − 𝒱(z'^n)‖₁ + Δt‖δq − δq'‖₁.

Taking z' = 0 (the main run solves the zero-data problem exactly), the volume
deviation of every member is non-expansive, and the step is unconditionally
stable. This is the pH reading the code should carry: R is the monotone part
(accretive in ℓ1), J is empty at the stores, and the "energy" is Σ|δV|. Both
the one-way edge of §2.2 and the pump satisfy (P1) and (P2), so they are
covered.

Where (P1) breaks, and what to do:
- **Non-monotone flow brakes.** A TABULAR or FUNCTIONAL outlet with a kickback
  dip (Bellinge G71F320/G71F68Y: "non-monotone flow brake") has ∂Q/∂h_up < 0
  on part of its curve. That is a negative resistance: multiple roots, and
  chatter even in an exact solve. Today's bisection picks a root arbitrarily
  (its "monotone residual" premise is false there). Fix: replace the table by
  its monotone envelope Q̃(h) = max_{s ≤ h} Q(s) for the member port, and count
  and report the nodes affected. Physically a vortex brake on the kickback is
  hysteretic, so the envelope is the conservative branch (drains at least as
  much as the rising branch).
- **Circular conveyance above ≈0.94 D** decreases with depth. Use
  K̃(y) = max_{s ≤ y} K(s) in the conduit port.
- **Structure table extrapolation.** Below h* − Y the first segment
  extrapolates and can give absolute flow < 0 (reverse flow out of a dry basin
  over its crest). Clamp δQ ≥ −Q(h*).
- **Depth-rated pump curves** (PUMP2 is a step lookup). Check that the curve is
  nondecreasing at init. If it is not, use the envelope and warn.

### 2.4 Case (a): two-way raised-end pipe, far node also per-member
It closes. With the §1.3 law, Q_BF is nondecreasing in h_B and nonincreasing in
h_F, and B and F are both unknowns of the same F(z). There is no owner, no
hand-over, and no "rated once, booked as the other node's inflow". The failures
that forced the ownership rule (`two_way[]`, `tw_seen`, carried
`new_link_dq`) came from evaluating one port at two inconsistent levels across
Gauss–Seidel sweeps, not from the port itself. Residual issues:
- (i) the jumps at z_B and z_F (monotone graph, §3);
- (ii) the unbounded slope K/(2 sqrt|Δ|) as Δ → 0 for a nearly level pair.
  Regularize s(x) → x/sqrt(δ0) for 0 < x < δ0 (δ0 = 0.03 ft, today's H0
  threshold). This is monotone, passive, and gives a finite Newton Jacobian.

### 2.5 Case (b): rule-switched pump (G71F68Y)
- **(b1) Frozen setting — closes.** Within a step the member's pump runs at the main run's
  setting σ*(t), curve at the member's depth, dry ramp r(d): (P1)+(P2) hold, the step is
  ℓ1-contractive and order-preserving. Cost: a wetter member cannot start the pump before the
  main run, so it piles up while σ* = 0 (conservative on the wet side). Recommended for O-2:
  admit the node with (b1) instead of excluding it.
- **(b2) Per-member switching — does NOT close.** σ_i set at step start from the member's own
  level, only when every rule premise reads the inlet node's depth/head (`ControlEngine` scan at
  init), hysteresis y_on > y_off kept. Each step is still (b1)-closed, but across steps members in
  different σ are not ordered: one that switches on earlier can end below a drier one, so
  order-preservation and the between-member ℓ1 contraction are lost (the MC does the same; it is
  physics, but outside the proof). Bounded alternative: volume ∈ [0, V(rim + sur)], pump flow ∈
  [0, Q_max] ⇒ z bounded; no chatter if Δt·Q_max/S_min < y_on − y_off, else a one-report dwell.
  **(b2) needs F-1 before it is built** (HSYM §7.1's "its own project").

## 3. Solve
One implicit solve per member, F(z) = 0, on the full active network (A ∪ N).
Do not split it into store bisection plus a separate correction solve.

- **Method.** Newton with a monotone safeguard. The Jacobian of an M-function
  is a column-dominant M-matrix:
  - diagonal: A_t(h)/Δt (plus outgoing slopes);
  - off-diagonal: ≤ 0;
  - column sums ≥ 0.
  So the `AdditiveSystem` LU (minimum-degree order, no pivoting) factors it as
  it is. Reuse its symbolic pattern: the union of plane edges and port edges is
  the same graph. Only numeric values change per member and per iteration.
- **Jumps.** At a jump (raised end, y_on/y_off, the dry ramp end) use the
  one-sided slope. If an iterate crosses the jump and the residual changes sign
  in that component, fix that node's level at the jump and take the port's
  flow from the balance there. This is the monotone-graph (complementarity)
  solution, and it is what "an empty member pumps what arrives" and "a basin
  sits at its pipe end" mean.
  *As built (O-4, PORT_HAMILTONIAN §6):* the ports stay black boxes, so kinks are
  found numerically: both one-sided differences per argument (far differences when
  the kink lies inside the difference step), the slopes of a port on a kink taken
  just inside the piece the solved step enters (re-solved while they change; this
  also covers kinks along a line in (z_up, z_dn)), the same for a store's volume
  corner, and, when the full step fails Armijo, the step bounded by the first strong
  kink along it (cross it by at most its distance, then just past it).
- **Damping.** Armijo on ‖F‖₁. Start from z^n. Stop at ‖F‖∞ < 1e-10·(S/Δt) or
  after 20 iterations.
- **Fallback.** Monotone nonlinear Jacobi (each node solved for its own
  component, all others frozen, simultaneous update). It converges
  monotonically from the sub-solution z = min(z^n, 0) and the super-solution
  (Rheinboldt). Count every use of the fallback and print the count. A
  fallback that silently rescues a method has fooled this project before (H17).
- **Zero perturbation.** F(0) = 0 exactly when δq = 0 and z^n = 0. Return
  without iterating, so z stays exactly 0.
- **No admitted node.** F is linear and equals the plane system scaled by κ_i,
  so C_i = 0. Short-circuit to C = 0 so the result is bit-identical to
  per-member-off.

**Cost** (estimates; O-2 measures in Release on Bellinge).
- Today: per admitted node, bracket + 60 bisection residuals per member × 4 sweeps; at a coupled
  manhole each residual calls `couplingOrificeQ` (60 inner steps) → ≈ 711 × 50 × 4 × 3,600 ≈ 5e8
  orifice evaluations per step; each structure rebuilds its 17-point table every sweep, each
  `flowAtShiftedDepth` call allocating an n_nodes scratch vector. Measured: demo_v5 ≈ 6× slower
  than per-member-off.
- Proposed: tables built once per step, shared by members (hoist the scratch vector). Per member
  3–6 Newton iterations, each O(#links) port evaluations + one sparse LU on n_a ≈ 1,011 nodes
  (tree-like, little fill, ≈ 1e4–3e4 flops); pond as store, no nested bisection. M = 10: ≈ 1e6
  flops/step; M = 50: ≈ 5e6–1e7 → a few ms vs ≈ 7 ms for the whole per-member-off 1D step. With
  only 16 stores admitted Newton converges in 2–3; with 711 manholes it is still one sparse LU per
  iteration, never dense. Target: ON ≤ 1.5× OFF.

## 4. Guard audit

| Current guard | Verdict | Reason |
|---|---|---|
| `Ldn`: downstream reads the plane, not the correction | Unnecessary under pH | The downstream level is an unknown of the same implicit solve. The 550 ft loop (G72R83B → G72UD11) was a one-step lag, and the lag is gone. |
| 4 Gauss–Seidel sweeps over admitted nodes | Unnecessary | Replaced by one coupled Newton solve. The sweeps were an unconverged fixed point, which is why levels chattered. |
| Upstream-first order | Unnecessary | Same: the coupled solve has no order. |
| Bisection with bracket expansion | Unnecessary | Newton plus a monotone fallback. Its monotone-residual premise is false at a non-monotone brake (§2.3). |
| Clamp to [−depth_det, full + sur − depth_det] after bisection | Survives as port definitions: floor = V's domain plus the dry ramps; ceiling = the overflow port | The clamp creates or deletes water. Under pH the overflow is booked (flooding or pond), and the floor is reached through outflow going to zero, not a projection. Keep a projection only as an assert-counted numerical safeguard. |
| Per-step `memberRange` σ secant | Survives only in the shared plane (structures at N nodes, dry-pipe edges); unnecessary at admitted nodes | Admitted nodes use the exact port. The plane still needs a slope for non-admitted structures, and the per-step RMS (not the report band) keeps that slope out of the feedback loop. |
| Drowned-end linearisation (kind 3) | Survives as a port definition plus a numerical safeguard | A drowned pipe is the two-way conduit port. The sqrt is regularized below δ0 to keep the Jacobian finite. In an exact implicit solve the stiffness would not oscillate; it would only slow Newton. |
| Sticky `admit()` | Survives as a numerical safeguard | Admission swaps a link between a linear law and a nonlinear law. With the anchor (P3) a swap is consistent at z = 0 but not at z ≠ 0, so swapping back and forth would inject volume. Sticky is cheap and keeps the system fixed. |
| Static admission of raised-end basins (from t = 0) | Survives as a port definition | The raised-end two-port is defined by geometry, not by the state, so its stores exist from the start. |
| Level bounds invert .. rim + sur | Survive as port definitions | V's domain and the overflow port (row above). |
| Exclusion of rule-switched pumps | Becomes a port definition: (b1) frozen setting, node admitted; (b2) only after F-1 | §2.5. |
| `pump_bal` "empty member pumps what arrives" | Unnecessary | The dry ramp r(d) plus the monotone-graph solution at the jump give exactly this, inside the port. |
| `two_way[]` ownership, `tw_seen`, carried `new_link_dq` | Unnecessary | Each link appears once in F (§5). |
| Exact-zero short-circuit | Survives as a numerical safeguard | Keeps the ZeroPerturbation invariant bit-exact. |
| 17-point clustered structure table | Survives as a port definition | It is the structure's rating, made monotone (§2.3) and built once per step for all members. |

## 5. Correction fields: each flow booked once
Today the stores are solved first. Their actual extra outflow minus the
planes' straight line becomes a source e, and a second `AdditiveSystem` solves
C on the plane matrix with the stores held. The same link is evaluated by the
store (as an outflow), by the correction source, and (for two-way pipes) by
the node across it.

Proposed, per member i:
1. Assemble F on all active nodes. Each link l is exactly one of two things:
   - a **port**, if either end is in A and l's law is nonlinear (§1.2): it
     contributes δQ_l(z) to G;
   - a **plane edge** otherwise: it contributes ju z_up + jd z_dn (or d z_up)
     to G, the same numbers the planes use.
   Pinned (spilling) N nodes are held at zero, as in the planes.
2. Solve F(z_i) = 0 (§3).
3. C_i = z_i − κ_i·Ψ, written to `rom1d_corr_` and passed to the ROM through
   `setMemberAdditive` as today. At a store, C_i,a = y_i,a − κ_i·Ψ_a exactly as
   now.

The mis-delivered outflow becomes the port itself. The correction is the
response of the linear part to "port flow minus plane-edge flow", and it is
solved jointly, not as a lagged source. Because F conserves (P2), a link's
flow leaves its up end and enters its dn end in one equation set. That makes
the booking-once property a theorem, not bookkeeping.

Two consequences:
- Backwater from a store's level travels upstream through the plane edges' jd.
  This removes the approximation "a storage node's level difference does not
  travel upstream".
- The coupled manhole's exchange volume (`setExternalCouplingVolume`) is read
  from the solved port flow, once.

## 6. Gates and implementation order

**Acceptance gates** (PORT_HAMILTONIAN phase 5, plus the gates this phase
touches):

| Gate | Requirement |
|---|---|
| `regression_storage_threshold_coverage` A (main run spills) | basin ≥ 0.891/0.966: calibrated (cov ≥ 0.80, width ≤ 1.5) |
| Same, B (main run below crest) | basin cov ≥ 0.70, width 0.8–1.5 (today 0.730/0.966); J2 width ≤ 3.0 (today 1.16) |
| Bellinge real storm vs `mc_v3` | 1D all manholes ≥ per-member-off (0.957/1.40); trunk not worse than 0.814/1.26 |
| G72UD11 | band one-sided like the MC (+≈1.2 m cap, not ±1.8 m) |
| G72R150 pond | within the MC's 0.45 m class (demo_v5 had 0.53) |
| G72R83B | drier members within 0.1 ft (MC 0.06 m) |
| Rain gates | SR-5 0.921/1.028, CL-1e 0.941/1.031, SR-7 0.921/0.987 within 2 % |
| Manning gates | unchanged at the printed digit (stores read the rain channel only) |
| Deterministic run | bit-identical with the sidecar on or off (EXPECT_EQ heads and flows) |
| Runtime, Bellinge Release | per-member ON ≤ 1.5× per-member OFF; fallback count printed |

No floor or ceiling is moved to make any of these pass. A miss goes back to
this note.

**Implementation order for O-2** (each step adds the named test):
1. **Port library**, pure header `uncertainty/RomStorePorts.hpp` (RomDiagTrust pattern): §1.2
   laws with anchors, §1.3 raised-end law, monotone envelope, dry ramp, δ0 regularization.
   Test `StorePortsAreMonotoneAndConservative`: random (h_up, h_dn), ∂Q/∂h_up ≥ 0 ≥ ∂Q/∂h_dn,
   anchor = 0, Q_BF > 0 ⇒ h_B > h_F. Mutations: today's kind 6 law fails the last check; a raw
   kickback brake table fails monotonicity.
2. **M-function Newton** `PortGraphStep` (pure; `AdditiveSystem`'s symbolic order, per-call
   numerics). Tests `PortGraphStepIsL1Contractive` (mutation: a non-monotone port breaks it),
   `PortGraphStepIsOrderPreserving`, `PortGraphZeroDataIsExact` (EXPECT_EQ 0),
   `PortGraphLinearReducesToPlane` (no ports ⇒ the `AdditiveSystem` step to 1e-12).
3. **Engine wiring**: `stepRom1dStorageMembers` becomes assemble → solve → C = z − κΨ; admission
   as today plus (b1) rule pumps; delete sweeps, bisection, `Ldn`, `two_way`, `pump_bal`.
   Tests: storage gate A/B; `StructureSecantKeepsDeterministicRunBitIdentical`; new
   `PerMemberStoresOffIsBitIdentical` (no admitted node ⇒ C ≡ 0).
4. **V(h) stores + overflow port** replace frozen S and the rim clamp. Test
   `PerMemberStoreConservesVolume` (closed fixture: Σ δV = Σ extra in − Σ extra outfall/overflow
   to 1e-9 rel.; mutation: restoring the clamp fails it).
5. **Raised-end and coupled-manhole ports** (pond as store, law owned by O-3). Tests: a raised-end
   backflow fixture added to the storage gate (MC vs ROM); existing 2D coupling tests.
6. **Bellinge vs `mc_v3`**, runtime, VALIDATION row; delete the hook after the gates pass.

## 7. Risks, and what would stop the work
- **F-1 before O-2 step 3** on (1) ℓ1 volume contraction replacing the quadratic energy as the
  storage function (§2.2–2.3) and (2) (b2). If F-1 rejects the ℓ1 reading, stop: the plan's
  premise "passive ports ⇒ energy-dissipating step" is then false here.
- **Monotone envelopes change the physics** at non-monotone brakes. If G71F320/G71F68Y move from
  ranking to conservative because of the envelope, report it so; do not tune it.
- **Admission chain.** Rating G72UD11's outlet as a conduit port may expose the next
  nonlinearity downstream. The cost model allows admitting more; the method does not prove more
  admission converges to the MC. Stop and report if a third downstream node needs a new port kind.
- **Jumps.** If Newton needs the fallback on > 1 % of member-steps, δ_dry/δ0 are wrong for this
  network: measure before widening, never widen to pass a gate.
- **Approximations that stay:** structure ratings hold the downstream node at h* (no member
  submergence); the Manning channel's deviation is not in the store's level; the street law waits
  for O-3.
- **Stop:** any §6 gate below per-member-off; deterministic run not bit-identical; the ℓ1 test
  fails on a fixture built only from §1.2 ports (a law in the table is not what this note claims).
