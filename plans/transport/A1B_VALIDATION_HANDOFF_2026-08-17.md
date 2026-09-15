# A1b Implementation — Validation & Commit Handoff (2026-08-17)

**For:** the checking agent (sandbox: `g++ -fsyntax-only` only).
**Base:** `fa9babba` (post node-store fix + its two gate commits).
**Plan:** `WATER_AGE_TRACKING_PLAN.md` §5 (LEGACY row), §7 A1 — the second
half of the A1 split. **Also owed this round:** the A1a deck UNPIN (§3.5).
**Standing findings:** lessons 1–32; especially 21 (two flips are IN this
changeset), 10 (INITIAL_STATE seeding added so no config row is inert
under LEGACY), 32 (the mirror is a store-based scheme — see §2.4).

---

## 1. Changeset (uncommitted)

```
new:  src/engine/transport/components/WaterAgeModule/WaterAgeLegacy.{hpp,cpp}
      (routeLegacyAge: single-scalar rerun of the CSTR stages —
       0. INITIAL_STATE seeds once on the first step;
       1. aging +dt (plan §1: advance then mix);
       2. accumulateLinkLoads mirror (rate convention q·age_link →
          downstream) + the loaders' node_age_vol_in rates;
       3. mixAtNodes mirror — identical volume-balance + max-principle
          clamp, using the SAME qual_vol_in denominator (runs at the END
          of execute, after the pollutant stages accumulated it); NO evap
          factor (plan §8 decision: evaporation removes volume at the
          parcel's age, mean unchanged);
       4. updateLinkQuality mirror with k = 0, no evap — STEADY/no-flow/
          zero-volume/volume-balance branches incl. the DW q_in
          correction. Writes ONLY water_age_state.)
mod:  src/engine/quality/QualityRouting.cpp
      (execute early-return gains `&& !water_age` — the pure-age LEGACY
       deck needs the volume stages; routeLegacyAge called LAST in
       execute; include)
mod:  src/engine/core/SWMMEngine.cpp
      (A1b bypass warning RETIRED — LEGACY now tracks; the
       ON+IGNORE_QUALITY warning moved ENGINE-level so it fires with or
       without a waterage component)
mod:  src/engine/transport/components/WaterAgeModule/WaterAgeComponent.cpp
      (its ignore_quality branch removed — engine-level covers it;
       WATER_AGE-OFF warning kept)
mod:  src/engine/data/WaterAgeData.hpp   (legacy_seeded flag; resize resets)
mod:  tests/unit/engine/test_water_age.cpp
      (gate 6 leg 2 FLIPPED per lesson 21: the A1b deferral warning must
       be GONE and the pure-age LEGACY deck must TRACK; three new gates:
       LegacySourceAgeShiftsEffluentExactly (21600 ± 5% under LEGACY),
       LegacyLevelPoolAgingIsExact (3600 + 120, INITIAL_STATE seeded),
       LegacyAgeLeavesPollutantsBitwise)
```

All TUs pass `g++ -std=c++20 -fsyntax-only`. Reconfigure: one new engine
`.cpp`.

## 2. Design decisions to review

1. **The mirror runs LAST in execute** and uses the pollutant stages'
   fully accumulated qual_vol_in as its denominator — one volume truth,
   no duplicated accumulation. It reads pollutant-side state and writes
   only water_age_state (the bitwise gate).
2. **No evaporation factor for age** (both node and link mirrors): plan
   §8's proposal adopted — evaporation removes volume at the parcel's
   current age, so the volume-intensive MEAN is unchanged. Legacy's
   pollutant path concentrates; age deliberately does not. Flag if you
   want the open item kept open instead.
3. **INITIAL_STATE seeds on the mirror's first step** (ARD seeds at its
   own init) — added so a configured INITIAL_STATE is never inert under
   LEGACY (lesson 10).
4. **Store-scheme ordering (lesson 32):** the mirror inherits legacy's
   own sequencing — node mixing reads this step's accumulated inflows
   BEFORE links draw the node age (updateLinkQuality reads
   `ws.node_age[un]` post-mixing), i.e. legacy's mix-then-discharge.
   The ARD defect's shape does not recur here by construction, but probe
   it: the LEGACY source-shift gate at several ROUTING_STEPs is the
   step-invariance instrument (§3.2b).
5. **STEADY link age = upstream node age** (mirror of findSFLinkQual with
   k = 0). The steady exact-exponential subtlety vanishes for age.

## 3. Validation protocol

