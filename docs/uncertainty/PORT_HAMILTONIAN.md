# Port-Hamiltonian structure for the uncertainty sidecar (plan + record)

Status: APPROVED 2026-11-01 (owner). Branch `ph/structured-skew` off `feature/uncertainty-sidecar` `56b950cf`.
Owner principle: one behaviour, no tuning switches; one internal A/B hook per phase, deleted when the phase lands.
Calibration rule: coverage ≥ 0.80 AND width ≤ 1.5 = calibrated; ≥ 0.80 wider = conservative; else ranking only.
No tolerance is tuned to green a spread test. Statistics-bearing phases get a cross-vendor review.

## 1. The idea

Every deviation operator is written as

    Q dψ/dt = (J − R) ψ + B u,      E = ½ ψᵀ Q ψ

- Q: storage / energy weight (node storage S in 1D, cell area A in 2D; nonlinear V(h) at per-member stores).
- J = −Jᵀ: conservative transport (central part of the one-way gate / advection). Conserves E.
- R = Rᵀ ⪰ 0: friction conductance, upwind half-diffusion, structure secants, grounding, kinematic diagonal. Loses E.
- B u: ports — rain, lateral inflow, outfalls, the 1D–2D orifice exchange.

If the reduced basis P is Q-orthonormal (PᵀQP = I), Galerkin keeps the form: J_r = PᵀJP is skew and
R_r = PᵀRP ⪰ 0, so every reduced operator is energy-stable by construction. This single rule explains the
four projection failures the project hit one at a time (PᵀS⁻¹AP 1e33; 2D similarity transform 1e41; defect 5,
plain projection of upwind advection; defect 7, relative Galerkin with 17-order depth contrast).

## 2. What was checked before approval (2026-11-01)

1. **1D modal channel is already pH.** H14's gate keeps the upstream diagonal and removes only L[up,dn]:
   per edge w·[[1, −(1−g)], [−1, 1]], sym part eigenvalues w(1 ± (1−g/2)) ≥ w·g/2 ≥ 0. H18's kinematic
   diagonal and the grounding are positive diagonals (R). With the S-orthonormal basis sym(PᵀLP) = Pᵀsym(L)P ⪰ 0.
   Random check (300 chains with loops, g ∈ [0,1], storage contrast 1e4): min eig sym(M) = +0.048 (never < 0);
   same operator on a Euclidean basis with plain projection: −12.3. ⇒ the former "phase 3" (rewrite 1D as J−R)
   is unnecessary; it reduces to a pinning test + docs. Runtime is unaffected: the engine already runs a dense
   k×k M through BatchPropagator whenever any reach is gated (SWMMEngine.cpp, refreshRom1dDirectionalOperator).
2. **2D has two Grams.** The volume Gram `GinvArea` (cell areas, fixed) can be absorbed by an area-orthonormal
   basis and deleted. The relative Gram `Ginv = (HP)ᵀA(HP)` depends on the live depth field and stays (Cholesky
   kept for that channel only). Phase 2 deletes about half of what the first draft claimed.
3. **Where the code is.** `SWMMEngine::stepRom1dStorageMembers` = 645 lines of hand-built ratings, admission
   rules, sweeps and guards. Phase 5 (stores + ports) is the only phase where pH changes behaviour.

4. **F-1 verdict on the stores-and-ports note (PH_STORES_AND_PORTS.md, 2026-11-01).** The quadratic energy
   ½ψᵀQψ does NOT close in deviation form at one-way ports (weir, free outlet, supercritical pipe, pump):
   δQ = d·z_up between two stores has sym(L) = d·[[1,−½],[−½,0]] with eigenvalue d(1−√2)/2 < 0. What
   closes for every port is ℓ1 contraction of volume differences (monotone flow laws + conservation +
   V(h) increasing ⇒ M-function; order preservation; Crandall–Tartar). This is the property the H19
   additive channel already uses (column-dominant M-matrix). Consequence for the whole plan: **two
   structural contracts, not one** — reduced modal operators are ℓ2-stable (Q-orthonormal Galerkin,
   sym ⪰ 0; only two-way/diagonal terms may be projected), full-order one-way transfer is ℓ1-stable
   and must stay full order. This is why H18's kinematic term sits on the diagonal only and why H19 is
   full order. Accepted for O-2: (b1) frozen pump setting, monotone envelopes for flow brakes / circular
   conveyance above 0.94 D / pump curves, clamp on table extrapolation, regularised raised-end law
   (δ0 = 0.03 ft). Deferred to its own project: (b2) per-member pump switching.

5. **Phase 0 measured (S-5, 2026-11-01; probe `bellinge-flood-uncertainty/scripts/probe/ph_operator_probe.cpp`).**
   ‖J_r‖_F/‖R_r‖_F and min eig of R_r in the volume Gram, k = 40:
   W3 plane 3000 s — Manning 0.037 (min eig 1.1e-3), additive 0.055 (4.3e-4);
   Bellinge 05:25 — Manning 0.11 (7.7e-14), additive 3.9 (2.3e-16, both parts ~0: operator nearly dead, |λ|max 6e-6/s);
   Bellinge 08:00 — Manning 0.023 (1.4e-4, cond G 1.6e5), additive 0.52 (1.7e-10).
   No negative eigenvalue on any fixture: the shipped 2D operators are already ℓ2-stable. The skew part is
   real physics on Bellinge (half the dissipative part in the additive channel at 08:00) and must be kept;
   in the PLAIN inner product the Manning skew reads 0.05/0.24/0.97 — the Gram choice is what makes the
   split meaningful (cond G 1.6e5 at 08:00). Phase 2 (area-orthonormal basis) is justified.

## 3. Phases

| # | Phase | Model | Gate |
|---|-------|-------|------|
| 0 | Measure ‖J_r‖/‖R_r‖ on the 2D reduced operators (Bellinge 05:25, 08:00; W3 plane; channel) | Sonnet (S-5) | report only |
| 1 | Shared `PHOperator` core: two-way edges (J skew + R PSD, reducible, ℓ2) and one-way edges (M-matrix, full order only, ℓ1); `reduce(P)` refuses one-way edges | Sonnet (S-3) | J_r skew, R_r PSD; ℓ1 non-expansive step on random one-way nets; mutation on flipped J sign; reduce() of a one-way edge is a refusal |
| 2 | 2D area-orthonormal basis (`MeshEigenBasis` generalized `L v = λ A v`), additive channel without the volume Gram | Sonnet (S-1, S-2) → Haiku (H-4 deletions) | W3 0.864/1.360, W4, H15, channel cells within noise; defect-5/defect-7 fixtures stable; uniform-area mesh bit-identical |
| 3 | 1D: pin the J−R property (test + docs); no numeric change | Haiku (H-1, H-2) | test fails when the upstream diagonal is dropped |
| 4 | H19 additive channel assembled from `PHOperator` | Sonnet (S-4) | bit-identical: SR-5, SR-7, CL-1e, 3 AdditiveChannelDirect tests |
| 5 | Per-member stores as a port interconnection (replaces `stepRom1dStorageMembers`) | Opus (O-1 design, O-2 impl) ; Fable F-1 only if the passivity argument does not close | storage gate A 0.891/0.966; Bellinge vs mc_v3 ≥ per-member-off (1D 0.957/1.40); G72UD11 one-sided |
| 6 | 1D–2D exchange as a dissipative two-port | Opus (O-3) | 2D coupling tests; Bellinge 2D street ≥ 0.698/0.21 |
| 7 | Delete old paths, docs, cross-vendor review of 2 and 5 | Haiku docs; Fable F-2 review | — |

Order: H-1, H-2, S-5, O-1 first (independent, no shared build state) → S-1 → S-2 → H-4 → S-3 → S-4 → O-2 → O-3 → 7.

## 4. Task cards

### H-1 (Haiku) — test `ReducedDirectionalOperatorIsEnergyStable`
File `tests/unit/engine/test_spectral_rom1d.cpp`. Build random gated operators as in §2.1 (chains + loops,
g ∈ [0,1], kinematic diagonal, grounding, storage contrast), random S-orthonormal P, M = PᵀLP; assert
min eig sym(M) ≥ −1e-12 (Jacobi on k ≤ 10). Mutation: zero the upstream diagonal contribution of the gated
part → must fail. Also call `assembleDirectionalReduced` itself on one fixture and assert the same.
Build only `test_engine_spectral_rom1d` in `build/darwin-v2` (Debug). Never rebuild while ctest runs.

### H-2 (Haiku) — docs
`docs/uncertainty/RELAXATION_RATES.md` and `HOW_IT_WORKS.md` §8: one paragraph each stating the J−R reading
of the 1D operator and why it is stable after projection (S-orthonormal Galerkin). Cite this plan.

### H-3 (Haiku, owner decision pending) — delete the shipped-off drop gate (`use_drop`, `H14_DROP`).

### H-4 (Haiku, after S-2) — delete `volume_inner_product`, `GinvArea`, the Ginv argument of
`setAdditiveOperator`, the plain-projection branch; update W3/W4 harness call sites; ctest the 2D targets.

### S-1 (Sonnet) — `MeshEigenBasis::build(..., area_weights)`
Generalized eigenproblem L v = λ A v via A^{−1/2} L A^{−1/2} (the H18 trick), P area-orthonormal.
Tests: PᵀAP = I to 1e-10; uniform areas bit-identical to today's P (EXPECT_EQ); eigen-relation against an
independently assembled L; golden refs untouched when area_weights = null.

### S-2 (Sonnet) — `DeviationOperator2D::assemble` on the area-orthonormal basis
Additive channel: M = PᵀAL P with no Gram. Relative channel: keep its depth Gram. Router passes the basis.
Gates: W3 (`regression_2d_rom_marcher_coverage`), W4 corr, H15, channel cells; `EnergyStableFormHasNoGrowingModeOnConvergingFlow`
and the defect-7 Manning fixture stable. One A/B hook `rom2dAreaBasis()` during the phase, deleted at the end.

### S-3 (Sonnet) — `src/engine/uncertainty/PHOperator.hpp`
Pure header (RomDiagTrust pattern). Tests as in the table. No consumers yet.

### S-4 (Sonnet) — `RomAdditiveChannel` built from `PHOperator`
Same pattern/factor code; assembly from PHOperator. Bit-identical gates (EXPECT_EQ on the three direct tests).

### S-5 (Sonnet) — phase-0 measurement
Probe in `~/Projects/bellinge-flood-uncertainty/scripts/probe/` against `build/darwin-v2-rel`: dump
`reducedOperator()`/`reducedOperatorAdd()` at 05:25 and 08:00 of the real storm (demo_v4 config) and on the
W3 plane; report ‖J_r‖_F/‖R_r‖_F and min eig sym per channel. Results → VALIDATION.md "pH §0" and this file.

### O-1 (Opus) — design note `docs/uncertainty/PH_STORES_AND_PORTS.md`
Each admitted node = store V(h); each rating = monotone two-port (structure table, raised-end pipe, pump,
conduit); one implicit energy-dissipating solve per member over the port graph. State which current guards
(`Ldn`, 4-sweep GS, bisection bounds, lag guards, sticky admission) become unnecessary and which survive as
port definitions. Passivity argument for the two-way raised-end pipe and the rule-switched pump (the two
cases the current code excludes). Acceptance gates as in the table.

### O-2 (Opus) — implement O-1 behind `rom1dStorageMembersEnabled()`, run the gates, Bellinge vs mc_v3.
### O-3 (Opus) — 1D–2D exchange as a two-port; replace pinned-node / coupling-residual special cases where covered.
### F-1 (Fable, only if needed) — review O-1 where the passivity argument does not close.
### F-2 (Fable) — cross-vendor review of phases 2 and 5.

