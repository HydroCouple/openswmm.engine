# H7a — LARD's species-row identity becomes an index — Handoff (2026-08-29)

**For:** the checking agent.
**Base:** `0e73f7ea`.
**Step:** `FINALIZATION_SEQUENCE_2026-08-29.md` step 2, **first of two rounds**.
**Standing findings:** lessons 1–178.

```
mod: src/engine/quality/lard/LagrangianSolver.hpp   (only file)
```

**⚠ This round is meant to change NOTHING observable.** Its claim is
bit-identity. Read §4 before judging that as a weak round — the position is
deliberate and it is stated rather than disguised.

**Syntax-checked, not merely inspected:** `g++ -fsyntax-only -std=c++20`
against the real include tree — **0 errors in `LagrangianSolver.hpp`**. (At
`-std=c++17` the tree fails in `TableData.hpp:733` on heterogeneous
`unordered_map::find`, pre-existing and unrelated.) Nothing was built, linked
or run.

---

## 1. The plan's premise is wrong, and that reshapes the round

`LARD_CLOSEOUT_PLAN_2026-08-24.md` §P2.1 says H7 *"mirrors X4 exactly:
temperature as a second reserved species row on the segments, sourced from
`node_temp_vol_in`."*

**The sourcing half is right** — verified: `node_temp_vol_in` exists
(`HeatData.hpp:236`, a rate in °C·ft³/s) and is the twin of `node_age_vol_in`.

**The "exactly" is wrong, at the one place that matters.** X4 identifies the
age row with a *threshold*:

```cpp
const bool is_age = (s >= np);          // LagrangianSolver.hpp, pre-H7a
```

That is correct **only while age is the single reserved row**. Add
temperature and `s >= np` captures **both** rows — and the consequences are
not a crash:

- temperature would be aged `+dt` every routing step,
- sourced from `node_age_vol_in` instead of `node_temp_vol_in`,
- published into `water_age_state.node_age`.

**A temperature field that quietly behaves like an age field produces
plausible numbers.** That is the worst failure shape this program has, and it
is exactly the D-UT10 index-arithmetic trap the roadmap keeps flagging.

**The ARD engine already solved this** — `ArdEngine.cpp:110-111` carries
explicit `age_row_` and `temp_row_` indices. LARD kept the threshold because
it only ever had one reserved row. So H7 is *X4's mirror plus a discriminator
generalisation*, and the generalisation is the risky part.

## 2. What this round does

One new struct and one function, **the single source of truth for the row
layout**, replacing three independent copies of `np + (age ? 1 : 0)` in
`step`, `substep` and `init`:

```cpp
struct SpeciesRowLayout { int np, age_row, temp_row, ns; };
inline SpeciesRowLayout rowLayout(const SimulationContext& ctx);
```

and the discriminator becomes `s == L.age_row`.

**`temp_row` is always -1 and is read by nothing.** H7b turns it on. The
commented line where it will be assigned is left in `rowLayout` so the
insertion point is unambiguous.

`age` is still available in `substep`/`init`, now **derived** as
`L.age_row >= 0` rather than re-read from options — one question, one answer
per call.

## 3. ⚠ A defect I introduced and caught, worth your attention

My first pass **deleted `const bool age` from all three functions** having
only noticed the `ns` computation. Six other sites still read it and the file
would not have compiled. Caught by grepping for orphaned readers *after*
editing, and by the syntax check.

**`publish()` is a FOURTH site that knows the layout implicitly** — it
declares its own `const bool age` and does not compute `ns`, so it compiles
untouched and I left it alone (CLAUDE.md §3). **H7b will need it**: publish
is where the temperature row would reach `links.conc` / the heat state, and
it is the one layout-aware function this round did not convert. Flagging it
so H7b does not discover it the hard way.

## 4. Why this round has no gate that fails at base — and why that is right

**Be clear-eyed: with one reserved row, `s == L.age_row` and `s >= np` select
the same row. This round cannot be distinguished from its predecessor by any
behavioural test.** There is no failing-at-base gate, because there is no
behaviour change.

Its observer is **bit-identity**: corpus and the water-age suite must be
unchanged. That is a real and quite strong observer for an index-layout
edit — it says the striping, the sizing and the row selection all still agree
across three functions.

