# The quality ledger converts at source — Handoff (2026-08-23)

**For:** the checking agent.
**Base:** the commit after `55a70839` (their audit round; HEAD when I wrote
this). **Standing findings:** lessons 1–151.
**Fixes:** Findings **5, 8, 9, 10, 11** and the EXPON/EMC formulation gaps the
audit exposed. **Parity round**: every change moves us TO legacy's design,
none away from it. Finding 6 needs no fix (legacy's 0.000 was correct for
that deck); Finding 7 stays blocked on issue #131.

---

## 1. The design, which is legacy's

**One internal convention, one conversion, applied at the booking seam.**

- `total_washoff_load` accumulates in **concentration mass units per second**
  (mg/s for a mg/L pollutant) — every contributor now agrees: EMC gains its
  missing `LperFT3` (legacy pre-multiplies at parse, `landuse.c:334`), EXPON
  gains `/3600` (parse-time, `:332`) and `/mcf` on its user-mass buildup
  (runtime, `:641`), RATING was already equivalent and is untouched.
- **`mcf_p`** mirrors `landuse.c:167-169` — `UCF(MASS)` for mg, `/1000` for
  µg, `1.0` for counts — and is applied **once, at every ledger booking**:
  `runoff_load`, `total_load`, `wet_deposition`, `infil_loss`, and the newly
  written `bmp_removal`. All ledger terms are now **user mass (lbs/kg)**,
  which is what the buildup family (`init/surface/sweeping/final`) already
  was — the audit's "three right" stay untouched.
- The **Washoff Summary prints raw** (its `/453592` deleted); the ledger row
  already printed raw. **They now agree by construction**, which is the
  whole point.
- `conc = load / q / LperFT3` → **mg/L**, legacy's `newQual = cOut/LperFT3`
  (`surfqual.c:370`). For EMC this is value-preserving (the old form skipped
  `LperFT3` in both load and conc, cancelling); for EXPON and RATING it is a
  real correction.

On the audit's known-mass deck the booking becomes
`100 × 18157.174 × 28.317 × 2.203e-6 ≈ 113.35 lbs` against legacy's
**113.082** — the 0.24 % being the runoff-volume residual the audit already
priced.

## 2. Everything in the changeset

