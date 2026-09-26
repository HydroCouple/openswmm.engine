# S3 — the snowpack water balance — Handoff (2026-08-20)

**For:** the checking agent.
**Base:** `8b7d1cf7` (S2a).
**Finding:** `SNOW_WATER_BALANCE_FINDING_2026-08-20.md` — **read first.**
**Register:** `SNOW_DIVERGENCE_REGISTER.md` — **new, and a deliverable of
this round.**
**Standing findings:** lessons 1–111.

**⚠ THIS IS A HYDROLOGY CHANGE. It moves runoff volume and timing on every
deck with `[SNOWPACKS]`.** That is intended, decided by the user
(2026-08-20), and §5.5 is where it has to be seen rather than assumed.

---

## 1. What this fixes — three divergences from legacy `routeSnowmelt`

Legacy is compact and its **order is the whole point**:

```c
wsnow[i] -= vmelt;                                 // SWE −= MELT, first
fw[i]    += vmelt + rainfall * tStep * asc;        // melt AND rain-on-snow
vmelt     = fw[i] - fwfrac[i] * wsnow[i];          // cap on POST-melt SWE
fw[i]    -= MAX(vmelt, 0.0);
```

The engine split this across two loops and got three things wrong:

- **F2 — SWE was reduced by the DRAINED EXCESS, not the melt.** Step 6
  overwrote `imelt` with the excess before step 7 read it, so melted snow
  retained as free water was counted **twice**. A pack melting slower than
  its capacity never depleted. *This is mass creation and I rank it first.*
- **F3 — rain on the covered fraction was discarded.** Excluded from the
  ground by `rain·(1 − asc)` and never added to the pack: it left the model.
- **F4 — capacity taken from pre-melt SWE.**

Plus **F5**, the S2a round's find: the instant-melt branch wrote `imelt`,
and steps 4–5 assigned over it, discarding the water of any pack under
0.001 in.

**All four were unreachable until `274b6506`** gave `setMeltCoeffs` its
caller (F1). With `dhm` at zero there was no degree-day melt to mis-account.
They became live one commit before they were found.

## 2. Changeset (uncommitted)

```
mod:  src/engine/hydrology/Snow.cpp   (steps 0, 6, 7 — routeSnowmelt order)
mod:  tests/unit/engine/test_transport_snow.cpp   (+4 gates, 11 → 15)
new:  plans/transport/SNOW_DIVERGENCE_REGISTER.md
new:  plans/transport/SNOW_WATER_BALANCE_FINDING_2026-08-20.md
```

`CMakeLists.txt` **not touched.** Syntax-clean under `-Wall -Wextra`.
Nothing built or run.

## 3. Design notes

### 3.1 Steps 6 and 7 merged into one loop, deliberately

The two-loop split is *what allowed* F2: `imelt` meant "melt" in one loop and
"drained excess" in the next, and nothing named the change. One loop, in
legacy's order, makes that reuse impossible to write.

### 3.2 The instant melt is held in a local, not in `imelt`

`std::vector<double> instant_melt`, added back in step 7 where nothing
reassigns it. Writing it into `imelt` at step 0 is precisely what failed.
**A heap allocation per call** — flag if that matters on large models; a
member scratch buffer is the alternative and I judged clarity worth it here
until someone measures otherwise.

### 3.3 `fw = min(fw, wsnow)` dropped from step 7

It was compensating for F2. With the capacity applied to post-melt SWE, `fw`
is bounded by `fwfrac·wsnow` already. **Check this** — if `fwfrac > 1` is
representable in a deck, the clamp was load-bearing and I have removed a
guard that mattered.

## 4. The gates are all CONSERVATION statements

None takes a reference value. That matters more than usual here: the correct
magnitudes depend on melt coefficients, cover and timestep, so a gate pinned
to one deck's numbers would rot the moment any changed.

- **12** — a slowly melting pack loses SWE. *The F2 gate*; `fwfrac = 0.90`
  puts the deck squarely in the regime where the defect lived.
- **13** — rain on a fully-covered pack reaches the free-water store.
  *The F3 gate*, with an `asc == 1` SETUP so the water cannot have arrived
  by any other route.
- **14** — an instantly-melted thin pack produces runoff. *The F5 gate.*
- **15** — the pack never holds more water than it was given, on a deck where
  **nothing falls at all**. The blunt ceiling; no change to melt physics can
  invalidate it.

