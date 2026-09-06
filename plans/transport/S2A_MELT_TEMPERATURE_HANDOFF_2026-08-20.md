# S2a — meltwater arrives at 0 °C — Handoff (2026-08-20)

**For:** the checking agent.
**Base:** `d7ee70be` (S1).
**Decision:** user, 2026-08-20 — *both tracks get the full answer*. This is
the heat half. **§7 scopes the age half (S2b) from a completed survey.**
**Standing findings:** lessons 1–108.

---

## 1. What this delivers

S1 fixed the *volume* of water arriving under a snowpack. It still arrived at
the configured `RAINFALL` temperature. **Meltwater is at 0 °C essentially by
definition** — that is what melting means — so on a winter deck that was the
difference between a snowmelt-fed stream and a rain-fed one.

Arriving water under a pack is **two different waters**: meltwater, and rain
that reached the ground through the snow-free fraction. `snow_net_*` is their
**sum**, and a sum cannot say what either is worth.

**The heat half needs no new state at all.** The freezing point is not
something a pack has to remember.

## 2. The split is published, not reconstructed

`snow_melt_imperv` / `snow_melt_perv` join `snow_net_*`, carrying the
melt-only term under the **identical** plowable/non-plowable area blend
(`SWMMEngine.cpp`, beside the existing lines). `snow_net − snow_melt` is then
the rain-through.

That follows **A4's precedent**: the split already exists as a local in the
solver, so publish it rather than have transport rebuild a blend it could get
subtly wrong. Rebuilding it downstream would also mean two copies of the area
weighting — the duplication D-H5e was about.

`arrivingMeltFraction` returns **0** wherever there is no pack, where
`IGNORE_SNOWMELT` is on, or where nothing arrived, so callers blend
unconditionally and a bare deck is bit-identical.

## 3. Changeset (uncommitted)

```
mod:  src/engine/data/SubcatchData.hpp      (snow_melt_imperv / _perv)
mod:  src/engine/core/SWMMEngine.cpp        (publish the melt-only blend)
mod:  .../components/WatershedCommon.{hpp,cpp}
      (arrivingMeltFraction, arrivingPrecipTemperature, kMeltwaterTempC)
mod:  .../HeatModule/HeatWatershed.cpp      (use the blended value)
mod:  tests/unit/engine/test_transport_snow.cpp  (+3 gates, 7 → 10)
```

`tests/unit/engine/CMakeLists.txt` is **NOT touched**. Syntax-clean under
`-Wall -Wextra`. Nothing built or run.

## 4. Design notes

### 4.1 `kMeltwaterTempC` is a named constant, not a literal

So a gate asserts against the constant the code uses rather than a number a
reader hopes it uses.

### 4.2 The melt fraction is clamped, and the clamp is not defensive noise

`melt <= net` holds by construction — both go through the same area weights.
The clamp exists because a future change to **one** blend and not the other
would otherwise hand a caller a fraction above 1 and silently invert a
mixture. Stated in the code.

### 4.3 `t_rain` still appears twice in `HeatWatershed`, correctly

At the run-on fallback and the no-water fallback. Neither is precipitation,
so neither takes the melt blend. **Check this if you disagree** — it is the
kind of partial substitution that looks like an oversight.

## 5. Validation protocol

1. **Isolated worktree at `d7ee70be`.** Lesson 71. Suite was **156/156**;
   carry no exemption forward (lesson 89).
2. **⛔ HARD STOP — lesson 79.** No `CMakeLists.txt` entry expected at all
   (§3). The lock cleared last round; if `git diff` and `git diff HEAD`
   disagree again, the index has re-staled — that is now a known mode.
3. Build, zero new warnings. Snow suite, then the full suite.

   **Anticipated failure modes — my record is 4 of 26, so trust the sweep.**

   (a) **Gate 9 may SKIP.** It needs a *partial* areal cover so the melt
   fraction is strictly between 0 and 1. It calls `GTEST_SKIP` with an
   explanation rather than passing vacuously if cover is total or absent.
   **A skip is a result: report it.** Closing it means adjusting the ADC
   curve or the rain intensity, not deleting the gate.

   (b) **Gate 8's `any_below_rain` could pass for the wrong reason** if the
   surface never reaches equilibrium. The per-subarea
   `arrivingPrecipTemperature == kMeltwaterTempC` assertions above it are the
   load-bearing ones; the deck-level check is corroboration.

   (c) **`snow_melt_*` may be published before `imelt` is final.** The
   publication sits with the existing `snow_net_*` lines and uses the same
   `soa.imelt` reads, so they are consistent by construction — **but confirm
   `imelt` is not mutated after that point**, because `snow_net` would then
   be stale too and that would be an S1 defect I missed.

