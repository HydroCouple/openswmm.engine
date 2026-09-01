# H2 Implementation — Validation & Commit Handoff (2026-08-17)

**For:** the checking agent.
**Base:** `4767aabb`.
**Plan:** `HEAT_TRANSPORT_PLAN.md` §2.1 (SurfaceExchange), §2.4 (met
forcing), §6 H2.
**Standing findings:** lessons 1–54.

---

## 1. What this delivers

Latent (`Je`) and sensible (`Jc`) heat exchange at the free water surface,
the first terms that make temperature *change* rather than merely move, plus
the relative-humidity met input they need. `[HEAT_FLUXES] SURFACE_EXCHANGE
ON` in `model.heat` enables it; **default OFF**, so an H1 deck is unchanged.

This is also where **ρw·cp become load-bearing** — H1 shipped without them
deliberately because with no fluxes they cancelled and their value was
unfalsifiable. Gate 4 is the observer that could not exist until now.

## 2. The question H2 was scoped to answer, and its answer

Applying a W/m² flux needs a water-surface area per element, and the open
question was whether one exists that is not solver-dependent. It does, and
it is not new: **heat exchanges exactly where EVAPORATION already does.**

| surface | area expression | engine precedent |
|---|---|---|
| storage node | `node::getSurfArea(nodes, i, depth, tables, unit_sys, subs)` | `Routing.cpp:490-497` |
| open conduit | `xsect::getWofY(xs, depth) · length · barrels` | `Routing.cpp:582-597`, `DynamicWave.cpp:2023-2029` |
| junction / outfall / divider | **none** — `getSurfArea` returns 0 for them | `Node.cpp:324-326` |
| closed conduit | **none** — gated by `xsect::isOpen` | `Routing.cpp:582` |

Both live in `Router::initNodeFlows` / `computeConduitLosses`, which run
under **every** routing model, so the module is not quietly dead under
STEADY or KINWAVE — the specific failure I was worried about, and the
fourth-appearance family of lesson 52.

Physically this is also the right answer rather than a convenient one: a
manhole is closed and does not exchange with the atmosphere, which is
exactly why legacy gives it no evaporation either.

## 3. Changeset (uncommitted)

```
new:  src/engine/transport/components/HeatFluxModules/SurfaceExchange.{hpp,cpp}
      (pure formulations + applySurfaceExchange binding)
mod:  src/engine/data/HeatData.hpp              (surface_exchange toggle)
mod:  src/engine/transport/components/HeatModule/HeatComponent.cpp
      ([HEAT_FLUXES] section + H3/H4 deferrals)
mod:  src/engine/transport/components/HeatModule/HeatLegacy.cpp
      (call the flux stage before the old-state snapshot)
mod:  src/engine/core/SimulationOptions.hpp     (humidity[12] + 5 constants)
mod:  src/engine/input/handlers/OptionsHandler.cpp   (5 keys)
mod:  src/engine/input/handlers/HydrologyHandler.cpp ([TEMPERATURE] HUMIDITY)
mod:  src/engine/core/SWMMEngine.cpp            (monthly humidity lookup)
new:  tests/unit/engine/test_heat_surface_exchange.cpp  (7 gates)
mod:  tests/unit/engine/CMakeLists.txt          (+1 target)
```

All eight touched TUs pass `g++ -std=c++20 -fsyntax-only`.

**Shared-file note:** `tests/unit/engine/CMakeLists.txt` is shared with the
concurrent save-as-paths session — stage only the one added line.

## 4. Design decisions to review

### 4.1 I did NOT rescale the accumulator to J/s, contrary to H1 §3.1

H1's handoff promised that H2 would convert `node_temp_vol_in` from °C·ft³/s
to J/s. **I did not**, and this is a deliberate second deviation.

Converting the FLUX into the accumulator's units achieves the same thing —
ρw·cp still set the flux's weight against thermal mass, so they are just as
observable (gate 4 proves it) — while touching one function instead of the
seven loader sites plus the mixing stage plus H1's nine gates. The *purpose*
of the promise was observability, and that is met; only the mechanism
differs. Flag if you would rather have the churn.

### 4.2 The module defaults OFF

An H1 deck must keep its behaviour, and every existing `.out` must stay
byte-identical. Each plan §2 module is independently toggleable, so opt-in
is also the shape the plan describes. The cost is that a user who turns on
`HEAT_TRANSPORT` and expects physics gets pure transport — mitigated by
`[HEAT_FLUXES]` being one line, but flag if you would rather default ON.

