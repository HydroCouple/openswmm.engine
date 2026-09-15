# The LID underdrain gates were calibrated against a 43200× bug — Handoff (2026-08-30)

**For:** the checking agent.
**Base:** `8f9f164d` (180 registered, 175 passing, 5 failing).
**Standing findings:** lessons 1–184.
**Diagnosis only — no code changed.** This round says *whose* problem the four
heat/LID failures are, and the answer is not the one the sequence assumed.

---

## 1. The verdict: PR #103 is RIGHT. The gates encode the defect it fixed.

The re-baseline left one question open: *did PR #103 break underdrain return,
or do the H5b/A4 gates encode a drain behaviour it deliberately changed?*

**Neither, quite. The gates encode a BUG that PR #103 repaired.** Derived
statically from the two versions, no run required:

**At `8c8faa3c` (before the merge):**
```cpp
g.drain_coeff[us] = p[0];                    // seeded from the deck, in/hr
...
return coeff * std::pow(h, expon);           // used directly AS ft/s
```
`drain_coeff` is a user-units rate — **in/hr** — and it was returned as though
it were **ft/s**. `UCF(RAINFALL)` is **43200**. So the underdrain ran
**43 200× too fast.** Compounding it, `h` was in **ft** while `offset`,
`hOpen` and `hClose` were seeded **unconverted in inches**, so the head
comparisons and the `pow(h, expon)` base were also wrong.

**At `8f9f164d` (after):**
```cpp
g.drain_coeff[us]  = p[0];                   // user units; see getDrainRate
g.drain_offset[us] = p[2] / ucfRainDepth;    // in|mm → ft
g.drain_hopen[us]  = p[4] / ucfRainDepth;
g.drain_hclose[us] = p[5] / ucfRainDepth;
...
double h_user = h * ucfRainDepth;                        // ft → in
return coeff * std::pow(h_user, expon) / ucfRainfall;    // → ft/s
```
Dimensionally correct: `in/hr · in^expon / 43200 → ft/s`, with the head
comparisons now in consistent units.

**Note the seeding line for `drain_coeff` is IDENTICAL in both.** PR #103
changed only the *comment* there and the *use* — which is why a diff-reading
sweep can miss that the storage convention was never what the old comment
claimed. The old comment said *"In internal units drain_coeff is already in
ft/s"*, and it was never true.

## 2. Why the four gates fail, and why that is correct behaviour

With drainage 43 200× too fast, a LID storage layer emptied essentially
instantly. Every one of the four gates was authored in that world:

| gate | what it asserts | why it now fails |
|---|---|---|
| `water_age_lid.StorageAgeIsTheSumOfTheLayerResidenceTimes` | `held_s/chain_s ≈ 1` | measured **0.00057** — storage now retains water, so held age is no longer the instant-flush sum |
| `water_age_lid.DrainLeavesAtStorageAgeAndReachesTheNode` | drain water arrives at the node | it arrives far later, outside the horizon |
| `heat_watershed.EveryRunonContributorKeepsTemperaturesInsideTheSources` | `runon_inflow[0] > 0` | **0** — the underdrain has not returned within the run |
| `heat_lid.ADrainedLayerStillConductsAndIsNotResetByThePolicy` | the layer has drained | *"storage still holds 0.0413 ft"* |
| `transport_dt_reference.LidColumnTemperatureConverges…` | second-order convergence | 1.67 ratio — the column no longer reaches the regime the band was fitted in |

**Every symptom is "the water is still there", which is what physically
correct drainage looks like** next to a 43 200× flush.

## 3. This is the `landuse.c` finding again

The quality-ledger round found our vendored `landuse.c` carrying `>= 0.0`
where stock EPA has `== 0.0`, under which any land use with a buildup function
washed off nothing — **the parity reference itself was corrupted, and work had
been calibrated against it.** This is the same shape one subsystem over: a
units defect in the reference implementation, with gates fitted to the defect,
so **repairing the defect presents as a regression.**

**The tell is identical in both cases: the "regression" makes results MORE
physical, not less.** That is the question to ask first whenever a fix breaks
tests.

## 4. What the fixing round must do — and must not