## 5. What this will not do
- Steady bands do not move (fixed point is operator-independent); J acts on transients only.
- Does not replace H11's phase coordinate; does not fix 2D street-band narrowness, inflow over-delivery or
  rule-controlled pumps by itself; the quasi-static residual (H17) stays.

## 6. Log
- 2026-11-01: plan approved; branch created; wave A launched (H-1, H-2, S-5, O-1).
- 2026-11-01: H-1/H-2 landed (2ecc153a); S-1 landed (463c808a); O-1 note written, F-1 verdict in §2.4 — energy contract replaced by ℓ1 for full-order ports; S-2 landed: basis neutral on every gated cell; harness-recipe finding (VALIDATION, phase 2) — H15 corr20 0.785 under the shipped recipe, owner decision pending; O-2 running.
- 2026-11-02: H-4 phase-2 clean-up landed; hook, GinvArea, volume_inner_product, skew_advection flags deleted.
- 2026-11-02: S-4 landed: H19 AdditiveSystem assembled from PHOperator (ℓ1 contract asserted), bit-identical on SR-5/SR-7/CL-1e.
- 2026-11-02: O-2 landed behind `rom1dStorageMembersEnabled()` (default OFF): RomStorePorts.hpp (laws),
  RomPortGraph.hpp (one Newton solve per member, Armijo, counted Jacobi fallback), stepRom1dStorageMembers
  rewritten (646 del / 642 add; Ldn, GS sweeps, bisection, pump_bal, two_way ownership deleted). 16 property
  tests incl. ℓ1 contraction + two mutations. Storage gate: A 0.889/0.958 cal., B 0.727/0.960 (−0.001, noise),
  new R 0.952/0.880 cal. **Three additions beyond the note, all found on Bellinge**: (i) floored/dry-factored
  anchored laws (members sank 3,600 ft below invert otherwise), (ii) per-member source-floor port (κq below
  −100 % of runoff), (iii) main-run volume mismatch (V(d*_new)−V(d*_old) ≠ dt·net*) shared in proportion to
  member water, gains additive, clamp ≤ 1 — a modelling choice, needs F-2. **Stop lines touched**: Jacobi
  fallbacks 1.25 % of member-solves with (b1) vs 0.4 % without (G71F68Y pump floor starves G71F06R_ovl);
  6 non-converged by 06:17 (kink-cycle stalls at ~1e-8 cfs, G72R33B pair). Bellinge vs mc_v3 NOT yet scored
  (run in progress). Runtime ≤ 1.5× unmeasured (1D smoke 1.63× before the floors). O-3 does not start until
  the Bellinge number exists.
- 2026-11-02: Bellinge `demo_ph5` (per-member ON) profiled mid-run with `sample`: ~85 % of engine samples in the
  coupling-orifice port law of `stepRom1dStorageMembers` (`couplingOrificeQ`, a 60-iteration bisection per
  evaluation, called inside every Newton iteration × member × coupled manhole, single thread); steps 3000→4000
  cost ≥ 5× steps 2000→3000. Runtime stop line (≤ 1.5× OFF) is failed at the storm peak. O-3 (pond as a store,
  exchange as a closed-form monotone two-port, no nested solve) is therefore required on cost, not only accuracy.
- 2026-11-02: O-3 (per-member path): the street pond is a store of the per-member solve (a_pond·z_p) and the
  orifice a regularised monotone two-port manhole -> pond (`ports::orificePortFlow`, `PondStore`;
  PH_STORES_AND_PORTS §1.5); `couplingOrificeQ` deleted (no other caller). Booking = pond volume change.
  Tests: orifice monotone/passive/regularised (mutation: bare sqrt ⇒ slope 1.2e6 vs bound 14 and the ℓ1 test with
  ponds fails to converge), ℓ1 contraction + order preservation + zero data on random graphs with ponds, manhole→pond
  conservation, coupled engine fixture: deterministic run bit-identical, zero perturbation ⇒ C ≡ 0 and booked
  volumes ≡ 0. Cost on the coupled fixture (1 coupled manhole, 20 members, Debug): orifice-law evaluations
  7,801 → 209 per step; wall ON/OFF 1.74–2.06 before, 1.67–1.96 after — the ≤ 1.5× target is NOT met here: on a
  5-node graph the remaining cost is the per-member Newton machinery (AdditiveSystem rebuild per iteration ≈ 1/3).
  Storage gate unchanged (A 0.889/0.958, B 0.727/0.960, R 0.952/0.880). Bellinge not run (Release tree busy).
  Follow-ups from O-3 (lead): (i) book dt·Q_port to the 2D ROM instead of the pond's volume change — exact
  1D↔2D transfer by construction (today's gap ≈ Newton tolerance, 1e-9 ft³); (ii) O-2's `AdditiveSystem::build`
  per Newton iteration — reuse the factor (modified Newton) unless the Jacobian moved; (iii) the Bellinge
  `demo_ph5` run on O-2 code was killed and restarted on O-3 code (one run answers accuracy and cost).
- 2026-11-02: O-3 follow-ups: (i) landed — booking = dt·Q_port, exact to 1e-12; (ii) modified Newton tried and REJECTED: factorisations halved, iterations doubled, wall ON/OFF unchanged (1.87 → 1.98 median), +14 Armijo rejections on R; remaining ON cost on the coupled fixture is BatchPropagator + reconstruction outside the Newton loop, not the solve.
- 2026-11-02: Bellinge `demo_ph5` on O-3 code (bfbcba09), per-member ON: steps 2000→3000 215 s wall (O-2 code 742 s —
  the orifice cost is gone) but steps 3000→4000 took 10,108 s with 6,350 Jacobi fallbacks and **607 non-converged
  solves** (`sample`: top engine frame `PortGraph::jacobi_`). Newton fails on the piecewise-smooth laws at the storm
  peak and the fallback does the work. Run killed at step 4000 (06:17→07:03); no accuracy score taken — 607 failed
  solves would make it meaningless. O-4 (semismooth / kink-bounded Newton) opened; rerun after it lands.
- 2026-11-02: O-4 (per-member solve on piecewise-smooth monotone ports, `RomPortGraph.hpp` only; laws unchanged).
  **Diagnosis** (Bellinge 1D smoke `ph_smoke1d.inp` to 07:00, Debug, hook ON; first 20 + every 50th fallback dumped with
  residual history, the incident ports' one-sided slopes and kink flags): no law is discontinuous (every port is C⁰,
  the frozen pump included); every fallback sat at a kink, in three classes, none of them ill-scaling of store vs
  manhole area. (a) *Wrong Jacobian at a kink*: the forward difference (h = 1e-7) straddled a kink 2.5e-8 ft away
  (G72R33Bout1 at y = 0, slopes 61.8 | 1.56, difference 16.6) → linear convergence with alternating sign (ratio ≈ 0.8),
  stall at 1e-9–1e-8 cfs; or took the wrong side at a store's floor (G80F66X → G80F11B, 0 | −5317 cfs/ft).
  (b) *True non-smooth cycles* across a strong corner, fed by "take the full step when no trial is better": the S of
  dry ramp → q*/δ ramp → envelope plateau at G72R33Bb (±0.04 ft 2-cycle), the floored line G71F06R_ovl → G71F68Y at its
  flow reversal (z −324 ↔ −227 ft, ‖F‖ 1e3 cfs). (c) *Per-axis sides at a kink along a line in (z_up, z_dn)* (a flow
  reversal): a non-descent direction and a period-6 cycle, ‖F‖ 4 → 4e4 (G71F320/G71F68Y). The G71F68Y pump itself was
  never the culprit; G71F06R_ovl (a linear node behind a port floored by the dry wet well) does sink to −300…−500 ft,
  which Jacobi converged to as well — a model finding (no floor at a non-admitted node), not a solver one.
  **Scheme**: semismooth (active-set) Newton. Both one-sided differences per port argument; a kink inside the
  difference step → far differences (the two pieces); a port on a kink takes its slopes just inside the piece the solved
  step enters (forward differences at z + η·dz, re-solved while they change, ≤ 3 re-solves — the B-derivative in the
  step's direction, which also covers line kinks); the same for a store's volume corner (direction = sign of its dz).
  When the full step fails Armijo, the step is bounded by the first strong kink along it (any port or store whose
  linearisation misses by ≥ 1 % of ‖F‖₁; bisection for the departure point, a quadratic law is recognised as smooth):
  candidates t_c ∈ [t_k, 2t_k] (cross by at most its distance), then just past the kink, then halving; with no
  decrease the best trial (or just past the kink) is taken, never the blind full step. Jacobi stays as a counted anomaly
  (`[PH-ports] ... Jacobi fallbacks N (anomaly; ...)`, plus kink-bounded and active-set re-solve counts). No tolerance,
  δ0, δ_dry or law changed; two mutation hooks `kink_bound`, `active_set`.
  **Tests**: test_engine_rom_member_ports 26 (24 + `KinkBoundedNewtonCrossesEnvelopeAndPumpFloorCorners`: two stores, a
  kickback brake under its envelope, a floored line and a floored pump; plain Newton stalls 20 its, the scheme 2 its,
  kink bound removed → fails; and `ActiveSetSlopeAtAKinkWithinTheDifferenceStep`: the G72R33Bb fixture, 1–2 its, forward
  slopes → stall; both mutated in the source as well), additive channel 12, spectral_rom1d 101, storage gate A 0.889/0.958,
  B 0.727/0.960, R 0.952/0.880 unchanged (Jacobi 0 on A/B/R; R factorisations 62,041 for 60,771 Newton iterations).
  **Smoke to 07:00 (Debug)**: before — 894 Jacobi fallbacks by step 3000 (06:17), then the run sits in `jacobi_`
  and had not finished after 65 min wall (79 CPU-min, killed; step 3000 at 587 s); after — done, 3,892 steps, 474 fallbacks in 194,550 solves (0.24 %), not converged 0, Newton
  iterations 310,901, wall 939 s under load (650 s on a quieter machine; step 3000 at 347–541 s vs 404–587 s before).
  **Remaining fallbacks are not kinks**: 462 of 474 are roundoff floors — Newton steps ≤ 1e-12 ft with ‖F‖ < 1e-6 cfs,
  where |slope| × the head's floating-point quantum exceeds the 1e-10·A/dt tolerance at a small node (G80F310,
  A = 1.2 ft², dry-ramp slope 4e4 cfs/ft; G71F320, plane jd 2.6e6 cfs/ft through a nearly level drowned pipe). Jacobi
  "converges" those only because its own tolerance is 1e-8·A/dt, 100× looser than Newton's. 11 (+1 near the floor) are genuine stalls on
  the bilinear dry-ramp region of floored lines (G80F66Y/G80F330, G71F090/G71F265). Owner decision: a roundoff-aware
  residual floor (or one tolerance for both solvers) would remove the 462; not done here (no tolerance changes).
- 2026-11-02 (lead, from O-4): OPEN model finding — a NON-admitted linear node whose outlet is a port floored by
  a dry wet well (G71F06R_ovl → G71F68Y) has no floor of its own and sinks to −300…−500 ft in the shared plane;
  Jacobi and Newton agree, so it is the model, not the solver. Fix belongs to admission: a node whose every
  outlet is a floored/one-way port of an admitted node must be admitted too (closure of the admitted set under
  "only drains through an admitted store"). Not done; blocks calling phase 5 complete.
- 2026-11-03: O-4b (`RomPortGraph.hpp` only). (1) Roundoff-aware convergence: Newton also stops when every proposed step is
  |dz_i| <= 8·eps·(|z_i| + |h*_i| + 1 ft) AND ‖F‖₁ <= Σ|slope_i|·8·eps·(|h*_i| + 1) (slope = Jacobian diagonal); `PortGraph::level`
  carries h* (engine fills it from the main-run head). An engineering floor of double arithmetic, not a tolerance change; counted in
  `stats.fp_floor_converged` and printed on the `[PH-ports]` line. (2) `jacobi_` uses exactly Newton's criterion (residual
  tolerance 1e-10·A/dt + 1e-12·mag, or the floor); its own 1e-8 is gone. (3) `kink_bound` / `active_set` deleted; the two O-4
  tests compare with a plain Newton reference inside the test (shown to stall on the same fixtures). Gates (Debug): member_ports 26,
  additive_channel 12, spectral_rom1d 101, storage_threshold A 0.889/0.958, B 0.727/0.960, R 0.952/0.880, 0 fallbacks, fp-floor 0.
  1D smoke to 07:00 NOT completed: the machine was swap-starved (14 GB swap, load 60-120, the process got 1 CPU-minute in 2 h);
  Bellinge rerun NOT launched. Rerun the smoke, then the Release run, on a quiet machine.
