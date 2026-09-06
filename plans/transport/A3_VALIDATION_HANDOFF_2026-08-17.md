# A3 Implementation — Validation & Commit Handoff (2026-08-17)

**For:** the checking agent.
**Base:** `8b5b3ef5`.
**Plan:** `WATER_AGE_TRACKING_PLAN.md` §3, §7 A3. Scoping:
`A3_SCOPING_2026-08-17.md` (read §2 — it lists three findings that changed
the shape of this phase).
**Standing findings:** lessons 1–63.
**User decisions taken 2026-08-17:** per-SUBAREA age (not lumped); run-on
CARRIES age; hotstart persistence DEFERRED.

---

## 1. What this delivers

Each subcatchment carries **three** ages, one per ponded subarea
(`IMPERV0`, `IMPERV1`, `PERV`), mirroring the RunoffSolver's own
`depth_imperv0/1/perv`. Per step: age +dt, then mix volume-weighted with what
arrived. Runoff leaves at the volume-weighted mean of the subareas that hold
water, and **that** age reaches the outlet node and travels as run-on.

A2b's placeholder — the `.out` subcatchment age column pinned at 0 — is
retired in this changeset (lesson 21).

## 2. The gap this closes, which is a defect not a feature

Before A3 the **flow** path added `q_runon` (`SWMMEngine.cpp:~1948`) while
the **age** path added nothing: `addWetWeatherLoads` skips any subcatchment
whose outlet is another subcatchment (`outlet_node < 0`). So a
two-subcatchment cascade — the plan's own A3 verify criterion — delivered
water to the node with no upstream age attached.

**Fifth appearance of the flow-knows/quality-doesn't family** (R4, E5a, A1a,
H1, now A3). `RunonCarriesTheDonorsAge` is the gate that fails on `8b5b3ef5`.

## 3. Changeset (uncommitted)

```
mod:  src/engine/data/WaterAgeData.hpp
      (SubArea enum; subarea_age, subarea_vol_prev, subcatch_runoff_age,
       subcatch_runon_age_vol_in; resize gains n_subcatch)
new:  src/engine/transport/components/WaterAgeModule/WaterAgeWatershed.{hpp,cpp}
mod:  src/engine/core/SWMMEngine.cpp        (call after runoff_.execute;
      run-on age beside the flow scatter; A2b placeholder retired)
mod:  src/engine/quality/QualityRouting.cpp (wet-weather loader delivers the
      SUBCATCHMENT's computed age, not the configured RAINFALL age)
mod:  src/engine/transport/components/WaterAgeModule/WaterAgeLegacy.cpp
mod:  src/engine/transport/components/EulerianArdComponent/ArdEngine.cpp
mod:  src/engine/core/HotStartManager.cpp   (2 sites)
      — the last three are the resize-signature sweep, see §4.3
new:  tests/unit/engine/test_water_age_watershed.cpp  (4 gates)
mod:  tests/unit/engine/CMakeLists.txt      (+1 target — shared file)
```

**Also in the tree from H4's round:** the `SEDIMENT_EXCHANGE` H4→H6
phase-name fix (`HeatComponent.cpp` + two test comments).

All touched TUs pass `g++ -std=c++20 -fsyntax-only`.

## 4. Design decisions to review

### 4.1 The mixing volume is a NET estimate — the call most worth challenging

The RunoffSolver does not expose per-subarea inflow and outflow separately,
only the depth before and after. So the volume that mixed in is
`v_in = max(0, v_new − v_old)`.

**When a subarea is simultaneously receiving rain and shedding runoff — the
ordinary case during a storm — this under-counts the mixing and biases the
age OLD.** It is exact when a subarea is only filling or only draining.

Closing it properly means the runoff solver publishing its per-subarea
inflow and outflow, which is a change to *hydrology*, not transport. **I
judged that out of scope for A3 and recorded it rather than smuggling it
in.** Flag if you would rather A3 wait for that plumbing — the alternative
is a phase that touches `processSubarea`'s signature.

### 4.2 Runoff reaches the node at the SUBCATCHMENT's age, not RAINFALL's

`addWetWeatherLoads` previously used the configured `RAINFALL` source age.
That is the age water arrives from the sky with; by the time it leaves the
surface it has aged and mixed. The loader now uses
`subcatch_runoff_age[i]`, **falling back to the configured age when the
watershed state is unsized** (WATER_AGE on but runoff never stepped).

