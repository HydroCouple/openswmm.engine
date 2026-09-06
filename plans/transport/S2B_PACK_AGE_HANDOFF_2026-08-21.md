# S2b — water age through the snowpack — Handoff (2026-08-21)

**For:** the checking agent — the one that can compile.
**Base:** `2992f7c5` (S4). **Uncommitted, in the working tree.**
**Scope source:** `S2A_MELT_TEMPERATURE_HANDOFF_2026-08-20.md` §7.
**Register:** `SNOW_DIVERGENCE_REGISTER.md` — §2.2 is new and is part of this
changeset.
**Standing findings:** lessons 1–117.

**Nothing was built or run.** The changeset was written in a syntax-only
sandbox on a machine that cannot compile this tree. Treat every claim below as
a prediction, including the ones that sound like measurements — there are
none.

---

## 1. ⛔ READ FIRST — a sequencing question this round did not have the standing
##    to settle

S2b was held behind S3 because **the age model is a complete-mix over the
pack's water, so an age model built on a balance with holes inherits them**.
S3 closed the four it found and S2b was unblocked.

**On 2026-08-21 the new snow parity deck found a fresh gap in the same
balance** — `SNOW_CONTINUITY_FINDING_2026-08-21.md`. Every subcatchment with a
pack loses water; the control subcatchment without one balances to the digit;
~3.4 inches over 20 acres is unexplained *after* the two missing ledger rows
are accounted for.

**The identical argument applies again, and it is a user decision.** The code
below is written and reviewable either way. If the balance moves, **the gates
survive** — every one is a bracket or a conservation statement and none pins a
value — but the *numbers* a later round quotes from this one would not.

Recommended: close the reporting half of the continuity finding first (add
`Initial Snow Cover` / `Snow Removed` / `Final Snow Cover` to the engine's
ledger, matching legacy `report.c:521/561`), re-measure, and then decide.

## 2. What changed

```
mod:  src/engine/hydrology/Snow.hpp              (SnowSoA: age, out_age,
                                                  precip_age, track_age)
mod:  src/engine/hydrology/Snow.cpp              (mixAge; resize; execute
                                                  steps 0/6/7; plowSnow)
mod:  src/engine/data/SubcatchData.hpp           (snow_melt_age_imperv/perv
                                                  at all SIX enumeration sites)
mod:  src/engine/core/SWMMEngine.cpp             (hand in precip_age +
                                                  track_age; publish the
                                                  melt-age blend)
mod:  src/engine/transport/components/WatershedCommon.hpp/.cpp
                                                 (arrivingPrecipAge)
mod:  src/engine/transport/components/WaterAgeModule/WaterAgeWatershed.cpp
                                                 (use it)
mod:  tests/unit/engine/test_transport_snow.cpp  (+5 gates, 18 -> 23; and
                                                  ONE existing gate's DECK
                                                  changed — see §5(a))
mod:  plans/transport/SNOW_DIVERGENCE_REGISTER.md (§2.2, new)
```

**`CMakeLists.txt` NOT touched.** No new files, so **⛔ HARD STOP, lesson 79 —
no CMakeLists entry is expected.** If you find yourself adding one, something
is wrong with this changeset and not with the build.

## 3. Design, and why each call sits where it does

### 3.1 The age lives in the snow solver, not in the transport layer

`plowSnow` moves water between surfaces **and to another subcatchment** inside
`snow_.execute`'s sibling call. An age update running afterwards sees only that
subcatchment *m* gained snow; it cannot know it came from *j*, nor at what age.
Following A4: **the values exist as locals, so the mixing happens where the
water moves.**

**This means S2b touches hydrology**, exactly as A4 did, and saying so here is
the point of lesson 70 — a scoping claim is a prediction, and the way it goes
wrong is by being quietly exceeded. It touches hydrology to *carry* a number,
not to change one: no water balance expression is altered.

### 3.2 `out_age` is separate from `age`, and that is not redundancy

