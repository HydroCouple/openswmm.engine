# H5b Implementation — Validation & Commit Handoff (2026-08-19)

**For:** the checking agent.
**Base:** `c292b8eb` (D-H5e).
**Plan:** `HEAT_TRANSPORT_PLAN.md` §6 H5b and **§6.1 D-H5b — which now
carries a CORRECTION (lesson 90) to the thermal parameters it originally
named. Read that first; it changed the numbers this phase is built on.**
**Standing findings:** lessons 1–89.

---

## 1. What this delivers

Temperature through the LID layer stack: `LidSpecies::TEMPERATURE = 1` on
A4's existing per-(unit, layer, species) block, advective mixing on the
published inter-layer inflows, **vertical conduction** (D-H5b), the
underdrain leaving at its storage-layer temperature, and the D-H5c dry-layer
policy.

It also closes **H5a's owed third run-on contributor** — the LID drain,
which could not have a temperature until this row existed.

## 2. A correction to the plan, found before any code was written

D-H5b told the implementer to follow HydroCouple's `GWComponent` and quoted
`sedDensity = 2650` kg/m³, `sedCp = 880` J/kg/°C from
`gwmodel.h:870-871`. **Those are dead in-class initializers.** The
constructor's member-init list overrides both: `m_sedDensity(1970)`,
`m_sedCp(2758)` (`gwmodel.cpp:51-52`). Effective `ρ·cp` differs by **2.3×**.

