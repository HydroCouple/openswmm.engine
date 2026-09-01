# `dt`-Refinement Reference — Validation & Commit Handoff (2026-08-19)

**For:** the checking agent.
**Base:** `815f0e8e`.
**Standing findings:** lessons 1–98.

**This round is different from every previous one: the deliverable is an
INSTRUMENT, and its bands are deliberately unmeasured. Setting them is the
validation task, not a side effect of it.** Read §3 before anything else.

---

## 1. What this is

Four falsifiers have accumulated that no single-run gate can observe:

| owed item | the defect | carried since |
|---|---|---|
| A4 falsifier iii | mix order — donor's NEW value, not its old one | `5b2b7418` |
| H5a falsifier vi | flux applied to the POST-mix volume `v_old + v_in` | `65cae8a8` |
| D-H5e caveat | one long relaxation step vs many short ones under the real nonlinear flux | `c292b8eb` |
| H5b falsifier ii | conduction as a SEPARATE pass rather than inside `J(T)` | `1c78e9dd` |

Each is a **`dt`-order error**: the correct and defective forms converge to
the *same* limit, so a fixed-`dt` assertion sees two numbers that are both
plausible and cannot say which is right. H5b's round established this for the
last one and proved a stronger statement — conduction's fixed point and the
atmospheric one *coincide*, so even a convergence check proves nothing on its
own.

One instrument closes all four.

## 2. What it measures, and why it is not "does it converge"

Every one of these schemes converges. The discriminator is **how fast,
relative to the answer's own scale.** Each gate runs the same deck at three
timesteps (`WET_STEP` 60/20/5, `ROUTING_STEP` 20/5/1) and asserts:

1. **Contraction** — `|A(h) − A(h/2)| > |A(h/2) − A(h/4)|`. Reference-free: a
   statement about the sequence, not about any expected value.
2. **Already close at the coarse step** — `|A(h) − A(h/4)|` is a small
   fraction of the **spread of the deck's own sources** (`|init − rain|`).
   The scale comes from the deck, so no golden number is embedded in the
   comparison itself.

A splitting or mix-order defect leaves (1) intact — it is still first-order —
and blows up (2), because its error **coefficient** is several times larger.
That asymmetry is the entire instrument.

Each gate also carries a SETUP assertion that the observable **moves at all**
between the coarse and mid steps. Lesson 96 cost a whole gate to a branch
that was never entered; a `dt`-refinement gate on a deck with no
`dt`-dependent term would be vacuous in exactly the same way.

## 3. ⛔ THE BANDS ARE NOT MEASURED, AND THAT IS THIS ROUND'S WORK

Every `band` argument is currently **0.05** (5 % of source spread). **I chose
that a priori and have run nothing** — this was written in a syntax-only
sandbox.

A band left at 0.05 is a gate that has not been established. **For each of
the five `ExpectConverged` call sites:**

1. Run it as delivered and **record `total / spread`** — the correct form's
   actual convergence error.
2. Apply the corresponding falsifier from §5 and **record `total / spread`
   again** — the defective form's.
3. **Set the band between them**, nearer the correct value, and **report both
   numbers.**

If the two are not separated by a comfortable factor, **say so** — that means
this instrument does not discriminate that particular defect, which is a real
finding and exactly what H5b's round produced for falsifier ii by a different
route. Do not stretch a band to make a gate pass. Lesson 55: find the regime
where the law is exact rather than widening the band to fit; if a defect is
not separated at these three levels, the honest options are a deeper ladder,
a different observable, or recording it as undiscriminated.

## 4. Design decisions to review

### 4.1 The ladder stops at `WET_STEP = 5 s`

`WET_STEP` is `HH:MM:SS`, so the runoff clock cannot go below one second.
60/20/5 is the deepest 4×-per-level ladder the deck format allows. **If the
separation in §3 is marginal, this is the first thing to challenge** — a
2×-per-level ladder (60/30/15/…) buys more levels at less contrast per level.