**The reason to spend a round on it** is that H7b changes physics. If the
indexing scheme and the temperature row land together, a moved corpus deck no
longer tells you *which* of the two did it, and the bit-identity check — the
strongest instrument available — is spent at the moment it is least
informative. Splitting buys a trusted layout to build on. Same reasoning as
the L3 5a/5b split in the finalization sequence.

## 5. Validation protocol

1. **Corpus: 20/20 `.out` byte-identical AND 20/20 `.rpt` unchanged.** This is
   the round. **Any movement is a finding and blocks H7b.**
2. `ctest -j8` ×3, with attention to **every water-age gate** —
   `test_water_age.cpp` including `DryElementHotstartCarriesTheAgedState`
   (`:911`), and the LARD suites. They exercise the row this round re-indexes.
3. **A deck with pollutants AND age** must be identical; so must
   **pollutants-only** (`age_row == -1`, `ns == np`) and **age-only**
   (`np == 0`). The `np == 0` case is the one where an off-by-one in
   `rowLayout` would show first, and no corpus deck covers it — **check it
   explicitly**.
4. **Falsifier sweep:**

   | falsifier | expected |
   |---|---|
   | i. revert `s == L.age_row` to `s >= np` | **PASSES.** Report it. This is the honest statement of §4 — today the two are equivalent, and the round's value is what it enables, not what it fixes. **If it FAILS, my equivalence claim is wrong and that is a real finding** |
   | ii. set `temp_row = np + 1` in `rowLayout` without changing `ns` | **nothing changes** — confirms `temp_row` is genuinely unread, so H7b starts from an inert field rather than a half-wired one |
   | iii. swap the reserved order (`temp_row` before `age_row`) while both are assigned | not testable this round (`temp_row` unassigned) — **record as owed for H7b**, where order becomes load-bearing |
   | iv. make `rowLayout` return `ns = np` while age is on | age gates fail loudly (store under-sized). Confirms the three call sites really do read the shared layout rather than a stale local |
   | v. drop the `age` derivation in `substep` only | compile error — confirms the six readers are genuinely fed by it |

5. **Record:** falsifier i's outcome above all, and step 3's `np == 0` answer.

## 6. Known gaps and the H7b decision

- **`publish()` is unconverted** (§3). H7b's first task.
- **The RWPT dispersion question is open and I am not deciding it silently.**
  Does temperature participate in `RwptDispersion`? Water age does. Heat
  physically disperses, but not with the same coefficient as a solute.
  **If H7b defers it, the bypass warning must NARROW to "heat is transported
  but not dispersed" rather than being deleted** — a warning removed for a
  capability that only half-landed is how the next reader gets surprised.
- **No gate asserts `rowLayout`'s ordering contract.** Nothing stops a future
  edit inserting a row between pollutants and age.
- The bypass warning at `SWMMEngine.cpp:341` is **untouched** — correct, since
  heat still does not advance under LARD.

## 7. Prepared commit message

```
refactor(lard): the species-row identity is an index, not a threshold

LARD identifies the water-age row with `s >= np`, which is correct only while
age is the single reserved row. H7 adds temperature as a second one, and under
the threshold both rows would answer to it: temperature would be aged +dt per
step, sourced from node_age_vol_in and published into water_age_state. That
produces plausible numbers rather than a crash, which is the failure shape this
program has been bitten by most.

The layout moves into one place -- SpeciesRowLayout / rowLayout() -- replacing
three independent copies of `np + (age ? 1 : 0)` in step, substep and init, and
the discriminator becomes `s == L.age_row`. This mirrors what the ARD engine
already does with explicit age_row_ / temp_row_ indices; LARD kept the
threshold only because it never had a second reserved row.

temp_row is present, always -1, and read by nothing: the change is inert by
construction, so the corpus and the water-age suite can prove it before H7b
puts physics on top. Landing the indexing scheme and the temperature row
together would spend the bit-identity check at the moment it can no longer say
which of the two moved a deck.

No behaviour change. There is deliberately no gate that fails at base -- see
plans/transport/H7A_LARD_ROW_LAYOUT_HANDOFF_2026-08-29.md sec 4.
```

---

# CHECK RECORD — 2026-08-29

**Verdict: sound; bit-inert as claimed; landed on `swmm6_rel`** (`refactor(lard):
the species-row identity is an index, not a threshold`, one file). All numbers
from an isolated `git worktree` at `a38f0c0b` + this file (lesson 172).