## 5. Validation protocol

1. **Isolated worktree at `8b7d1cf7`.** Lesson 71. Suite was **158/158**.
2. **⛔ HARD STOP — lesson 79.** No `CMakeLists.txt` entry expected.
3. **⚠ LESSON 109 — THE SoA ENUMERATION CHECK.** S2a segfaulted because
   `SubcatchData` lists its arrays by hand in **six** functions and a field
   was added to one. **This changeset adds no SoA field**, so the check is
   vacuous here — but run it anyway and confirm that, because "S3 adds no
   field" is a claim of mine and the cost of it being wrong is a null deref.
4. Build, zero new warnings. Snow suite, then the full suite.

   **Anticipated failure modes. My record is 5 of 31 — trust the sweep.**

   (a) **Existing `test_snow.cpp` gates will move, and some SHOULD fail.**
   35 gates were written against the pre-S3 balance; any that pinned SWE,
   `fw` or melt output now describe the defective behaviour. **Each failure
   needs deciding individually: is the gate asserting legacy's answer (fix
   the code) or the engine's old one (fix the gate)?** Report each with both
   values — this is the largest single risk in the changeset and it is not
   automatable.

   (b) **Gate 13's `asc == 1` SETUP may not hold** if a deck picks up an
   explicit ADC row from `Opts::adc_cover`. It uses the default (1.0), so it
   should — but O1 in the register says cover behaviour is not fully
   understood.

   (c) **Gate 14's sub-threshold pack may not build at all.** `sd0 = 0.0005`
   in deck units; if the parser or the unit conversion floors it, the pack is
   absent rather than instantly melted, and the SETUP will say so.

   (d) **The 14/14 deck corpus.** §4 of the register: no reference deck is
   *known* to have `[SNOWPACKS]`, but that has never been confirmed.
   **Confirm it before reporting 14/14** — if one has snow, S3 moves it, and
   that movement is correct and must be shown rather than explained away.

5. **Falsifier sweep:**

   | falsifier | expected failing gates |
   |---|---|
   | i. reduce SWE by the excess again (restore F2) | **12**, and probably 15 |
   | ii. drop the `rain_on_snow` term (restore F3) | **13** |
   | iii. capacity from pre-melt SWE (restore F4) | **probably nothing** — flagged. It shifts a threshold rather than losing water, and no gate here isolates it. **Owed if it escapes**; closing it needs a deck poised exactly at capacity |
   | iv. write the instant melt into `imelt` at step 0 again (restore F5) | **14** |
   | v. all four at once | 12–15 |

6. **Prior suites:** full C++ suite, 14/14 decks, ASan/UBSan. **S1's and
   S2a's gates must still pass** — they run on packs, so if S3 moved them the
   interaction needs explaining.

7. **Record:** every `test_snow.cpp` gate that moved, with both values and
   the decision taken; falsifier iii; whether any reference deck has snow;
   and **the measured effect on pack persistence** — how long a pack survives
   before and after, which is F2's whole signature.

## 6. Known gaps

- **Falsifier iii predicted to escape.**
- **O1 and O2 in the register are open**, not fixed here: `SD100` unread, and
  the plowable surface never areally depleted. O2 is load-bearing for gate 11
  and **unverified against legacy** — worth checking while you are in
  `snow.c`.
- No gate asserts the **full per-step balance**
  (`Δ(wsnow + fw) = snowfall + rain·asc − imelt`) directly. Gates 12–15 test
  its consequences. A direct ledger would need per-step instrumentation the
  engine does not expose, and D-H5e's round showed that is worth building
  only when the consequences stop being enough.

## 7. Prepared commit message

```
fix(hydrology): the snowpack water balance loses and creates water (S3)

Three divergences from legacy routeSnowmelt, all unreachable until 274b6506
gave setMeltCoeffs its caller:

SWE was reduced by the DRAINED EXCESS rather than by the melt, because
step 6 overwrote imelt with the excess before step 7 read it. Snow that
melted but stayed within the free-water capacity was counted twice -- as
snow and as free water -- so a pack melting slower than its capacity never
depleted at all.

Rain falling on the snow-covered fraction was discarded. It is excluded
from what reaches the ground by rain*(1-asc) and was never added to the
pack, so it left the water balance entirely.

The free-water capacity was measured against pre-melt SWE.

Steps 6 and 7 are now one loop in legacy's order, which is what makes the
imelt reuse that caused the first defect impossible to write. Step 0's
instant-melt water is held aside and added back after routing, where
nothing reassigns it -- steps 4 and 5 used to assign over it.

Per the parity policy, SNOW_DIVERGENCE_REGISTER.md now records every
deliberate departure and every defect fixed, so a deck comparison against
EPA SWMM knows what to expect.
```

