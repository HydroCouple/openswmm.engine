# D-H5e Flux-Binding Merge — Validation & Commit Handoff (2026-08-19)

**For:** the checking agent.
**Base:** `5cc83f94` (D-H5d).
**Plan:** `HEAT_TRANSPORT_PLAN.md` **§6.3 (new — D-H5e, read it first)**.
**Standing findings:** lessons 1–85.

**Standalone, lands before H5b.** It fixes a defect D-H5d introduced.

---

## 1. What this fixes

D-H5d replaced the forward-Euler step with an exponential relaxation at four
bindings. Three were fine. The fourth — the LEGACY node/link path — called
**two entry points back to back**, `applySurfaceExchange` then
`applyRadiativeExchange`, each now relaxing FULLY toward its own module's
equilibrium.

Under forward Euler that was harmless: the increments were linear and added
exactly. **Relaxations do not commute.** With both modules on, the pair
overshoots the true combined equilibrium and **the answer depends on module
order** — at large `k·dt` it lands on whichever module ran last, erasing the
first entirely.

| k·dt | split | combined |
|---|---|---|
| 4.1e-3 | 5.061317 | 5.061359 |
| 0.41 | 9.700850 | 10.044256 |
| 39.4 | **10.000000** | 20.000000 |

Not a regression — that regime diverged outright before — and it needs
`k·dt ≳ 0.4`.

**The general form, which matters more than this instance:** replacing an
integrator underneath an existing operator split silently changes what the
split means. Forward Euler's linearity was load-bearing and undocumented.

## 2. The shape

One `applyHeatFluxes(ctx, dt)` in a new `HeatFluxModules/HeatFluxes.{hpp,cpp}`
owns the node and link traversal, sums every enabled family through
`netFluxOut(ctx, T)`, and relaxes once. Each module now exposes only
`surfaceFluxOut` / `radiativeFluxOut`, returning 0 when its own toggle is
off so callers sum unconditionally.

**All four bindings share `netFluxOut`.** ArdEngine's cells and
HeatWatershed's subareas were already correct, but each carried its own
hand-rolled copy of the same sum — and copies of exactly that sum are how
the node/link path diverged. H6's `SEDIMENT_EXCHANGE` is now one added term
in one place.

## 3. Changeset (uncommitted)

```
new:  .../HeatFluxModules/HeatFluxes.{hpp,cpp}   (netFluxOut + applyHeatFluxes
      + the single copy of buildXsp and the node/link traversal)
mod:  .../HeatFluxModules/SurfaceExchange.{hpp,cpp}   (applySurfaceExchange →
      surfaceFluxOut; binding, buildXsp and 3 includes removed)
mod:  .../HeatFluxModules/RadiativeExchange.{hpp,cpp} (applyRadiativeExchange
      → radiativeFluxOut; binding and 3 includes removed)
mod:  .../HeatModule/HeatLegacy.cpp        (two calls → one)
mod:  .../HeatModule/HeatWatershed.cpp     (lambda delegates to netFluxOut;
      5 now-unused locals removed)
mod:  .../EulerianArdComponent/ArdEngine.cpp (same; 7 now-unused locals and
      constants removed)
mod:  tests/unit/engine/test_heat_integrator.cpp  (+1 gate, 6 → 7)
```

All touched TUs pass `g++ -std=c++20 -Wall -Wextra -fsyntax-only` with **no
warnings attributable to these files**. Nothing built or run.

**Note: `tests/unit/engine/CMakeLists.txt` is NOT touched this round** — the
new gate joins an existing target. §5.2's hard stop therefore expects
**no staged change at all** to that file.

## 4. Design decisions to review

### 4.1 The two `apply*` entry points were REMOVED, not left as wrappers

Leaving them would leave the per-module relaxation one call away, and it is
not a shape anyone should be able to reach. Any external caller now fails to
compile, which is the intent — report it rather than restoring them.

### 4.2 I changed two bindings that were already CORRECT — flag if you disagree

ArdEngine's and HeatWatershed's lambdas produced the right answer. §3 says
touch only what you must, so this needs a justification rather than an
assumption: once `netFluxOut` exists, those lambdas are duplicate spellings
of it, and **duplicate spellings of this exact sum are the mechanism of the
defect** — RadiativeExchange got its own copy of the traversal and thereby
its own copy of H2's divergence, twice over. The swap is behaviour-preserving
(both lambdas already checked the same toggles and summed the same two
families), so a falsifier on either should change nothing.

