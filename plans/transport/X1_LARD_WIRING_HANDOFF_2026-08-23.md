# X1 Validation Handoff — LARD Wiring (`QUALITY_SOLVER LAGRANGIAN` dispatch)

**Date:** 2026-08-23 · **Author:** implementing agent (syntax-checked sandbox —
all six touched TUs pass `g++ -std=c++20 -fsyntax-only`; **nothing has been
compiled against GTest or executed**) · **Step:** subplan X1
(`LARD_AGE_EXPEDITE_SUBPLAN_2026-08-23.md` §3) = strategy Phase 0 trimmed
(`plans/LAGRANGIAN_QUALITY_STRATEGY.md` §12).

**Your job:** build, run the new suite, run the falsifier sweep as a table,
run the standing verification, fix what you find (instructions below on the
two decisions you may need to make), and commit. This changeset is small on
purpose; the risk is concentrated in the two claims marked ⚠ below.

---

## 1. Changeset

| File | Change |
|---|---|
| `src/engine/core/SimulationOptions.hpp` | `QualitySolverKind::LAGRANGIAN = 2` + doc updates |
| `src/engine/input/handlers/OptionsHandler.cpp` | `LAGRANGIAN` \| `LARD` parse to the enum (was: fall to `ext_options`) |
| `src/engine/core/InpWriter.cpp` | writes `QUALITY_SOLVER LAGRANGIAN` (A1a save-as rule, third instance) |
| `src/engine/quality/lard/LagrangianSolver.hpp` | **NEW**, header-only. `step()` zeroes `nodes/links.conc` **and** `conc_old` (lesson 39: the .out old/new interpolation is live) |
| `src/engine/core/SWMMEngine.hpp` | include + `lard::LagrangianSolver lard_;` member |
| `src/engine/core/SWMMEngine.cpp` | open(): bypass warning guarded by **exactly** stepRouting's stage-liveness predicate; stepRouting(): `else if LAGRANGIAN → lard_.step()` inserted before the LEGACY default arm |
| `src/engine/transport/components/EulerianArdComponent/ArdConfig.cpp` | warning wording: "inert under the LEGACY engine" → "inert under the selected engine" (old wording became false on a LARD deck). The substring existing tests assert (`"QUALITY_SOLVER is not EULERIAN_ARD"`) is untouched — verified by grep |
| `tests/unit/engine/test_lard_wiring.cpp` | **NEW** — 5 gates, fixture prefix `_lw_` (collision-checked namespace) |
| `tests/unit/engine/CMakeLists.txt` | `add_gtest_unit(test_engine_lard_wiring test_lard_wiring.cpp)` |

**Header-only skeleton is deliberate:** `src/engine/CMakeLists.txt` globs
without `CONFIGURE_DEPENDS` (IO1 carried obligation c), so a patch-applied
.cpp silently doesn't compile. X2 adds .cpp files with an explicit
reconfigure step. You still need one reconfigure for the **test** target —
that CMakeLists edit triggers it automatically on build.

## 2. Design decisions (challenge in this order)

1. **The skeleton zeroes published concentrations rather than leaving state
   untouched.** A LARD run must not present frozen `Cinit` seeds as results.
   Zeroing includes `conc_old` because the .out writer interpolates old/new.
2. **The warning lives at open(), engine-level** (A1b precedent), not inside
   the solver — and its guard **mirrors stepRouting's predicate token for
   token** (`np>0 || legacyReactionsActive || water_age || heat_transport`,
   `&& !ignore_quality`). If you change one site, change both (lesson 52
   family — that family has now appeared four times).
3. **Reserved-species state (age/heat) is left untouched, not zeroed.** Their
   engines simply don't run under this dispatch; the warning names all three
   families. Alternative considered and rejected: zeroing age/heat arrays
   would destroy INITIAL_STATE/hotstart-loaded state that A2a promises to
   preserve across lifecycle operations.
4. **`LARD` parses as an alias** (the `EULERIAN_ARD`/`ARD` precedent).
5. **No `assembleExternalLoads` under the skeleton** — loads would accumulate
   into accumulators nothing drains.

## 3. Anticipated failure modes, likelihood order

1. ⚠ **Gate 2's control-liveness floor.** `peak_link_conc > 1.0` for LEGACY
   on a 1-hour run. If it fails, the deck needs a longer END_TIME (the
   node-store suite runs 4 h) — **lengthen the run, do not lower the floor**
   (lesson 55: the fix is a better deck, not a wider band).
2. ⚠ **Falsifier i may be weaker than claimed.** Gate 2's ability to fail
   rests on `initQuality()` seeding `Cinit = 50` into nodes at start
   (verified by code read: junction `Y0 = 1.5` > wet threshold `0.003281`,
   `SWMMEngine.cpp` ~6330). If the sweep shows gate 2 GREEN with the fill
   removed, the seed did not land — strengthen the gate by asserting the
   seeded nonzero state directly after `start()` before the first step, and
   record why the code read was wrong.
