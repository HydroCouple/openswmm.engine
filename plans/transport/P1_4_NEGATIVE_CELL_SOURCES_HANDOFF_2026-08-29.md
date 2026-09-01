# P1.4 — negative `[TRANSPORT_SOURCES]` rows are extraction — Handoff (2026-08-29)

**For:** the checking agent.
**Base:** HEAD at time of writing (engine `swmm6_rel`).
**Step:** `FINALIZATION_SEQUENCE_2026-08-29.md` step 1.
**Standing findings:** lessons 1–170.
**Implemented syntax-only.** Nothing built, linked or executed.

```
mod: src/engine/quality/NegativeSources.hpp                        (+bookNegativeCellSourceClamp)
mod: src/engine/transport/components/EulerianArdComponent/ArdEngine.cpp  (sign carry + clamp)
mod: src/engine/transport/components/EulerianArdComponent/ArdConfig.cpp  (parse warning)
mod: tests/unit/engine/test_ard_transport_bcs.cpp                  (+3 gates)
```

---

## 0. ⚠ The round's scope changed mid-implementation. Read this first.

The user decided this clamp should get **its own ledger row**. I began
implementing that, then found the decision could not be executed as stated,
reverted the two edits that assumed it, and re-asked. **The premise I offered
the choice on was one I had not checked** — my error, recorded as such.

**Why the row is impossible today, verified in source:**

1. `ArdEngine.cpp:379` — `src_srow_.push_back(np + cfg.src_msx[i])`. Cell
   sources resolve to **MSX species rows only**, always `np + msx_index`.
   Boundaries too (`bc_srow_`, `:357`). A `[TRANSPORT_SOURCES]` row can never
   name a pollutant — and this is **deliberate and already gated**
   (`ArdTransportBcsTest.PollutantRowsAreRefused`).
2. **MSX species have no mass-balance row at all.** `grep -c 'mass_balance\.'`
   across the whole `EulerianArdComponent/` directory returns **zero**.

So a per-pollutant row would be **permanently zero**. The user re-decided:
**count-and-warn now, widen the ledger as its own round.** That is what this
changeset does.

**Consequence to keep visible:** delivered cell-source mass still reaches
`qual_routing_final` through the end-of-run inventory sweep
(`SWMMEngine.cpp:4290`, an `=` assignment from state), so it is not invisible —
it surfaces as **CONTINUITY ERROR**. **ARD's quality balance cannot close on
any deck that uses sources**, before or after this round. That is the deferred
round's job, and it is recorded in `NegativeSources.hpp`'s doc comment so the
next reader meets it at the code rather than in a plan.

## 1. The defects fixed

**(a) A negative source did nothing, silently.** `updateTransportRows` ended
with `src_now_[i] = std::max(0.0, r)` — the rate was zeroed before the apply
loop saw it. **This is the same shape X6 fixed at the loader seam in this very
file** ("silently DROPPED negative loads"); cell sources were scoped out of
D-NS1 (X6 §2.5) and kept the defect. Now the sign is carried.

**The BC clamp one loop above is deliberately NOT changed.** A boundary
*concentration* below zero is meaningless; a source *rate* below zero is
extraction. Same expression, different quantity — noted in the code so the
asymmetry does not read as an oversight.

**(b) No clamp existed**, because no negative ever arrived. Extraction now
clamps per cell to the mass that cell holds, counts through the D-NS1 seam,
and warns once.