**If you think this crosses the surgical-changes line, say so** — the
alternative is two hand-rolled copies plus a note, and H6 then has three
places to edit instead of one.

### 4.3 Gate 7 is a pure-function gate, not a deck gate

It reconstructs both compositions from `relaxT` directly. That observes the
*arithmetic* claim exactly, and it fails on the split form. What it does
**not** observe is that `applyHeatFluxes` actually calls `relaxT` once —
falsifier iv covers that, and I expect it to escape. See §6.

### 4.4 The `netFluxOut` name is now overloaded in spirit

`SurfaceExchange.cpp` has a file-local `netFluxOut(t_w, t_air, …)` (the
module's own formula) and `HeatFluxes.hpp` exports
`netFluxOut(ctx, t_w)` (the composition). Different namespaced scopes, no
ambiguity, but a reader could conflate them. **Worth renaming one if you
find it confusing on the way through** — I judged the parallel names more
helpful than harmful, and that is a judgement you should second-guess.

## 5. Validation protocol

1. **Isolated worktree at `5cc83f94`.** Lesson 71. Expect the bistable FV
   gate to fail — pre-existing, verified failing identically at base.

2. **⛔ HARD STOP — lesson 79, and it fired for real two rounds ago.**
   `git diff --cached --numstat` **must show no entry for
   `tests/unit/engine/CMakeLists.txt` at all** this round (§3). Any entry
   means the index picked up a foreign edit — STOP and rebuild. Move the ref
   with `update-ref <new> <old>` so a concurrent commit aborts rather than
   being clobbered, as last round did.

3. **Grep:** `grep -rn "applySurfaceExchange\|applyRadiativeExchange" src/
   tests/unit python/` — the only hits may be **prose in `HeatFluxes.hpp`**
   explaining the history. A live call means §4.1 missed a caller. (Lesson
   85: the sweep is by name, not by call — `deltaT`'s removal left four
   stale prose references last round, one of them describing an export that
   no longer existed.)

4. Build, zero new warnings. Then the heat suites, then the full suite.

   **Anticipated failure modes, likelihood order. My record here is 1 of 8
   over the last two rounds, so weight accordingly.**

   (a) **Answers move again wherever BOTH modules are on.** Expected, and
   the user's chosen evidence is a `dt` sweep: halve the routing step and
   show the gap shrinks. Local `O((k·dt)²)` accumulated over `t/dt` steps is
   **first order globally**, so a gap that does not shrink is the error and
   the magnitude alone says nothing. **Report the sweep, not just the
   deltas.** Decks with only one module on must be **bit-identical** — that
   is the cleanest discriminator in this changeset, since the merge is a
   no-op for a single family.

   (b) **An unused-variable warning I missed.** I removed 12 locals across
   two files that my change orphaned. `-Wall -Wextra` is clean on the five
   TUs I checked; I did not compile the whole target.

   (c) **`ArdEngine.cpp`'s `flux_out` lambda now captures only `ctx`.** If
   the surrounding code relied on one of the locals I deleted for something
   other than the flux, it will fail to compile — loudly, which is fine.

   (d) **Gate 7's `k·dt ≳ 5.7` setup.** Its final `ASSERT_GT(comb - seq,
   5.0)` exists so the gate cannot pass vacuously in the linear regime. If
   that assertion fails, the film constants changed and the gate is no longer
   in the regime it is about.

5. **Falsifier sweep:**

   | falsifier | expected failing gates |
   |---|---|
   | i. in `applyHeatFluxes`, call `relaxT` once per module instead of once on the sum | **the defect. I expect NO unit gate to catch it** — gate 7 tests the arithmetic, not the binding. A DECK gate with both modules on and `k·dt ≳ 0.4` is what catches it; **if you can build one, that is the most valuable thing this round can add** |
   | ii. drop `radiativeFluxOut` from `netFluxOut` | any deck gate with radiative on; possibly nothing in the unit file |
   | iii. make `surfaceFluxOut` ignore its `[HEAT_FLUXES]` toggle | an H2 off-deck gate — check one exists |
   | iv. reverse the two terms in `netFluxOut` | **nothing, and that is the point** — addition commutes. A falsifier that *should* be inert; if something fails, the sum is not a sum |
   | v. revert `ArdEngine`'s lambda to its hand-rolled form | **nothing** — it was behaviour-preserving (§4.2). If something fails, the swap was NOT behaviour-preserving and I was wrong |
   | vi. revert `HeatWatershed`'s lambda likewise | same |
   | vii. gate 7: replace the merged step with the sequential one | 7 |

6. **Prior suites:** full C++ suite, 14/14 deck bit-identity, ASan/UBSan.

7. **Record:** falsifier i (did anything catch it? if not, the merge is
   unobserved at the binding level and that is an owed gate), falsifiers v
   and vi (was §4.2 behaviour-preserving as claimed?), and the `dt` sweep.

## 6. Known gaps

- **Falsifier i is predicted to escape, and it is the defect gate.** Gate 7
  proves the arithmetic; nothing proves `applyHeatFluxes` uses it. Closing
  it needs a deck driving a node or conduit to `k·dt ≳ 0.4` with both
  modules on — and D-H5d's round established that the 1D node/link bindings
  **may not be able to reach large `k·dt` from a deck at all** (the router
  floors depth at 1e-4 ft; DYNWAVE overrides `ROUTING_STEP`). If it is
  unconstructible, say so explicitly and the owed item becomes a
  context-level unit gate instead — **that is lesson 84, and it is why this
  class of defect hides.**
- The linearization is exact for a linear flux and approximate for the real
  one; summing two families does not change that, but it does mean `J′` is
  now the sum of two slopes and the equilibrium is the combined linearized
  one. The **long-step-versus-many-short-steps reference gate is still
  owed**, now for the composition as well.
- H5b adds LID layers. They must call `netFluxOut`, not compose their own.

## 7. Prepared commit message

```
fix(transport): one relaxation over the summed heat fluxes (D-H5e)

