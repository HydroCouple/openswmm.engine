# H5b follow-up — `live` vs `mass` — Validation & Commit Handoff (2026-08-19)

**For:** the checking agent.
**Base:** `1c78e9dd` (H5b).
**Plan:** `HEAT_TRANSPORT_PLAN.md` §6.1 D-H5b / D-H5c.
**Standing findings:** lessons 1–95.

**A defect fix in code I shipped last round**, found by the H5b validation
and recorded in its §9.8. Standalone, small.

---

## 1. The defect, and the second half of it that §9.8 did not name

`HeatLid.cpp` used one predicate, `live[k]` — "does this layer hold water" —
for two questions that are not the same:

1. **Does it participate in advective mixing?** Water is the right test.
2. **Does it have thermal mass?** Water is the *wrong* test. A buried layer
   is a matrix with water in its voids; drained, it still has
   `ρ_s·cp_s` — the larger term — and it still conducts.

§9.8 named consequence (1): a drained layer dropped out of the tridiagonal
system, so its neighbours conducted **straight across the gap that should
insulate them**.

**There is a second consequence §9.8 did not name, and it is arguably
worse.** `live[k]` also gated the D-H5c dry policy. So a drained layer was
*reset every step* — to air temperature under `AIR`, to 20 °C under
`DEFAULT` — overwriting a real physical state. D-H5c answers "what does an
element with **no thermal mass** report?" A present matrix was never one of
those, and under `DEFAULT` the layer was pinned to a constant regardless of
what its neighbours were doing.

Both sites now use `mass[k] = thick[k] > 0 && (k == SURFACE ? live[k] :
donor[k] != ABSENT)`. The surface layer keeps the water test, correctly:
it is ponded water over a face, so dry it has nothing.

## 2. STORAGE, not soil, is the reachable case — and that changed the gate

§9.8's example was a drying **soil** layer. But `live[soil]` is
`soil_moist × soil_thick > 1e-12`, and a soil layer holds field capacity
essentially forever — it is nearly never dry in a wet simulation.

**Storage drains to exactly zero between storms.** So does pavement in a
permeable-pavement stack. The gate is built on storage for that reason. The
soil case is real but hard to reach; the storage case is ordinary.

I checked this rather than writing the gate around §9.8's example — the
example named the mechanism correctly and the layer incidentally.

## 3. Changeset (uncommitted)

```
mod:  .../HeatModule/HeatLid.cpp              (mass[] beside live[];
      both the conduction index set and the dry-policy branch)
mod:  tests/unit/engine/test_heat_lid.cpp     (+1 gate, 8 → 9)
```

`tests/unit/engine/CMakeLists.txt` is **NOT touched** — the gate joins an
existing target.

Syntax-clean under `-Wall -Wextra`. Nothing built or run.

## 4. Design decisions to review

### 4.1 A drained buried layer is now governed by conduction, not by policy

This narrows what `DRY_ELEMENT_TEMPERATURE` covers: absent layers, and dry
surface layers. **Flag if you read D-H5c as broader than that** — the user's
decision said "a dry subarea or absent LID layer", and a drained storage
layer with a gravel matrix is neither of those in the sense the decision was
answering. But it is a scope reading, and it is mine.

Gate 6 still exercises the policy on an **absent** PAVEMENT layer, which is
unaffected.

### 4.2 The surface layer keeps the water test

Asymmetric on purpose. If you would rather `mass[]` were uniform, the
consequence is that a dry surface layer enters the system with
`cap ≈ 0`, hits the `b[i] = ... : 1.0` fallback, and equilibrates instantly
with whatever is below it — an invented thermal mass. I judged that worse
than the asymmetry.

## 5. Validation protocol

1. **Isolated worktree at `1c78e9dd`.** Lesson 71. Suite was **154/154** and
   **nothing failed at base** — do not carry any exemption forward
   (lesson 89).
2. **⛔ HARD STOP — lesson 79.** `git diff --cached --numstat` must show **no
   entry at all** for `tests/unit/engine/CMakeLists.txt` (§3). Any entry
   means the index picked up a foreign edit. `update-ref <new> <old>`.
