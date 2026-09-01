# Snow track — status after S4, and the road ahead

**Written:** 2026-08-21 · **HEAD:** `2992f7c5` on `swmm6_rel`, **3 ahead of
`hydrocouple/swmm6_rel`, unpushed** · **Scope:** the snow sub-track of the
Unified Transport Program (Phase 1), and what it unblocks.

**Provenance:** every number below was re-measured in the working tree today,
not copied from the handoffs. Where a handoff claim and the tree disagree it is
called out.

---

## 1. Verdict on the last completed task (S4)

**S4 is complete and the report of it is accurate.** Independently confirmed:

| claim | verified how | result |
|---|---|---|
| S3 then S4, in order | `git log` | `c316c83e` → `2992f7c5`, S4 on S3 ✅ |
| 158/158 ctest | `ctest_full.log` tail | `100% tests passed, 0 failed out of 158` ✅ |
| 14/14 decks byte-identical | `cmp` of all 14 `.out` against the `mf_validation_2026-08-19` baseline | 14 SAME, 0 DIFF ✅ |
| ASan clean, 71 tests | `asan_snow.log` and siblings | 35 + suites, no findings ✅ |
| falsifier sweep 3 of 5 + 1 new | `falsifier_sweep.log` | 0, i, ii fail as predicted; iii and iv escape ✅ |
| F7 actually fixed | `Snow.cpp:54` `awe.assign(total, 1.0)` vs legacy `snow.c:199` `awe[i] = 1.0` | matches ✅ |
| gate count | `test_transport_snow.cpp` | 18 gates (11 → +4 S3 → +15b → +2 S4) ✅ |
| no reference deck has `[SNOWPACKS]` | grep over the corpus | 0 hits ✅ |
| register uncommitted | `git ls-files plans/` returns nothing — `plans/` is ignored wholesale | consistent with the standing rule ✅ |

**The single most important thing in this round is not a fix, it is a
retraction.** D1 — the claim that legacy's `0.0172615` was a botched `2π/365` —
was the program's own error, and it was recorded as an upstream EPA defect for
two rounds. 364 = 4 × 91 puts the melt peak exactly on day 172, the summer
solstice. That correction is now the register's §1a and the lesson it carries
(*a magic constant that looks wrong may be a calibration*) is worth more than
either code change in the commit.

**Second most important: gate 16 failed for the right reason.** Reading `SD100`
was necessary and not sufficient — `awe` initialised to 0 where legacy uses 1.0,
which pinned cover at 1 by a second, independent mechanism. The gate asserted
the *observable* (`asc < 1`), not the mechanism, so it survived the changeset
being half-right. That is the design property to keep; a gate written against
`si` would have passed and shipped F7.

### 1.1 One nit worth fixing in the artefacts

`tests/output/s4_validation_2026-08-20/run_decks.sh` opens with *"This changeset
touches NO engine source — only a test file and one CMakeLists line."* That is
inherited boilerplate from an earlier round and is false for S4, which touches
`Snow.cpp` and `SWMMEngine.cpp`. The 14/14 result stands anyway — it stands
because no reference deck has snow, which is a *different* reason than the one
the script states. Worth correcting so the next reader does not inherit the
wrong justification.

---

## 2. Where S4 sits — the end-to-end map

The snow work is a **detour inside Phase 1**, opened by S1 and not on the
original roadmap. It exists because `SnowSolver::setMeltCoeffs` had no caller
anywhere in `src/engine/`, so degree-day snowmelt had never fired in this
engine's history, and four accounting defects sat behind it unreachable.

```
Phase 1 — 1D transport on existing formulations        ACTIVE, ~34 of ~45
  ├── E-suite (ARD engine)          ✅ complete
  ├── Species registry / MSX        ✅ complete
  ├── Water age (A1–A4)             ✅ complete for 1D
  ├── Heat (H1–H5b, D-H5a/b/d/e)    ✅ through H5b · H6–H7 ⬜
  ├── I/O config (IO1–IO2)          ✅ · IO3–IO6 ⬜ (IO3 status disputed)
  └── SNOW DETOUR
        F1   melt coefficients never applied      ✅ 274b6506
        S1   mixing volume under a pack           ✅ d7ee70be
        S2a  meltwater arrives at 0 °C            ✅ 8b7d1cf7
        S3   water balance: F2–F5                 ✅ c316c83e
        S4   SD100 + F7 + D1 retraction           ✅ 2992f7c5   ← YOU ARE HERE
        S2b  pack AGE model                       ⬜ NOW UNBLOCKED
Phase 2 — HydroCouple (HC1–HC4)                        ⬜ 0 of 4
Phase 3 — 2D surface transport (S1–S7)                 ⬜ 0 of 7
Phase 4 — Groundwater (G0–G4)                          🔄 plan delivered, G0 sign-off owed
Phase 5 — LARD                                         ⬜ 0 of 4
GUI track                                              ⬜ 0 of 14
```