3. **Gate 3 step-count mismatch.** Trace sizes differing between LEGACY and
   LARD runs is itself a finding (quality feeding back into stepping) — do
   not paper over with size-truncation; diagnose.
4. **Corpus movement from the ArdConfig wording change.** If any parity
   deck's `.rpt` carried the old warning text, its bytes move. Before the
   corpus run: `grep -rl "inert under the LEGACY engine" tests/parity/` and
   the baseline `.rpt`s. Expected: no hits (the warning requires a
   transport.ard component under a non-ARD solver; no corpus deck is
   configured that way). If there are hits, the movement is attributable and
   the baseline regenerates with the round — say so in the commit.
5. **Duplicate or missing warning under lenient/editor opens** — open() runs
   once per lifecycle, but if a re-open path exists that reruns the warning
   block on the same ctx, the warning appears twice. Harmless to users,
   fails nothing today; record if observed.
6. **MSVC**: the header-only solver is trivially inlined; no export-surface
   change. Tests link `openswmm_engine_internal` on MSVC as usual.

## 4. Gates (tests/unit/engine/test_lard_wiring.cpp)

| # | Gate | Claim |
|---|---|---|
| 1 | `LagrangianOptionRoundTripsThroughSaveAs` | parses (both spellings); survives writeInpFile → reopen |
| 2 | `LagrangianPublishesZeroWhereLegacyPublishesSignal` | LARD: peak node/link conc **exactly 0.0** every step; LEGACY control on same deck: signal > 1.0; LARD warning present, absent on LEGACY |
| 3 | `LagrangianLeavesHydraulicsBitIdentical` | J2 depth + C3 flow bitwise equal per step, LEGACY vs LARD |
| 4 | `DefaultDeckStillRunsLegacyTransport` | no QUALITY_SOLVER line → LEGACY, links read kCin ± 1e-6 |
| 5 | `BypassWarningFiresExactlyWhenTheStageWouldBeLive` | present: age-only deck; absent: bare deck; absent: IGNORE_QUALITY |

## 5. Falsifier sweep (run as a table; every row must show the listed gates red)

