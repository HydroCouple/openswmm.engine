# A4 Implementation — Validation & Commit Handoff (2026-08-18)

**For:** the checking agent.
**Base:** `d85429fb` (post-A3; two foreign commits sit on top of `b5be8ec3`).
**Plan:** `WATER_AGE_TRACKING_PLAN.md` §7 A4. Brief:
`A4_IMPLEMENTATION_BRIEF_2026-08-17.md`.
**User decisions taken 2026-08-18:** publish the inter-layer rates from the
solver; per-layer state generic over species; the underdrain leaves at the
STORAGE layer's age. Hotstart deferred — see §4.4.

---

## 1. Read this first: the brief's §1 premise is false

The brief said `LIDGroupSoA::f_old_surf/soil/stor/pave` are "inter-layer flux
rates … stored", and built its whole "an exact mixing volume is possible"
argument on them. They are not.

* `f_old_soil`, `f_old_stor`, `f_old_pave` are allocated at `LID.cpp:146-149`
  and **never read or written anywhere else in the codebase.**
* `f_old_surf` is touched only by `batchSwaleModPuls`, where it is the
  Modified Puls time-weighting term — the **net `dx/dt`** of the surface
  layer, `inflow − evap − infil − runoff`.

A net rate of change is precisely the quantity that made A3 report elapsed
time instead of age. Building A4 on those fields would have reproduced the
defect the brief was written to prevent.

**The conclusion survives by a different route.** Complete-mix means outflow
leaves at the layer's own value, so only each layer's **inflow** is needed —
and every `batch*Flux` routine already computes exactly that as a local
(`soil_infil`, `soil_perc`, `pavePerc`, `storageInflow`, …). A4 publishes
them as `in_surf/in_pave/in_soil/in_stor`, written after every clamp in all
eight routines. That is a hydrology touch, which the brief assumed would not
be needed; it is purely additive, and the full suite is unchanged by it.

## 2. Changeset (uncommitted)

```
mod:  src/engine/hydrology/LID.{hpp,cpp}   (4 per-layer inflow rate fields;
      written in all 8 batch*Flux routines; in_stor gated on the storage
      layer existing — see §5 falsifier vi)
new:  src/engine/data/LidLayerSpeciesData.hpp   (generic per-(unit, layer,
      species) block; LidLayer, LidSpecies)
new:  src/engine/transport/components/WaterAgeModule/WaterAgeLid.{hpp,cpp}
mod:  src/engine/data/WaterAgeData.hpp     (node LID-drain age accumulator;
      the two run-on age accumulators of §3)
mod:  src/engine/core/SimulationContext.hpp (lid_layer_state)
mod:  src/engine/core/SWMMEngine.hpp       (lid() accessor)
mod:  src/engine/core/SWMMEngine.cpp       (init; per-unit inflow age; the
      per-layer update; drain age → node; run-on age for the LID-drain and
      outfall paths; the A6b np-guard — see §3)
mod:  src/engine/quality/QualityRouting.cpp (LID drain loader retires its
      RAINFALL stand-in, which named phase A4 as its fix)
new:  tests/unit/engine/test_water_age_lid.cpp  (6 gates)
mod:  tests/unit/engine/CMakeLists.txt     (+1 target — SHARED FILE, and HEAD
      has moved twice this round; merge onto HEAD's blob, do not stage a
      worktree copy)
```

242 insertions across 8 modified files, 1111 lines of new files.

## 3. What A4 found in already-committed code

### 3.1 A3's run-on age was divided by a rate its numerator never saw

`routeSubcatchmentAge` computes `runon_age = subcatch_runon_age_vol_in /
runon_inflow`. A3 filled the numerator from the subcatchment cascade alone —
but `runon_inflow` has **three** contributors: the cascade, LID underdrain
return flow (`lid_drain_runon_cfs`), and outfall return flow
(`outfall_runon_vol`).

This is not a precision loss. Measured on A4's composition deck: arriving
water at **3.834 h under a 4 h rain, with no source younger than 4 h anywhere
in the model** — subarea ages of 3.644 / 3.653 / 3.883 h, all below the
youngest thing entering. After the fix: 4.189 / 4.197 / 4.427 h.

A3's gates could not see it because none of its decks had a LID or an outfall
returning flow, so `runon_inflow` came only from the cascade. **Sixth
appearance of the flow-knows/quality-doesn't family** (R4, E5a, A1a, H1, A3,
now this).