In a complete-mix pool the water leaving carries the pool's age. **Except when
the pool empties** — then `age` is 0 with no water to describe, and the
meltwater still carries what the pack had. `out_age` is the age of the water
published as `imelt` this step; `age` is the age of what remains. Gate 22 is
the gate for exactly this and it is the one most likely to catch a rewrite that
"simplifies" the two into one.

### 3.3 Age, then mix, and both in `plowSnow`

`plowSnow` is the step's first snow call (`SWMMEngine.cpp:1596`, immediately
before `execute`, matching legacy `runoff.c:254`). Ageing there means new snow
mixes in at its source age and is aged from the *next* step — the A3/A4
convention that arriving water is not older than the instant it arrived.

**Consequence to check:** if any caller ever invokes `execute` without
`plowSnow`, the pack stops ageing and nothing says so. The two calls are
adjacent today. **Worth a grep**: `grep -n "snow_\.\(execute\|plowSnow\)"
src/engine/` should return exactly one adjacent pair.

### 3.4 The melt-age blend is weighted by melt VOLUME, not by area

`snow_melt_imperv` blends plowable and non-plowable melt by area. The **age**
blend uses `imelt·fArea` as the weight instead, because an age is an intensive
property of the water and the rate is not: two surfaces contributing different
rates do not contribute their ages equally. Using the area weights would have
been the same expression as the rate blend and would still have been wrong.

### 3.5 `arrivingPrecipAge` calls `arrivingMeltFraction` — the same call

Not an equivalent expression. If the age and temperature tracks computed the
fraction separately they could drift, and **the drift would be invisible**:
both answers stay inside their own brackets, and only a deck comparing arriving
age against arriving temperature could see it. One call makes it
unrepresentable.

### 3.6 The `-1.0` sentinel falls back to the RAIN age, never to 0

**0 is a real age** — the age of water that fell this instant — so using it as
a "missing" marker would make an unpublished pack look like the freshest water
in the model. Same shape as H1's dry-element column, which is still open.

## 4. ⚠ Things I could not check and you can

These are the reasons this handoff exists. Each is a specific, cheap check.

1. **`openswmm::SimulationContext ctx;` default-construction** — gates 19, 21
   and 22 drive `SnowSolver` directly and need a context object that `plowSnow`
   only dereferences inside the `sfrac[0] > 0` branch, which none of them
   takes. **If `SimulationContext` is not default-constructible, or is
   prohibitively heavy to stack-allocate**, the fallback is to size
   `ctx.subcatches` to the gate's subcatchment count and set `area` — not to
   delete the gates.
2. **`WaterAgeSource` visibility in `SWMMEngine.cpp`.** The new block at
   ~line 1598 names it. If it does not resolve, add the `WaterAgeData.hpp`
   include rather than reaching for the raw index.
3. **`snow_.state()` non-const in that block.** `auto& snow_soa =
   snow_.state();` sits a few lines above `const auto& soa = snow_.state();`.
   Both overloads exist; confirm no shadowing warning.
4. **The SoA enumeration count (lesson 109).**
   `grep -c "snow_melt_age_perv" src/engine/data/SubcatchData.hpp` should be
   **7** — one declaration plus six enumeration sites — matching
   `snow_melt_perv`'s own count net of its doc mentions. **I ran this check on
   the file and it passed; run it again after any merge**, because it is the
   check S2a's segfault existed to justify.
5. **`std::fabs` / `<cmath>`** — already included at the top of the test file.

## 5. Anticipated failure modes, in likelihood order

**(a) — HIGHEST, AND IT IS A DELIBERATE EDIT, NOT A BUG.**
`MeltwaterMixesIntoTheSubareaAge` (gate 2) **had its deck changed**, and if you
revert that change it will fail on correct behaviour.

Its original deck ran with `INITIAL_STATE` at 0, so the surface and the pack
both started at age 0 and the only discriminator was "did anything arrive at
all". **Under S2b meltwater no longer arrives at age 0** — it carries the
pack's residence time, which on that deck is the elapsed run time, so mixing it
into a surface also at the elapsed run time moves nothing. Its premise was
retired by the fix, exactly as S3 retired
`APackAbsorbingRainPublishesAGenuineZero`. The surface now starts 10 hours old
and the pack starts young, so arriving meltwater still has to pull it down.

