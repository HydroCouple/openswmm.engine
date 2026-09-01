# F8 — the runoff ledger learns about snow — Handoff (2026-08-22)

**For:** the checking agent — the one that can compile.
**Base:** `fc6374d4`. **Uncommitted, in the working tree.**
**Finding:** `SNOW_CONTINUITY_FINDING_2026-08-21.md` — §1 is what this closes;
its §2 is retracted and this round does not depend on it.
**Register:** `SNOW_DIVERGENCE_REGISTER.md` §5 (F8) and §2 (F9, new).
**Standing findings:** lessons 1–119.

**Nothing was built or run.** Written in a sandbox that cannot compile this
tree. Every claim below is a prediction.

---

## 1. What this fixes

**Three rows and one missing writer.**

The engine's runoff continuity ledger had **no snow terms**. Legacy
`report.c:521/561` prints `Initial Snow Cover`, `Snow Removed` and
`Final Snow Cover` whenever `Nobjects[SNOWMELT] > 0`. On any deck with a pack
the starting pack was unaccounted input and the surviving pack unaccounted
output. Measured on the snow parity deck: **−8.193 %**, and **+1.419 %** with
the rows supplied.

**And `runoff_snowremov` had no writer anywhere.** Declared in `MassBalance`,
exposed through `SWMM_RUNOFF_SNOWREMOV`, returned by the mass-balance API and
read by callers — and never assigned, while `SnowSoA::removed` accumulated the
real figure with no consumer. **Same shape as F1** (`setMeltCoeffs` with no
caller) **and as the snapshot quality vectors** that wrote every pollutant
column as zero while the header advertised them. Three instances now; it is a
pattern in this codebase, not a coincidence.

## 2. F9 — a second defect, fixed in the same changeset, deliberately

`plowSnow` computed the ploughed volume with a hardcoded `* 43560.0` and the
comment `// acres → ft2`. **`subcatches.area` is in PROJECT land-area units**,
which are acres only in US; in SI they are hectares, so the plough volume came
out **2.471× too small** — the identical defect `SWMMEngine.cpp`'s
rainfall-volume site carries a comment about having already fixed once
(`LANDAREA_TO_FT2 = 1.0 / ucf::UCF(ucf::LANDAREA, options)`).

**Why it is in this changeset rather than its own.** `runoff_snowremov` had no
writer until this round, so this number reached nobody. Wiring it up is what
makes the error visible, and **shipping a newly-visible wrong number is worse
than shipping no number at all.** Stated here so it is a decision rather than
scope creep.

**I made the identical mistake writing this round's helper** and caught it on
a unit check before compiling — which is the argument for the check, and the
reason lesson 118 got written one round ago.

## 3. Changeset

```
mod:  src/engine/core/SimulationContext.hpp     (runoff_init_snow /
                                                 runoff_final_snow;
                                                 runoff_error() counts all three)
mod:  include/openswmm/engine/openswmm_massbalance.h
                                                (SWMM_RUNOFF_INITSNOW = 7,
                                                 SWMM_RUNOFF_FINALSNOW = 8 —
                                                 APPENDED, nothing renumbered)
mod:  src/engine/core/openswmm_massbalance_impl.cpp   (two cases)
mod:  src/engine/core/SWMMEngine.cpp            (snowCoverVolumeFt3(); init and
                                                 final sites; the snowremov
                                                 writer)
mod:  src/engine/hydrology/Snow.cpp             (F9 — LANDAREA conversion)
mod:  src/engine/plugins/DefaultReportPlugin.cpp (three rows, legacy order)
mod:  tests/unit/engine/test_transport_snow.cpp (+3 gates, 23 → 26;
                                                 Opts::weplow, Opts::f_out)
```

**`CMakeLists.txt` NOT touched.** No new files — **⛔ HARD STOP, lesson 79.**

## 4. Design notes

### 4.1 `snowCoverVolumeFt3` mirrors legacy exactly, and two details are
###     load-bearing

Legacy `snow_getSnowCover` (`snow.c:587`):

