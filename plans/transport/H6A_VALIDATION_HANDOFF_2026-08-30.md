# H6a Implementation — Validation & Commit Handoff (2026-08-30)

**For:** the checking agent.
**Plan:** `HEAT_TRANSPORT_PLAN.md` §2.5 (SolarRadiation), §6 H6a, §6.4 D-H6a.
**Reference:** Spencer (1971) / NOAA solar position; Bird & Hulstrom (1981)
clear-sky; Kasten–Czeplak cloud attenuation; Bolz cloud emissivity.
**Standing findings:** lessons 1–139.

> **⚠ The author of this change could not build or run the test suite.**
> No `cmake` was available in the implementation environment. Every source
> file was checked with `g++ -std=c++20 -fsyntax-only` and the numeric
> values were confirmed by a standalone probe
> (`tests/output/h6a_solar_2026-08-30/probe.cpp`, output in `probe.out`),
> but **nothing has been linked, and no gate has ever executed.** Task 1
> below is therefore not a formality.

---

## 1. What this delivers

`[RADIATIVE_FLUXES] SHORTWAVE` grows from a single constant to three
mutually exclusive spellings, plus the sections and the C API to drive them.

| | |
|---|---|
| `SHORTWAVE GLOBAL <W/m²>` | H3's constant, unchanged, still the default |
| `SHORTWAVE GLOBAL TIMESERIES <name>` | measured record, tseries-interpolated |
| `SHORTWAVE GLOBAL COMPUTED` | Spencer/NOAA position → Bird clear-sky |

Cloud fraction modulates **both** directions (D-H6a-2): shortwave by
Kasten–Czeplak `1 − k·C^n`, longwave by Bolz `εatm ← εatm(1 + k_lw·C²)`.

### Changeset (uncommitted)

```
new:  src/engine/transport/components/HeatFluxModules/SolarRadiation.{hpp,cpp}
new:  include/openswmm/engine/openswmm_heat.h
new:  src/engine/core/openswmm_heat_impl.cpp
new:  tests/unit/engine/test_heat_solar_radiation.cpp        (17 gates)
new:  tests/output/h6a_solar_2026-08-30/probe.{cpp,out}      (numeric evidence)
mod:  src/engine/data/HeatData.hpp        (ShortwaveMode, SolarConfig,
      CloudConfig, RadiativeConfig::sw_mode/sw_ts_index,
      HeatState::shortwave_now/cloud_now)
mod:  .../HeatFluxModules/RadiativeExchange.{hpp,cpp}  (two DEFAULTED params)
mod:  .../HeatFluxModules/HeatFluxes.cpp              (prologue + H6b comment)
mod:  .../HeatModule/HeatComponent.cpp     (SHORTWAVE spellings, two new
      sections, cross-section checks)
mod:  .../HeatModule/{HeatWatershed,HeatLid}.cpp      (prologue)
mod:  .../EulerianArdComponent/ArdEngine.cpp          (prologue)
mod:  tests/unit/engine/CMakeLists.txt
mod:  plans/transport/HEAT_TRANSPORT_PLAN.md          (§2.2 corrections,
      §5, §6 H6a, D-H6a-4 amended)
mod:  CHANGELOG.md
```

---

## 2. ⚠ START HERE — what is NOT verified, and why it matters

This section is the reason the handoff exists. Work it top to bottom before
trusting a green run.

### 2.1 Nothing has been compiled as a whole or linked

Run the build and the heat suite first. Expect the possibility of link
errors the syntax check cannot see (the `[[nodiscard]]`/ODR class, missing
CMake registration of `openswmm_heat_impl.cpp` — sources are globbed by
`src/engine/CMakeLists.txt:30`, so it *should* be picked up, **verify that
it is**).

```
cmake --build build -j
ctest --test-dir build -R "heat" --output-on-failure
```

### 2.2 Bird's coefficients are TRANSCRIBED FROM MEMORY

`SolarRadiation.cpp`'s five transmittance terms, the `0.2758/0.35`
broadband aerosol weighting, `Ba = 0.85`, the `0.9662` direct-beam factor
and the `0.79` diffuse factor were written **without the 1981 paper in
reach**. They produce physically sound output (see §3), which is exactly
what a plausible-but-wrong coefficient looks like — the H3 Brunt-in-kPa
lesson.