### 4.2 Bands are fractions of source spread, not absolute values

So that a band means the same thing if a deck's forcing changes. The cost is
one more thing that can be zero — hence the `ASSERT_GT(spread, …)` guard.

### 4.3 One deck family, four configurations

All four gates share `write_files`, differing only in which capabilities and
flux modules are on. That keeps the instrument single, but it does mean a
deck defect hits all four gates at once. **Flag if you would rather they were
independent** — I judged one debuggable deck better than four.

### 4.4 The storage node exists only for gate 3

`ST1` is a small functional-storage node, added so the atmospheric flux has a
free surface with a resolved-but-modest volume. Without it the only heat
elements are conduits, whose volumes swing with the routing step and would
confound a `dt` refinement with a hydraulic change.

## 5. Validation protocol

1. **Isolated worktree at `815f0e8e`.** Lesson 71. Suite was **154/154** and
   nothing failed at base (lesson 89) — carry no exemption forward.

2. **⛔ HARD STOP — lesson 79.** `git diff --cached --numstat` must read
   exactly `1  0` for `tests/unit/engine/CMakeLists.txt`. Any deletion count
   above zero: STOP, rebuild the index, `update-ref <new> <old>`.

3. **Runtime.** The fine level runs a 30-minute deck at `ROUTING_STEP 1` —
   five gates × three levels. If any single gate exceeds roughly a minute,
   **report the time**; a `dt`-refinement suite that is too slow to run every
   round is not an instrument, and shortening `end_min` is the lever.

4. Build, zero new warnings. Then §3 — **the measurement is the work.**

   **Anticipated failure modes. My record is 2 of 20, so weight the sweep
   above this list.**

   (a) **A gate's SETUP "the answer moves with `dt`" fires.** Most likely on
   gate 1 (age): if the LID column reaches steady state well inside 30
   minutes, the published age may be `dt`-independent. Lengthen the deck or
   pick an observable still in transient — **do not delete the SETUP.**

   (b) **Contraction fails because the hydraulics moved too.** Refining
   `ROUTING_STEP` changes the flow solution, not only the transport step, so
   part of `|A(h) − A(h/2)|` is hydraulic. If contraction fails while the
   transport scheme is correct, **check whether the flows converged** before
   suspecting transport — and if they did not, that is a finding about the
   instrument, not about H5b.

   (c) **Gate 4's two legs disagree.** Storage and soil are asserted
   separately on purpose; if one converges and the other does not, the
   conduction coupling is behaving differently at the ends of the column than
   in the middle.

5. **Falsifier sweep — each maps to exactly one gate, which is the point:**

   | falsifier | gate | measure |
   |---|---|---|
   | i. `WaterAgeLid`: read the donor's NEW age (drop the `a_old` snapshot) | 1 | A4 iii |
   | ii. `HeatWatershed`: use `v_old + v_in` as the flux volume | 2 | H5a vi |
   | iii. `relaxT`: return the explicit step always | 3 | D-H5e |
   | iv. `HeatLid`: apply conduction as a second pass after the atmospheric solve | 4 | H5b ii |
   | v. all four at once | 1–4 | a cross-check that the gates are independent |

   **For each: record `total / spread` before and after.** That table is the
   deliverable of this round.

6. **Prior suites:** full C++ suite, 14/14 decks, ASan/UBSan. This changeset
   adds only a test file — **any movement in an existing gate is news.**

7. **Record:** the five measured bands with both numbers each; the falsifier
   table; the runtime; and whether any of the four defects turned out to be
   **undiscriminated** at this ladder depth.

## 6. Changeset (uncommitted)

```
new:  tests/unit/engine/test_transport_dt_reference.cpp   (4 gates,
      5 ExpectConverged call sites)
mod:  tests/unit/engine/CMakeLists.txt                    (+1 target)
```

No engine source is touched. Syntax-clean under `-Wall -Wextra`; **nothing
built or run.**