Restore verified between cases (R4 refinement 8 — checksum the restored file,
don't trust `cp`).

| # | Falsifier (edit to apply) | Must fail |
|---|---|---|
| i | remove the four `std::fill` lines in `LagrangianSolver::step` | gate 2 (first sample reads the Cinit seed, 50) |
| ii | remove the InpWriter LAGRANGIAN branch | gate 1 (reopens LEGACY) |
| iii | remove the open() warning block | gates 2, 5(a) |
| iv | drop the liveness condition from the warning guard (warn unconditionally for LAGRANGIAN) | gates 5(b), 5(c) |
| v | drop `water_age`/`heat_transport` from the warning guard | gate 5(a) |
| vi | drop `!ignore_quality` from the warning guard | gate 5(c) |
| vii | change the dispatch branch to call `quality_.execute` | gate 2 (LEGACY signal under LARD) |
| viii | make `step()` also write `ctx.nodes.depth[2] += 1e-9` | gate 3 |
| ix | remove the parser branch (LAGRANGIAN falls to ext_options) | gates 1, 2 (runs LEGACY: signal nonzero, no warning) |

Rows with an empty "must fail" column after your run mean an unobserved
defense — do not accept the round until every row bites or its emptiness is
explained in writing (R4 refinement 4).

## 6. Standing verification

- Full C++ suite from an **isolated worktree** (lesson 71), `ctest -j8`.
  Expected: prior failure set unchanged (the known FV refinement gate only,
  if still unfixed at your base) + `test_engine_lard_wiring` green.
- **Parity corpus 18/18 byte-identical** (`tests/parity/run_corpus.sh`), same
  build config both sides (lessons 146/148). No corpus deck selects
  LAGRANGIAN, so the expectation is **zero movement**; any movement must be
  attributable to §3.4 or rejected.
- ASan/UBSan over the new suite + `test_engine_ard_node_store` +
  `test_engine_water_age` (the neighbors sharing the dispatch seam). The one
  known pre-existing hit is `HotStartManager.cpp:246` (misaligned load —
  not this round's).
- Warnings: zero new compiler warnings on the touched TUs.

## 7. What this round does NOT claim

No transport, no loads, no treatment, no age/heat under LARD, no D-NS1, no
C-API/MCP surface for the new enum value (E6/A6 own that; the GUI reads the
key through options hydration). `[PROCESS_COMPONENTS]` untouched. X2's brief
starts from the `lard_.step()` call site and replaces the skeleton body.

## 8. On acceptance

- Commit with the changeset table above in the message; record lessons if
  any falsifier row surprised you.
- Update `IMPLEMENTATION_ROADMAP.md` Phase 5 L0 row → ✅ with the hash, and
  the subplan's X1 row.
- Report back: gate results, falsifier table with observed failures,
  suite/corpus/sanitizer counts, and any deviation from §2's decisions.

---

# 9. Validation results (2026-08-23) — PASSED after one implementation and one deck fix

**Base:** `af7fb7db` (HEAD had moved four commits past the audit round by
validation time). **Committed `24602eb2`.** Artefacts:
`tests/output/lard_x1_2026-08-23/`.

## 9.1 ⛔ The changeset's core piece had never landed

**`SWMMEngine.cpp` contained no LARD dispatch and no warning** — zero
matches for `lard_`/`LAGRANGIAN` in the file, worktree and HEAD alike. §1's
table lists both hunks; the sandbox syntax-check covered six TUs, but the
two hunks that make the option *do* anything never reached this tree. The
enum parsed, the writer wrote, and nothing consumed the value: gates 2, 4
and 5 failed, exactly the shape the standing register calls
registered-referenced-never-landed (fourth instance).

**Both hunks were implemented here as §1/§2 specify**: the warning at
open() beside `warnIfLegacyBindingBypassed`, guarded token-for-token by
stepRouting's stage-liveness predicate; the dispatch as an
`else if (LAGRANGIAN) → lard_.step()` arm before the LEGACY default; no
`assembleExternalLoads`. With them in place, 4 of 5 gates went green
immediately.

## 9.2 Anticipated failure #1 arrived as predicted — gate 4, not gate 2

§3.1 named the control-liveness floor. Gate 2's `> 1.0` floor was fine; it
was **gate 4's `kCin ± 1e-6`** that the 1-hour run could not meet: the
LEGACY tail links read 99.999991 → 99.998137 (links 1→4), the exponential
approach to steady state still relaxing. Per instruction: **END_TIME
lengthened to 4 h** (the node-store suite's figure), band untouched. 5/5.

## 9.3 The falsifier table — nine rows, nine bites

| # | falsifier | predicted | observed |
|---|---|---|---|
| i | fills removed | 2 | **2** ✓ — so the `Cinit` seed DOES land; §3.2's worry about the code read was unfounded |
| ii | InpWriter branch removed | 1 | **1** ✓ |
| iii | warning block removed | 2, 5(a) | **2, 5** ✓ |
| iv | warn unconditionally | 5(b), 5(c) | **5** ✓ |
| v | reserved species dropped from guard | 5(a) | **5** ✓ |
| vi | `!ignore_quality` dropped | 5(c) | **5** ✓ |
| vii | dispatch runs `quality_.execute` | 2 | **2** ✓ |
| viii | `depth[2] += 1e-9` in step() | 3 | **3** ✓ |
| ix | parser branch removed | 1, 2 | **1, 2, 5** ✓ (5(a) additionally: the age-only deck runs LEGACY silently — consistent, not a defect) |

Every variant derived from sha256-verified pristine copies
(`variants.py`); restoration verified after the sweep.

## 9.4 Standing verification

- **ctest 160/161 ×3.** The one failure is `test_engine_2d_infil_integration`
  — another session's untracked file, failing identically before this round.
  The suite total is 161 (this round's test joined at 160→161... the LARD
  test was already registered by the delivered CMakeLists edit).
- **Corpus 18/18 byte-identical**, config guard silent. Base = falsifier ix
  (parser branch removed — the deepest X1 disablement; every other X1
  surface is enum-gated and provably unreachable from a deck that never
  says LAGRANGIAN). §3.4 pre-checked: zero grep hits for the old ArdConfig
  wording anywhere under `tests/parity/`.
- **ASan/UBSan** over `test_engine_lard_wiring` + `test_engine_ard_node_store`
  + `test_engine_water_age`: all pass; the single hit is the known
  pre-existing `HotStartManager.cpp:246` misaligned load (§6's listed
  exception).
- **Zero new warnings** on the touched TUs — the build's 54 "warning
  generated" summaries all trace to pre-existing TUs (`TableData.hpp`
  includes, `test_gap_fixes`, `test_rdii`, `test_routing`).

## 9.5 Deviations

1. **The SWMMEngine.cpp hunks were written by the validator** (§9.1) — to
   §1's spec, but they are not the author's bytes.
2. **Gate deck END_TIME 1 h → 4 h** (§9.2), with the measured reason in a
   comment at the edit.
3. **The committed `SWMMEngine.cpp` and `InpWriter.cpp` blobs were built
   from HEAD** plus X1's hunks only. Both files also carry other sessions'
   uncommitted work (two `refreshRenderFieldsIfStale()` calls; the
   `[2D_INFILTRATION*]` writer sections). The clean pair was built and the
   full suite run on it (5/5) before committing.

## 9.6 Notes for X2

- The dispatch arm and skeleton behave exactly as §2 decided; nothing in
  the falsifier sweep suggests the seam needs reshaping before segment
  transport replaces the body.
- Gate 2's zero-claim reads `ctx` directly; when X2 lands real transport
  the gate flips from `== 0.0` to a parity band — the control run is
  already in place for that.
- `test_engine_concurrent`'s shared-input race and the standing 2D-infil
  failure remain other rounds' items.

# 10. Commit

`24602eb2` — `feat(quality): QUALITY_SOLVER LAGRANGIAN is a real, warned,
inert dispatch`, on parent `af7fb7db`. Nine files, +514 −9. Roadmap Phase 5
L0 row and subplan X1 row updated with the hash.
