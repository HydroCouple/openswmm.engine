# ⚠ Shared-Tree State — Read Before Committing (2026-08-23)

**Written by:** the Y-track implementing agent, on finding two hazards while
surveying for Z1. **Nothing in this note has been repaired** — repairing a
shared index while another session is mid-round is how you turn a
bookkeeping artifact into lost work. It is written down so whoever owns the
tree next fixes it deliberately.

---

## 1. ⛔ FOUR PHANTOM STAGED DELETIONS — do not `git commit -a`

```
$ git diff --cached --name-status | grep ^D
D  include/openswmm/engine/openswmm_water_age.h
D  src/engine/core/openswmm_water_age_impl.cpp
D  tests/unit/engine/test_transport_options_api.cpp
D  tests/unit/engine/test_water_age_api.cpp
```

**All four are phantoms — proven, not assumed:**

| file | on disk | in HEAD |
|---|---|---|
| `include/openswmm/engine/openswmm_water_age.h` | ✅ | ✅ |
| `src/engine/core/openswmm_water_age_impl.cpp` | ✅ | ✅ |
| `tests/unit/engine/test_transport_options_api.cpp` | ✅ | ✅ |
| `tests/unit/engine/test_water_age_api.cpp` | ✅ | ✅ |

**The tell:** all four are exactly the files added by **X5 (`d7b6c079`)** and
**Y0 (`948b2840`)** — the two most recent transport commits. Whatever wrote
this index was working from a snapshot that predates them, so it sees their
files as "removed". This is the same family as the incident recorded in
`PROGRESS.md` §5 / `tests/output/index_repair_2026-08-22/`, where a stuck
`.git/index.lock` produced **1850** phantom deletions and no work was ever
actually at risk.

**Why it still matters:** a single `git commit -a` (or any tooling that
commits the staged tree wholesale) would make these deletions **real**, and
would silently remove X5's public header — breaking Y3's dialog, X5's two
test suites, and the GUI build that consumes `openswmm_water_age.h`.

**Repair procedure** (the one that worked on 2026-08-22 — follow it, don't
improvise):

1. Checksum every path HEAD lists, so the repair is provably lossless:
   `git ls-tree -r --name-only HEAD | xargs -I{} sha256sum {} > /tmp/before.txt`
2. `git reset` (mixed — rebuilds the index from HEAD, touches no working file).
3. Re-checksum and diff against `before.txt` — **aggregate must be identical,
   not one byte changed**.
4. Re-run `git status`; the four deletions should be gone and only genuine
   working-tree modifications should remain.

**Do not run this while §2's round is mid-sweep.**

## 2. Z1 WAS STARTED AND REVERTED — now unclaimed (observed, then gone)

**Timeline, all within one survey (≈15 minutes):**

1. **First look:** Z1 (reserved species as `[INFLOWS]` constituents, per
   `AMENDMENT_1_WATER_AGE_AS_SPECIES_2026-08-23.md`) was **partially
   implemented** across five files — `WaterAgeData.hpp`
   (`node_ext_inflow_age`), `Inflow.{hpp,cpp}`, `QualityRouting.cpp`
   (`addAgeVolume` override consult), `WaterAgeComponent.cpp` — with
   comments citing the amendment by name.
2. **And it was falsified at that moment:** `QualityRouting.cpp` carried
   `// FALSIFIER A: override consult removed` with the lookup deleted — a
   sweep in progress.
3. **Second look, minutes later:** every marker **gone**. No
   `node_ext_inflow_age`, no `FALSIFIER`, no `D-Y4` comment anywhere in
   `src/`. `addAgeVolume` is back to its pre-Z1 one-liner.
4. **Not committed:** `git log --all --grep` finds no Z1 commit. The tip is
   `0dd5dd58` (a revert of an FV Preissmann-slot fix), and the newest
   transport commit is still Y0 `948b2840`.

**Conclusion: Z1 is UNCLAIMED and UNIMPLEMENTED.** Someone began it and
rolled it back without landing it. Whether that was abandonment, a reset to
start clean, or a restore that overshot, the tree now has none of it.

**Before starting Z1, re-verify** — this note is a snapshot, and the whole
point of §1–§4 is that snapshots of a shared tree go stale in minutes:

```
grep -rn "node_ext_inflow_age" src/          # expect: nothing, if still unclaimed
grep -rn "FALSIFIER" src/                    # expect: nothing, or a sweep is live
git log --all --oneline --grep="Z1"          # expect: nothing, if still unclaimed
```

**This episode is the argument for §4.1.** Z1's existence was discoverable
only by grepping for a comment string, and its disappearance only by
grepping again. A one-line claim in §5 would have made both states
legible without archaeology.

## 3. A correction to Amendment 1 (mine, not the Z1 session's)

`AMENDMENT_1_WATER_AGE_AS_SPECIES_2026-08-23.md` §6 says Z1 *"must FLIP
A1a's TIMESERIES deferral gate, not delete it."* **That instruction is
wrong and should not be followed.**

