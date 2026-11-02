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
- 2026-11-01: H-1/H-2 landed (2ecc153a); S-1 landed (463c808a); O-1 note written, F-1 verdict in §2.4 — energy contract replaced by ℓ1 for full-order ports; S-2 running.
