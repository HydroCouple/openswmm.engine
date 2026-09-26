# The runoff ledger double-counts cascaded run-on — Handoff (2026-08-22)

**For:** the checking agent.
**Base:** `1da1d7ca` (note HEAD may be `24d51e6e`, xsect perf — see §6).
**Standing findings:** lessons 1–139.
**Found by:** `tests/parity/transport/age_legacy.inp`, **on its first run**,
the first corpus deck ever to route a subcatchment onto another subcatchment.

**This is Finding 1 of the two the new decks turned up. Finding 2 — the
subcatchment `__TEMPERATURE__` column is never written — is NOT in this
changeset and is scoped in §7.**

---

## 1. The defect

| | precip | infiltration | surface runoff | continuity |
|---|---|---|---|---|
| openswmm 6.0 | 6.960 | 4.620 | **3.976 in** | **−23.667 %** |
| legacy 5.x | 6.960 | 4.620 | **2.348 in** | −0.271 % |

Same hydrology. Precipitation and infiltration agree to the digit; **only
runoff differs, by 1.628 in — exactly S1's own shed volume.** S1's water is
booked once when S1 sheds it and again when S2 discharges it.

`SWMMEngine.cpp:2357` added every subcatchment's runoff unconditionally.
Legacy guards precisely this at `subcatch.c:761-765`:

```c
if ( Subcatch[i].outNode == -1 && Subcatch[i].outSubcatch != i )
    vOutflow = 0.0;
```

— a subcatchment whose outlet is not a drainage node, and is not itself,
contributes **zero** to `RUNOFF_RUNOFF`.

**Why it survived fifty rounds:** I checked all fifteen previous corpus decks
and **not one has a subcatchment whose outlet is another subcatchment.**
Cascading is ordinary in real models and was absent from every deck we own.

## 2. The fix, and its blast radius

```cpp
const bool sheds_to_node = ctx_.subcatches.outlet_node[ui] >= 0;
const bool sheds_to_self = ctx_.subcatches.outlet_subcatch[ui] == i;
if (sheds_to_node || sheds_to_self) { ...runoff_runoff += ...; }
```

**Legacy's self-outlet exclusion is carried deliberately.** A subcatchment
routed to *itself* is a real system output, not a cascade. Dropping that
clause would be a silent divergence in a case nobody would think to test.

**⚠ This is a LEDGER term only, and that claim is checkable.** `runoff_runoff`
has exactly four readers, all reporting: the continuity total
(`SimulationContext.hpp:1180`), the report row
(`DefaultReportPlugin.cpp:717`), the mass-balance API
(`openswmm_massbalance_impl.cpp:104`), and its own accumulation. **Nothing
routes water with it.** So:

- **every `.out` must be byte-identical** — including all eighteen corpus
  decks;
- **`.rpt` files move** on any deck with a cascade, and only there.

If an `.out` moves, my reading of the four call sites is wrong and that is
the round's finding.

## 3. Changeset

```
mod: src/engine/core/SWMMEngine.cpp        (the guard + why)
mod: tests/unit/engine/test_massbalance.cpp (+1 gate, ~150 lines)
mod: tests/parity/transport/gen_transport_parity.py  (§5 — ASCII)
mod: tests/parity/transport/*.inp          (regenerated, comments only)
mod: plans/transport/{IMPLEMENTATION_ROADMAP,PROGRESS}.md
```

**The gate is a cascade-vs-direct comparison, not an absolute number.** Two
fixtures differing *only* in `SA`'s outlet — `JN` (node) vs `SB` (peer). An
absolute expectation would pin whatever this build produces; the invariant is
that moving an outlet from a node to a peer must remove that subcatchment's
contribution from the ledger and change nothing else. It asserts
`cascade < direct`, `cascade < 0.75 × direct` (SA is the 70 %-impervious
half, so a token difference would mean the guard fired on the wrong term),
and that **precipitation is unchanged** between the two — without that last
one the fixtures could differ by more than the outlet and nothing else would
count.

**A defect I shipped and caught before you did:** the first draft contained
`EXPECT_NEAR(cascade, cascade, 0.0)`, a tautology. Removed. Lesson 91's shape,
written by the person who keeps citing lesson 91.

## 4. Validation protocol

1. **Build. The new gate must FAIL at base** (revert the `SWMMEngine.cpp`
   hunk only) and pass with it. A gate not seen failing is not a gate.
2. **Full `ctest -j8`**, three times. Expect **160/161** — the new gate plus
   the standing Track I 2D-infiltration failure.
3. **Corpus before/after, two build directories** (`tests/parity/README.md`
   §6). **Expected: 18/18 `.out` identical.** Only `.rpt` continuity should
   move.
4. **Then read the `.rpt`s.** On the three transport decks, runoff continuity
   should go from ≈ −23.667 % to something near legacy's −0.271 %. **Quote
   both numbers.** The other fifteen decks have no cascade and their `.rpt`
   continuity must not move at all.
