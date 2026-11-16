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
