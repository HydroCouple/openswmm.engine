# H5a Implementation — Validation & Commit Handoff (2026-08-19)

**For:** the checking agent.
**Base:** `5b2b7418`.
**Plan:** `HEAT_TRANSPORT_PLAN.md` §6 H5a and **§6.1 (new — read it first;
it records three user decisions taken today that changed the shape of this
phase and split H5 into H5a/H5b).**
**Standing findings:** lessons 1–73.

---

## 1. What this delivers

Temperature on subcatchment surfaces: **three per subcatchment**, one per
ponded subarea, mirroring A3's `subarea_age` row-for-row. Per runoff step,
per subarea: the surface energy balance acts on the water already there,
then what arrived mixes in by gross inflow volume. Runoff leaves at the
volume-weighted mean of the subareas holding water, and **that** temperature
reaches the outlet node and travels as run-on.

Plus a deck-selectable `DRY_ELEMENT_TEMPERATURE` policy (D-H5c).

## 2. Why this phase was re-scoped before a line was written

The H5 plan line scoped "watershed + LID temperature states", but its two
verify criteria were a **runoff temperature equilibration test** and a **LID
column conduction test** — and *neither mechanism was in the scope line*.
Surface energy balance was deferred to H6; no conduction term existed
anywhere. **Both criteria were unreachable by the phase as written**, which
is lesson 59 at the level of a plan rather than a gate. Raised with the user
before implementing; resolved as D-H5a (balance comes forward), D-H5b
(conduction is in, in H5b), D-H5c (dry policy is the deck's choice).

**A survey result that SHRINKS the phase:** the H5 line also promised
"DWF/GW/RDII source temperatures". Those already exist — all seven
`HeatSource` pathways are consumed at the loader seam in `QualityRouting.cpp`.
I checked the consumption sites, not the enum declaration; that distinction
is lesson 69 and it is the reason this paragraph is here rather than a claim
in a brief.

## 3. Changeset (uncommitted)

```
mod:  src/engine/data/HeatData.hpp
      (HeatSubArea, DryTempPolicy, HeatConfigData::dry_temp_policy,
       HeatState watershed rows + resizeWatershed/watershedSized)
new:  src/engine/transport/components/HeatModule/HeatWatershed.{hpp,cpp}
mod:  src/engine/transport/components/HeatFluxModules/SurfaceExchange.{hpp,cpp}
      (export deltaT, airTempCelsius, kSqFtToSqM/kCuFtToCuM/kMphToMs)
mod:  src/engine/transport/components/HeatFluxModules/RadiativeExchange.cpp
      (delete the now-duplicate local copies of the three above)
mod:  src/engine/transport/components/HeatModule/HeatComponent.cpp
      (DRY_ELEMENT_TEMPERATURE key)
mod:  src/engine/quality/QualityRouting.cpp  (wet-weather loader delivers the
      SUBCATCHMENT's computed temperature, not the configured RAINFALL one)
mod:  src/engine/core/SWMMEngine.cpp  (runoff-clock call; cascade and
      outfall run-on contributors; outfall temp-volume producer)
new:  tests/unit/engine/test_heat_watershed.cpp   (7 gates)
mod:  tests/unit/engine/CMakeLists.txt            (+1 target — SHARED FILE)
```

All touched TUs pass `g++ -std=c++20 -fsyntax-only`. **Nothing was built,
linked or run** — that is your half.

## 4. Design decisions to review

### 4.1 The run-on divisor is the KNOWN rate — the call most worth checking

`runon_inflow` has three contributors. H5a can supply a temperature for two
of them: the **cascade** (donor's runoff temperature) and the **outfall
return** (`heat_state.node_temp`, available since H1). The **LID underdrain**
cannot be supplied until H5b, because a drain's temperature is a per-layer
quantity that does not exist until the LID species row does.

A3 hit exactly this and filled its numerator from one of three while dividing
by all three, producing water younger than anything entering the model. So
H5a carries a **pair**: `subcatch_runon_temp_vol_in` (Σ q·T) *and*
`subcatch_runon_temp_rate` (Σ q), and divides one by the other. An uncounted
contributor then averages over less water instead of being dragged toward
0 °C. Both are written only by `addRunonTemperatureAt`, so writing one
without the other is not something a call site can do by accident (lesson 66).

**Challenge this if you disagree** — the alternative is to block H5a on H5b,
and I judged an explicitly-partial mean better than a phase that cannot land.

### 4.2 Two clocks, and the flux `dt`

`routeSubcatchmentTemperature` runs on the **runoff** clock (right after
`runoff_.execute`, where the depths it reads are current). H2's node and
link bindings run on the **routing** clock inside `routeLegacyHeat`. Each
passes its own `dt`. Verified reachable: the met writes at
`SWMMEngine.cpp:1379/1424/1431` are all inside `stepRunoff` **before**
`runoff_.execute` at `:1648`.

### 4.3 The exchange area excludes the LID footprint

`RunoffSoA::area` is subcatchment area **minus** LID footprint
(`Runoff.cpp:197-199`). Using `ctx.subcatches.area` would heat the footprint
H5b then heats again. Subarea fractions are `f0 = fi·pctZero`,
`f1 = fi·(1−pctZero)`, `fp = 1−fi`.

### 4.4 Three deliberate differences from `WaterAgeWatershed`

No `+dt` aging term; **no zero floor** (a sub-zero temperature is ordinary,
and clamping would manufacture energy on every frozen surface); a dry
subarea takes the D-H5c policy rather than A4's `= 0`.

### 4.5 Exported three helpers rather than writing a third copy

`deltaT`, `airTempCelsius` and the unit constants were file-local in
`SurfaceExchange.cpp` **and** duplicated in `RadiativeExchange.cpp`. The
watershed binding needed them, so they moved to the header and the duplicates
were deleted. Deleting them is not scope creep — my change is what made them
duplicates, and leaving them would have made unqualified lookup ambiguous.

### 4.6 Seeded from INITIAL_STATE, not kDefaultTemp

`resizeWatershed` seeds from `global_temp[INITIAL_STATE]`, the same source
`QualityRouting.cpp:164-166` seeds nodes and links from.

## 5. Validation protocol

1. **Isolated worktree at `5b2b7418`.** Lesson 71 — a count from the main
   tree is not attributable in either direction. Expect the bistable FV gate
   to fail there (`0.0552…` vs `0.0525…`); that is pre-existing, not this
   changeset.
2. `tests/unit/engine/CMakeLists.txt` must be **merged onto HEAD's blob**,
   never staged from a copy. Check the diffstat reads `1 insertion(+)` with
   no deletions before committing.
3. **Run these greps.**
   - `grep -rn "options.heat_transport" src/engine/` — H5a adds three guard
     sites; confirm the runoff-stage call, the outfall producer and the
     loader branch are all gated. Lesson 52 has appeared five times.
   - `grep -rn "heat_state.resize\|resizeWatershed" src/engine/` — `resize`
     must NOT touch the watershed rows and `resizeWatershed` must not be
     reachable with one argument.
4. Build, zero new warnings. `ctest -R test_engine_heat` (the existing heat
   suites must be **unchanged**), then the full suite.

   **Anticipated failure modes, in likelihood order:**

   (a) **`[TEMPERATURE] TIMESERIES air_ts` may not parse, or may not reach
   `ClimateState::temperature`. I DID NOT VERIFY THIS.** The survey told me
   temperature is written at `SWMMEngine.cpp:1379-1380` with the source
   resolved at `:1335-1367`; I did not read those lines to confirm a
   TIMESERIES source is among them. **This is the single most likely reason
   this changeset does not run, and I am flagging it rather than letting you
   discover it** — H3's gate 3 and A4's brief §1 were both cases of me
   asserting a mechanism I had not read. If the key is wrong, fix the deck
   (find the air-temperature source the engine actually reads), not the
   gates.

   (b) **Gate 5's dry branch may never be reached.** It needs subareas
   genuinely dry at the end of a 2-hour run after a 5-minute storm. If
   HOLD/AIR/DEFAULT all return the same number, check `subarea_vol_prev`
   before loosening anything — the likely cause is water still ponded, not a
   broken policy.

   (c) **Gate 5's `EXPECT_NEAR(t_air, 35.0, 0.5)`** assumes °F→°C on 95 °F
   exactly. If `ClimateState::temperature` carries something other than °F
   here the gate fails loudly, which is correct.

   (d) **Gate 6 may pass for the wrong reason** — if a malformed
   `[HEAT_FLUXES]` line already fails `open` for an unrelated parse reason,
   the gate cannot distinguish. Confirm the error message names
   `DRY_ELEMENT_TEMPERATURE`.

5. **Falsifier sweep:**

   | falsifier | expected failing gates |
   |---|---|
   | i. drop the `addRunonTemperature` call from the cascade scatter | **3** — the defect gate. If it fails nothing, the run-on claim is unobserved |
   | ii. revert the loader to `addTempVolume(…RAINFALL)` | 2 (and possibly 3) |
   | iii. divide by `subcatches.runon_inflow` instead of `subcatch_runon_temp_rate` | **4** — this is A3's exact defect, re-armed |
   | iv. drop `addRunonTemperatureAt` from the OUTFALL producer | **probably nothing on these decks** — flagged in advance: no gate here has a returning outfall. **Owed**, and it is the same gap that hid A3's defect for a whole phase |
   | v. skip the surface-balance block (advection only) | 2, 3 |
   | vi. apply the balance to `v_old + v_in` instead of `v_old` | **probably nothing** — flagged: it heats a step's rain before it arrived, a `dt`-order error. If nothing catches it, that is an owed transient gate, the same shape as A4's escaping falsifier iii |
   | vii. seed `resizeWatershed` from `kDefaultTemp` instead of INITIAL_STATE | 1 (its 0 °C check will not catch it; the seed assertion should) — **check this one specifically**, I am not confident gate 1 observes it |
   | viii. restore the `std::max(t, 0.0)` floor | **nothing on these decks** — all forcing here is above freezing. **Owed**: a sub-zero deck |
   | ix. ignore the `DRY_ELEMENT_TEMPERATURE` key entirely | 5, 6 |
   | x. use `ctx.subcatches.area` instead of `soa.area` | **nothing** — no deck here has a LID. Owed to H5b |

6. **Prior suites:** HEAT_TRANSPORT-off decks must take no new path (gate 7
   is the observer). 14/14 deck bit-identity must hold — **and note the
   standing caveat that the corpus contains no WATER_AGE or HEAT deck at
   all**, so it proves the pollutant path is undisturbed and nothing more.
7. **Record:** falsifier i (the argument of §2), falsifier iii (whether A3's
   defect is genuinely unrepresentable now or merely absent), falsifier vii,
   and whether §5(a) was in fact the failure.

## 6. Known gaps, stated rather than discovered

- **Four of ten falsifiers are predicted to escape** (iv, vi, viii, x). Each
  needs a deck this phase does not have: a returning outfall, a transient, a
  sub-zero forcing, a LID. Recording them with the reason is the A2b/A4
  precedent; do not close them by widening a band.
- Gate 2 is an **"it moved" observer**, which is lesson 61 exactly. It is
  here because the closed-form steady temperature under this forcing is not
  something I could state without running the model. **A magnitude observer
  is owed** — the recipe is a single-subarea deck held at steady depth, where
  `deltaT` can be evaluated once by hand against the ponded volume.
- `QualityRouting.cpp:154-156` carries a comment saying the 2-argument
  `resize` "defaults it to 0" — the A3 validator removed that default, so
  the comment is now false. **Pre-existing, adjacent, not touched** (§3).
- Hotstart persistence for the watershed temperature: none, matching A3's
  deferral and for the same reason (the subarea depths are not in the
  hotstart either).

## 7. Prepared commit message

```
feat(transport): temperature on subcatchment surfaces (H5a)

Per-subarea ponded temperature mirroring A3's water-age rows, with the
surface energy balance applied to ponded subareas on the runoff clock and
run-on carrying the donor's temperature.

The H5 plan line was internally inconsistent: both its verify criteria
presupposed mechanisms its scope line omitted and H6 deferred. Resolved
with the user as D-H5a/D-H5b/D-H5c and H5 split into H5a (watershed) and
H5b (LID layers + conduction).

Run-on carries a (temperature-volume, rate) PAIR rather than a numerator
alone, so the LID underdrain contributor arriving in H5b averages over
less water rather than dragging the mean toward 0 C — the defect shape A3
shipped and A4 found.

DRY_ELEMENT_TEMPERATURE HOLD|AIR|DEFAULT makes the dry-element value the
deck's choice: A4's "no water, no age" zero is wrong for a temperature.

deltaT, airTempCelsius and the unit constants move from two file-local
copies into SurfaceExchange.hpp; the watershed binding would have been a
third.
```

---

# 8. Validation result (checking agent, 2026-08-19) — COMMITTED `53b95219`

Isolated worktree at `5b2b7418`. **150/151** ctest (the bistable FV gate,
failing to the digit at `0.055224237275644343` vs `0.052534507871460516` —
identical to the A4 round's base measurement). **14/14** decks bit-identical.
**41 tests** clean under ASan/UBSan across five suites, zero diagnostics.
Zero warnings from any H5a file. Gates **10**, up from seven.

## 8.1 Five of the seven delivered gates failed on arrival — one test bug

`write_deck` hardcoded `config="_h5a.heat"` while each gate wrote its own
`_h5b.heat` … `_h5f.heat`. Gates 2–6 all ran against **gate 1's** config:
fluxes off, RAINFALL 8 °C, no policy key. That is the `8` appearing where
12, 20 and 35 were expected, and why gate 6 "accepted" a bad policy value —
the file containing it was never read. Fixed by making the config path a
parameter. §5's anticipated failure modes (a)–(d) were all wrong about the
cause; **(a) was not the problem — `[TEMPERATURE] TIMESERIES` parses fine**
(`HydrologyHandler.cpp:214-216`, `temp_source = 1`).

## 8.2 The delivered code produces NaN on an ordinary deck

The surface balance is **forward Euler with no stability limit**, and a
ponded film has almost no thermal mass per unit exchanging area. Measured on
a subcatchment fed only by run-on: a **0.52 ft³ film over 27226 ft² takes a
+862 °C step in 60 s**, the flux is re-evaluated at 182 °C, and the sequence
runs `5 → 182 → −1.8e4 → −3.9e9 → 3.9e15 → … → inf → NaN`. The NaN reaches
`subcatch_runoff_temp`, the wet-weather loader carries it to the node, and
the report prints it.

**This is not deck-specific.** The same trace fires on the *delivered,
passing* decks: S1's pervious subarea takes a **+1388 °C** excursion on its
first wet step and survives only because rain deepens it fast enough to
recover. Gate 2's "it moved" assertion was being satisfied by a number that
went to 182 °C and came back.

`deltaT`'s own contract names this hazard — "a film of water has no thermal
mass and would otherwise take an unbounded excursion in one step" — but its
guard catches only an exactly-zero heat capacity. **H2's node and link
binding has the same shape** (`SurfaceExchange.cpp:150,165` check positivity
only); it is not exposed because node and link volumes are large relative to
their surfaces. That is worth an explicit look before H5b adds LID layers,
whose volumes are smaller again.

**Fix taken, and its limit.** The step is *refused* when `|ΔT| > 5 °C`, not
clamped. Clamping to a driving temperature either freezes the surface at the
air temperature or oscillates between the clamp and a re-diverging
excursion, and both look like physics. Refusing is bit-identical for a
resolved element (a pond moves 0.01–1 °C/min against a threshold of 5), and
it under-estimates exchange on an unresolved film — bounded in the published
quantity, since the runoff temperature is volume-weighted and a thin film
carries little volume. **Integrating this properly needs a sub-stepped or
implicit scheme. That is a design decision of the same kind as D-H5a/b/c and
is left to the user rather than taken here.**

## 8.3 Falsifier sweep: 10 of 11 observed

| # | as predicted | outcome |
|---|---|---|
| i | gate 3 | **escaped as delivered.** Gate 3 rebuilt; now fails it |
| ii | gate 2 (and possibly 3) | **escaped as delivered** — nothing in the file read a node temperature. Gate 2 gained one |
| iii | gate 4 | **escaped as delivered** — see §8.4. New gate 8 fails |
| iv | *predicted to escape* | **closed** — new gate 8 |
| v | gates 2, 3 | gates 2 **and 10** |
| vi | *predicted to escape* | escapes. Still owed: needs a transient reference |
| vii | "check this one specifically, I am not confident gate 1 observes it" | **correct — it did not.** Gate 1 gained a rain-free leg; now fails it |
| viii | *predicted to escape* | **closed** — new gate 10 |
| ix | gates 5, 6 | gates 5 **and 1** |
| x | *predicted to escape* | **closed** — new gate 8, and see §8.5 |
| xi (new) | — | the §8.2 bound; gate 9 |

## 8.4 §4.1's design decision was correct and completely unguarded

The run-on divisor is the call §4.1 asked to be checked. It is right. But on
**every** deck in the delivered file the cascade is the only contributor to
`runon_inflow`, so `subcatch_runon_temp_rate` and `subcatches.runon_inflow`
hold *bit-identical* values — measured `7.68722753` for both. A3's defect was
not merely absent from those decks, it was **unrepresentable on them**, which
is exactly what let it ship in A3.

Gate 8 separates the contributors so each has a witness: S1 hosts a
bioretention underdrain returning to S1 (in `runon_inflow`, **not** in the
known rate — H5b supplies its temperature), and S2 receives the outfall
discharge carrying a 40 °C dry-weather flow. With every flux off, everything
must lie in [12, 40]. Correct code gives S1 exactly **12.0** and S2
**16.402**; falsifier iii divides a zero numerator by 0.98 cfs and hands S1
0 °C, falsifier iv drops S2 back to 12 °C exactly.

## 8.5 §4.3's rationale for `soa.area` is right for the wrong reason

The stated reason is the LID footprint. The dominant reason is **units**:
`Runoff.cpp:197` builds `soa.area` as `ctx.subcatches.area / ucf_area −
lid_area_ft2`, so the context row is in the deck's **user area units** —
acres here, **43560×** — and the footprint is the second, far smaller
correction. Substituting it was measured at 14.34 °C vs 12.95 °C with the
ponded ledger collapsing from 27342 ft³ to 0.78 ft³.

And a structural note the double-counting argument would miss: a temperature
is intensive, so **the exchange area cancels exactly against the thermal mass
inside `deltaT`** (`a_ft2 / v_old ≡ 1 / depth_prev`). The flux term cannot
see the substitution at all. It survives only through `runon_depth_rate =
runon_rate / area`. Both comments corrected in the source.

## 8.6 Gate 3's premise could not be shown by any deck

Two versions were measured before the one that works. Both subcatchments on
one gage: the receiver's own rain (10.1 cfs) outweighs the run-on (7.7 cfs)
and throughput pulls it **below** the donor, 9.62438 vs 9.62463 — the
opposite of the predicted direction. Receiver starved of rain: the sign comes
right (13.98 vs 9.62) but dropping the scatter still leaves 10.78, also above
the donor, because a slowly-fed surface sits nearer the atmospheric
equilibrium whatever its inflow temperature. **The surface balance was the
confound in both** — it warms the receiver whether or not the cascade carried
anything. The rebuilt gate turns the fluxes off and takes its contrast from a
40 °C dry-weather flow returned through an outfall, so water above 12 °C on a
rain-free receiver can only have crossed the cascade, and the failure mode
produces exactly 12.

## 8.7 Also found

- **Gate 5's "long dry tail after a short storm" rained for its whole two
  hours** — `rain_series` was always generated out to `end_min + 5`. All
  three policies read a soaking subarea, which is why they agreed. The storm
  now stops at 10 min and the gate asserts `subarea_vol_prev == 0` before
  reading a policy value, so it can never again pass on a wet surface.
- **HEAD moved twice during this round**, and my staged index reverted the
  second one. `git read-tree HEAD` ran before `6dde88b0` landed, so the first
  commit deleted its 8 files and its CMake line. The `git diff --cached
  --numstat` read `1  1` where §5.2 says to expect `1  0` — **the tell fired
  and I committed past it.** Rebuilt with `commit-tree` on the true parent
  and replaced the ref; both foreign commits verified intact, all 10
  non-CMake blobs verified byte-identical to the validated worktree.
  §5.2's check is worth stating as a hard stop, not a look.

## 8.8 Still owed

- **Falsifier vi** — applying the balance to `v_old + v_in` heats a step's
  rain before it arrived. A `dt`-order error, invisible at steady state;
  needs a transient reference. Same shape as A4's escaping falsifier iii.
- **The integrator** (§8.2). The 5 °C refusal is a bound, not a solution.
- H2's node/link binding carries the same unbounded explicit step.
- §6's own list stands otherwise: no magnitude observer for gate 2 (still an
  "it moved"), no hotstart persistence, and `QualityRouting.cpp:154-156`'s
  comment about a defaulted `resize` is still false (pre-existing, untouched).