> **Naming hazard.** Phase 3's steps are *also* called S1–S7 in
> `IMPLEMENTATION_ROADMAP.md`. The snow rounds are S1, S2a, S2b, S3, S4; the 2D
> rounds are S1–S7. Two live tracks share five labels. Renaming the snow track
> (`SN1…SN4`) costs an hour now and prevents a wrong-file merge later.

### 2.1 The snow defect ledger, end to end

| # | What was wrong | Landed |
|---|---|---|
| F1 | `setMeltCoeffs` had no caller — degree-day melt never fired | `274b6506` |
| — | S1: mixing volume under a pack read `rainfall`, not `snow_net_*` | `d7ee70be` |
| — | S2a: meltwater arrived at the deck's RAINFALL temperature, not 0 °C | `8b7d1cf7` |
| F2 | SWE reduced by the **drained excess**, not by the melt — mass creation | `c316c83e` |
| F3 | Rain on the covered fraction discarded — left the balance entirely | `c316c83e` |
| F4 | Free-water capacity taken from **pre-melt** SWE — 1.0 % of runoff volume | `c316c83e` |
| F5 | Instant-melt branch's water assigned over and discarded | `c316c83e` |
| F6 | The deck's `SD100` never read — every snow deck sat at `asc = 1` | `2992f7c5` |
| F7 | `awe` initialised to 0 where legacy uses 1.0 — cover pinned a second way | `2992f7c5` |
| D1 | ~~legacy's seasonal constant is wrong~~ — **RETRACTED, ours was** | `2992f7c5` |

**Every divergence found in the snow module has been the engine's.** Nothing is
reportable upstream to EPA. That was the question S4 was opened to answer, and
it now has a documented answer.

**The compounding pattern is the story of this track.** F2–F5 were unreachable
until F1 gave melt something to mis-account. F7 was unreachable until F6 stopped
pinning `si`. Three of S3's four gates passed with their own defect fully
restored, because the shared deck writer starts every pack *ripe* — a store at
capacity drains every drop instantly, so "SWE −= melt" and "SWE −= excess" are
arithmetically the same number. **Each fix is what made the next one visible.**
Expect that to continue rather than to stop here.

---

## 3. What is still open

### 3.1 Owed by this round (small, do first)

1. **`SNOW_DIVERGENCE_REGISTER.md` needs three edits, not the two reported.**
   - **F7** as a new §2 row (currently absent).
   - **Falsifier iv** recorded as *provably redundant*: `si <= 0 || wsnow >= si`
     — when `si <= 0`, the second condition already answers for every
     non-negative `wsnow`, so the ADC branch is unreachable and the guard cannot
     be falsified. Same shape as S1's falsifier v.
   - **§4** still reads *"No reference deck is **known** to contain
     `[SNOWPACKS]`"* and calls confirming it a prerequisite. It was confirmed, in
     both S3 and S4. Leaving it phrased as unconfirmed keeps a settled question
     open in the document of record.

2. **Decide whether the register is tracked.** S3 §2 lists it as part of the
   changeset; the standing rule keeps workplans out of the tree, and `plans/` is
   ignored wholesale. This is a genuine tension, not an oversight: the register
   is not a workplan — it is **the document a user needs in order to interpret a
   deck comparison against EPA SWMM**, and it is the only artefact that explains
   why S3 moves their runoff volumes. *Recommendation: move it out of `plans/`
   to `docs/` (or `CHANGELOG` prose) and track it.* The engineer flagged this
   correctly and it needs your word.

3. **Falsifier iii stays open by design** — writing `SD100` into `si` for
   `PLOWABLE` is unobservable because `getArealDepletion` returns 1.0 for
   `PLOWABLE` before reading `si`. Harmless today; the reason to record it is
   that an unobservable write is how a later change acquires a wrong premise.

### 3.2 Both plan documents are two rounds stale

- **`PROGRESS.md`** says *"As of 2026-08-20 · HEAD `8b7d1cf7`"*. S3 and S4 are
  absent. Its test-evidence table still reads `test_transport_snow.cpp | 11`; it
  is 18.
