# H3 Implementation — Validation & Commit Handoff (2026-08-17)

**For:** the checking agent.
**Base:** `221c5dac`.
**Plan:** `HEAT_TRANSPORT_PLAN.md` §2.2 (RadiativeExchange), §6 H3.
**Reference:** `HydroCouple/RHEComponent/src/element.cpp:106-135`,
`rhemodel.cpp:43-47` — read directly, not paraphrased.
**Standing findings:** lessons 1–56.

---

## 1. What this delivers

The four radiative terms of plan §2.2, as pure functions plus a binding on
the same free surfaces H2 uses:

| term | formula | direction |
|---|---|---|
| net shortwave | `Jsn = (1 − Rs) Jin max(0, 1 − fs)` | into water |
| back longwave | `Jbr = εw σ Tw⁴` | out of water |
| atmospheric longwave | `Jan = εatm σ Ta⁴ (1 − RL) fsky` | into water |
| land-cover longwave | `Jlc = εlc σ Ta⁴ (1 − fsky)` | into water |

`[HEAT_FLUXES] RADIATIVE_EXCHANGE ON` enables it; parameters come from a new
`[RADIATIVE_FLUXES]` section. **Default OFF**, like H2.

## 2. Two corrections the plan text does not carry

Reading the reference rather than the plan summary caught both. Either would
have shipped as a silent physics error.

### 2.1 The sky-view factor SPLITS the longwave budget

Plan §2.2 writes `Jan = εatm σ Ta⁴ (1 − RL)` with no `fsky`, and `Jlc` with
`(1 − fsky)`. The reference multiplies `Jan` by `fsky`
(`element.cpp:127-128`). They are **complementary shares of one hemisphere**,
not independent terms: written the plan's way, an open-sky element receives
full atmospheric longwave *and* whatever land-cover term survives, and a
fully-canopied one still receives sky radiation through the canopy.

**Gate 3 asserts the physical invariant** — at equal emissivities the total
incoming longwave is independent of `fsky` — which is a stronger statement
than reproducing either term alone.

### 2.2 Brunt's square root takes PASCALS

`emiss = Aa + 0.0027 √(e_a · 1000)` (`element.cpp:125`). Vapour pressure is
computed in kPa, so the ×1000 is a unit conversion. Feeding kPa understates
the term by √1000 ≈ 31.6 — and the result, ~0.502 against a correct
0.567, **is still a plausible emissivity**. Nothing except a reference value
catches this; gate 2 exists for it alone.

## 3. Changeset (uncommitted)

```
new:  src/engine/transport/components/HeatFluxModules/RadiativeExchange.{hpp,cpp}
mod:  src/engine/data/HeatData.hpp   (RadiativeConfig + radiative_exchange)
mod:  src/engine/transport/components/HeatModule/HeatComponent.cpp
      ([RADIATIVE_FLUXES] parser; the H3 deferral flipped to real)
mod:  src/engine/transport/components/HeatModule/HeatLegacy.cpp   (+1 call)
new:  tests/unit/engine/test_heat_radiative_exchange.cpp   (7 gates)
mod:  tests/unit/engine/CMakeLists.txt   (+1 target — shared file, stage the
      one line only)
```

All five touched TUs pass `g++ -std=c++20 -fsyntax-only`.

## 4. Design decisions to review

### 4.1 The sediment shortwave split is NOT carried over — deliberately

RHE splits absorbed shortwave into a water share and a bed share with
`exp(−extinction · depth)` and hands the latter to its sediment column
(`element.cpp:109-111`). **There is no sediment column until H4**, so H3
keeps all absorbed shortwave in the water.

This is a known overestimate of warming in shallow clear water, not an
oversight. The alternative — discarding the bed share — would lose energy
with nowhere to book it. `EXTINCTION` is refused by name in
`[RADIATIVE_FLUXES]` (gate 6) so a user cannot configure a split that does
not exist. **Flag if you would rather discard than over-warm.**

### 4.2 Land-cover longwave uses AIR temperature, not a canopy state

The reference carries a separate `landCoverTemperature`; plan §2.2 specifies
air temperature and I followed the plan. Known consequence: a daytime canopy
warmer than the air radiates more than this predicts. Recorded in the
function's own comment.