5. **Falsifier sweep:**

   | falsifier | expected |
   |---|---|
   | i. drop the `sheds_to_self` clause | a self-routed subcatchment stops counting. **⚠ No fixture here has one** — write one, or record that this clause is untested. It is carried from legacy on faith |
   | ii. invert the guard (`!sheds_to_node`) | the gate fails, and the cascade total should *exceed* the direct one |
   | iii. use `outlet_subcatch >= 0` instead of `outlet_node >= 0` | should be equivalent — `PostParseResolver.cpp:1184-1192` sets one and clears the other. **If it is NOT equivalent, some path leaves both set or both clear**, and that is a bigger finding than this changeset |
   | iv. delete the precipitation assertion from the gate | nothing today. It guards against a future edit that changes more than the outlet |
   | v. run a deck with a 3-deep cascade (S1→S2→S3→node) | only S3 should count. **Untested here** — the corpus deck is 2-deep |

6. **Record:** the gate's failure message at base, both continuity numbers,
   and falsifiers i, iii and v — all three are gaps I could not close.

## 5. A separate small thing in the same changeset

The three decks carried a `§` in one comment line, so they were not pure
ASCII and the generator's byte count printed one low. Both fixed; the
generated diff is **two `;;` comment lines and no model row**. `.out` files
must not move — if they do, something reads deck comments.

## 6. Known gaps

- **`sheds_to_self` is untested** (falsifier i) and carried from legacy by
  reading. The most likely thing in here to be wrong.
- **Cascade depth > 2 is untested** (falsifier v).
- **`RUNOFF_RUNOFF` is the only ledger term checked.** If cascading also
  mis-books evaporation or infiltration, this changeset does not look. Legacy
  zeroes only `vOutflow`, so I do not expect it — but I did not measure it.
- **The `.rpt` has no gate.** The unit gate reads the API; nothing asserts the
  report row, and the report row is what a user sees.
- **HEAD moved mid-round** to `24d51e6e` (xsect perf) during the deck round,
  and the validator confirmed the headline `.out` byte-identical on a build
  of it. **This changeset is written against `1da1d7ca`** — rebase and say so
  if the hunk has moved.

## 7. ⛔ Finding 2, NOT in this changeset

**The subcatchment `__TEMPERATURE__` column is never written.** Nodes and
links carry live temperature (−4.147…17.66 °C); every subcatchment reads
**exactly 0.0** all run. `SWMMEngine.cpp:4645` fills the *age* row of
`snapshot.subcatch_quality`; there is no equivalent for temperature, so it
keeps its `assign(…, 0.0)` and the output plugin faithfully writes the zero.
**The value exists** in `ctx_.heat_state.subcatch_runoff_temp`.

Third instance of F8's family — and the handoff's own §4.3 column-presence
check **would have passed**, because the column is there. Only reading the
values caught it. That is lesson **139**: *a column that exists is not a
column that is written. The header is authored once at open; the values are
authored every step, and only one of those is evidence.*

Its own round, with its own falsifiers. It is the smaller of the two: the
column currently reads all zeros, so filling it moves one column on one deck.

## 8. Prepared commit message

```
fix(hydrology): cascaded run-on was booked as a system output

A subcatchment that sheds onto another subcatchment has not left the runoff
system -- its water arrives as run-on and is counted again when the receiver
discharges. The ledger added every subcatchment's runoff unconditionally, so
on a cascade the same water was booked twice.

Measured on tests/parity/transport/age_legacy.inp, whose S1 drains onto S2:
runoff continuity -23.667 % against legacy 5.x's -0.271 % on the same
hydrology, precipitation and infiltration agreeing to the digit and only
Surface Runoff differing -- by exactly S1's own 1.628 in.

Legacy guards it at subcatch.c:761-765: outNode == -1 && outSubcatch != i
zeroes vOutflow before massbal_updateRunoffTotals. The self-outlet exclusion
is carried too -- a subcatchment routed to itself is a real system output.

runoff_runoff is read by the continuity total, the report row and the
mass-balance API and by nothing that routes water, so no .out moves; only
.rpt continuity on decks with a cascade.

It survived fifty rounds because not one of the fifteen decks in the
bit-identity corpus routed a subcatchment onto another subcatchment. The deck
that found it was added the day before, for a different reason.

The gate compares cascade against direct on two fixtures differing only in
one outlet, rather than pinning an absolute number.
```

---

## 9. Validation results (2026-08-22) — COMMITTED `421e95c2`

**The fix lands exactly on legacy's number, moves nothing else, and the three
gaps §6 flagged are all closed. One of them turned up a second defect.**

`ctest` **159/160 three times running** (the standing Track I 2D-infiltration
failure; §4.2 predicted 160/161 — the gate joined the existing
`test_engine_massbalance` binary, so the ctest count is unchanged at 160).
Numbers: `tests/output/runon_ledger_2026-08-22/`.

### 9.1 The gate fails at base, and says why

Reverting the `SWMMEngine.cpp` hunk only:

```
Expected: (cascade) < (direct), actual: 22276.073597149789 vs 18157.173828410578
cascaded run-on is still being booked as a system output
Expected: (cascade) < (direct * 0.75), actual: 22276.07 vs 13617.88
```

**The cascade total EXCEEDS the direct one** at base — the double-count as a
number rather than a percentage.

