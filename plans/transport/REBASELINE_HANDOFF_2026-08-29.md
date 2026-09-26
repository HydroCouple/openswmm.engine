# Re-baseline the suite after the merge repair — Handoff (2026-08-29)

**For:** the checking agent.
**Base:** `8f9f164d` — the first commit on `swmm6_rel` that compiles from a
clean checkout since merge `a38f0c0b`.
**Standing findings:** lessons 1–183.

**No code changes. This round produces a NUMBER and an attribution**, and
blocks H7b until it does.

---

## 1. Why it cannot be skipped

The standing figure "177/177" predates the develop merge and is retired. The
H7a check measured **180 registered, 5 failing** — but it measured them in a
tree carrying the peer session's **uncommitted** work, because that was the
only way to build anything at all.

**That tree no longer exists.** `8f9f164d` committed the two merge *repairs*
and deliberately left the peer's in-flight `REPORT_SIGNED_HEADS` (#156 O-6)
uncommitted. So the binary you would build at HEAD today is **not** the binary
those five failures were measured against.

**Do not inherit the list of five. Re-derive it.** Lesson 71 is exactly this:
a measurement from a shared tree is not attributable to the commit you think
it belongs to.

## 2. ⚠ One of the five is probably not what it looks like

`fv_tpa_closure` and O-6 are plausibly **the same subject**. The O-6 hunks
rewrite node HEAD reporting under `REPORT_SIGNED_HEADS` and their own comment
says the point is that *"sub-atmospheric TPA columns are observable"*.

So `fv_tpa_closure` may have been failing **because** O-6 was half-present, or
may be a genuine pre-existing failure that O-6 exists to fix. Those are
opposite conclusions and the tree you measure in decides which one you get.
**Measure it at `8f9f164d` with O-6 absent, and say which.**

The same caution applies more weakly to the rest: they were all measured
against a binary containing unreviewed feature work.

## 3. Protocol

1. **`ctest -N`** → record the registered count at `8f9f164d`. This replaces
   177 as the standing figure; write the number and the commit together, since
   a count without a commit is what went stale last time.
2. **`ctest -j8` ×3.** Record every failure, all three runs. A test failing in
   1 of 3 is a flake and must be labelled as one rather than folded into the
   count (`test_engine_report_timing` is the known one).
3. **Run each failure STANDALONE.** The `-j8` fixture-collision family is real
   in this repo (`b85b802d`); a test that fails parallel and passes alone is a
   different finding from one that fails both ways.
4. **Attribute each remaining failure.** For each, answer: does it fail at
   `a38f0c0b` (the merge) as well? At `8c8faa3c` (the commit before)? That
   bisect is what separates *the merge broke it* from *it arrived broken from
   develop* — and only the second is somebody else's problem to fix.
5. **Corpus at `8f9f164d`**: 20/20 `.out` and `.rpt`. This becomes the
   reference all subsequent rounds diff against, so it needs to exist at a
   commit that builds.
6. **Record in `IMPLEMENTATION_ROADMAP.md`**, not only here (lesson 167).

## 4. What to report

- the new registered count, tied to `8f9f164d`
- the failure set at `8f9f164d`, **with O-6 absent** — and explicitly whether
  it differs from the five measured earlier
- `fv_tpa_closure`'s verdict per §2
- for each failure: merge-caused or inherited-from-develop
- which failures touch **heat** (`water_age_lid`, `heat_watershed`,
  `heat_lid`) — H7b modifies that subsystem, so a pre-existing failure there
  must be *named and understood* before H7b, or "did H7b break it?" is
  unanswerable

## 5. What this round must NOT do

- **Do not fix the failures.** Attribution first; each fix is its own round
  with its own falsifiers. A re-baseline that also repairs is a re-baseline
  nobody can trust.
- **Do not commit the O-6 work** to make something pass. It is mid-feature and
  belongs to its author.
- **Do not rebuild the standing figure from a tree with uncommitted changes** —
  that is the mistake this round exists to undo. `git status` must be clean of
  everything but `plans/`, and if it is not, say so rather than proceeding.

## 6. After this round

H7b is unblocked *if* the heat failures are understood. If any of the three
heat gates fails for a reason that touches temperature transport, **H7b waits
behind that fix** — building a temperature capability on top of an unexplained
temperature failure is how a defect gets attributed to the wrong round.

---

# CHECK RECORD — 2026-08-30

