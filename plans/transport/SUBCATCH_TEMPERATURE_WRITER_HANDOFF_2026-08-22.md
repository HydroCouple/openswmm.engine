# The subcatchment temperature column had no writer — Handoff (2026-08-22)

**For:** the checking agent.
**Base:** `421e95c2`.
**Standing findings:** lessons 1–142.
**This is Finding 2** from the age/heat deck round. **Finding 3 — self-routed
subcatchments recirculate their own runoff — is NOT here and is scoped in §7.**

---

## 1. The defect

`heat_parity.inp`, first run: nodes and links carried live temperature
(−4.147…17.66 °C); **every subcatchment read exactly 0.0 for the whole run.**

The subcatchment loop in the snapshot builder had an **age** writer and no
temperature sibling, so `snap.subcatch_quality` kept its `assign(…, 0.0)` and
the output plugin faithfully wrote the zero. The value was in
`ctx_.heat_state.subcatch_runoff_temp` the entire time.

Three writers; two existed:

| | node | link | subcatchment |
|---|---|---|---|
| age | ✅ | ✅ | ✅ |
| temperature | ✅ `:4588` | ✅ `:4645` | **absent** → now `:4682` |

**Third instance of F8's family**, and the deck round's own column-presence
check would have passed — the column was in the header all along. Only
reading the values caught it (lesson 139).

## 2. The fix, and the one deliberate divergence

One writer, mirroring the node and link loops. **It sits OUTSIDE the
`has_runoff` gate that the age column beside it obeys, and that asymmetry is
the only judgement call in the changeset:**

- **Age is gated** because legacy's washoff convention says a subcatchment
  producing nothing reports nothing (`subcatch.c:929`).
- **Temperature cannot borrow that convention.** 0 °C is a real temperature
  and cannot double as "no water" — H1's reasoning at nodes and links. And
  **D-H5c exists precisely so the dry value is the deck's choice**
  (`HOLD | AIR | DEFAULT`), so the state carries a number the deck asked for
  and blanking it would throw that away.

If you think the asymmetry is wrong, the argument to beat is D-H5c: a deck
that writes `DRY_ELEMENT_TEMPERATURE HOLD` and then reads 0.0 for every dry
subcatchment has had its option silently ignored.

## 3. Changeset

```
mod: src/engine/core/SWMMEngine.cpp            (the writer + why, ~28 lines)
mod: tests/unit/engine/test_heat_watershed.cpp (+2 gates)
mod: plans/transport/{IMPLEMENTATION_ROADMAP,PROGRESS}.md
```

**Two gates, because one of them cannot see the design decision:**

1. `SubcatchmentTemperatureColumnIsWrittenToTheOutput` — reads **values out of
   the finished `.out`**, not `ctx.heat_state`. The state was always correct,
   so a state gate cannot fail on this defect (lesson 104). Asserts the column
   is **not identically zero** *and* that it **varies across periods** — the
   second catches a writer that emits a constant, which would satisfy the
   first and still be wrong.
2. `DrySubcatchmentStillReportsATemperature` — gates §2's divergence. Rain
   stops at 15 of 60 minutes; every subcatchment-period with **zero runoff**
   must still carry a nonzero temperature. **Without this, moving the write
   back inside `has_runoff` would pass every other heat gate in the file.**
   It opens with a SETUP leg (`dry_periods_seen > 0`) so that a deck which
   never dries out fails loudly instead of proving nothing — the leg the
   cascade round's falsifier ii landed on.

## 4. Validation protocol

1. **Both gates must FAIL at base** (revert the `SWMMEngine.cpp` hunk only).
   Gate 1 should report "every subcatchment temperature … is exactly 0.0".
   **Quote both failure messages.**
2. `ctest -j8` ×3. Both gates land in the existing `test_engine_heat_watershed`
   binary, so **the ctest total stays 160** — expect **159/160**, the standing
   Track I failure. (My last handoff said 160/161 and was wrong about this:
   ctest counts registered tests, and this suite registers one per binary.)
