# X4 Validation Handoff — Water Age under LARD + the A5 Cross-Engine Gate

**Date:** 2026-08-23 · **Author:** implementing agent (syntax-only sandbox;
all touched TUs `-fsyntax-only` clean; **nothing executed**) · **Step:**
subplan X4 (`LARD_AGE_EXPEDITE_SUBPLAN_2026-08-23.md` §3) = strategy §8 +
water-age plan A5 (LARD leg) · **Base:** `8c141a5e` (X2).

**Your job:** §0 hunk check, build, run three suites (`lard_age` new;
`lard_wiring` flipped again; `lard_transport` must stay green untouched),
falsifier sweep, standing verification, fix-within-rules, commit.

---

## 0. Hunk-presence check

| grep (repo root) | expected |
|---|---|
| `grep -c "add_species" src/engine/quality/lard/SegmentStore.hpp` | **1** |
| `grep -c "add_species" src/engine/quality/lard/LagrangianSolver.hpp` | **1** (the call) |
| `grep -c "water_age\|node_age\|link_age" src/engine/quality/lard/LagrangianSolver.hpp` | **24** |
| `grep -c "temperature state" src/engine/core/SWMMEngine.cpp` | **1** (the re-worded warning) |
| `grep -c "heat" tests/unit/engine/test_lard_wiring.cpp` | **12** |
| `grep -c "^TEST(" tests/unit/engine/test_lard_age.cpp` | **5** |
| `grep -c "lard" tests/unit/engine/CMakeLists.txt` | **3** |

## 1. Changeset

| File | Change |
|---|---|
| `src/engine/quality/lard/SegmentStore.hpp` | `add_species(s, delta)` — +dt on every LIVE segment (stale slots never age) |
| `src/engine/quality/lard/LagrangianSolver.hpp` | Age = species row `np`: AGE phase (+dt on segments AND node stores **before** transport — the routeLegacyAge "age then mix" convention); drain/mix/release/passthrough unified over `ns = np + age`; age state reads/writes `water_age_state.node_age/link_age` (seconds), sources from `node_age_vol_in` (rate × dt); **no decay on the age row**; publish = volume-weighted mean, and an **empty slab's held link age ages in place** (+dt) instead of resetting — the A2b state/report separation; init seeds from hotstart-loaded state first, else `INITIAL_STATE`, then seeds segments from `link_age` (the lesson-37 profile collapse, recorded) |
| `src/engine/core/SWMMEngine.cpp` | open() warning: `water_age` **dropped from the trigger** (age is live); text now temperature-only (H7), still containing the pinned phrase "does not advance under the LARD engine yet" |
| `tests/unit/engine/test_lard_wiring.cpp` | gate 5 flipped again: heat deck warns (a); **age deck must NOT warn (a2 — new leg)**; IGNORE_QUALITY leg's trigger switched to heat. `DeckSpec.heat` added |
| `tests/unit/engine/test_lard_age.cpp` | **NEW** — 5 gates, prefix `_la_` |
| `tests/unit/engine/CMakeLists.txt` | `add_gtest_unit(test_engine_lard_age …)` |

## 2. Design decisions (challenge in this order)