**If any OTHER existing gate moves, that is a finding, not a tolerance to
widen.** S2b adds state; it does not change the water balance.

**(b)** Gate 20 fails its `f < 1.0` SETUP — cover is still 1. After S4 partial
cover needs `SD100` **and** a graded `ADC` row; the deck sets both
(`sd100 = 24.0` against `sd0 = 6.0`, and `adc_cover = 0.5`). If it fires, one
of the two is not reaching the solver, and that is an S4 regression rather than
an S2b defect.

**(c)** Gate 21's `ASSERT_GT(st.wsnow[r_perv], 1.0)` fails — the plow never
fired. Check `weplow`, `sfrac[4]` and `to_subcatch` in that order.

**(d)** Gate 21's age-volume conservation fails by exactly `dt × total
volume`. That means the baseline was computed without the step's ageing.
**The gate already accounts for it** — if you find yourself widening the band,
re-read the baseline expression first.

**(e)** The **snow parity deck** (`tests/parity/snow/`) moves.
**It should not.** S2b publishes a value that already existed and changes no
balance expression. **A movement in `snow_parity.out` means the plow
publication changed the hydrology**, which is the one thing §3.1 must not do —
and it is the single most valuable check in this round, because it is the only
one the unit gates cannot make.

**(f)** The 14 reference decks move. They have no `[SNOWPACKS]` section at all
(confirmed in S3 and S4), so this would mean the SoA additions perturbed
something unrelated.

## 6. Validation protocol

1. **Isolated worktree at `2992f7c5`** (lesson 71 — a count from the main tree
   is not attributable).
2. **⛔ HARD STOP — lesson 79.** No `CMakeLists.txt` entry expected.
3. **⚠ LESSON 109 — the SoA enumeration check.** §4(4). **This changeset DOES
   add SoA fields**, so unlike S3 the check is not vacuous here. Run it before
   compiling.
4. Build, **zero new warnings** under `-Wall -Wextra`. Snow suite, then the
   full suite (158 at base, expect 158 + 5 = 163 registered gates in
   `test_engine_transport_snow`).
5. **The parity deck** — §5(e). `cmp` against
   `tests/parity/snow/baseline/snow_parity.out`.
6. **14/14 reference decks**, ASan/UBSan.
7. **Falsifier sweep** (§7).
8. **Record:** every gate that moved with both values and the decision taken;
   falsifiers iv and v specifically; whether the parity deck moved; and the
   answer to §4(1), because the fallback there changes three gates.

## 7. Falsifier sweep

| # | falsifier | expected failing gates |
|---|---|---|
| i | Never mix snowfall into the pack (drop the `mixAge` in `plowSnow`) | 19, 22 |
| ii | Publish `age` instead of `out_age` in the melt blend | **22** — and 18 on a deck whose pack melts out |
| iii | Blend the melt age by area instead of by melt volume (§3.4) | **probably nothing** — flagged. It needs two surfaces with different ages *and* different rates at once, which no gate here arranges. **Owed if it escapes**; closing it needs a deck with `snn0 > 0` and a per-surface age contrast |
| iv | Reconstruct the plow transfer after `plowSnow` instead of publishing it | **21** — and **if it escapes, gate 21 is decorative**, which is the one outcome that would mean this round shipped an unobserved change |
| v | Compute the melt fraction independently in `arrivingPrecipAge` rather than calling `arrivingMeltFraction` | **probably nothing** — flagged, and deliberately so: §3.5 says the drift is invisible to every bracket. It is a structural guarantee, not a tested one, and it should be recorded as such rather than left looking verified |
| vi | Drop the `track_age` guard (always on) | **nothing, and that is correct** — the arrays are pure bookkeeping. It is a cost check, not a correctness one |
| vii | All of i–iv at once | 18–22 |

**iii and v are predicted to escape.** Both are recorded rather than papered
over, because an unobserved invariant is how a later change acquires a wrong
premise — S4's falsifier iii is the precedent.

## 8. Known gaps

- **A2 in the register §2.2 — the pack's initial water starts at age 0, not at
  `INITIAL_STATE`.** ⬜ Owed a decision, and **tied to the hotstart question**:
  pack SWE *is* persisted (`SWMMEngine.cpp:5646`), so unlike A2a's case the
  volume would be restored, which makes persisting the age possible and
  therefore a decision rather than an impossibility. **Settle both together or
  neither** — an age restored over a volume that was not, or a volume restored
  under an age that was not, are the same fiction with the sign flipped.
- **`WATER_AGE_SNOW`** — A3 carries this as untouched *and* undeferred. S2b is
  where it stops being deferrable. Decide whether it is this question or a
  different one before closing the round.
- **The continuity finding** (§1). If option (b) was taken, this round's
  numbers sit on an open balance and the results section must say so in its
  first paragraph.
- **Falsifiers iii and v predicted to escape** (§7).

## 9. Prepared commit message

```
feat(transport): water age through the snowpack (S2b)