**Your first substantive job:** obtain Bird & Hulstrom (1981) — or NREL's
published Excel/`SOLPOS` implementation — and check each coefficient
term-by-term. Then decide what `test_heat_solar_radiation.cpp` **gate 5**
is: it is currently labelled a *regression pin*, not a parity gate. If the
coefficients check out, relabel it and say so in the file's header comment.
If any is wrong, the fix is one constant and the pin value moves — record
the before/after here.

### 2.3 Solar position is Spencer/NOAA, and the plan used to say SPA

D-H6a-4 was **amended at implementation** — read it before reviewing the
module. The short version: a faithful NREL SPA needs ~260 rows of
periodic-term constants, and writing them from memory alongside the test
vector meant to check them buys an "external" gate that is a second copy of
the same memory. Spencer/NOAA is ~40 lines, checkable by inspection, and
accurate to ~0.1° — against a cloud fraction that is a whole-number guess.

**Do not "upgrade" this to SPA as part of validating H6a.** If SPA is
wanted, it is a separate change against NREL's published C source, where
the tables can be *diffed*. `solarPosition()` is the only function that
knows how a position is obtained; nothing downstream changes.

### 2.4 The gate hierarchy is deliberate — do not flatten it

`test_heat_solar_radiation.cpp`'s header ranks its own gates. Preserve that
when you edit: astronomical > cross-implementation > invariant >
transcription pin. A reviewer who tightens gate 5's band without doing §2.2
has made the suite *look* stronger while changing nothing.

---

## 3. Numeric evidence already gathered

From `tests/output/h6a_solar_2026-08-30/probe.out` — the built C++, not a
model of it:

| check | value | expectation |
|---|---|---|
| declination, Jun solstice | 23.4520° | 23.44° (obliquity) ✓ |
| declination, Dec solstice | −23.4199° | −23.44° ✓ |
| declination, Mar/Sep equinox | −0.066° / +0.249° | ~0 ✓ |
| E0, Jun solstice / Jan 1 | 0.96744 / 1.03505 | <1 near aphelion, >1 near perihelion ✓ |
| 40 °N solstice noon zenith | 16.5503° | \|40 − 23.44\| = 16.56 ✓ |
| midnight `cos_zenith` | exactly 0 | clamped, not negative ✓ |
| air mass, z = 0 / 60° | 0.999712 / 1.994293 | 1 / 2 ✓ |
| pressure, 0 m / 1400 m | 1013.25 / 855.99 mb | standard atmosphere ✓ |
| Bird GHI, z = 0, sea level | 1056.09 W/m² | ~1050 for a standard atmosphere ✓ |
| Bird GHI monotone in z | 1056 → 43 → 0 | strictly decreasing, dark below horizon ✓ |
| cloud SW factor, C = 0 / 1 | exactly 1 / 0.25 | identity / `1 − k` ✓ |
| cloud LW factor, C = 0 / 1 | exactly 1 / 1.17 | identity / `1 + k_lw` ✓ |

**A longitude/timezone sign check worth keeping:** a site on its zone
meridian (−105°, UTC−7) returns the *same* hour angle as (0°, UTC+0) —
−0.3321°, the equation of time alone. A flipped sign shows up here as a
~14 h error, not a subtle one. Gate 2 pins it.

---

## 4. Systematic evaluation — work these in order

### Step 1 — Build and run (§2.1). Nothing below is meaningful until this passes.

### Step 2 — The H3 baseline must not have moved

This is plan §2.5's own verify criterion and the highest-consequence check
in the change, because the cloud correction reaches into the RHE-gated
Brunt term.

```
ctest --test-dir build -R "heat_radiative_exchange" --output-on-failure
```

Every H3 gate calls the **two-argument** `atmosphericEmissivity` and the
**four-argument** `netRadiativeFluxOut`; the new parameters are defaulted
so those call sites are untouched. Then run the corpus:

```
# heat_parity must be BYTE-IDENTICAL with cloud unconfigured
<corpus driver> --deck heat_parity
```

→ **If `heat_parity` moves at all, stop.** The H3 baseline is the only
external reference the radiative path has; losing it silently is worse than
any bug in this change. Report the diff rather than rebaselining.

### Step 3 — Verify the claims this handoff makes about its own design

Each of these is a design decision that a reviewer should confirm actually
holds in the code, not just in the prose:

1. **`cloudLongwaveFactor(0, k)` returns a literal `1.0`**, and
   `atmosphericEmissivity` *short-circuits* on `cloud_factor == 1.0` rather
   than multiplying. Check both. The claim is bit-identity, not closeness.
2. **`netRadiativeFluxOut`'s `jin_wm2` sentinel is negative, not NaN or 0** —
   0 is a legal night-time irradiance and must not read as "unset".