1. **Age ages BEFORE transport** (+dt, aged value is the step's old state) —
   token-for-token the routeLegacyAge convention, so the A5 comparison is
   between engines, not between conventions.
2. **No max-principle clamp on the age mix** (LEGACY carries `a_max`): the
   LARD mix is a convex combination of already-aged values, bounded by its
   inputs by construction. If a falsifier finds a path where that argument
   fails, add the clamp AND record why the argument was wrong.
3. **No evap factor for age** (plan §8: evaporation leaves mean age
   unchanged) — consistent with X2's omission for pollutants.
4. **Empty-slab link age ages in place** rather than resetting to 0 —
   LEGACY's dry-link behavior, and what the owed dry-hotstart gate expects.
   Consequence: a link that empties and later rewets briefly blends the
   held age with arriving water via the segment seed at refill; recorded.
5. **Seeding order: hotstart beats INITIAL_STATE** (the ARD precedent,
   `hotstart_loaded`).
6. **`node_age_vol_in` is consumed as rate × dt** with the SAME denominator
   as pollutants — one mixing formula, one seam (D-UT10). The LID-drain
   age channel needs nothing here: the loader folds it into
   `node_age_vol_in` (QualityRouting.cpp:405-413).

## 3. Anticipated failure modes, likelihood order

1. ⚠ **Gate 2 (A5) 5% band.** Three engines, three transport definitions.
   The A1b-recorded offsets were seconds against hours, so 5% should be
   generous. If it fails: print all three ages AND the measured τ; decide
   which engine is the outlier before touching the band; a LARD outlier is
   a defect, a LEGACY outlier may be the CSTR's known dispersion — record
   which. Band widens only to a measured floor ×3, never past 10%.
2. ⚠ **Gate 4's front-arrival margin.** Estimated travel ~13 min at 5 cfs;
   the run stops at 5 min. If the fresh front arrives early (deck
   hydraulics differ from the estimate), the age dips below band — verify
   with the LEGACY control (add one run if needed) and shorten the run,
   don't widen the band.
3. **Gate 5's ±60 s shift band.** A1a saw exactness at rs ≤ 2; rs = 5 here.
   If off, sweep rs {1,2,5} — exact at small rs and drifting with rs is
   quadrature (accept measured floor ×3); flat offset is a seam defect.
4. **Gate 1's 10% τ band** — junction blending biases the outfall age high
   slightly; 10% should absorb it.
5. **`ws.resize` wiping subarea state:** init only resizes on size
   mismatch, mirroring routeLegacyAge; if a gate sees zeroed subcatchment
   ages under LARD on a watershed deck (none in this round), that is the
   A3 3-arg-resize family — check the call.
6. **Transport suite must be untouched-green** — the ns-widening must be
   inert at `WATER_AGE OFF` (`ns == np`). Any `lard_transport` movement is
   a real finding.

## 4. Falsifier sweep

| # | Falsifier | Must fail |
|---|---|---|
| i | remove the AGE phase (`add_species` + node `+= dt`) | A1, A4 (ages read ~0 / seed-only) |
| ii | age AFTER transport instead of before | A5 cross-check (convention split) or A4's exactness band; if neither, record for the dt-instrument |
| iii | drop `node_age_vol_in` from the mix (`m_ext = 0` for the age row) | A5 shift gate (gate 5 — shift reads ~0) |
| iv | seed segments at 0 instead of `link_age` | A4 (outfall reads ~300, not 3900) |
| v | decay the age row like a pollutant (use a nonzero k deck probe) | A3 (bitwise) stays green — probe A1 with kdecay>0 added: age must NOT drop |
| vi | let the age row share the pollutant stride (`ns`-vs-`np` index swap in mix) | A3 (TSS moves when age turns on) — the A2b conflation family, THE load-bearing falsifier of this round |
| vii | reset empty-slab link age to 0 | no current gate — **expected empty**; the owed dry-hotstart gate is its observer, note it in §7 |
| viii | drop the `heat`-only warning re-scope (warn on age again) | W5(a2) |

## 5. Standing verification

Full suite isolated worktree; corpus 18/18 (nothing here touches LEGACY/ARD
paths; the warning re-word is LAGRANGIAN-gated); ASan/UBSan over the three
LARD suites + `test_engine_water_age`; zero new warnings.

**Corpus obligation (subplan §3):** with X4 landed, the `age_lard` parity
deck — `age_ard` with one line changed to `QUALITY_SOLVER LAGRANGIAN` — is
now buildable. Add it if the age decks from
`CORPUS_AGE_HEAT_DECKS_HANDOFF_2026-08-22.md` have landed by your round;
otherwise record the dependency.

## 6. What this round does NOT claim

Heat under LARD (H7), subcatchment/LID age interplay beyond what the shared
loaders deliver (A3/A4 feed `node_age_vol_in`; nothing LARD-specific),
age hotstart SAVE fidelity (A2a saves from `node_age/link_age` — LARD
publishes both, so saves should work; not gated this round), the dry-link
hotstart gate (still owed program-wide), `.out` age column (engine-side
A2b machinery reads `water_age_state` — should light up unchanged; gate it
if cheap: one `.out` read on the A5 deck asserting a nonzero trailing
column would close the loop).

## 7. On acceptance

Commit; roadmap A5 row → LARD leg ✅ (ARD leg was A1-era) + subplan X4 row;
record lessons; report gates/falsifiers/counts/deviations. Falsifier vii's
empty row carries forward as the dry-hotstart gate's constituency.

---

# 8. Validation results (2026-08-23) — PASSED after one re-scope and one added gate

**Base:** `8c141a5e`; all seven §0 greps passed. **Committed `9f155227`.**
Artefacts: `tests/output/lard_x4_2026-08-23/`.

## 8.1 ⛔ Gate A3's bitwise claim is impossible in this engine — by mechanism, not concession

`PollutantsAreBitIdenticalUnderAgeTracking` failed at the **ULP scale**:
100.00000000000003 vs 100.0000000000001 — a few 1e-13, where the defect it
polices (a stride leak) injects ~3.9e3-second age values or zeros.

**Mechanism, verified at the criterion:** `mergeable()` loops over **every**
species. Consecutive releases differ in age by ~dt = 5 s against a tolerance
of `kMergeRtol × age ≈ 0.36 s`, so turning age on changes which segments
merge — different partitioning, different FP association. Merges conserve
every species' mass, so the deviation is pure re-association: **measured
1e-15 relative**. LEGACY's A3 gate could demand bitwise because a CSTR has no
discretization to couple. The age-blind-merge alternative was considered and
rejected: with no pollutants every segment becomes trivially mergeable and an
age-only deck collapses to one segment per link — killing the round's
headline.

**Re-scoped to a 1e-9 relative band** — six orders above the measured floor,
ten below the defect — with the mechanism in the gate comment. **Falsifier vi
still fails it loudly** (verified), so the load-bearing falsifier keeps its
teeth.

## 8.2 The falsifier table

| # | predicted | observed |
|---|---|---|
| i | A1, A4 | A1 A2 A4 A5-shift — louder ✓ |
| ii | A5/A4, "if neither, record" | **none — recorded**: age-before vs age-after is invisible to steady observables; the X3 dt-refinement instrument's constituency (third entry, after X2's iii/vi/viii) |
| iii | shift gate | `ExternalInflowSourceAgeShiftsTheOutfall` ✓ |
| iv | A4 | `InitialStateSeedsAndAgesExactly` ✓ |
| v | no gate — probe prescribed | probe measured **645.73 → 476.67 s** with TSS identical; **then made a permanent gate** (§8.3) which now fails under v ✓ |
| vi | A3 — THE load-bearing row | `PollutantsAreBitIdentical…` ✓ (with the re-scoped band) |
| vii | expected empty | empty ✓ — the owed dry-hotstart gate's constituency, §7 as written |
| viii | W5(a2) | `BypassWarningFires…` ✓ |