S2a fixed the TEMPERATURE of water arriving under a pack; its age was still
the configured RAINFALL value, as though meltwater had fallen from the sky
the instant it appeared. A pack is a store, and the whole point of an age
model is to measure how long water sits in one.

Each snow surface now carries a water age, complete-mixed over wsnow and fw
together. Snowfall mixes in at the RAINFALL source age, melt leaves at the
pack's age without changing it, and arrivingPrecipAge blends the two on the
SAME arrivingMeltFraction the temperature half uses -- not an equivalent
expression, the same call, because a drift between them would stay inside
both brackets and no gate could see it.

The age lives in the snow solver because plowing moves water between surfaces
and to another subcatchment inside plowSnow. An age update running afterwards
sees only that a subcatchment gained snow; it cannot know where it came from
or at what age. Following A4, the values exist as locals, so the mixing
happens where the water moves. That means this touches hydrology -- to carry
a number, not to change one.

out_age is separate from age on purpose: in a complete-mix pool the water
leaving carries the pool's age, except when the pool empties, and then age is
0 with no water to describe while the meltwater still carries what the pack
had.

One age per surface rather than two is a deliberate approximation and is
recorded as one in SNOW_DIVERGENCE_REGISTER.md, along with the open question
of whether the pack's initial water should take the INITIAL_STATE age.
```

---

## 10. Validation results (2026-08-21) — COMMITTED `2a58d82c`

**§1's caveat stands: these numbers sit on the open continuity finding.** The
gates do not — every one is a bracket or a conservation statement, as §1
promised — but any figure quoted from this round is provisional until the
snow water balance is closed.

**158/158 ctest** (after a flake, §10.6), **14/14 reference decks
byte-identical**, **the snow parity deck byte-identical to the same deck on
the base**, **76 tests ASan-clean**, zero new warnings, **23 gates registered**
exactly as §6.4 predicted. Sweep: **5 of 9 observed**, of which one is a
falsifier this round added for its own fix.
Numbers: `tests/output/s2b_validation_2026-08-21/`.

### 10.1 ⛔ THE BRANCH DOES NOT BUILD, AND IT IS NOT THIS CHANGESET

`2a5b964f` — an FV commit — added **48 lines to `SWMMEngine.cpp`, 11 of them
mentioning `infil`**. They are Track I's 2D-infiltration call sites, swept in;
the supporting files (`SurfaceRouter2D::infil`, `SimulationSnapshot::
surface_infil_rate`, `src/engine/2d/infil/`) are still uncommitted. Verified
by building both: **`2992f7c5` builds clean, `2a5b964f` fails with 9 errors**,
all of that shape.

S2b was therefore validated at its stated base `2992f7c5` (lesson 71 — a
count from an unbuildable tree is not attributable) and committed on the
current HEAD, whose `SWMMEngine.cpp` was **merged** rather than overwritten:
the numstat reads `33 0` for that file, so the 48 lines are intact.

The commit message of `2a5b964f` says splitting its parts "would mean
committing an intermediate that was never built". It is one.

### 10.2 ⛔ THE INDEX IS STALE AND WOULD REVERT FOUR COMMITS

`.git/index.lock` has been held since 05:58 with no live git process, so the
index could not be refreshed. It now predates `2a5b964f`, `7fb60748`,
`857b88ba` **and** `2a58d82c`: `git diff --cached --numstat` shows ~2,900
deletions across 33 files. **A commit made from this index reverts all four.**

The working tree is correct — all eight changeset files hash-match HEAD. The
lock was not touched (standing constraint). Once it clears:
`git reset -q HEAD` before anything else commits.

### 10.3 Two units errors of the same shape, in the same gate pair

`[WATER_AGE_SOURCES]` is parsed in **hours**; every age the engine publishes
is in **seconds**.

- Gate 2's changed deck (§5(a)) and gate 20 both wrote `10.0 * 3600.0` into
  the deck, making the source **36 000 hours** — `precip_age` measured at
  **1.296e8 s**, which dragged the pack age to 1.96e6 s on a one-hour deck and
  fired gate 20's SETUP.
- With the deck fixed, gate 20 then compared `a_pack` (seconds) against
  `o.rain_h` (hours) and failed again at 4116.59 against 10.

Both fixed; the comparison now converts explicitly. Same family as the
`dt`-reference round's `sourceSpread`.

### 10.4 §5(d) was right that the baseline is the thing to re-read — and the
###      baseline was right

Gate 21 failed by exactly `dt × moved`, 60 against 40180, precisely as §5(d)
describes. But the baseline expression was correct: **the code was wrong.**

`plowSnow` ran ageing and plowing in **one pass over subcatchments**, so water
plowed from *j* into *m > j* was aged **twice** — once in the donor, once when
the loop reached the receiver holding it — and water plowed into *m < j* was
not aged at all. The answer depended on subcatchment **order**. Measured
directly: donor 20000→20060, 1.0 ft moves, receiver mixes to 10030, then ages
again to **10090**.

Ageing is now a separate pass completed before any plowing. Age-volume is
conserved to the digit (40180), and falsifier 0 — re-interleaving them — is
caught by gate 21.

### 10.5 Falsifier ii is unobservable from a deck, and gate 22 is not decorative

§7 ii aimed at gate 22 and **escaped**. The reason is structural: `age` and
`out_age` diverge at exactly **one** site — the instant-melt branch, which
zeroes `age`. On ordinary melt-out `age` is never zeroed, so `age == out_age`
and the publication cannot tell them apart. Confirmed by measurement on a
melt-out deck: correct and defective both publish **900 s**.

And the instant-melt branch needs a sub-threshold pack, which melts on step 1
— when a deck-built pack's age is still **0**. So the divergence point and the
only moment a deck can reach it coincide with the value being zero.

**This is the concrete argument for settling §8's A2**: if the pack's initial
water took the `INITIAL_STATE` age, falsifier ii would become observable.

Gate 22 itself is sound. It gates the distinction **at the source**, and a new
falsifier **ii-b** — collapsing the two at the one site where they diverge —
fails it. §3.2's claim holds; the falsifier was aimed at the wrong layer.

### 10.6 The rest

- **§5(e), the most valuable check: PASSES, on the right comparison.** The
  stored `baseline/snow_parity.out` does **not** reproduce at `2992f7c5`
  either — it is stale. Base-vs-S2b is **byte-identical**, which is the
  control that answers the question, and it independently clears the
  `plowSnow` restructure of moving any water. **The parity baseline needs
  regenerating at a known commit or it will cry wolf every round.**
- **§4(1): `SimulationContext` is default-constructible** — gates 19, 21 and
  22 compile and run as written; no fallback needed.
- **§4(4), lesson 109: `grep -c snow_melt_age_perv` = 7**, as predicted.
- **§3.3's grep returns exactly one adjacent pair.**
- **§5(a) held**: `test_snow.cpp` is 35/35 and no other existing gate moved.
- **ctest flake, pre-existing**: `test_engine_output_node_stats` failed once
  under `-j 8` on a missing `site_drainage_model.out`, and passes alone and on
  re-run. A fixture race between tests sharing `tests/unit/engine/data/`.
- Falsifiers **iii**, **v** escaped as predicted; **vi** is inert, which §7
  says is correct.

### 10.8 Addendum — §10.3's sweep stopped at the gate that failed

**§10.3 says "both fixed". The deck values were. One of the two bounds derived
from them was not.**

Gate 2's `init_h` was corrected to hours; the line that consumes it was left
alone:

```cpp
const double unmixed_s = o.init_h + elapsed_s;   // 10 + 3600 = 3610 s
```

It means **39 600 s**. At 3610 s the ceiling sits *below* the arriving water's
own age, so `EXPECT_LE(age, unmixed_s)` and the `any_mixed` test beneath it
were both decided by where the mixing happened to land rather than by anything
the gate asserts. **It passed** in the 158/158 run — which is why it was not
reported, and why it is the more dangerous of the two.

Fixed: the conversion is explicit and the reason is stated at the line. The
unit is now documented on `Opts::rain_h` and `Opts::init_h` themselves, since
carrying it in a suffix is what allowed the same slip in two directions at once
— one gate wrote seconds into an hours field, the other compared an hours field
against seconds.

**(118)** *a unit that lives only in an identifier is not carried by the
compiler, so finding one instance of the slip obliges a sweep of every use —
and the instances that still PASS are the dangerous ones, because they pass
for a reason unrelated to what they test.*

Sweep run: every `o.rain_h` / `o.init_h` in the file is now either `0.0`, where
the unit does not show, or explicitly multiplied at the boundary.

**Three gate edits — 21's fix, 20's units, and this one — have not been
compiled together.** The suite needs a re-run before §10's counts stand.

### 10.9 ⚠ SUPERSEDED — my baseline diagnosis was wrong, and the right answer
###      is an open defect

I wrote that the stale baseline "came from the openswmm MCP server's engine
process, not a build of the tree" — a different **binary**. That is wrong
twice over: three CLI builds reproduce each other and disagree with the stored
file, and one of them is `install/Darwin/bin/openswmm`, which is not a
different binary from the tree at all.

The variable is not the binary, it is the **execution path**. That run was
this tree's engine driven through the MCP **session API**
(`lifecycle_open_model` + `lifecycle_run_simulation`) instead of the CLI, at a
commit one of the three agreeing builds also used. Under the API the packs
barely melted — 7.25 in reached the ground against 12.98 in, from the same
12.000 in of precipitation.

**Hypothesis: the daily `setMeltCoeffs` hook does not fire on the API stepping
path.** F1's signature one layer up — the same function having no caller on
one of two paths rather than none at all. Tracked as **O4** in the register
with the two-run check that settles it.

The rule the superseded note reached for is still the right one and now has a
better reason: **a bit-identity baseline must be generated by the same binary
*on the same path* the sweep uses**, at a named commit, in the same build
directory. The regenerated baseline records all of it.

### 10.7 Still owed

- §8's **A2 / hotstart** decision, now with a second reason: it is what makes
  the melt-age publication testable.
- **`WATER_AGE_SNOW`** — still untouched and undeferred; S2b did not settle it.
- The **continuity finding**, and with it every number above.
- Falsifiers iii and v remain structural guarantees rather than tested ones.
- **A suite re-run.** Three gate edits have landed since the 158/158 count and
  have not been compiled together (§10.8).
- **Regenerate `tests/parity/snow/baseline/snow_parity.out`** from a build of
  the tree, at a named commit, in the build directory the sweep uses (§10.9).
  Until then the stored file is provisional and base-vs-head is the only valid
  comparison.

---

## 10.10 The re-run round (2026-08-21) — GATE 2 WAS VACUOUS, AND THE FIX IN
##       §10.8 IS WHAT MADE IT MEASURABLE

§10.8 closed with "three gate edits have not been compiled together". They are
now, and compiling them was not the point — **measuring gate 2 was.**

**Full build clean** (327/327, 0 warnings from the changed file).
**ctest 159/160**, **falsifier sweep 6 of 10 observed**, **76 tests
ASan/UBSan clean**, **snow parity deck reproduced by three independent
builds**. Numbers: `tests/output/s2b_recompile_2026-08-21/`.

### 10.10.1 ⛔ Gate 2 passed with its own defect restored — measured, twice

§10.8's fix was arithmetically right and left the gate observing nothing. The
print says it plainly:

```
[ gate2 ] subarea 0 age 3600.000000  (ceiling 39600.0)   <- correct code
[ gate2 ] subarea 0 age 3600.000000  (ceiling 39600.0)   <- S1 defect restored
```

Every subarea read **exactly the elapsed run time** either way, and the gate
passed both times. The S1 defect is `arrivingPrecipRate` returning the gage —
the very thing gate 2 is named for.

**Two reasons, and the second is the one worth keeping:**

1. `INITIAL_STATE` does not seed `subarea_age`. It seeds the network
   (`ArdEngine.cpp:189`), the legacy mirror and the LID layers;
   `WaterAgeWatershed.cpp` has no seeding at all. So "the surface starts old"
   — the premise §5(a) gave the gate when S2b retired its original one — was
   never true, and the ceiling sat 11x above the value it bounded.
2. **On a single-phase deck the age channel CANNOT see a mixing-volume
   defect.** The surface age and the pack age are both pure elapsed time when
   each starts at zero, so mixing one into the other moves nothing. Correct
   code and the defect are arithmetically identical. No tolerance, deck
   parameter or assertion rewrite fixes that; only a deck where the two
   waters have different histories does.

**The fix is a two-phase deck**: one hour of rain configured at 10 h old over
a half-covered pack, then one dry hour in which the only arriving water is
melt. The wet phase gives the surface an age of its own; the dry phase is the
gate. The value the age must move off is **measured at the changeover**
(`run()` now optionally captures `subarea_age` and the clock) rather than
assumed, because it is not computable in advance.

**And the first version of that deck escaped too**, for a reason worth
recording: pure accrual does not land on the bound. The age advances one WET
step (60 s) at a time while the capture uses the routing clock, so accrual
alone reads ~59.5 s under it and a 1-second band passes the defect. Measured:
accrual moves **3540.0 s** in a 3599.5 s window; correct mixing moves
**12 638.8 s**. The bar is now half the window — two orders of magnitude clear
of the quantisation, seven times clear of the signal.

Under the S1 defect the rewritten gate now fails, along with five others.

**(119)** *a gate retired by a fix and re-aimed at a new premise is a NEW
gate, and it owes the same falsifier the original one owed. §5(a) re-aimed
gate 2 and never re-falsified it; §10.8 corrected its arithmetic and never
re-falsified it either. Both edits were right and neither was a test.*

### 10.10.2 ⛔ The snow continuity finding is not this engine's behaviour

**§10.6 and §10.9 both misdiagnosed the parity baseline, and the real answer
overturns a finding that this whole program has been sitting on.**

`baseline/snow_parity.out` (`ed4d0b63…`) is reproduced by **no build**. Three
independent ones agree with each other and disagree with it, all producing
`d9eb7f94…`:

| build | commit | result |
|---|---|---|
| `openswmm.engine.s2bwt`, `build/s2b` | `2992f7c5` | `d9eb7f94…` |
| `build/darwin-tests-release` | `310e8ffb` | `d9eb7f94…` |
| `install/Darwin/bin/openswmm` | (installed) | `d9eb7f94…` |

So §10.9's "it came from the MCP server's process, a different binary" is
wrong twice over: the installed binary the MCP server uses reproduces **this
build**, and a worktree at a different commit in a different build directory
does too. The retired artefact's analysis-options header is byte-identical to
theirs — same deck, an engine that is not in this tree.

**What the deck actually says**, with the surviving pack read from the `.out`
rather than inferred from the gap it is meant to explain:

| subcatchment | precip | init snow | infil | runoff | final snow | unaccounted |
|---|---|---|---|---|---|---|
| `SUB_DEEP` | 12.00 | 4.00 | 6.27 | 8.62 | 0.3568 | **0.753** |
| `SUB_ADC` | 12.00 | 2.00 | 7.48 | 5.54 | 0.9356 | **0.044** |
| `SUB_THIN` | 12.00 | 0.0005 | 6.00 | 6.00 | 0.0000 | **0.0005** |
| `SUB_BARE` | 12.00 | — | 6.00 | 6.00 | — | **0.000** |

Continuity error **−8.193 %**, not +39.543 %. Supplying the three missing
ledger rows takes it to **+1.419 %**, not "roughly 27 %", and **the 3.4-inch
hole does not exist**. What remains names itself: `SUB_DEEP` is the only
subcatchment with an out-of-watershed removal fraction (`Fout = 0.20`) and
**Snow Removed** is the missing row that carries it; `SUB_ADC`'s 0.044 in is
the free water its surviving pack holds and `newSnowDepth` does not count.

**This inverts the finding's own ranking.** §3 of
`SNOW_CONTINUITY_FINDING_2026-08-21.md` called the plow/removal path "the
weakest candidate" and said "the table already argues against it". The table
did — and the table does not reproduce. On the real numbers it is the leading
candidate.

**The tell was in the retired table all along:** `SUB_THIN`, whose pack is
0.0005 in, was shown losing **4.81 in** — the same as a pack 4,000 times
deeper. A loss attributed to the snow path that does not scale with the snow
was never physical.

§1 of the finding — the ledger has no snow terms — **is untouched and still
certain**: `grep -rn "Snow Cover" src/engine/` still returns nothing.

The baseline has been regenerated with its provenance recorded
(`baseline/SHA256SUMS`: commit, build directory, and the sha256 of both the
binary and the engine dylib), and the retired sums are kept there because
their numbers are quoted in three documents.

### 10.10.3 The rest of the sweep

- **ctest 159/160.** The failure is `test_engine_2d_infil_integration`,
  residual 0.0030977 — the 0.31 % end-of-step re-derivation recorded for
  Track I. Read from the assertion, not assumed from the name. It belongs to
  `310e8ffb`, which landed mid-round, so **HEAD is red on a test that is not
  this changeset's** and that program owes it.
- **Falsifier sweep, 6 of 10 observed**, all restorations sha256-verified and
  the pristine rebuild clean. New this round: **`s1`**, the defect gate 2 is
  named for — observed by six gates. `0`, `i`, `ii-b`, `iv`, `vii` behave
  exactly as in §10's sweep, so the shared scaffolding change (`series`,
  `run`) cost no gate its teeth. `ii`, `iii`, `v` escape and `vi` is inert,
  all as §7 and §10.5 predicted.
- **14 decks: 4 byte-identical, 10 moved.** Every mover is an FV/VJ/slot deck
  and every stayer is dynamic-wave or legacy, which partitions the movement
  onto `2a5b964f` ("node stages above their own conduits") and the FV work
  above it. This round's changeset touches **no engine source at all** — one
  test file — so it cannot be the cause; the partition is what says so rather
  than the diff alone.
- **§10.1 is resolved by someone else.** `310e8ffb` landed Track I's
  infiltration closure, so HEAD compiles again. `aa1c4f0a` did not:
  `SWMMEngine.cpp:129` called `surface_router_.infil()` against a header with
  no such member.
- **§10.2 cleared.** The index lock released; the four commits are intact and
  HEAD is `310e8ffb`.

### 10.10.4 Still owed

- §8's **A2 / hotstart** decision — unchanged, and §10.5's argument for it
  stands.
- **`WATER_AGE_SNOW`** — still untouched and undeferred.
- **The three ledger rows** (`Initial Snow Cover`, `Snow Removed`,
  `Final Snow Cover`). Now a reporting fix with a predicted outcome rather
  than a diagnosis.
- **`SNOW_DIVERGENCE_REGISTER.md`** still owes an **F7** entry and the
  falsifier-iv redundancy note.
- Falsifiers `iii` and `v` remain structural guarantees rather than tested
  ones.