1. Reconfigure, build, zero new warnings.
2. `ctest -R test_engine_water_age` — 10 gates (7 A1a + 3 new, with the
   flipped leg).
   *Anticipated failure modes, likelihood order:*
   (a) **LEGACY level pool** — the CSTR path's behavior on the
   FIXED-stage zero-flow deck is less exercised than ARD's; if ages
   drift beyond 0.1%, measure the actual qual_vol_in at the level-pool
   nodes first (a nonzero v_in with equal ages is still the identity —
   the clamp protects it — so only a VOLUME anomaly can perturb).
   (b) **LEGACY source-shift equilibration** — same flush physics as
   gate 2; run the step-invariance probe (rs 1/5/20: the shift must be
   21600 at each — this is ALSO the lesson-32 probe for the mirror's
   ordering).
   (c) **Gate 6 flip** — if the A1b warning needle still matches,
   check for a second emission site before blaming the test.
3. **Falsifier sweep** (verified restoration):

   | falsifier | expected failing gates |
   |---|---|
   | i. comment the routeLegacyAge call | flipped leg (age stays 0), both new analytic gates |
   | ii. comment the aging (+dt) stage | LEGACY level pool (stays 3600); source shift unaffected (shift is age-additive) |
   | iii. comment the link-load accumulation (stage 2 link loop) | LEGACY source shift (aged inflow never propagates) |
   | iv. re-add the evap factor in the node mirror | LEGACY level pool IF the deck evaporates (likely no evap on this deck — if green, record the row as unobserved and either add an evap deck or note the §8 decision is gated only by review) |
   | v. comment the INITIAL_STATE seeding | LEGACY level pool (120 vs 3720) |
   | vi. drop the execute-guard water_age term | flipped leg (pure-age LEGACY returns early, ages stay 0) |
   | vii. restore the A1b warning | gate 6 flipped leg |
4. **Prior suites all green**; bit-identity: WATER_AGE-off decks
   untouched (the execute changes are guarded); the LEGACY bitwise gate
   is the WATER_AGE-on assurance. Sanitizers over the suite.
5. **THE OWED UNPIN (lesson 21 carry):** the A1a decks still write
   ROUTING_STEP 1 with a comment describing the node-store defect as
   LIVE. In this round: switch the flowing decks to an ordinary
   ROUTING_STEP (5), re-verify the analytic bands (post-`7b2dfaae` the
   21600-s shift should be step-invariant — measure at 1 and 5 and
   record), and rewrite the comment to name the FIX (`7b2dfaae`) instead
   of the defect. If any band moves at rs = 5, that is NEW information
   about the fix — record before adjusting.
6. **Cross-engine check (plan A5 preview, optional):** same flowing
   deck, LEGACY vs EULERIAN_ARD steady effluent age — record the
   difference as the first G-UT2-style data point (no gate yet; scheme
   tolerance unmeasured).
7. Append results to §5; commit with §4.

## 4. Commit message

```
feat(transport): LEGACY water-age mirror (A1b)

QUALITY_SOLVER LEGACY now tracks water age: routeLegacyAge runs a
single-scalar rerun of the CSTR stages at the end of
QualitySolver::execute - aging +dt, link-load accumulation (q*age to the
downstream node) plus the loaders' per-source age-volume rates, the
mixAtNodes volume-balance mix with the max-principle clamp on the SAME
qual_vol_in denominator, and the updateLinkQuality branches with k = 0.
No evaporation factor for age (plan section-8 decision: evaporation
removes volume at the parcel's age, the mean is unchanged).
INITIAL_STATE seeds on the mirror's first step so no configured row is
inert under LEGACY. The mirror writes only water_age_state - pollutant
trajectories are bit-identical with WATER_AGE ON (gated). The A1b
deferral warning is retired and its gate flipped in this changeset; the
ON+IGNORE_QUALITY warning moved engine-level. The execute early-return
admits pure-age LEGACY decks (np == 0). Gates: test_water_age.cpp
(10: +LEGACY exact 6-h source shift, exact seeded level-pool aging,
LEGACY bitwise razor). This round also owes the A1a ROUTING_STEP unpin
(handoff section 3.5).

Plan: WATER_AGE_TRACKING_PLAN.md section 5/7 A1 (A1b half).
Validation record: plans/transport/A1B_VALIDATION_HANDOFF_2026-08-17.md
```

## 5. Validation results

*(appended by the checking agent)*

### 5.0 Outcome

**Committed.** All 10 delivered gates passed on arrival, including the
flipped leg. Validation's contribution was not a defect in the mirror —
which is faithful — but the discovery that **the instrument §2.4 nominated
to guard against the ARD store defect cannot see it** (§5.2), plus the
owed unpin, one prediction corrected, and the measured cross-engine data
point the plan asked for.

Final: **14/14** gates (10 delivered + 4 added), suite **139/140** (only
the known pre-existing `FvEngine.RefiningTheMeshConvergesTowardTheDynwave-
Hydrograph`), bit-identity **14/14 `.out`** and 14/14 `.rpt` in content,
ASan/UBSan **0 findings across 56 tests**. Falsifier sweep 10/10 caught —
after adding the gate that catches the one that wasn't.