3. **`updateSolarForcing` is called in all FOUR binding prologues**
   (`HeatFluxes.cpp`, `ArdEngine.cpp`, `HeatWatershed.cpp`, `HeatLid.cpp`).
   They run on two clocks; a missed one means a stale `Jin` under that
   engine. Grep for `netFluxOut(` and confirm every entry point that
   reaches it refreshes first.
4. **No fifth flux family was added.** `netFluxOut` still sums exactly
   `surfaceFluxOut + radiativeFluxOut`. If this change grew a term, it has
   silently taken on H6b's node/link merge decision.
5. **The API and the parser refuse the same things.** Gate 10 asserts it;
   read both and confirm the gate is testing the real overlap, not a subset.

### Step 4 — Hunt for the failure modes this design is exposed to

Not "does it work" but "where would this break silently":

- **A `COMPUTED` deck that reaches `updateSolarForcing` unsited.** The
  parser refuses it and the runtime falls dark as a backstop. Confirm the
  backstop is unreachable *and* that it fails dark rather than at latitude
  0. An equatorial-noon answer is the plausible-wrong result the whole trap
  exists for.
- **Timeseries past its end.** `table_tseries_lookup_cursor` returns 0, so
  a solar record that runs out goes dark rather than holding yesterday's
  noon forever. Confirm — and confirm the *cloud* series does the same,
  where 0 means "clear", which is the benign direction but should be
  deliberate.
- **`heat_state.shortwave_now` across hotstart / model reopen.** It is
  runtime state, deliberately not config. Check that a reopened or
  hot-started model resolves it fresh on the first step rather than
  resuming from a stale value — and that `HeatState::clear()` zeroes it.
- **Unit-system dependence.** `[RADIATIVE_FLUXES]` takes W/m² in *both* US
  and SI decks. Plan §7 flags this for the TIMESERIES path specifically:
  confirm a US-units deck reads a solar series as W/m² and not as something
  the unit machinery converted. **This is an open item, not a solved one.**
- **`ClimateState::elev` is FEET** and `SolarConfig::elevation_m` is
  metres; `SolarRadiation.cpp` converts with `kFtToM` on the fallback path.
  Confirm, then confirm an SI deck's `elev` is also feet internally (it
  should be — the engine is foot-second throughout) rather than
  double-converted.

### Step 5 — Adversarial decks

Write these if they do not exist; each targets a specific silent-success:

| deck | must |
|---|---|
| `SHORTWAVE GLOBAL TIMESERIES` + `[CLOUD_COVER]` | **warn** about double-counting, and still run |
| `[SOLAR_RADIATION]` present, mode not COMPUTED | **warn** that coordinates are unused |
| `FRACTION GLOBAL 75` | error — it is a fraction, not a percent |
| `LATITUDE GLOBAL 100` | error, not saturation to 90 |
| cloud series carrying 1.7 and −0.3 | clamped at runtime, run continues |
| `SHORTWAVE GLOBAL COMPUTED extra_token` | error |

### Step 6 — Fix what you find, then reconcile the record

Fix defects directly; this handoff is not a proposal. Then update, in this
order: the module header if a formulation changed → the gate that should
have caught it → `HEAT_TRANSPORT_PLAN.md` §2.5 if the *design* changed →
`CHANGELOG.md`. And append a `## 6. Findings` section here recording what
moved, in the style of `H5B_MASS_FIX_HANDOFF_2026-08-19.md`.

---

## 4b. Already found and fixed in an internal review round

An independent review pass ran before this handoff was written. Seven real
defects came out of it, all fixed. They are listed because **each one is a
class of mistake likely to recur elsewhere in the change**, and because a
reviewer who knows what was already caught can spend attention elsewhere.

1. **NaN walked through every range guard.** `strtod` accepts `"nan"`, and
   every guard is `v < lo || v > hi` — both FALSE for NaN. `LATITUDE GLOBAL
   nan` was stored with no error. Fixed with `parse_finite` in the new
   parsers (gate 13). **The pre-existing `frac()` lambda and the
   `[HEAT_SOURCES]` path have the same hole and were deliberately NOT
   touched** — out of scope, but they are real. See §5.
2. **A `< 0` elevation sentinel collided with legal input.** The parser
   admits −500 m; consumption read any negative as "use the climate value".
   Every below-sea-level site silently got the wrong pressure. Replaced
   with `has_elevation` (gate 14).