```c
for (i = SNOW_PLOWABLE; i <= SNOW_PERV; i++)
    snowCover += (wsnow[i] + fw[i]) * fArea[i];
return snowCover * (Subcatch[j].area - Subcatch[j].lidArea);
```

- **`fw` is IN.** The free-water store is water the pack is holding, not water
  that has left. Counting only `wsnow` understates the term by up to `fwfrac`
  of every pack — and would leave a residual that looks exactly like a leak,
  which is the shape of the residual F8's first write-up chased and retracted.
- **LID area is OUT**, matching the plough volume (Gap #60) and legacy Build
  5.2.0. Water in a LID unit is already in init/final STORAGE via
  `lid_.storedVolume()`; counting it here doubles it.

### 4.2 The rows are guarded on the project having packs, not on being nonzero

Matching legacy's `Nobjects[SNOWMELT] > 0`. **A deck with packs showing 0.000
is saying something**; a deck without them should not carry the rows at all.

### 4.3 The "Initial Storage" label is left alone

Legacy calls the same row "Initial LID Storage". Renaming it is not part of
this defect and the 14-deck corpus compares report bytes — it would move decks
that have nothing to do with snow. Recorded in the source so the difference is
a decision.

## 5. ⚠ Things I could not check and you can

1. **Ordering at the init site.** `runoff_init_snow` is computed at
   `SWMMEngine.cpp:~6793`, and it is only correct if the snowpack initial
   state has already been applied to the SoA (`~5613`/`5646`). Line order says
   yes; **confirm it**, because a zero here is silent and would restore
   exactly the defect being fixed. **Gate 24's `ASSERT_GT(runoff_init_snow,
   0)` is the guard** — if it fires, this is why.
2. **`ucf::UCF` availability in `Snow.cpp`.** `../core/UnitConversion.hpp` is
   already included; confirm `ucf::LANDAREA` resolves.
3. **`ctx.snowpack_names.size()`** in `DefaultReportPlugin.cpp` — used the same
   way in `InpWriter.cpp:213`. Confirm the header is reachable there.
4. **The enum addition** is append-only (7, 8) with nothing renumbered, so no
   ABI break. Confirm no `switch` over `SWMM_RunoffTotal` elsewhere now warns
   on unhandled cases under `-Wswitch`.

## 6. Anticipated failure modes, in likelihood order

**(a) The 14 reference decks are UNCHANGED.** None has a `[SNOWPACKS]`
section, so `has_snow` is false and no row is emitted. **If any deck moves, it
is the `runoff_error()` change**, which now counts three terms that are zero on
a snow-free deck — arithmetically inert, so a movement means one of them is
*not* zero and that is a finding.

**(b) The snow parity deck MOVES, and it must.** `−8.193 % → +1.419 %`, and
three new rows in the `.rpt`. **This is the round's headline measurement.**
The stored baseline is superseded by design; regenerate it after, with
provenance, per `tests/parity/snow/README.md` §3.

**(c) Gate 26's SETUP fires** — `soa.removed` is 0, no snow was ploughed.
Check `snn0`, `weplow`, `f_out` in that order. Note the deck writer previously
hardcoded the `REMOVAL` row to zeros, so **no existing gate ploughs anything**
and this path has never run in a test.

**(d) Gates 24/25 close but by luck**, because `runoff_error()` divides by a
`total_in` that now includes the snow term. **The SETUP legs are what prevent
this** — each asserts the individual terms are nonzero before asserting the
closure.

**(e) An SI deck's plough volume moves by 2.471×.** That is F9, and it is
correct. No deck in any corpus is SI *and* snowy, so expect this to be
invisible — **and say so rather than reporting it as verified.**

## 7. Falsifier sweep

| # | falsifier | expected failing gates |
|---|---|---|
| i | Drop `runoff_init_snow` from `runoff_error()` | 24, 25 |
| ii | Drop `runoff_final_snow` from `runoff_error()` | 24, 25 |
| iii | Restore `runoff_snowremov` to having no writer | **26**, and 24/25 only if the deck ploughs — it does not, so 26 alone |
| iv | Count `wsnow` only, dropping `fw`, in `snowCoverVolumeFt3` | 24 and 25 — **and if it escapes, the two gates are closing on a balance where the free-water store happens to be empty**, which the `ripe` flag controls. Re-run 25 with `ripe = true` before accepting an escape |
| v | Restore F9's `* 43560.0` | **nothing on a US deck** — flagged, and it is why F9 rides along rather than standing alone. Closing it needs an SI snow deck, which no corpus has |
| vi | Emit the rows unconditionally instead of under `has_snow` | **nothing in the gates** — but the 14 decks should then move, which is where it shows |

**v and vi are predicted to escape the gates and be caught by the decks.** That
split is the point of running both.

## 8. Known gaps

- **F9 is unobservable on every deck this program has** (§7 v).
- **A2 is now DECIDED (user, 2026-08-22): both yes** — the pack's initial water
  takes the `INITIAL_STATE` age, and the pack age persists through a hotstart.
  **Not implemented here**; this changeset is the ledger. Recorded in the
  register as decided-and-owed. It also makes S2b's falsifier ii observable,
  which §10.5 of the S2b handoff wanted.
- **O4 is unsettled** and this round's numbers must come from CLI runs.

---

## 9. Validation results (2026-08-22) — COMMITTED `0ad28685`

**Every prediction in §6 held except one, and the exception is the round's
finding.** Build clean (376/376, no warning from any changed file), **ctest
159/160**, **26 snow gates + 36 `test_engine_snow`**, **14/14 reference decks
byte-identical in `.out` *and* in `.rpt`**, **falsifier sweep 7 of 7 observed**
once the deck leg is counted, **80 tests ASan/UBSan clean**.
Numbers: `tests/output/f8_validation_2026-08-22/`.

### 9.1 The headline: −8.193 % → **+0.407 %**

```
  Initial Snow Cover .......         2.500         1.500
  Total Precipitation ......        20.001        12.000
  Infiltration Loss ........        10.728         6.436
  Surface Runoff ...........        10.899         6.539
  Snow Removed .............         0.204         0.122
  Final Snow Cover .........         0.567         0.340
  Final Storage ............         0.013         0.008
  Continuity Error (%) .....         0.407
