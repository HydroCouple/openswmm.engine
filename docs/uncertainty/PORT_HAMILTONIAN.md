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