### 9.2 §4.3 and §4.4: the ledger moves and nothing else does

**18/18 `.out` byte-identical** across two build directories with genuinely
different engine libraries (`7de1e636…` base, `598644dc…` patched; no vacuity
note). §2's claim that `runoff_runoff` has four readers and none of them
routes water is verified rather than argued.

Runoff continuity, base → patched, all eighteen:

| deck | base | patched |
|---|---|---|
| `age_legacy` | −23.667 | **−0.271** |
| `age_ard` | −23.667 | **−0.271** |
| `heat_parity` | −23.667 | **−0.271** |
| the other fifteen | unchanged to the digit | unchanged |

**−0.271 % is legacy 5.x's figure on the same hydrology, to three decimals.**
Not "near legacy" — the same number.

### 9.3 ⛔ Falsifier i closed the gap AND found a second defect

§6 called `sheds_to_self` "the most likely thing in here to be wrong". **It is
right, and it is live** — dropping the clause takes a self-routed
subcatchment's booked runoff from **2.328 in to 0.123 in**, a 19× change, so
the parser really does set `outlet_subcatch[ui] == i` and the clause is
exercised.

But writing the self-route fixture the falsifier needed turned up something
else. All five probe decks, ours against legacy:

| deck | ours | legacy |
|---|---|---|
| `direct` (SA→JN, SB→JN) | 0.417 | 0.417 |
| `cascade` (SA→SB→JN) | 0.218 | 0.218 |
| **`selfroute` (SA→SA)** | **2.328** | **0.417** |
| `three_deep` (SA→SB→SC→JN) | 0.318 | 0.318 |
| `three_flat` (all→JN) | 0.625 | 0.625 |

**Four of five agree to the digit. `selfroute` does not**, and our continuity
on it is **−265 %**.

The ledger is not the problem — the guard counts SA correctly, which is why
removing the clause changes the number. **The divergence is one layer up, in
the routing.** Legacy refuses to feed a subcatchment's runoff back into
itself (`subcatch.c:546-548`):

```c
k = Subcatch[subcatchIndex].outSubcatch;
if ( k >= 0 && k != subcatchIndex ) { subcatch_addRunonFlow(k, q); ... }
```

**Legacy carries `!= subcatchIndex` in three places** — the run-on
distribution (546), the ledger (763), and surface quality
(`surfqual.c:363`). This changeset gives us the second. **We do not have the
first**, so a self-routed subcatchment recirculates its own runoff
indefinitely and the ledger faithfully reports a hydrology that itself
diverges.

**Not fixed here.** It changes routed water rather than a ledger term, it
deserves its own falsifiers, and no corpus deck has a self-route. Recorded as
owed, with the fixture already written
(`tests/output/runon_ledger_2026-08-22/decks/selfroute.inp`).

### 9.4 Falsifiers iii and v: both gaps closed, both clean

- **iii — `outlet_subcatch < 0` in place of `outlet_node >= 0`**: byte-
  identical ledger totals on all five decks. The two really are complements,
  so `PostParseResolver` sets exactly one. §5's "if it is NOT equivalent that
  is a bigger finding" does not fire.
- **v — a 3-deep cascade** (`SA→SB→SC→JN`, written for this round): books
  **0.318** against the all-direct control's **0.625**, so only `SC` counts.
  The guard is per-subcatchment and depth is irrelevant to it. Legacy agrees
  to the digit on both.

### 9.5 Falsifiers ii and iv

- **ii — invert the guard**: the gate fails, but **not where §5 predicted**.
  It fails on the SETUP leg, `ASSERT_GT(direct, 0.0)` with `direct` = 0,
  because inverting stops *both* fixtures' node-outlet subcatchments counting
  and there is no total left to compare. The prediction ("the cascade total
  should exceed the direct one") describes the un-inverted base, not this.
  The gate still fails, and it fails at the leg designed to catch a fixture
  that produced nothing.
- **iv — drop the precipitation assertion**: passes, as predicted. It guards
  a future edit, not today's.

### 9.6 §5's ASCII fix

The three decks are now pure ASCII (13956 / 13984 / 14591 bytes = characters).
The generated diff is comment-only and **no `.out` moved** — the 18/18 above
was run with the regenerated decks, so nothing reads deck comments.

### 9.7 What is still owed

- **The self-route routing guard** (§9.3) — its own round. It is the twin of
  the guard this changeset landed, one layer up.
- **Finding 2**, the subcatchment `__TEMPERATURE__` column (§7), untouched.
- **§6's remaining gaps stand**: only `RUNOFF_RUNOFF` was checked — if
  cascading also mis-books evaporation or infiltration nothing here looks,
  though legacy zeroes only `vOutflow`; and **the `.rpt` row still has no
  gate**, only the API does.
- **A transient, worth recording**: mid-round the tree briefly failed to
  build `openswmm_gpu_omp` (`ExplicitKokkosSurfaceSolver.cpp`, `devCopy`)
  while another session was editing the 2D headers. It cleared on its own
  within minutes. Nothing to do with this changeset; noted so a future
  bisect does not chase it.