### 5.1 The mirror is faithful

Read line by line against the pollutant path: `accumulateLinkLoads` (rate
convention `q·value` to the downstream node), `mixAtNodes`
(`(c_old·v_old + mass_in)/(v_old + v_in)` with the `c_max` clamp, same
`qual_vol_in` denominator), `updateLinkQuality` (STEADY / no-flow /
zero-volume / volume-balance with the DW `q_in` correction). The deliberate
differences — `k = 0`, no evaporation factor — are the two the header
claims. The `c_old` analogue is `ws.node_age + dt`, which is the aging-then-
mixing the design describes. Running last in `execute` means `qual_vol_in`
is fully accumulated, and `assembleExternalLoads` has already zeroed and
refilled `node_age_vol_in`, so the mirror reads one volume truth.

### 5.2 The nominated instrument is blind to the thing it was nominated for

§2.4 says the ARD store's defect "does not recur here by construction, but
probe it: the LEGACY source-shift gate at several ROUTING_STEPs is the
step-invariance instrument". I built exactly that (gate 11, the shift at
rs 1/5/20) — and then wrote the defect into the mirror to check the
instrument. Falsifier ix takes the link stage's upstream age from the
**pre-mixing** node value instead of the post-mixing one, which is the ARD
node store's failure transplanted.

**Nothing failed. Not the delivered ten, not the step-invariance gate.**

The reason is the property that makes the shift a good invariant elsewhere:
it is a *difference of two runs*, so any bias common to both cancels — and
the ordering error is exactly such a bias. The gate is blind to the ordering
**because** it is step-invariant, not despite it.

What does separate the orderings is the slope of the ABSOLUTE bias. Measured
on the 5-conduit chain, outfall age minus V/Q:

| ROUTING_STEP | 1 | 2 | 5 | 10 | 20 |
|---|---|---|---|---|---|
| correct (post-mix donor) | +4.504 | +9.504 | +24.504 | +49.504 | +99.503 |
| falsifier ix (pre-mix) | +9.504 | +19.504 | +49.504 | +99.503 | +199.500 |

Exactly `5·dt − 0.5` against exactly `10·dt − 0.5`: **one routing step of
splitting bias per element crossed, versus two.** Gate 13
(`LegacySplittingBiasIsOneStepPerElement`) measures that slope at two
routing steps and bounds it at 1.5 dt/element; it is falsifier ix's only
observer.

### 5.3 The mirror is exact in the limit — and ARD is not

The `5·dt − 0.5` law extrapolates to **zero**: at dt → 0 the LEGACY mirror
reproduces the residence-time theorem (mean age of water leaving a steady
system = V/Q) exactly, and everything above it is a clean O(dt)
operator-splitting error from age-then-mix. That is the strongest
correctness statement available for this scheme and no gate asserted it, so
gate 12 (`LegacyOutfallAgeConvergesToResidenceTime`) now does.

Running the same measurement on ARD gives `+20.939 + 1·dt`: a **residual of
+20.9 s (2.25% of V/Q) that does NOT vanish as dt → 0**. Recorded as an open
item — it is small, it is not this changeset's, and chasing it belongs with
plan A5.

### 5.4 Cross-engine (plan A5 / G-UT2 preview) — and a comparison to avoid

| ROUTING_STEP | 1 | 5 | 20 |
|---|---|---|---|
| LEGACY C5 (link) | 933.796 | 949.788 | 1009.747 |
| ARD C5 (link) | 876.443 | 876.447 | 876.449 |
| apparent gap | 6.5% | 8.4% | 15.2% |
| LEGACY outfall − V/Q | +4.504 | +24.504 | +99.503 |
| ARD outfall − V/Q | +21.939 | +25.939 | +40.939 |

**The 6.5–15.2% link gap is mostly not an error in either engine.** A LINK's
published age is a different quantity per scheme: LEGACY's fully-mixed tank
publishes its OUTLET value, ARD the volume-weighted MEAN over the link's
cells. The outfall NODE is the common quantity, and there the two agree far
better (+4.5 vs +21.9 s at rs 1). Any future G-UT2 tolerance should be
written against the outfall, or it will be measuring a definition rather
than a discrepancy.

Also note ARD's absolute age is step-invariant while LEGACY's moves +19%
from rs 1 to 60 — the `5·dt` splitting bias. Users comparing LEGACY age
across routing steps will see that; it is inherent to age-then-mix, and a
Strang split (dt/2, mix, dt/2) would reduce it to O(dt²). Not done here:
§2's ordering is the plan's vetted choice and changing it is a design
decision, not a validation fix. Flagged with the measurement so the call can
be made deliberately.

### 5.5 Falsifier sweep — 10 cases