3. Build, zero new warnings. Heat suites, then the full suite.

   **Anticipated failure modes — and my record here is 2 of 16, so the sweep
   matters more than this list.**

   (a) **Gate 9's SETUP 2 may not reach a drained storage layer.** It asserts
   `stor_depth < 1e-9` after a 10-minute storm and a 3-hour run. If that
   fires, lengthen the run or raise the drain coefficient. **Do not relax the
   assertion** — a non-dry layer means the conflation is never reached and
   the gate is vacuous, which is the whole failure mode it exists to avoid.

   (b) **Gate 9's SETUP 3** requires the soil neighbour to sit more than
   0.5 °C from `kDefaultTemp`, so "reset to 20" and "conducted to 20" cannot
   coincide. The deck puts the sources at 4 °C and 34 °C to straddle it.

   (c) **Values move on any deck where a layer drains.** Expected, and the
   direction is the point: a drained layer that used to be pinned to a policy
   value now carries a conducted one. **Report which gates moved, with both
   values.** H5b's other 8 gates should be unaffected — none of them drains a
   layer while conduction is on.

4. **Falsifier sweep:**

   | falsifier | expected failing gates |
   |---|---|
   | i. revert `mass[k]` to `live[k]` in the conduction index set only | **9** — the defect gate for the half §9.8 named |
   | ii. revert `mass[k]` to `live[k]` in the dry-policy branch only | **9** — the half it did not. **If only one of i and ii fails, the gate observes one consequence and the other is unguarded; say which** |
   | iii. make `mass[]` uniform (drop the SURFACE special case) | **probably nothing** — flagged. It needs a deck with a dry surface over a wet layer and conduction on. Owed if it escapes |
   | iv. widen `mass[]` to include absent layers (`thick > 0` only) | 6, if `pave_thick` is 0 for a bioretention cell — **verify that assumption**, it is (c) from last round's list and was never confirmed |

5. **Prior suites:** full C++ suite, 14/14 decks, ASan/UBSan. **A4's
   `test_water_age_lid` must still be bit-unchanged** — this changeset does
   not touch the age row, and last round established that as the check that
   matters most for anything in this file.

6. **Record:** falsifiers i and ii separately, and whether gate 9's SETUP
   assertions were reachable as written.

## 6. Known gaps

- **Falsifier iii is predicted to escape.** No deck here dries a surface
  layer while conduction is on.
- The **fine-`dt` external reference** remains the highest-value owed item in
  the heat track: it closes A4 falsifier iii, H5a falsifier vi, D-H5e's
  linearization caveat, and H5b falsifier ii — which last round established
  cannot be discriminated any other way, because conduction's fixed point and
  the atmospheric one coincide and both compositions are first-order
  consistent with the same ODE. **One instrument, four items.** That is what
  I would do next.

## 7. Prepared commit message

```
fix(transport): a drained LID layer still has thermal mass (H5b follow-up)

HeatLid used live[k] -- "does this layer hold water" -- for two questions.
It is the right test for advective mixing and the wrong one for thermal
mass: a buried layer is a matrix with water in its voids, and drained it
still has rho_s*cp_s and still conducts.

So a drained layer dropped out of the tridiagonal system, leaving its
neighbours conducting across the gap that should insulate them, AND fell
under the D-H5c dry policy, which reset a real physical state every step --
to a constant 20 C under DEFAULT, regardless of its neighbours.

mass[k] now gates both sites. The surface layer keeps the water test: it is
ponded water over a face, so dry it genuinely has nothing.

Storage is the reachable case, not soil -- storage drains to exactly zero
between storms where soil holds field capacity indefinitely. The gate is
built on that, with setup assertions that the layer is present, has a
non-zero heat capacity, is actually dry, and has a neighbour far enough from
kDefaultTemp that a reset and a conduction result cannot coincide.
```

---

# 8. Validation result (checking agent, 2026-08-19) — COMMITTED `815f0e8e`

Isolated worktree at `1c78e9dd`. **154/154** ctest, nothing failing at base.
**14/14** decks bit-identical. **A4's age values bit-identical** across all
six of its decks. **84 tests** clean under ASan/UBSan. Zero warnings.
Falsifier sweep **4 of 4**, up from **0 of 4** as delivered.

## 8.1 Every falsifier was inert on arrival, and the gate was the reason

Not one of i–iv failed anything as delivered — including the two the handoff
expected gate 9 to catch. The gate was not weak; **the branch it is about was
never entered.**