## 7. Known gaps

- **The bands.** §3. This is the round's work, not a footnote.
- **The instrument cannot prove a scheme correct**, only that its error
  coefficient is small relative to a defective one's. It is a discriminator,
  not a parity check. Conduction still has no external reference (CSH and RHE
  model a streambed, not a layered LID), and this does not change that.
- Gate 3 exercises the linearization and the composition together. If it
  fails, they are not separable without a third configuration.

## 8. Prepared commit message

```
test(transport): a dt-refinement instrument for four owed falsifiers

Four falsifiers accumulated across A4, H5a, D-H5e and H5b that no single-run
gate can observe, because each is a dt-order error: the correct and
defective forms converge to the same limit. H5b's round proved the stronger
statement for conduction -- its fixed point and the atmospheric one coincide,
so even a convergence check proves nothing alone.

Each gate runs one deck at three timesteps and asserts a contracting
sequence (reference-free) plus a coarse-step error small relative to the
deck's own source spread. A splitting or mix-order defect stays first-order,
so it passes the first leg and fails the second: its error coefficient is
several times larger. That asymmetry is the instrument.

The bands are deliberately unmeasured -- see the handoff. Setting them from
the measured correct-form and defective-form errors is the validation task.
```

---

## 9. Validation results (2026-08-19) — COMMITTED `0e8e57df`

Isolated worktree at `815f0e8e`, preset `Darwin-tests-release`.
**155/155 ctest** (154 prior + this one), **14/14 decks byte-identical** to the
`815f0e8e` run, **88 tests clean under ASan/UBSan** with zero sanitizer
diagnostics, zero warnings from the new translation unit.
**Falsifier sweep: 5 of 5 observed**, each by its own gate.
Full numbers: `tests/output/dtref_validation_2026-08-19/measurements.md`.

### 9.1 The bands (§3 — the round's work)

| gate | observable | correct | ratio | defective | ratio | sep | **band** |
|---|---|---|---|---|---|---|---|
| 1  | LID storage age  | 0.000747 | 3.25 | 0.002268 | 2.99 | 3.04x | **0.0012** |
| 2  | subarea temp     | 0.008761 | 2.68 | 0.024273 | 2.45 | 2.77x | **0.014**  |
| 3  | node temp        | 0.017842 | 2.66 | NaN      | —    | inf   | **0.030**  |
| 4a | LID storage temp | 0.000650 | 3.62 | 0.002237 | 2.44 | 3.44x | **0.0011** |
| 4b | LID soil temp    | 0.016085 | 3.30 | 0.037097 | 2.49 | 2.31x | **0.023**  |

Gate 3's falsifier does not converge more slowly — the explicit step
**diverges** (NaN at the mid and fine levels, H5a's failure mode with the
5 °C refusal removed), and the finiteness leg catches it. That band is
therefore bounded only from the correct side.

`ExpectConverged` now **prints** `coarse / mid / fine / ratio / err-over-spread`
on every run, not only on failure: re-establishing a band after a change to
the schemes should not require editing the gate to see the numbers.

### 9.2 Two of the five did not discriminate as written

**Gate 1 failed in its correct form on a units error and could not see its
own falsifier.** `sourceSpread` returned the age spread in **hours** while
`subcatch_runoff_age` is published in **seconds**, so the ratio read
**248.8** against a band of 0.05 — a 3600x scale error. With that fixed the
correct form still sat at 0.0691, and falsifier i produced **0.0688**: a
separation of **1.00x**. The observable was four fifths not-LID —
the defect moved the subcatchment's runoff age by 5 s in 7440. Gate 1 now
reads the **storage layer's own age**, two hops from the source, where the
ordering error compounds.

