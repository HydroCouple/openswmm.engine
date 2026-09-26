# X5 Validation Handoff — Water-Age Source-Table C API (A6-min) + X6's Owed API-Flux Gate

**Date:** 2026-08-23 · **Author:** implementing agent (syntax-only; nothing
executed) · **Step:** subplan X5 = water-age plan A6's GUI-facing subset
(TRANSPORT_QUALITY_GUI_PLAN §6 prereq 5), plus the API-flux extraction gate
X6 §5.vi deferred to this round · **Base:** `d79c8bcf` (X6).

**This is the last engine round of the subplan.** After it, remaining work
is the GUI track Y1–Y4 in `openswmm.gui`.

---

## 0. Hunk-presence check

| grep (repo root) | expected |
|---|---|
| `grep -c "swmm_water_age_" include/openswmm/engine/openswmm_water_age.h` | **9** |
| `grep -c "SWMM_ENGINE_API int swmm_water_age_" src/engine/core/openswmm_water_age_impl.cpp` | **8** |
| `grep -c "^TEST(" tests/unit/engine/test_water_age_api.cpp` | **5** |
| `grep -c "water_age_api" tests/unit/engine/CMakeLists.txt` | **1** |

**⚠ NEW SOURCE FILE — reconfigure required.** `src/engine/CMakeLists.txt`
globs without `CONFIGURE_DEPENDS` (IO1 carried obligation c), so
`openswmm_water_age_impl.cpp` will NOT compile until CMake re-runs. If the
new symbols come back undefined at link time, that is this, not a code
defect: reconfigure and rebuild before diagnosing anything else.

## 1. Changeset

| File | Change |
|---|---|
| `include/openswmm/engine/openswmm_water_age.h` | **NEW.** 8 entry points: `get_enabled`, global get/set, override count/get/set/remove, `save`. `SWMM_WaterAgeSource` enum mirrors the engine's storage order |
| `src/engine/core/openswmm_water_age_impl.cpp` | **NEW.** HOURS at the boundary / SECONDS inside (no ucf — hours are unit-system-invariant); negatives pass through unfloored (D-NS1); the parser's A1a NODE-scope rule enforced identically; `save` writes only non-zero rows and resolves node indices to NAMES |
| `tests/unit/engine/test_water_age_api.cpp` | **NEW** — 5 gates, prefix `_wa_` |
| `tests/unit/engine/CMakeLists.txt` | registered |

## 2. Design decisions (challenge in this order)

1. **Hours across the boundary, seconds inside** — the config file speaks
   hours, so an editor round-trips the user's own numbers. No unit-system
   conversion is correct here (a `ucf` call would be a defect).
2. **The API enforces the PARSER's scope rule** (DWF/EXTERNAL_INFLOW only
   for NODE overrides) — an editor must not be able to author a table the
   file parser refuses. Gate 2 is that claim.
3. **Negatives pass through unfloored** (D-NS1): clamping lives at
   consumption where the held age is known.
4. **`save` skips zero rows** so a save-as of an untouched model does not
   come back looking configured (the A1a save-as family, inverted).
5. **Node NAMES in the saved file, not indices** — indices are not stable
   across edits; the parser resolves names. Gate 3 round-trips through the
   real parser rather than string-matching the file.
6. **No lifecycle guard beyond the handle check** — editors work in OPENED,
   and the loaders re-read the table each step, so mid-run edits are
   legitimate. If review wants OPENED-only, add `CHECK_EDITABLE` AND a
   gate leg.

## 3. Anticipated failure modes, likelihood order

1. ⚠ **The reconfigure (§0).** Undefined symbols = CMake didn't re-glob.
2. ⚠ **Gate 3's node-name round-trip** depends on `node_names.name_of`
   returning the parse-time name and the parser resolving it back to index
   0. If `nd` comes back −1, check whether override resolution happens at
   apply time (it does in A1a) — the assertion is on the resolved index.
3. **Gate 4's ±60 s band** mirrors X4's gate 5 (same claim through the API).
   If it drifts, sweep rs {1,2,5} per that round's rule before touching it.
4. **Gate 5's moderate leg** (`-50` flux) uses the same mg/s convention X6
   measured (÷28.3168 on the `[INFLOWS]` path). The API flux does NOT go
   through that conversion — it is a raw internal rate — so `-50` may be a
   much larger effect here than the same number on a deck row. The gate
   only asserts direction (`mod < base*0.999`), so it holds either way; if
   `mod` comes back at zero concentration, reduce the magnitude and record
   the measured internal-unit scale.
5. **Gate 5's `EXPECT_LT(big, mod)`** could tie at exactly 0.0 if both
   fluxes fully strip the node. If so, lower the moderate leg rather than
   dropping the assertion.
6. **`swmm_water_age_get_override` with all-null out-pointers** returns
   BADINDEX on an empty table (gate 2 relies on that ordering: index
   validation precedes the null writes).

## 4. Gates

