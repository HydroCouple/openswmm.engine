# The quality ledger's units, audited — scoping (2026-08-22)

**For:** the checking agent.
**Base:** `55a70839`.
**Standing findings:** lessons 1–151.

**⚠ This is a SCOPING document, not a changeset.** I traced the units chain
far enough to answer Finding 5, to show that Findings 5 and 6 are one problem
rather than two, and to turn up an eighth finding. **I stopped before fixing
anything, deliberately** — see §5.

---

## 1. What was traced, and it inverts Finding 5

**Our washoff mass is in milligrams. Legacy's is in pounds.**

| | expression | units |
|---|---|---|
| ours | `w_lid_rain = c_rain37 * L_PER_FT3_37 * v_lid_rain;  // mg` | `total_washoff_load` is **mg/s**, so `mass = … * dt` is **mg** |
| legacy | `massLoad = cOut * vOut2 * Pollut[p].mcf` | **lbs (or kg)** — `mcf` applied **at source** |

Legacy applies `mcf` **before** booking, so **both** its per-subcatchment
total (`surfqual.c:357`) and its `RUNOFF_LOAD` ledger term (`:366`) are in
lbs. We book the same raw `mass` (mg) into both.

Then:

- the **Subcatchment Washoff Summary** applies `MG_TO_LBS = 1/453592` — ✅
  **correct**, and its header says `lbs`;
- the **`Runoff Quality Continuity` ledger row** prints the same variable
  **raw** — ❌ **wrong**, mg under a mass-units header.

**So Finding 5's answer is the opposite of the obvious one.** The summary
printing 0.000 is not a bug in the summary: **it is the summary correctly
reporting a genuinely tiny number of pounds**, while the ledger row beside it
prints a large number of milligrams and looks authoritative.

**Do not "fix" the summary.** That was my first instinct and it would have
destroyed the one correct reading in the report.

## 2. Which makes Finding 6 partly dissolve — and partly not

Legacy against ours on the simplest deck, as reported:

| row | legacy | ours | ours ÷ 453592 |
|---|---|---|---|
| washoff | 0.000 | 1.369 | **3.0e-6** — plausibly agrees |
| buildup | **0.885** | 2.500 | 5.5e-6 — **does not agree** |

**If the whole ledger were uniformly in mg, buildup would agree too. It does
not.** That points at the ledger **mixing units across its own terms** —
some booked with a conversion applied, some without — which **no single
conversion can repair** and which is why this is an audit rather than a
one-line fix.

**⚠ I have not verified the buildup arithmetic.** The mg reading of buildup
is inferred from the pattern, not traced to a source expression the way §1's
washoff chain was. **That trace is the first task in §4.**

## 3. 🛑 Finding 8, turned up on the way

**`qual_bmp_removal` has ZERO write sites.** Every mention in `src/engine/`:

```
SimulationContext.hpp:1066   declaration
SimulationContext.hpp:1089   .assign(np, 0.0)
SimulationContext.hpp:1114   std::move out
SimulationContext.hpp:1136   std::move back
SimulationContext.hpp:1156   positional enumerator list
```

Declared, resized, moved and enumerated — **written nowhere.** Legacy writes
it at `surfqual.c:352`. The report renders a `BMP Removal` row from it.

**Fourth instance of F8's family**, after the snapshot quality vectors (A2b),
the snow ledger rows (F8), and the subcatchment temperature column (Finding
2). Every one is a row that exists in the header, is rendered every run, and
is filled by nothing.

## 4. What the next round should do, in this order

1. **Trace buildup's units to a source expression**, the way §1 traced
   washoff. Until that is a measurement rather than an inference, §2's
   conclusion is a hypothesis.
2. **Audit every `qual_*` term** — `init_buildup`, `surface_buildup`,
   `wet_deposition`, `sweeping`, `bmp_removal`, `infil_loss`, `runoff_load`,
   `final_buildup` — and record, per term, **the units at its write site**.
   The write sites are all in `SWMMEngine.cpp` (2127, 2683, 2728, 2816, 2823,
   2835, 2908, 4073, 6384) except `bmp_removal`, which has none.
3. **Only then decide where the conversion belongs.** Legacy's answer is
   "at source, before booking" — which makes every downstream reader
   unit-clean and is why its summary and its ledger agree. Matching that is
   the smaller change *if* the audit says our terms are uniformly mg.