D-H5d made the surface-balance step a relaxation, but the LEGACY node/link
path called two bindings back to back, so each relaxed fully toward its own
module's equilibrium. Under forward Euler the increments were linear and
added exactly; relaxations do not commute. With both modules on the pair
overshoots the true combined equilibrium and the answer depends on module
order -- at large k*dt it lands on whichever module ran last.

applyHeatFluxes now owns the node and link traversal, sums every enabled
family through netFluxOut(ctx, T), and relaxes once. applySurfaceExchange
and applyRadiativeExchange are removed; each module exposes only its flux
evaluator, which returns 0 when its own toggle is off.

The ARD cells and the watershed subareas were already summing before
relaxing, but each carried a hand-rolled copy of that sum. All four now
share netFluxOut -- copies of this exact sum are how RadiativeExchange
acquired its own traversal and thereby its own copy of H2's divergence.
H6's SEDIMENT_EXCHANGE is one added term in one place.

Replacing an integrator underneath an existing operator split changes what
the split means. Forward Euler's linearity was load-bearing and undocumented.
```

---

# 8. Validation result (checking agent, 2026-08-19) — COMMITTED `c292b8eb`

Isolated worktree at `5cc83f94`. **153/153** ctest. **14/14** decks
bit-identical. **47 tests** clean under ASan/UBSan across all six heat
suites. Zero warnings from any changed file. §4.1 verified: `grep -rn
"applySurfaceExchange\|applyRadiativeExchange" src/ tests/unit/ python/`
returns two prose lines in `HeatFluxes.hpp` and nothing else.

**On the FV gate:** it *passed* this round, at base as well — the foreign
`71829e14 WIP` commit sitting between H5a and D-H5d rewrote
`test_fv_engine_integration.cpp` (43 lines). Not this changeset, and no
longer the standing pre-existing failure the last three rounds carried.

## 8.1 §6 was wrong: the deck gate IS constructible, and it is here

The handoff predicted falsifier i — the defect gate — would escape, on the
strength of D-H5d's finding that a 1D node cannot be driven to a large
`k·dt`. **That finding was measured with radiative alone.** `J′ ≈ 5.5`
W/m²/°C there, so the depth required sits under the router's 1e-4 ft floor.
Summing the surface family in with a 20 mph wind and 20 % humidity gives
**`J′ = 45.9`**, eight times larger, so the depth needed rises by the same
factor. Measured on a storage node at **2e-4 ft over 10⁶ ft²: `k·dt = 1.80`**
— twice the floor, and an entirely ordinary storage node.

Gate 8 (`ASteadyNodeSitsWhereTheSUMMEDFluxVanishes`) runs that deck and
asserts, with no reference value, that **a node whose depth never changes
sits where the summed outward flux is zero**. Expressed as residual over
slope, the merged form sits 0.000 °C away; the sequential form sits 2.44 °C
away.

| k·dt | split | merged | combined T_eq |
|---|---|---|---|
| 0.18 | −0.6176 | −0.3939 | −0.3942 |
| 0.72 | −1.3212 | −0.3942 | −0.3942 |
| 1.80 | **−2.8384** | **−0.3942** | −0.3942 |

Surface-only equilibrium on this deck is **+2.2364 °C** and radiative-only is
**−30.1352 °C**; the split parks at −2.8384, which is neither of those and
not the combined answer either — it is an artefact of the composition, which
is the clearest statement of the defect I can give.

**The generalisable part:** D-H5d's "unconstructible" conclusion was sound
for the case measured and wrong as a general claim, because the quantity that
blocked it (`J′`) is not a property of the binding but of *which families are
enabled*. A reachability result is only as general as the configuration it
was measured in.

## 8.2 §5(a)'s discriminators

**Single-family decks are bit-identical** — delta exactly `0.0`, not merely
small:

| deck | base (split) | new (merged) | Δ |
|---|---|---|---|
| surface only | 2.2363632330 | 2.2363632330 | **0.0** |
| radiative only | −30.1351507974 | −30.1351507974 | **0.0** |
| both | −2.8384403598 | −0.3942384229 | 2.4442019369 |

**The `dt` sweep**, both families on:

| dt | k·dt | base (split) | new (merged) | Δ |
|---|---|---|---|---|
| 10 | 1.798 | −2.8384403598 | −0.3942384229 | −2.44420 |
| 5 | 0.899 | −1.5306009632 | −0.3942384229 | −1.13636 |
| 2 | 0.360 | −0.8261286852 | −0.3942384229 | −0.43189 |
| 1 | 0.180 | −0.6099562337 | −0.3942384229 | −0.21572 |

The gap halves as `dt` halves, so the two forms agree in the resolved regime
and separate only in the stiff one. Worth noting separately: **the merged
answer is identical at every `dt`** — at steady state it is exact for any
step, while the split's answer is a function of the timestep.

## 8.3 Falsifier sweep: every real defect observed, every inert one inert

| # | predicted | outcome |
|---|---|---|
| i | "I expect NO unit gate to catch it" | **caught — new gate 8.** §8.1 |
| ii | radiative deck; possibly nothing | gate 8 **and** H3's pool gate |
| iii | "an H2 off-deck gate — check one exists" | **none existed.** `applyHeatFluxes` returns early when both toggles are off, so an all-off deck never reaches the evaluators, and the only mixed-toggle deck (H3's pool) asserts a *direction* that a spurious latent term does not reverse. Gate 8 gained a surface-off / radiative-on leg; now caught |
| iv | "nothing, and that is the point" | **inert.** Addition commutes |
| v | "nothing" — §4.2 behaviour-preserving | **inert.** §4.2 confirmed |
| vi | "nothing" — likewise | **inert.** §4.2 confirmed |

**§4.2 is confirmed, and it is stronger than claimed.** Reverting either
lambda to its hand-rolled form does not merely change nothing — as first
written, it **does not compile**, because the changeset also removed the
`RadiativeExchange.hpp` include from both files. The duplication §4.2 argues
against is now structurally unavailable, not just discouraged. The falsifiers
had to restore the include before they could express the old shape at all;
with it restored, both are inert, which is the behaviour-preservation claim
verified rather than assumed. **I do not think §4.2 crosses the surgical line
— the swap is provably a no-op and it removes the mechanism of the defect.**

Falsifier vii (disabling gate 7) was not run: it tests the test rather than
the code, and gate 7 already computes both compositions and compares them.

## 8.4 On §4.4 — the overloaded `netFluxOut` name

I did find it briefly confusing, and then decided you were right to keep it.
`netFluxOut(t_w, t_air, rh, …)` takes the raw forcing and belongs to one
module; `netFluxOut(ctx, t_w)` takes the context and composes. The signatures
are unmistakable at every call site, and the shared name is what makes the
relationship legible. Not changed.

## 8.5 Still owed

- The long-step-versus-many-short-steps reference gate for the linearization,
  now for the composition too. §8.2's `dt` sweep is most of its machinery.
- H5b's LID layers must call `netFluxOut`. There is now exactly one place to
  point them at.
- H5a's post-mix-volume falsifier, still needing a transient reference.