## ⚠ Finding independent of H7a: HEAD `a38f0c0b` does not compile

The `develop` merge (`5150480e` → `a38f0c0b`) left two breaks:
`SWMMEngine.cpp:2058` references `target_sc`, which does not exist in this
branch, and `LID.cpp:1507-1540` defines four `LIDSolver::total*Volume()`
methods that `LID.hpp` never declares. A peer session holds uncommitted
repairs for both in the shared tree (`SWMMEngine.cpp` +5/−1, `LID.hpp`
+19/−1); I applied those two patches to BOTH sides of the A/B so the
comparison is HEAD-as-repaired vs HEAD-as-repaired + H7a. Saved as
`tests/output/h7a_row_layout/foreign_merge_repair_{SWMMEngine,LID}.patch`;
not staged here — they are the peer's to commit. **Until they land, a fresh
checkout of `swmm6_rel` is red.**

The merge also changed the ctest census: **180 tests, 5 failing** —
`water_age_lid.DrainLeavesAtStorageAgeAndReachesTheNode`,
`heat_watershed.EveryRunonContributorKeepsTemperaturesInsideTheSources`,
`heat_lid.ADrainedLayerStillConductsAndIsNotResetByThePolicy`,
`transport_dt_reference.LidColumnTemperatureConvergesUnderRefinement`,
`fv_tpa_closure.SealedSiphonMassConserved` — every one fails identically
against the base dylib, so they are the merge's, not H7a's. (`2d_infil`'s
writer round-trip, red at the previous HEAD, now passes.) The "standing
figure" needs re-baselining once the merge fallout is fixed.

## §5.1 corpus — 20/20 `.out` byte-identical, 0/20 `.rpt` moved

Engine sha256 `52c24385…` (base) vs `58e44c6b…` (patched). Base CLI ran via
a `DYLD_LIBRARY_PATH` wrapper (its `LC_RPATH` points at the build tree).

## §5.2 suites — unchanged

`test_engine_water_age` 22/22, `lard_wiring` 5/5, `lard_age` 6/6,
`lard_transport` 6/6, `lard_rwpt` 5/5, `lard_dt_reference` 5/5 — identical
at base and patched. `ctest -j8` ×3: 175/180 each run, the five above.

## §5.3 the trio — identical, including `np == 0`

| deck | layout | `.out` | `.rpt` |
|---|---|---|---|
| `_la_a5_lard` | pollutants + age | IDENTICAL (29 359 B) | same |
| `_lt_par_lard` | pollutants only (`age_row == -1`) | IDENTICAL (17 614 B) | same |
| `_lw_warn_age` | **age only (`np == 0`)** | IDENTICAL (17 624 B) | same |

**The `np == 0` identity is not vacuous:** falsifier iv moves exactly that
deck (below), so the age row is live on it and the layout is what selects it.

## §5.4 falsifiers

| # | expected | observed |
|---|---|---|
| i. `s == L.age_row` → `s >= np` | PASSES | **PASSES** — 49/49 gates, trio identical. The equivalence claim holds today; the round's value is what it enables ✓ |
| ii. `temp_row = np + 1`, `ns` untouched | nothing changes | **nothing changes** — 49/49, trio identical; `temp_row` is genuinely unread ✓ |
| iii. reserved-order swap | not testable | **owed for H7b** (recorded) |
| iv. `ns = np` with age on | age gates fail loudly | **`LardAgeTest.SteadyOutfallAgeIsTheMeasuredResidenceTime` and `ThreeEnginesAgreeAtTheOutfallNode` FAIL; `_la_a5_lard` and `_lw_warn_age` `.out` MOVE, `_lt_par_lard` identical** — the three sites read the shared layout, and the age-only deck exercises the row ✓ |
| v. drop the derived `age` in `substep` | compile error | **compile error at `:233` and `:352`** ("use of undeclared identifier 'age'") ✓ |

## Notes

- `publish()` (`LagrangianSolver.hpp:438`) is the unconverted 4th site, as
  §3 says; H7b's first task. The RWPT-dispersion question (§6) is
  untouched and stays the user's to decide.
- Shared-tree file `cmp`-equals the isolated one after every restore;
  staged by pathspec, the two foreign repairs excluded.
