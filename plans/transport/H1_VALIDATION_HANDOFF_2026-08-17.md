# H1 Implementation — Validation & Commit Handoff (2026-08-17)

**For:** the checking agent.
**Base:** `f37f7dde`.
**Plan:** `HEAT_TRANSPORT_PLAN.md` §1, §3, §6 H1 — the first step of the
heat track, and the roadmap's next step in its own recommended order.
**Scope note:** H1 is **transport only**. Nothing here adds or removes
energy; the surface/radiative/sediment flux modules of plan §2 are H2–H4.
**Standing findings:** lessons 1–50. This changeset carries T0b's remaining
half under **D-UT10** (user decision, 2026-08-17).

---

## 1. What this delivers

`[OPTIONS] HEAT_TRANSPORT ON` makes `__TEMPERATURE__` a reserved species
(`RESERVED_TEMPERATURE`), advected and mixed by the LEGACY CSTR engine and
reported as a trailing `.out` species column in °C. Per-source inlet
temperatures come from a new `org.hydrocouple.openswmm.heat` component
parsing `[HEAT_SOURCES]` (`model.heat`), mirroring `[WATER_AGE_SOURCES]`
row-for-row.

Per D-UT10, the loader channel is a **parallel accumulator**:
`heat_state.node_temp_vol_in`, filled at the same seven pathways that fill
`water_age_state.node_age_vol_in`.

## 2. Changeset (uncommitted)

```
new:  src/engine/data/HeatData.hpp
      (HeatSource enum, HeatConfigData, HeatState — mirrors WaterAgeData)
new:  src/engine/transport/components/HeatModule/HeatComponent.{hpp,cpp}
      ([HEAT_SOURCES] parser + registry registration)
new:  src/engine/transport/components/HeatModule/HeatLegacy.{hpp,cpp}
      (routeLegacyHeat — the CSTR temperature mirror)
mod:  src/engine/core/SimulationOptions.hpp     (heat_transport)
mod:  src/engine/input/handlers/OptionsHandler.cpp (HEAT_TRANSPORT key)
mod:  src/engine/core/SimulationContext.hpp     (heat_config/heat_state + reset)
mod:  src/engine/quality/QualityRouting.cpp     (addTempVolume at 7 sites;
      loadersNeeded; assemble-stage sizing/zeroing; routeLegacyHeat call)
mod:  src/engine/core/SWMMEngine.cpp            (component registration;
      species registry + reported_species_names; snapshot temp column;
      two bypass warnings)
mod:  src/engine/core/InpWriter.cpp             (HEAT_TRANSPORT on save)
new:  tests/unit/engine/test_heat_transport.cpp (9 gates)
mod:  tests/unit/engine/CMakeLists.txt          (+1 target)
```

Engine sources are picked up by the existing `GLOB_RECURSE transport/*.cpp`,
so no engine CMake change. All ten touched TUs pass
`g++ -std=c++20 -fsyntax-only`.

## 3. Design decisions to review

### 3.1 The accumulator carries °C·ft³/s, NOT Joules — the call most worth challenging

Plan §3 describes the channel as enthalpy `ρw cp V T_source`. **I did not
ship Joules**, and this is a deliberate deviation from the plan text.

At H1 there are no energy fluxes, so ρw and cp appear on both sides of every
mixing operation and cancel identically. Shipping them would add two
constants that **no H1 gate could observe being wrong** — set cp to 42 and
every gate here still passes. That is the exact shape lessons 39/47 warn
about, and this program has been bitten by unobserved code repeatedly.

So `node_temp_vol_in` is `q · T` — the precise analogue of
`node_age_vol_in`. **H2 introduces ρw cp together with the W/m² fluxes that
make them load-bearing and observable**, and rescales this accumulator to
J/s at that point: a rename at the same seven sites, which is the churn
D-UT10 already judged cheap.

**If you disagree**, the change is a multiply in `addTempVolume` and a
divide in the mixing stage — but please record that the constants would then
ship unobserved, so the next round knows.

