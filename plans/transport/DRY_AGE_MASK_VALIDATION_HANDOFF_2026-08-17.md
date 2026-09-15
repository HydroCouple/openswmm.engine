# Dry-Element Age Mask — Validation & Commit Handoff (2026-08-17)

**For:** the checking agent.
**Base:** `06580dd6` (post species-ID reader).
**Not a plan phase** — A2b carry (c), the last correctness item in the
shipped age feature.
**Standing findings:** lessons 1–42.

---

## 1. The defect

A2b validation observed links publishing **exactly `6.000000` h of age on
water that never existed** — a deck delivered no inflow, the links stayed
dry, and the age column reported the INITIAL_STATE seed plus elapsed time.
A user plotting the age column sees a confident number for an empty pipe.

## 2. The decision I made (and why it is reversible)

Two options were on the table: mask dry elements, or document the
convention. **I chose the mask, at the REPORT boundary only.**

- **The state must keep aging.** If a dry element's stored age were reset,
  a refilling pipe would jump discontinuously, and A2a's hotstart would
  lose the age it exists to restore. So `water_age_state` is untouched.
- **The report is where the nonsense is**, so that is where the mask
  lives: the snapshot's age column reads 0 when the element's own
  REPORTED depth is ~0. The record becomes internally consistent — a
  reader seeing depth 0 sees age 0.
- **Precedent in the same file:** legacy's washoff gate
  (`subcatch.c:929`) reports 0 rather than a residual concentration when
  runoff is zero. Same shape, already cited by the snapshot-quality fix.

**Keyed on DEPTH, not reported volume** — this is the trap worth naming:
legacy maps a junction's reported volume to 0 by convention, so a
volume-keyed mask would zero **every junction age in every model**. Gate 2
is the negative control for exactly that.

If you would rather document the convention and report the aging value,
revert the two guards; the state side needs no change either way.

## 3. Changeset (uncommitted)

```
mod:  src/engine/core/SWMMEngine.cpp
      (age column: wet test on snap.nodes.depth / snap.links.depth,
       kDryReportDepth = 1e-9; comment records the decision, the state
       rationale, and the junction-volume trap)
mod:  tests/unit/engine/test_output_quality.cpp
      (deck helper gains a `dry` knob — InitDepth 0 + FREE outfall, the
       configuration that exposed this; +2 gates)
```

All TUs pass `g++ -std=c++20 -fsyntax-only`.

## 4. Validation protocol

1. Build, zero new warnings.
2. `ctest -R test_engine_output_quality` — 7 gates now (5 + 2 new:
   `DryElementsReportNoAge`, `WetElementsStillReportAgeAfterTheMask`).
   *Anticipated failure modes:*
   (a) **The dry deck may not actually be dry** — gate 1 asserts reported
   depth ≤ 1e-6 per link BEFORE asserting the age, so a wet deck fails
   loudly on the liveness leg rather than passing vacuously (lesson 24/36
   discipline). If it trips, the deck needs adjusting, not the mask.
   (b) **`INITIAL_STATE 6.0` on a dry deck** — the seed goes into the
   state regardless of wetness (the ARD engine seeds cells at init), so
   unmasked the column reads ~6.167. If it reads 0 even with the mask
   removed (falsifier i), the discriminator is dead and the gate proves
   nothing — check that the age state is actually seeded on a dry mesh.
   (c) Link column index is `v[6]` (5 fixed + TSS + age); node is `v[7]`.
3. **Falsifier sweep:**

   | falsifier | expected failing gates |
   |---|---|
   | i. remove the link mask (report the state directly) | `DryElementsReportNoAge` (reads ~6.167) |
   | ii. remove the node mask | not observed by these two gates — the dry gate reads LINKS. **Flagged in advance (lesson 11):** either extend the dry gate to read node columns too, or record the node mask as review-only. I'd extend it; it is two lines. |
   | iii. key the mask on reported VOLUME instead of depth | `WetElementsStillReportAgeAfterTheMask` (every junction age masked to 0) — this is the trap gate |
   | iv. mask the STATE instead of the report (zero `water_age_state` when dry) | both new gates still pass; the water-age suite's hotstart/continuity gates should catch it — **verify which**, because if nothing catches it the state/report separation is unobserved |