4. **Fix `qual_bmp_removal`** (Finding 8) or **remove the row** — a rendered
   row backed by nothing is worse than an absent one, because it reads as a
   measured zero.
5. **Gate it across the seam**, per lesson 147: the summary's system total and
   the ledger's runoff-load row are the *same mass by construction*, so they
   must agree after conversion. That equality is checkable and nothing checks
   it today — which is exactly how a 453592× discrepancy survived.

## 5. Why I stopped here

Three reasons, and the first is the one that matters:

- **My first instinct was wrong.** I would have "fixed" the summary to match
  the ledger, because the summary was the thing visibly printing zeros. The
  trace says the summary is the correct one. **A units bug shows its symptom
  on the correct side as readily as the wrong one**, and Finding 6's numbers
  do not distinguish them without the source trace.
- **Findings 5, 6 and 8 are one chain.** Fixing 5 alone changes the numbers
  Finding 6 is measured against, so 6 would then have to be re-measured from
  scratch — and a fixed 5 would make the ledger *look* consistent while
  buildup stayed wrong.
- **Everything in §1 is a code read.** It is a careful one and the exactly-
  453592 ratio corroborates it, but lessons 126, 130 and 144 are all the same
  shape: **a read is not a run.** The next round should confirm §1 by
  measurement — put a known mass on a deck and check which printed number
  carries it — before changing a line.

## 6. Everything else still open, unranked

| | |
|---|---|
| **Finding 7** — LID deck sheds **34×** legacy (15.482 vs 0.456 acre-feet) | issue #131 family; justifies deferring the LID corpus deck |
| `old_runon_inflow` orphan | maintained by rotate/reset and in two positional enumerator lists — **removing it moves the hotstart layout.** Decision owed |
| `Runoff.cpp:327-329` comment says "non-LID area", code uses full area | one of the two is wrong |
| `test_engine_concurrent` read/write race on `site_drainage_model.inp` | the `b85b802d` guard models duplicate *names*, not shared-input races |
| No corpus deck has an LID, an outfall return, or a self-route | three regression-blind spots |
| `CMAKE_CXX_COMPILER` uncompared; cache search has no depth margin | `run_corpus.sh` |

---

# 7. Audit results (2026-08-23) — §1 is refuted by measurement

**Run on:** HEAD `55a70839`. **No code changed** — §4's ordering says audit
first, and §5's reason for stopping applies with more force now, not less.
Artefacts: `tests/output/qual_units_2026-08-23/`.

**§5 was right to distrust §1, and the distrust was not paranoid enough.**
§1 says our washoff mass is in milligrams and the Washoff Summary is
therefore correct. **Measured: our washoff mass is in `mg/L · ft³`, and BOTH
printed numbers are wrong** — the summary by **28.3168×**, the ledger row by
**16018×**.

## 7.1 The known-mass measurement §5 asked for

A deck with **EMC washoff only, 100 mg/L, no buildup**, so the mass is
`C × V` and nothing else can contribute. The deck's own runoff is 0.417
acre-feet = 18157.174 ft³.

| | ours | legacy | ratio |
|---|---|---|---|
| ledger `Surface Runoff` row | **1 815 717.383** | **113.082** | **16 056.6** |
| Washoff Summary, system | **4.003** | **113.082** | **28.25** |

`100 × 18157.174 = 1 815 717.383` — **our ledger is exactly `C·V`, to the
digit.** The arithmetic closes:

```
C·V                      = 1 815 717.383   [mg/L·ft³]   <- what we book
  × 28.3168 L/ft³        =    51 415 391   [mg]
  ÷ 453 592 mg/lb        =       113.352   [lb]         <- legacy prints 113.082
combined  28.3168/453592 = 1/16 018.5
```

Measured 16 056.6 against predicted 16 018.5, and 28.25 against 28.3168 —
both **0.24 % out, the same residual as the two engines' runoff volumes.**

**So the summary is not "correctly reporting a tiny number of pounds".** It
is reporting `mg/L·ft³ ÷ 453592`, which is short of pounds by exactly the
missing `L_PER_FT3`. §1's instruction "do not fix the summary" happens to be
right — but for the opposite reason: the summary is wrong too, and fixing it
alone would leave the ledger 16018× out.

**§1 generalised from the one expression that already carries the
conversion.** `w_lid_rain = c_rain37 * L_PER_FT3_37 * v_lid_rain` is the LID
wet-deposition path and it *is* mg. The **EMC/landuse path** at `:2745`
(`load = wp.coeff * q_flow * frac`) applies neither `L_PER_FT3` nor a mass
factor. Lesson 149's shape, landing on the document that cites it.