### 4.3 Sign convention flipped once, in one place

RHE's `netMCRadiation` is positive INTO the water, with `backLWRadiation`
pre-negated at source. This module returns **positive OUT**, matching
SurfaceExchange so the two sum. The flip lives only in
`netRadiativeFluxOut`; every individual term is returned in its own natural
direction and documented as such.

### 4.4 Fractions are refused outside [0, 1], not clamped

An emissivity typed as `97` for 0.97 would scale every longwave term by a
hundred and still produce finite numbers. Gate 6 covers it.

## 5. Validation protocol

1. Reconfigure, build, zero new warnings.
   **Lesson 52's standing grep:** `grep -rn "heat_config\." src/engine/` and
   ask of each hit whether it needs the new `radiative_exchange` flag. I
   believe not — the module rides inside `routeLegacyHeat` exactly as H2's
   does — but that is the claim this family keeps falsifying.
2. `ctest -R test_engine_heat_radiative_exchange` — 7 gates.
   *Anticipated failure modes, likelihood order:*
   (a) **The pool may not hold water** — gate 5's OFF leg asserts the seed
   first, so a dry deck fails there with a legible message.
   (b) **Air temperature** must be 10 °C (50 °F); if `climate_state.
   temperature` differs every reference number is off. Check it before
   reading gate 1 as wrong.
   (c) **Gate 5 runs 5 minutes** — H2 lesson 55's discipline, the regime
   where the flux is effectively constant. If a leg is marginal, **shorten
   further rather than widening**; do not loosen a band that is asserting a
   direction.
3. **Falsifier sweep:**

   | falsifier | expected failing gates |
   |---|---|
   | i. drop `fsky` from `atmosphericLongwave` | **3** — and only 3; every other assertion uses fsky = 1 where the factor is invisible. This is the plan-text error under test |
   | ii. use kPa in `atmosphericEmissivity` | **2**, and 1's Jan leg, and 4's night value |
   | iii. drop the `(1 − RL)` factor | 1's Jan leg, 4's night value |
   | iv. use Celsius instead of Kelvin in `kelvin4` | 1 and 4 catastrophically — record the magnitude, it should be absurd rather than subtle |
   | v. flip the sign of `netRadiativeFluxOut` | 4 and 5 (sun cools, night warms) |
   | vi. drop `max(0, 1 − fs)` in `netShortwave` | 1's over-unity shade leg |
   | vii. accept fractions outside [0,1] | 6 |
   | viii. leave the H3 deferral error in `HeatComponent` | 7 — retiring a deferral must flip its gate in-changeset (lesson 21) |
4. **Prior suites:** default OFF and every path behind
   `heat_config.radiative_exchange`, so **`test_engine_heat_transport` 9/9
   and `test_engine_heat_surface_exchange` 7/7 must be unchanged**, and
   14/14 deck `.out` bit-identity must hold against `221c5dac`.
5. **Record:** (a) whether falsifier i fails ONLY gate 3 — if it fails
   nothing, the sky-view correction is unobserved and §2.1 is an assertion
   rather than a tested claim; (b) the magnitude under falsifier iv.

## 6. Known gaps

- **No RHEComponent golden-file parity run.** Gate 1 carries values computed
  from the reference's *formulas*, which catches transcription errors but
  not a misunderstanding of its inputs. Plan §6 H3 asks for parity against
  RHE outputs for identical forcing; that needs the component built and run,
  which I cannot do here. **This is the one thing I would most like added.**
- **Shortwave is a constant.** RHE's per-element `[RADIATIVE_FLUXES]` ranges
  and timeseries are refused with a named error; a diurnal cycle needs them.
  Clear-sky computation is plan phase H6.
- H2's owed gate (falsifier iv, top width vs `w_max`) is still owed and
  applies here too — this module uses the same area expression.

## 7. Commit message

