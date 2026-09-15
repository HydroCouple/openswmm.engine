# Tracker reconciliation + G0 sign-off — Handoff (2026-08-25)

**For:** the checking agent.
**Base:** HEAD at the time of writing (post `55a70839`; the quality-ledger
units fix is a **separate, still-unvalidated** changeset in the tree — see §7).
**Standing findings:** lessons 1–156.

**⚠ This round is unusual and needs a different kind of checking.** It changes
almost no code. Its deliverables are ~20 **factual claims about the code**,
made in documents that future planning will treat as authoritative. **A wrong
claim here is worse than a wrong line of code**, because nothing will execute
it and fail. Your job is to **independently re-derive the claims**, not to
confirm them.

**Do not read my documents first.** §3 gives you the questions without my
answers. Answer them from the tree, then diff against §4. That ordering is
the whole design of this protocol — lesson 152 is that a corroborating
observation confirms a wrong hypothesis as readily as a right one.

---

## 1. What changed

```
new: plans/transport/PROGRAM_REVIEW_2026-08-25.md          (the audit)
new: plans/transport/G0_SIGNOFF_RECOMMENDATION_2026-08-25.md
mod: plans/transport/IMPLEMENTATION_ROADMAP.md   (Phase 1x added; IO3/IO5/R5/
     A5/L5–L7/GUI rows corrected; Phase 3 relabelled 2D-S1…2D-S7; Phase 2 and
     Phase 4 verification notes; G0 row closed; lessons 153–156)
mod: plans/transport/PROGRESS.md                 (phase table rewritten; counts
     caveated as lower bounds; four open-items rows)
mod: plans/TWO_ZONE_GROUNDWATER_EXPLICIT_LTS_PLAN_2026-08-15.md  (G0 sign-off
     + a §11 correction)
mod: src/engine/transport/components/EulerianArdComponent/ArdEngine.hpp   ← COMMENT ONLY
mod: src/engine/core/SimulationOptions.hpp                                ← COMMENT ONLY
```

**Only two source files, both comment-only.** They said things that were
false: `ArdEngine.hpp` listed treatment/sources/ledger as "pending (E5)" five
rounds after E5a and E5b landed; `SimulationOptions.hpp` called LARD
"skeleton dispatch only" five rounds after X2. **Both were read as
authoritative by the audit before being checked** — that is lesson 154 and it
is why they were repaired here rather than left.

## 2. Build check (small, but do it first)

Comment edits can still break a build — an unterminated block comment, a
stray `*/`, a doc comment that swallows the next declaration.

1. `cmake --build` clean.
2. `ctest -j8` — **expect exactly what base gives you**. This round must not
   move a single test.
3. **Corpus: 19/19 `.out` identical**, matched configs, guard silent. A
   comment-only round that moves a deck means something other than a comment
   changed.

If any of the three moves, stop — that is the finding, and everything below
is secondary.

## 3. ⛔ Re-derive these WITHOUT reading my documents

Answer each from the tree. Write your answers down **before** opening §4 or
`PROGRAM_REVIEW_2026-08-25.md`.

**Phase 1 status questions**
1. Does `swmm_reaction_validate_expression` exist? Where, and how large is
   the surrounding reaction C API? Are there Python or MCP bindings for it?
2. Does `InpWriter` write a `[PROCESS_COMPONENTS]` pointer section? Does it
   call a per-component `saveData()`? **What happens to embedded
   `[REACTION_*]` sections on a save-as — and does the engine tell the user?**
3. Do `swmm_process_component_*` C entry points exist? Is there any
   reload/staleness surface? Python? MCP?
4. Does an analytical gate for H4 (G-UT3, Taylor/MSX/CSH) exist anywhere?

**Phase 2**
5. Does any `HydroCouple*.h` exist in the engine tree? Does any build file
   reference the `~/Projects/HydroCouple` checkout?
6. What are the enumerators of `PluginType`? What symbol does the plugin
   factory `dlsym`?
7. **What happens if a `[PROCESS_COMPONENTS]` row names a `.dylib`/`.so`?**
   Trace it to the exact behaviour, not the intent.
8. How many times does `IModelComponent` appear in the repo, and where?

**Phase 3 / 4**
9. Does `src/engine/2d/` contain any scalar transport? **Careful:** search for
   "advection" and then determine what each hit actually advects.