**Standing figure: `8f9f164d` — 180 registered, 175 passing, 5 failing (×3, no
flakes), corpus reference stored.** Measured in three CLEAN `git worktree`s
(`8f9f164d`, `8c8faa3c`, `47c00ae3`) configured like `build/darwin`; the
shared tree was NOT used — it carries 16 modified source/test files (O-6 and
more) and §5.3 forbids it. Nothing fixed, nothing committed. Artifacts:
`tests/output/rebaseline_8f9f164d/` (`standalone.log`, `corpus/`).

## §3.1–3.3 at `8f9f164d`

`ctest -N` = **180**. `ctest -j8` ×3: **175/180 every run**, the same five
each time — no flake in any run (`test_engine_report_timing` passed all
three). Each of the five fails **standalone** too, so none is a fixture
collision. Failing gates:

| binary | failing gate(s) | symptom |
|---|---|---|
| `test_engine_water_age_lid` | `StorageAgeIsTheSumOfTheLayerResidenceTimes`, `DrainLeavesAtStorageAgeAndReachesTheNode` | `held_s/chain_s = 0.00057` (expected ≈1) |
| `test_engine_heat_watershed` | `EveryRunonContributorKeepsTemperaturesInsideTheSources` | `runon_inflow[0] == 0` — "the LID underdrain is not returning" |
| `test_engine_heat_lid` | `ADrainedLayerStillConductsAndIsNotResetByThePolicy` | "storage still holds 0.0413 ft" |
| `test_engine_transport_dt_reference` | `LidColumnTemperatureConvergesUnderRefinement` | err/spread 0.0897 vs band 0.0011 (ratio 1.67 — first-order, not converging) |
| `test_engine_fv_tpa_closure` | `FreeSurfaceDeckIsBitIdentical`, `SealedSiphonSustainsSubAtmosphericFlow`, `OpenHumpVentsAndBreaksTheSiphon`, `SealedSiphonMassConserved` | free-surface `.out` differs with TPA on; siphon gates |

**Does it differ from the five measured during the H7a check (in the
peer-laden tree)?** Same five binaries. This is now attributable to a commit.

## §3.4 attribution

**Four heat/LID gates — brought in by merge `a38f0c0b`, from the REMOTE
branch, not by a conflict.** All four PASS at `8c8faa3c` (the commit before
the merge; also 180 registered) and FAIL at `8f9f164d`. The merge's second
parent (`5150480e`, github `swmm6_rel`) carries PR #103 / `5f6a2ba5`
`fix(lid): restore unit conversions + subcatchment/mass-balance coupling
(#102)` and `e2295827` (ddspot) — `LID.cpp` +143/−… and `SWMMEngine.cpp` ±66
under LID drain handling. The common symptom is one thing: **LID storage no
longer drains through the underdrain** (storage holds, runon never returns,
age never accumulates, the temperature column stops converging). **The
repair's choice of receiver is NOT the cause**: I rebuilt `8f9f164d` with
develop's own fallback (`drain_subcatch >= 0 ? drain_subcatch : sc`) in the
repaired `addRunonTemperatureAt` call and all four still fail identically.
So the verdict is "inherited from the remote LID PR" — whether that PR broke
underdrain return or the H5b/A4 gates encode a drain behaviour the PR
deliberately changed is the next round's question, and it is a **heat**
question (§4, last bullet): **H7b waits behind it** (§6).

**`fv_tpa_closure` — arrived red with #156 (`47c00ae3`), before either
merge, with O-6 absent throughout.** 4 of 9 gates fail at `47c00ae3`,
`8c8faa3c` and `8f9f164d` alike (`FreeSurfaceDeckIsBitIdentical` first:
`tpa_fs_base.out != tpa_fs_on.out`). Per §2 this is the second reading:
a genuine pre-existing failure that O-6 presumably exists to fix, not one
O-6's half-presence caused. The peer is rewriting the whole fixture
(`test_fv_tpa_closure.cpp` +203/−109 uncommitted, "over-the-top drawdown");
it belongs to that session.

## §3.5 corpus reference

`tests/output/rebaseline_8f9f164d/corpus/` — 20/20 `.out` and `.rpt` at
`8f9f164d` (same CLI on both sides; PROVENANCE records the engine sha256
see the file). Subsequent rounds diff against this.

## What this means for the sequence

- Cite **"175/180 at `8f9f164d`, five known"** — never 177.
- **H7b is BLOCKED** until the LID-underdrain regression is understood (three
  of the five are temperature/age-through-LID gates).
- The `fv_tpa_closure` red is #156/O-6's and is not a transport-program
  blocker.