```
feat(transport): shortwave and longwave radiation at the surface (H3)

[HEAT_FLUXES] RADIATIVE_EXCHANGE ON adds the RHE section 6 terms to the
LEGACY heat mirror: net shortwave (1-Rs)Jin(1-fs), back longwave eps_w sigma
Tw^4, atmospheric longwave with Brunt emissivity, and land-cover longwave,
applied on the same free surfaces H2 uses. Parameters come from a new
[RADIATIVE_FLUXES] section. Default OFF, so H1/H2 decks and every existing
.out are unchanged.

Reading RHEComponent directly rather than the plan summary caught two things
the summary omits, either of which would have shipped as silent physics:
the sky-view factor SPLITS the longwave budget (Jan carries fsky, Jlc
carries 1-fsky - they are complementary shares of one hemisphere, and
without it an open-sky element double-counts), and Brunt's square root takes
PASCALS, not the kPa the vapour pressure is computed in - in kPa the
emissivity is understated by sqrt(1000) and still looks plausible at 0.502
against a correct 0.567.

The sediment shortwave split is deliberately not carried over: RHE sends a
depth-attenuated share to its sediment column, and there is no sediment
column until H4, so H3 keeps all absorbed shortwave in the water. EXTINCTION
is refused by name so nobody configures a split that does not exist.

Gates: tests/unit/engine/test_heat_radiative_exchange.cpp - the four terms
against the reference's arithmetic, Brunt in pascals, the sky-view
invariant (total incoming longwave independent of fsky at equal
emissivities), the night/day sign reversal, a pool that cools at night and
warms under sun with an exchange-off setup assertion, out-of-range refusal,
and the H3 deferral retired while H4's stays.

Plan: HEAT_TRANSPORT_PLAN.md section 2.2/6 H3.
Validation record: plans/transport/H3_VALIDATION_HANDOFF_2026-08-17.md
```

## 8. Validation results

**Verdict: the physics is right — both §2 corrections check out independently
— but the gate defending the more important one could not observe it.**
Fixed, plus one file the manifest missed. Commit `7038bea9`. Artifacts:
`tests/output/h3_validation_2026-08-18/`.

| check | result |
|---|---|
| build (reconfigured) | clean, zero warnings in the H3 files |
| `test_engine_heat_radiative_exchange` | **7/7** after the gate-3 fix; 6/7 as delivered |
| `test_engine_heat_surface_exchange` (H2) | 6/7 as delivered → **7/7** after §8.3 |
| `test_engine_heat_transport` (H1) | **9/9 unchanged** |
| full `ctest` | **145/146** — only the known FV refinement gate |
| 14-deck `.out` bit-identity | **14/14** |
| ASan + UBSan (all three heat suites) | **0 findings** |
| falsifiers | **8 of 8 caught** |

### 8.1 Gate 3 failed, and as written it could not have passed either way

`SkyViewSplitsTheLongwaveBudget` asserts that at **equal emissivities** the
total incoming longwave is independent of `fsky`. It failed by 6.1055 W/m²
per 0.25 of `fsky` — and the cause is the gate, not the module.

The third argument of `atmosphericLongwave` is Brunt's **Aa coefficient**,
not an emissivity: `εatm = Aa + 0.0027 √(e_a·1000)`. The gate passed `0.97`
there, making **εatm = 1.0370 — an emissivity above unity** — against
εlc = 0.97. The premise "equal emissivities" was never established, so the
sum drifts by `(εatm − εlc)·σTa⁴·Δfsky` whether the code is right or wrong.
Reproduced by hand to every digit:

```
sigma*Ta^4 = 364.4595   eps_atm(as configured) = 1.037009   eps_lc = 0.970000
fsky 0.00 -> 353.525754      0.75 -> 371.842321
fsky 0.25 -> 359.631276      1.00 -> 377.947843
step = 6.105522 W/m2 per 0.25 fsky        (observed: 6.105522)
```

With the emissivities genuinely equal the invariant holds **exactly** —
353.525753839 at every `fsky`. So the implementation satisfies §2.1; the gate
did not test it.

Fixed by deriving the land-cover emissivity from `atmosphericEmissivity()`
with the same coefficient the atmospheric term uses, so "equal emissivities"
is true by construction, plus an `ASSERT_LT(eps, 1.0)` so a coefficient read
as an emissivity is caught immediately rather than as a drifting sum.