Z1 delivers time-varying age through the **`[INFLOWS]`** path. A1a's
deferral is about the **`[WATER_AGE_SOURCES] … TIMESERIES <name>`**
spelling — a *different surface*, still genuinely deferred. Its gate
remains a valid observer of a real deferral and **must stay as it is**.
Flipping it would delete a legitimate defence for a capability that has not
actually arrived.

The amendment's §6 is corrected in place with this note referenced.

## 4. Durable protocol (this is the FOURTH concurrent-session incident)

The record so far: A2b-era fixture clobber · X1's two lost `SWMMEngine.cpp`
hunks · Y2a's two CMakeLists carrying other sessions' registrations · this
one. The mitigations already in force (hunk-presence greps in every handoff,
clean-blob commits, `GIT_INDEX_FILE` per round) are working — every
incident was *caught*. What is missing is announcement, not detection:

1. **Claim a round in writing before touching code.** A one-line entry in
   this file (or its successor) naming the round, the files it will touch,
   and the session — so the next agent's survey finds it. Z1's markers were
   discoverable only by grepping for a comment string.
2. **Leave a marker while a falsifier is applied.** A `FALSIFIER` comment in
   the code is good; a line in this file saying "engine tree is falsified
   right now" is better, because it is where someone looks *before*
   building.
3. **Never `git commit -a` in a shared tree.** Commit named paths, or the
   clean-blob procedure the validators have been using.
4. **A full build before trusting any test verdict** (Y2a's correction — a
   stale partial build produced a phantom "standing failure" in Y1's
   report).

## 5. Current round claims (append here)

| Round | Repo / files | Session | State |
|---|---|---|---|
| **Z1** | engine: `WaterAgeData.hpp`, `Inflow.{hpp,cpp}`, `QualityRouting.cpp`, `WaterAgeComponent.cpp` | unknown → **now the Y-track agent** | started by another session, **reverted** (§2); re-claimed and being implemented 2026-08-23 |
| 2D/FV | engine: `InpWriter.cpp` (5 `[2D_INFILTRATION*]` hunks), `SimulationSnapshot.hpp` | unknown | uncommitted, long-running |
| Y2b-1 | gui: `irunlayer.h`, `plotattribute.{h,cpp}`, `src/plot/swmmoutrunlayer*` + descriptor TU | Y-track agent | queued behind Z1 |
| Closeout P0.2 | engine + gui: `CHANGELOG.md` only | closeout agent | **committed** 2026-08-25 (`2ecdef8a` engine, `d9ad932` gui), path-scoped |
| **Closeout P1.1** | engine: `tests/unit/engine/test_lard_dt_reference.cpp`; temporary falsifier in `src/engine/quality/lard/LagrangianSolver.hpp:224` during the sweep | closeout agent | **committed `6566f407`** 2026-08-25; falsifier reverted, tree byte-clean; full ctest 170/171 (sole failure = pre-existing in-flight `test_engine_2d_infil_integration`, not in HEAD) |
| Closeout P1.3 | engine: `src/engine/core/openswmm_model_impl.cpp`, `tests/unit/engine/test_options_malformed_values.cpp` | closeout agent | **committed `22e55228`** 2026-08-25; falsifiers A+B reverted (`grep FALSIFIER src/` clean); full build + ctest 170/171 (same pre-existing sole failure); HEAD verified == disk on both files |

**2026-08-25 index observation (both repos, §1's family again).** The engine
index shows phantom `D` for the files H1 (`d80bba34`) and slot R0
(`af6dc869`) added — `test_options_malformed_values.cpp`,
`test_fv_closure_shapes.cpp`, `tests/parity/slot_program/decks/*` — while
they sit on disk (`??`) matching HEAD byte-for-byte. The GUI index likewise
phantom-deletes Y2a/Y2b-1/Y3's files (`speciesattributes.h`,
`resultdescriptor.h`, `wateragesourcesdialog.h`), all present on disk and in
HEAD. The slot session is live (R0b `8e6add0f` landed mid-survey), so per
§1's own caveat **no repair was run** — all closeout commits are path-scoped.
Whoever next owns a quiet tree should run the §1 repair procedure.

**2026-08-25 (later) — index repair RUN; R2a re-landed.** The §1 repair was
executed in BOTH repos by the slot session (openswmm-gui-87) with all live
sessions' consent: plain `git reset` against a verified-intact HEAD; every
phantom `D` cleared; no worktree file touched. Root cause of the day's ref
divergence: slot R2a `044da0cf` was orphaned when the E-B/C stack advanced
`swmm6_rel` from `1db158e6` without re-checking the tip; R2a is re-landed as
**`3da777dc`** on top of `7cb3a2e2` (rescue branch `r2a-rescue` still marks
the orphan until the user pushes). GUI: species-funnel/tracks/export landed
as `2f09c6e`, mesh-infil/min-size/BC/reproject consolidation as `4754d68`,
on top of `f2ae4a7`. In-flight exclusions honored: reaction-editor round
(gui-f9), SWMMEngine failure-reporting hunks + simulationrunner (gui-26),
python/gitignore/gpkg churn (unowned). Ref moves are now CAS-only by
convention: `git update-ref refs/heads/X <new> <expected-old>`.