### 4.3 The Bowen singularity returns 0 rather than propagating

`Br = CB (Pa/P) (Tw − Ta)/(e_sw − e_a)` divides by a vapour-pressure deficit
that is exactly zero when air sits at the water's temperature and 100 % RH —
an ordinary night, not a pathological deck. Since `Je` carries the same
deficit as a factor, `Jc = Br·Je` has a finite limit; returning 0 at the
singularity preserves it. A naive division would put NaN into every
downstream temperature in one step. Gate 2 is the observer.

### 4.4 Air temperature is converted °F → °C at the boundary

`ClimateState::temperature` is °F (legacy convention); every formulation
here is Celsius. One named conversion in `airTempCelsius`, not scattered.

### 4.5 `HUMIDITY` is monthly, like `WINDSPEED`, and a bare value fills all 12

`ClimateState::humidity` has existed with a 50 % default and **no writer
anywhere** since before this program — the GeoPackage climate format reads a
humidity column that never reached the running state. This is its first
consumer and its first writer. TIMESERIES humidity is not implemented; flag
if you want it deferred with an explicit error rather than by omission.

## 5. Validation protocol

1. Reconfigure (new test target), build, zero new warnings.
2. `ctest -R test_engine_heat_surface_exchange` — 7 gates.
   **Run lesson 52's standing grep first:**
   `grep -rn "options.heat_transport" src/engine/` and ask of each hit
   whether the surrounding context also needs the new `surface_exchange`
   flag. H2 adds no stage-level guard of its own — the module rides inside
   `routeLegacyHeat`, which H1's guards already gate — but that claim is
   exactly the kind this family keeps falsifying.
   *Anticipated failure modes, likelihood order:*
   (a) **The storage deck may not hold water.** Gates 3–5 need a wet pool;
   the `[STORAGE] FUNCTIONAL 0 0 5000` row plus InitDepth 6 should give one.
   Gate 3's setup assertion (exchange OFF ⇒ exactly 20 °C) fires first if
   the pool is dry or if something else is moving temperature.
   (b) **`[TEMPERATURE] TIMESERIES air_ts` may not produce 50 °F.** If the
   air temperature is not 10 °C the fluxes are all off; check
   `climate_state.temperature` before reading the gates as wrong.
   (c) **RECT_OPEN parameters** — `3.0 4.0` is depth then width; if the
   conduit never wets, gate 5's positive control fires.
   (d) **Gate 4's 0.02 band** on a ratio of two ~1 hour cooling deltas
   assumes the flux is roughly constant over the run. If the pool cools far
   enough for T-dependence to bite, widen it — but record the measured
   ratio, because a value near 1.0 means something quite different from 0.52.
3. **Falsifier sweep:**

   | falsifier | expected failing gates |
   |---|---|
   | i. drop the minus sign in `deltaT` | 3 (the pool warms) |
   | ii. remove the `ρw·cp` divisor (use volume alone) | 4 (ratio 1.0) — the observability claim itself |
   | iii. return `Br` without the zero-deficit guard | 2, and probably NaN through 3–5 |
   | iv. use `xsect_w_max` instead of `getWofY(xs, depth)` | none of these gates — **flagged in advance**: the area would be wrong (too large) for every partially-full conduit but every assertion here is directional or a ratio. **Record this as unobserved**; closing it needs an absolute first-step energy assertion against a hand-computed area, which I did not write |
   | v. drop the `xsect::isOpen` gate | 5(b) — a closed conduit exchanges |
   | vi. make `getSurfArea` return `MIN_SURFAREA` for junctions | 5(a) |
   | vii. skip the monthly humidity assignment in SWMMEngine | 6 |
   | viii. transcribe 0.61275 as 0.6108 (the other common form) | 1 |
4. **Prior suites:** the module is off by default and every new path is
   behind `heat_config.surface_exchange`, so **`test_engine_heat_transport`
   must stay 9/9** and 14/14 deck `.out` bit-identity must hold against
   `4767aabb`. The `[TEMPERATURE] HUMIDITY` key is additive; decks without
   it keep the 50 % default they already had.
5. **Record:** (a) falsifier iv — I believe the top-width choice is
   unobserved by this gate set and would rather know than assume; (b) the
   measured ratio in gate 4, since that number is the whole content of the
   claim that ρw·cp stopped being decorative.

## 6. Known gaps