Fixed by carrying an age with both other contributors:
`subcatch_lid_drain_age_cfs` (filled in A6b from A4's own drain age) and
`subcatch_outfall_age_vol` (filled at the outfall from that node's age), both
handed to `subcatch_runon_age_vol_in` in `assembleRunon` where the volumes
become rates.

### 3.2 The LID drain accumulator was gated on pollutants

A6b's drain-quality block ran only when `np_use > 0`, so a pure-water-age
model never accumulated `lid_drain_qual_vol` at all — neither the drain water
nor its age reached the node. The guard now admits `water_age`, which keeps
the age's numerator and its mixing denominator on the same cadence. Falsifier
viii puts the guard back and gate 4 fails.

### 3.3 LID layer parameters are still never unit-converted

Not A4's to fix (issue #131's remaining item), but it dominated this round.
The probe on a conventional `[LID_CONTROLS]` block shows `soil_thick = 18`
(inches read as feet) and `soil_ksat = 0.5` (in/hr read as ft/s, **43 200x**
too fast): the column drained every drop within one step, no layer held
water, no layer aged, the underdrain never flowed, and all four setup legs
fired.

**The gate decks are therefore written in feet and ft/s**, with a `@warning`
block saying so and saying what to do when the conversion lands. They will
fail loudly at that point, which is correct for a test standing on a known
defect — please do not "fix" them by widening a band.

## 4. Design decisions to review

### 4.1 Donor resolution is an explicit per-TYPE table

`donorsFor()` in `WaterAgeLid.cpp`, not inference. The eight stacks genuinely
differ and the solver is itself written one routine per type. The single
conditional is permeable pavement, whose storage takes soil percolation when
there is a soil layer and pavement percolation when there is not — mirroring
the solver's own `storageInflow_local`. **Flag if you would rather this were
derived from layer thicknesses**; I judged an explicit table more checkable
than a rule with the same number of exceptions.

### 4.2 A layer with no water carries no age

Two ways a layer ends up empty, both handled at the top of the update: the
TYPE does not have it (`donor == kAbsent` — a vegetative swale has soil
*parameters*, because that is where its infiltration conductivity lives, but
no soil *layer*), or it holds and receives nothing. Without this a swale's
soil row sat on a nonzero `soil_moist`, received nothing, and climbed to the
elapsed run time — and for a rain garden that row is what the underdrain
would have reported.

### 4.3 The unit's inflow age comes from A3's `subcatch_runoff_age`

Not from a reconstruction over the `subarea_age` rows. I wrote the
reconstruction first and it was wrong: a subarea can be shedding while its
END-of-step stored volume is zero, and the untouched zero of a dry row then
drags the mix below the youngest source. Using A3's published answer also
keeps the LID and the outlet node consistent with each other. **The cost is
that impervious and pervious capture are no longer distinguished** — flag if
that distinction is wanted, because it needs A3 to publish per-subarea
*outflow*, which it does not.

### 4.4 Hotstart deferred — verified, not inherited

There is no `surf_depth` / `soil_moist` / `stor_depth` / `pave_depth`
anywhere in `HotStartManager.cpp`, so a restored layer age would be a mean
over a volume that was not restored. A3's reasoning, checked against this
phase's own state.

### 4.5 Surface overflow is not fed back to A3's subareas

A LID's surface overflow returns to the subcatchment and its age is the
surface layer's, but it does not re-enter `subarea_age`. Recorded, not done.

## 5. Validation status and the falsifier sweep

| check | result |
|---|---|
| build | clean, no new warnings |
| `test_engine_water_age_lid` | **6/6** |
| full `ctest` | **150/150** — including every existing LID test, so the rate publication is behaviour-neutral |
| falsifiers | 11 run, **10 observed** |

| falsifier | result |
|---|---|
| i. mix on the NET change in stored volume (A3's defect) | gates 2, 3 |
| ii. drop the `+dt` aging term | gates 2, 3, 4 |
| iii. read the donor's NEW age instead of its old one | **nothing** — see below |
| iv. drain from the SOIL layer | gate 4 |
| v. revert the loader to the RAINFALL stand-in | gate 4 |
| vi. publish `in_stor` with no storage layer | gate 5 |
| vii. let an absent-by-type layer age | gate 5 |
| viii. re-gate the A6b accumulator on pollutants | gate 4 |
| ix. inflow age = the raw RAINFALL age | gate 6 |
| x. LID drain run-on carries no age (§3.1) | gate 6 |
| xi. outfall run-on carries no age (§3.1) | **nothing** — see below |

**Falsifier iii is unobserved and it is a real hazard.** Reading the donor's
already-updated value would let a parcel fall through the whole stack in one
step. At steady state — where every gate measures — the donor's age moves by
about `dt` between steps, 1.7 % against a 15 % band, so nothing sees it.
Catching it needs a TRANSIENT: step the inflow age and check the front takes
the chain time to arrive. **Owed, with the reason.**

**Falsifier xi is unobserved because no deck routes an outfall back to a
subcatchment.** The fix in §3.1 is symmetric with the LID-drain half that
gate 6 does cover, but its own path is untested. **Owed**, and the deck is
cheap: an `[OUTFALLS]` row with `route_to` a subcatchment.

## 6. Verification protocol for the checking agent

1. **Greps.** `grep -rn "water_age_state.resize\|ws\.resize(" src/engine/` —
   the signature did not change this round, but `WaterAgeState` gained three
   vectors, so every site must still be reached. `grep -rn "A4" src/engine/
   tests/` — A4 retires the QualityRouting stand-in; nothing else should
   still name it as owed.
2. `ctest` — expect 150/150. The LID suite is the one that would catch a
   behaviour change from publishing the rates.
3. Falsifier sweep: `tests/output/a4_impl_2026-08-18/falsifiers.py`.
4. **Deck bit-identity is expected to hold at 14/14 and to prove little** —
   A3's round established that no deck in that corpus enables `WATER_AGE`.
   The brief calls adding one owed work; it is still owed, and it would now
   cover A3's and A4's reserved columns at once.
5. ASan/UBSan. Expect the pre-existing misaligned CRC load at
   `HotStartManager.cpp:246` and nothing else.

## 7. Commit message

```
feat(transport): water age through the LID layer stack (A4)

Every layer of every LID unit is a complete-mix tank: what it holds ages by
dt, what arrives mixes in by volume, and what leaves - percolation,
evaporation, exfiltration, the underdrain - leaves at the layer's own age.
Only the inflow is needed, and every batch*Flux routine already computed it
as a local, so the four rates are published as in_surf/in_pave/in_soil/
in_stor and the age is exact rather than estimated. The f_old_* fields are
NOT those rates: f_old_surf is the Modified Puls net dx/dt of the surface
layer and the other three are allocated and never written.

State is per (unit, layer, species) from the start, so H5's temperature is a
row rather than a second array. Which layer feeds which comes from an
explicit per-type table, because the eight stacks genuinely differ. A layer
the type does not have, or one holding and receiving nothing, carries no age
- without that a vegetative swale's soil row, which exists only to carry an
infiltration conductivity, climbed to the elapsed run time and became what a
rain garden's underdrain reported.

The underdrain leaves at the storage layer's age, and the wet-weather loader
now delivers it instead of the RAINFALL stand-in that named this phase as its
fix. That path was also gated on pollutants, so a pure water-age model never
accumulated the drain volume at all and neither the water nor its age reached
the node.

A3's run-on age was divided by a rate its numerator never saw: runon_inflow
carries LID drain and outfall return flow as well as the subcatchment
cascade, but only the cascade contributed an age. On a LID deck that produced
arriving water at 3.834 h under a 4 h rain, younger than anything entering
the model. Both other contributors now carry an age.

Gates: tests/unit/engine/test_water_age_lid.cpp - the block sized against a
unit that is RECEIVING water, a one-layer unit held for its residence time
V/Q, the storage layer at the sum of the layer residence times (plan §7 A4's
own criterion), the drain leaving at the storage age and reaching the node,
an absent layer never mixing, and the unit's inflow carrying the
subcatchment's age rather than the rain's.

Plan: WATER_AGE_TRACKING_PLAN.md section 7 A4.
Brief: plans/transport/A4_IMPLEMENTATION_BRIEF_2026-08-17.md
Validation record: plans/transport/A4_VALIDATION_HANDOFF_2026-08-18.md
```

## 8. Also owed

- Falsifiers iii and xi (§5).
- The brief's §6 items, minus the first: `postOutputSnapshot`'s stale comment
  was already fixed in A3's commit `b5be8ec3`.
- `WATER_AGE_SNOW` (plan §8) remains untouched **and undeferred** — no error
  names the decision.
- An age-enabled deck in the bit-identity corpus (§6.4).
- Issue #131's LID unit conversion (§3.3), which this round's decks are
  standing on.

## 9. Validation results

*(appended by the checking agent, 2026-08-18)*

Validated in an isolated worktree at `d85429fb` carrying only the changeset,
per `A3_RUNON_FIX_SPLIT_2026-08-18.md` §3. Artefacts:
`tests/output/a4_validation_2026-08-18/`.

### 9.1 Outcome

| check | result |
|---|---|
| build | clean, **zero** new warnings in any A4 file |
| `test_engine_water_age_lid` | **6/6** |
| `test_engine_water_age_watershed` | **8/8** — 7 from A3 plus the contributor gate of §9.2 |
| full `ctest` | **149/150** — `FvEngine.RefiningTheMeshConvergesTowardTheDynwaveHydrograph`, the known bistable gate, failing with **the same digits** as at the A3 round's base (`0.055224237275644343` vs `0.052534507871460516`) |
| deck `.out` bit-identity | **14/14** against the A3-validated build |
| ASan + UBSan | 4 suites / **81 tests**, one hit: the pre-existing misaligned CRC load at `HotStartManager.cpp:246`. Nothing else |
| falsifiers | 11 run, **10 observed** |

**The instruction's §3.1 was right and my 150/150 was not attributable.** In
the main tree with foreign edits present the FV gate passed; in isolation at
`d85429fb` it fails, to the digit, exactly as it did at `8b5b3ef5`. Same
deterministic pre-existing failure — but the main-tree number had been
masking it, which is precisely the reason for the isolation requirement.

### 9.2 Requirement 3 — the A3 suite now has a LID-bearing deck

`WaterAgeWatershedTest.RunonFromEveryContributorKeepsAgesAboveTheSource`.
One subcatchment hosting a bioretention cell whose underdrain returns as
run-on, with `[OUTFALLS] OUT 9.0 FREE NO S1` routing the outfall's discharge
back as well — so all three `runon_inflow` contributors are live at once.
Setup legs assert each is actually present (run-on non-zero, the layer block
active, `route_to[0] == 0`) rather than assuming the deck built.

The assertion needs no reference value, which is what makes it durable:
**nothing in the model is younger than the RAINFALL source age**, so an
arriving age below it is impossible rather than merely inaccurate. That is
the signature the original defect announced itself with.

### 9.3 Requirement 4 — the stated falsifiers, and both now fail a gate

| falsifier | arriving age | verdict |
|---|---|---|
| x. LID-drain run-on carries no age | **3.968 h** under a 4 h rain | gate 8 + `UnitInflowCarriesTheSubcatchmentAge` |
| xi. outfall run-on carries no age | **0.348 h** under a 4 h rain | gate 8 |

Both produce the impossible-value signature. Requirement 3 was therefore not
optional and is now met: **falsifier xi failed nothing before this gate
existed** — it is the one the implementation round recorded as owed.

Worth recording: **the outfall half is the more consequential one.** Zeroing
it drops the arriving age to 0.348 h against 3.968 h for the LID half, so on
this deck outfall return dominates run-on. The instruction's option (2) —
landing the outfall half alone as an A3 fix — would in fact have removed the
larger error. That does not change the recommendation, because a partial fix
is still unguarded without a deck that has both, and this deck could not
exist before A4.

### 9.4 The full sweep

| falsifier | result |
|---|---|
| i. mix on the NET change in stored volume | gates 2, 3 |
| ii. drop the `+dt` aging term | gates 2, 3, 4 |
| iii. read the donor's NEW age | **nothing** — owed, see §5 |
| iv. drain from the SOIL layer | gate 4 |
| v. revert the loader to the RAINFALL stand-in | gate 4 |
| vi. publish `in_stor` with no storage layer | gate 5 |
| vii. let an absent-by-type layer age | gate 5 |
| viii. re-gate the A6b accumulator on pollutants | gate 4 |
| ix. inflow age = the raw RAINFALL age | gate 6 |
| x. LID-drain run-on carries no age | gates 6, 8 |
| xi. outfall run-on carries no age | gate 8 |

Only iii escapes, for the reason given in §5: at steady state the donor's age
moves by about `dt` between steps, 1.7 % against a 15 % band. It needs a
transient deck. **Owed, unchanged.**

### 9.5 Greps

`grep -rn "water_age_state.resize\|ws\.resize(" src/engine/` — six sites, all
passing the full argument list; the signature did not change this round.
`grep -rn "A4" src/engine/` — no source still names A4 as owed; the
QualityRouting stand-in is retired and its comment updated.

### 9.6 Commit

`5b2b7418` — 13 files. `tests/unit/engine/CMakeLists.txt` was staged by merging
the A4 line onto **HEAD's blob**, per §3.2; verified afterwards that the
foreign `saveas_paths` and 2D lines survive in the committed tree.