10. Is there any two-zone groundwater kernel, σ-column, or GW LTS scheduler?
    If you find something that looks like one, **check whether it is Track I
    infiltration**, and check what document `Infil2D.hpp` names as its parent.

**Phase 5 / GUI**
11. Pick any three of `24602eb2`, `8c141a5e`, `647a3603`, `b9852cee`,
    `9f155227`, `d79c8bcf`, `d7b6c079`, `948b2840`. Do they exist? Does the
    code each claims to add exist, at the size claimed?
12. In `openswmm.gui`: does a reaction editor exist? A water age editor —
    **and is it reachable from the UI, or only present as a file?** A heat
    editor?

**G0 / D-N1**
13. How are the LTS tier lists stored in `ExplicitInertialSolver.hpp` — fixed
    arrays or dynamic containers? **Enumerate every fixed-size thing that
    would have to change to raise the tier ceiling above 8**, including the
    GPU path.
14. **Does a bitwise surface-regression corpus deck exist?** How many decks
    are in `tests/parity/MANIFEST`, and how many are 2D?

## 4. My answers — compare only after §3

| # | my claim |
|---|---|
| 1 | EXISTS, `openswmm_reactions.h:78`, impl `openswmm_reactions_impl.cpp:114`, ~38 entry points / 804 LOC, shipped under labels **E-C1/E-C3** not R5. **No** Python, **no** MCP |
| 2 | Pointer section YES (`InpWriter.cpp:2520-2569`, incl. carry-alongside). `saveData()` **NO** — `:2574` says so; `:2580-2586` **warns the user embedded sections are lost**. `ReactionsWriter.cpp` exists but is wired only to the C API |
| 3 | `openswmm_process_components.h:46-72`, ~110 LOC. **No** reload/staleness, Python or MCP |
| 4 | **ABSENT** — H4 is marked delivered; its G-UT3 gate never was |
| 5 | No `HydroCouple*.h` anywhere; no `find_package`; the checkout is **not referenced by the engine build in any form** |
| 6 | `INPUT, OUTPUT, REPORT, STATE_IO` (`IPluginComponentInfo.hpp:75`) — no `PROCESS_COMPONENT`. Factory `dlsym`s `openswmm_plugin_info` only (`PluginFactory.cpp:253`) |
| 7 | **Hard error** — detected `ProcessComponentRegistry.cpp:62-73`, rejected `:216-223` with "arrives with plan phase HC2" |
| 8 | **Once, in `paper/paper_v2.md`.** Zero source occurrences |
| 9 | **No scalar transport.** "Advection" hits are **momentum** advection in the local-inertial scheme (`SolverOptions2D.hpp:216`, `InertialKernels.hpp:242`) |
| 10 | **No GW kernel.** Reserved enums at `Infil2D.hpp:85-86` that the C API **refuses** (`ApiInfil2D.cpp:124-130`). Track I IS delivered and is different — but `Infil2D.hpp:21` names the two-zone GW plan as its parent |
| 11 | All eight exist on `swmm6_rel`, subjects matching; `SegmentStore.hpp` 288 lines, `LagrangianSolver.hpp` 576, `RwptDispersion.hpp` 356, 2,213 lines of LARD tests |
| 12 | Reaction editor **1150 lines**; age editor exists **and is reachable by two paths** (the "Y3b owed / unreachable" note is stale); **no heat editor file exists** |
| 13 | `cells_by_tier_`/`edges_by_tier_` are **already `std::vector<std::vector<int>>`** (`:150-151`). Fixed: `std::array<long,8> tier_occupancy_` (`:205`), `std::clamp(…,1,8)` at `ExplicitInertialSolver.cpp:79` **and** `ExplicitKokkosSurfaceSolver.cpp:230`, plus the parser bound |
| 14 | **NO.** 19 decks in MANIFEST, **zero 2D**. `test_2d_lts_equivalence.cpp` covers tier counts but is conservation/equivalence, **not bitwise** |

**Any disagreement is the round's finding.** Report it as a correction, not a
nitpick — these numbers are now load-bearing for planning.

## 5. Falsifiers for a documentation round