**This matters for §5(a).** As delivered the gate failed both with and
without falsifier i — a gate that fails either way discriminates nothing.
After the fix, **falsifier i fails ONLY gate 3, and gate 3 only** (6 passed,
1 failed). §2.1 is now a tested claim rather than an assertion.

### 8.2 §5(b) and an independent check of the reference arithmetic

**Falsifier iv's magnitude is absurd, as hoped:** `backLongwave` returns
**0.0087998** instead of 406.1761205 — a factor of **46,157** — because
(293.15/20)⁴ is what separates Kelvin from Celsius here. Nobody would ship
that unnoticed.

`RHEComponent` is **not** in the local `HydroCouple` checkout (that repo is
the framework; there is no `RHEComponent/` directory and no
`backLWRadiation`/`netMCRadiation` symbol anywhere in it), so I could not run
the golden-file parity of §6 and could not verify the cited line numbers.
What I could do instead — and did — is **recompute every gate-1 reference
value outside the codebase**, from the stated formulas and σ = 5.67e-8:

| term | recomputed | gate |
|---|---|---|
| `backLongwave(20, 0.97)` | 406.1761205 | 406.1761205 |
| `atmosphericLongwave(Aa=.5, RL=.03)` | 200.4523032 | 200.4523032 |
| its Brunt emissivity | 0.56700905 | 0.56700905 |
| `landCoverLongwave(0.97, fsky=0.4)` | 212.1154523 | 212.1154523 |
| `netShortwave(800, .08, fs=0/.25/1.5)` | 736 / 552 / 0 | 736 / 552 / 0 |

Every one matches. That is not RHE parity — it cannot catch a
misunderstanding of RHE's *inputs*, which is precisely what §6 wants — but it
does independently confirm the transcription and the unit handling. **§2.2's
kPa/Pa correction is confirmed too**: in kPa the emissivity is 0.502119
against 0.567009, and 0.502 is entirely plausible, so nothing but a reference
value catches it. Gate 2 earns its place.

### 8.3 One file the manifest missed: H2's gate asserted the deferral H3 retires

H2's `FluxModuleTogglesParseAndDefer` asserted that `RADIATIVE_EXCHANGE`
*refuses* — correct until this changeset implements it. Retiring a deferral
has to flip **every** gate that asserted it, and §5.3 viii covers only H3's
own. Moved H2's leg to `SEDIMENT_EXCHANGE` (H4), which is now the deferral
still owed, rather than deleting it — so the suite keeps a live observer for
the next retirement. Same shape as H1's `ProcessComponentsTest` stand-in;
that is twice in three phases, and it is worth adding to the standing checks:
**grep the test tree for the phase name you are retiring.**

### 8.4 Falsifier sweep

| falsifier | outcome |
|---|---|
| i. drop `fsky` from `Jan` | **caught — gate 3 alone** (after §8.1) |
| ii. Brunt in kPa | **caught** — gates 1, 2 and 4's night value |
| iii. drop `(1 − RL)` | **caught** — gates 1 and 4 |
| iv. Celsius⁴ | **caught** — 46,157× low |
| v. flip the net sign | **caught** — gates 4 and 5 |
| vi. drop the shade clamp | **caught** — `netShortwave(800,.08,1.5) = −368` |
| vii. accept fractions outside [0,1] | **caught** — gate 6 |
| viii. leave the H3 deferral | **caught** — gates 5 and 7 |

### 8.5 On the design decisions

- **§4.1 (no sediment split):** agree, and refusing `EXTINCTION` by name is
  the right guard — a configurable split into a column that does not exist
  would be worse than the documented over-warming. Worth carrying into H4's
  handoff as an explicit "unretire this" item.
- **§4.2 (air temperature for land cover):** agree with following the plan;
  the divergence from RHE is documented in the function itself, which is
  where a reader will meet it.
- **§4.3 (one sign flip):** verified — `netRadiativeFluxOut` is the only
  place the convention turns, and falsifier v shows it is load-bearing.

### 8.6 Isolation

Worktree at `221c5dac` carrying H3 only.
`tests/unit/engine/CMakeLists.txt` is shared with the concurrent
save-as-paths session — one line taken and staged.