3. **`[CLOUD_COVER]` with only coefficients was silently inert** —
   coefficient rows never set `configured`, so the whole section was
   skipped with no diagnostic (gate 15).
4. **Warnings outlived a rejected configuration.** They went straight to
   `ctx.warnings` while an error reset the config, so on a lenient open the
   user was warned about a config that never took effect — including being
   told to "Set SHORTWAVE GLOBAL COMPUTED" when they had written exactly
   that and mistyped it. Now buffered and merged only on success (gate 16).
5. **`shortwave_now` defaulted to 0.0, making the sentinel unreachable.**
   `radiativeFluxOut` passes it positionally into `jin_wm2`, whose
   "unset" sentinel is negative — so any call before the step's
   `updateSolarForcing` dropped the shortwave term to zero instead of
   falling back on the configured constant. Default is now −1.0, folded to
   0 at the API boundary (gate 6, extended).
6. **`HeatState::resize` did not reset the new scalars**, so a
   re-initialize left the previous run's forcing readable (gate 6).
7. **C API drift from the parser:** `BADINDEX` where the sibling files use
   `BADPARAM` for a failed *name* lookup; an ELEVATION range that
   contradicted the parser's; and two header claims that were simply false
   (that switching shortwave modes clears the others, and an undocumented
   refusal). All corrected.

**The one thing the review checked and found clean:** the SHORTWAVE
three-spelling restructure itself, and the physics transcription of the
Spencer/NOAA series (equation of time, declination, eccentricity) — those
were verified against the published coefficients by inspection. Bird was
verified for *structure* (pressure-corrected air mass on Rayleigh and mixed
gases, uncorrected on ozone/water/aerosol — correct) but **not for its
constants**, which remains §2.2.

## 5. Known-weak spots — do not let a green run reassure you

1. **Bird coefficients** (§2.2). The single largest unverified surface.
2. **Land-cover longwave still uses air temperature**, not a canopy
   temperature state — an H3 known difference this change does not touch,
   and now interacting with a cloud factor that also keys off air
   temperature. Worth one thought about whether the two compound.
3. **Cloud attenuation applies to every spelling, including
   `TIMESERIES`.** That is deliberate (a clear-sky-corrected record is a
   legitimate input) but it is the most likely user error in the whole
   feature, and it only warns. Consider whether the warning is loud enough.
4. **`GROUND_ALBEDO` vs `ALBEDO`.** Two albedos, two surfaces (land vs
   water), two config structs, adjacent names. Nothing stops a user
   swapping them and the model will run. There is no gate for this because
   there is no wrong-looking output — flag if you see a way to make it
   detectable.
5. **The equinox divergence in gate 3 is asserted as a FEATURE** — this
   module is closer to truth there than `Climate.cpp`'s single-cosine fit.
   If someone later "fixes" the disagreement by matching legacy, that is a
   regression, and gate 3's final `EXPECT_LT` is the only thing saying so.
6. **`parse_finite` was added only to the NEW parsers.** The pre-existing
   `frac()` lambda in `[RADIATIVE_FLUXES]` and the `[HEAT_SOURCES]`
   `parse_celsius` path both accept `nan`/`inf` through the same
   `v < lo || v > hi` hole (§4b item 1). Left alone under the
   surgical-changes rule — **but they are live bugs**, and fixing them is a
   small, well-defined follow-up worth doing while the reason is fresh.
7. **`[RADIATIVE_FLUXES]` accepts trailing junk.** Its guard is
   `toks.size() < 3`, so `ALBEDO GLOBAL 0.3 JUNK` silently ignores the
   fourth token, while the new `[SOLAR_RADIATION]` parser requires exactly
   three. Two conventions now live in one file. Pre-existing; not changed;
   worth reconciling deliberately in one direction or the other.
8. **Three `.fuse_hidden*` files** appeared in
   `src/engine/transport/components/{EulerianArdComponent,HeatModule}/`
   during editing (FUSE artifacts of in-place writes on a mounted
   filesystem). They could not be deleted from the authoring environment.
   **Delete them before committing** — they are not part of the change.

---

## 6. Findings — CHECK ROUND 2026-08-31 (run together with step 3)