4. **Falsifier sweep:**

   | falsifier | expected failing gates |
   |---|---|
   | i. `arrivingPrecipTemperature` returns `t_rain` always (the S1-only behaviour) | **8** — the defect gate. If it does not fail, S2a is unobserved |
   | ii. publish `snow_melt_*` without the area blend (raw `imelt[imperv_idx]`) | **probably nothing** — flagged. It only differs when the plowable fraction is non-zero, and this deck's `snn0` is 0. **Owed if it escapes**; a plowable deck would close it |
   | iii. drop the `[0,1]` clamp | **nothing** — §4.2 says so outright; it guards a future divergence, not a current one |
   | iv. use `kMeltwaterTempC = t_rain` | 8 |
   | v. apply the melt blend to the run-on term too | **probably nothing** — no gate here has run-on onto a snowy subcatchment. Owed |

5. **Prior suites:** full C++ suite, 14/14 decks, ASan/UBSan. **H5a's suite
   must be bit-unchanged** — no existing deck has a pack, and gate 10 asserts
   the pack-less path returns the configured value untouched.

6. **Record:** falsifier i; whether gate 9 skipped; and **the measured
   arriving temperature and subarea temperatures**, S1-only versus S2a. S1's
   round measured the volume defect at `3600 s` exactly; this is the value
   half of the same number.

## 6. Known gaps

- Falsifiers ii and v predicted to escape; both need decks this file lacks
  (a plowable pack; run-on onto a snowy subcatchment).
- **Rain that falls ON the snow-covered fraction** (`rain·asc`) contributes
  to `imelt` through `rainMeltRate` but I did **not** establish where the
  water itself is accounted. `fw` receives only `imelt·dt` (`Snow.cpp:314`).
  **This is a question about the snow module's water balance, not a claim** —
  it needs checking before S2b's age mixing rests on it.

## 7. S2b — the age half, scoped from a completed survey

The pack state, surveyed at `d7ee70be` (reading the writes, not the
declarations):

| fact | site |
|---|---|
| Pack is **per snow surface**, 3 per subcatchment (`PLOWABLE`, `IMPERV`, `PERV`) | `Snow.hpp:55-58` |
| Water is **two stores**: `wsnow` (SWE) and `fw` (free water) | `Snow.hpp` SoA |
| Snowfall accumulates: `wsnow += snowfall·dt` | `Snow.cpp:381` |
| Melt converts snow → free water; free water **above `fwfrac·wsnow`** drains out as the published `imelt` | `Snow.cpp:314-320` |
| SWE then decreases by the drained amount | `Snow.cpp:330` |
| Plowing moves snow **between surfaces and to another subcatchment** | `Snow.cpp:414-447` |
| Removal zeroes both stores | `Snow.cpp:206-207` |

**The design that follows:**

- One age per **snow surface** (`n_subcatch × 3`), complete-mix over
  `wsnow + fw` together. Two stores would be more faithful; one is the
  minimum that carries residence time, and the melt path moves water between
  them within a single step anyway.
- Ages by `+dt`, mixes snowfall in at the `RAINFALL` source age, and melt
  **leaves at the pack's age without changing it** — the A4 column pattern.
- `arrivingPrecipAge` blends exactly as `arrivingPrecipTemperature` does:
  `(1 − f)·a_rain + f·pack_age`, with the same fraction.

**The one thing S2b needs that S2a did not: a published plow transfer.**
`plowSnow` moves water between surfaces and subcatchments *inside*
`snow_.execute`, so an age update running afterwards cannot reconstruct which
water went where. Following A4 again — the values exist as locals, so publish
them — rather than snapshotting before and guessing after. **That means S2b
touches hydrology**, exactly as A4 did, and saying so now avoids repeating
lesson 70 (a scoping claim is a prediction, not a constraint).

**Hotstart is the open question**, and A2a's precedent argues for deferring:
it declined to persist subarea depths because a restored age over an
unrestored volume is a fiction. Pack SWE *is* in the hotstart
(`SWMMEngine.cpp:5646`), so unlike A2a's case the volume would be restored —
which makes persisting the age *possible* here, and therefore a decision
rather than an impossibility.

## 8. Prepared commit message

```
feat(transport): meltwater arrives at the freezing point (S2a)

S1 fixed the VOLUME of water arriving under a snowpack; it still arrived at
the configured RAINFALL temperature. Meltwater is at 0 C essentially by
definition, and on a winter deck that difference is a snowmelt-fed stream
against a rain-fed one.

Arriving water under a pack is two different waters -- melt, and rain that
reached the ground through the snow-free fraction. snow_net_* is their sum,
and a sum cannot say what either is worth. snow_melt_imperv / snow_melt_perv
now carry the melt-only term under the identical area blend, following A4's
precedent of publishing a split that already exists as a solver local.

arrivingMeltFraction returns 0 wherever there is no pack, IGNORE_SNOWMELT is
on, or nothing arrived, so callers blend unconditionally and a bare deck is
bit-identical.

The heat half needs no pack state: the freezing point is not something a
pack has to remember. The age half (S2b) does, and is scoped in the handoff.
```

---

## 9. Validation results (2026-08-20) — COMMITTED `8b7d1cf7`