**(c) A parse-time warning**, mirroring the node seam's warning for negative
`[INFLOWS]` baselines. VALUE rows only — a TIMESERIES that dips negative at
runtime is covered by the clamp warning (X6 §2.6's caveat, same reasoning).

## 2. Design decisions to challenge

1. **Per-cell clamp, not per-conduit.** The source's share is distributed
   ∝ dx, so a conduit with unevenly loaded cells can clamp in one cell while
   another still has mass. Clamping on a conduit total would over-extract the
   poor cells.
2. **Shortfall is a MASS** (`unmet_conc · a · dx`), matching
   `negsrc.shortfall_mass`'s units at the node seam, so the summary line adds
   apples to apples across engines.
3. **Counted, not ledgered** — forced by §0, not chosen. The
   `bookNegativeAgeClamp` precedent.
4. **`r == 0.0` rather than `r <= 0.0`.** Zero is still nothing to do.

## 3. Bit-inertness argument — and why it needs checking, not trusting

For a **positive** `r` the arithmetic is unchanged: `dphi` is computed by the
same expression in the same order and added to the same cell. I introduced a
local and a reference, which should not move a bit.

**But that is a claim about codegen, and the corpus is how it gets tested.**
Every ARD deck must be byte-identical. **A moved deck is a finding.**

## 4. Validation protocol

1. **The gates must FAIL at base.** Revert the three source hunks, keep the
   gates. Expect `NegativeSourceExtractsMass` to report ~5.0 where 3.0 is
   expected (extraction silently zeroed). **Quote it.** If it passes at base,
   something else already handles negative rates and §1(a) is wrong.
2. `ctest -j8` ×3 — expect **177** with no new failure.
3. **Corpus: 20/20 `.out` byte-identical.** No corpus deck uses
   `[TRANSPORT_SOURCES]`, so this round should be inert; §3's claim is what is
   under test.
4. **Check `.rpt` too.** The parse warning and the clamp summary reach
   `ctx.warnings`. **Confirm no corpus deck gains a warning line** — if one
   does, a positive-source deck is tripping the negative branch.
5. **Falsifier sweep:**

   | falsifier | expected |
   |---|---|
   | i. restore `std::max(0.0, r)` | `NegativeSourceExtractsMass` fails — the round's core claim |
   | ii. remove the clamp (`dphi = -phi`), keep the sign | `OverExtractionClampsAndStaysNonNegative` fails: `clamp_events == 0` and cell concentrations go negative |
   | iii. clamp but skip `bookNegativeCellSourceClamp` | the count/warning legs fail while extraction still works — **confirms the gate observes the bookkeeping, not just the physics** |
   | iv. drop the `!row.is_ts` condition on the parse warning | no gate fails today (**no TIMESERIES source gate exists** — recorded as owed in §6) |
   | v. a POSITIVE source deck (gate 3's `_e5_src`) | unchanged and byte-identical. **A clamp that fires on ordinary decks is lesson 148's failure** |
   | vi. extraction of exactly the held mass | boundary case: expect cells at zero, and **report whether a clamp is counted** — I predict NOT (the `phi + dphi < 0.0` test is strict), but I have not run it and the answer belongs in the record either way |

6. **Record:** step 1's base numbers, falsifiers iii and vi, and step 4's
   `.rpt` answer.

## 5. Anticipated failure modes, likelihood order

1. **Gate 1's tolerance.** I reused gate 3's 15 % band by analogy. The
   extraction case has a different error structure — the source competes with
   advection rather than adding to a clean stream. **If it fails marginally,
   measure the achievable floor and re-pin; do not widen the band to fit**
   (lesson 149).
2. **Gate 2's deck may clamp on the very first step** before the chain wets,
   which is a legitimate clamp but not the one the gate is about. If
   `clamp_events` is nonzero only at startup, tighten to "clamps after the
   chain is wet" rather than accepting a startup artefact.
3. **`_e5_nover`'s run may not reach steady state** within the deck's horizon;
   the gate only asserts clamping happened, so this should be safe, but if the
   run ends before C3 sees flow the gate is vacuous — **check `clamp_events`
   is not passing for the wrong reason.**
4. `mesh_.cell_dx[uc]` indexing — I use the same `uc` the concentration uses.
   If `cell_dx` were conduit-indexed rather than cell-indexed the shortfall
   would be scaled wrong; I read it as cell-indexed
   (`NetworkMeshData.hpp:184`, "cell length (ft)"). **Cheap to confirm.**

## 6. Known gaps

- **The ledger row is deferred**, and with it ARD's ability to close a quality
  balance on any source-using deck (§0). **This is the largest known gap in
  the ARD engine and it is now written at the code.**
- **No TIMESERIES source gate** (falsifier iv) — the parse warning's `is_ts`
  branch is unobserved.
- **The dry-cell undelivered remainder is still dropped.** `ArdEngine.cpp`'s
  5c comment claims *"the mass-balance ledger (E5b) will book the undelivered
  remainder explicitly"* — **E5b never did.** A source into a conduit whose
  cells are dry loses that share with no record. Untouched by this round;
  it belongs with the ledger round. **A comment promising behaviour no code
  provides is worse than silence** (lesson 154's family) — flagging it rather
  than fixing it here, because fixing it needs the row that does not exist.
- **`negsrc.first_node` now sometimes holds a mesh CONDUIT row**, not a node
  index, depending on which clamp fires first. The field is diagnostic-only
  and the warning text says which it is, but the name is now imprecise.
  Recorded rather than renamed (CLAUDE.md §3).

## 7. Prepared commit message

```
fix(ard): a negative [TRANSPORT_SOURCES] row is extraction, not a no-op

updateTransportRows ended with src_now_[i] = std::max(0.0, r), so a negative
cell-source rate was zeroed before the apply loop ever saw it: the row did
nothing, and said nothing about doing nothing. That is the same shape X6 fixed
at the loader seam in this file; cell sources were scoped out of D-NS1
(X6 sec 2.5) and kept the defect.

The sign is now carried, extraction clamps per cell to the mass that cell
holds, and the clamp is counted and warned through the D-NS1 seam. A negative
VALUE row also warns once at parse time, mirroring the node seam's warning for
negative [INFLOWS] baselines. The boundary-concentration clamp one loop above
is deliberately unchanged -- a negative concentration is meaningless where a
negative rate is extraction.

The clamp is COUNTED, not ledgered, and that is forced rather than chosen:
[TRANSPORT_SOURCES] rows resolve to MSX species rows only (np + src_msx, and
pollutant rows are refused by an existing gate), and MSX species have no
mass-balance row anywhere -- the ARD component writes to ctx.mass_balance in
zero places. A per-pollutant row for this would be permanently zero. Giving
MSX species real ledger rows is a deliberate separate round; until it lands,
delivered source mass reaches qual_routing_final through the end-of-run
inventory sweep and therefore surfaces as continuity error, so ARD's quality
balance cannot close on a deck that uses sources. Recorded in
NegativeSources.hpp so the next reader meets it at the code.

Three gates in test_ard_transport_bcs.cpp: extraction lowers the downstream
concentration by the analytic amount and leaves upstream alone; over-extraction
clamps, counts and warns without driving a cell negative; the parse warning
fires on a negative VALUE row and only then.

Positive-source decks are bit-identical by construction (same expression, same
order); the corpus is what tests that claim.
```

---

# CHECK RECORD — 2026-08-29

**Verdict: the round is sound AFTER two corrections; landed on `swmm6_rel`
(see `git log` for the hash — `fix(ard): a negative [TRANSPORT_SOURCES] row
is extraction, not a refusal`).** The handoff was written syntax-only and
its account of the base behaviour was wrong in a way its own gates exposed.

## Step 1 — the gates at base (quoted)

All three FAIL at base — but **not as §1(a) predicted.** The decks do not
open:

```
[TRANSPORT_SOURCES] 'C3': VALUE '-7079.211648' is not a non-negative number.
open failed for _e5_nsrc.inp        swmm_engine_open == 5
```

`ArdConfig.cpp:261` applied the boundary rule (`v < 0.0` → error) to BOTH
`[TRANSPORT_BOUNDARIES]` and `[TRANSPORT_SOURCES]`, so a negative VALUE row
was a **hard refusal at parse**, never a silent zero. The `std::max(0.0, r)`
in `updateTransportRows` guarded only the TIMESERIES path (a series dipping
negative at runtime). The handoff's patch never touched `:261`, so **with the
patch applied unchanged all three gates still fail identically** — the round
as delivered could not pass its own gates. Corrected: the source branch
accepts a signed rate (`is_bc && v < 0.0` refuses only boundaries; the
message splits accordingly). §1(a) of this handoff and the prepared commit
message were rewritten to say what base actually did.

**Second correction:** gate 2 called `swmm_engine_start` without
`swmm_engine_initialize` (`run_recording` does both) →
`SWMM_ERR_LIFECYCLE (6)`. Added the call.

**Strengthened:** gate 2 asserted only counts/warnings; its title claimed
non-negativity it never observed. It now tracks the extracted conduit C3's
projected concentration (min ≥ 0) and C5's (min ≥ 0, final < 0.75 mg/L).
The FIRST version watched C5 only, and falsifier ii did NOT bite on it —
the volume-weighted projection carries the sign in C3, but the node
hand-off does not propagate a negative donor, so C5 read 0. Observe the
cell you clamp. Gate 1's band tightened from 15 % to 1 % (measured floor
7e-8: `c5.back() = 3.0000000706`, `c1.back() = 5.0000000000`).

## §4.2 ctest — 176/177 ×3, ISOLATED worktree at HEAD

The shared tree is a moving target: peer edits to `DynamicWave.cpp/.hpp`
and `Routing.cpp` landed between my base snapshot and the patched build and
produced `std::bad_alloc` in every ARD run — indistinguishable from a P1.4
defect until the base test binary + patched dylib / patched binary + base
dylib swap pinned it to the dylib and `find -newer` pinned it to the peer
files. **Every number in this record comes from a `git worktree` at
`5b4cffb8` with only the four P1.4 files applied**, configured with
`build/darwin`'s options. The one failure,
`test_engine_2d_infil_integration.SectionsRoundTripThroughTheWriter`
("the writer dropped [2D_INFILTRATION_OPTIONS]"), fails identically against
the base dylib — pre-existing at HEAD (the shared tree carries an
uncommitted `InpWriter.cpp` that is presumably its fix).

## §4.3–4.4 corpus

**20/20 `.out` byte-identical**, engine sha256 `9eb6d163…` (base) vs
`8261693…` (patched) — a real dylib-level A/B. The base CLI copy's
`LC_RPATH` pointed at the worktree's build dir (lesson 135 exactly), so it
ran through a `DYLD_LIBRARY_PATH` wrapper whose identity was proven by
having it REFUSE the negative deck. **No corpus `.rpt` moved** (timestamps
excluded) and no deck gained a `WARNING`/`D-NS1` line (census: 0/0 on 14
decks, 12/12, 12/12, 13/13, 11/11, 1/1, 1/1 unchanged on the rest).
Artifacts: `tests/output/p1_4_negative_cell_sources/`.

## §4.5 falsifiers

| # | expected | observed |
|---|---|---|
| i. `std::max(0.0, r)` restored | gate 1 fails | **fails: `c5.back() = 4.99999960`** vs 3.0 (extraction zeroed; deck now opens, so this is the runtime half of the defect). Gate 2 also fails (no clamp, species left downstream) ✓ |
| ii. clamp removed, sign kept | gate 2: `clamp_events == 0`, negatives | **bites on all three legs once C3 is watched: `c3_min = −1.322`**, never clamped, no unmet mass. On the C5-only version only the count legs bit — see Step 1 ✓ |
| iii. clamp kept, booking skipped | count/warning legs fail, physics passes | **exactly that** — "never clamped" + "no unmet mass" while both physics legs pass: the gate observes the bookkeeping ✓ |
| iv. `!row.is_ts` dropped | nothing fails | **nothing fails** (13/13) — the TIMESERIES-source gap is real and stays owed ✓ |
| v. positive-source deck `_e5_src` | byte-identical, no warning | **`.out` IDENTICAL, 0 D-NS1/EXTRACTION lines** ✓ |
| vi. extraction of exactly the incoming mass (r = c_in·Q) | "predict NOT counted" | **COUNTED: 169 clamps, 12 361 units unmet.** The strict-`<` question never arises because the fill transient clamps regardless — see the finding below |

## Finding for the vocabulary owner — every extraction deck warns

Gate 1's deck extracts 40 % of the incoming mass at steady state — plainly
feasible — and still logs **108 clamps (3 760 units unmet)** while the chain
wets: a freshly wet cell holds ~0 and the extraction is asked of it at once.
So the **runtime clamp warning fires on every extraction deck** and is not,
by itself, a sign of a mis-specified model. That is D-NS1's contract as
written (count and summarise) and the node seam presumably behaves the same
during fill, but it is lesson 148's shape for the RUNTIME line (falsifier v
only checked the parse line on positive decks). Options are a wet-cell /
steady-state qualifier on the warning, or accepting it. **Not resolved
here; a reporting-contract call.**

## Also done

- `summarizeNegativeSourceClamps`'s text said "The ledger carries the mass
  actually removed" — false for cell clamps after this round. It now says
  node-seam clamps are ledgered and ARD cell-source clamps are counted only.
- Verified §5.4: `cell_dx` is cell-indexed (`NetworkMeshData.hpp:184`,
  `n_cells() == cell_dx.size()`); the shortfall scaling is right.
- §0's claim verified: `grep -c 'mass_balance\.'` over the ARD component is
  0; `bookNegativeAgeClamp`'s un-booking has no cell analogue.
- Staged by pathspec after confirming the shared-tree hunks for the four
  files are byte-identical to the isolated tree's (313-line diffs `cmp`
  equal), so nothing foreign rode along; peer's uncommitted `ArdEngine.hpp`
  comment change left untouched.