3. **Corpus before/after, two build directories.** **`heat_parity.out` MUST
   move** — it is the only deck with heat, and its subcatchment column goes
   from all-zero to live. **The other seventeen must be byte-identical.**
   This is the first round where a moving `.out` is the pass condition, so
   check the direction: if `heat_parity` does *not* move, the writer is not
   reached and the gates are passing for some other reason.
4. **Read `heat_parity`'s new column.** The subcatchment values should sit in
   the same range the node and link columns already do (−4.147…17.66 °C on
   the base run). **A column that is written but physically absurd is still a
   defect** — quote min and max.
5. **Falsifier sweep:**

   | falsifier | expected |
   |---|---|
   | i. move the write back inside `has_runoff` | gate 2 fails, gate 1 **passes**. That asymmetry is the point of having two |
   | ii. write a constant (e.g. `= 1.0`) | gate 1's *varies* leg fails; the *nonzero* leg passes |
   | iii. drop the `temp_col` guard | a deck with `HEAT_TRANSPORT` off should still be unaffected — `temp_i` would index a column that does not exist. **Expect a crash or corruption, and if neither happens, ask why** |
   | iv. remove gate 2's SETUP leg and set `rain_stop_min = -1` | gate 2 then passes vacuously with zero dry periods. Confirms the SETUP leg is load-bearing |
   | v. run `heat_parity.inp` and check `age` and `temperature` on the same dry period | age 0, temperature nonzero — the asymmetry, end to end on a real deck rather than a fixture |

6. **Record:** both base failures, `heat_parity`'s column range, and
   falsifiers i and v.

## 5. Known gaps

- **`DRY_ELEMENT_TEMPERATURE AIR` and `DEFAULT` are still untested** anywhere.
  Gate 2 uses `HOLD`. The other two policies now matter *more*, because this
  changeset is what makes them observable at a subcatchment at all.
- **No gate compares the subcatchment column against
  `subcatch_runoff_temp`.** Gate 1 asserts the column is alive and varying,
  not that it carries the right number. A writer reading the wrong array
  would pass.