### 4.3 `resize` gained a defaulted third parameter — and I swept every caller

`WaterAgeState::resize(n_nodes, n_links, n_subcatch = 0)`. **The default is
a trap**: four existing two-argument call sites would have silently emptied
`subarea_age` mid-run. All four now pass `ctx.n_subcatches()` —
`QualityRouting.cpp`, `WaterAgeLegacy.cpp`, `ArdEngine.cpp`, and two in
`HotStartManager.cpp`. Gate 4 is the observer.

This is lesson 14's shape (a signature change sweeping a neighbour), and
**it is the defect I would most expect to have missed one of** — please run
the grep in §5.1 rather than trusting the list.

### 4.4 Run-on age is a per-step RATE, zeroed after consumption

`subcatch_runon_age_vol_in` follows the `node_age_vol_in` convention
exactly: donors accumulate `q · age`, the consumer divides by the flow rate,
and the array is cleared at the end of the update.

### 4.5 Hotstart deferred, per your decision

No persistence, and none is possible today: the subarea depths themselves
are not in the hotstart (`A3_SCOPING §2.3`), so a restored age would be a
mean over a volume that was not restored. **Owed row, not a silent gap.**

## 5. Validation protocol

1. **Run these greps first.**
   - `grep -rn "water_age_state.resize\|ws\.resize(" src/engine/` — every
     hit must pass three arguments (§4.3). This is the one I most expect to
     be incomplete.
   - `grep -rn "WaterAgeSource::RAINFALL" src/engine/` — any remaining
     consumer should be deliberate.
2. Build, zero new warnings. `ctest -R test_engine_water_age` (both suites —
   the existing 16 must be unchanged) then the full suite.
   *Anticipated failure modes, likelihood order:*
   (a) **The rainfall deck may produce no runoff.** Every gate asserts its
   setup first (`stat_runoff_vol > 0`, `outlet_subcatch[0] == 1`), so a dead
   deck fails loudly rather than passing vacuously — lesson 59. If setup
   fails, fix the deck (rain intensity, Horton parameters, subarea widths),
   not the assertion.
   (b) **Gate 3 needs water still ponded at the end.** If `den == 0` it says
   so; lengthen the storm rather than loosening the comparison.
   (c) **Gate 2's inequality is directional, not a magnitude** — see §6.
3. **Falsifier sweep:**

   | falsifier | expected failing gates |
   |---|---|
   | i. drop `addRunonAge` from the run-on scatter | **2** — the defect gate. If it fails nothing, the run-on claim is unobserved |
   | ii. revert the loader to `addAgeVolume(…RAINFALL)` | 2 (S2 sheds at exactly the rain age) |
   | iii. weight the runoff age by subarea FRACTION instead of volume | 3 |
   | iv. drop the `+dt` aging term | 1's `any_aged` leg |
   | v. call any one `resize` with two arguments | 4 |
   | vi. use `v_new` instead of `max(0, v_new − v_old)` as `v_in` | **probably nothing** — flagged in advance. It would over-mix and bias young. If no gate catches it, §4.1's approximation is unobserved in BOTH directions and that is worth an owed gate |
   | vii. leave the A2b placeholder (age column 0) | **nothing here** — the `.out` column is not gated in this file. **Owed**: an output gate reading the subcatchment age column BY NAME (lesson 40) |
4. **Prior suites:** WATER_AGE-off decks take no new path;
   `test_engine_water_age` (16) and the heat suites must be unchanged, and
   14/14 deck `.out` bit-identity must hold against `8b5b3ef5` **except**
   where a deck has subcatchments AND water age — those now report a real
   subcatchment age instead of 0. **Report which decks move and by how
   much**; a change there is correct, but it should be seen.
5. **Record:** falsifier i (the whole argument of §2), falsifier vi (§4.1's
   observability), and whether §5.1's grep found a missed resize site.

## 6. Known gaps

- **Gate 2 is directional only.** It asserts S2's runoff is older than the
  rain, not that it equals a computed mix. A magnitude gate needs the
  cascade's volume split, which the net-estimate of §4.1 makes approximate
  anyway. **Lesson 61 says this is not enough** — recorded as owed rather
  than claimed.
