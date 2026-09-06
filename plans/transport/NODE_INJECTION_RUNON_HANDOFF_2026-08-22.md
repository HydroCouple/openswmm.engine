# The node injection double-counts run-on — Handoff (2026-08-22)

**For:** the checking agent.
**Base:** `84984990`.
**Standing findings:** lessons 1–148.
**This is Finding 4**, and it is the largest of the four.

---

## 1. The defect

| deck | legacy | ours |
|---|---|---|
| cascade | 0.218 | **0.511** |
| three_deep | 0.318 | **0.536** |

`SWMMEngine.cpp:2183` fed each outlet node `q_runoff + q_runon`. Legacy's
`subcatch_getWtdOutflow` is `(1-f)·oldRunoff + f·newRunoff` **and nothing
else**. The excess on cascade is **0.293 acre-feet against the donor's own
runoff of 0.294**.

**Our conveyance was receiving 2.3× what our own runoff ledger said left the
surface, and neither continuity check noticed** — each balance was
self-consistent on its own side of the seam. Lesson 147.

## 2. ⛔ The per-contributor question, answered — and it answers uniformly

My last handoff called this "most of the round" and warned it was not a
one-line deletion, because `runon_inflow` also carries LID-drain (Gap #25)
and outfall (Gap #28) run-on. **The answer is that all three are already
inside `runoff[]`, and the reason is structural:**

`assembleRunon` sums **every** contributor into one array, `runon_inflow[]`.
`Runoff.cpp:331-333` then consumes that array **wholesale**:

```cpp
double runon_q = ctx.subcatches.runon_inflow[ui];   // CFS
if (runon_q > 0.0) precip += runon_q / total_area;  // ft/sec
```

**The solver does not distinguish where a contribution came from — it reads
one lump.** So whatever any contributor put in comes back out inside
`runoff[]`, and there is no contributor for which the node addition is
legitimate.

**The one edge I checked for and did not find:** a subcatchment that receives
run-on but never consumes it would lose water under this fix.
`Runoff.cpp:322` sets `total_area = soa_.area[ui]` — **the full subcatchment
area, the same quantity the node loop already guards on** (`area[ui] <= 0.0
→ continue`). So the two guards coincide and no subcatchment falls between
them.

**⚠ A comment/implementation mismatch found on the way, not fixed:**
`Runoff.cpp:327-329` says run-on is distributed "over the **non-LID** area",
but `total_area` is the full area. One of the two is wrong. It does not
change this fix — the guards coincide either way — but it is a live
discrepancy in a load-bearing comment. **Recorded, not touched.**

## 3. Changeset

```
mod: src/engine/core/SWMMEngine.cpp         (drop q_runon at the node + why)
mod: tests/unit/engine/test_massbalance.cpp (+1 gate)
mod: plans/transport/{IMPLEMENTATION_ROADMAP,PROGRESS}.md
```

**The gate is deliberately not another one-sided balance.** Both existing
balances closed while this defect was live; that is what made it invisible.
`ConveyanceReceivesWhatTheSurfaceShed` asserts **correspondence across the
seam**: on a deck where every subcatchment drains to one junction and nothing
can be lost between surface and node, `SWMM_ROUTING_WET_WEATHER` must equal
`SWMM_RUNOFF_RUNOFF`.

**It runs both fixtures, and `direct` is the control.** With no cascade there
is no run-on to double-count, so the control must have been correct before
the fix *and* after it. **If the control moves, the fix broke the ordinary
case** — and the ordinary case is every model anyone has.

## 4. ⚠ Blast radius — this one moves decks, and I cannot predict which

Unlike the three before it, this changes **water arriving at nodes**, so any
deck with subcatchment cascading, LID drains routed to a subcatchment, or
outfall return moves its `.out`.

**Expected: the three transport decks move** (they cascade `S1 → S2`). **The
other fifteen should not** — but `sdm_fv_*` and `sdm_struct_dw_ard` are the
Site Drainage Model and I have not read their `[SUBCATCHMENTS]` outlets. **If
an SDM deck moves, check whether it cascades before treating it as a
regression.**

**The config guard `84984990` added is load-bearing for this round.** A
moved deck must be attributable to this changeset and nothing else — run a
matched pair and confirm the guard is silent before reading the table.

## 5. Validation protocol

1. **The gate must FAIL at base** (revert the `SWMMEngine.cpp` hunk). Expect
   a cascade ratio near 2.3 and the **control passing** at base. **Quote
   both** — a base run where the control also fails means the gate is
   measuring something other than the defect.
2. `ctest -j8` ×3. Gate joins `test_engine_massbalance`; total stays 160.
3. **Corpus before/after, matched configs.** Record which decks move and
   check each mover's `[SUBCATCHMENTS]` outlets.
4. **Re-run the five-deck fixture set against legacy.** `cascade` should go
   0.511 → **0.218** and `three_deep` 0.536 → **0.318**. **`direct`,
   `selfroute` and `three_flat` must not move** — none has run-on to
   double-count, and if any of them moves the fix reaches further than its
   argument.
5. **Falsifier sweep:**

   | falsifier | expected |
   |---|---|
   | i. restore `+ q_runon` | the gate fails on the cascade leg, control still passes |
   | ii. drop `q_runoff` instead, keeping `q_runon` | both legs fail — confirms the gate is not passing on an accident of magnitude |
   | iii. a deck with an **LID drain routed to another subcatchment** | the seam equality should hold. **Untested — I have no such fixture**, and it is the contributor I reasoned about rather than measured |
   | iv. a deck with **outfall return** (Gap #28) | same, and same gap |
   | v. delete the control assertion | nothing today; it guards the ordinary case against a future edit |

6. **Record:** the base failure with both legs, the five-deck table, and
   **iii and iv** — those two are the contributors I resolved by reading
   `Runoff.cpp` rather than by running a deck, and §2's argument stands or
   falls on them.

## 6. Known gaps

- **⚠ `old_runon_inflow` is now unread.** My change orphaned it. **I have not
  removed it**, and that is deliberate: it is a data member with rotate,
  reset and *serialisation* machinery (`SubcatchData.hpp` lines 292, 643,
  718, 784, **821**, 936, 984, 1003), and 821 looks like a hotstart field
  enumerator. **Removing it could change the hotstart layout**, which is far
  outside this fix's argument. CLAUDE.md §3 says remove what my change
  orphaned — but not at the cost of a format change nobody asked for.
  **Flagged for a decision, not taken.**
- **LID-drain and outfall run-on are reasoned about, not measured** (§5 iii,
  iv). The structural argument is strong — one array, one wholesale read —
  but no deck exercises either.
- **The `Runoff.cpp` non-LID comment mismatch** (§2), untouched.
- **No corpus deck has an LID drain routed to a subcatchment** or an outfall
  return, so the standing sweep cannot catch a regression in either.

## 7. Prepared commit message

```
fix(hydrology): the node injection double-counted run-on

Each outlet node was fed q_runoff + q_runon, but run-on is already inside
runoff[]: assembleRunon sums every contributor -- subcatchment cascade, LID
drain (Gap #25), outfall return (Gap #28) -- into one runon_inflow[] array,
and Runoff.cpp:333 consumes that array wholesale as an inflow rate. Legacy's
subcatch_getWtdOutflow returns (1-f)*oldRunoff + f*newRunoff and nothing
else.

Measured: cascade 0.511 acre-feet against legacy's 0.218, three-deep 0.536
against 0.318, the excess equal to the donor's own runoff. The conveyance was
receiving 2.3x what the runoff ledger said left the surface.

Neither continuity check could see it. The runoff balance closed, the routing
balance closed, and each was self-consistent on its own side of the seam. So
the gate does not add a third balance: it asserts that routing wet-weather
inflow equals runoff surface outflow on a deck where nothing can be lost
between them, and runs the no-cascade case as a control.

The per-contributor question resolves uniformly because the solver reads one
lump and cannot distinguish contributors. total_area there is the full
subcatchment area -- the same guard the node loop already applies -- so no
subcatchment receives run-on without consuming it.
```

---

# 8. Validation results (2026-08-22) — PASSED after one repair

**Validated on:** HEAD `84984990`, the base named in §0. **Committed
`55a70839`.** Artefacts: `tests/output/node_runon_2026-08-22/`.

**The engine hunk was correct as delivered and needed no change.** The gate
did: as written it **could not pass on any deck**, at base or with the fix.
And §5.5's iii and iv — the two contributors you resolved by reading rather
than running — are now **measured**, on decks built for the purpose. Both
were being double-counted, and one was doing worse than that.

## 8.1 ⛔ The gate could not pass — the tolerance was unphysical

§5.1 asks for the control to pass at base. It did not, and it did not pass
with the fix either:

| leg | base | with the fix |
|---|---|---|
| control (`direct`) | 18156.2399 vs 18157.1738 — **FAIL** | same — **FAIL** |
| cascade | 22274.6986 vs 9493.2718, ratio **2.3464** | 9492.5478 vs 9493.2718, ratio **0.99992** |

The fix is plainly right — 2.3464 → 0.99992 — but `1e-6` is not a tolerance
this comparison can reach. The two totals integrate on **different clocks**:
the ledger accumulates `runoff[]·dt` on the runoff step, the routing total
accumulates the interpolated `q` on the routing step.

**Measured rather than assumed** (`seam_probe.cpp`, one deck, four wet steps):

| WET_STEP | direct | cascade |
|---|---|---|
| 15 min | 5.144e-05 | 7.627e-05 |
| 5 min | 1.969e-05 | 2.771e-05 |
| 1 min | 6.581e-06 | 9.088e-06 |
| 20 s | 2.194e-06 | 3.020e-06 |

Monotone to zero as the step shrinks. **A leak would not converge; a
quadrature difference does.** So it is not a defect, and 1e-6 was simply
below the floor — even at 20 s the fixture cannot reach it.

**Repair.** `kSeam = 1e-3`: twenty times above the measured floor, three
orders below the defect's 1.35. And beside it, the statement that does not
rot with a tolerance — **the cascade deck's relative seam error must be no
worse than ten times the control's**, the control measuring what agreement
this engine and this clock can actually reach. Measured **1.5×** with the fix
and **26175.9×** without it.

After the repair, §5.1 reads as specified: cascade fails at **ratio 2.3464**,
**the control passes**, and the self-calibrating leg fires at 26175.9×.

## 8.2 §5.2 — ctest

**159/160 on eleven of twelve full runs**; the constant failure is
`test_engine_2d_infil_integration`, another session's untracked 2D file with
no `[SUBCATCHMENTS]`. Total stayed 160.

**One run in twelve additionally aborted `test_engine_concurrent`.** It is not
this changeset: **0/25 standalone**, **5/5 solo under `ctest -j8`**, and the
abort produced no assertion output. `test_concurrent_engines.cpp:180` reads
`site_drainage_model.inp` from the shared `data/` cwd **while other tests in
the suite write `site_drainage_model.out`/`.rpt` into that same directory** —
the shared-cwd family of `b85b802d`, whose configure-time guard catches
duplicate fixture *names* and cannot see a read/write race on a shared input.
Recorded, not chased.

## 8.3 §5.3 — corpus: the three transport decks, and only those

```
15/18 identical, 3 moved, 0 missing
  age_legacy    DIFFERS (7806 of 43398 bytes)
  age_ard       DIFFERS (8141 of 43398 bytes)
  heat_parity   DIFFERS (8015 of 51520 bytes)
```

Exactly §4's prediction. **The config guard from `84984990` was silent** on
its first real round, so the movement is attributable.

**Every mover cascades and every non-mover does not** — checked, not assumed:
the three transport decks are `S1 → S2`, `S2 → J1`, `S3 → J1`. The SDM decks
(`sdm_fv_*`, `sdm_struct_dw_ard`), which §4 flagged as unread, route
**S1→J1 … S7→J10, every one to a junction** — no cascade, so identical is
the right answer for the right reason. `force_*` and `orif_legacy` have no
`[SUBCATCHMENTS]` at all.

## 8.4 §5.4 — five decks against legacy, both columns

| deck | legacy runoff / routed | base | patched |
|---|---|---|---|
| direct | 0.417 / 0.417 | 0.417 / 0.417 | **0.417 / 0.417** |
| **cascade** | 0.218 / **0.218** | 0.218 / **0.511** | 0.218 / **0.218** |
| selfroute | 0.417 / 0.123 | 0.417 / 0.123 | **0.417 / 0.123** |
| **three_deep** | 0.318 / **0.318** | 0.318 / **0.536** | 0.318 / **0.318** |
| three_flat | 0.625 / 0.625 | 0.625 / 0.625 | **0.625 / 0.625** |

All five agree with legacy in both columns. `direct`, `selfroute` and
`three_flat` did not move — the fix reaches exactly as far as its argument.

## 8.5 §5.5 — the sweep, with iii and iv measured

| falsifier | expected | measured |
|---|---|---|
| **i.** restore `+ q_runon` | cascade fails, control passes | identical to base: ratio **2.3464**, control passes ✓ |
| **ii.** `q_runon` only | both legs fail | control **0 vs 18157**, cascade **1.3464** ✓ |
| **iii.** LID drain to a peer | *(untested by you)* seam holds | **1.9412 → 1.0000**, legacy 1.0000 ✓ |
| **iv.** outfall return | *(untested by you)* seam holds | **2.0759 → 0.9986**, legacy 0.9987 ✓ |
| **v.** delete the control | inert today | inert — in every variant where the control fires, another leg fires too ✓ |

**i and base are the same edit** — §5.5.i is "restore `+ q_runon`", which is
reverting the hunk.

**§2's structural argument holds for all three contributors, and now by
measurement.** New fixtures: `decks/lid_drain.inp` (a `BC1` bio-cell on SA
with `DrainTo SB`, no cascade — the LID drain is the only run-on) and
`decks/outfall_return.inp` (`OF … FREE NO SB`). Each was double-counted at
base by very nearly 2×, and each closes with the fix.

**iv found more than a double-count.** The outfall deck's *surface runoff*
also collapses, **10.020 → 0.705 acre-feet against legacy's 0.779**: the
doubled node inflow was feeding the outfall that fed it back, and the loop
amplified. This is the one deck where the defect was not merely mis-booked
but genuinely manufacturing water.

**Note on ii:** the self-calibrating leg is neutralised there (`err_direct`
is 1.0, so the 10× bar is 10.0 and the cascade's 0.346 passes it). The two
absolute legs caught it. The relative statement is only meaningful when the
control is healthy — which is the right behaviour, but worth knowing.

## 8.6 Findings outside the changeset

**Finding 7 — the LID-drain deck produces 34× legacy's water.** `lid_drain`
sheds **15.482 acre-feet** against legacy's **0.456**, and the fix does not
change that (base and patched both 15.482; only the routed column halves).
One `BC1` of 87120 ft² on a 5-acre subcatchment under 1 inch of rain. This is
the LID area/unit family — `df7bdf12` and issue #131, "LID layer params still
never unit-converted" — surfacing on the first deck built to exercise
`DrainTo`. **No corpus deck has an LID at all.**

Findings 4 (this one), 5 and 6 from the previous rounds are otherwise
unchanged; 5 and 6 remain open.

## 8.7 Deviations

1. **Gate tolerance replaced** (§8.1) — required; as delivered the gate could
   not pass.
2. **A third assertion added** — the cascade-vs-control relative bar, so the
   gate does not depend on a chosen number.
3. **`SWMMEngine.hpp:464` corrected** — `wet_q_interp_`'s doc comment said
   "runoff+runon"; the change orphaned that, CLAUDE.md §3.
4. **`old_runon_inflow` left in place**, agreeing with §6. It is not merely
   unread — it is still *maintained* (`rotate` at 984, `reset` at 1003) and it
   sits in **two positional enumerator lists** (`r(...)` at 784, `e(...)` at
   821). Removing it moves the layout.
5. **`plans/` not committed**, per the standing rule.
6. **The committed `SWMMEngine.cpp` blob was built from HEAD**, not the
   worktree — another session's `has_subcatchments` work is in the same file.
   HEAD plus this one hunk, built and run on its own (**14/14**) before
   committing.

## 8.8 Still owed

- **Finding 7** (LID magnitudes) and **Findings 5, 6** are open.
- **The `Runoff.cpp:327-329` "non-LID area" comment** still contradicts
  `total_area = soa_.area[ui]` — §2 recorded it, and nothing here touched it.
- **`old_runon_inflow`'s removal needs a hotstart-layout decision.**
- **No corpus deck has an LID drain, an outfall return, or a self-route**, so
  the standing sweep still cannot catch a regression in any of the three.
  `tests/output/node_runon_2026-08-22/decks/` now holds fixtures for all of
  them.
- **`test_engine_concurrent`'s shared-cwd race** (§8.2).

# 9. Commit

`55a70839` — `fix(hydrology): the node injection double-counted run-on`, on
parent `84984990`. Three files: `src/engine/core/SWMMEngine.cpp` (+27 −7),
`src/engine/core/SWMMEngine.hpp` (+1 −1),
`tests/unit/engine/test_massbalance.cpp` (+114).