- **The `.rpt` still has no gate** — carried from the last round.
- **This does not touch the LID layer temperatures** (H5b), which have their
  own state and are not in any corpus deck (issue #131).

## 6. What the last round closed, recorded here so it is not re-derived

`421e95c2` closed all three gaps its §6 flagged. `sheds_to_self` is **live**,
not decoration — drop it and a self-routed subcatchment's booked runoff falls
2.328 → 0.123 in. Falsifiers iii and v came back clean: `outlet_subcatch < 0`
and `outlet_node >= 0` are genuine complements, and cascade depth is
irrelevant to a per-subcatchment guard (3-deep books 0.318 against an
all-direct 0.625, legacy agreeing to the digit on both).

## 7. ⛔ Finding 3, NOT in this changeset — and it is the biggest of the three

**Writing the falsifier-i fixture exposed a routing defect.** On a
self-routed subcatchment:

| deck | ours | legacy |
|---|---|---|
| direct | 0.417 | 0.417 |
| cascade (2-deep) | 0.218 | 0.218 |
| **selfroute** | **2.328** | **0.417** |
| cascade (3-deep) | 0.318 | 0.318 |
| all-direct | 0.625 | 0.625 |

Four of five agree to the digit. `selfroute` diverges **5.6×**, with −265 %
continuity on our side. **The ledger is not the problem — the divergence is
one layer up.** Legacy carries `!= subcatchIndex` in **three** places:

| site | what it guards |
|---|---|
| `subcatch.c:546-548` | **run-on distribution** — we do not have it |
| `subcatch.c:763` | the ledger — `421e95c2` gives us this |
| `surfqual.c:363` | washoff — we do not have it |

So a self-routed subcatchment **recirculates its own runoff**, and the ledger
faithfully reports a hydrology that is itself wrong.

**It changes routed water, so it needs its own round and its own falsifiers**,
and no corpus deck has a self-route. The fixture is written. It is the
largest of the three findings and I would take it next.

**(142) a guard that appears once in our code may appear three times in
legacy, and porting one site is not porting the invariant.** We matched
`subcatch.c:763` and called the divergence closed; two sibling sites with the
same condition were never looked for. When a fix is justified by "legacy
guards this here", grep legacy for the *condition*, not the line.

## 8. Prepared commit message

```
fix(heat): the subcatchment temperature column had no writer

heat_parity.inp, first run: nodes and links carried -4.147...17.66 degC while
every subcatchment read exactly 0.0 for the whole run. The snapshot's
subcatchment loop had an age writer and no temperature sibling, so
subcatch_quality kept its assign(..., 0.0) and the output plugin faithfully
wrote the zero. The value was in ctx_.heat_state.subcatch_runoff_temp all
along, and the column was in the header, which is why a column-presence check
would have passed over it.

The write sits outside the has_runoff gate the age column obeys. Age is gated
because legacy's washoff convention says a subcatchment producing nothing
reports nothing; temperature cannot borrow that, because 0 degC is a real
temperature and DRY_ELEMENT_TEMPERATURE exists so the dry value is the deck's
choice.

Two gates: one reads values out of the finished .out (the state was always
correct, so a state gate cannot fail on this) and asserts the column is
neither identically zero nor constant; the other stops the rain and asserts a
dry subcatchment still reports, which is the only thing that can catch the
write being moved back inside the gate.
```

---

## 9. Validation results (2026-08-22) — COMMITTED `29cbc361`

**The writer is right. Both gates were not** — one misread the output API and
the other could not see the defect it exists for. Both fixed, both now fail at
base and on falsifier i.

**ctest 159/160 three times** (the standing Track I failure; §4.2's count
prediction was right). **17/18 corpus decks byte-identical, `heat_parity`
moved** — the pass condition §4.3 named. Numbers:
`tests/output/subcatch_temp_2026-08-22/`.

### 9.1 ⛔ Both gates failed WITH the fix, on the output reader

```
Expected equality of these values:
  swmm_output_get_subcatch_attribute(h, attr, period, v.data(), &count)
    Which is: -1
```

`swmm_output_get_subcatch_attribute(handle, subcatch_idx, period, …)` returns
**all variables for ONE subcatchment**; its second parameter is a
subcatchment index, not an attribute code. Both gates passed the attribute
there, indexed past the end of a two-element object list, and got `-1` for
every period — so they failed on the read and never reached what they are
about. The reader that does what they wanted is
`swmm_output_get_subcatch_result(handle, period, var, values)`.

Repointed, with the distinction recorded at the call site. **The name is the
trap**: "get_subcatch_attribute" reads as "one attribute across
subcatchments", and it is the opposite.

### 9.2 ⛔ Gate 2 could not catch the thing it exists for, and the reason is a
###      predicate mismatch

With the reader fixed, gate 2 failed its own SETUP: `dry_periods_seen == 0` at
the 60-minute deck. Measured — the subcatchments are **still shedding
0.0624 cfs at the last reported period**. Rain stopping is not runoff
reaching zero. The SETUP leg did exactly its job.

Extending to 360 minutes gave **44 of 72 reported-dry periods** and the gate
passed. **It also passed under falsifier i** — the write moved inside
`has_runoff` — which is the single case it was written to catch.

**The two "dry"s are different predicates.** The writer tests
`runoff[s] != 0.0`, an exact double comparison on a decaying quantity that
essentially never turns false; the gate reads the `.out`'s float column, which
rounds to `0.0f` long before. Every one of those 44 periods was dry to the
reader and wet to the writer.

**Fixed with `starve_receiver`** — S2 sits on a gage that never rains, so its
runoff is exactly 0.0 from the first step and both predicates agree. Falsifier
i now reports `12 of 12 dry subcatchment-periods report 0.0 degC` and the gate
fails, while gate 1 passes: the asymmetry §3 wanted from having two gates.

**(143)** *"dry" is not one predicate. A gate that reads a rounded output
column and a writer that tests an exact double disagree on a decaying
quantity, and the gate then passes on the defect while looking like it
exercised it. Make the fixture dry by construction, not by waiting.*

### 9.3 §4.1 and §4.4: both base failures, and the column is physical

At base, with the reader and fixture corrected:

```
every subcatchment temperature in the .out is exactly 0.0 across 12 periods
  — the column exists and nothing writes it
88 of 88 dry subcatchment-periods report 0.0 degC
```

With the writer, `heat_parity`'s subcatchment column reads
**S1 −4.147…19.59, S2 −4.147…17.67, S3 −4.147…19.68 °C** against **0…0** at
base. Nodes and links carry −4.147…17.66. The shared minimum is the coldest
water in the model and the subcatchment maxima sit slightly above the piped
water, which is what a surface under warm air should do. Not absurd.

The corpus moved `heat_parity` by **2003 of 51520 bytes** — 3 subcatchments ×
4 bytes × ~167 periods is 2004. One float column, exactly.

### 9.4 Falsifier iii: inert, and here is why

§5 said "expect a crash or corruption, and if neither happens, ask why."
Neither happens: dropping `temp_col` changes nothing, and a no-heat deck's
`.out` is byte-identical.

**Because `subcatch_runoff_temp` is empty when heat is off.**
`resizeWatershed` is called only from `routeSubcatchmentTemperature`, which
returns at once on `!ctx.options.heat_transport`
(`HeatWatershed.cpp:100,112`). The `s < …size()` test therefore fails for
every `s` and nothing is written. `temp_col` is **provably redundant at this
site** — and so are its node and link siblings at `:4588` and `:4645`. Kept,
because it mirrors them and documents intent; recorded as belt-and-braces
rather than load-bearing.

### 9.5 Falsifier v answers a different question than it asked

On `heat_parity`, at reported-dry periods: temperature is nonzero (0 of ~90
are zero) — the asymmetry's temperature half holds. **But age is nonzero
too** (0 of ~90). The age gate is the same `runoff == 0.0` exact test, so on
a deck with no genuinely dry subcatchment it never engages.

**This is not a divergence** — legacy uses the identical exact comparison
(`subcatch.c:929`, `if ( runoff == 0.0 ) z = 0.0;`). It means the washoff
convention is narrower than it reads: it blanks the column only for a
subcatchment that receives nothing at all, not for one that has stopped
producing. Recorded as an observation about both engines, not a defect in
either. It also sharpens §2's argument: the exactly-dry case is precisely
where temperature must still report, and gate 2 now tests exactly it.

### 9.6 Falsifiers ii and iv

- **ii — write a constant**: gate 1's *varies* leg fails, the *nonzero* leg
  passes, as predicted.
- **iv — remove the SETUP leg on a deck with no dry period**: gate 2 passes
  vacuously. The leg is load-bearing, and §9.2 is what it caught.

### 9.7 Housekeeping and what is owed

- **The commit carries one hunk.** `SWMMEngine.cpp` in the tree also held two
  uncommitted `refreshRenderFieldsIfStale()` calls from another session's 2D
  work; the committed blob is HEAD + the temperature block only, and it was
  built and run on its own (12/12) before committing rather than assumed to
  compile. Worth flagging to that session: their second call, at the
  no-plugin snapshot path, is **not** inside `#ifdef OPENSWMM_HAS_2D` while
  the first one is.
- §5's gaps stand: `AIR` and `DEFAULT` dry policies are still untested and
  now matter more; no gate compares the column against
  `subcatch_runoff_temp` itself; the `.rpt` still has no gate; LID layer
  temperatures are untouched.
- **Finding 3 — self-routed subcatchments recirculate — is still open** and
  is the largest of the three (§7). Nothing here touches it.