- 2026-11-03: Debug 1D smoke on HEAD (O-4b): 3,892 steps in 636 s; 194,550 solves, 305k Newton iterations, fp-floor
  converged 736, Jacobi fallbacks 34 (0.017 %, all converged), non-converged 0; G80F11B 92 member-steps ≤ 0.12 ft
  below invert. Accepted (criteria: non-converged 0, fallbacks ≤ 1 %). Release rebuilt; Bellinge `demo_ph5` rerun launched.
- 2026-11-04: Bellinge `demo_ph5` on HEAD (O-4b): step 3000 at 3,998 s, 162 fallbacks, 4 non-converged (pre-O-4:
  6,350/607) — the solver is fixed, but steps 3000→4000 still take hours. `sample`: `stepRom1dStorageMembers` body
  610 + `AdditiveSystem::build` 420 of ~1,500 engine samples. Cause: `PortGraph::solve` does `jac = edges` and
  `sys.build(n, …)` — the FULL 1,011-node network is rebuilt and factored on every Newton iteration for every
  member, although only ~11 stores + ponds are nonlinear and the rest is the member-independent linear plane
  operator. O-5 opened: factor the linear multi-port once per step (shared across members), Newton on the
  admitted set through its Schur complement. Same solution by construction; cost per iteration O(k²), k ≈ 11–50.
- 2026-11-04: Bellinge HEAD run reached step 4000 (07:03) at 15,302 s: steps 3000→4000 added 2,853 fallbacks and
  535 non-converged (totals 3,015 / 539). The O-4 kink handling, which clears the 1D-only smoke (34 fallbacks,
  0 failed), does NOT carry to the coupled storm peak, where many rim manholes + street ponds are admitted. The
  1D smoke is therefore not a sufficient reproducer. Next (after O-5 leaves RomPortGraph.hpp): dump every
  non-converged per-member system at the peak (admitted graph, A, tabulated port laws, F, z) to a file and
  replay it in a Debug unit test (O-6). The accuracy score of this run will be contaminated by the 539.
- 2026-11-04: O-5 (`RomPortGraph.hpp`; engine: `g.prepare()` once per step, `rom1d_corr_sys_` deleted). The per-member
  solve no longer factors the whole network per Newton iteration per member. Partition: N = every store and every node
  a port touches (admitted stores, ponds, the far ends of their ports), L = all other unpinned nodes. No port touches
  L, so L rows are linear with the main run's ju/jd/d and S/dt — member-independent; only b and z^n (right-hand side)
  are the member's. A_LL factored once per step (AdditiveSystem), Schur complement J_NN − A_NL·A_LL⁻¹·A_LN formed with
  one L solve per N node that has an L neighbour; each Newton pass factors only the dense k×k system. The iterates are
  the full-network Newton's (the L residual scales by exactly 1 − λ along a step: carried as a scalar; boundary z_L
  follows the step; z_L recovered with one L solve per member), so criteria, kink bound, active set, fp floor and
  Jacobi are unchanged. Counters: `factorisations` = k×k, new `l_factorisations`, `l_solves`, `k_max`.
  Tests: member_ports 28 (26 unchanged + `SchurComplementSolveMatchesFullNetworkSolve`: 50 graphs of 30–200 linear
  nodes with 2–6 stores, ponds, member-dependent sinks, random pins, 3 members each, vs the pre-O-5 solver kept as a
  test-only reference header: worst max|Δz|/max|z| 3.0e-14, Newton iterations 752 = 752, 0 fallbacks; mutation —
  boundary step without the W term — fails it at 0.26 and two other tests; `LinearFactorIsSharedByMembers…`: 1 L
  factorisation for 10 members, refactored on an edge change; coupled cost test asserts L factorisations = per-member
  steps, 409 = 409 for 20 members), additive_channel 12, spectral_rom1d 101, storage gate A 0.889/0.958, B 0.727/0.960,
  R 0.952/0.880 identical, 0 fallbacks (R Newton 60,760 vs 60,771 before: roundoff). Coupled fixture wall ON/OFF 1.79
  median (5-node graph, unchanged within noise).
  **Debug 1D smoke to 07:00** (load 10–24, another session's build running): 3,892 steps in **336 s** (HEAD O-4b 636 s on
  a quieter machine; step 2000 127 vs 165 s, step 3000 220 vs 359 s); solves 194,550, Newton 305,185 (305,332), fp-floor
  741 (736), Jacobi 35 (34, all converged), non-converged 0, G80F11B 92 member-steps ≤ 0.123 ft below invert (same);
  k ≤ 30, k×k factorisations 438,830, L factorisations 3,891, L solves 256,872.
