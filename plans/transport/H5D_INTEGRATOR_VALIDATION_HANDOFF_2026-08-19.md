# D-H5d Integrator — Validation & Commit Handoff (2026-08-19)

**For:** the checking agent.
**Base:** `65cae8a8` (H5a). **Note the hash** — the H5a round reported
`53b95219`, but that commit was rebuilt with `commit-tree` after the staged
index was found reverting foreign commit `6dde88b0`. `53b95219` still exists
as an unreferenced object. **Branch it against `65cae8a8`.**
**Plan:** `HEAT_TRANSPORT_PLAN.md` **§6.2 (new — D-H5d, read it first)**.
**Standing findings:** lessons 1–79.

**This is a standalone defect fix, landing BEFORE H5b** — the user's
sequencing choice, so that a falsifier sweep can tell a correctness fix from
a design change.

---

## 1. What this fixes

H5a's validation found the surface energy balance diverging to NaN. Heat
capacity is `ρ·cp·V`, so a thin film has almost none: a **0.52 ft³ film over
27,226 ft²** takes a **+862 °C step in 60 s**, the flux re-evaluates at
182 °C, and the sequence runs `5 → 182 → −1.8e4 → −3.9e9 → inf → NaN`, out
through `subcatch_runoff_temp` into the node temperatures and the report.

**H2 and H3 carry the identical unbounded step** at their node and link
bindings, unexposed only because those volumes are large. All three are fixed
here. H5b's LID layer volumes are smaller than anything H5a touches, which is
why this lands first.

The flux **formulations** of H2 and H3 were never wrong. Only the stepping
was: CSH runs these same formulas at a **1e-4 s** timestep with a selectable
ODE solver (`cshmodelio.cpp:3260`, base step `cshmodel.cpp:33`). We took its
physics at a 60 s hydraulic step and not its integrator.

## 2. The scheme

`ρ cp V dT/dt = −A·J(T)`, linearized about the current temperature and
integrated exactly:

```
J′   = (J(T₀+h) − J(T₀)) / h      h = kProbeC = 1e-3 °C
k    = A·J′ / (ρ cp V)
ΔT   = (J₀/J′)·expm1(−k·dt)       T_eq = T₀ − J₀/J′
```

Three properties, each gated: **|ΔT| ≤ |T_eq − T₀|** so the step can never
overshoot equilibrium at any `dt`; **no iteration**, so no cap to tune;
**degrades to forward Euler as `dt → 0`**, so resolved answers are unchanged.

`J′ ≤ 0` is anti-damping — no fixed point to relax onto — and falls back to
the explicit step.

## 3. Changeset (uncommitted)

```
mod:  .../HeatFluxModules/SurfaceExchange.hpp   (relaxT + equilibriumT +
      kProbeC REPLACE the exported deltaT)
mod:  .../HeatFluxModules/SurfaceExchange.cpp   (relaxT, equilibriumT,
      netFluxOut helper; node + link bindings)
mod:  .../HeatFluxModules/RadiativeExchange.cpp (node + link bindings — these
      had their own HAND-INLINED explicit conversion, which is why they
      carried the same defect through a different spelling)
mod:  .../HeatModule/HeatWatershed.cpp          (subarea binding; kMaxStepC
      deleted with the divergence it guarded)
new:  tests/unit/engine/test_heat_integrator.cpp  (6 gates)
mod:  tests/unit/engine/CMakeLists.txt            (+1 target — SHARED FILE)
```

All touched TUs pass `g++ -std=c++20 -fsyntax-only`. Nothing built or run.

## 4. Design decisions to review

### 4.1 `deltaT` was REMOVED from the header, not kept alongside

The unstable form is now unreachable from outside `SurfaceExchange.cpp`,
where it survives only as `relaxT`'s fallback. Keeping both exported would
leave the hazard one call away, and H5b adds a fourth binding — lesson 66,
make it unrepresentable rather than documented.

**This is the change most likely to break something I cannot see.** If any
caller outside these files used `deltaT`, it will fail to compile, which is
the intended outcome; report it rather than re-exporting.

### 4.2 `kMaxStepC` deleted

