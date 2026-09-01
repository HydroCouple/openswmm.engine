# R3 Implementation — Validation & Commit Handoff (2026-08-16)

**For:** the checking agent (sandbox: `g++ -fsyntax-only` only; nothing
linked/executed).
**Base:** `352638e6` (post-R2).
**Plan:** `MULTISPECIES_REACTIONS_MSX_PLAN.md` §5 R3 + carried D-R8/D-R9.
**Standing findings:** reconfigure before building (one new `.cpp`);
unique-diagnostic falsifiers per defense.

---

## 1. Changeset (uncommitted)

```
new:  src/engine/transport/components/ReactionModule/ReactionIntegrator.{hpp,cpp}
new:  tests/unit/engine/test_reaction_integrators.cpp     (6 gate groups)
mod:  src/engine/transport/components/ReactionModule/ReactionExpression.cpp
      (D-R8: unary minus precedence 4 → 2, now BELOW '^'; D-R9: empty-span
       guard returns 0.0)
mod:  tests/unit/engine/test_reaction_expressions.cpp
      (D-R8 goldens flipped: -2^2 = -4, (-2)^2 = +4, 2^-2 unchanged,
       -2*3 unchanged; the R2 validator's mathexpr.c documentation block
       retained with the resolution appended; new D-R9 empty-span gate)
mod:  tests/unit/engine/CMakeLists.txt   (test_engine_reaction_integrators)
```

## 2. What R3 adds

- **`ReactionIntegrator::step`** — one reaction step on a local species
  block: RATE species via EUL (single explicit step, MSX semantics) /
  RK5 (Cash–Karp embedded, atol/rtol-weighted RMS control) / ROS2
  (2-stage L-stable Rosenbrock, γ = 1 − 1/√2, FD Jacobian, embedded
  error) / BDF2 (Newton + dense LU, BE startup, halving on Newton
  failure); then EQUIL species by joint damped Newton (FD Jacobian,
  halving line search, hard iteration cap); then FORMULA species in
  declaration order. Terms re-evaluate inside every RHS. Rate-unit
  scaling (SEC/MIN/HR/DAY → 1/s) applied at the RHS. `COUPLING NONE`
  freezes other species at start-of-step values (MSX semantics);
  `FULL` integrates the RATE set jointly.
- **Allocation-free step:** all scratch in the caller-owned `RxWorkspace`
  (D-R3); dense LU with partial pivoting for the small per-element
  systems (D-R7 — no SUNDIALS anywhere).
- **Hard-failure discipline:** substep-cap overrun, step-size collapse
  (with the D-R7 escape-hatch pointer in the message), Newton divergence,
  and non-finite states are loud `RxStepReport` failures, never silent.
- **D-R8 implemented:** `-2^2 = -4` (sympy/Python convention; rationale
  comment at the precedence site and in the goldens). **D-R9
  implemented:** empty-span evaluation returns 0.0 by contract.
- NOT here (by plan): engine bindings (R4 qualroute, R6/E4 ARD);
  `[REACTION_SOURCES]`/per-element parameters; the `.msx` translator
  (this phase's analytic gates stand in until R4 networks run chemistry
  against EPANET-MSX itself).
- Implementation notes for review: one in-flight fix during authoring —
  `errNorm` originally iterated the full RATE set against group-local
  arrays (out-of-bounds under `COUPLING NONE` with >1 species); now takes
  the explicit group view. Worth an ASan pass over the gates.

## 3. Validation protocol

1. **Reconfigure**, build, zero new warnings from touched files.
2. `ctest -R test_engine_reaction_expressions` — the D-R8 flip is the
   riskiest edit: all pre-existing goldens must stay green (`2^3^2 = 512`,
   `2^-2 = 0.25`, `- -5`, kinetics) with ONLY the documented `-2^2`
   flip. Falsifier probe: revert the precedence change (2 → 4) — the
   `-2^2 = -4` golden must fail with +4.
3. `ctest -R test_engine_reaction_integrators` — six gate groups.
   *Anticipated failure modes, in likelihood order:*
   (a) **Gate 3 ROS2 substep bound (`< 200`)**: the embedded error
   controller may legitimately resolve the fast transient under the tight
   deck tolerances, inflating substeps. If ROS2 fails ONLY the economy
   bound while passing slow-mode accuracy and L-stable damping: record the
   measured count, raise the bound to what you measured ×2, and note it —
   the physical claims (stability + accuracy at dt ≫ 1/λ_fast) are the
   gate's core; the economy bound is informative. Do NOT loosen the
   accuracy assertions.
   (b) BDF2 first-order-decay tolerance (5 % band at k·dt = 2.5 via BE
   startup + BDF2): if outside, record measured error and judge — the
   band was set analytically, not measured.
   (c) EUL divergence assertion in gate 3: EUL takes ONE explicit step at
   h·k = 1e4 by MSX semantics, so |F| ≈ 1e4 — if this somehow PASSES
   accuracy, the integrator is not doing what it claims; investigate, do
   not adjust.
   (d) FD Jacobian + strict tolerances under Release fast-math (if any
   platform flag enables it): watch for flaky EQUIL convergence.