Validated in an isolated detached worktree at `deb42172` (base measured
first: 180/181 ×1, only `fv_tpa_closure` — the standing figure). The heat
changeset (H6a + step 3 + this round's fixes) was applied on top; evidence
under `tests/output/step3_heat_api/`.

### 6.1 The suite had NEVER run, and its one defect proved it (FIXED)

First execution: **7 of 18 gates failed** — every gate that expects a deck
to OPEN. Root cause is a FIXTURE bug, not an engine bug: `write_deck` and
every call site emitted

```
[PROCESS_COMPONENTS]
org.hydrocouple.openswmm.heat _h6a_x.heat        ← missing config="…"
```

and the parser refuses the row (`missing required config="…" argument`).
Worse than the 7 reds: the REFUSAL gates (8, 9, 12, 13) were passing
**vacuously** — the open failed on the malformed component row, not on the
refusal under test. All 18 call sites converted to `config="…"`;
**18/18 pass**, and the refusal gates now fail for the right reason when
falsified. (The step 3 suite used the correct spelling from the start.)

### 6.2 §2.2 Bird coefficients — VERIFIED, gate 5 relabelled

Checked term-by-term against pvlib's NREL-faithful `clearsky.bird`
(fetched from the pvlib-python source):

- ozone exponent **−0.3034** ✓ (some secondary sources print −0.3035;
  pvlib and the code agree on −0.3034)
- forward-scatter ratio **Ba = 0.85** ✓ (pvlib `asymmetry=0.85`; the
  PlantPredict rendering's 0.84 is its own variant)
- 0.9662 direct, 0.79 diffuse, 0.5/1.02 diffuse shape, TAA with K1 = 0.1,
  rs = 0.0685 + (1−Ba)(1−TAS) ✓
- air-mass usage ✓: pressure-corrected on Rayleigh + mixed gases,
  UNCORRECTED on ozone/water/aerosol — matches pvlib exactly
- one knowingly kept truncation: broadband AOD weight **0.2758** vs
  pvlib's 0.27583 (NREL's own Excel uses 0.2758; ~3e-5 relative)

Gate 5 and the file header are relabelled: verified reference pin.

### 6.3 §3 design claims — all five hold in the code

(1) literal 1.0 + short-circuit ✓; (2) negative jin sentinel ✓;
(3) `updateSolarForcing` in all FOUR prologues ✓ (HeatFluxes, ArdEngine,
HeatWatershed, HeatLid); (4) `netFluxOut` still sums exactly two families ✓;
(5) gate 10 drives real parser/API overlap ✓ — and the round found one
place they disagreed, §6.5.

### 6.4 §4 failure hunts — closed by reading, one by new gate

- COMPUTED unsited: parser refuses; runtime backstop fails dark ✓.
- Timeseries past its end: `table_tseries_lookup_cursor`
  (`TableData.hpp:275`) returns 0 both past the end AND before the start —
  solar goes dark, cloud goes clear. Deliberate and benign ✓.
- Reopen/re-init freshness: `HeatState::resize` restores the −1 sentinel
  (gate 6 pins it) ✓.
- Units: the TIMESERIES path reads the table RAW — W/m² in both unit
  systems by construction ✓ (plan §7's open item can close).
- `ClimateState::elev` is FEET (`Climate.hpp:109`), `kFtToM` on the
  fallback path only; `snow_elev` is stored VERBATIM from the deck
  (legacy parity — legacy applies no conversion either), so no double
  conversion exists ✓.

### 6.5 §5.6 was HALF wrong — and the live half is FIXED

`parse_celsius` **never had the NaN hole**: its range test is the
conjunctive ACCEPT form (`v >= lo && v <= hi`), which NaN fails. The
`[RADIATIVE_FLUXES]` `frac()` ladder DID have it (reject form), and it was
a live deck/API disagreement — `ALBEDO GLOBAL nan` stored NaN while the
API's `frac_ok` refuses it. Fixed by routing the ladder's parse through
`parse_finite`; gate 13 gained an ALBEDO nan/inf leg. §5.7 (trailing junk
in `[RADIATIVE_FLUXES]`) is unchanged and stays open.

### 6.6 §5's adversarial decks now exist (gate 13b)

FRACTION 75 → error; LATITUDE 100 → error; TIMESERIES + [CLOUD_COVER] →
warns, runs; [SOLAR_RADIATION] without COMPUTED → warns, runs; a cloud
SERIES carrying 1.7 → clamped to 1.0 at runtime, run continues (asserted
through `swmm_heat_get_current_cloud` after a step). The trailing-token
COMPUTED deck was already exercised by gate 16.

### 6.7 The H3 baseline did not move

`test_engine_heat_radiative_exchange` green; corpus verdict recorded in
the step 3 record (`STEP3_HEAT_API_GAP_HANDOFF_2026-08-30.md` CHECK
RECORD), including `heat_parity` byte-identity.