- **`IMPLEMENTATION_ROADMAP.md`** has no S3 or S4 row at all, and two of its
  live entries are now false:
  - line ~1350, **"⬜⬜ BLOCKING S2b — two holes in the snow water balance"** —
    both holes are closed by `c316c83e`. **S2b is no longer blocked.**
  - line ~1366, **"⬜ OWED (parity) — `Snow.cpp:349` uses `2π/365`… decide
    whether legacy parity or the corrected constant is wanted"** — decided, and
    the decision went the opposite way to the entry's framing.

### 3.3 Carried, not owed by this round

- **O3** — whether a LID under a pack should receive the snow-modified rate.
  `SWMMEngine.cpp:1735` feeds the LID `subcatches.rainfall` for both hydrology
  and transport, so the two agree; whether that shared value should be
  snow-modified is a fidelity question, deliberately unanswered since S1.
- **The bit-identity corpus cannot see any of this work.** No reference deck has
  `[SNOWPACKS]`, none has `WATER_AGE` on, none has heat. "14/14 unchanged" proves
  the pollutant path is undisturbed and nothing else. After four rounds of snow
  fixes this has stopped being a caveat and become a gap.
- **G0 sign-off** — groundwater plan decisions D-N1–N5, sitting since 2026-08-15,
  cheap review work that gates all of Phase 4.
- **The dry-link hotstart gate never landed** — exists only in a working tree.
- **Dry elements report a carried temperature indefinitely** — needs a per-column
  no-data sentinel the `.out` format does not have.

---

## 4. Next steps, in order

**Now — close out S4 (under an hour)**

1. Make the three register edits in §3.1.
2. Correct the `run_decks.sh` header comment (§1.1).
3. Answer the tracked-register question (§3.1 item 2).
4. Refresh `PROGRESS.md` and `IMPLEMENTATION_ROADMAP.md`: add the S3 and S4 rows,
   clear the "BLOCKING S2b" marker, close the seasonal-constant parity item with
   the retraction, correct the gate count.
5. **Push.** Three commits — `8b7d1cf7`, `c316c83e`, `2992f7c5` — have been
   sitting unpushed, and two of them move hydrology on every deck with snow.

**Next — S2b, the pack age model**

Scoped from a completed survey in `S2A_MELT_TEMPERATURE_HANDOFF §7`, and now
resting on a balance that has been fixed rather than one with two known holes.
That ordering was the explicit call — an age model built on a leaking balance
inherits the leak, and calling that an approximation is lesson 64.

The design as scoped: one age per snow surface (`n_subcatch × 3`), complete-mix
over `wsnow + fw` together, ageing by `+dt`, snowfall mixed in at the RAINFALL
source age, melt leaving at the pack's age without changing it.

Two decisions S2b must make before it writes code:

- **The published plow transfer.** `plowSnow` moves water between surfaces *and*
  between subcatchments inside `snow_.execute`, so an age update running
  afterwards cannot reconstruct which water went where. Follow A4: the values
  exist as locals, publish them. **This means S2b touches hydrology** — say so up
  front rather than discovering it mid-round.
- **Hotstart.** Pack SWE *is* persisted (`SWMMEngine.cpp:5646`), so unlike A2a's
  case the volume would be restored, which makes persisting the age possible —
  and therefore a decision rather than an impossibility.

**Also worth doing before S2b, not after — build a snow parity deck**

Four rounds of hydrology fixes have landed against a corpus structurally
incapable of observing any of them. One deck with `[SNOWPACKS]`, `SD100` set and
a real ADC curve, added to the 14, would turn "byte-identical because nothing
tested it" into "byte-identical where it should be and moved where it should
have moved" — and it is the only thing that would have caught F1 in the first
place. **It also becomes a prerequisite the moment S2b lands, because S2b is the
first round whose correctness the current corpus cannot even in principle
confirm.**

**Then — back to the mainline**

The snow detour ends at S2b. Phase 1 resumes at **H6–H7** (HTS sediment layer,
which also carries H3's deliberately-omitted shortwave bed split, and which
forces the node/link merge decision) and **IO3–IO6**. G0 sign-off remains the
cheapest unblocking action available anywhere in the program.

---

## 5. One judgement to carry forward

This track has now produced two errors of the same shape in opposite directions:
D1 read a calibrated constant as an arithmetic slip and "corrected" it; F6/F7
read a pinned field as a design choice and left it alone. Both were resolved the
same way — **by reading legacy and measuring what the value makes true**, not by
reasoning about what it ought to be. The register exists so that judgement is
recorded rather than re-litigated. Keeping it accurate is worth more than the
next fix.