4. **ASan/UBSan run** of the two reaction test binaries (the §2 errNorm
   note + the D-R9 guard make this round worth the sanitizer cost):
   no findings expected.
5. Prior suites (`reactions_config`, `reaction_expressions` rest,
   `process_components`, ARD gates) green; full suite; standard
   no-config bit-identity spot-check (integrator only runs when invoked —
   nothing calls it in production yet; the binding is R4/R6).
6. Append results to §5; commit with §4.

## 4. Commit message

```
feat(reactions): integrators EUL/RK5/ROS2/BDF2 + EQUIL Newton + FORMULA (R3)

ReactionIntegrator::step on a local species block: RATE kinetics via
explicit Euler (MSX EUL), Cash-Karp RK5 with atol/rtol control, L-stable
ROS2 (FD Jacobian, embedded error), and Newton-solved BDF2 with BE startup
(the D-R7 stiff workhorse — no SUNDIALS); joint damped-Newton EQUIL;
declaration-order FORMULA; terms re-evaluated inside every RHS; rate-unit
scaling; COUPLING NONE/FULL per MSX semantics; allocation-free via
caller-owned workspace; loud hard-failure discipline (substep cap, step
collapse w/ D-R7 escape-hatch pointer, Newton divergence, non-finite).
Implements carried D-R8 (-2^2 = -4, sympy-consistent; goldens updated with
history retained) and D-R9 (empty-span eval returns 0 by contract).
Gates: analytic batch-reactor references incl. the stiffness ladder
(lambda ratio 1e6: implicit solvers hold at dt >> 1/lambda_fast, explicit
EUL proven to diverge), coupled-chain analytic, EQUIL/FORMULA algebra,
exact rate-unit scaling, COUPLING NONE freeze semantics.

Plan: MULTISPECIES_REACTIONS_MSX_PLAN.md §5 R3 (+D-R8/D-R9).
Validation record: plans/transport/R3_VALIDATION_HANDOFF_2026-08-16.md
```

## 5. Validation results

**R3 as delivered failed all six integrator gates on four real defects.**
Fixed on instruction and committed as **`a3fbc78b`**; base `352638e6` matched
HEAD, tree carried exactly the six files of §1. Artifacts:
`tests/output/r3_validation_2026-08-16/`.

### 5.1 Build (protocol 1) — three new warnings, now removed

`rc=0`, but **not** zero new warnings: `ReactionIntegrator.cpp` left three
dead lambdas (`gather`, `scatter`, `f` at lines 215/219/224 — superseded by
the group-view `fg`). Removed as orphans of the changeset's own refactor.
Everything else is the pre-existing set.

### 5.2 The failures were one symptom, four causes (protocol 3)

0/6 gates. Every accuracy failure pointed the same way — **too little decay**
(RK5 0.42016 vs 0.41042; gate 5 1.8869 vs 1.8835; gate 2 0.38430 vs
0.36287) — which is a bookkeeping signature, not a tolerance one. The
standalone probe (`diag_probe.cpp`, before/after logs) isolated it:

| solver | k·dt | before | exact | substeps | after |
|---|---|---|---|---|---|
| RK5 | 2.5 | 0.42016 (**+2.4 %**) | 0.41042 | 22 | 0.41042 (−7e-9) |
| RK5 | 0.06 | 4.70882267 (−6e-12) | 4.70882267 | **1** | unchanged |
| BDF2 | 2.5 | 1.42857 (**+248 %**) | 0.41042 | 1 | 0.41042 (−7e-9) |
| ROS2 | 2.5 | step collapse | — | 0 | 0.41042 (−4e-9) |
| ROS2 | 0.01 | 5.0088 (+1.2 %) | 4.9502 | **99 991** | 4.95025 (−1e-11) |

RK5 being *exact* at one substep and wrong at 22 localizes it precisely.

1. **`t += h` advanced by the PROPOSED step, not the taken one.** The accept
   branches wrote `h = min(dt, 0.9·h·en^…)` before the shared
   `if (accepted) { t += h; }`. Fixed with an explicit `h_next` adopted only
   after `t += h`. Rejection paths still mutate `h` directly — correct, since
   `t` does not advance there.