4. **Prior suites:** the water-age suite (16 gates) reads
   `water_age_state` directly and must be UNCHANGED — that is the
   evidence the mask did not touch state. A2b's
   `WaterAgeReportsAsATrailingColumnInHours` uses the wet level pool and
   must stay green. WATER_AGE-off decks are untouched (the guard is
   inside `age_col`), so 14/14 `.out` bit-identity should hold against
   `06580dd6`.
5. **Record:** whether falsifier iv is caught anywhere. That is the one
   coverage question I could not answer by construction.

## 5. Commit message

```
fix(transport): a dry element reports no water age

A2b validation observed links publishing exactly 6.000000 h of age on
water that never existed: a deck delivered no inflow, the links stayed
dry, and the age column reported the INITIAL_STATE seed plus elapsed
time. A user plotting age saw a confident number for an empty pipe.

The state must keep aging - resetting it would make a refilling pipe jump
and would cost A2a's hotstart the age it exists to restore - so the mask
lives at the REPORT boundary: the age column reads 0 when the element's
own reported depth is ~0, which also makes the record self-consistent (a
reader seeing depth 0 sees age 0). Same shape as legacy's washoff runoff
gate (subcatch.c:929), which reports 0 rather than a residual
concentration.

Keyed on DEPTH, not reported volume: legacy maps a junction's reported
volume to 0 by convention, so a volume-keyed mask would zero every
junction age in every model. That trap has its own gate.

Gates: tests/unit/engine/test_output_quality.cpp +2 - a bone-dry deck
(InitDepth 0, FREE outfall) with INITIAL_STATE 6 h must report 0 on every
link, with a liveness assertion that the deck really is dry; and the wet
level pool must still report its age (the negative control for the
junction-volume trap).
```

## 6. Validation results

**Verdict: the decision is right, the implementation did not work. Fixed and
committed** as `584d1065`, with a corrected link predicate, three gate
repairs and one new gate. Artifacts:
`tests/output/dry_age_mask_validation_2026-08-17/`.

### 6.1 The mask was inert on exactly the elements the defect was reported on

`DryElementsReportNoAge` **failed on arrival**, and it failed on its own
liveness assertion — §4.2(a)'s anticipated mode, working as designed:

```
link 0 reports depth 9.9999997473787516e-05 — the deck is not dry
```

That number is not noise. **A dry conduit never reports depth 0**: the
dynamic-wave router floors it at `FUDGE` = 1e-4 ft. The threshold was 1e-9,
five orders of magnitude below the floor, so the link branch could never
fire. Measured on the delivered dry deck with the delivered mask in place:

| | reported depth | reported volume | reported age |
|---|---|---|---|
| dry conduit | 9.99999975e-05 | 0.0107 ft³ | **6.166667 h** |
| wet conduit | 1.5 | 1263.5 ft³ | 2.166667 h |
| dry junction | 0 | 0 | 0 |
| **wet junction** | 1.5 | **0** | 2.166667 |

The 6.166667 h in that first row is the original A2b observation verbatim —
`INITIAL_STATE 6 h` plus ten minutes — surviving the fix meant to remove it.
The node branch did work, so half the feature was live and the inert half
was the half the defect was reported on.

The last row is the handoff's junction-volume trap, now measured rather than
asserted: a **wet** junction reports volume 0. §2 is right that nodes must
key on depth.

### 6.2 The fix: a link is wet if it HOLDS water **or** CONVEYS it

Neither field alone works, and the two failure modes are mirror images:

- **Depth fails for links** (floored at FUDGE) — the delivered bug.
- **Volume fails for nodes** (0 by convention when wet) — the handoff's trap.
- **Volume alone fails for regulators.** A pump, orifice or weir stores
  nothing — volume 0 *and*, for a pump, depth 0 — while carrying full flow,
  and its `link_age` is a real number. Measured with the mask removed:
  pump 0.124060 h, orifice 0.130469, weir 0.134809 — each **exactly** its
  upstream node's age (J1 0.124060, J2 0.130469, J3 0.134809). A volume-only
  test blanks the age of every regulator in every model, which is the node
  trap again with the fields exchanged. My first fix did exactly that; the
  regulator deck caught it.

So the link predicate is:

```cpp
holds   = ctx_.links.volume[l] > quality::ZERO_VOLUME   // 1 litre, ft3
conveys = std::fabs(ctx_.links.flow[l]) > constants::TINY
wet     = holds || conveys
```

Both read from **internal** state, deliberately: `snap` volumes are in user
units while `ZERO_VOLUME` is ft³, so a snapshot-keyed test would be an SI
unit bug. Both constants are the engine's own — `ZERO_VOLUME` is what
`QualityRouting` itself uses to decide an element holds no water, so the age
is now reported exactly when the age was routed, rather than against a
threshold invented for the mask. Nodes are unchanged.

Result: dry conduits 6.166667 → **0**; wet conduits, wet junctions and
flowing regulators all keep their ages.

### 6.3 Gate repairs

1. **The liveness assertion was unsatisfiable** (`link depth ≤ 1e-6`). Now
   asserts volume < `ZERO_VOLUME` — the field that does go to ~0 — plus
   `EXPECT_NEAR(depth, 1e-4)`, which documents the router's floor and will
   fail loudly if it ever moves.
2. **Extended the dry gate to nodes** (§4.3's own suggestion for falsifier
   ii). It did not help — see 6.4 — but it is the right assertion.
3. **Extended the wet gate to a standing-water LINK.** Nothing read a wet
   link's age, so dropping the `holds` leg blanked every stagnant pipe in
   the model with the whole suite green (falsifier vi). Now caught.
4. **New gate `FlowingRegulatorsKeepTheirAge`** — a running pump with a
   liveness assertion on both halves (volume really is 0, flow really is
   non-zero). This is the gate that catches the defect I introduced in 6.2.

### 6.4 Falsifier sweep — 4 of 6 caught, and both escapes are informative

| falsifier | outcome |
|---|---|
| i. no link mask | **caught** — 6.1666665 on all three dry links |
| ii. no node mask | **escapes** — see below |
| iii. node mask keyed on volume (the trap) | **caught** by two gates |
| iv. mask the STATE instead of the report | **escapes the entire suite** — see below |
| v. drop the CONVEYS leg | **caught** by the new regulator gate |
| vi. drop the HOLDS leg | **caught**, after repair 3 |

**ii escapes because there is nothing to hide, not because the gate is
weak.** With the node mask removed the dry deck's nodes still report 0 — the
node age *state* is already 0 there, since the ARD seeding is wetness-gated
at init for node stores while link cells are seeded regardless. That is why
dry links carried 6.167 h and dry nodes did not. **The node branch is
defence-in-depth, not live code**, and the case that would make it live is a
hotstart restoring an age into an element that starts dry. Keep it — it
costs nothing and A2a can produce exactly that state — but do not count it
as tested.

**iv escapes all 141 tests**, which answers §5's open question: nothing
anywhere catches masking the state instead of the report — not the
water-age suite's hotstart gates, not the continuity gates. The full suite
under falsifier iv returns 140/141, identical to clean.

Worth being blunt about the consequence: **the "a refilling pipe would jump"
half of §2's rationale does not survive scrutiny.** A refilling pipe's age
comes from the water arriving in it; the stale state it held while dry
occupies 0.0107 ft³ against an arriving 1263 ft³, an influence of order
1e-5. The rationale that *does* hold is the hotstart one — a save taken
while an element is dry must carry the aged value. So the state/report
separation is real but its only observable consequence is hotstart fidelity,
and that is where the missing gate belongs: save a native hotstart from a
run whose links are dry, reload, assert the restored link age is non-zero.
It belongs in `test_water_age.cpp` beside A2a's hotstart gates rather than
here, so I have recorded it rather than written it.

### 6.5 Everything else

| check | result |
|---|---|
| build | clean, no new warnings |
| `test_engine_output_quality` | **8/8** (5 prior + 2 delivered + 1 new) |
| full `ctest` | **140/141** — only the known pre-existing FV refinement failure |
| water-age suite | **16/16 unchanged** — the evidence the mask did not touch state |
| 14-deck bit-identity vs A2b baseline | **14/14** (WATER_AGE-off is inside `age_col`) |
| ASan + UBSan | output-quality **0 findings**; water-age 16/16 with the pre-existing `HotStartManager.cpp:246` misaligned CRC load |

Validated in a worktree at `06580dd6` carrying only the two changeset files,
the shared tree again holding other agents' work.