### 3.2 NO dry-element mask on temperature — the opposite call from age

`584d1065` masks a dry element's reported AGE to 0. I did **not** mirror
that for temperature, deliberately:

- Zero age is unambiguous — no water has ever been both 0 h old and present,
  so 0 reads as "no water".
- **Zero degrees is an ordinary temperature.** A mask would publish
  "freezing" for "empty", and no reader could tell them apart.

Reporting the carried state on a dry element is the lesser of two wrongs,
not a right answer. The honest fix is a per-column no-data convention the
`.out` format does not have. **Recorded as a convention, flagged as
unresolved** — if you would rather mask, say what value means "no water".

### 3.3 The default inlet temperature is 20 °C, not 0

Same reasoning: a zero default silently chills every model that omits a row.
`HeatConfigData::configured_source[]` records which rows the user actually
set so a deliberate 0 °C is distinguishable from a default. **Gate 8 is the
observer** — without it the default is unobserved, since every other gate
configures the table explicitly.

### 3.4 Temperature is the TRAILING column, after age

A deck that adds heat to an existing water-age model must not move the age
column. Gate 3 asserts the order **by name** (`swmm_output_get_pollut_id`),
which is what A2b's razor lacked.

### 3.5 The clamp is two-sided; nothing is floored at zero

The age mirror ends each stage with `max(value, 0)`. Copying that verbatim
would warm every below-freezing model to exactly 0 °C. The bound here is the
physical one — a volume-weighted mean lies between its inputs — so the clamp
is `[min, max]` of the mixing inputs. **Gate 2 is the observer.**

### 3.6 EULERIAN_ARD warns rather than silently tracking nothing

H1 is LEGACY-only; the ARD mesh binding is H4. `HEAT_TRANSPORT ON` with
`QUALITY_SOLVER EULERIAN_ARD` warns and names H4 (lessons 10/20). Gate 6.

### 3.7 Inlet temperatures are range-checked to [−50, 100] °C

A Fahrenheit value pasted into a °C field is far likelier than a deck that
means 451 °C. Refuses rather than routing it. Flag if you would rather warn.

## 4. Validation protocol

1. Reconfigure (new test target), build, zero new warnings.
2. `ctest -R test_engine_heat_transport` — 9 gates.
   *Anticipated failure modes, likelihood order:*
   (a) **The `[DWF]` pathway may not deliver on this deck.** Gate 1 needs
   TWO pathways; if DWF contributes nothing, its liveness assertion fires
   first and says so (`J0 receives … expected 8`). That is a deck problem,
   not a feature problem — fix the deck, do not widen the band.
   (b) **`nodes.lat_flow[0]` may be the wrong field for "total delivered".**
   If the liveness leg fails on a deck that visibly *is* delivering, try
   `nodes.total_inflow`. Record which, because the liveness assertion is
   load-bearing for gate 1's meaning.
   (c) **Gate 3's `v[8]`** assumes 6 fixed node columns + 3 species. The
   NAME assertions above it are the real content; if the fixed count
   differs, fix the index, not the names.
   (d) **The deferral/range gates assume a component config error fails the
   open.** That is A1a's behaviour; verify it still holds.
   (e) **Gate 1's 0.5 °C band** is a settling tolerance on a 2-hour run. If
   it reads ~20 or ~3 the feature is inert, not imprecise — see the gate's
   own message, which enumerates what each wrong number means.