| finding | change |
|---|---|
| 5 + audit | washoff switch normalised; `mcf_p` at five booking sites; summary prints raw |
| formulation | EMC uses **internal** flow (legacy has no `UCF(FLOW)` there — the old `q_flow` form was inert under CFS and wrong under any other flow unit); EXPON `/3600`; **the buildup cap now compares like with like** (it compared an mg-based load against a lbs cap before) |
| 8 | `qual_bmp_removal` written: `load × effic/100`, booked × dt × mcf, then subtracted — legacy `surfqual.c:352` |
| 9 | the dead `+=` into `qual_final_buildup` **removed** (overwritten by `=` at end-of-run every run; and mg against that site's lbs). Residual ponded mass at final time is in neither path — recorded as a small parity gap vs legacy's `FINAL_STORED_LOAD` |
| 10 | continuity error gets legacy's **three branches** (`massbal.c:900-911`): near-equal → 0, else `/in`, else **`/out`** — the missing branch is why 1.8M units leaving an empty system printed 0.000 |
| 11 | vendored `landuse.c:633` restored to stock `== 0.0`. Under `>=` any land use with a buildup function washed off nothing ever — **the parity reference itself was corrupted**. Measured inert on the EMC deck; NOT inert on any buildup deck |
| LID run-on | `w_lid_runon` gains `× LperFT3` (conc_old is mg/L; legacy findLidLoads applies the same factor) |

```
mod: src/engine/core/SWMMEngine.cpp              (washoff block, ~6 hunks)
mod: src/engine/core/openswmm_massbalance_impl.cpp (three-branch error + <cmath>)
mod: src/engine/plugins/DefaultReportPlugin.cpp  (summary prints raw)
mod: src/legacy/engine/landuse.c                 (>= → ==, stock EPA)
mod: tests/unit/engine/test_massbalance.cpp      (+1 gate)
```

## 3. The gate — the audit's acceptance test

`SummaryAndLedgerAgreeAndCarryTheKnownMass`: the known-mass EMC deck, then
**both printed numbers out of the finished `.rpt`** — not the context,
because the defect lived between the accumulator and the printer where a
context gate cannot see. Three assertions: ledger == summary (the seam);
ledger == `C·V·LperFT3·UCF(MASS)` (the truth — agreement alone would pass two
numbers that are wrong together); summary == same.

**A defect caught in my own gate before shipping, worth flagging:**
`"Surface Runoff ..........."` appears in **both** the water-quantity and
quality blocks, and a bare `find()` lands on the water one — acre-feet
compared against pounds. The gate anchors on `"Runoff Quality Continuity"`
first. Falsifier v checks the anchor is load-bearing.

## 4. Blast radius

- **`.rpt`**: quality blocks change on every deck with pollutants —
  ledger rows shrink by ~16057× (to true pounds), summary rows grow by
  ~28.3×, `BMP Removal` becomes nonzero where decks use `bmp_effic`,
  continuity error becomes real.
- **`.out`**: **EMC-only decks must be byte-identical** (conc is
  value-preserved). **EXPON and RATING decks move** — their routed
  concentrations were in wrong units (EXPON also 3600× hot before the cap
  clamped it). `force_ard`/`force_legacy` corpus decks have pollutants but
  **no [WASHOFF]** — their washoff loop books nothing, so **18/18 `.out`
  expected identical**; their `.rpt` quality blocks may move on wet-deposition
  terms if `Crain` is nonzero (it is 0.0 on both — check).
- **Legacy side**: Finding 11's fix changes legacy results on **any deck
  with a buildup function** — that is the reference being repaired, and the
  parity CONTROL for future quality rounds must be rebuilt from it.

## 5. Validation protocol

1. **The gate fails at base** (revert engine hunks, keep the gate). Expect
   the truth leg to report a ratio near **16057**. Quote it.
2. `ctest -j8` ×3 — total stays 160; expect 159/160.
3. **Corpus, matched configs: 18/18 `.out` identical expected** (§4). If a
   transport deck moves, TSS there is inert (no washoff) — investigate, do
   not wave through.
4. **Re-run the audit's known-mass deck.** Ledger and summary should both
   print ≈ 113.35 lbs against legacy's 113.082. **Quote all three.**
5. **The buildup deck with an antecedent dry period** (the audit's Finding-6
   note): with vendored legacy repaired AND our EXPON fixed, ours vs legacy
   is the first meaningful buildup/washoff comparison this program has had.
   **Both sides changed this round — treat it as a fresh measurement, not a
   regression check.**
6. **Falsifier sweep:**

   | falsifier | expected |
   |---|---|
   | i. drop `mcf_p` from the `runoff_load` booking only | gate: seam leg fails (ledger ≠ summary), truth leg fails at ~16057× |
   | ii. drop it from `total_load` only | seam leg fails the other way; summary off by 1/453592 |
   | iii. restore the summary's `/453592` | seam leg fails; **truth leg on the summary** catches it even if someone "fixes" the ledger to match |
   | iv. remove EXPON's `/3600` | EMC gate unaffected — **needs a buildup deck; none is gated.** Owed |
   | v. search the water block instead of anchoring | the gate compares acre-feet to pounds and fails loudly — confirms the anchor is load-bearing |
   | vi. revert vendored `==` to `>=` | our gate unaffected (no buildup fn) — **the legacy control regression has no automated gate.** Owed |

7. **Record:** the base ratio, the three known-mass numbers, step 5's fresh
   buildup comparison, and iv/vi as open gaps.

## 6. Known gaps

- **EXPON and RATING have no known-mass gate** (falsifier iv) — the gate
  covers EMC, the path the audit measured. A buildup deck with a dry period
  gates both EXPON's `/3600` and Finding 11's legacy restore; it needs the
  fresh cross-engine measurement of §5.5 first.
- **µg and counts pollutants are unexercised** — `mcf_p`'s `/1000` and `1.0`
  branches are read from legacy, not measured.
- **The five hardcoded `43560.0`** the audit flagged (two in this quality
  path) are NOT fixed here — wrong under SI, the F9 shape, and register O6's
  SI deck is what makes them observable. Scoped out to keep this round one
  thing.
- **Residual ponded mass at final time** is in no ledger term (Finding 9's
  removal did not add it to the 4073 computation) — small parity gap vs
  legacy's `FINAL_STORED_LOAD`, recorded.
- **The Washoff Summary header still prints "lbs" under SI** — it would be kg.
  O6 family.

## 7. Prepared commit message

```
fix(quality): the ledger converts to user mass at source, as legacy does

The washoff accumulator mixed three unit systems -- EMC booked mg/L·ft³,
EXPON user-mass (and 3600x hot: the per-hour coefficient was used per
second), RATING mg -- and the two printed readings of the same variable
disagreed by 453592x: the continuity ledger printed it raw while the Washoff
Summary divided by MG_TO_LBS. On a known-mass deck (EMC 100 mg/L,
V = 18157 ft³) the ledger printed 1,815,717 against legacy's 113.082 and the
summary printed 4.003.

One convention now: the accumulator is concentration mass units per second
(EMC gains LperFT3, EXPON gains /3600 and /mcf on its buildup, RATING was
already equivalent), and mcf -- legacy landuse.c:167-169 -- is applied once
at every ledger booking, exactly as legacy applies Pollut[].mcf at source.
The summary prints raw and agrees with the ledger by construction. The
buildup cap now compares like with like; reported concentration is
load/q/LperFT3 = mg/L (surfqual.c:370), value-preserving for EMC.

Also: qual_bmp_removal gets its first writer (surfqual.c:352 -- the row was
rendered from a variable nothing filled); the dead += into qual_final_buildup
is removed (overwritten by = every run, and in the wrong units); the quality
continuity error gains legacy's third branch (massbal.c:908-911) so mass
leaving an empty system no longer prints 0.000; and the vendored legacy
landuse.c:633 is restored to stock EPA == 0.0 -- under >= any land use with a
buildup function washed off nothing, which corrupted the parity reference
every quality comparison rides on.

The gate reads both printed numbers out of the finished .rpt on a known-mass
deck and asserts they agree AND carry C·V·LperFT3·UCF(MASS) -- agreement
alone would pass two numbers that are wrong together.
```

---

# 8. Validation results (2026-08-23) — PASSED after two repairs

**Base:** `55a70839` (the audit round committed nothing, so HEAD had not
moved). **Committed `5b21f9a6`.** Artefacts:
`tests/output/qual_units_fix_2026-08-23/`.

**The units design is right and the numbers land on legacy.** Two things had
to be added: **Finding 10 was fixed at the API and not at the printed row the
audit measured**, and fixing that alone made parity *worse* until legacy's
`BUILDUP_LOAD` fallback came with it. One existing gate also had to move, and
it was encoding a formula neither legacy nor its own siblings used.

## 8.1 §5.1 — the gate at base

```
ledger 1278280.181 lbs against C·V = 79.742122903 lbs (ratio 16030.174949690469)
the ledger row (1278280.181) and the Washoff Summary (2.818) ... disagree
```

**16030.2**, against §5.1's predicted "near 16057". All legs fail.

## 8.2 ⛔ Finding 10 was fixed in the wrong place

`swmm_get_quality_continuity_error` got the three branches. **The printed
`Runoff Quality Continuity → Continuity Error` does not use that function** —
`DefaultReportPlugin.cpp:779` computes it inline, and the changeset left it
on the two-branch form. The known-mass deck still printed **0.000** with
Surface Buildup 0.000 against Surface Runoff 113.269.

That is the exact symptom Finding 10 was raised on, still present after its
fix. **Repaired**: the same three branches at the printed site.

**And that repair alone made parity worse.** With the error live, our EMC
deck printed **−100.000 %** where legacy prints 0.000 — because legacy books
washoff as `BUILDUP_LOAD` when a land use has no buildup function
(`landuse.c:585-593`, *"otherwise add washoff to buildup mass balance totals
so that things will balance"*), and we did not. **Second repair**: mirror it.
The two are one change; either alone is a regression.

Measured after both, against legacy on the known-mass deck:

| | Surface Buildup | Surface Runoff | Error |
|---|---|---|---|
| ours | **113.269** | **113.269** | **0.000** |
| legacy | 113.082 | 113.082 | 0.000 |

Falsifier vii (below) is the proof both halves are load-bearing.

## 8.3 ⛔ An existing gate was encoding a non-legacy formula

`MassBalanceApiTest.QualityKnownImbalanceMatchesExpected` broke on the API
change — **158/160** on the first three ctest runs, which §5.2 did not
predict. It was not a regression:

```
legacy massbal.c:888-898   totalInflow  includes initStorage
                            totalOutflow includes finalStorage
ours   runoff_error()      SimulationContext.hpp:1178 — same
       routing_error()     :1190 — same
       quality (old)       divided by inflow ALONE while subtracting final
                            storage in the numerator
```

The quality error was the only one of the three that followed neither legacy
nor its siblings. `expected` moved from `2.0/100.0` to `2.0/105.0`
(in = 10+30+40+20+5, out = 68+20+10+5) with the derivation recorded at the
gate.

## 8.4 §5.2 — ctest

**159 of 161**, three consecutive runs. The total is **161, not 160**: another
session added `tests/unit/engine/test_lard_wiring.cpp` (untracked) mid-round.

Both failures are other sessions' in-flight untracked files:
- `test_engine_2d_infil_integration` — the standing 2D mesh-budget failure.
- `test_engine_lard_wiring` — **3 of its 5 tests fail identically at base**,
  measured, and its decks contain **zero** `[WASHOFF]`/`[BUILDUP]`/
  `[LANDUSES]`/`[COVERAGES]` sections, so nothing here can reach them.

## 8.5 ⛔ §5.3 — four decks moved, not eighteen identical

```
14/18 identical, 4 moved:
  sdm_fv_o1  sdm_fv_o2  sdm_fv_o2_superbee  sdm_struct_dw_ard
```

**§4's "18/18 expected" checked three of the four pollutant decks and missed
the one that has washoff.** `force_legacy`, `force_ard` and `orif_legacy`
have `[POLLUTANTS]` and no `[WASHOFF]` as §4 said — but all four **SDM**
decks carry `EXP` washoff on three land uses and `RC` on a fourth, which is
precisely what this changeset corrects. They were never going to hold.

**Attributed at the column level, not the byte level.** Reading both `.out`
files variable by variable:

| | worst relative difference |
|---|---|
| subcatchment rainfall / evap / infil / runoff / gw / soil | **0.0000e+00** |
| node depth / head / volume / lat_inflow / tot_inflow / flooding | **0.0000e+00** |
| link flow / depth / velocity / volume / capacity | **0.0000e+00** |
| **pollutant column (all three object types)** | **1.0e+00 — MOVED** |

Every hydraulic column bit-identical; only TSS moved. A quality-units change
that touched water would be the finding, and it did not.

**And the movement is toward legacy.** On `sdm_struct_dw_ard`:

| | washoff ledger | routed link load |
|---|---|---|
| base | 1286.744 | 4052 |
| **patched** | **949.071** | **7443** |
| legacy | 907.284 | 7778 |

41.8 % → **4.6 %**, and 47.9 % → **4.3 %**. The `.out` concentration rise
(link TSS max 0.83 → 577.6 mg/L) is the buildup cap releasing: with an mg
load compared against a lbs cap, EXPON/RATING washoff was clamped ~453 000×
too tight, and the clamp was what made the old number *look* plausible.

The corpus was re-run after every subsequent edit, and once more with **both
sides rebuilt from the current tree** after another session's LARD work
landed mid-round — identical byte counts all three times. The config guard
(`84984990`) was silent each time.

## 8.6 §5.4 — the known-mass deck

| | ledger | summary (system) | SA | SB |
|---|---|---|---|---|
| base | 1 815 717.383 | 4.003 | 2.818 | 1.185 |
| **patched** | **113.269** | **113.269** | **79.742** | **33.527** |
| legacy | 113.082 | 113.082 | 79.647 | 33.435 |

Ledger and summary **identical to the digit** — the seam — and both within
**0.17 %** of legacy, which is the runoff-volume residual the audit priced.

## 8.7 §5.5 — the first meaningful buildup comparison

POW buildup + EXP washoff with `DRY_DAYS 5`, so both engines have mass on the
ground when it rains. Four cells, because both sides changed this round:

| | Surface Buildup | **Surface Runoff** | Remaining |
|---|---|---|---|
| legacy **as vendored** (`>=`) | 0.885 | **0.000** | 50.885 |
| legacy **stock** (`==`) | 0.885 | **19.193** | 31.692 |
| ours **base** | 2.500 | **51.369** | 1.131 |
| **ours patched** | 2.500 | **19.403** | 33.097 |

**167 % out → 1.1 % out.** Both ledgers close (50.885 and 52.500 either side).

**Finding 11 is not inert here** — the vendored control washed off **nothing**
where stock washes off 19.193. That answers falsifier vi by measurement: the
corruption mattered on exactly the deck shape every real quality model has,
and the earlier "inert" reading came from an EMC deck with no buildup
function.

The residual: our Surface Buildup accrues 2.500 during the storm against
legacy's 0.885, because legacy does not accrue while it is raining. **That is
the next quality divergence and it is not a units problem.**

## 8.8 §5.6 — falsifier sweep

| falsifier | expected | measured |
|---|---|---|
| **i.** `mcf_p` off `runoff_load` | seam + truth fail | ledger 36 197 059.9 vs summary 79.742; truth ratio **453 926.5** ✓ |
| **ii.** `mcf_p` off `total_load` | seam fails the other way | summary 36 197 059.9 vs ledger 79.742 ✓ |
| **iii.** restore the summary's `/453592` | seam fails; truth catches it | summary prints **0** ✓ |
| **iv.** EXPON's `/3600` removed | EMC gate unaffected — *owed* | gate passes; **buildup deck 19.403 → 51.369** against legacy's 19.193 — **now measured** |
| **v.** search the water block | fails loudly | ledger reads **0.293 acre-feet** against 79.742 lbs; all three legs fail ✓ |
| **vi.** revert vendored `==` → `>=` | no automated gate — *owed* | **measured in §8.7**: legacy washoff 19.193 → 0.000 |
| **vii.** *(new)* drop the `BUILDUP_LOAD` fallback, keep the report fix | — | printed error **−100.000 %**; the seam legs still pass — which is why leg 3 was added |

**i's ratio is 453 926, not §5's predicted 16057** — dropping `mcf` leaves the
booking in mg, not in `mg/L·ft³`; only reverting the EMC branch as well gives
16 057. Both fail loudly; the prediction's arithmetic was one factor out.

## 8.9 Deviations

1. **The printed continuity error given legacy's three branches** (§8.2) —
   required; Finding 10's fix did not reach the row it was raised on.
2. **Legacy's `BUILDUP_LOAD` fallback mirrored** (§8.2) — required; without it
   repair 1 is a parity regression.
3. **`QualityKnownImbalanceMatchesExpected` moved to `2.0/105.0`** (§8.3).
4. **A third gate leg added** — the buildup row must balance the ledger row,
   which is what falsifier vii showed nothing covered.
5. **The committed `SWMMEngine.cpp` blob was built from HEAD** plus the six
   `stepSurfaceQuality` hunks only; the other six hunks in that file belong to
   another session (`has_subcatchments`, the 2D refresh calls, the temperature
   default, LARD wiring). Built and run on its own — **15/15** — before
   committing.

## 8.10 Still owed

- **EXPON/RATING have no automated gate** (§6's first item). §8.7's deck is
  the fixture; both sides having moved, it is now a measurement worth
  freezing — but freezing it needs the buildup-accrual divergence resolved
  first, or the gate pins a number we already know is wrong by 2.8×.
- **The buildup-accrual difference** (§8.7): ours accrues during rain, legacy
  does not. New, and the largest remaining quality divergence.
- **`qual_routing_seep` is never added to `total_out`** in the quality
  continuity error, though legacy's `totalOutflow` includes `seepLoss`
  (`massbal.c:897`). Seen while fixing §8.3; not touched.
- **`runoff_error()` and `routing_error()` still have the two-branch form** —
  Finding 10's shape survives in both siblings.
- µg and counts `mcf_p` branches unexercised; the five hardcoded `43560.0`;
  residual ponded mass in no ledger term; the SI "lbs" header — all as §6
  recorded.
- **No corpus deck exercises EMC washoff**, only EXP and RC. The path this
  round measured most precisely is the one the standing sweep cannot see.

# 9. Commit

`5b21f9a6` — `fix(quality): the ledger converts to user mass at source, as
legacy does`, on parent `55a70839`. Five files:
`SWMMEngine.cpp` (+140 −23), `openswmm_massbalance_impl.cpp` (+18 −2),
`DefaultReportPlugin.cpp` (+24 −6), `landuse.c` (+8 −1),
`test_massbalance.cpp` (+186 −1).