## 7.2 §4.1 and §4.2 — every `qual_*` term, units at its write site

| term | site | expression | units |
|---|---|---|---|
| `qual_init_buildup` | 6384 | `mass * norm` | **user mass (lb/kg)** |
| `qual_surface_buildup` | 2683, 2728 | `buildup_change * norm` | **user mass** |
| `qual_sweeping` | 2127 | `removed * norm` | **user mass** |
| `qual_final_buildup` | 4073 (`=`) | `buildup * norm` | **user mass** |
| `qual_final_buildup` | 2823 (`+=`) | `w_mass_pq` | **mg** — and **dead**, see §7.4 |
| `qual_wet_deposition` | 2816 | `c_rain · L_PER_FT3 · v_rain` | **mg** |
| `qual_infil_loss` | 2835 | `c_ponded · v_infil` | **mg** |
| `qual_runoff_load` | 2908 | `total_washoff_load · dt` | **incoherent — see below** |
| `qual_bmp_removal` | — | **none** | **never written** (Finding 8 ✓) |

`norm` is `frac × area[ui]` with `area` in **user** land-area units and the
buildup coefficients in mass per that same unit, so the buildup family is
self-consistently in the header's units. **That is the one family that is
right.**

**`qual_runoff_load` is a sum of terms in different units:**

| contributor | site | expression | units |
|---|---|---|---|
| EMC | 2751 | `coeff · q_flow · frac` | **mg/L·ft³/s** |
| EXPON | 2755 | `coeff · q_expon^n · buildup · norm` | **user mass · (in/hr)^n / s** |
| RATING | 2758 | `coeff · q_flow^n · frac` | **mg/L·ft³^n/s** |
| ponded outflow | 2860 | `w_outflow_pq / dt` | **mg/s** |
| LID run-on | 2881 | `w_lid_runon / dt` | **mg/s** |

**Five contributors, at least three unit systems, added into one accumulator
and then into a ledger whose other terms are in pounds.** No single
conversion downstream can repair that — which is §2's conclusion, reached by
a different and now-measured route.

**⚠ EXPON and RATING are not merely unit-shifted from legacy — the formulas
differ.** Legacy computes a *concentration* (`landuse_getWashoffQual`, mass/ft³)
and multiplies by `vOutflow`; RATING there is `coeff · (runoff·area)^(expon−1)`
against our `coeff · q_flow^expon`. That is a modelling difference on top of
the units one and is **out of this audit's scope**.

## 7.3 Where the conversion belongs — legacy's answer, verified

`landuse.c:334` — **at parse time**:

```c
if ( func == EMC_WASHOFF ) x[0] *= LperFT3;
```

so `landuse_getWashoffQual` returns **mg/ft³ for every washoff type**, and
`landuse_getWashoffLoad` applies `Pollut[p].mcf` **once, at source**:

```c
washoffLoad = washoffQual * vOutflow * landuseArea / area * Pollut[p].mcf;   // lbs
```

Every legacy booking is therefore in lbs, which is why its summary and its
ledger agree. **Our `QualityHandler.cpp:314` stores the EMC type and never
scales the coefficient**, and nothing anywhere in `src/engine/` applies a
mass conversion — the only `453592` in the tree is in the report plugin.

**And the report plugin already knows the right conversion, in two of three
places:**

| reader | comment | conversion | verdict |
|---|---|---|---|
| Outfall Loading, `:1011-1028` | — | `LT_PER_FT3 / 453592` | ✅ |
| Link Pollutant Load, `:2306-2312` | *"stat_total_load is in **ft³ × mg/L**"* | `× 28.317/453592` | ✅ |
| Subcatchment Washoff, `:1496-1502` | *"total_load is in **mg**"* | `/ 453592` | ❌ |
| `Runoff Quality Continuity`, `:760` | — | none | ❌ |

**The link-load comment names the exact unit this audit measured.** The
codebase already contains the correct reading of its own accumulator, three
lines away from the wrong one.

**Recommendation:** match legacy — scale the EMC coefficient by `L_PER_FT3`
at parse, make the washoff path produce mg/ft³ for all three functions, and
apply the mass factor once before booking. Then the two `✅` readers get
their conversion removed, and every ledger term is in header units. **That is
a large, corpus-moving change and it needs its own handoff.**