W1 CrudRoundTripsInHours · W2 RejectsBadArgumentsAndOutOfScopeOverrides ·
W3 SavedFileParsesBackToTheSameTable · W4 ApiEditShiftsTheSimulatedAge ·
W5 NegativeApiMassFluxExtractsAndClamps (**X6 §5.vi's owed observer**).

## 5. Falsifier sweep

| # | Falsifier | Must fail |
|---|---|---|
| i | `set_global_source` floors negatives at 0 | W1 (negative leg), W3 (the −2 h row) |
| ii | `set_override` always appends (no find/update) | W1 (count == 3 after the update) |
| iii | drop the `node_scoped` check | W2 (GW override accepted) |
| iv | `save` writes seconds instead of hours | W3 (values come back ×3600) |
| v | `save` writes node INDEX instead of name | W3 (parse error or wrong node) |
| vi | `save` writes zero rows too | W3's "untouched sources stay absent" leg |
| vii | **restore `w <= 0` in `addExtInflowLoads`** (X6 §5.vi, now observable) | W5 (moderate leg: `mod == base`) — **this is the row X6 could not run; confirm it bites and cite the number in X6's §9** |
| viii | drop the negative-API warning | W5 (`warned` leg) |
| ix | `get_global_source` ignores its out-pointer null check | W2 (crash instead of BADPARAM — run under ASan) |

## 6. Standing verification

Full suite isolated worktree (**every prior suite must be untouched — this
round is purely additive to the engine's behavior; the only shared file is
CMakeLists**). Corpus **19/19** — no deck exercises the API. ASan/UBSan
over the new suite + `test_engine_water_age` + `test_engine_negative_sources`.
Zero new warnings. **Confirm the new TU actually compiled** (`nm` the
library for `swmm_water_age_save`, or trust the link — the gates cannot
pass otherwise).

## 7. Not claimed / owed

Python + MCP age surfaces (full A6) · node/link age STATE getters (the GUI
reads age through the `.out` descriptors, not this header) · heat's
equivalent (`openswmm_heat.h`, GUI plan prereq 5's other half — owed when
G4g is scheduled) · reaction APIs (R5) · the `[TRANSPORT_SOURCES]`
negative-row scope from X6 §2.5 · the X2.viii routing-step instrument ·
X4.vii's dry-hotstart gate.

## 8. On acceptance

Commit; subplan X5 row → ✅ and note the engine track COMPLETE; update
X6's §9 with falsifier vii's measured result (its owed row); roadmap A6 →
partial (GUI subset landed, Python/MCP outstanding); record lessons;
report gates/falsifiers/counts. **Then the remaining subplan work is the
GUI track Y1–Y4 in `openswmm.gui`** — Y1/Y2 are unblocked today, Y3 is
unblocked by this round.

---

## 9. Validation results (2026-08-23, validating agent)

**Committed `d7b6c079`** on `d79c8bcf`, branch `swmm6_rel`. Four files, all
new but the one-line CMake registration; no shared-file complications.
All four §0 greps passed; §0's reconfigure warning heeded (explicit cmake
re-run before building; `nm` shows all 8 symbols; the corpus BASE build
deliberately skipped the reconfigure so its stale glob excludes the new
TU — the IO1 gotcha used as the base-binary mechanism).

**All five gates passed on the first run** — the only implementation round
of the subplan to do so. One gate leg was strengthened from a falsifier
finding rather than a failure:

- **Falsifier vi was invisible to W3 as authored**: a written zero row
  (`GW GLOBAL 0`) parses back to the same 0.0 as absence — the value
  check CANNOT see it, by construction. Absence is a property of the
  FILE, so that one leg now reads `_wa_saved.age` and asserts no
  untouched source appears at all. vi now bites. (The §2.5 rule —
  round-trip through the parser, not string matching — holds for every
  VALUE claim; the absence claim is the one legitimately textual leg.)

### Falsifier sweep — 9/9 bite

i (W1 negative leg + W3), ii (W1 count), iii (W2 scope), iv (W3 ×3600),
v (W3 parse), vi (after the absence leg), **vii — X6 §5.vi's owed row,
CONFIRMED**: restoring `w <= 0` fails FOUR W5 legs (`mod == base ==
100.0` exactly, no warning, zero clamps, no monotonicity) — cited into
X6's §9 as its addendum. viii (W5 warning leg), ix under ASan exactly as
§5 prescribed (SEGV at null in `swmm_water_age_get_global_source` instead
of BADPARAM).

### Standing verification

ctest full ×3: standing `test_engine_2d_infil_integration` only. Corpus
**19/19**. ASan/UBSan clean over water_age_api + water_age +
negative_sources. Zero new warnings. §3's anticipated failures 2–5 did
not materialize (gate 3's name resolution, gate 4's ±60 s, gate 5's
magnitudes all held as designed).

## 10. THE ENGINE TRACK IS COMPLETE

X1 wiring `24602eb2` → X2 transport `8c141a5e` → X3a substepping
`647a3603` → X3b RWPT `b9852cee` → X4 age `9f155227` → X6 D-NS1
`d79c8bcf` → X5 API `d7b6c079`. Remaining subplan work is the GUI track
Y1–Y4 in `openswmm.gui` (Y1/Y2 unblocked previously; **Y3 unblocked by
this round**). Standing engine debts carried: X2.viii rs-instrument,
X4.vii dry-hotstart gate, [TRANSPORT_SOURCES] negative rows, negative
DWF/GW/RDII concs, heat's API half, full A6 (Python/MCP + state getters).