- **No `.out` gate** for the retired placeholder (falsifier vii).
- **Snow** reaches runoff through `snow_net_imperv/perv`, so melt water is
  treated as rain-aged. Plan §8's `WATER_AGE_SNOW` question (does the pack
  hold age or pass it through?) is **untouched and undeferred** — no error
  names it. That is a gap in the deferral discipline, not just in features.
- Hotstart (§4.5), and the pre-existing owed gates from H2/H4.

## 7. Commit message

```
feat(transport): water age on subcatchment surfaces (A3)

Each subcatchment carries three ages, one per ponded subarea (IMPERV0,
IMPERV1, PERV), mirroring the RunoffSolver's own depth_imperv0/1/perv - per
subarea rather than lumped, because impervious water is systematically
younger than pervious and the depths already exist separately. Per step the
age advances by dt and then mixes volume-weighted with what arrived; runoff
leaves at the volume-weighted mean of the subareas holding water, and that is
what reaches the outlet node.

This closes a defect, not just a gap: run-on water carried NO age. The flow
path adds q_runon while addWetWeatherLoads skips any subcatchment whose
outlet is another subcatchment, so a two-subcatchment cascade - the plan's
own A3 criterion - delivered water with no upstream age attached. Fifth
appearance of the flow-knows/quality-doesn't family.

The mixing volume is estimated as the NET gain, because the runoff solver
does not publish per-subarea inflow and outflow separately. That under-counts
mixing when a subarea fills and sheds at once and biases the age old; closing
it means changing hydrology, not transport, and is recorded rather than
smuggled in.

WaterAgeState::resize gained a third parameter defaulting to zero, which
would have silently emptied the new arrays at four existing call sites - all
swept, with a gate that watches for a fifth. A2b's placeholder subcatchment
age column is retired here. Hotstart persistence is deferred by decision: the
subarea depths are not persisted either, so a restored age would be a mean
over a volume that was not restored.

Gates: tests/unit/engine/test_water_age_watershed.cpp - per-subarea state
sized and aging, run-on carrying the donor's age (fails before this change),
volume- not area-weighting, and the resize sweep.

Plan: WATER_AGE_TRACKING_PLAN.md section 3 / 7 A3.
Scoping: plans/transport/A3_SCOPING_2026-08-17.md
Validation record: plans/transport/A3_VALIDATION_HANDOFF_2026-08-17.md
```

## 8. Validation results

*(appended by the checking agent, 2026-08-18)*

Validated in an isolated worktree at `8b5b3ef5` carrying only the manifest.
Artefacts: `tests/output/a3_validation_2026-08-18/`.

### 8.1 Outcome

| check | result |
|---|---|
| §5.1 greps | **complete** — six `resize` sites (five pre-existing, not four), all three-argument; the only other `WaterAgeSource::RAINFALL` consumer is the LID drain path, deliberately deferred to A4 in its own comment |
| build | clean, zero new warnings |
| `test_engine_water_age*` | **23/23** — the existing 16 unchanged, A3's **7** (4 delivered; see §8.4) |
| full `ctest` | **147/148** — the one failure is the known bistable `FvEngine.RefiningTheMeshConvergesTowardTheDynwaveHydrograph`, which fails at base |
| deck `.out` bit-identity vs `8b5b3ef5` | **14/14**, and §4's "report which decks move" has a **null answer for a reason worth naming** — see §8.6 |
| ASan + UBSan | clean on every A3 path. Two suites abort on a **pre-existing** misaligned `uint32_t` CRC load at `HotStartManager.cpp:246`, which A3 does not touch (its hunks are at :588 and :610); with those tests filtered out, `test_engine_water_age` is 14/14 with zero findings |
| falsifiers | 12 run (your 7 + 5 of mine). **As delivered: 4 of 12 observed.** After the work below: **8 of 12**, and three of the four survivors are provably not defects |

### 8.2 §4.1 is not an approximation — it is the answer

**This is the finding of the round, and it changes the recommendation you
asked for.**

The net-gain mixing volume `max(0, v_new − v_old)` is zero exactly when a
subarea sheds as fast as it fills, which is the ordinary state of an
impervious surface during a storm. So no rain is ever admitted, and the age
stops being a residence time and becomes *the elapsed time since the surface
first wetted*.