## 7.4 Three more findings

**Finding 9 — the ponded booking into `qual_final_buildup` is dead.**
`stepSurfaceQuality:2823` accumulates ponded mass with `+=` during the run;
`computeFinalQualityMassBalance:4073` **assigns** the same term with `=` at
the end. The run-time accumulation is discarded every time. It is also in mg
against 4073's user mass, so had it survived it would have corrupted the term.

**Finding 10 — the Runoff Quality Continuity error is vacuous when nothing
builds up.** `DefaultReportPlugin.cpp:779` computes
`err = (total_in > 0.0) ? … : 0.0`. On the EMC deck `total_in` is 0 and
**1 815 717 units of mass leave a system that received none, under a printed
error of `0.000`.** Third instance of this shape in the program, after
`runoff_error()` in F8 and the snow closure gate. `total_out` also carries
`qual_bmp_removal`, which is Finding 8's permanent zero.

**Finding 11 — our vendored legacy diverges from stock EPA 5.2.4 in the
washoff guard.**

```
stock  landuse.c:636   funcType != NO_BUILDUP && buildup == 0.0
ours   landuse.c:633   funcType != NO_BUILDUP && buildup >= 0.0
```

Present since the file was vendored (`03ed283a`). Under `>= 0.0` the guard
fires whenever a buildup function exists, so **the parity control washes off
nothing on any deck with buildup.** **Measured, it is inert on the POW+EXP
deck** — patching it to `== 0.0` and rebuilding (the dylib relink confirmed in
the log, not assumed) left buildup 0.885 and washoff 0.000 unchanged, because
buildup is zero while it is still raining. **So it did not cause Finding 6.**
It is still a corrupted control and every future quality parity number
depends on it.

## 7.5 What Finding 6 actually is

Not units. On the POW+EXP deck legacy washes off **0.000 because its buildup
is zero for the whole time runoff exists** — buildup accrues after the storm,
washoff can only happen during it. Ours washes off 1.369 because our buildup
accrues during the storm too. **That is an ordering difference, and the deck
is simply a bad cross-engine fixture** — its answer is "0 against something",
which cannot calibrate anything.

**Finding 6 should be re-scoped**: a fixture with an antecedent dry period
*before* the storm, so both engines have buildup on the ground when it rains.

## 7.6 Unrelated, seen while tracing

**Five hardcoded `43560.0` in `SWMMEngine.cpp`** (2742, 2790, 2874, 4422,
4943) converting `subcatches.area` to ft². `area` is in **user** units, so
this is wrong by 43561.596/43560 under US and **entirely wrong under SI**,
where the field is hectares. `F9` fixed exactly this shape in `Snow.cpp` with
`/ ucf::UCF(ucf::LANDAREA, opts)`. Two of the five sit in the quality path
under audit here.

## 7.7 Why nothing was changed

§4's order is audit, then decide, then fix. The audit says the decision in
§4.3 is bigger than §4.3 assumed: it is not "our terms are uniformly mg, so
convert once" but "three of nine terms are right, five are in two different
wrong units, and one is a sum across three unit systems." Any partial fix
moves the numbers Findings 6, 7 and 10 are measured against.

**§4.4 also has to wait**, though it looks self-contained: writing
`qual_bmp_removal` requires knowing which units to write it in, and that is
the open question.

**§4.5's cross-seam gate is the one thing that could land now** — the summary
system total and the ledger's runoff-load row are the same mass by
construction, and today they differ by 453592×. A gate on that equality
would have caught all of this, and it does not depend on which side is fixed
first.

## 7.8 Suggested order for the next round

1. **The cross-seam gate first** (§4.5), asserting summary-total == ledger-row
   after conversion. It fails today; it is the acceptance test for everything
   below.
2. **`L_PER_FT3` on the EMC coefficient at parse**, matching `landuse.c:334`.
3. **Mass factor at source**, then strip the two `✅` conversions in the report
   plugin.
4. **Finding 9** (the dead `+=`) and **Finding 10** (the vacuous error), both
   small and both independent of the units decision.
5. **Finding 11** — restore `== 0.0` in the vendored legacy, on its own, and
   re-measure every quality parity number that was taken against it.
6. **Finding 6 re-scoped** with an antecedent-dry-period fixture.
7. **Finding 8** last, once the units are settled.

Expect the corpus to move on `force_legacy`, `force_ard`, `orif_legacy` and
`sdm_struct_dw_ard` at step 2 or 3.
