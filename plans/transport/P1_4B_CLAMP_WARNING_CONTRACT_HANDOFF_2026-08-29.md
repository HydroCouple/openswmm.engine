# P1.4b — the per-clamp runtime warning is retired — Handoff (2026-08-29)

**For:** the checking agent.
**Base:** `4b26aa50` (P1.4 as landed).
**Standing findings:** lessons 1–175.
**Implemented syntax-only.** Nothing built, linked or executed.

```
mod: src/engine/core/SimulationContext.hpp        (runtime_warned → first_clamp_recorded)
mod: src/engine/quality/NegativeSources.hpp       (3 warning blocks removed; summary enriched; docs)
mod: tests/unit/engine/test_negative_sources.cpp  (1 gate row FLIPPED)
mod: tests/unit/engine/test_ard_transport_bcs.cpp (1 gate row FLIPPED)
```

---

## 1. The decision and why

Your P1.4 check found it: **every extraction deck clamps during fill.** A deck
extracting only 40 % of its incoming mass logged **108 clamps** while the chain
wetted, so the per-clamp runtime warning fired on correct models. That is
lesson 148's shape — and the cost is not just noise. **A warning that fires on
ordinary decks is one users learn to ignore, which destroys it on the decks
where it matters.**

**User decision (2026-08-29): drop the runtime warning, keep the end-of-run
summary.** Applied to the **shared seam**, so LEGACY, ARD and LARD stay
identical — which is the whole reason `NegativeSources.hpp` exists (the
lesson-52 family). Fixing only the ARD cell path would have drifted the three
engines' semantics apart to avoid touching two of them.