**MUST NOT: "fix" `LID.cpp` to restore the old drainage.** That would
reinstate a 43 200× error to keep five gates green. If any pressure exists to
do that because the sequence is blocked, **the sequence is what should yield.**

**MUST: re-derive the four gates' expectations against correct drainage.** For
each, decide deliberately whether the gate's *claim* still holds and only its
*numbers* moved, or whether its claim depended on instant flushing:

1. `StorageAgeIsTheSumOfTheLayerResidenceTimes` — the claim is structural
   (held age = Σ layer residence times) and should still hold; the ratio 0.00057
   suggests the run is now too short for the chain to fill. **Likely a horizon
   fix, not an expectation fix — check before changing any number.**
2. `DrainLeavesAtStorageAgeAndReachesTheNode` — same; extend the run until the
   drain actually delivers, then assert.
3. `EveryRunonContributorKeepsTemperaturesInsideTheSources` — `runon_inflow[0] == 0`
   means the contributor never contributed. The claim is a bound on
   temperature *given* run-on; with no run-on the gate is **vacuous, not
   failing**. It needs a deck that produces run-on within its horizon.
4. `ADrainedLayerStillConductsAndIsNotResetByThePolicy` — the name asserts
   behaviour of a **drained** layer. If the layer no longer drains in the run,
   the premise is gone. Either drive it to drain or rename to match what it
   now tests.
5. `LidColumnTemperatureConvergesUnderRefinement` — the band was measured in
   the old regime. **Re-measure the floor; do not widen the band to fit**
   (lesson 149).

**Every one of these is a horizon/premise question before it is a number
question.** Changing expected values first is how a gate stops observing
anything.

## 5. Protocol

1. **Confirm §1 by measurement**, not by reading my diff: instrument
   `getDrainRate` on the `heat_lid` deck at both commits and report the drain
   rate ratio. **I predict ≈43 200× modulo the `pow(h)` base change** — if it
   is not within a couple of orders of that, my analysis is wrong and the rest
   of this document should not be trusted.
2. Take the five gates one at a time, each with its own before/after numbers.
3. **The corpus must stay 20/20** against
   `tests/output/rebaseline_8f9f164d/corpus/` throughout — no corpus deck has
   LID underdrains, so gate work must not move any of them.
4. **Falsifier for the round as a whole:** restore the old
   `return coeff * pow(h, expon)` and confirm all five gates go green again.
   **That is the proof they were fitted to the defect** — and the reason the
   restoration must then be reverted rather than kept.

## 6. Consequences to record

- **`fv_tpa_closure` is unrelated and is the peer's** — it arrived red with
  `47c00ae3`, before either merge, O-6 absent throughout.
- **H7b stays blocked** until at least the three heat gates are re-derived,
  per the re-baseline handoff §6: building a temperature capability on top of
  unexplained temperature failures makes attribution impossible.
- **Any prior H5b/A4 result measured through a LID underdrain is suspect** —
  those rounds validated against 43 200×-fast drainage. Nothing is known to be
  wrong; it is that the evidence was taken in the defective regime and should
  not be cited as if it were not.

---

# CHECK RECORD — 2026-08-30

**Verdict: §1's direction is right — PR #103 is correct and the gates encode
the defective regime — but the diagnosis is too narrow in two ways its own
§5.4 falsifier exposed.** No code changed; nothing committed. Measured in
clean worktrees at `8c8faa3c` and `8f9f164d`. Artifacts:
`tests/output/lid_underdrain_gates/` (probe `.err` files, falsifier logs,
the `heat_lid` deck).

## §5.1 — confirmed by measurement: 12 470.8× = 43 200 / √12

`getDrainRate` instrumented (env-gated stderr) at both commits, same
`heat_lid` deck (`DRAIN 0.001 0.5 0 0`, 3 h):

| | head at first drain | rate (ft/s) | fits |
|---|---|---|---|
| `8c8faa3c` | **50 ft** | 7.07e-3 | `1e-3 · h^0.5` exactly (in/hr used as ft/s) |
| `8f9f164d` | 0.0417 ft (= 0.5 in) | 1.64e-8 | `1e-3 · (12h)^0.5 / 43200` exactly |