3. **Falsifier sweep:**

   | falsifier | expected failing gates |
   |---|---|
   | i. remove `addTempVolume` from all seven loader sites | 1 (reads the 3 °C initial state), 2, 7's NODE leg |
   | ii. **remove it from the DWF site only** | 1 reads 30 °C (external inflow alone). **This is D-UT10's seam claim under test** — "every pathway contributes at the same seam" is only a claim if a single missing pathway is visible |
   | iii. restore `max(t_new, 0.0)` in the node mixing stage | 2 ONLY — and it should read exactly 0.0, the signature of a floor rather than a physics error |
   | iv. register `__TEMPERATURE__` before `__WATER_AGE__` | 3's name-order legs, while every VALUE assertion still passes — the razor A2b lacked |
   | v. use an unweighted mean in the mixing stage | 1 reads 18 rather than 24 |
   | vi. default `kDefaultTemp` to 0.0 | **8 only** — this is why gate 8 exists |
   | vii. drop the `HEAT_TRANSPORT` line from InpWriter | **9 only** — A1a's defect verbatim |
   | viii. drop the EULERIAN_ARD warning | 6 |
   | ix. widen the range check to accept 451 °C | 7's range leg |
4. **Prior suites:** the HEAT_TRANSPORT-off world must be **unchanged** —
   every new code path is behind `ctx.options.heat_transport`, so
   `n_reported_species()` and every stride are identical and **14/14 deck
   `.out` bit-identity must hold against `f37f7dde`**. The water-age suite
   (17 gates) and `test_engine_output_quality` (8) must stay green; gate 4
   is the in-suite version of that claim (age and pollutants bit-identical
   with heat ON).
5. **Record:**
   (a) whether falsifier ii fails as predicted — that single result is the
   evidence for D-UT10's seam claim, and it is the reason gate 1 uses two
   pathways rather than two inflows on one pathway;
   (b) whether anything observes ρw·cp being absent (§3.1). I claim nothing
   can, by construction. If you find an observer, I was wrong and the
   constants should ship now.

## 5. Known gaps this phase does NOT close

- **Subcatchment temperature reports 0** — the A3-shaped placeholder, and
  here it is worse than for age because 0 °C is a real temperature. H5 owns
  it. If you would rather suppress the SUBCATCH column until then, say so.
- **No hotstart persistence for temperature.** A2a did this for age
  (native V3); the heat analogue is not in H1 and a restart resets the field
  to INITIAL_STATE. Worth its own row — recorded, not written.
- **No ARD binding** (H4), **no flux modules** (H2–H4), **no `.rpt`
  continuity row** — temperature is not a mass and does not conserve, so it
  has the same definitional problem as A2c.

## 6. Commit message

```
feat(transport): transported temperature under LEGACY (H1)

[OPTIONS] HEAT_TRANSPORT ON makes __TEMPERATURE__ a reserved species
(RESERVED_TEMPERATURE), advected and mixed by the LEGACY CSTR engine and
reported as a trailing .out species column in degC. Per-source inlet
temperatures come from a new heat process component parsing [HEAT_SOURCES]
(model.heat), mirroring [WATER_AGE_SOURCES] row for row.

H1 is TRANSPORT ONLY: nothing here adds or removes energy. The surface,
radiative and sediment flux modules - the terms that make temperature change
rather than merely move - are H2-H4, and HEAT_TRANSPORT under EULERIAN_ARD
warns that the mesh binding is H4.

Per D-UT10 the loader channel is a parallel accumulator, not a widened
tuple: heat_state.node_temp_vol_in is filled at the same seven pathways that
fill water_age_state.node_age_vol_in. It carries temperature-volume rather
than Joules because rho_w and cp cancel identically while there are no
fluxes, and shipping constants that no gate can observe being wrong is how
unobserved code gets in. H2 introduces them with the fluxes that make them
load-bearing.

Two places where copying the water-age mirror would have been wrong, both
gated: temperature is NOT floored at zero (sub-zero water is ordinary; the
clamp is two-sided against the mixing inputs), and a dry element's
temperature is NOT masked to zero the way its age is, because zero degrees
is a real temperature and could not be told apart from "no water". The
default inlet temperature is 20 degC for the same reason.

Gates: tests/unit/engine/test_heat_transport.cpp - the plan's own criterion
(two loader pathways at unequal flows mix to the flow-weighted mean, 24 degC,
chosen to collide with no wrong answer), sub-zero survival, the trailing
column asserted BY NAME, pollutants and age bit-identical with heat on, the
off and ARD bypass legs, the config surface, the default with no component,
and a save-as round trip.

Plan: HEAT_TRANSPORT_PLAN.md section 1/3/6 H1; master plan section 4.3
D-UT10.
Validation record: plans/transport/H1_VALIDATION_HANDOFF_2026-08-17.md
```