**Both LID gates were reading a layer at the noise floor.** With the
underdrain at `1.0e-3` the storage layer settles at **3.06e-4 ft of a 0.75 ft
capacity — 0.04 %** — and its volume is **not monotone in `dt`**
(3.116e-4 / 3.232e-4 / 3.058e-4). A scan over the drain coefficient (§3 of
`measurements.md`) shows every LID observable improving by one to two orders
as the layer fills: at `1.0e-3` the contraction ratios are 1.0–1.5, at
`≤1.0e-4` they are 2.0–4.1. `drain_coeff` is now part of each gate's
configuration — **0 for gate 1** (sealed, so the mix order is the only
`dt`-dependent term left; 1.00x → 3.04x) and **1.0e-4 for gate 4** (two
thirds full and draining; 1.49x → 3.44x and 1.77x → 2.31x).

Gate 1 at `1.0e-4` **anti**-discriminates — correct 0.001362, defective
0.000292. The defect's sign opposes the discretization error there, so a
larger error coefficient reads as a *smaller* `|coarse − fine|`. Leg (2) is a
one-sided test and this is its blind spot.

### 9.3 Answers to §4

**4(a) — the SETUP did not fire.** Gate 1's age is still in transient at
30 minutes on both deck variants.

**4(b) — measured, and it decides everything.** The flow solution is not the
same at the three levels. On the no-LID deck (gates 2, 3) the node depth
moves **0.10 %** coarse→fine and contracts at **4.32**, so those gates measure
the transport scheme. On the LID deck it moves **4.83 %** and contracts at
**2.76** — which is why the LID gates' ratios sit at 2.4–3.6 rather than 4,
and why the contraction ratio is reported but not asserted: pinning it would
gate the hydraulics. The falsifiers do **not** change the hydraulics (node
depth and every layer volume are bit-identical between correct and defective
runs), so every separation above is measured at a fixed flow solution.

**4(c) — gate 4's two legs agree** at `1.0e-4` (ratios 3.62 and 3.30). They
do *not* agree if the cell is sealed as gate 1 is: the soil leg falls to 1.40,
and to **0.93 on a 60-minute deck**, which would fail contraction. That is
the end-of-column-versus-middle asymmetry the handoff anticipated, and it is
what set gate 4's drain apart from gate 1's.

**4.1 — the ladder was not the problem** and was left at 60/20/5. Every call
site separates at this depth once the deck lets the term under test dominate.

**4.3 — keep the one deck family.** The round is the argument for it: a
single knob on the shared writer was the difference between an instrument and
two vacuous gates, and it fixed both at once.

### 9.4 The gates are not fully independent

Falsifier ii trips **gate 3** as well as gate 2, at 0.0433 against a band of
0.030. That node's water is the subcatchment's runoff, so the subarea
mixing-volume defect reaches it. A gate-3 failure narrows the cause to the
relaxation **or** the subarea mixing volume, not to one of them; noted in the
gate. In the other direction, falsifier iii is **invisible to gate 2**
(7.7e-08) — the explicit step gives a `dt`-independent subarea answer that
happens to sit near the converged one.

### 9.5 Runtime (§5.3)

**39–46 ms for the whole binary** — 12 simulations of a 30-minute deck, the
finest at `ROUTING_STEP 1`. The concern does not arise; `end_min` untouched.

### 9.6 What is still owed

- **The four items this instrument was built for are now gated**, but gate 1
  and gate 4 are gated *in the regime where the term under test dominates*,
  not on an ordinary deck. A defect that only appears in a fast-draining cell
  is still unobserved.
- Leg (2) is one-sided: a defect whose sign opposes the discretization error
  reduces `|coarse − fine|`. Gate 1 at `drain_coeff = 1.0e-4` is a worked
  example. A two-sided form would need a reference value, which is the thing
  this instrument exists to avoid.
- Conduction still has **no external reference** (CSH and RHE model a
  streambed, not a layered LID). This discriminates two compositions; it does
  not validate either.
- Issue #131: when the LID unit conversion lands, convert the deck rather
  than widening a band — every number in 9.1 is tied to feet and ft/s.
