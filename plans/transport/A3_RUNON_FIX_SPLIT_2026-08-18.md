# A3 run-on defect — separability finding and validation instruction

**For:** the checking agent, before validating A4.
**HEAD:** `d85429fb`. A4 uncommitted (12 files).
**Standing findings:** lessons 1–70.

---

## 1. I said "split the A3 fix out and land it fast". Half of that was wrong.

Last turn I recommended extracting the `runon_inflow` age fix into its own
commit against HEAD, on the grounds that it corrects a live defect in
committed code (`b5be8ec3`) and should not wait on A4's validation.

**I checked before writing this, and the two halves differ:**

| contributor | producer | separable from A4? |
|---|---|---|
| subcatchment cascade | `subcatch_runoff_age` (A3) | already correct in `b5be8ec3` |
| **outfall return** | `water_age_state.node_age[uj]` (A1a) — `SWMMEngine.cpp:3618-3623` | **YES** — depends only on committed state |
| **LID drain return** | `ctx_.lid_layer_state.drain_value` (**A4's new per-layer state**) — `SWMMEngine.cpp:1805-1820` | **NO** — cannot exist before A4 |

So "the A3 fix" is not one fix. The outfall half reads node ages that have
existed since A1a and could land today. The LID half is only computable once
A4's `lid_layer_state` exists, because the drain's age *is* a per-layer
quantity.

This is lesson 69's shape a second time — I asserted a property (separability)
without checking it — except this time the check happened before the
instruction went out rather than after. That is the only difference, and it
is the whole difference.

## 2. What this means for the live defect

The severity claim stands: on `b5be8ec3` a deck with a LID underdrain or a
returning outfall reports run-on water **younger than anything entering the
model** (3.834 h under 4 h rain, against subarea ages 3.644/3.653/3.883 h).
The numerator counts one of three contributors and divides by all three.

But **the full correction requires A4**, so the useful options are:

1. **Validate and land A4 as one changeset** (recommended). The defect is
   then fixed completely, in the phase that makes the fix possible. Cost: the
   defect lives on main until A4 lands.
2. **Land the outfall half alone** as an A3 fix, leaving the LID half to A4.
   Cost: a partial fix is arguably worse to reason about than a whole one —
   a LID deck would still under-age while an outfall deck would not, and
   nothing in the code would say which.

I recommend (1), and I am flagging that I recommended (2) last turn on an
unchecked premise.

## 3. Instruction to the checking agent

Validate A4 **including** the A3 correction it carries, as one changeset.
Beyond the A4 handoff's own protocol:

1. **Isolated worktree at `d85429fb`.** The implementer's 150/150 was run in
   the main tree with foreign edits present, so it is not attributable to
   A4. Re-run there: full suite, deck bit-identity, ASan/UBSan.
2. **`tests/unit/engine/CMakeLists.txt` must be merged onto HEAD's blob** at
   stage time — HEAD has moved twice this round under foreign commits, and
   H2's round nearly reverted someone's line exactly this way (caught only by
   a diffstat reading `2 +-` where `1 +` was expected).
3. **The A3 gate suite needs a LID-bearing deck.** The defect existed because
   no A3 deck had a LID or a returning outfall — lesson 59 at the level of a
   whole contributor. Adding that deck to `test_water_age_watershed.cpp` is
   what stops the gap reopening; without it the fix is correct but unguarded.
4. **Falsifier, stated in advance:** zero the LID-drain age accumulator while
   leaving its rate contribution, and separately the outfall one. Each should
   drive the arriving age below the rain age — the impossible-value signature
   that made the original defect visible. **If neither fails a gate, the fix
   is unobserved** and requirement 3 is not optional.
5. **Do not treat A4's `@warning`-marked LID decks as broken** when issue
   #131's unit conversion lands. They are written in feet and ft/s
   deliberately; the correct response then is to convert the decks, not to
   widen the bands.

## 4. Recorded

- The severity number and the fix are logged against **A3**, not A4, in
  `IMPLEMENTATION_ROADMAP.md` — that is where the defect shipped and where
  the gate gap was.
- The `np_use > 0` drain gate (a pure-age model never delivered drain water)
  is the seventh instance of the lesson-20 configuration trap and is fixed in
  the same changeset at `SWMMEngine.cpp:1844`.