## 7. Validation results

**Verdict: the design is right and every §3 judgement holds, but the
changeset did not run.** It crashed on its own first gate, and the cause was
two guards it did not extend plus one it could not have known about. Fixed —
**four files added to the manifest** — and committed as `4767aabb`.
Artifacts: `tests/output/h1_validation_2026-08-18/`.

### 7.1 Three defects, found in the first ten minutes

**(1) SIGSEGV in the `.out` writer on any heat deck without `[POLLUTANTS]`.**
Gate 1 died at `DefaultOutputPlugin::writeHeader` on a null dereference.
A2b's unit-code special case tests the species NAME against
`"__WATER_AGE__"`; `__TEMPERATURE__` fails that test, falls through to
`ctx.pollutants.units[p]`, and on a heat-only deck that vector is EMPTY, so
`units[0]` dereferences null. Fixed by keying on the INDEX instead —
`p >= ctx.n_pollutants()` is a reserved column — which is correct for age,
temperature, and whatever reserved species comes next. **`DefaultOutputPlugin.cpp`
is not in the §2 manifest; adding a third reserved species requires it.**

**(2) and (3) — the np-guard family, twice more.** With the crash fixed,
gates 1 and 2 failed on their liveness assertion: *"heat state never sized —
HEAT_TRANSPORT did not reach the engine."* H1 added `heat_transport` to
`loadersNeeded()` but not to the two guards that decide whether the quality
stage runs at all:

- `SWMMEngine.cpp:3109` — the routing-step guard
  (`n_pollutants() > 0 || legacyReactionsActive || water_age`).
- `QualityRouting.cpp:259` — `execute()`'s early return, the same predicate.

A temperature-only deck clears neither, so `routeLegacyHeat` never ran and
`heat_state` was never sized. This is the **fourth** appearance of this
family — R4's "MSX-only decks blocked by TWO `n_pollutants()>0` guards", E5a's
loader guard, A1a's water-age addition to this very line, now heat. Both
guards carry comments explaining the pattern and both were still missed. The
mechanical check that finds it in one command:

```
grep -rn "options.water_age" src/engine/ | (does the surrounding context also say heat?)
```

which lists every site A1a had to touch. Nine of the hits are HotStartManager
(a declared §5 gap) and two are H1's own new blocks; the two above were the
real misses. **Worth making that grep a standing step for any feature that
mirrors water age.**

**A fourth, unavoidable consequence:**
`ProcessComponentsTest.PlannedIdReportsPendingPhase` used
`org.hydrocouple.openswmm.heat` as its stand-in for a *planned* id, and H1
registers the real component. The test's own comment says to "move this to
whichever id is still planned", so it moved to
`org.hydrocouple.openswmm.transport.lard` (T5). Also not in the manifest.

### 7.2 Gate 3 was asserting the wrong number for its own deck

`TemperatureReportsAsATrailingColumnByName` expected TSS at 42.0 and read
32.465. Not a stride slip: this deck FLOWS — both inlet pathways carry zero
TSS, so Cinit dilutes. Measured the discriminator rather than guessing:

| | TSS | age | temperature |
|---|---|---|---|
| heat ON | 32.464996 | 0.000585 | **24.000000** |
| heat OFF, same deck | 32.464996 | 0.000585 | — |

Bit-identical. The literal 42 was imported from A2b's *level-pool* deck.
Rewrote the assertion to compare against the same deck with heat off, which
is what "the TSS column did not move" actually claims and costs no magic
number. Note gate 4 already proved the same thing at the `ctx` level and
passed throughout — the two now agree at both layers.