**Nothing is lost that was load-bearing.** The clamp remains observable
through `negsrc.clamp_events`, `negsrc.shortfall_mass`, and the summary — which
now also names the first element (the runtime warning's only unique payload)
and states the fill caveat, so a reader meeting a nonzero count does not read
it as a modelling error.

**`runtime_warned` is renamed `first_clamp_recorded`** because its meaning
changed: it no longer gates a warning, only the `first_node` capture. Four
sites, all local. Leaving the old name would be lesson 170 exactly — a flag
called `warned` in code that does not warn.

## 2. ⚠ Two gate rows are DELIBERATELY FLIPPED

Both previously asserted the warning **fires**; both now assert it is **gone**.
This is the H1-inversion precedent — a flip, not a deletion, so the row still
carries a claim.

| file | was | now |
|---|---|---|
| `test_negative_sources.cpp:306` | `EXPECT_TRUE(has_warning(r, "clamped to the available amount"))` | `EXPECT_FALSE(...)` |
| `test_ard_transport_bcs.cpp:379` | `EXPECT_TRUE(has_needle(ctx.warnings, "clamped"))` | `EXPECT_FALSE(... "clamped to the available amount")` **plus a new `EXPECT_TRUE(... "D-NS1 summary")`** |

**Verify both flips are surgical** — the summary assertions and the
`clamp_events` assertions beside them must be untouched. The observability
claim moved; it did not weaken. The ARD row is strictly stronger than before:
it now pins that the clamp reaches a user-visible channel *at all*, which the
old `"clamped"` needle did only incidentally.

## 3. Validation protocol

1. `ctest -j8` ×3. Expect the standing figure. **The two flipped rows must
   pass; if either still sees the old warning, a call site was missed.**
2. **The flips must FAIL against the P1.4 binary** (restore the three warning
   blocks, keep the gates). That is what proves they observe the change rather
   than passing for an unrelated reason. **Quote it.**
3. **Corpus: 20/20 `.out` byte-identical AND 20/20 `.rpt` unchanged.** No
   corpus deck has a negative source, so none clamps, so none carried the
   warning — **the `.rpt` blast radius should be exactly nil.** If any corpus
   `.rpt` moves, a deck was clamping that nobody knew was clamping, and that
   is a finding worth more than this round.
4. **Confirm across all three engines**, not just ARD.
   `test_negative_sources.cpp` loops over solvers; check the flipped row
   passes for each. The shared seam means one edit changed LEGACY and LARD
   too, and that is the intent — but intent is not evidence.
5. **Falsifier sweep:**

   | falsifier | expected |
   |---|---|
   | i. restore the warning in `bookNegativeSourceClamp` only | `test_negative_sources`' flipped row fails for LEGACY/LARD; the ARD row still passes — **confirms the two gates cover different seams** |
   | ii. restore it in `bookNegativeCellSourceClamp` only | the ARD row fails, the other passes. Mirror of i |
   | iii. delete the summary push as well | the `"D-NS1 summary"` assertions fail — **confirms the round did not remove the last observer**, which is the thing most worth being sure of |
   | iv. an over-extraction deck | `clamp_events > 0` and the summary names a first element ≥ 0. Pins the `first_node` capture that `first_clamp_recorded` now exists solely to do |
   | v. a positive-source deck | no D-NS1 output of any kind. The original lesson-148 guard, unchanged |

6. **Record:** step 2's base failure, falsifiers i/ii, and step 3's `.rpt`
   answer.

## 4. Known gaps

- **The fill-clamp phenomenon itself is untouched.** Extraction still clamps
  ~108 times on a correct deck; we stopped *reporting* each one. Suppressing
  the clamps themselves would need a "the element is still filling" predicate,
  and **"dry" is not one predicate** (lesson 143) — it would have to be
  defined by construction and gated, which is its own round. **Recorded as a
  deliberate non-fix, not an oversight.**
- **No gate asserts the fill caveat's wording** in the summary. If someone
  rewords it, nothing notices.
- **`negsrc.first_node` may hold a mesh CONDUIT row** when an ARD cell clamp
  is first (carried over from P1.4). The summary says "element index", which
  is vague enough to be true and precise enough to be unhelpful. Recorded.
- Still owed from P1.4: a **TIMESERIES-source gate**, and the **MSX ledger
  row** — without which ARD's quality balance cannot close on any deck using
  sources.

## 5. Prepared commit message

```
fix(quality): the D-NS1 per-clamp warning fired on correct models -- retire it

The P1.4 check measured what the warning actually does: every extraction deck
clamps while the system fills, because a near-empty element cannot satisfy any
extraction. A deck extracting 40% of its incoming mass logged 108 clamps and
108 was not the interesting number -- zero of them indicated a problem. A
notice that fires on ordinary decks is one users learn to ignore, which
destroys it on the decks where it matters (lesson 148).

The per-clamp warning is removed from all three seams -- node, age and ARD cell
-- in the shared helper, so LEGACY, ARD and LARD stay identical rather than
drifting apart to spare two of them an edit. The clamp stays observable through
negsrc.clamp_events, negsrc.shortfall_mass and the end-of-run summary, which
now also names the first element (the warning's only unique payload) and states
that clamping during fill is expected, so a nonzero count is not misread as a
modelling error.

runtime_warned becomes first_clamp_recorded: it no longer gates a warning, only
the first_node capture, and a flag called "warned" in code that does not warn
is the trap this program keeps paying for.

Two gate rows are deliberately flipped from "the warning fires" to "the warning
is gone", keeping the observability claim rather than dropping it; the ARD row
additionally pins that the clamp still reaches a user-visible channel, which
its old needle only did incidentally.

Protocol: plans/transport/P1_4B_CLAMP_WARNING_CONTRACT_HANDOFF_2026-08-29.md
```

---

# CHECK RECORD — 2026-08-29

**Verdict: sound; landed on `swmm6_rel` on top of `4b26aa50`** (`fix(quality):
the D-NS1 per-clamp warning fired on correct models -- retire it`). The
changeset is exactly the four files listed; no other site referenced
`runtime_warned` (swept `src include python tests docs`), and no other test
needled the retired strings — the only hits outside the four files were
regenerated `_nx_*.rpt` / `_wa_flux_*.rpt` outputs in `data/` (untracked).
Every number below is from a `git worktree` at `4b26aa50` + these four
files (lesson 172).

## §3.1 / §3.4 — both flips pass, all three engines

`test_engine_negative_sources` 4/4 (the flipped row iterates LEGACY,
EULERIAN_ARD, LAGRANGIAN — each passes), `test_engine_ard_transport_bcs`
13/13. `ctest -j8` ×3: **176/177** each run; the one failure is
`test_engine_2d_infil_integration.SectionsRoundTripThroughTheWriter` ("the
writer dropped [2D_INFILTRATION_OPTIONS]"), pre-existing at HEAD (P1.4's
record already showed it failing against the base dylib).

## §3.2 — the flips at base (quoted)

Against the `4b26aa50` engine, gates kept:

```
LEGACY: the per-clamp runtime warning is supposed to be gone — it fires on ordinary decks (lesson 148)
EULERIAN_ARD: the per-clamp runtime warning is supposed to be gone — …
LAGRANGIAN: the per-clamp runtime warning is supposed to be gone — …
[  FAILED  ] NegativeSourcesTest.OverExtractionClampsWarnsAndStaysNonNegative
the per-clamp runtime warning is supposed to be gone
[  FAILED  ] ArdTransportBcsTest.OverExtractionClampsAndStaysNonNegative
```

Every other row in both suites passes at base, so the flips observe exactly
the change and nothing else moved.

## §3.3 — corpus

**20/20 `.out` byte-identical, 0/20 `.rpt` moved** (timestamps excluded),
and no corpus `.rpt` carries a `D-NS1` line on either side — no corpus deck
was clamping unnoticed. Engine sha256 `67882a68…` vs `f0bb7e8a…`. The base
CLI copy ran through a `DYLD_LIBRARY_PATH` wrapper whose identity was proven
by having it still emit `clamped to the available amount` on the
over-extraction deck. Artifacts: `tests/output/p1_4b_clamp_warning/`.

## §3.5 — falsifiers

| # | expected | observed |
|---|---|---|
| i. warning restored in `bookNegativeSourceClamp` only | 3-engine row fails, ARD row passes | **exactly that** — LEGACY / EULERIAN_ARD / LAGRANGIAN all report the warning; `test_engine_ard_transport_bcs` 13/13 ✓ (note ARD's *node*-inflow test lives in the 3-engine suite, so "EULERIAN_ARD" failing here is the node seam, not the cell seam) |
| ii. restored in `bookNegativeCellSourceClamp` only | ARD row fails, other passes | **exactly that** ✓ |
| iii. summary push deleted | both `"D-NS1 summary"` assertions fail | **both fail** — "the end-of-run summary did not fire" ×3 engines and "the clamp reached no user-visible channel at all" — the last observer is intact ✓ |
| iv. over-extraction deck | `clamp_events > 0`, summary names a first element ≥ 0 | patched `.rpt`s: LEGACY `… 2881 time(s) … first at element index 2`; ARD cell `… 721 time(s) … first at element index 2` ✓ |
| v. positive deck | no D-NS1 output | `WarningsFireExactlyOnNegativeConfigs(b)` passes (asserts no `"D-NS1"` needle, zero counters) and the corpus `.rpt` census is 0 ✓ |

## Notes for the record

- `first_clamp_recorded` was only ever a one-shot for `first_node`; the
  rename is honest (§1's lesson-170 point) and the header change forces a
  wide rebuild for peers in the shared tree — expected, not a finding.
- Test names still say *Warns* (`OverExtractionClampsWarnsAndStaysNonNegative`)
  while the row now asserts the warning's absence; the summary still counts
  as "warns", so it is not false, but the next reader will pause. Left
  alone (CLAUDE.md §3).
- The summary's first element index is a node index at the node/age seams
  and a mesh conduit row at the cell seam, as §4 concedes.

---

## IMPLEMENTER'S NOTE — 2026-08-29 (after the check)

**Committed `0e73f7ea`** on `swmm6_rel`, on top of `4b26aa50`.

**Correction to the CHECK RECORD above.** It opens *"landed on `swmm6_rel` on
top of `4b26aa50`"*, and the covering message says the commit was **blocked**.
The tree agreed with the message: HEAD was still `4b26aa50` and all four files
were unstaged. The verification was real and complete; only the landing had
not happened. **A record written before its last step completes will assert
that step** — worth naming because this program's whole staleness problem is
records that got ahead of the tree.

**The blocking lock was mine, not foreign.** 0 bytes, created 21:16, owned by
the sandbox user — and `.git/` in this repo holds a graveyard of the same
thing (`HEAD.lock.post.*`, `HEAD.lock.stale*`, `_probe`) going back to July.
The `com.apple.Virtualization.VirtualMachine` process the checker spotted is
the sandbox VM: my own git calls orphan empty lockfiles because the mount
permits create and rename but denies unlink. **The holder pattern was
identified correctly and attributed to an unknown third party.** Cleared by
renaming aside, which is the only removal this mount allows.

The checker was right to refuse it. A lock of unknown provenance is not
something to delete on inference — the rule did its job, and the cost was one
round-trip to someone who knew the provenance.

**Also done:** `OverExtractionClampsWarnsAndStaysNonNegative` →
`OverExtractionClampsSummarizesAndStaysNonNegative`, the note left for the
file's next reader. The other three `*Warn*` test names describe **parse**
warnings, which still exist and still fire; they are accurate and untouched.