- 2026-11-04: O-6 (coupled storm peak). **Dumps**: `PortGraph::on_failure` + `uncertainty/RomPortGraphDump.hpp`
  (`OPENSWMM_PH_DUMP_DIR`; diagnostic only): whole graph, start / Newton exit / final levels, Newton trace, N set and
  Schur base, every store and port law tabulated (adaptive knots, bilinear replay). `demo_ph5` on HEAD + hook, Release,
  stopped after step 3000 (06:21): 252 dumps (200 fallbacks = cap, 52 not converged) in `SWMM6/data/ph_dumps`; k = 251
  at 04:33 growing to 619 at 06:21 (coupled manholes + ponds). Machine thermally throttled (CPU speed limit 26 %) for
  most of it: step 1000 at 4,464 s vs 594 s before. **Replay** (Debug, `test_rom_port_graph_replay.cpp`): the O-5
  solver on the tables reproduces 160 of 252 (all 52 NC as fallbacks, 46 as Jacobi failures). **Causes** (by
  scratch variants of the solver): (A) *many independent corners* — ~20 coupled manholes whose cell ground lies
  below their member level, each a 0.01 ft dry ramp worth ~90 cfs into its street pond (ramp slope 9e3 cfs/ft vs
  A/dt ≈ 0.6); the global kink bound crosses one corner per iteration (λ 1e-5…1e-3, 20 its, ‖F‖ 64 → 63.7): 24 fb;
  (B) *law curving before its kink* (orifice square root below the street-level crossing, bilinear floored lines
  G80F66Y/G80F310/G80F330): "just past the kink" located where the error first departs from the tangent, t ≈
  0.003–0.02, while the kink sat near t ≈ 0.3–0.4 → 20 its at λ ≈ 0.005: 82 fb + 10 nc (+ 4 needing A or A+B);
  (C) *a sunk unfloored port end*: G71F06R_ovl (not admitted) drains only through a floored line into the empty
  wet well G71F68Y and sits 68–157 ft below the main run in 64 dumps, 39 of the 52 NC — the O-4 open model finding;
  (D) not reproduced by the tables: 66 fb + 1 nc (6 at |F| < 1e-6 cfs, the roundoff class). Pond/manhole area
  ratios 460–680 and k > 600 were present but not causes on their own. **Fix** (`RomPortGraph.hpp`, no tolerance
  or law constant changed): (1) when the full step fails Armijo, each N node is first bounded by its own laws'
  kinks (cross by ≤ its distance, then just past it; the L block takes the exact linear response, so the L residual
  is zero after it), then the old global bound, then halving; with no decrease the per-node "just past" step is
  taken; counted as `node_bounded` (`[PH-ports] ... kink-bounded N (per node M)`); (2) a kink is placed at
  max(departure point, t_c/(2 − t_c)) from the crossing parameter. (3) Engine (`stepRom1dStorageMembers`): the
  floored lines and raised-end ports carry the dry factor at BOTH ends, admitted or not — the "admission closure"
  in its floor sense (a node draining into an empty store can no longer be pulled below its invert by the line;
  not testable on the tables, the full run checks it). **Replay after**: 242 of 252 by Newton without fallback (all
  150 reproduced A/B converge, ≤ 20 its, median 5); the 10 left are 9 NC carrying the sunk (C) state from earlier
  steps and 1 table artifact (an anchor knot at the solution; the writer no longer puts anchors at knots). Mutation:
  without (1) the A dumps fall back again, without (2) the B dumps do. Committed: 3 compacted dumps (1.7 MB,
  `tests/unit/engine/data/ph_dumps`, README there) in `CommittedBellingeDumpsConverge`. **Gates (Debug)**:
  member_ports 28, additive_channel 12, spectral_rom1d 101, replay 2 (+3 env-gated diagnostics), storage gate A
  0.889/0.958, B 0.727/0.960, R 0.952/0.880 unchanged (R Newton 61,269 vs 60,760: the raised end's far-end floor);
  1D smoke `ph_smoke1d.inp` to 07:00: 3,892 steps in 395 s, solves 194,550, Newton 302,004, fp-floor 970, Jacobi
  fallbacks 29 (all converged), not converged 0, kink-bounded 35,053 (per node 29,435). Full `demo_ph5` relaunched.
- 2026-11-05: Bellinge `demo_ph5` on O-6 code reached step 4000 (07:03): 456 Jacobi fallbacks, **14 non-converged**
  of 200,000 solves (0.007 %) — was 3,015 / 539 before O-6 and 6,350 / 607 before O-4. Wall 53,860 s is not a
  measurement (machine throttled to 26 % CPU and loaded by another session for most of it). Run continuing to
  10:00 for the score vs mc_v3. The 14 remain open (no dump dir on this run; a dump run on the O-6 code would
  classify them) — below the 1 % stop line but not zero.
- 2026-11-05: **Bellinge `demo_ph5` on O-6 code finished** (6,816 steps; wall 86,386 s is not a measurement —
  the machine was throttled/loaded for most of it). Solver: 340,750 member solves, 551 Jacobi fallbacks, 26
  non-converged (0.008 %), k up to 986 (admitted 405 nodes at the peak), 6,227 member-steps > 0.05 ft below
  invert, deepest −105.8 ft (G71O020 2,708, G70F11P 1,630 — class C is not closed). **Score vs mc_v3
  (per-member ON vs OFF = demo_v4)**: 1D all manholes 0.944/1.650 CONSERVATIVE (OFF 0.957/1.40 calibrated);
  trunk 0.811/1.200 (OFF 0.814/1.26); 2D street 0.665/0.155 ranking (OFF 0.698/0.21); P(>20 cm) Brier 0.0305
  (OFF 0.041, no-skill 0.0997). **Phase 5 verdict: stable enough to run, not better than OFF on the bands.**
  The hook stays OFF. Open: admission closure (class C), N-set growth at the peak (k 986 → the dense k×k LU is
  the remaining cost), the 26, and the 1D over-width 1.65 with members ON.
  Per-node (median 05–10 h, m; MC span / OFF / ON): G72R83B 0.062 / 2.995 / 0.999; G72R150 0.454 / 3.445 / 0.877;
  G72UD11 1.105 / 0.916 / 0.722; G70F11P 0.733 / 0.038 / 0.157; G71O020 0.007 / 0.033 / 0.076. **At the storage
  basins the per-member path is a clear improvement** (3–4× closer to the MC); the aggregate 1D over-width must
  come from the ~400 admitted rim manholes (step-3 admission), which also drive k to 986. Hypothesis: admit
  storage nodes + threshold outlets only (the step-2 scope) and leave rim manholes to the shared plane.
- 2026-11-05: **Score split by node class** (`bellinge-flood-uncertainty/scripts/score_mc_by_class.py`, demo repo
  2505f5c; OFF → ON, cov/width): STORAGE (16) 0.845/1.99 → 0.778/0.94; coupled near rim (64) 0.871/1.17 →
  0.861/1.26 (calibrated both); coupled far from rim (931, 18,073 of 19,949 samples) 0.964/1.42 → 0.952/1.71 —
  **this class carries the regression**, and not broadly: a tail of shallow laterals (G62F196/192/191, G70S214/213,
  G72F311, G62F403/012, G72F081, G71F043) with MC span 0–2 cm, OFF band 7–130 mm, ON band 1.4–2.3 m (100–300×).
  The rim-manhole hypothesis above is wrong. Something in the per-member path puts metres of head on tiny-storage
  laterals: candidates — the per-member correction C = z − κΨ spreading a store's outflow mismatch through the
  plane onto small-S nodes, the main-run volume-mismatch sharing, members sunk below invert (G71O020 −106 ft)
  leaking through the plane, or the source-floor port. O-7 (diagnosis in Debug) opened.
- 2026-11-05: **O-7 diagnosis — the lateral tail is a street-pond bookkeeping defect across sync batches, not
  the correction field.** CSVs (`demo_ph5` vs `demo_v4`): at all 10 nodes the ON band is one-sided (q95 moves,
  q05 stays at the main run), starts with the 06:20–06:40 burst that both runs share (OFF up to 1–2.5 m too,
  gone in 10 min), and then stays: 28–38 % of members (p_exceed of the crown, bimodal flag on) sit above the
  crown for hours with q95 at the manhole's RIM (G62F196 −0.17…+0.47 ft from rim 07–09 h, G72F311 ±0.4, G70S213
  ±0.2), decaying over hours (G62F196 8.3 → 6.6 ft by 09:50). q50 is not protected there (G70S213 08:20 +1.6 ft,
  G62F196 08:00 +1.0 ft): a one-sided per-member source moves the median; the zero-perturbation invariant still
  holds. Bands peak at the lateral HEADS and fall downstream (G62F196 2.43 → G62F195 0.77 m … G60F340 0.45), so
  not backwater from an admitted store, not (a)/(c). **2D output**: within 25 m of every one of the 10 nodes
  a cell holds an ON q95 pond of 0.12–2.1 m for hours where OFF has 0 (G72F311 0.948 m constant 07–08 h, pond
  surface 27.21 m = the rim; G62F196 0.74 m, surface 31.09 vs rim 31.10; G70S213 23 m at 07:00), i.e. member
  ponds level with the manhole rim, holding 100–250 m³ that a 1 m² manhole never passed. **1D smoke does not
  reproduce** (no 2D, no ponds: ph_o6 G62F196 band 0.03 ft at 06:40), nor does forcing the 10 laterals to be
  admitted junction stores in the smoke (temporary rim-criterion admission: bands identical to ph_o6), so
  stores, L recovery, volume-mismatch sharing, source floor and sunk members are ruled out on these nodes.
  **Mechanism (e)**: the 2D advances in sync batches (`couplingSyncSpan` = ROUTING_STEP 10 s, Bellinge DW steps
  average 5.3 s and 1–2 s at the peak, so 2–10 routing steps per batch) while `stepRom1dStorageMembers` runs every
  routing step. It (1) read each member pond's start level from the 2D ROM (`memberCouplingDepth`), which only
  changes at a batch, and (2) handed its exchange over with `setExternalCouplingVolume`, which REPLACED the
  previous step's volumes, so the batch booked only its last step. A pond above its manhole poured into it every
  routing step from the same un-depleted level (water created ~(N−1)× per batch; a manhole pouring into an empty
  pond lost the other way); the member manhole is held at street level, its rim overflow port loses the excess,
  and the pond empties only at 1/N of its outflow. Coupled fixture (COUPLING_SYNC 30 s, 5 s steps): worst member
  10.1 of 12.3 m³ unbooked. **Fix** (`SpectralROM.hpp/.cpp`, engine): `addExternalCouplingVolume` adds the
  routing steps' volumes until the batch books them (a NaN never overwrites a pending volume);
  `memberCouplingDepth` subtracts booked AND pending volume, so the next routing step sees the pond after this
  step's exchange; engine diagnostic `rom1dStreetExchangeVolume()` (Σ over routing steps of the per-member street
  exchange). No tolerance, knob or law constant. **Tests**: `PerMemberPortsCoupled.StreetExchangeIsBookedOnceUnder
  SyncBatches` (booked + pending − volume booked by the 2D's own law before admission = Σ per-step exchange:
  1.6e-14 of 10.8 m³ at sync 30 s, 3.4e-16 at every step; replace semantics: 10.1 m³ → fails) and
  `SpectralROM.ExternalCouplingVolumesAddUpUntilBookedAndShowInThePond` (sum booked once, pending in the pond
  level; fails on either mutation: replace, or pond level without pending). The requested "laterals within 2× the
  shared-plane band" test was tried on the coupled fixture (rain 10–80 mm/h, 0.15–0.3 m pipes, 30–80 m, sync 0/30 s):
  no small fixture shows the blow-up before the fix (its pond never sits above the manhole long enough), so the
  pin is conservation, not the band. **Gates (Debug)**: member_ports 29, additive_channel 12, spectral_rom1d 101,
  2d_spectral_rom 65, 2d_rom_nonintrusive/integration/router_operator pass, storage gate A 0.889/0.958, B
  0.727/0.960, R 0.952/0.880 unchanged; 1D smoke to 07:00 (probe recompiled against the new header) bit-identical
  to ph_o6 (`.uncertainty.csv` cmp-equal): 3,892 steps, 194,550 solves, Newton 302,004, Jacobi 29 (all converged),
  not converged 0, wall 290 s (ph_o5: 35/0/336 s). The coupled Bellinge was NOT rerun (owner decision); whether
  the fix removes the tail there, and what it does to the 1D 0.944/1.650 aggregate, is open.
- 2026-11-06: **Bellinge `demo_ph5` on O-7 code finished** (6,816 steps; 39,755 s wall incl. sleep/throttle gaps —
  not a measurement; `caffeinate -w` brought the last 3 h to ~126 CPU-min per 80 wall-min). Solver: 340,750 member
  solves, Newton 1,137,914, Jacobi fallbacks 475, non-converged 28 (0.008 %), members > 0.05 ft below invert 4,387
  (6,227 before), deepest −105.9 ft (unchanged: class C still open). **Score vs mc_v3 — the O-7 pond-booking fix
  removes the 1D regression**: 1D all manholes **0.947/1.416 CALIBRATED** (OFF 0.957/1.40; O-6 ON 0.944/1.650);
  trunk 0.815/1.210 (OFF 0.814/1.26); 2D street 0.620/0.158 ranking (OFF 0.698/0.21; O-6 ON 0.665/0.155);
  P(>20 cm) Brier 0.0191 vs no-skill 0.1113 = 0.17 (OFF 0.041/0.106 = 0.39; O-6 ON 0.0305/0.0997 = 0.31). Reliability
  95–100 % class → 89.5 % observed (940 cells), 50–75 % → 44 %. By MC span: 5–20 cm 0.910/1.776 conservative,
  20–100 cm 0.795/1.216 ranking only (as with OFF). Phase 5 with the hook ON is now at least as good as OFF on
  every 1D aggregate and clearly better on flood-probability skill; the 2D street band coverage did not improve
  (0.62 vs 0.70). Decision pending: make per-member the default (hook deleted) vs keep OFF — see summary.
  By class (OFF → ON, cov/width): STORAGE (16) 0.845/1.99 conservative → **0.801/0.805 CALIBRATED**; coupled near
  rim (64) 0.871/1.169 → 0.860/1.174; coupled far from rim (931) 0.964/1.419 → 0.956/1.447 (calibrated, was
  conservative 1.71 before O-7). Summed band change ON−OFF is negative in every class (storage −425 m, near rim
  −306 m, far −370 m). The shallow-lateral tail is fixed at G62F196/192/191 (2.27/1.87/1.71 m → 0.012/0.036/0.031),
  G72F311, G62F403, G72F081 (cm scale). **Four nodes remain 1.2–1.6 m wide with MC span ≈ 0**: G70S213 (1.640),
  G70S214 (1.503), G62F012 (1.477), G71F043 (1.161). G71F043 is the node that goes below its invert in the smoke
  (258 member-steps) — a second mechanism (sunk member / class C), not the pond booking. Basins: G72R83B 2.995 →
  0.363 (MC 0.062), G72R150 3.445 → 0.991 (MC 0.454), G72UD11 0.916 → 0.228 (MC 1.105: now too narrow),
  G70F11P 0.038 → 0.138 (MC 0.733: still narrow).