`live[k]` is `v_old > tiny || v_in > tiny` — depth **OR INFLOW**. Gate 9's
SETUP asserted only `stor_depth < 1e-9`, which is not the predicate the code
uses. Three decks were needed:

1. **As written** the gate failed SETUP 2 outright: storage still held
   5.06e-4 ft after three hours, because the outfall routes its discharge
   back onto S1 and the LID is fed continuously. §5(a) said to lengthen the
   run or raise the drain — neither helps when the inflow never stops.
2. **With that loop closed** depth reached exactly 0 and SETUP 2 passed — but
   `in_stor` sat at **9.92e-7 ft/s, six orders above the 1e-12 threshold**,
   because the subcatchment keeps trickling in long after the storm. I
   confirmed by direct measurement that falsifiers i and ii produced output
   **identical to the last digit**.
3. **`FromImp = 0`** is what closes it: the unit then receives only rain on
   its own footprint, so every inflow is exactly zero once the storm stops,
   the soil settles to field capacity and storage reaches the dry-but-present
   state. SETUP 2 now asserts depth **and** inflow, so this cannot regress
   silently.

This is the H5b-round lesson repeating one level down: **a zero depth is not
an absence of flow.** Last round it made a heat-content ledger read a 12 %
loss that was really the outfall re-feeding the column; here it made a whole
gate vacuous.

## 8.2 §2's reachability claim is right about depth, wrong about `live`

"Storage drains to exactly zero between storms" is true of `stor_depth` and
false of `live[storage]`, which also tests inflow. The soil case §9.8 named
is likewise unreachable, but for the reason §2 gives (field capacity), so
that half stands. Rain barrel and infiltration trench were also checked:
same persistent trickle, `in_stor` 2.07e-7 and 2.38e-7.

**A correction to something I nearly reported as an engine defect:** the
rain-barrel and trench decks appeared to segfault. They do not. The crash
was in my own probe, which indexed `lid().group(0)` — the bio-cell group,
empty for those decks. `frame #0` named the probe, not the engine. The engine
handles all three types.

## 8.3 One assertion sees one half — the gate needed three

- **(a) Not reset** catches ii, but only against a **margin**. As written it
  was `EXPECT_NE(storage, kDefaultTemp)`, and the reset lands at **20.004**,
  not 20 — the policy value enters the solve's right-hand side and conduction
  pulls it partway back inside the same step. The exact compare passed on it.
  Now `|storage − kDefaultTemp| > 0.5`, against a measured 28.339.
- **(b) Still in the solve** catches i, which (a) cannot see: an excluded
  layer is frozen at what it held when it dried, which is not kDefaultTemp
  either. Measured as movement between two end times — coupled
  **28.31646534 → 28.33914633**, excluded **28.30066495 → 28.30066495**,
  bit-identical. Exact and reference-free.
- **(c) The dry SURFACE still takes the policy** catches iii, which §6
  predicted would escape. Uniform `mass[]` gives a `cap ≈ 0` layer the
  solver's fallback and it equilibrates instantly: **29.54 against the
  policy's 20**, exactly the failure §4.2 describes.
- **iv** (absent layers in `mass[]`) is caught by the same leg. And §5's
  unverified assumption is now confirmed: **`pave_thick = 0`** for a
  bioretention cell, so gate 6's absent-PAVEMENT case is genuine — it sits at
  20 with capacity 0.

## 8.4 §5(c): no gate moved

None of H5b's other eight gates changed a value, which follows from §8.1:
on the default deck family (`FromImp = 100`) the dry-but-present branch is
never entered, so the changeset is inert there. The 14/14 deck identity and
the bit-identical A4 ages are consistent with that.

## 8.5 On §4.1 — the narrowed scope of D-H5c

I agree with the reading. A drained gravel storage layer is neither "a dry
subarea" nor "an absent LID layer", and the gate now pins both halves of the
boundary: a present-but-dry layer is governed by conduction (§8.3b) and a dry
surface layer is governed by the policy (§8.3c). The scope is no longer a
reading — it is asserted in both directions.

## 8.6 Still owed

- **The fine-`dt` external reference** — unchanged as the highest-value item,
  closing A4 falsifier iii, H5a falsifier vi, D-H5e's linearization caveat
  and H5b falsifier ii. Four items, one instrument.
- The dry-but-present state is only reachable with `FromImp = 0`. Any future
  gate about it must close the inflow the same way, or it will pass on
  nothing.