2. **ROS2's combination weights are inconsistent.** `(1−1/(2γ), 1/(2γ)) =
   (−0.707, +1.707)`: as h→0, k1→f and k2→−f, so the update tends to
   `y − 2.414·h·f`. No step size can satisfy that, hence the collapse.
   Corrected to the standard Verwer/Hundsdorfer **3/2, 1/2**. The embedded
   estimate was wrong for the same reason — `(h/2)(k2−k1) → −h·f` never
   vanishes, which is what burned 99,991 substeps at k=0.001. Now
   `(h/2)(k1+k2)`, the difference against the first-order companion
   `y + h·k1`, which does vanish with h.
3. **BDF2 had no error control at all** — one backward-Euler step over the
   whole dt. Added the embedded first-order companion (the explicit
   predictor; `f(y)` is already computed for the Jacobian, so it is free)
   plus **variable-step** BDF2 coefficients `a0=(1+r)²/(1+2r)`,
   `a1=r²/(1+2r)`, `β=(1+r)/(1+2r)`, `r=h/h_prev` — the fixed-step 4/3, 1/3,
   2/3 form was invalid once the controller changes h between substeps.
4. **`COUPLING NONE` left all but the last species unintegrated.**
   `freeze_others()` restores every other species from the start-of-step
   snapshot, and it runs inside each later group's RHS — *after* the earlier
   group wrote its result into `species`. Gate 6 caught it exactly
   (`species[0]` returned as `a0`, untouched). Group results are now staged
   in `ws.grp_out_` and published once, after the final freeze.

### 5.3 Two gates were also wrong (§3c was the right instinct, wrong gate)

- **Gate 1's EUL row cannot pass.** One explicit step at k·dt = 2.5 gives
  `c0·(1−2.5) = −7.5`; no relative band around a positive exact value
  expresses that, and a 0.35 band only hides the behavior. EUL now leaves the
  closed-form table and its exact one-step value is pinned instead — that is
  the MSX contract, and gate 3 already asserts the loud failure.
- **`ASSERT_TRUE(rep.ok)` inside the solver loop aborts the whole test.**
  ROS2's collapse aborted gate 1 before BDF2 ever ran, so **BDF2's 248 % error
  never appeared in the ctest output** — it looked like a passing solver.
  Changed to `EXPECT` + skip. Worth generalizing: an ASSERT inside a
  table-driven loop silently truncates the table.
- **Stiffness-ladder economy bound**, exactly the §3a case: accuracy and
  L-stable damping pass; only `substeps < 200` failed (ROS2 2707, BDF2 3979).
  Raised to 8000 (measured ×2) per §3a, with the reason recorded in the test:
  the "one big step" premise does not survive an *accuracy* controller —
  L-stability buys stability, not permission to skip an unresolved transient,
  and atol 1e-10 forces the fast mode to be resolved before h can grow.

### 5.4 Post-fix results

- **6/6** integrator gates, **8/8** expression gates.
- §3.2 falsifier probe (revert D-R8 precedence 2 → 4): `-2^2` golden fails
  **+4 vs −4** as required. Restored.
- The four fixes need no separate falsifiers — the pre-fix run in §5.2 *is*
  the falsification evidence, with each defect mapped to the gate it broke.
- **Full suite 132/133**; the one failure is the known pre-existing
  `FvEngine.RefiningTheMeshConvergesTowardTheDynwaveHydrograph`.
- **ASan + UBSan (protocol 4): 0 findings**, 14/14 gates, over both reaction
  binaries — covers the §2 `errNorm` group-view note and the new `grp_out_`
  staging. Build dir `build/darwin-asan/` (Ninja, RelWithDebInfo) left in
  place; delete when done.
- Deck bit-identity spot-check: **14/14 `.out` sha256 identical** to the R2
  baseline. Expected — nothing calls the integrator yet — but D-R8 changed
  the compiler, which *does* run at open for reaction decks.

### 5.5 Open, NOT fixed

**The implicit solvers are expensive at these tolerances.** A trivial decay
(k·dt = 2.5, atol 1e-10 / rtol 1e-8) costs RK5 22 substeps but ROS2 13,267
and BDF2 19,556. That is inherent to low-order embedded pairs — a 1st/2nd
pair forces `h ~ √(tol)` while Cash-Karp's 4th/5th pair allows `h ~ tol^(1/5)`
— and BDF2's controller is mine, so its count is a consequence of the
conservative pair I chose. Not a correctness issue and not blocking (nothing
calls the integrator until R4/R6), but **it will matter the moment this runs
per-element per-step**: R4 should either loosen the default tolerances,
give ROS2/BDF2 a higher-order companion, or cache the Jacobian across
substeps. Worth a decision before the binding lands, not after.

Minor: `kMaxSubsteps = 100000` is per group, so a COUPLING NONE system with
many species can multiply that.

### 5.6 Follow-up: FD Jacobian caching (`7c2c151b`)

Taken up on request, from §5.5's list. The Jacobian is now held in the
workspace and refreshed only on the first substep of a group, after a
rejected step or Newton failure, or after 20 accepted substeps. The factored
`I − c·h·J` is cached with it, keyed on `c·h` — and BDF2's Newton loop no
longer rebuilds and re-factors that matrix every iteration, which it had been
doing even though `J` is held at `y` and `β·h` is fixed across the loop.

Measured on a nonlinear coupled system, 200 steps, cache off vs on
(`bench_cache_off.log` / `bench_cache_on.log`):

| solver | n=1 | n=3 | n=6 |
|---|---|---|---|
| ROS2 | 19.1 → 15.2 ms (**1.26×**) | 83.3 → 47.5 (**1.75×**) | 394.8 → 182.0 (**2.17×**) |
| BDF2 | 26.9 → 22.0 ms (**1.22×**) | 138.3 → 87.3 (**1.58×**) | 610.1 → 289.5 (**2.11×**) |

Substep counts are unchanged across the pair (631 vs 632, 1541 vs 1541,
2273 vs 2272) and checksums agree to ~1e-11, so the speedup is not bought by
taking sloppier steps. It scales with species count because the Jacobian is
`n` of the `n+2` RHS evaluations — the ×2 at n=6 should keep growing.

**A gate had to be added before this could be trusted.** Every pre-existing
analytic gate is LINEAR (first-order decay, coupled chain, EQUIL algebra,
rate units), and for a linear system a cached Jacobian is *exactly* equal to
a freshly computed one — the whole suite was structurally blind to a stale-
Jacobian defect. Evidence: after caching, ROS2's linear results were
bit-identical to the pre-cache run. New gate
`NonlinearDecayMatchesClosedFormWithCachedJacobian` runs `A' = −k A²` against
`A₀/(1 + k A₀ t)`, where `J = −2kA` genuinely varies along the trajectory.