- 2026-11-15: O-8b (depth-1 admission closure; `RomAdmissionClosure.hpp`, uncommitted). The O-8 component rule is replaced, as
  THE rule (no knob), by one hop: a non-admitted node is admitted when it is on the boundary (the far end of a ONE_WAY, RAISED
  or FREE link whose other end is admitted; the engine floors those lines at both ends) AND its component of the free graph
  among non-admitted nodes is ungrounded (no outfall, no grounded flag, no free link to the outside). Only the boundary node
  is admitted; the interior inherits the floor through free conduits and the plane. Engine call site unchanged (once per growth
  of the admitted set, sticky). **Tests** (member_ports 34): chain behind a pump admits only the node directly behind (the
  component rule would admit U1 too; asserted), two basins via a raised pipe admit B2 not its lateral, a lateral behind a floored
  line admits L1 not L2/X, decision (d) test (a node behind a one-way port WITH another free path to the outfall stays linear,
  without one is admitted), random graphs (boundary only, ungrounded by independent walk, left-out boundary nodes grounded,
  repeated application terminates). **Gates (Release)**: member_ports 34, port_graph_replay 2, additive_channel 12,
  spectral_rom1d 101, storage gate A 0.889/0.958, B 0.727/0.960, R 0.952/0.880 (unchanged). **Release 1D smoke** (`ph_smoke1d`
  to 07:00, 4 threads): 3,892 steps, wall **59 s** (O-8 component rule Debug: 5,968 s at step 3200), k_max **42** (827), Newton
  298,626, Jacobi fallbacks 1 (all converged), not converged 0, admitted nodes 27 (closure added 17), store member-steps > 0.05 ft
  below invert 95 (G80F11B 90, G72R33Bb 5; deepest -0.123 ft; O-7 smoke: G80F11B 92). **Not closed**: the any-node line
  (non-admitted, linear nodes) is 1.99 M member-steps, deepest -2044.5 ft; closure OFF (`OPENSWMM_PH_NO_CLOSURE=1`) gives 2.02 M, same
  deepest, and G71F06R_ovl (22,842) is the only node the closure removes from the list. G70F11P 21,466, G71O020 18,814, G80F13P,
  G72R211, G73R45x remain. G71O020 has a free conduit to G71U02F, so decision (d) leaves it linear, yet it sinks: the reversed
  one-way line from G71F12P (side orifice, kept as q* + d*z_up) out-drains the free path. Rule (d)'s premise does not hold for
  that port; the fix is on the one-way line (treat a ONE_WAY port's far end as floored irrespective of other paths), not on depth.
- 2026-11-15: O-9 (one-way structures and pumps are floored ports; `stepRom1dStorageMembers`, tests `OneWayPorts.*`, uncommitted).
  **Law.** A weir, orifice, outlet or pump with an end that is an unknown of the member graph (an admitted node, or the end of a
  port already made) is a port with the monotone rating floored at zero in its own direction (`flowAtShiftedDepth` table, the (b1)
  depth-rated pump table, `anchoredRatedFlow`): Q_i = max(0, Q_law), δQ = Q_i − Q* >= −Q*. The promotion repeats while it adds ports
  (each makes its far end an unknown); a link is a port or a plane edge, never both. A one-way edge whose table cannot be built
  (pump of another type, non-finite rating) and that still falls to the generic floored line is now floored at zero too
  (`max(q* + d·z_up, 0)`), where it used to run backwards. **Findings that change the premise of the task** (measured, Release 1D smoke
  to 07:00, 4 threads): (1) every structure that touches an ADMITTED node was already a port; the literal step 1 changes one link on
  Bellinge, the pump G80F13Pp1 behind the outlet G80F310 -> G80F13P (G80F13P leaves the any-node list). (2) G71F12P, G70F11P,
  G80F13P and G60F60Y are not admitted: the admission seeds are STORAGE nodes with a weir/orifice/outlet outlet, a wet well that is a
  junction behind pumps is never a seed, and the closure only reaches nodes next to an admitted one (nearest admitted node by hops:
  G71F12P 2 via the pumps to G71F68Y, G71O020 3, G70F11P 12, G60F60Y 50). Their pumps and the side orifice G71F12Pw1 -> G71O020 are
  L-L plane lines (13 of 20 non-conduit plane edges at step 3000 have no admitted end, 7 have). (3) **The 1.99 M "linear node below
  invert" member-steps are 90 % the shared plane's own level**: at the deep nodes u_i (the additive planes' κ_i·Ψ at member 9/39,
  rain deficit) is -50 ft before the member solve (G72F800 z -55.97 u -55.89; G73F113 -51.59 / -51.33) and is the same with per-member
  OFF (the reconstruction clamps it at the invert). New diagnostic: member-steps below the invert only through the per-member
  correction (plane level >= -0.05 ft): O-8b code 183,130 (deepest -160.5 ft: G72R211 13,169, G72R644 7,468, G72R210, G72R190,
  G72R180...), O-9 191,174 (same deepest; top-40 nodes 170,320 -> 170,716). So the any-node count cannot be made to fall by orders of
  magnitude by a port law; that target is not met. **Gates (Release)**: member_ports 37 (34 + 3), port_graph_replay 2,
  additive_channel 12, spectral_rom1d 101, storage gate A 0.889/0.958, B 0.727/0.960, R 0.952/0.880 (unchanged). **Smoke**
  (O-8b -> O-9): wall 59 -> 48 s, k_max 42 -> 43, Newton 298,626 -> 298,801, Jacobi fallbacks 1 -> 0, not converged 0 -> 0, admitted
  27 -> 27, store line 95 (G80F11B 90, G72R33Bb 5, deepest -0.123 ft) unchanged, any-node 1,991,191 -> 1,982,368 (deepest -2044.5 ft,
  G70F11P 21,466 and G71O020 18,814 unchanged). **Tried and not adopted (each measured, patch in the session scratchpad)**:
  (a) promoting L-L structure/pump edges to ports without admitting their drained node: the node is an unknown with no floor, runs
  away (any-node deepest -22,111 ft, correction-only -10,380 ft: G71F12P z -22.6 with u -1.7, G60F60Y -49): the old plane line was a
  phantom source that held it up; (b) (a) plus admitting every node a structure or pump drains (28 more admitted, closure +17 more,
  k 93, wall 86 s, 1 not converged): G71F12P/G70F11P/G80F13P leave the list but the store G70F11P goes to -2.61 ft (797 member-steps)
  and correction-only rises to 287,692. Cause of the store sink, traced at G70F11P t=21375: the main run is a dry well (d* = 0) whose
  inflow the dynamic wave has not stored yet (V* does not rise with dt·net*), so the member's inflow deficit relative to that flow
  (-q0·dt on two parallel pipes) is booked against zero main-run water and gives negative member water; the storage accounting
  (`store_scale`) shares a main-run loss only when the store holds water (Wold > δ_dry·A). **Open (owner)**: seeding admission from
  one-way structures/pumps on junctions needs that empty-store accounting fixed first. **Remaining sunk nodes after O-9 (correction
  only, with nearest admitted node)**: G72R211 (G72R410, 1 hop, free conduit into the admitted store), G72F020 and G72F030 (G72K020,
  1-2 hops; the line to K020 carries -28 cfs reversed), G72R210/190/180/170/165, G72R644 (G72R643), G71F147, G71F043/G71F044 (G71F68Y,
  4 hops through pumps); all behind conduit lines, not structures: a second mechanism, not touched here.
- 2026-11-16: **first unthrottled coupled Bellinge run** (O-9 code, per-member ON, caffeinate, load ~4): step 1000 at
  497 s, 2000 at 1,110 s, 3000 at 3,830 s; k_max 524 → 675 → **1,717** (O-6: 986; 1D smoke 43). Steps 2000→3000
  took 2,720 s vs 215 s on O-6 code. Cause: on the coupled network the depth-1 closure admits the far end of every
  street-pond orifice port, so N takes in the ~700 coupled manholes and their ponds, and the dense k×k Schur LU is
  O(k³) per Newton iteration. The per-member cost line (≤ 1.5× OFF) is failed by construction with k ≈ 1,700; the
  fix is structural, not a knob: either exclude pond-orifice far ends from closure (the pond store itself is already
  floored, so the orifice cannot run backwards there) or make the Schur solve sparse. Run continuing for the score.