| falsifier | expected |
|---|---|
| i. `grep -E 'X4\|Y0\|Z1' IMPLEMENTATION_ROADMAP.md` | **hits now.** Before this round: zero, for ~20 validated commits. If it still returns zero, Phase 1x did not land |
| ii. `grep -n '^| S[1-7] |' IMPLEMENTATION_ROADMAP.md` | **no hits** — Phase 3 rows are `2D-S1…2D-S7`. Snow's `S1/S2a/S2b/S3/S4` **must still be untouched** in all sixteen historical handoffs; if any were renamed, that is a defect, not thoroughness |
| iii. read `ArdEngine.hpp`'s docblock and `SimulationOptions.hpp:85-100` | both describe **current** behaviour. Check the LARD one against `SWMMEngine.cpp:342/350/356` — the "NOT YET" list must match the live warnings |
| iv. the G0 sign-off's conditions | are they **binding or decorative**? The migration guide is written as a **merge blocker** and the 2D corpus deck as a **precondition of D-N1**. If a future round can satisfy the letter while ignoring both, say so |
| v. `PROGRESS.md`'s counts | still marked as **lower bounds**? I deliberately did **not** re-derive the gate/deck counts. If you have a build, **`ctest -N` and a `wc` sweep close a gap open since 2026-08-22** — that would be worth more than anything else in this round |

## 6. Known gaps — mine

- **I built nothing and ran nothing.** Every claim in §4 is a code read or a
  `git log` read. This program's lessons 126/130/144/152 are all the same
  shape, and this round is maximally exposed to them.
- **The counts are not re-derived** (falsifier v). §3's phase table and §1's
  gate counts are marked as lower bounds rather than measured.
- **The audit sampled; it did not sweep.** Three subagents covered the plan
  enumeration, the X/Y/Z track and Phases 2–5 + GUI. A step neither I nor
  they asked about could still be mis-marked.
- **`PROGRAM_REVIEW`'s "~40 of ~45" for Phase 1 is an estimate**, not a
  count. It is better than the "34 of ~45" it replaced and it is still soft.
- **I did not check the GUI repo's own trackers** for the same disease this
  round treated in the engine's. Given that four of the collisions and stale
  claims found so far were cross-repo, **that is the most likely place for
  the next instance**.

## 7. ⚠ Tree state you must not confuse with this round

**The quality-ledger units fix is in the tree and is NOT validated.** It
touches `SWMMEngine.cpp`, `openswmm_massbalance_impl.cpp`,
`DefaultReportPlugin.cpp`, `legacy/engine/landuse.c` and
`test_massbalance.cpp`, and it has its own handoff
(`QUALITY_LEDGER_UNITS_FIX_HANDOFF_2026-08-23.md`) with its own protocol —
including an expected base-failure ratio near **16057** and a **19/19 corpus
expectation**, both of which will interact with §2 of this document.

**If you validate both in one pass, separate the attributions explicitly.**
This round must move nothing; that round moves `.rpt` quality blocks on every
deck with pollutants. A merged report cannot tell them apart, and this
program has already lost a round to exactly that (the "four moved decks" that
were a build-config difference).

Other sessions' work is also live in the tree — check `SHARED_TREE_STATE`,
commit named paths only, and **never `git commit -a`**.

## 8. Prepared commit message

```
docs(transport): reconcile the trackers with the verified code state; close G0

Three parallel audits against the source tree, not the plan documents. The
bookkeeping was wrong in both directions: Phase 5 read "not started, 0 of 4"
with 2,213 lines of LARD suites in the tree, the GUI read "0 of 14" with an
1150-line reaction editor shipped, and the X/Y/Z/closeout tracks -- roughly
twenty validated commits -- returned zero hits in the roadmap's canonical
tables.

Folds those tracks in as Phase 1x. Settles the roadmap's self-contradictions
on IO3 and IO5 from the code (both PARTIAL; the engine warns that embedded
[REACTION_*] sections are lost on save). Corrects R5, which shipped under
labels E-C1/E-C3. Relabels Phase 3 as 2D-S1..2D-S7 to end a five-label
collision with the snow rounds -- renaming the unstarted side, because the
snow labels are baked into sixteen handoffs that are historical record.

Adds verification notes where "not started" was hiding something: Phase 2 is
refused, not unstarted, and Phase 4 has two traps that make it read as landed.

Repairs two source docblocks that described work as pending five rounds after
it landed, and which the audit trusted over the plan docs before checking.

Closes G0, owed since 2026-08-15. Reviewing rather than rubber-stamping found
two errors in the same section pointing opposite ways: D-N1's risk entry was
stale (the tier lists were already dynamic vectors, so it is a chore, not a
risk), and the protection that entry relies on was never built (19 corpus
decks, none 2D). A 2D deck is now a precondition of D-N1.
```