---

## 8. Validation results (2026-08-20) — COMMITTED `c316c83e`

**158/158 ctest**, **14/14 decks byte-identical**, **69 tests ASan-clean**,
zero new warnings. **Falsifier sweep: 5 of 5** — including iii, which §5.5
predicted would escape.
Numbers: `tests/output/s3_validation_2026-08-20/`.

### 8.1 THREE of the four gates could not see their own defect

One cause for all three: **the shared deck writer starts every pack RIPE**,
with `fw0 = fwfrac·sd0` — exactly the capacity. Introduced in S1 so that melt
would leave a pack at all; here it is what hides the defects.

- **Gate 12 (F2) passed with F2 fully restored.** A store already at capacity
  drains every drop of melt the instant it appears, so `excess == vmelt` and
  "SWE −= melt" and "SWE −= excess" are *the same number*. The defect is
  arithmetically invisible on a ripe pack.
- **Gate 13 (F3) passed with F3 fully restored, twice over.** `fw` starts at
  0.45 ft, so `fw > 0` is satisfied by the initial condition; and at 40 °F the
  store fills with **meltwater** whether or not the rain reaches it. It now
  runs unripe and **below freezing**, so the only thing that can put water in
  the store is the rain, with a SETUP asserting SWE did not move.
- **Gate 12's SETUP was also arithmetically wrong**: it compared the pack
  against `sd0` alone, while a ripe pack is *given* `sd0 + fwfrac·sd0`. At
  `fwfrac = 0.90` that reads 0.937 ft against a 0.5 ft ceiling and reports
  mass creation that is not there. Gate 15 had the right expression.

### 8.2 Falsifier iii does NOT escape

§5.5 called it unclosable without "a deck poised exactly at capacity" — which
is what a ripe pack is, and the round already had one. Measured on gate 15's
deck: **2148.145 ft³ of runoff against 2125.994** with the capacity taken
pre-melt, a 1.0 % retiming. New **gate 15b** states the invariant it breaks:
the free-water store can never exceed its *current* capacity, because a
shrinking pack has to give up the water it no longer has the snow to hold.
Numbered 15b so S4's 16 and 17 keep their numbers.

### 8.3 An S1 gate moved, correctly

`APackAbsorbingRainPublishesAGenuineZero` failed: `snow_net` read the full
gage rate instead of 0. Not a defect — **S3 makes a ripe pack transmit rather
than absorb**. F3 routes rain onto the covered fraction into a store that is
already full, so it drains straight back out the same step. The gate is about
a pack *absorbing* rain, so its deck now starts unripe (also the physical
state at 20 °F). Its premise was retired by the fix, as H5b's round retired
H5a's gate 8.

### 8.4 F2's signature, measured (§5.7)

Unripe pack, 40 °F, one hour, no precipitation:

| | SWE after 60 min |
|---|---|
| S3 | 0.48642 / 0.49321 ft |
| pre-S3 | **0.5 / 0.5 ft — exactly unchanged** |

`runoff_vol` is 0 in both, because the store has not filled yet. That is the
regime the defect lived in: no output to look at, and a pack that never
depletes.

### 8.5 Answers to the rest of §5

- **§5.3, the SoA enumeration check: vacuous, and confirmed so.** S3 adds no
  SoA field — `git diff --stat -- src/engine/data/` is empty.
- **§5.4(a): `test_snow.cpp`'s 35 gates all still pass** under S3. None of
  them pinned SWE or `fw` through the routing loop.
- **§5.4(d), the register's unconfirmed claim, now CONFIRMED:** none of the
  14 reference decks contains a `[SNOWPACKS]` section. `grep -il` over all 14
  returns 0, so 14/14 byte-identity is a real result and not a coincidence.
- **§6: `SNOW_DIVERGENCE_REGISTER.md` and the finding were NOT committed** —
  the standing rule is that workplans and validation artefacts stay out of
  the tree. Flagging it because §2 lists the register as part of the
  changeset: if you want it tracked, say so and it can go in on its own.