```

**Better than the +1.419 % §1 predicted, and the difference is itself a
check.** The prediction came from the S2b re-run round's reconciliation, which
had `Snow Removed` as an unquantified hypothesis and read the surviving pack
from `newSnowDepth` — SWE only. The ledger supplies both: removal is
**0.122 in** and the pack with its free water is **0.340 in** against the
0.323 in SWE. 0.1916 − 0.122 − 0.017 = 0.055 in, which is the 0.407 %. The
two independent routes agree to the third decimal, and **that is what
retires the "unexplained loss"** rather than the new number on its own.

### 9.2 ⛔ §6(b) predicted the deck must MOVE. It must not, and it did not.

The `.out` is **byte-identical** to the pre-F8 baseline. That is the correct
outcome and the prediction was wrong on its own terms: F8 writes ledger fields
and report rows, and `snowCoverVolumeFt3` reads state without touching it, so
there is no hydrology in the changeset to move a binary result. The `.rpt`
moved and is where the whole change lives — three rows and the error line.

F9 does touch a number that reaches an output, and it is worth being exact
rather than saying "invisible": on a US deck `1/Ucf[LANDAREA][US]` is
**43561.596**, not 43560, so `removed` moves by a factor of **1.0000366** —
3.7e-5, below the report's three decimals. It is not zero, and the reason to
prefer it anyway is parity: legacy parses `Subcatch.area` to ft² with exactly
this division (`output.c:250` converts back the same way), so the literal was
off from legacy as well as from SI.

### 9.3 ⛔ Falsifier i escaped both ledger gates, and `runoff_error()`'s own
###     guard is why

§6(d) worried gates 24 and 25 might "close but by luck". They did, in a form
the SETUP legs could not catch: **`runoff_error()` returns 0.0 when
`total_in` is zero**, and on a deck whose only input is the pack, dropping
`runoff_init_snow` from the input side leaves `total_in` at exactly zero.
The gate does not read a large error — it reads the fallback, and passes.

Measured: falsifier i **PASS** on both gates on the first sweep. Falsifier
vii (i–iv at once) failed on gate 26 alone, for the same reason.

Fixed with **0.2 in/hr of rain on both decks**. At 10 °F gate 24's pack cannot
melt and an hour of that is far inside its free-water capacity, so nothing
leaves and the gate's premise survives — the pack now also absorbs, which the
final-cover term has to carry, and gate 24 consequently catches falsifier iv
as well, which it did not before. Both decks now assert
`runoff_rainfall > 0` in SETUP, and gate 24 asserts `runoff_runoff == 0` so
the added rain cannot quietly become an outflow.

After the fix: **i, ii, iii, iv, v, vii all observed.**

**(120)** *a closure gate is only as good as the denominator it divides by.
An error expressed as a fraction of total input cannot see a term removed
from the input side when that term IS the input — the function's own
empty-ledger guard answers first, and it answers zero.*

### 9.4 F9 was predicted unobservable. It is not, and the gate that proves it
###     needs no deck.

§7 v: "nothing on a US deck — closing it needs an SI snow deck, which no
corpus has." True of decks. But `plowSnow` takes a `SimulationContext`, and
the unit system is a field on it — so `SnowRemoved.PlowingConvertsHectaresNot
AcresUnderSI` builds the same pack twice, once under `FlowUnits::CFS` and once
under `CMS`, and asserts the ratio is the hectare/acre 2.471 rather than 1.
It fails on the restored defect.

**And restoring F9 also failed `SnowRemoved.PlowingAccumulatesRemoved`** — an
existing gate that hardcoded the identical `43560.0` literal, which is why the
first full ctest of this round came back 158/160. §5(a)'s "no other existing
gate moved" was not on the handoff's list and should have been: a defect
duplicated in a gate is a gate that ratifies it.

### 9.5 Everything else in §5 and §6 held

- **§5(1) ordering: correct.** `initHydrology()` at `SWMMEngine.cpp:5324`,
  `initMassBalance()` at `5327`; the deck's `SD0` reaches the SoA at `5753`,
  inside the former. Gate 24's `ASSERT_GT(runoff_init_snow, 0)` never fired.
- **§5(2), §5(3): resolved by the compile.** `ucf::LANDAREA` resolves in
  `Snow.cpp` and `ctx.snowpack_names` in `DefaultReportPlugin.cpp`.
- **§5(4): no `-Wswitch` warnings**, zero occurrences in the build log.
- **§6(a): the 14 decks are unchanged**, `.out` and `.rpt` both — the five
  `.rpt` files that differed textually differ only in wall-clock lines.
- **§6(c): gate 26's SETUP did not fire.** `soa.removed` is nonzero on the
  first run; `snn0 = 0.5`, `weplow = 0.05`, `f_out = 0.40` are right.
- **§6(e) is quantified rather than asserted** — see §9.2.
- **§7 vi behaves exactly as the split predicted:** inert in the gates,
  **14 of 14 reports move** when the rows are emitted unconditionally. That
  is the leg that makes the `has_snow` guard a tested decision.
- **Row order matches legacy `report.c:512-570` line for line**, including
  which rows sit inside the guard.

### 9.6 Still owed

- **A2** — decided both-yes by the user on 2026-08-22, not implemented. It is
  the next snow item and it makes S2b's falsifier ii observable.
- **O4** — unsettled. This round's numbers are all CLI, per its §5.
  `SNOW_CONTINUITY_FINDING`'s retracted §2 is now fully explained by it.
- **`WATER_AGE_SNOW`** — scoped (`WATER_AGE_SNOW_SCOPING_2026-08-22.md`),
  untouched.
- **The register was already current** — F7, F8 and F9 all have entries; this
  round only had to fill in the commit and correct F8's measured value from
  the predicted +1.419 % to the measured +0.407 %, and F9's "unobservable"
  note (§9.4).
- **The parity deck is still not in the corpus runner** — README §5. The
  count stays 14 when it should be 15, which is why this round had to run it
  by hand.