This is lesson 69's exact shape — a declaration is not a value — landing on
the parameters the decision itself named, which is the second time a plan
section I wrote has done this (A4's brief §1 was the first). The plan is
corrected in place with a `@warning`, and `ConductionConfig` carries the
provenance per field.

`HTSComponent` uses 1670/1807 for **streambed** sediment. Three defensible
pairs exist in the reference and nothing in it picks one for a LID; taking
GWComponent's is recorded as a **choice**, not an inheritance.

## 3. The thermal step is COLUMN-COUPLED — §4.1 is the call to challenge

Conduction couples adjacent layers, so it is a second operator on the same
state. **Lesson 80 says applying it as its own pass after a per-layer
relaxation does not compose** — that is precisely the defect D-H5e fixed
between SurfaceExchange and RadiativeExchange, and it would have been
reproduced inside a single phase.

Two alternatives were measured, not guessed:

| approach | conserves energy | stable | verdict |
|---|---|---|---|
| per-layer relaxation, frozen neighbours | **no** — the column leaks | yes | rejected |
| explicit conduction | yes | **no** — `k·dt = 1.91` at 60 s on a 1e-4 m film against 0.3 m soil | rejected |
| **implicit coupled solve** | **yes** (5.7e-10 J/m², machine zero) | yes | **shipped** |

Backward Euler over the ≤4-layer tridiagonal system: the inter-layer flux is
evaluated once at the new time and applied equal and opposite, so only the
atmospheric term enters or leaves. The same column with the soil layer alone
gives `k·dt = 5.3e-4` — **conduction looks harmless until a film appears**,
which is why the stiffness had to be computed rather than assumed.

## 4. Design decisions to review

### 4.1 Backward Euler here, exponential relaxation everywhere else

`relaxT` integrates a single element's linear ODE **exactly**; the column
solve is first-order in `dt`. A one-layer LID (a rain barrel) therefore
steps differently from a node of the same volume, though both have the same
fixed point. **Flag if you would rather the column matched** — the only way
to keep exactness under coupling is a matrix exponential over a 4×4 system,
which I judged out of proportion. This is the honest cost of the choice and
it is not hidden in the code.

### 4.2 Adjacency comes from A4's donor map, not a new stack table

`donorsFor` already encodes which layer feeds which, per LID type, and for a
vertical stack that IS the physical adjacency. A second table could disagree
with it silently. Consequently `Donors`, `layerVolumes` and `buildOffsets`
moved out of `WaterAgeLid.cpp` into a new `LidLayerCommon.{hpp,cpp}` — the
D-H5e move, for the same reason.

### 4.3 `ensureSized` — the eighth instance of the lesson-20 trap

A4's `initLidLayerAge` both sized and seeded, under a `water_age` guard. With
two species that is wrong twice: a **heat-only** deck would never size the
block, and an **age-only** deck would leave the temperature row at 0 °C,
which D-H5c exists to say is a real temperature. Both initialisers now call
`ensureSized` first and seed only their own row, so neither depends on the
other having run. **Gate 2 is the observer.**

### 4.4 Every layer is seeded, including dry and absent ones

The age row leaves a dry layer at 0 because 0 is age's "nothing here". A
temperature row must not.

### 4.5 The atmospheric flux acts on the TOPMOST PRESENT layer only

A buried layer has no sky and no wind. For a rain barrel that layer is
storage, which is why the code takes `idx[0]` rather than naming SURFACE.

## 5. Validation protocol

1. **Isolated worktree at `c292b8eb`.** Lesson 71.
   **The FV mesh-convergence failure is GONE (lesson 89)** — the suite was
   153/153 last round. Do **not** carry the old exemption forward; if
   something fails at base, that is news.

2. **⛔ HARD STOP — lesson 79.** `git diff --cached --numstat` must read
   exactly `1  0` for `tests/unit/engine/CMakeLists.txt` (§6 adds one
   target). Any deletion count above zero means STOP and rebuild the index.
   Move the ref with `update-ref <new> <old>` so a concurrent commit aborts.

3. **Greps.**
   - `grep -rn "st.resize(" src/engine/` — every LID-block sizing must go
     through `ensureSized` now (§4.3).
   - `grep -rn "until H5" src/engine/` — must be empty; the
     `HeatSource::RAINFALL` marker is retired in this changeset.
   - `grep -rn "donorsFor\|layerVolumes" src/engine/` — one definition, in
     `LidLayerCommon.cpp`.

4. Build, zero new warnings. Heat suites, then the full suite.

   **Anticipated failure modes. My record is 2 of 12 over three rounds, so
   weight these lightly and trust the sweep.**

   (a) **The `[LID_CONTROLS]` decks may not produce the layer states the
   gates assume.** A4's round found all four setup legs firing because the
   column drained every drop within one step (issue #131). If a gate's SETUP
   assertion fails, fix the deck's layer parameters — **do not** loosen the
   assertion.

   (b) **Gate 5 may find no gradient to narrow.** It asserts
   `off_hi - off_lo > 0.1` first for exactly that reason. If that fires, the
   advection-only column is already uniform and the deck needs a stronger
   contrast between `rain_c` and `init_c`, not a smaller threshold.

   (c) **Gate 6's absent-PAVEMENT assumption.** A bioretention cell has no
   pavement layer, so it is the cleanest dry case — but if `pave_thick`
   arrives non-zero from the unconverted deck, the layer is not absent and
   the policy branch is not the one being tested.

   (d) **Gate 3's `EXPECT_NE(drain, rain_c)`** is a weak observer by design
   (it distinguishes "carried" from "borrowed", not magnitude). If it passes,
   check that the storage temperature is not *coincidentally* near the rain
   temperature on this deck.

5. **Falsifier sweep:**

   | falsifier | expected failing gates |
   |---|---|
   | i. skip the conduction terms in the tridiagonal assembly | **5** — the defect gate for D-H5b. If nothing fails, conduction is unobserved |
   | ii. apply conduction as a SEPARATE pass after the atmospheric step | **probably nothing** — flagged in advance. It is the lesson-80 defect and needs a large `k·dt` column plus both operators on. **If you can build that deck, it is the most valuable thing this round can add**; D-H5e's round showed the required regime IS reachable once the surface family is summed in (lesson 86) |
   | iii. make the inter-layer flux non-symmetric (drop the `+= h` on one side) | **5**, and it should break energy conservation — **check whether any gate observes the asymmetry directly**; if not, that is an owed ledger gate |
   | iv. revert `ensureSized` to `resize` | **2** — both legs |
   | v. seed only wet layers (A4's rule) | 1 |
   | vi. publish the drain from SURFACE for every type | 3 |
   | vii. drop the LID-drain `addRunonTemperatureAt` call | **probably nothing** — gate 7 asserts an invariant that a MISSING contributor satisfies. Owed: a gate that observes the drain's contribution positively |
   | viii. ignore `LAYER_CONDUCTION` and always conduct | 4's `ASSERT_TRUE(layer_conduction)` will not catch it — **expected to escape** |
   | ix. use the header's 2650/880 instead of 1970/2758 | **nothing** — no gate pins the parameter values. Deliberate: pinning them would gate a choice, not a behaviour. **But say so explicitly in the report** |

6. **Prior suites:** full C++ suite, 14/14 deck bit-identity, ASan/UBSan.
   **A4's `test_water_age_lid` suite must be unchanged** — the species
   stride widened underneath it, so if any age value moved, `layer_index` is
   wrong and that is the most dangerous possible outcome of this changeset.

7. **Record:** falsifier i, falsifier iii (is asymmetry observed?),
   falsifier vii, and whether A4's age suite is bit-unchanged.

## 6. Changeset (uncommitted)

```
mod:  src/engine/data/LidLayerSpeciesData.hpp   (TEMPERATURE row; ensureSized)
mod:  src/engine/data/HeatData.hpp              (ConductionConfig;
      layer_conduction; RAINFALL marker retired)
new:  src/engine/transport/components/LidLayerCommon.{hpp,cpp}
mod:  .../WaterAgeModule/WaterAgeLid.cpp        (helpers moved out;
      ensureSized)
new:  .../HeatModule/HeatLid.{hpp,cpp}
mod:  .../HeatModule/HeatComponent.cpp          (LAYER_CONDUCTION key)
mod:  src/engine/core/SWMMEngine.cpp            (init, inflow, route, and the
      LID-drain run-on temperature contributor)
new:  tests/unit/engine/test_heat_lid.cpp       (8 gates)
mod:  tests/unit/engine/CMakeLists.txt          (+1 target — SHARED FILE)
```

All touched TUs pass `g++ -std=c++20 -Wall -Wextra -fsyntax-only`. Nothing
built or run.

## 7. Known gaps

- **Three falsifiers predicted to escape** (ii, vii, viii). ix is a
  deliberate non-gate. Each is named above with what would close it.
- **No CSH/RHE parity reference exists for conduction** — it is the one part
  of the heat track with no external number to check against, because the
  reference implementations model a streambed, not a layered LID. The
  property gates (conservation, bracketing, narrowing) are what stands in
  for parity, and they are weaker.
- The conductivity mixture is **volume-weighted arithmetic**. A geometric or
  Johansen mixture is more standard for soils; both need a second parameter
  set with nothing in HydroCouple to gate them against.
- H5a's owed items are untouched: the transient LID deck for falsifier vi,
  and the sub-zero forcing deck.

## 8. Prepared commit message

```
feat(transport): temperature through the LID layer stack (H5b)

LidSpecies::TEMPERATURE on A4's per-(unit, layer, species) block -- one more
row, no second array, no change to layer_index. Advective mixing on the
published inter-layer inflows, the underdrain leaving at its storage-layer
temperature, and the D-H5c dry-layer policy.

Vertical conduction (D-H5b) is solved WITH the atmospheric flux as one
implicit tridiagonal system over the column, not as a separate pass.
Conduction couples adjacent layers, so it is a second operator on the same
state: applying it sequentially after a relaxation is the composition defect
D-H5e just fixed between SurfaceExchange and RadiativeExchange. Applying it
explicitly is stiff where it matters -- k*dt = 1.91 at a 60 s step on a
1e-4 m film against 0.3 m of soil, the regime that produced H5a's NaN. The
implicit form conserves the column's heat content to machine zero.

Thermal parameters follow GWComponent's CONSTRUCTOR (1970 kg/m3, 2758
J/kg/C), not the dead in-class initializers the plan originally quoted
(2650/880) -- effective rho*cp differs by 2.3x.

Donors, layerVolumes and buildOffsets move to LidLayerCommon: the donor map
is also the physical adjacency, and a second stacking table could disagree
with it. LidLayerSpeciesState::ensureSized replaces resize at both
initialisers so a heat-only deck sizes the block and an age-only deck does
not leave the temperature row at 0 C.

Closes H5a's owed third run-on contributor: a LID drain's temperature is a
per-layer quantity that did not exist until this row.
```

---

# 9. Validation result (checking agent, 2026-08-19) — COMMITTED `1c78e9dd`

Isolated worktree at `c292b8eb`. **154/154** ctest — nothing failed at base
either, so lesson 89 holds and the old FV exemption is correctly retired.
**14/14** decks bit-identical. **112 tests** clean under ASan/UBSan across
nine suites. Zero warnings from any changed file. Falsifier sweep **8 of 9**,
up from 5 of 9 as delivered.

Greps: `st.resize(` empty ✓; `donorsFor|layerVolumes|buildOffsets` one
definition each in `LidLayerCommon` ✓. **`until H5` was NOT empty** — one
stale claim survived at `HeatWatershed.cpp:166`, saying the LID underdrain is
an *uncounted* run-on contributor. This changeset is what makes it false;
corrected, and the grep is empty now.

## 9.1 §6's most dangerous outcome: A4's age values are bit-identical

Not "the suite passes" — the values. Dumped every LID layer age, drain age
and subcatchment runoff age across all six of A4's decks, base vs H5b: the
only difference in the whole output is the header line `species=1` →
`species=2`. `layer_index` is correct under the widened stride.

## 9.2 The changeset retires a gate's premise in another suite

`HeatWatershedTest.EveryRunonContributorKeepsTemperaturesInsideTheSources`
**fails on arrival**, and correctly. I built it last round with the LID
underdrain as the deliberately *uncounted* contributor, asserting
`runon_inflow > subcatch_runon_temp_rate` so the two divisors could be told
apart. H5b supplies the drain's temperature, so they now coincide exactly
(0.9797978893380731 both) and the premise is retired.

Inverted into the stronger claim it was always reaching for: **every cfs of
run-on has a known temperature.** That is a completeness invariant a fourth
contributor cannot quietly break, and it immediately earned its keep —
**falsifier vii, predicted to escape, now fails it.** Everything else on that
deck is unchanged (S1 12.0, S2 16.402).

Worth noting what this costs: with all three contributors counted, dividing
by `runon_inflow` instead of the known rate is now arithmetically identical,
so A3's original defect is **unrepresentable** in the heat track rather than
merely unobserved. The completeness assertion is what keeps it that way.

## 9.3 Gate 4 did not do what its name, and the file header, said

It asserted finiteness and bracketing and then `cap_total > 0`. It never
computed `Σ cap·T`. So an inter-layer flux applied to one side only —
falsifier iii, predicted to fail gate 5 — **passed everything**.

Measured on a column with the storm stopped, the underdrain shut and the
outfall no longer routing back: the correct form drifts **−0.014 %** over
half an hour (residual seepage, which a flow-through element cannot avoid),
while the asymmetric form **manufactures 3.7 %** and climbing —
49.87 → 51.74 MJ/m². Gate 4 now computes the content at two times and
requires it not to move, with a 1 % band that separates the two by more than
two orders. Falsifier iii is closed.

## 9.4 §4.3's trap is real, and gate 2 could not reach it — for two reasons

Falsifier iv (`ensureSized` → `resize`) escaped as delivered. Both of gate
2's legs are single-capability, and `resize` sizes correctly in both; the
wipe only happens when **both** initialisers run against the same block, and
no gate in the suite enabled both (`Opts::water_age` defaults to false).

Added the both-on leg — and it *still* escaped, for a second reason worth
recording: `initLidLayerAge` seeds from the `INITIAL_STATE` source, and
H5b's age config had no such row, so the seed was 0 and **a wipe of a zero
seed writes the value it replaces.** With `INITIAL_STATE GLOBAL 4.0` added
and a SETUP assertion that the seed is non-zero, the leg fails on falsifier
iv. Measured damage on a both-on deck: SOIL age 17842.60 → 17681.34 s,
STORAGE and drain 17917.45 → 17750.90, subcatchment runoff age
16453.76 → 16337.80 — silent, and it would have shipped.

## 9.5 Falsifier ii: observable numerically, but no reference-free invariant

I measured it rather than accepting the prediction. Sequential passes on a
column with conduction and both atmospheric families on, against the coupled
solve:

| t (min) | surface | soil | storage |
|---|---|---|---|
| 15 | 7.535 → 7.731 | 23.244 → 23.632 | 29.910 → 29.921 |
| 30 | 11.006 → 11.238 | 18.327 → 18.647 | 29.623 → 29.644 |
| 60 | 12.141 → 12.511 | 14.902 → 15.184 | 28.818 → 28.855 |
| 120 | 11.992 → 15.630 | 14.570 → 15.634 | 28.394 → 28.446 |

The t = 120 row looks decisive — the sequential surface and soil land within
0.004 °C of each other, the signature of conduction running last and erasing
the atmospheric gradient the coupled solve holds at 2.58 °C. **It is not
usable: at t = 120 the surface layer is dry (capacity 0), so that 15.63 is a
stale HOLD value, not a live answer.** On live layers the gap is 0.2–0.4 °C.

And unlike D-H5e's case, no invariant separates them: conduction's fixed
point (uniform) and the atmospheric operator's fixed point coincide, so both
compositions share a steady state, and both are first-order consistent with
the same ODE, so a `dt`-refinement test converges for both. **Only an
external fine-`dt` reference discriminates.** That is the same instrument
already owed for A4's falsifier vi, H5a's falsifier vi and D-H5e's
linearization caveat — one gate would close four items, and it is the single
highest-value thing left in the heat track.

## 9.6 The rest of the sweep

| # | predicted | outcome |
|---|---|---|
| i | 5 | gate 5 ✓ |
| ii | "probably nothing" | escapes — §9.5 |
| iii | 5 | **escaped**; gate 4 rebuilt as a ledger, now caught |
| iv | 2, both legs | **escaped twice**; now caught — §9.4 |
| v | 1 | gates 1, 4, 5 **and** the watershed contributor gate |
| vi | 3 | gate 3 ✓ |
| vii | "probably nothing" | **caught** by the inverted watershed gate — §9.2 |
| viii | "expected to escape" | **caught** by gate 5, whose OFF/ON comparison sees an always-on operator |
| ix | deliberate non-gate | inert, as intended — **no gate pins 1970/2758, by design.** Stating it explicitly as §5 asks: the parameter choice is recorded in `ConductionConfig`'s provenance comments and in §2 of this handoff, and nothing tests it. Pinning it would gate a choice, not a behaviour |

## 9.7 On §4.1 — backward Euler for the column, exponential elsewhere

I would keep it. The inconsistency is real and you named it honestly, but a
one-layer LID and a node of the same volume already differ in more than the
integrator (different exchange geometry, different advective seams), and the
fixed points agree. A 4×4 matrix exponential to remove a first-order term
that the column solve shares with every other first-order term in the runoff
step is not proportionate. The place it would matter is precisely the
missing reference gate in §9.5.

## 9.8 Still owed

- **The fine-`dt` reference gate** (§9.5) — now blocking four separate items.
- Falsifier ii until that lands.
- A dry-but-present layer is excluded from the conduction system (`live[k]`
  requires water), so a dry soil layer between a wet surface and wet storage
  lets those two conduct **directly across the gap**. Reachable during
  drying; not observed by anything here.
- §7's own list stands: no CSH/RHE parity reference for conduction, and the
  volume-weighted conductivity mixture is the simplest defensible choice
  rather than a validated one.