- **No absolute energy-balance gate.** Gate 1 pins the formulations against
  outside values and gates 3–5 pin direction and placement, but nothing
  asserts a hand-computed ΔT for a known area and volume. That is the gate
  that would close falsifier iv, and it needs the deck's storage geometry
  read back out of the context.
- Radiative (H3) and sediment (H4) exchange refuse with named errors.
- Evaporated volume is still **not** removed from hydraulics (plan §2.1
  says v1 exchanges heat only); the mass and energy books therefore disagree
  slightly by construction. Recorded in the plan, not introduced here.

## 7. Commit message

```
feat(transport): latent and sensible surface heat exchange (H2)

[HEAT_FLUXES] SURFACE_EXCHANGE ON adds the CSH 4.4-4.5 free-surface terms to
the LEGACY heat mirror: latent Je = rho_w Le(T) f(w) (e_s(Tw) - e_a) and
sensible Jc = Br Je via the Bowen ratio, applied as -(Je + Jc) A / (rho_w cp
V) at the source-term stage. Default OFF, so an H1 deck and every existing
.out are unchanged.

The area question H2 was scoped to answer has an answer that is not new:
heat exchanges exactly where evaporation already does - storage-node free
surfaces (node::getSurfArea) and open conduits (top width x length x
barrels, gated by xsect::isOpen), and nowhere else. Junctions and closed
conduits have no free surface by the engine's own convention, which is also
why they do not evaporate. Both expressions live in code that runs under
every routing model, so the module is not silently inert under STEADY or
KINWAVE.

rho_w and cp become load-bearing here. H1 shipped without them because with
no fluxes they cancelled identically and no gate could observe their value;
a flux competes with thermal mass, so doubling cp must halve the cooling -
which is now a gate.

Relative humidity gets its first writer: ClimateState::humidity has carried
a 50% default with nothing ever setting it, so [TEMPERATURE] HUMIDITY
arrives with the module that needs it, monthly like WINDSPEED.

Gates: tests/unit/engine/test_heat_surface_exchange.cpp - the formulations
against values computed outside this codebase (e_s, Le, f(w), Bowen, Je,
Jc), the removable zero-deficit singularity, cooling under dry air with an
exchange-off setup assertion, the cp ratio test, free-surface placement with
an open-conduit positive control, the humidity plumbing, and the H3/H4
deferrals.

Plan: HEAT_TRANSPORT_PLAN.md section 2.1/2.4/6 H2.
Validation record: plans/transport/H2_VALIDATION_HANDOFF_2026-08-17.md
```

## 8. Validation results

**Verdict: accepted, with gate 4 moved into the regime where its own law
holds.** Commit `221c5dac`. Artifacts:
`tests/output/h2_validation_2026-08-18/`.

| check | result |
|---|---|
| build (reconfigured for the new `.cpp`) | clean, zero warnings in the H2 files |
| `test_engine_heat_surface_exchange` | **7/7** after the gate-4 fix; 6/7 as delivered |
| `test_engine_heat_transport` (H1) | **9/9 unchanged** |
| full `ctest` | **144/145** — only the known FV refinement gate |
| 14-deck `.out` bit-identity | **14/14** — default-OFF holds |
| ASan + UBSan | **0 findings** on both heat suites |
| falsifiers | **7 of 8 caught**; iv escapes, see §8.3 |

### 8.1 The standing grep is clean

`grep -rn "options.heat_transport" src/engine/` returns ten sites; nine are
H1's guards and registration, and `surface_exchange` is consulted in exactly
one place — `applySurfaceExchange`'s own entry guard, inside `routeLegacyHeat`,
which H1's two guards already gate. So the §5 claim holds and this is **not**
another instance of the family. Worth stating explicitly, because the same
claim was made and was wrong at H1.

### 8.2 Gate 4 failed as delivered, and the fix is a shorter deck, not a wider band

`SpecificHeatIsNowObservable` read **0.5648** against 0.5 ± 0.02 — §5(d)'s
anticipated mode. It is the finite-cooling nonlinearity, not a defect: the
1× pool cools 5.1 °C over the hour, which lowers its own flux through
`e_s(Tw)` and the Bowen ratio, so the doubled-cp run loses proportionally
more than half. Measured across run length, which is the evidence rather
than the explanation:

| END_TIME | ΔT (cp = 4184) | ΔT (cp = 8368) | ratio |
|---|---|---|---|
| 5 min | 0.019617 | 0.009813 | **0.500229** |
| 15 min | 0.186918 | 0.093836 | 0.502017 |
| 30 min | 1.036222 | 0.530027 | 0.511499 |
| 1 h | 5.124713 | 2.894522 | 0.564816 |
| 2 h | 13.217905 | 10.325891 | 0.781205 |

Clean convergence to 0.5. **§5(b)'s answer: the measured ratio is 0.500229,
and ρw·cp are unambiguously load-bearing** — H1's unobservability really is
over.

Fixed by giving `write_deck` an `end_time` parameter and running gate 4 at
**5 minutes**, then **tightening** the band from 0.02 to **0.005**. That is a
stronger razor than the delivered one, not a weaker one: at 5 min the law
holds to 2e-4, and a ratio of 1.0 (cp ignored) misses by a hundred bands
instead of twenty-five. Widening to accommodate an hour would have moved the
gate in the wrong direction. The table above is recorded in the gate so the
choice of 5 minutes is justified by data rather than taste.

### 8.3 §5(a): falsifier iv is unobserved, and there are TWO reasons

Confirmed — swapping `getWofY(xs, depth)` for `xsect_w_max` leaves all 7
gates green. But the cause is not what the handoff assumed, and both halves
matter to whoever closes it:

1. **On this deck the two expressions are the SAME NUMBER.** The gates use
   `RECT_OPEN`, and `XSectKernels.hpp:908` reads
   `case XSectShape::RECT_OPEN: return xs.w_max;` — a rectangular section's
   top width is depth-independent by definition. No assertion of any strength
   could separate them here.
2. **Assertion strength is the second, independent reason.** Re-ran the
   falsifier with the section changed to `TRIANGULAR`, where the top width
   genuinely varies with depth: **still 7/7**. Every leg in the suite is
   directional (`cooled at all`) or a ratio, and an over-large area cools
   more, which passes a directional test.

So closing this needs *both* a varying-width section and an absolute
assertion — the handoff anticipated the second and not the first. The
cheapest shape is a one-routing-step deck (`END_TIME` = `ROUTING_STEP`) so
the expected ΔT is a single application of `se::deltaT` with an area computed
from the context's own depth, comparable directly against `link_temp`. The
formulations are already exported and gate 1 already calls them, so the
machinery exists. Left as the §6 gap it was declared to be, now with the
recipe.

### 8.4 Falsifier sweep

| falsifier | outcome |
|---|---|
| i. drop the minus sign | **caught** — pool warms to 29.28 °C |
| ii. remove the ρw·cp divisor | **caught** — NaN through gates 3 and 4 |
| iii. Bowen without the guard | **caught** — "not finite at zero deficit" |
| iv. `xsect_w_max` for top width | **escapes** — §8.3 |
| v. drop the `isOpen` gate | **caught** — "a CIRCULAR conduit exchanged heat" |
| vii. skip the humidity assignment | **caught** — gate 6 |
| viii. 0.6108 instead of 0.61275 | **caught** — gate 1, all three e_s values |

vi (`getSurfArea` returning `MIN_SURFAREA` for junctions) was not run as a
source patch: it would require editing `node::getSurfArea`, which is shared
hydraulics rather than H2 code, and falsifier v already demonstrates the
same gate's sensitivity from the link side. Recorded as not-run rather than
as passed.

### 8.5 On §4.1, the accumulator that was not rescaled

Agree with the deviation. The promise H1 made was observability, and gate 4
now delivers it to 2e-4 with the constants applied to the flux instead of the
accumulator — the mechanism differs, the guarantee does not, and it avoids
churning seven loader sites plus H1's nine gates. Recorded so the next reader
does not go looking for the J/s conversion H1's §3.1 advertised.

### 8.6 Isolation

Validated in a worktree at `4767aabb` carrying H2 only.
`tests/unit/engine/CMakeLists.txt` is shared with the concurrent
save-as-paths session, so only H2's one line was taken and staged.

**Separately: the main index needed refreshing before this changeset could
even be read.** After several commits through `GIT_INDEX_FILE`, `git status`
was reporting seven files as *staged deletions* — including all five H1
sources and `test_output_species_ids.py` — every one of which existed both in
HEAD and on disk. `git diff HEAD` was consequently returning nonsense
(`0+/145-` for files that had been edited, not deleted). Ran a plain
`git reset` after checksumming all 184 listed paths and confirming not one
byte changed. The index is now accurate.