- 2026-11-16: O-10 (the street pond eliminated analytically: one composite port per coupled manhole;
  `PondOrificePort` in `RomStorePorts.hpp`, `stepRom1dStorageMembers`, tests `CompositePondPort.*`, uncommitted).
  **N composition, measured** on the 252 committed Bellinge failure dumps (`SWMM6/data/ph_dumps`, coupled storm peak, O-6 code; the
  dumps carry node names and store flags; reader in the session scratchpad): mean k 380 (max 619) = 143 pond nodes + 143 admitted
  stores + 94 linear port ends (max: 239 + 239 + 141). (a) admitted stores that are NOT coupled manholes: 0 of 143, every admitted
  store in those dumps has a pond; (b) pond nodes: 143 = 38 % of k; (c) coupled manholes in N only as the near end of a pond orifice:
  none, a manhole with a pond is always an admitted store itself (the `near` rule admits it; the pond port is created only for
  admitted nodes); (d) closure/promotion far ends: the 94 linear port ends, none of them next to a pond only (0 of 94). **The O-9
  log entry's cause ("the closure admits the far end of every pond orifice port") is not what the code does**: the closure's link
  table has no pond links, so the ponds themselves are the extra half of k. **Law.** The pond has one port (the orifice), so its
  level z_p* at the end of the step is a scalar function of the manhole's level z_m: a(z_p − z_p^n)/dt = E(z_m, z_p), E the orifice's
  extra flow (the O-9 port law, unchanged). The left side minus the right is strictly increasing in z_p, one root, bracketed by z_p^n
  and z_p^n + dt·E(z_m, z_p^n)/a; found to roundoff by a safeguarded regula falsi (bisection every fourth step), because Newton
  differences the law at 1e-7 ft and its kink test compares slopes to 1e-6. The composite port is manhole -> street (dn = −1) with
  Q(z_m) = E(z_m, z_p*(z_m)): nondecreasing in z_m, 0 at the main run, and the pond gains exactly dt·Q, so the booking
  `ext_vol = −portVolume` is unchanged. Per-port state is only the member's pond start level (read from the 2D ROM per member,
  into a slot vector); the pond is not an unknown, not a store, not a graph node. **Tests** (member_ports 37 → 41): the law solves
  the pond equation to roundoff (4,000 draws, wet and dry ponds, both flow directions), is monotone, anchored (bit-exact 0),
  |Q| <= |E(z_m, z_p^n)|; composite vs the O-9 two-unknown graph (kept as the test reference: `makeRandomGraph(..., pond_mode 1)`)
  on 60 random graphs, 303 ponds, half with one-way ports: member levels worst 6.7e-11, pond level 6.0e-11 (relative to max(1, |z|)),
  booked volume 1.1e-9 of the scale (the reference stops at its own pond residual 1e-10 ft·a, which the tolerance carries: the
  composite is exact in that equation); conservation a·(z_p − z_p^n) = dt·Q; zero data exact with no pond node (n = 30); order
  preservation and ℓ1 contraction of network plus pond water with composite ports (worst 0.9888). Mutation (composite flow
  evaluated at the pond's start level): the law test and the equivalence test fail. Unchanged: `StreetExchangeIsBookedOnceUnderSyncBatches`
  (2e-14 / 4e-16 m^3), `ZeroPerturbationAddsNothingThroughPerMemberPath`, `DeterministicRunBitIdenticalWithCoupledStores`.
  **Gates (Debug)**: member_ports 41, port_graph_replay 2 (the dumps are self-contained tabulated graphs with the pond already a
  linear node, so the reader and the test need no change), additive_channel 12, spectral_rom1d 101, 2d_spectral_rom 65, storage gate
  A 0.889/0.958, B 0.727/0.960, R 0.952/0.880 (unchanged). **k on the coupled fixture**: 3 → 2 (k x k factorisations 21,146 → 15,953,
  Newton iterations per step 44.4 → 37.9); ON/OFF wall 1.87 → 2.03 on that 1-manhole fixture (the root solve costs more than the
  one removed unknown saves at k = 2). **Debug 1D smoke** (`ph_smoke1d` to 07:00, load from a Release run alongside): 3,892 steps,
  wall 364 s, Newton 298,801, Jacobi fallbacks 0, not converged 0, k_max 43, admitted 27 (closure +17), store line 95 (deepest
  −0.123 ft), any-node line 1,982,368 (−2044.5 ft): every counter equals O-9's (no pond in the 1D smoke). **Expected Bellinge k at
  the coupled peak**: k = 2S + E before, S + E after, with S the admitted coupled manholes and E the linear port ends. With the
  dumps' E/S = 0.66, O-9's k_max 1,717 means S ≈ 645, E ≈ 425, after ≈ 1,070 (−38 %); the dense Schur LU is O(k^3), so the
  factorisation falls to about 0.24 of its cost (1,000–1,100 is the range if E is nearer 0.5–0.8 S). That is not the ≤ 1.5× OFF
  line: k stays about S + E ≈ 1,000. The remaining levers are structural and not taken here: the `near` admission of coupled
  manholes (S itself), or a sparse Schur solve. Not run: the coupled Bellinge.
- 2026-11-16: O-11 (sparse Newton path; `RomPortGraph.hpp`, `RomAdditiveChannel.hpp`, tests `SparseNewton.*`, uncommitted). The per-member
  Newton system is the same linear system in two factorisations: the dense k×k Schur complement (O(k²) to form, O(k³) to factor per pass) or,
  above `PortGraph::k_dense` N nodes, the FULL (N ∪ L) Jacobian in a sparse LU. `AdditiveSystem::refactor(op, dt, pinned)` is new: numeric
  factorisation on the cached min-degree pattern (no validation, no pattern growth, no symbolic step; a pair the pattern lacks, or a first
  call, falls back to `build()`); `build()` is the same code split into validation + pattern growth + `assembleAndFactor_` (the planes are
  bit-identical, gates below). `prepare()` (once per graph) registers the pattern: plane-edge terms plus every port's pair (slopes 0); in
  sparse mode it skips the Schur block (no Jbase, no W rows: `schurBase()` is empty above k_dense, dumps carry an empty Schur base, as with
  `with_schur = false`). Per Newton pass: Q = store areas (N) / S (L), the ports' terms refilled from the pass's slopes (`ju >= 0`, `-jd >= 0`:
  still a column-dominant M-matrix, so no pivoting), right-hand side −F on N and −c·F_L0 on L (the L residual scales by c along a step, as
  before), one `refactor` + one forward/backward pass; dz_N and dz at the boundary L nodes are read off the solution. A capped (per-node
  bounded) step gets its boundary-L response from one L solve instead of the stored W rows. Everything else (kink bound, active set, Armijo,
  roundoff floor, Jacobi fallback, tolerances) is the same code. New counter `sparse_factorisations` (`factorisations` stays the k×k count).
  **Crossover** (`SparseNewton.CrossoverMicroBenchmark`, Release, chain-like network of 1,300 nodes, k stores each draining out of the
  system, prepare + 4 members, ms per Newton iteration, dense | sparse): k 10 0.20 | 0.39; k 30 0.17 | 0.35; k 100 0.33 | 0.37; k 300 1.27 | 0.33;
  k 1000 10.3 | 0.56 (257 ms vs 14 ms for the 25 iterations). The sparse cost is the network, nearly flat in k; the dense cost is k³. They meet at
  k ≈ 100: `k_dense = 100` (dense for k <= 100; the rule needs no n because a smaller network only favours sparse). The unit test asserts
  sparse < dense at k = 1000 (Release only). **Equivalence** (`MatchesTheDenseSchurPathOnRandomGraphs`): 50 random graphs (generic random graphs
  with loops, weirs, raised ends, composite ponds, floored one-way ports, pins; tree-like graphs with up to 453 stores/N nodes), 3 members each:
  worst max|dz|/max|z| 8.5e-14, Newton iterations 1,265 = 1,265, Jacobi fallbacks 0 = 0, k <= 453, 39 graphs with ponds. Replay
  (`CommittedBellingeDumpsConverge`) now runs every committed dump on both paths: both converge, 0 fallbacks. **Gates (Release)**: member_ports
  43 (41 + 2), port_graph_replay 2, additive_channel 12, spectral_rom1d 101, storage gate A 0.889/0.958, B 0.727/0.960, R 0.952/0.880, SR-5
  0.921/1.029, SR-7 0.922/0.969, CL-1e 0.941/1.026 (all identical). **Release 1D smoke** (ph_smoke1d to 07:00): 55 s (O-10 Release ref. 48 s, same
  machine class, load from other work), k 43 (dense path), Newton 298,801, 0 Jacobi fallbacks, 0 not converged, k x k factorisations 413,968,
  sparse factorisations 0.
  **Coupled Bellinge (`demo_ph5`, Release, per-member ON, OMP 4, caffeinate)**: wall at step 1000/2000/3000/4000/5000/6000 = 455 / 865 / 1,124 /
  1,709 / 2,144 / 2,530 s, done 6,816 steps in **2,879 s** (O-9 code 497 / 1,110 / 3,830; O-6 594 / 3,775 (throttled) / 3,998; steps 2000 -> 3000
  took 259 s against 2,720 s). k_max 1,011. `[PH-ports]`: member solves 340,750, Newton 1,079,305 (Armijo rejected 21, kink-bounded 160,884 (per
  node 102,138), active-set re-solves 1,129,236, fp-floor 1,114), **Jacobi fallbacks 126, not converged 10** (the counters step from 93/10 at
  step 4000 to 106/10 at 5000 and stay there), zero-data exits 1,147, k x k factorisations 0, **sparse factorisations 2,208,541**, L factorisations
  4,928, L solves 2,699,837, admitted nodes 1,011 (admission closure added 877), store member-steps > 0.05 ft below invert 21,968 (deepest -19.3 ft,
  G72R52X 18,222 of them), any-node 87,974 (deepest -25.5 ft). The OFF wall is not recorded (`demo_v4.log` has progress lines without wall).
  The solution is the dense path's (equivalence above), so none of the following is an O-11 effect; it is what O-8..O-10 produce at the storm peak.
  **Scores vs mc_v3** (O-7 in brackets): 1D all manholes 0.937/1.247 (0.947/1.416); storage 0.750/0.585 (0.801/0.805), OFF 0.845/1.988; trunk
  transect 0.788/1.152; P(>20 cm) Brier 0.0160 vs no-skill 0.1311 (0.0191 vs 0.1113; the no-skill term differs between the two scorings of the
  same cells: not chased); 2D street 0.511/0.156 (0.620/0.158), by span 20-100 cm 0.310/0.156. Node bands (median 05-10 h, MC span / OFF / ON, m):
  G72UD11 1.105 / 0.916 / 0.096, G72R83B 0.062 / 2.995 / 0.106, G72R150 0.454 / 3.445 / 0.399, G70F11P 0.733 / 0.038 / 0.170, G71O020 0.007 /
  0.033 / 0.012, G70S213 0 / 0.100 / 0.069, G70S214 0 / 0.126 / 0.046, G62F012 0.019 / 0.088 / 0.054, G71F043 0.005 / 0.016 / 0.021. Reading: the
  cost line is met (the full run is 48 min instead of the 4–11 h of O-6/O-7/O-9); the accuracy fell relative to O-7 (storage and 2D coverage),
  and the 10 non-converged member solves plus the one deep store (G72R52X, -19 ft) are the open items; G72UD11's ON band (0.096 m) is 11 x narrower than the MC's (1.105 m;
  OFF 0.916 m). The closure admitted 1,011 nodes (every node a store): that, not the Newton cost, is what to look at next.
- 2026-11-16: O-12 (the FREE clause removed from the admission closure; `RomAdmissionClosure.hpp`, tests `AdmissionClosure.*`, uncommitted).
  **Decision, from the code**: in `stepRom1dStorageMembers` the plane-edge loop turns EVERY conduit that touches an admitted node into a
  port floored at BOTH ends (`dryFactor(dU, zu)` and `dryFactor(dD, zd)` on any end that is an active node; O-6). So the far end of a
  plain two-way conduit from an admitted store already has its own floor through that line, and the plane handles the rest; no distinct
  FLOORED_LINE kind is needed (every FREE conduit touching an admitted node IS a floored line). Only ONE_WAY and RAISED far ends, where
  water cannot come back, need a floor of their own. FREE links stay in the function as the grounding/component links. Cause of the
  O-11 regression: with the FREE clause each admitted node made its free neighbours boundary nodes and sticky admission repeated it:
  1,011 of 1,011 admitted. **Tests** (member_ports 44): (a) re-written (chain behind a pump admits nothing through the two-way
  conduit; a ONE_WAY line into U2 admits U2 only), (c) re-written (lateral behind a floored two-way line stays linear, with or without a
  grounded flag; a ONE_WAY far end still joins), NEW `TwoWayChainAroundOneAdmittedStoreAdmitsNothing` (20 junctions, two-way
  everywhere, one store; mutation = FREE clause: admits both neighbours, the chain under repetition), random-graph test's boundary
  definition restricted to non-FREE links. **Gates (Release)**: member_ports 44, port_graph_replay 2, additive_channel 12,
  spectral_rom1d 101, storage gate A 0.889/0.958, B 0.727/0.960, R 0.952/0.880 (unchanged). **Release 1D smoke** (ph_smoke1d to 07:00):
  48 s (O-11 55 s), k <= 33 (43), admitted 18 (27), closure added 7 (17), Jacobi fallbacks 1, not converged 0, store member-steps > 0.05 ft
  below invert 96 (deepest -0.123 ft). **Coupled Bellinge (`demo_ph5`, Release, ON, OMP 4, caffeinate)**: wall at step 1000/2000/3000/4000 =
  444 / 836 / 1,042 / 1,430 s (O-11 455 / 865 / 1,124 / 1,709), 5000 1,750, 6000 2,033, done 6,816 steps in **2,290 s** (O-11 2,879).
  `[PH-ports]`: member solves 340,750, Newton 1,096,844, Jacobi fallbacks 218, **not converged 6** (O-11 126/10), k_max **574** (1,011),
  sparse factorisations 2,174,908, **admitted nodes 407, closure added 16** (1,011 / 877), store member-steps > 0.05 ft below invert 3,153
  (deepest -105.9 ft; G70F11P 1,630, G71O020 460, G71R090 317, G80F11B 113; O-11 21,968, deepest -19.3 ft, all G72R52X), any-node 400,083
  (deepest -125.0 ft; O-11 87,974, -25.5 ft), monotone envelopes on 4 links. G72R52X no longer sinks (not in the store list; band 0.037 m vs MC
  0.273). **Scores vs mc_v3** (O-7 / O-11 in brackets): 1D all manholes 0.945/1.416 (0.947/1.416 / 0.937/1.247); storage 0.795/0.795
  (0.801/0.805 / 0.750/0.585); trunk transect 0.798/1.188 (- / 0.788/1.152); P(>20 cm) Brier 0.0202 vs no-skill 0.1112 (0.0191/0.1113 /
  0.0160/0.1311); 2D street 0.620/0.158 (0.620/0.158 / 0.511/0.156), by span 20-100 cm 0.319/0.158. Node bands (median 05-10 h, MC / OFF /
  ON, m): G72UD11 1.105 / 0.916 / 0.225 (O-7 0.228; O-11 0.096), G72R83B 0.062 / 2.995 / 0.393 (0.363), G72R150 0.454 / 3.445 / 0.966
  (0.991), G70F11P 0.733 / 0.038 / 0.134 (0.138), G71O020 0.007 / 0.033 / 0.012, G70S213 0 / 0.100 / 1.624, G70S214 0 / 0.126 / 1.534,
  G62F012 0.019 / 0.088 / 1.398, G71F043 0.005 / 0.016 / 1.152. Reading: the accuracy is back to O-7's; the cost is 2,290 s. **OFF (hook 0,
  `demo_v4.inp`) wall 1,663 s (474 / 885 / 1,067 / 1,186 at 1000..4000 steps)**: ON/OFF = **1.38**, inside the 1.5 target.
  Open: the ON bands at G70S213/G70S214/G62F012/G71F043 (MC 0-0.02 m, ON 1.1-1.6 m, OFF 0.01-0.13 m) are the largest widenings; 6 non-converged
  member solves; 4 deep store sinks (G70F11P, G71O020, G71R090, G80F11B).
- 2026-11-16: **cross-vendor review received** (phases 2 and 5, source + tests + logged Release results; no rerun).
  Phase 2: no blocker (A-orthonormality, generalized eigenrelation, null-area exactness, volume projection,
  non-negative reduced energy all covered; relative-channel depth Gram coherent with its own trial space HP);
  H15 corr20 stays the shipped-recipe result, the obsolete recipe is not restored as a switch. Phase 5 BLOCKER:
  `PortGraph::solve()` returns false when Newton and the Jacobi fallback both fail, but
  `stepRom1dStorageMembers` ignores it and publishes the failed levels/correction (6 member-steps in the final
  run) → R-1: a failed member solve is rejected for that step (C_i = 0, member rides the shared plane, no per-member
  exchange booked, pond slot unchanged, counted) + an engine test that forces a failure. Non-blocking: a focused
  property test for the main-run volume-mismatch sharing (zero data, balance, order, clamp ≤ 1) → in R-1.
  Default decision per the review: keep the hook OFF until R-1 lands and the owner accepts the residuals
  (storage coverage 0.795 < 0.80; four shallow laterals 1.1–1.6 m vs MC ≈ 0; G72UD11 narrow).