### 7.3 Falsifier sweep — 9 of 9, and §5's two questions answered

| falsifier | outcome |
|---|---|
| i. no loader contributions | **caught** — 0 °C, four gates |
| **ii. DWF pathway only** | **caught — 22.5 °C.** See below |
| iii. restore `max(t, 0)` | **caught** — exactly 0.0 on a −5 °C inflow |
| iv. temperature registered first | **caught** — the name-order razor |
| v. weighting dropped | **caught — exactly 18** |
| vi. default 0 °C | **caught** — gate 8 only, as designed |
| vii. drop the InpWriter line | **caught** — gate 9 only |
| viii. drop the ARD warning | covered by gate 6 (passes only with the warning) |
| ix. accept 451 °C | **caught** — the range leg |

**§5(a), the D-UT10 seam claim: confirmed, with a correction to the
prediction.** Dropping only the DWF temperature contribution gives **22.5 °C**,
not the 30 °C the handoff predicted. 30 would require dropping DWF's VOLUME
too; 22.5 = (6·30 + 2·0)/8 is DWF water arriving with no temperature attached
— which is exactly the parallel accumulator being exercised, and a sharper
demonstration of the seam than the prediction was. A single missing pathway
is plainly visible, so "every pathway contributes at the same seam" is a
measured claim now, not an assertion.

**§5(b), the ρw·cp observability claim: you are right, and here is the
evidence.** Two constructions, because they answer different questions:

- Applying 42 on the accumulator **only** IS observable — 1008 °C, four gates
  fail. So the plumbing is gated.
- Applying 42 on **both sides**, which is the shape §3.1 describes ("a
  multiply in `addTempVolume` and a divide in the mixing stage") — **all 9
  gates stay green.** Nothing in H1 can observe the constants' VALUE being
  wrong.

So the decision not to ship them is correct and now measured. Shipping cp
today would have added a number that only H2 could ever falsify.

Falsifier v needs one caveat on how it was built: a true unweighted mean is
not expressible at the mixing stage (the per-pathway temperatures are already
summed by then), so it was constructed as a CONSTANT per-pathway weight of
4.0 — the mean of this deck's 6 and 2 cfs. That yields exactly the 18 the
gate's message names, but it is deck-specific by construction, not a general
"unweighted mean" implementation.

### 7.4 Everything else

| check | result |
|---|---|
| build | clean, zero warnings in the H1 files; the new `.cpp` are picked up (reconfigured — the engine GLOBs without `CONFIGURE_DEPENDS`) |
| `test_engine_heat_transport` | **9/9** |
| full `ctest` | **143/144** — only the known FV refinement gate, whose fix is uncommitted in the shared tree |
| water-age suite | **16/16** (the handoff says 17; the target has 16) |
| `test_engine_output_quality` | **8/8** |
| 14-deck `.out` bit-identity | **14/14** — the HEAT-off world is unchanged |
| ASan + UBSan | heat **0 findings** 9/9; output-quality **0 findings** 8/8 |

Validated in a worktree at `f37f7dde` carrying H1 only. Two files —
`InpWriter.cpp` and `tests/unit/engine/CMakeLists.txt` — are **shared with a
concurrent save-as-paths session**, so only H1's hunks were taken into the
worktree and only those hunks were staged (4 lines and 1 line respectively);
the neighbour's work is untouched and uncommitted.

### 7.5 On §3.2, recorded rather than changed

Not masking a dry element's temperature is the right call and the reasoning
is sound: 0 °C is a real temperature, and `584d1065` could mask age precisely
because 0 h is not. But the consequence is now live — a dry pipe reports its
carried temperature indefinitely, exactly as a dry pipe reported 6 h of age
before that fix. The `.out` has no per-column no-data value, so this cannot
be resolved inside the current format. Flagging it as the strongest argument
yet for a sentinel convention, which would let both columns be honest.