H5a's refuse-above-5-°C bound. A relaxation step cannot overshoot, so nothing
can reach the threshold — keeping it would be a constant no gate could
observe being wrong (lesson 39). Deleting it also removes H5a's side effect
that an unresolved film carried its inflow temperature instead of exchanging.

### 4.3 One `netFluxOut` per module, not two inlined copies

`relaxT` needs the flux at `T` and at `T + h`. Each binding routes both
through a single expression (a free function in `SurfaceExchange.cpp`, a
lambda in `HeatWatershed.cpp`) so the probe cannot come to measure a
different function than the one being stepped.

### 4.4 Gate 2's numbers were computed, not copied from the report

The linear slope is **14 W/m²/°C**, chosen because it reproduces the measured
**+862 °C** explicit step on the reported film geometry. I verified that
numerically before writing the gate; my first draft used slope 8, which gives
492 °C and would have **failed its own `> 500` setup assertion**. Recording
this because the same class of unchecked expected-value shipped in H3's
gate 3 and A4's brief §1.

## 5. Validation protocol

1. **Isolated worktree at `65cae8a8`.** Lesson 71. Expect the bistable FV
   gate to fail (`0.0552…` vs `0.0525…`) — pre-existing.

2. **⛔ HARD STOP — this is lesson 79 and it is not a suggestion.**
   Before committing, `git diff --cached --numstat` **must** read exactly
   `1  0` for `tests/unit/engine/CMakeLists.txt`. **Any deletion count
   above zero means STOP and rebuild the index — do not commit past it.**
   The H5a round saw `1  1`, noted it, committed anyway, and reverted a
   foreign commit; recovery needed `commit-tree` on the true parent. The
   check fired correctly and was treated as information rather than as a
   stop.

3. **Grep:** `grep -rn "deltaT" src/` — every remaining hit must be prose.
   A live call outside `SurfaceExchange.cpp` means §4.1 missed a caller.

4. Build, zero new warnings. `ctest -R test_engine_heat` — **the H2, H3, H4
   and H5a suites must all still pass**, which is most of the point: if a
   resolved-regime answer moved, `expm1`'s small-step limit is not doing what
   §2 claims.

   **Anticipated failure modes, likelihood order.** I got all four wrong last
   round, so weight these accordingly:

   (a) **An H2 or H3 gate moves by a small amount.** The most likely real
   outcome. Those suites were written against forward-Euler answers on
   large-volume elements, where the two schemes agree to ~1e-7 relative — but
   "agree to 1e-7" is not "bit-identical", and a gate with a tight absolute
   band may sit just inside it. **If one moves, report the old and new values
   before changing anything**; the question is whether the delta is
   second-order-in-`dt` (expected) or first-order (a sign or factor error).

   (b) **A deck `.out` moves.** Same cause, reaching the report. The 14/14
   corpus has no heat deck, so this would surface in the H1/H4 suites
   instead.

   (c) **Gate 1's 1e-6 relative band.** Measured at 1.9e-7 — a 5× margin,
   which is real but not generous. If it fails, check `dt` and the volume
   before widening: the claim is a convergence statement and the honest fix
   is a smaller `dt`, not a wider band (lesson 55).

   (d) **Gate 5's exact-equality checks** (`EXPECT_EQ(…, 0.0)`) on degenerate
   inputs. These assert an early return, not a computation, so they should be
   exact; a near-zero result means a guard is computing before checking.