- 2026-11-16: R-1 (review blocker: a failed member solve was published; uncommitted). **Outcome as implemented**: in `stepRom1dStorageMembers`
  `solve()`'s return is used. A failed member-step (Newton and Jacobi both fail) is REJECTED: C_i = 0 and z_i = the plane level u_i (so the
  store state `rom1d_mem_z_`, `rom1d_mem_dv_` and `rom1d_nl_y_` carried to the next step is the plane's, not the iterate's); no street exchange
  is booked (its `ext_vol` slot stays NaN, skipped by `addExternalCouplingVolume`; the 2D ROM's own path handles it as for a non-admitted
  point); the pond start level is read from the 2D ROM each step and is never written from a solve; counted in `rom1d_pg_rejected_`
  (`[PH-ports]`: "member-steps rejected"). No retry, no knob. Documented in PH_STORES_AND_PORTS §3 "As built (R-1)".
  **Tests** (member_ports 47): `PerMemberPortsCoupled.FailedMemberSolveIsRejectedAndRidesThePlane` (seam
  `rom1dForceMemberSolveFailureForTest`, test only: starves one member's Newton/Jacobi at one step; asserts rejected counter 1 and
  `jacobi_failed` +1, C_i exactly 0 at every node, other members bit-identical to an unfailed twin run, the published store level equals the
  twin's level minus the twin's correction, no street hand-over for the member that step (the twin's is > 0), the next step books again, no
  further rejection or failure). Mutation (publish the failed iterate and book its exchange): 5 failures. NEW `StoreVolumeMismatchSharing.*`
  (the non-blocking gap; fixture basin behind a zero-crest weir draining to empty, rain-only soft forcing, 541 steps, 264 loss / 276 gain steps,
  31 near-dry): (a) zero data => correction, volume deviation and level exactly 0; (b) per step and store, carried dV_i = s*dV_old_i exactly,
  and for a loss the fraction (V_old_i - V_carried_i)/V_old_i = 1 - s is the same for every member (V_i = W + dV_i, V_carried_i = s*W_old + carried_i);
  (c) carried order equals old-deviation order, and the rank of member levels fixed at the first spread holds with no violation through the
  dry threshold; (d) 0 <= s <= 1 and |carried_i| <= |dV_old_i| (never scaled up). Mutation (drop the clamp's upper bound): (d) fails
  (s = 1.00011 at step 91). New read-only diagnostics: `rom1dStoreCarryScale/WaterBefore/WaterAfter/CarriedVolumes/MemberStoreVolumes`.
  **Gates (Release)**: member_ports 47, port_graph_replay 2, additive_channel 12, spectral_rom1d 101, 2d_spectral_rom 65, storage gate A 0.889/0.958,
  B 0.727/0.960, R 0.952/0.880 (unchanged). **Release 1D smoke** (ph_smoke1d): 37 s (O-12 48 s; machine quieter), k <= 33, admitted 18, closure
  added 7, 1 Jacobi fallback, not converged 0, member-steps rejected 0. Coupled Bellinge not run (as instructed).
- 2026-11-18: O-13 (the four wide shallow laterals and the narrow G72UD11; `RomStorePorts.hpp` `HeadBound`, `stepRom1dStorageMembers`,
  per-node trace, tests `HeadBound.*`, uncommitted). **Data first** (O-12 CSVs, mc_v3, demo_v4.inp, 2D h5): at G70S213, G70S214, G62F012,
  G71F043 the ON band is one-sided upward (q95 +4…+9 ft for hours, q05 at the main run), q50 is NOT protected (G70S213 08:00 +1.6 ft,
  downstream G70F212 +2 ft for 2 h); no admitted store, structure, pump or raised end within 3 hops of G70S213/G62F012 (plain 0.15–0.28 m
  laterals); every one has an ON 2D pond within 25 m (q95 0.2–0.6 m, decaying over hours; OFF 0 after 07:00; main run dry). MC at 06:30:
  G62F012 +0.46 m, G71F043 ±0.65 m, G70S213/S214 ~0 — the shared plane at 06:30 gives +1.5 m (OFF and ON alike).
  G72UD11 is a JUNCTION (sump behind G72R83B's raised pipe end, 30.48 m), with the adverse chain G72R580/570/560 behind it; MC is
  one-sided DOWNWARD there (members that the basin's 4–6 cm spill has not yet filled: −1.1 m at 08–09 h, +0.04 above the crest).
  **Per-node trace** (new, diagnostic only: `OPENSWMM_PH_DUMP_NODES=a,b,..` + `OPENSWMM_PH_DUMP_DIR` [+ `OPENSWMM_PH_DUMP_T0`] writes
  `node_trace.csv`, one row per member solve and named node: class store/port end/linear, d*, S, b, plane level u, z_prev, z, C, and the
  extra flow on every incident edge and port at the solved levels, plus the store's carry scale and water; and ≤ 40 full graph dumps
  (RomPortGraphDump format) of solves lifting a traced node > 1 ft). Coupled Bellinge Release to 07:46, 13 nodes, 530k rows
  (`SWMM6/data/o13_diag`). **Classification**: (A) G70S213 is a LINEAR node (port end, not admitted until 07:00), its band is the
  correction C (u = 0), not κΨ: the store G70S214 above it (dry main run, member held at its rim by its pond: 100–200 cfs in from the
  street, the same out over the rim) drains through the dry pipe's port, whose law Q_n(y_up) ignores the downstream level — it delivered
  the pipe's full bore, 0.99274 cfs, constant, for 1,000+ s into G70S213 while G70S213 rose to +89 ft (no rim port on a linear node);
  at 07:00 G70S213 is admitted and its pond is filled by the 2D ROM's own exchange from that absurd head (the 23 m q95 cell). G71F043:
  admitted store fed 1.3 cfs by G71F044 (a straight line, ju 0.9–1.3, jd −0.16…−0.5) while standing 1.8 ft ABOVE G71F044 in member head;
  the plane under it reached u = 67 ft at 06:37. (B) G62F012 (and the persistent part at all four): before admission both G62F014 and
  G62F012 are linear (C = 0) and the plane carries them +3 ft in the 06:25 surcharge (S = MIN_SURFAREA, rain-channel source 0.3–0.9 cfs);
  admitted as near-rim manholes the members spill into the street; afterwards the member pond drains back through the manhole at the
  pipe's capacity (pond port −1.0…−1.4 cfs in, pipe ~1 cfs out, manhole held at street level for hours) — physical given the pond, but the
  pond is the plane's 3× surcharge overshoot made into real water. (C) G72UD11: plane deficit u q05 −1.2…−1.6 ft vs MC −3.6 ft; the
  solve halves it again: 1D-smoke trace of the store's carry scale s (loss shared in proportion, gain equally) shows 526 loss / 317 gain
  steps with cumulative product of s = 0.51 — the alternating-sign main-run mismatch ratchets every member deviation down. Candidates
  (a) empty-store accounting, (c) the G71F68Y pump law, (d) dry-main-run pond port: not the cause at these nodes (G72UD11's members are
  at the floor only in the deficit tail; G71F043's inflow is the line from G71F044, not the pumps; ponds are wet in members because of
  (A)/(B), the composite law itself books correctly). **Fix taken: (A), as a law**: every conduit law in a member (dry-pipe rating, free
  end, the floored straight line) is bounded by the full-bore friction flow at the member's own head over the downstream control,
  Q ≤ K_b·s(h_up − max(h_dn, z_dn,end)), K_b = PHI/n·max(A·R^(2/3))·nbar/sqrt(L) raised to q*/s(H0) where the main run carries more
  (anchor exact), off where the main run's own H0 < δ0. Q = min(law, bound): monotone, 0 once the member's downstream water reaches its
  upstream water. No knob, no tolerance. **Tests** (member_ports 47 → 49): `HeadBound.AnchoredMonotoneAndZeroAgainstTheMembersHead`
  (anchor exact for q* below and above the full-bore flow, off when drowned/adverse, zero at h_dn ≥ h_up, monotone on a 350×390 grid),
  `HeadBound.DryPipeIntoALinearNodeNeverDeliversUphill` (G70S214 → G70S213 fixture: pond-fed store with a rim, dry pipe into a linear
  node with no outlet, 2,000 steps: max h_B − h_A = −1.4e-11 ft, B fills to A's level 10.9 ft; without the bound 3.3e3 ft; zero data
  exact). Mutation (bound returns the law): both fail. **Gates (Release)**: member_ports 49, port_graph_replay 2, additive_channel 12,
  spectral_rom1d 101, 2d_spectral_rom 65, storage gate A 0.889/0.958, B 0.727/0.960 (unchanged), R 0.952/0.880 → 0.952/0.889 (median width; not A/B-separated, the only change in the tree is the bound). **Release 1D smoke**
  (ph_o13): 44 s, k ≤ 33, admitted 18, closure +7, Jacobi 0, not converged 0, rejected 0, store below invert 100 (G80F11B 95, G72R33Bb 5,
  deepest −0.123 ft), any-node 1,956,222 (−2044.5 ft), correction-only 137,514 (R-1: 1 fallback, 96 / 136,902). **Coupled Bellinge
  (`demo_ph5`, Release, ON)**: wall 1000/2000/3000/4000 = 445 / 845 / 1,069 / 1,571 s, total **2,516 s** (O-12 2,290). `[PH-ports]`:
  solves 340,750, Newton 1,236,941, Jacobi fallbacks 309, **not converged 19 = rejected 19** (O-12 6), k_max 558, admitted 388 (closure
  +16), store member-steps below invert **10,151, deepest −399.8 ft** (O-12 3,153 / −105.9; new: G80F13P 806, G73F041 603, G71F043 341,
  G71F044 341, G73F301 341), any-node 686,330 (400,083), correction-only 88,312 (35,356). **Scores vs mc_v3** (O-12 in brackets): 1D all
  0.949/1.645 CONSERVATIVE (0.945/1.416); storage 0.807/0.914 **CALIBRATED** (0.795/0.795); trunk 0.828/1.267 (0.798/1.188); far from rim
  0.958/1.704 (0.956/1.447); Brier 0.0198 vs 0.1039 (0.0202/0.1112); 2D street 0.657/0.160 (0.620/0.158). Nodes (MC / OFF / ON, m):
  G72UD11 1.105 / 0.916 / **1.298** (0.225), G72R83B 0.062 / 2.995 / 0.167 (0.393), G72R150 0.454 / 3.445 / 1.220 (0.966), G70F11P 0.733 /
  0.038 / 0.181, G71O020 0.007 / 0.033 / 0.030, G70S213 0 / 0.100 / 1.666 (1.624), G70S214 0 / 0.126 / 1.555 (1.534), G62F012 0.019 /
  0.088 / 1.529 (1.398), G71F043 0.005 / 0.016 / 1.060 (1.152), G72R52X 0.273 / 0.088 / 0.037. **Reading**: the bound removes the uphill
  delivery (G70S213 07:00 q95 +93 → +9.6 ft) and moves G72UD11 from 5× narrow to 1.2× wide and storage to calibrated, but the four
  laterals' median band is unchanged — their persistent band is (B), the plane's surcharge overshoot turned into member pond water — and
  new far laterals widen (G73F430/420/410 3.4/3.2/2.9 m, G62F113–118 ~1.8 m vs MC ≤ 0.08), with 19 rejected solves and deeper store sinks.
  Net: not better than O-12 on the 1D aggregate; owner decision whether to keep it (law is right; what it exposes is (B) and the
  G71F043/G71F044 sinks). Open: (B) admission of a near-rim manhole whose members are at the rim only through the plane's surcharge
  burst; (C) the loss/gain-asymmetric carry ratchet at G72UD11.
- 2026-11-18: O-14 (mechanism B, the shared plane's overshoot at the 06:25 burst; `stepRom1dAdditiveChannel`,
  `stepRom1dStorageMembers`, tests `PlaneVolumeCarry.*`, uncommitted; **STOPPED before Bellinge**: the law moves SR-5/SR-7/CL-1e and
  storage gate R at the printed digit, see Gates). **Data first** (OFF CSVs, mc_v3 heads/link flows/system storage regressed on the
  members' rain and Manning multipliers, new env-gated plane trace `OPENSWMM_ADD_DUMP_NODES=a,b|*` [+ `OPENSWMM_PH_DUMP_DIR`,
  `_T0`, `_T1`] → `plane_trace.csv`: head, depth, rim, overflow, pinned, S, plane-0 source dq, summed runoff, Psi, incident ju/jd/d;
  1D smoke, OFF, whole network 06:19–06:50, `SWMM6/data/o14_diag`). The OFF overshoot at 06:31 is half rain plane, half Manning
  channel (smoke rain-only vs Manning-only: G62F012 +0.90 / +0.92 m; MC: rain +0.36, Manning +0.10 m); this entry is the plane.
  **Ranking.** (2') **the plane carried HEAD across a change of S** — cause. The step was (S_new/dt + J)Psi_new = (S_new/dt)Psi_old
  + dq; the member balance d[V(h+Psi)−V(h)]/dt = dq − dQ linearises to d(S Psi)/dt, so every step booked (S_new − S_old)·Psi_old of
  water no member has. At a crown S jumps MIN_SURFAREA ↔ ½W·L of the attached pipes, so each receding surcharge creates water.
  Network budget, 06:19–06:50 per unit coefficient: sources 31,000 ft³, created by rising S 194,000, destroyed by falling S 39,000;
  plane volume 120,000 → 253,000 (06:31) → 290,000 (06:51) against the MC's extra network storage per +20 % rain (system storage
  regressed on rain) 31,600 / 47,500 / 36,900 / 28,500 ft³ at 06:21 / :31 / :41 / :51. Local maxima of Psi are free-surface nodes
  with no source of their own (G73F070 131 ft, G72R290 80 ft per unit c at 06:31: S rose 1.2 → 100 ft² under Psi 20–48 ft). Shallow
  laterals: G62F012's Psi rises on the recession with its source gone (1.4 → 2.6 ft), following its downstream node. Plane / MC
  rain sensitivity, network median 1.10 / 1.50 / 2.36 at 06:21 / :31 / :41 (grows after the burst). (1) E: 1.2–1.47 on the burst onset
  (22,860–22,970 s), 0.92 at the 06:21 frame (MC ln-ln slope 0.92), 0.44–0.62 on the recession (MC 0.52–0.66); the clamp 3 is never
  approached: at most 1.4× for two minutes, excluded. (2) as posed (DW's Σdqdh·dt in S): that term is the implicit Picard treatment
  of the outflows, already the J of the backward-Euler step; adding it double-counts, excluded. (4) H17 full-pipe ju: friction
  from geometry equals the main run's head drop (G73F430→420: hf 4.13 ft vs dh 4.1; G71F043→041: 0.99 vs 0.985), excluded. (3) rim:
  in the G62F/G71F04 clusters the MC is 1.5–2.6 m below the rim (not capped); it is the mechanism of ONE cluster, G73R451–455 /
  G73F430–410 (lowest rim 0.07 m above the main run; the MC's flow through the 0.2 m bottleneck G73F430→420 changes −2 … +2 % with
  rain while the plane passes +7 … +21 % per c; MC flooding per unit c 5 cfs at 06:21, 31 cfs at 06:41). Secondary, not fixed here.
  **Law** (no knob): the planes carry volume — (S_new/dt + J)Psi_new = (S_old/dt)Psi_old + dq, i.e. Psi_old ×= S_prev/S_now before
  the step (`rom1d_add_S_prev_`); the per-member graph's LINEAR nodes the same (z_prev ×= S_prev/S_now, `rom1d_pg_S_prev_`; stores
  already carry dV). Docs: ADDITIVE_CHANNEL §2 "What is carried". **Tried, not adopted**: S as the secant of the node's volume over
  the member range ±σ (σ = 1.645 RMS, as the structure secants): no better on the network (smoke 1.095/3.20/13.2 vs 1.092/3.14/11.6)
  and worse at G73F430 (06:21 15.9 m vs 8.95): once σ ≫ the pipe, S_sec ∝ 1/σ feeds back.
  **Effect, 1D smoke vs mc_v3 05–07 h (1D-only vs coupled MC: indicative)**, cov / width med / p90 / p99 / nodes×frames > 3×:
  OFF 0.956/1.171/3.61/18.4/1131 → 0.945/1.092/3.14/11.6/931; ON (O-13) 0.956/1.157/3.59/17.1/1106 → 0.944/1.092/3.09/11.6/912.
  Nodes with median ratio > 3: 30 → 10 of 958. Plane volume now 47,800 / 60,300 / 41,700 / 25,000 ft³ (MC 31,600 / 47,500 /
  36,900 / 28,500). Rain plane q95 vs MC rain part at the six (m, head carry → volume carry, MC): G62F012 06:31 0.91 → 0.57 (0.36),
  06:41 0.37 → 0.07 (0.05); G62F113 06:41 6.50 → 0.51 (0.46); G71F043 06:21 2.16 → 4.79 (0.54), 06:31 0.85 → 0.19 (0.61), 06:41
  0.21 → 0.01 (0.29); G73F430 06:21 2.10 → 8.95 (0.71), 06:31 4.76 → 8.15 (0.88). So: the recession overshoot is gone; on the
  RISING limb a node whose S falls (entering surcharge) now spikes (the volume it held is real, the members it belongs to were
  already full) and G73F430's rim cluster is worse; G71F043 is narrow after 06:31. Network-wide plane/MC 06:31 / :41 median 0.74 / 0.63.
  **Tests** (member_ports 49 → 51): `PlaneVolumeCarry.BurstIntoASmallStorageJunctionCreatesNoWater` (J0→J1 small pipe→J2→J3→O1,
  10-min burst, J1's S 12.6 → 224 ft²; plane volume ΣS·Psi never above the cumulative source; max ratio 1.0000, mutation = head
  carry 2.389) and `PlaneVolumeCarry.PlaneTracksTheMembersHeadOnTheRecession` (two deterministic members at rain ×1.2/×0.8; on the
  drained recession plane / members' sensitivity 0.776, mutation 2.313; bounds 0.5–2.0). **Gates (Release)**: additive_channel 12,
  spectral_rom1d 101, member_ports 51, port_graph_replay 2; storage gate A 0.889/0.958, B 0.727/0.960 (unchanged), **R 0.952/0.889 →
  0.952/0.887**; **SR-5 0.921/1.029 → 0.921/1.028, SR-7 0.922/0.969 → 0.917/0.969, CL-1e 0.941/1.026 → 0.941/1.025** (spin-up: S
  of filling pipes varies; SR-7's change is in the early-window member coverage, its late width is identical). Per the task card
  this is a STOP: Bellinge OFF/ON not run, pending owner. **Release 1D smoke** (ph_o14, ON): 39 s, k ≤ 33, admitted 18, closure +7,
  Jacobi 0, not converged 0, rejected 0, Newton 258,039 (O-13 298,365), store below invert 118 (deepest −0.125 ft), any-node 798,159
  (O-13 1,956,222; deepest −359 ft vs −2044.5), correction-only 15,063 (137,514; deepest −3.2 ft vs −134). **Open**: (a) owner:
  accept the SR/R digit moves; (b) rising-limb spike at nodes entering surcharge with plane volume (needs the members' own storage
  across the crown — per-member, not a plane law); (c) the rim of the G73R45x cluster; (d) the Manning channel's own 06:31
  overshoot (G62F012 +0.92 m vs MC +0.10); (e) Bellinge scoring of OFF as `demo_off` needs `demo_off.inp` (demo_v4.inp writes
  `OUTPUT_FILE demo_v4.2d.h5`; the O-12 OFF run overwrote demo_v4.2d.h5 on 11-16) — copy demo_v4.inp with OUTPUT_FILE demo_off.2d.h5.
- 2026-11-18: O-15 (mechanism C, the loss/gain-asymmetric carry ratchet; `stepRom1dStorageMembers` sharing block, `RomStorePorts.hpp`
  `storeCarryScale`/`carryStoreVolume`, tests `StoreVolumeMismatchSharing.*`, uncommitted). **Accounting as found**: the main run's
  ΔV* = V(d*_new) − V(d*_old) differs from dt·net* (conduit storage, lagged areas, flap/surcharge weirs); m = ΔV* − dt·net*. Loss
  (m < 0): s = (V(d*_new) − dt·net*)/V(d*_old) = 1 + m/V(d*_old) < 1 scaled every |dV_i|; gain: s clamped to 1. Product over an
  alternating sequence decays (100 cycles of ±40 ft³ on 1000 ft³: 0.017). **Rule chosen**: s on BOTH sides (unclamped above, ≥ 0, 1
  below the dry threshold), then dV ≥ −V(d*_new) (member water ≥ 0; the only clamp). Over a cycle returning the main run's water the
  product is exactly 1. **The additive alternative (dV unchanged, the mismatch is main-run bookkeeping) was tried first, as the
  task's first reading, and REJECTED on the gate**: R 0.952/0.887 → 0.624/0.894 (B1 per-member; A, B unchanged), so members'
  deviations do scale with the main run's water at the raised-end basin; symmetric proportional share gives R 0.952/0.902.
  **Tests** (Debug): member_ports 52 (51 + the ratchet test; share test rewritten to `carried == max(s·dV_prev, −V_after)`, order kept,
  member water ≥ 0; ratchet test: new rule returns 5 deviations to 1e-10 after 100 cycles, old rule asserted as the mutation fails),
  port_graph_replay 2, additive_channel 12, spectral_rom1d 101. **Storage gate** A 0.889/0.958 → 0.891/0.961, B 0.727/0.960 →
  0.728/0.962, R 0.952/0.887 → 0.952/0.902 (floors untouched). **Debug 1D smoke** (ph_o15, 418 s): k 33, admitted 18, Jacobi/not
  converged/rejected 0/0/0, closure +7, Newton 274,485 (ph_o14 258,039); **store below-invert 118 → 638 member-steps (G72R33Bb 5 → 522,
  G80F11B 113 → 116)**; any-node 801,586 (798,159), correction-only 18,063 (15,063; deepest −4.26 ft vs −3.21). Band widths vs ph_o14
  (max / mean over time): G72UD11 4.28/0.47 → 6.38/0.79 ft, G72R83B 4.68/0.90 → 4.72/0.95, G72R150 1.32 → 1.30, G72R33Bb 0.29 → 0.31.
  **Open**: (a) the ratchet was masking part of G72UD11's over-width (MC 1.2 m one-sided): removing it widens that node, so mechanism
  C is closed but G72UD11 is not (its band needs the downstream-pipe nonlinearity, O-13 open item); (b) G72R33Bb below-invert rose
  (a gain scales a near-empty store's deficit up; s has no cap beyond the dry threshold) — owner call whether a floor on the solved
  level, not just the carried volume, is wanted; (c) Bellinge not run, per instruction.