Measured on a 100 % impervious, zero-depression-storage deck under sustained
2 in/h rain, where the surface is a complete-mix tank at steady state and the
mean age of the water leaving is exactly `V/Q`:

| | age of shed water |
|---|---|
| analytic `V/Q` (V = 5104.5 ft³, Q = 10.08 cfs) | **0.14062 h** |
| delivered, net-gain mixing volume | **0.88592 h** — 6.3x too old |
| gross-inflow mixing volume | **0.14022 h** — 0.3 % |

**And the deferral's premise does not hold.** §4.1 says closing this needs
the runoff solver to publish per-subarea inflow *and* outflow, which would be
a hydrology change. Only the INFLOW is needed: complete-mix means outflow
leaves at the subarea's own age, so it never enters the update. And the
inflow is already published — `ctx.subcatches.rainfall` (ft/s, written by
`Runoff.cpp:295`) plus run-on, which the solver spreads over the whole area
as extra precipitation (`Runoff.cpp:331-333`). That is precisely the
expression `processSubarea` is handed. So

```
v_in = (rainfall + runon/area) · frac[k] · area · dt
```

is exact for a `RouteTo OUTLET`, snow-free deck, needs nothing from
hydrology, and is what plan §3 asks for in the first place ("volume-mixed
with incoming rainfall"). Implemented, and gate 5 now holds it to the
analytic residence time.

What genuinely remains approximate is narrower and is now stated in the
header: with a snowpack the solver substitutes `snow_net_imperv/perv` per
subarea, and `RouteTo IMPERV`/`PERV` moves water between subareas without
appearing in either term.

**A second, undocumented decision was folded into the same expression.** The
delivered arriving-age rule is `a_in = has_runon ? runon_age : a_rain` —
whenever *any* run-on exists, the entire arrival is stamped at the donor's
age and rain is discarded. §4 does not mention this, which reads as oversight
rather than choice. On the delivered gate deck rain outweighed run-on
**366:1** and was being thrown away. Now flow-weighted between the two.

### 8.3 The gate deck rained for 5 of its 60 minutes

`RG1 INTENSITY 0:05` reads one value per five-minute interval, and the series
carried entries only at 00:00 and 01:00 — so the storm was **five minutes**
followed by a 55-minute recession. `stat_runoff_vol` was 1104 ft³ against
36 300 ft³ of nominal rain, which is what gave it away.

That is why §4.1's defect was invisible: **the mixing term only acts while
water is arriving, so every gate was watching a draining surface — the one
regime in which the net-gain estimate is exact.** The deck now carries one
entry per interval. I nearly filed the 6.3x above as 97x from the original
deck before checking `stat_runoff_vol`; the figures in §8.2 are from the
corrected deck.

### 8.4 Gate changes

**Gate 2 gains a NODE leg.** Every delivered assertion reads
`ctx.water_age_state` directly, so reverting the loader to the configured
RAINFALL age (falsifier ii) left all four gates green while handing the
network 3 h water — §4.2 was entirely unobserved. The leg compares J1's age
against S2's shed age (S2 is its only contributor): they agree to 0.0004 h,
and falsifier ii separates them by 0.26 h.

**Gate 5 — `ShedAgeIsTheResidenceTimeUnderSteadyRain`.** The magnitude gate
§8.2 needed. Both `V` and `Q` come from the run, so the reference is computed
rather than fitted, and the assertion is on the ratio. `PctZero 50` splits
the impervious area in two so `frac[k]` is 0.5 rather than 1 — at 100 % a
mixing volume that forgets the subarea fraction is inert (falsifier xi).

**Gate 6 — `CascadeShedAgeSitsInsideItsAnalyticBracket`**, the plan's own A3
criterion. Two physical bounds, no fitted tolerance: S2 cannot shed younger
than the rain, nor older than S1's shed age plus one full turnover of S2's
own surface (`V/Q`).

**Gate 7 — `SubcatchmentAgeReachesTheOutByName`.** Closes falsifier vii,
which the handoff recorded as owed. Reads the column by NAME via
`swmm_output_get_pollut_id` (lesson 40).

**Gate 4 is unfalsifiable, and the trap was removed instead.** §4.3's hazard
is real but no observer can see it: every one of the five sites is guarded by
a size mismatch, so a wipe can only land before any age exists, and
`routeSubcatchmentAge` re-sizes at the top of its next call. I reverted each
site in turn — including `ArdEngine::init`, the only one that runs *after* a
runoff step — and the published ages came back **bit-identical**. So the
`= 0` default is gone: a two-argument call is now a compile error, which is
the only observer that can catch a defect that repairs itself at runtime. All
five callers already pass three arguments and there are none outside
`src/engine`, so this costs nothing. Gate 4 stays as a cheap invariant,
relabelled as such rather than counted as coverage.

### 8.5 The falsifier sweep

| falsifier | as delivered | after |
|---|---|---|
| i. drop `addRunonAge` | gate 2 ✔ | gates 2, 6 |
| ii. loader back to `RAINFALL` | **nothing** | gate 2's node leg |
| iii. weight by FRACTION | gate 3 ✔ | gate 3 |
| iv. drop `+dt` | gates 1, 2 ✔ | gates 1, 2, 5, 6 |
| v. a `resize` that wipes | **nothing** | nothing — §8.4 |
| v-b. *(mine)* the same at `ArdEngine::init` | — | nothing — §8.4 |
| vi. the net-gain mixing volume | n/a | **gate 5** |
| vii. keep the A2b placeholder | **nothing** | **gate 7** |
| viii. *(mine)* drop the dry-subarea guard | nothing | nothing — the guard is arithmetically redundant: a zero-volume subarea adds `a·0` to the numerator and `0` to the denominator either way. §3's real claim is area-vs-volume, which is falsifier iii |
| ix. *(mine)* run-on carries the RECEIVER's age | — | nothing — see below |
| x. *(mine)* the delivered arriving-age rule | — | nothing — see below |
| xi. *(mine)* `v_in` without `frac[k]` | — | gate 5 |

**§5's requested records.** Falsifier i, the whole argument of §2, is
observed — and by a large margin: S2 sheds at **1.597 h against 3 h of rain**
when the run-on accumulator is never filled, because the arrival is then
stamped age 0. Falsifier vi (the delivered mixing volume) is caught only by
the new gate 5; nothing in the delivered suite could see it. §5.1's grep found
no missed resize site.

**ix and x escape, and I did not manufacture a gate for them.** Measured on
the cascade deck against a bracket upper bound of 3.4008 h: clean 3.2564 h,
falsifier ix 3.2878 h, falsifier x 3.3458 h. An *exact* flow-weighted arrival
bound would sit at 3.2911 h — enough for x, still not for ix. Catching a
donor/receiver swap needs a deck where the two ages differ far more than 0.06
h, not a tighter bound on this one. **Owed, with the numbers.** Note this also
means the arriving-age fix in §8.2 was made on physics grounds, not because a
gate demanded it.

### 8.6 The bit-identity corpus cannot see this phase

14/14 identical, and §4 asked which decks move: **none, because not one of
the 14 enables `WATER_AGE`.** So the corpus proves the changeset is inert on
water-age-off decks — worth having — and is structurally incapable of showing
the `.out` placeholder retirement. That, plus falsifier vii failing nothing,
is why gate 7 exists.

### 8.7 Two stale comments

`postOutputSnapshot`'s comment still promised the age column "stays 0 here",
directly above the code that now fills it. Updated. (H4's round found the same
shape in a test *name*; this is the third phase running where the retiring
changeset left its own explanation behind.)

### 8.8 Still owed

* A magnitude gate that can see a donor/receiver mix-up in `addRunonAge`
  (§8.5), which needs a cascade with a much larger age contrast.
* Snow: plan §8's `WATER_AGE_SNOW` question remains **undeferred** — no error
  names it, which is a gap in the deferral discipline as your §6 says.
  Inter-subarea `RouteTo IMPERV`/`PERV` is in the same position.
* Runoff leaves at the **stored-volume** weighted mean of the subareas, not
  their outflow-weighted mean; per-subarea outflow genuinely is not
  published, so a depression-storage subarea counts toward the departing age
  in proportion to what it holds rather than what it sheds. Now stated in the
  header.
* Hotstart persistence (§4.5), and the pre-existing UB at
  `HotStartManager.cpp:246`.

### 8.9 Commit

`b5be8ec3` — 10 files, staged through a temp index and verified byte-identical
to the validated worktree.