**158/158 ctest**, **14/14 decks byte-identical**, **77 tests clean under
ASan/UBSan**, zero new warnings. **Falsifier sweep: 4 of 5** — including ii
and v, both of which §5.4 predicted would escape. Gate 9 skipped as delivered
and is now closed. Full numbers:
`tests/output/s2a_validation_2026-08-20/measurements.md`.

**Base:** validated and committed on `f105273a`, not `d7ee70be`. Two commits
landed between — the `hydrocouple/swmm6_rel` merge (PR #137, legacy
TRANSECTS) and a test-registration fix — and neither touches a file in this
changeset.

### 9.1 The changeset segfaulted on its first gate

`EXC_BAD_ACCESS` at `0x0` in `SWMMEngine::stepRunoff`. **`SubcatchData`
enumerates its arrays by hand in SIX places and `snow_melt_*` was in one of
them** (`resize()`). Missing from `grow()`, `reserve()`, `erase()`,
`shrink_to_fit()` and `reset_state()`. The parser adds subcatchments through
`grow()`, so the arrays stayed size 0 while `snow_net_*` grew and the
publication wrote past the end. All five added.

### 9.2 The magnitude (§5.6)

Hydrology identical either way.

| deck | | S2a | S1-only |
|---|---|---|---|
| pure melt, no rain | arriving | **0 °C** | 20 °C |
| | subareas | 0 / 0 / 0 | 20 / 20 / 20 |
| melt + rain-through | arriving | **7.235 / 7.235 / 11.389 °C** | 20 / 20 / 20 |
| | subareas | 7.220 / 7.189 / 11.389 | 20 / 20 / 20 |
| | runoff | 7.195 | 20 |

S1's round measured the volume defect at 3600 s exactly; this is the value
half — the full 20 °C on a pure-melt deck. The **per-subarea spread** in the
second deck is something the S1 form cannot produce at all: one scalar cannot
differ between surfaces.

### 9.3 Gate 9 skipped — and the two facts that explain it

§5.3(a) called this and was right; the cause was not in the handoff. The
default ADC curve is **all ones** (`Snow.hpp:92`) and **`si` is pinned to the
initial pack depth** (`SWMMEngine.cpp:5613` — the deck's `SD100` field is
never read). So every deck here sits at `asc = 1`, `rain·(1 − asc)` is
identically zero, and `f = 1` exactly. Closed with an explicit flat `ADC`
row: cover 0.5 and 0.2 in/hr give **f = 0.4306**, near the middle of its
range. Gate 9's bracketing was also tightened to **strict** — non-strict
bracketing is satisfied by either endpoint, and one of those endpoints is
precisely the S1-only answer.

### 9.4 Falsifier ii closed by a new gate 11

The area blend needs **two** conditions at once, and no deck had either:
`snn0 > 0` so a plowable surface exists, and partial cover, because the
plowable surface is never depleted (`Snow.cpp:100`) while the others are —
which is what makes the two melt rates differ. With `snn0 = 0.4` and cover
0.5 the published value is `2.450428683e-06`, strictly between the surface
rates `3.501e-06` and `1.750e-06`; reading one surface raw lands on an
endpoint.

### 9.5 Falsifier v does not escape — and confirms §4.3

H5a's own suite catches it: `RunonCarriesTheDonorsTemperature` and
`EveryRunonContributorKeepsTemperaturesInsideTheSources`. Run-on is not
precipitation, so the melt blend does not belong there. The second `t_rain`
site (line 297) is the no-runoff placeholder, read only when the subcatchment
produced nothing — no water carries it. **I agree with §4.3 on both.**

Falsifier iii is the only escape and it is deliberate: §4.2 says outright the
clamp guards a future divergence, not a current one, so no deck can reach it.

### 9.6 §5.3(c) confirmed

Every write to `imelt` is inside `SnowSolver::execute` (`Snow.cpp:205-321`),
which returns before the publication, and nothing else in `src/engine/`
writes it. `snow_net_*` and `snow_melt_*` are consistent by construction —
no S1 defect there.

### 9.7 Noticed, not fixed

`Snow.cpp:205` — the sub-0.001-inch instant-melt branch does
`imelt[ui] += (ws + fw)/dt`, but step 4 then **assigns** `imelt[ui]`
unconditionally and step 5 zeroes it because `wsnow` is now 0. The water from
an instantly-melted thin pack is discarded rather than delivered. Tiny
volumes, but a silent loss, and it is in the path S2b's age mixing will rest
on — related to §6's open question about where `rain·asc` water is accounted.

### 9.8 Still owed

- §6's question stands: **where the water of rain falling ON the covered
  fraction is accounted.** `fw` receives only `imelt·dt`, and 9.7 is a second
  hole in the same balance. Worth settling before S2b.
- No gate has run-on onto a snowy subcatchment; falsifier v was caught by a
  bare-deck gate, not by that configuration.
- S2b as scoped in §7, including the published plow transfer and the hotstart
  decision.