Old/new at equal head = **12 470.8**, the analytic `43200/12^0.5` to six
figures — the handoff's "≈43 200 modulo the `pow(h)` base" prediction holds.
Note the old head: **50 ft** — the storage layer itself was seeded in inches
read as feet, so the old regime was not only a fast drain but a 12× deeper
column. Runoff continuity on that deck: **96.5 % at `8c8faa3c`, −0.067 % at
`8f9f164d`** — the "more physical" tell from §3, in numbers.

## §5.4 — the round's falsifier FAILED as written, and that is the finding

**Restoring only `return coeff * pow(h, expon)` at `8f9f164d` leaves all four
gates RED.** The gates were not fitted to the drain rate; they were fitted to
the **whole pre-#103 units regime** — PR #103 converted every LID parameter
(surface store, soil/storage/pavement thickness in→ft, all three `ksat`
in/hr→ft/s, void ratio→fraction, `init_sat` %→fraction, drain delay h→s,
offset/hOpen/hClose in→ft, plus the `storageDrain` fallback) and added an
availability clamp on `exfil + drain`. Split by construction:

| falsifier | what it restores at `8f9f164d` | `water_age_lid` | `heat_lid` | `transport_dt_ref` | `heat_watershed` |
|---|---|---|---|---|---|
| A (§5.4 as written) | drain return only | red | red | red | red |
| **B** | **all 19 unit hunks** (clamp + `total*Volume` kept) | **green 6/6** | **green 9/9** | **green 4/4** | red |
| C | #103 units kept, clamp OFF | red | red | red | red |
| **D** | pre-merge own-subcatch drain semantics in `SWMMEngine.cpp` | — | — | — | **green 12/12** |

So: **three gates encode the units regime (B), not the drain line alone**;
the availability clamp is not what they see (C); and **`heat_watershed` is
a different defect entirely (D)**.

## The fourth gate is a heat finding, not a units finding

The merge's `SWMMEngine.cpp` changed the LID-drain destination logic: before,
`target_sc = drain_subcatch >= 0 ? drain_subcatch : sc` and the drain was
booked as **run-on to that subcatchment** (own subcatchment included), with
`addRunonTemperatureAt`/age paired to it. After, the run-on branch fires only
for `drain_subcatch != sc`; **a drain returning to its own subcatchment is
now `runoff[usc] += drain_flow · area` — with NO temperature and NO age
carried.** `EveryRunonContributorKeepsTemperaturesInsideTheSources` asserts
`runon_inflow[0] > 0` on exactly that deck (`S1 BC1 … drain-to S1`), which is
why it reads 0. Two things follow:

1. The gate's *premise* changed (own-subcatch drain is no longer "run-on") —
   a semantics decision that came in with the remote branch and needs an
   owner: is an underdrain that discharges onto its own subcatchment run-on
   or runoff? Legacy `lid_addDrainInflow` should be the tie-breaker.
2. Whichever it is, **the merged path drops the drain's heat and age on that
   branch** (`runoff +=` with no `addRunonTemperatureAt` / age pairing).
   That is H5b's pair invariant broken — flow-knows/quality-doesn't, the
   seventh appearance — and it is a **temperature-transport defect**, so per
   the re-baseline handoff §6 **H7b stays blocked behind it**.

## §5.2 / §4 — per-gate "after" numbers, for the fixing round

Unchanged from the re-baseline record: `held_s/chain_s = 0.00057`;
`runon_inflow[0] = 0`; storage holds 0.0413 ft; convergence err/spread
0.0897 vs band 0.0011 (ratio 1.67). The fixing round should take §4's
horizon-first order for the three units gates, and treat `heat_watershed`
as the run-on-semantics + heat-pairing question above, not as a horizon.

## §5.3 corpus — vacuous this round

No code changed, so the corpus cannot move; not run. The reference at
`tests/output/rebaseline_8f9f164d/corpus/` stands.

## Notes

- Handoff §1's static reading was right about the drain line and wrong that
  it was the *only* thing; §5.4 was the instrument that caught it, which is
  what falsifiers are for.
- `fv_tpa_closure` untouched, still the peer's.