| # | falsification | gates that failed | predicted |
|---|---|---|---|
| i | comment the `routeLegacyAge` call | 6, 8, 9, 11, 12 | ✓ |
| ii | comment the aging (+dt) stage | 6, 9, 12 | ✓ (shift unaffected, as predicted) |
| iii | comment the link-load accumulation | 8, 11, 12 | ✓ |
| iv | re-add the evap factor | 11, 12 | **predicted unobservable — it is observable** |
| v | comment the INITIAL_STATE seeding | 9 | ✓ |
| vi | drop the execute-guard water_age term | 6, 8, 9, 11, 12 | ✓ |
| vii | restore the A1b warning | 6 | ✓ |
| viii | drop the loaders' `node_age_vol_in` term | 8, 11 | *added* |
| ix | pre-mix donor in the link stage | **13 only** | *added* — see §5.2 |
| x | drop the moved IGNORE_QUALITY warning | 14 | *added* |

**Falsifier iv corrected.** §3 predicted this row would come back green
because the gate deck does not evaporate, and asked for it to be recorded as
unobserved. It fails two gates. The reason matters for the §2.2 decision:
legacy's factor is `if (v_new > 0 && v_new < v_old + v_in) c *= (v_old +
v_in)/v_new`, and at steady state **every flowing node satisfies that** —
`v_new ≈ v_old` and `v_in > 0`. It is not an evaporation-only factor; it
fires on ordinary outflow, and on the pollutant side the `c_max` clamp
absorbs it. For age it pins the mix at `a_max = a_old + dt` so a node never
mixes down toward younger inflow. Omitting it is right, for a stronger
reason than "evaporation leaves the mean unchanged".

**Falsifier viii added** because iii falsifies only the LINK half of the
accumulation; the per-source loader rates reach the same accumulator by a
different line and had no dedicated observer.

**Falsifier x added** because A1b MOVED the ON+IGNORE_QUALITY warning from
the component's apply hook to engine-level, and nothing observed it in
either home. The move is right — WATER_AGE ON needs no component — but a
warning that changes homes is exactly when one goes missing. Gate 14 uses a
deck with **no** waterage component, which is the case the old site could
not reach.

### 5.6 The owed unpin (§3 item 5) — done

The A1a decks pinned `ROUTING_STEP 1` with a comment calling the ARD store
defect live. It was fixed in `7b2dfaae`, so the pin is retired to an
ordinary `ROUTING_STEP 5` and the comment now names the fix. Re-measured
across rs 1/2/5/10/20/60 before touching the decks: the ARD shift is
21599.998 → 21599.9999 and the level pool is 3720.0000 at every step, so no
band depended on the pin. All gates stayed green through the change with no
band adjustments.

### 5.7 Prior suites, bit-identity, sanitizers

- `ctest` **139/140**; the single failure is the known pre-existing
  `FvEngine.RefiningTheMeshConvergesTowardTheDynwaveHydrograph`.
- Bit-identity vs `fa9babba` over the 14 E0/E2 decks: **14/14 `.out`
  bit-identical**; 14/14 `.rpt` identical in content. Raw `cmp` flags all
  14 because every report carries `Analysis begun/ended on` wall-clock
  stamps, and 3 differ further only in `Total elapsed time` — no result
  line moves. Expected: every A1b branch is gated on
  `ctx.options.water_age` and `routeLegacyAge` early-returns on it.
- ASan+UBSan **0 findings across 56 tests**: water_age (14),
  ard_node_store (4), ard_dispersion (11), ard_e5b (7),
  ard_transport_bcs (10), reaction_legacy_binding (10). The ASan tree
  needed an explicit reconfigure for the new `.cpp` — the engine CMake
  GLOBs without `CONFIGURE_DEPENDS`, and the first run failed with
  `exit=134` and `findings=0`, which reads like a sanitizer abort but was
  a stale binary.

### 5.8 Open items

- **ARD's +20.9 s (2.25% of V/Q) outfall-age residual that survives
  dt → 0** (§5.3). New, small, not this changeset's; belongs with plan A5.
- The LEGACY `5·dt` splitting bias (§5.4) — inherent to the plan's
  age-then-mix ordering; a Strang split would make it O(dt²). Design call,
  not a defect.
- Carried from A1a and untouched here: `transported_count()` has no
  callers; the registry records `__WATER_AGE__` units as `"hours"` while
  the state publishes SECONDS; `reset()` still does not clear
  `ctx.reactions`.
- Artifacts: `tests/output/a1b_validation_2026-08-17/` — `a1b_probe.cpp`
  and its logs (including the falsified-ix run), `falsifiers.sh` + per-case
  logs, `run_decks.sh`, `bit_identity.log`, `asan_run.log`,
  `ctest_full.log`.