5. **Falsifier sweep:**

   | falsifier | expected failing gates |
   |---|---|
   | i. restore the explicit step in `relaxT` (return `explicit_dT` always) | **2, 3, 4** — the defect gates. If 2 does not fail, this whole changeset is unobserved |
   | ii. drop the `J′ > 0` guard | 5 |
   | iii. use `exp(−k·dt) − 1` instead of `expm1` | **1** — if it does not fail, gate 1's band is too loose to observe the small-step limit it exists for |
   | iv. flip the sign of `ΔT` | 2, 3, 4, 6 |
   | v. probe at `T − h` instead of `T + h` (slope sign inverted) | 2, 3, 5 |
   | vi. revert only the RADIATIVE bindings, leaving SurfaceExchange fixed | **probably nothing** — flagged in advance: no gate here drives a radiative-only element to divergence. **Owed**, and it is the same "fixed once, missed once" shape §4.3 exists to prevent |
   | vii. revert only the H5a subarea binding | **probably nothing** in this file; H5a's own gate 8 may catch it — **report which** |
   | viii. use the post-mix volume `v_old + v_in` in the watershed binding | nothing here (that is H5a's owed falsifier vi) |

6. **Prior suites:** full C++ suite, 14/14 deck bit-identity, ASan/UBSan.
   Heat-off decks take no new path.

7. **Record:** falsifier i (does gate 2 actually fail on the old code?),
   falsifier iii (is gate 1 observing anything?), falsifiers vi and vii
   (which bindings are genuinely guarded), and **any H2/H3 gate that moved,
   with both values**.

## 6. Known gaps

- **Falsifiers vi and vii are predicted to escape.** Three of the four
  bindings are guarded only through the shared `relaxT`; a per-binding
  revert has no dedicated witness. Closing it needs a deck driving a
  radiative-only small-volume element, which is owed rather than faked.
- The linearization is **exact for a linear flux and approximate for the
  real one** (`Je` carries `exp(17.27T/(237.3+T))`). Over a step where `T`
  moves far, `J′` at `T₀` is not `J′` along the path — the scheme stays
  stable and bounded but the equilibrium it relaxes toward is the linearized
  one. **A gate comparing a long step against many short ones is owed**;
  that is the same transient-reference shape A4's falsifier iii still needs.
- `[HEAT_FLUXES] SEDIMENT_EXCHANGE` (H6) will add a fifth flux family; it
  must join `netFluxOut`, not get its own inlined conversion. That is
  precisely how RadiativeExchange came to carry this defect separately.

## 7. Prepared commit message

```
fix(transport): integrate the surface energy balance semi-implicitly (D-H5d)

The balance was stepped with forward Euler and no stability limit. Heat
capacity is rho*cp*V, so a thin film has almost none: a 0.52 ft3 film over
27226 ft2 took a +862 C step in 60 s and diverged to NaN through
subcatch_runoff_temp into the node temperatures and the report.

Linearize the net outward flux about the current temperature and integrate
the linear ODE exactly, dT = (J0/J')*expm1(-k*dt). The step cannot overshoot
equilibrium at any dt, needs no iteration, and reduces to forward Euler as
dt -> 0, so resolved answers are unchanged.

Applied at all four bindings. RadiativeExchange had its own hand-inlined
explicit conversion and carried the same defect through a different
spelling; H2's node and link paths carried it unexposed because those
volumes are large, and H5b's LID layers are smaller again.

deltaT is removed from the header rather than kept beside relaxT, and
H5a's kMaxStepC refuse-bound is deleted with the divergence it guarded.

The flux formulations of H2 and H3 were never wrong. CSH runs these same
formulas at a 1e-4 s timestep with a selectable ODE solver; we took its
physics at a 60 s hydraulic step and not its integrator.
```

---

# 8. Validation result (checking agent, 2026-08-19) — COMMITTED `5cc83f94`

Isolated worktree at `65cae8a8`. **152/153** ctest — the bistable FV gate,
verified failing to the same digits at base with the changeset absent.
**14/14** decks bit-identical. **45 tests** clean under ASan/UBSan across all
six heat suites. Zero warnings from any changed file. Every H2, H3, H4 and
H5a gate passes. §5's anticipated failure modes (a)–(d) did not occur.

## 8.1 The scheme is sound, and the `J′ ≤ 0` fallback is unreachable

Deleting `kMaxStepC` puts the whole weight on `relaxT` never falling back to
the explicit step, so I swept the sign of `J′` over the **real** flux
functions rather than reasoning about it: Tw ∈ [−40, 60], Tair ∈ [−40, 50],
RH ∈ [1, 100], wind ∈ [0, 12] m/s — **3,135,820 samples, `J′ > 0` in every
one**, for the surface flux, the radiative flux and their sum
(`jprime_sweep.cpp`). The fallback is dead code in practice, which is what
makes §4.2 safe.

## 8.2 The answers DID move, and they converge — §5(a) in numbers

Passing gates are not unchanged answers; none of the H5a gates assert a
magnitude. Measured base vs new on the same decks:

| deck | base | new | Δ |
|---|---|---|---|
| `_hx_on` (H2 storage pool) | 14.8752873720 | 14.8798003047 | +4.51e-3 |
| `_hx_rh` (H2) | 15.9706306577 | 15.9742487091 | +3.62e-3 |
| `_hx_cp1` / `_hx_cp2` | 19.9803822023 | 19.9803828178 | +6.2e-7 / +1.5e-7 |
| `_hr_on` / `_hr_night` (H3) | 19.9894956153 | 19.9894956659 | +5.1e-8 |
| `_hr_day` (H3) | 20.0303441071 | 20.0303439608 | −1.5e-7 |
| `_h5b` (H5a subarea) | 9.6239217602 | 9.5723504270 | −5.16e-2 |
| `_h5j` (thin film) | 13.9824832215 | 13.8296052692 | −1.53e-1 |
| `_h5k` (sub-zero) | −5.4709685448 | −5.3538240439 | +1.17e-1 |
| `_h5c` (fluxes OFF) | 22.6074104055 | 22.6074104055 | 0 |

The H2 surface decks move ~30,000× more than the radiative ones, so §5(a)'s
question — second-order or a sign/factor error — is the right one. **It is
neither: the gap is first order in `dt`, which is what it must be.** Halving
the routing step halves it, cleanly:

| ROUTING_STEP | base | new | Δ |
|---|---|---|---|
| 60 | 14.4939905808 | 14.5243932537 | 3.0403e-2 |
| 30 | 14.6862055226 | 14.7005656099 | 1.4360e-2 |
| 15 | 14.8307837020 | 14.8376547301 | 6.8710e-3 |
| 5 | 14.9249918825 | 14.9272116046 | 2.2197e-3 |

The *local* difference is O((k·dt)²), but accumulated over `t/dt` steps the
*global* difference is O(dt) — the ordinary gap between a first-order scheme
and the exact solution of the same ODE. Both converge to the same limit and
the new scheme is nearer it at every step size. §5(a)'s framing should read
"second-order locally, first-order globally"; a *zeroth*-order gap (one that
did not shrink with `dt`) would have been the sign or factor error.

## 8.3 The no-overshoot property holds PER MODULE, not for the composition

**The finding of this round.** `applySurfaceExchange` and
`applyRadiativeExchange` are separate entry points, each now calling `relaxT`
with **its own module's flux only**. Under forward Euler that composed
exactly — the two increments are linear and add. Under relaxation it does
not: each sub-step relaxes fully toward *its own* equilibrium, so with both
modules enabled the pair can overshoot the true combined equilibrium and the
answer depends on the order the modules run in.

Measured (`split_probe.cpp`), two equal-strength modules with equilibria at
30 °C and 10 °C, true combined equilibrium 20 °C, starting at 5 °C:

| k·dt | split (node/link) | combined (subarea) | error |
|---|---|---|---|
| 4.1e-5 | 5.000615 | 5.000615 | −0.000000 |
| 4.1e-3 | 5.061317 | 5.061359 | −0.000042 |
| 0.41 | 9.700850 | 10.044256 | −0.343406 |
| 39.4 | 10.000000 | 20.000000 | **−10.000000** |
| 410 | 10.000000 | 20.000000 | **−10.000000** |

At large `k·dt` the split lands on the **last module's** equilibrium. The
watershed binding is not affected — it sums both families into one
`net_out` lambda before relaxing, which is correct.

§6 already names this hazard for a *future* `SEDIMENT_EXCHANGE`
("it must join `netFluxOut`, not get its own inlined conversion") without
noticing it **already exists between SurfaceExchange and RadiativeExchange**.
Not a regression — the same case diverged outright before — and it only bites
at `k·dt ≳ 0.4`, which for a node or link needs a sub-millimetre depth the
router's own floor mostly prevents. But §2's headline property should read
*per module*, and merging the two node/link bindings into a single relaxation
is a design change I have left for the user rather than taken (§5.0), exactly
as the integrator choice was left in the H5a round.

## 8.4 Falsifier sweep: 7 of 9 observed

| # | predicted | outcome |
|---|---|---|
| i | 2, 3, 4 | gates 2, 3, 4 **and** H5a's thin-film gate |
| ii | 5 | **escaped — but the behaviour is guarded.** `sign(k) ≡ sign(J′)` given the earlier positivity checks, so `if (!(k > 0.0))` shadows the named `J′ > 0` line and removing only the first is a no-op. Added **ii-b** (remove both): gate 5 fails. The line is redundant, not the guard |
| iii | 1 | **escaped as delivered.** At `k·dt ≈ 4e-7`, `exp(x)−1` is already accurate to ~1e-10 — four orders inside gate 1's 1e-6 band, so the gate could not observe the `expm1` it exists for. Gate 1 gained a small-`dt` sweep down to `dt = 1e-14`, where the subtraction cancels to a hard zero. Now fails |
| iv | 2, 3, 4, 6 | 4 integrator + 2 H5a + 3 H2 + 1 H3 + 1 H4 gates |
| v | 2, 3, 5 | 4 integrator gates **and** H5a's thin-film gate |
| vi | *predicted to escape* | escapes — and the reason is now measured, not assumed. See §8.5 |
| vii | "report which" | **H5a's `AThinFilmDoesNotDivergeUnderTheExplicitStep`** — the subarea binding is guarded, by the H5a round's gate rather than by anything in this file |
| viii | nothing here | escapes; still H5a's owed transient falsifier |

## 8.5 Falsifier vi is not merely unobserved — it may be unconstructible

§6 says closing it "needs a deck driving a radiative-only small-volume
element". I tried two routes and both are blocked by engine-level floors:

- **Thin node.** Divergence needs `k·dt ≳ 2`, i.e. a water depth around
  1e-5 m. The router treats depths below its 1e-4 ft floor as dry, so a node
  never gets thin enough.
- **Long step.** A storage node at 0.002 ft over 400,000 ft² with
  `ROUTING_STEP 3600` still landed at its equilibrium under **both** schemes
  (base −27.4445474192, new −27.4413394936). DYNWAVE's variable step does not
  honour a routing step that large.

So the node and link bindings are structurally hard to drive into the regime
this changeset fixes — which is the same fact that kept the defect hidden in
H2 and H3. The honest statement is not "we owe a deck" but "the 1D bindings
cannot reach the failure mode from a deck; only the subarea and, from H5b,
the LID bindings can." Recording the attempts so the next round does not
repeat them.

## 8.6 Also done

- `deltaT` is gone from the header (§4.1 verified: `grep -rn deltaT src/`
  returns prose only, no live caller). Its **removal orphaned four
  references by name** — one in `SurfaceExchange.hpp` describing an export
  that no longer exists, and three describing the area-cancellation property.
  All four updated to `relaxT`; the property itself was re-checked and still
  holds exactly, since area enters only through `k = A·J′/(ρ cp V)` and
  `A/V ≡ 1/depth_prev`.
- §2's `|ΔT| ≤ |T_eq − T₀|` and §4.4's slope-14 calibration both verified;
  gate 2's setup assertion reproduces +862.17 °C as claimed.
- **The `1  0` hard stop was enforced this time** — checked and gated in the
  staging script before `commit-tree`, and the ref moved with
  `update-ref <new> <old>` so a concurrent commit would have aborted it
  rather than being overwritten. HEAD had moved twice again before this
  round (`ff074ab6`, then `6dde88b0`/`9a42dce1` and a `WIP` commit).

## 8.7 Still owed

- **The composition** (§8.3) — one relaxation per element, not one per module.
- Falsifier viii / H5a's falsifier vi: the post-mix-volume error, needing a
  transient reference.
- §6's linearization caveat: a long step against many short ones. §8.2's
  `dt`-refinement table is the beginning of that gate and could become it.
- Falsifier vi, with the §8.5 caveat that a 1D deck may not be able to
  express it at all.