## 8.3 Gate added: `AgeDoesNotDecayWithThePollutant`

§4.v prescribed a manual probe; a defense without a standing observer is the
F8 family, so the probe became gate 6: same single-conduit deck at k=0 and
k=1e-3, TSS must decay (premise ASSERT), outfall age must not move (1 %,
absorbing the §8.1 partition jitter). Measured under the falsifier:
645.73 → 476.67 s, caught.

## 8.4 The corpus obligation — `age_lard` lands, and the age deck becomes a triple

`age_ard` with one line changed (`QUALITY_SOLVER LAGRANGIAN`), verified
identical otherwise; MANIFEST row appended. First run: **18/19, only
`age_lard` moving** (968 of 43398 bytes), attributed by column: **node and
link age only** — subcatchment age (shared loaders) and every hydraulic
column bit-identical. The movement is the feature lighting up against an
X1-era base, and it doubles as §6's suggested `.out`-column observation: the
trailing age column is nonzero and carries structure. **19/19 is the
standing expectation from here.**

## 8.5 Standing verification

- **ctest 162/163 ×3** — the standing 2D-infil failure only.
- **`lard_transport` untouched-green in every falsifier build** (§3.6): the
  ns-widening is inert at `WATER_AGE OFF`.
- **Corpus** §8.4; the config guard silent; base `build/darwin` at its X1
  state (X2+X4 both corpus-inert for the original 18, transitively
  consistent with X2's own 18/18).
- **ASan/UBSan** over the three LARD suites + `test_engine_water_age`:
  clean except the known `HotStartManager.cpp:246`.
- **Zero warnings** from X4 TUs (53 pre-existing summaries elsewhere).

## 8.6 Deviations

1. A3 re-scoped (§8.1) — measured mechanism, teeth verified.
2. Gate 6 added (§8.3).
3. The committed `SWMMEngine.cpp` is HEAD + X4's one hunk (the warning
   re-scope); the 2D session's two uncommitted hunks excluded; clean blob
   built and run alone (17/17 across the three suites).

## 8.7 Owed, accumulating

- **X3's dt-refinement instrument** now carries four constituents: X2's
  iii/vi/viii and X4's ii (age-ordering).
- The dry-hotstart gate (X4's vii), program-wide.
- Heat under LARD (H7); age hotstart SAVE fidelity un-gated; the A2b `.out`
  read exists only as the corpus attribution, not as a suite gate.

# 9. Commit

`9f155227` — eight files, +959 −58, on parent `8c141a5e`. Roadmap A5 row →
LARD leg ✅ and subplan X4 row updated.