Re-validated: **7/7** integrator gates, 8/8 expression gates, full suite
**132/133**, **ASan/UBSan 0 findings** over the cached path.

### 5.7 Follow-up: default solver is now RK5 (`eca08593`)

§5.5 said the implicit solvers were "expensive at these tolerances." **That
was measured at the GATE tolerances (atol 1e-10 / rtol 1e-8), which are four
orders tighter than what ships** (`ReactionData` defaults: 1e-6 / 1e-4). The
sweep (`tol_sweep.log`) corrects it — and then reframes the whole question:

| atol/rtol | RK5 | ROS2 | BDF2 |
|---|---|---|---|
| 1e-10 / 1e-8 (gates) | 22 | 13 267 | 19 556 |
| 1e-8 / 1e-6 | 10 | 1 328 | 1 957 |
| **1e-6 / 1e-4 (shipping)** | **5** | **134** | **197** |

Clean √tol scaling, confirming the 1st/2nd-pair diagnosis — but 134/197 at
the real defaults, not 13k. The alarm in §5.5 was overstated by two orders.

The decisive measurement was the stiff ladder at both tolerances
(`stiff_rk5.log`):

| case | RK5 | ROS2 | BDF2 |
|---|---|---|---|
| stiff, default tol | 2682 | **281** | 408 |
| stiff, atol 1e-10 | **2744** | 26 938 | 39 693 |

RK5's substep count is essentially **flat in tolerance** (2682 → 2744 over
four orders) because it is stability-limited, while the implicit pairs are
accuracy-limited. So ROS2-as-default lost to RK5 on ordinary kinetics by 27×
*and* on stiff kinetics at tight tolerance by 10×; it won only in the
stiff-and-loose corner. Default changed to RK5.

**The lever that mattered was not the tolerance knob at all.** Loosening
tolerances buys a constant; a higher-order companion buys ~5× (exponent
√tol → tol^(1/3)); choosing the right solver buys 27×. Kdecay pollutants —
which E4 routes as RATE expressions, and which every legacy deck has — are
first-order linear decay, the least stiff case there is.

**The cliff, and why it is acceptable:** RK5 costs ≈ λ_fast·dt/3.3 substeps
(matches the measurements), so past λ_fast·dt ≈ 3e5 — λ_fast ≳ 1000 /s on a
5-minute step — it hits `kMaxSubsteps`. It fails loudly there, and the
substep-cap message now names the remedy (set SOLVER to ROS2/BDF2, or
declare the fast species EQUIL) rather than saying only "exceeded the
substep cap". Gates `DefaultSolverIsExplicitRK5` and
`ExplicitDefaultOnStiffKineticsFailsWithActionableMessage` pin both halves.

**Not measured against real MSX reference decks.** The ladder is synthetic
(λ_fast = 100 /s). Genuinely fast aqueous kinetics are normally declared
EQUIL, which keeps them out of the integrator — but that is reasoning, not
measurement. E4 profiling against actual MSX cases is what confirms or kills
this default.

9/9 integrator gates, full suite **132/133**, ASan/UBSan 0 findings.
