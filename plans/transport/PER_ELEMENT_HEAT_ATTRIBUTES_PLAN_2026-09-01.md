# Per-element heat attributes — Implementation Plan (2026-09-01)

**Phase name: PE (per-element).** Radiative and conduction attributes that
are physically properties of *a place* — shading, sky view, land-cover
emissivity and temperature, hyporheic velocity, burial depth, ground
temperature, bed material — become specifiable per element, with per-element
values overriding the global ones.

**Motivating observation.** Every one of these is currently a single number
for the whole model. A network with a shaded riparian reach and an exposed
concrete channel gets one shade factor. A trunk sewer 4 m deep and a lateral
0.8 m deep get one `GROUND_DEPTH` and therefore one ground conductance. An
unlined swale and a lined culvert get one `HYPORHEIC_VELOCITY`. These are not
calibration knobs being over-generalised — they are **the terms that make one
reach behave differently from another**, and collapsing them to a scalar
removes the spatial signal the model exists to produce.

**Reference.** HydroCouple's `RHEComponent` already does this: per-element
`shadeFactor`, `skyViewFactor`, `landCoverEmiss`, `landCoverTemperature` and
a `shadeFactorMultiplier` in its `[ELEMENTS]` table, plus a `[METEOROLOGY]`
section that assigns a variable over an element RANGE as `VALUE` or
`TIMESERIES` (`rhemodelio.cpp` `readInputFileElementsTag`,
`readInputFileMeteorologyTag`). `CSHComponent` carries the same shape for its
met variables. **`ShadeComponent` is an empty stub** (LICENSE + README only) —
there is no reference implementation for *computing* shade from geometry, so
PE5 below is a design, not a transcription.

---

## 1. The architectural obstacle, stated first

```cpp
double netFluxOut(const SimulationContext& ctx, double t_w) noexcept;
double surfaceFluxOut(const SimulationContext& ctx, double t_w) noexcept;
double radiativeFluxOut(const SimulationContext& ctx, double t_w) noexcept;
```

**No flux evaluator takes an element.** Each reads `ctx.heat_config` and
`ctx.climate_state` and returns a number that is by construction the same
everywhere. This is the whole of the problem: the physics beneath is already
parameterised correctly — `netRadiativeFluxOut` takes a `RadiativeConfig&`
and would happily accept a different one per call — but nothing can hand it a
different one because the call site does not know which element it is
evaluating.

Threading identity through touches **five** call sites (the four flux
bindings plus H6b's coupled stepper) and is the bulk of PE1's risk. It is
also the reason this cannot be done attribute-by-attribute as a series of
small changes: the first attribute pays the entire threading cost, and every
one after it is nearly free.

**Consequence for sequencing.** PE1 must land the identity plumbing with
**zero behaviour change** — same globals, byte-identical corpus — and only
then may PE2 begin honouring overrides. A round that does both at once cannot
distinguish a plumbing defect from a physics defect.

## 2. Attribute inventory — what becomes per-element, and what does not

Three questions decide each row: does it vary over a real network; can a
modeller actually supply it; and does it change an answer. An attribute that
fails any of them stays global.

### 2.1 Per-element (PE2 scope)

| Attribute | Section | Why it varies | Strength |
|---|---|---|---|
| `SHADE_FACTOR` | `[RADIATIVE_FLUXES]` | Riparian canopy, buildings, culvert covers | **Headline** |
| `SKY_VIEW` | `[RADIATIVE_FLUXES]` | The complement of the above for longwave | **Headline** |
| `EMISS_LANDCOVER` | `[RADIATIVE_FLUXES]` | Canopy ≈ 0.97 vs concrete ≈ 0.91 | Strong |
| `LANDCOVER_TEMPERATURE` | `[RADIATIVE_FLUXES]` | **NEW FIELD** — see §2.3 | Strong |
| `ALBEDO` | `[RADIATIVE_FLUXES]` | Turbid vs clear, ice cover | Moderate |
| `HYPORHEIC_VELOCITY` | `[SEDIMENT_EXCHANGE]` | Lined pipe = 0; gravel bed ≫ 0 | **Headline** |
| `GROUND_DEPTH` | `[SEDIMENT_EXCHANGE]` | Burial depth: a trunk vs a lateral | **Headline** |
| `GROUND_TEMPERATURE` | `[SEDIMENT_EXCHANGE]` | Follows burial depth and land cover | **Headline** |
| `BED_THICKNESS` | `[SEDIMENT_EXCHANGE]` | Silted invert vs swept invert | Strong |
| `THERMAL_DIFFUSIVITY` | `[SEDIMENT_EXCHANGE]` | Sand vs clay vs concrete bedding | Strong |
| `SOLUTE_DIFFUSIVITY` | `[SEDIMENT_EXCHANGE]` | Same material argument | Moderate |
| `SEDIMENT_DENSITY`, `SEDIMENT_SPECIFIC_HEAT` | `[SEDIMENT_EXCHANGE]` | Same material argument | Moderate |
| `WIND_FUNC_A`, `WIND_FUNC_B` | `[HEAT_FLUXES]` | Sheltered culvert vs open channel | Moderate |

### 2.2 Stays GLOBAL, deliberately

| Attribute | Why |
|---|---|
| `[SOLAR_RADIATION]` latitude / longitude / timezone / Bird atmosphere | Site geometry. A model spanning enough distance to matter is a different model. |
| `[CLOUD_COVER] FRACTION` and its coefficients | One sky. D-H6a-2 made one fraction drive two modules precisely so it could not drift; per-element would reopen that. |
| `ATM_EMISS_COEFF` (Brunt \f$A_a\f$), `ATM_LW_REFLECTION` | Properties of the atmosphere, not the reach. |
| `SHORTWAVE` (all three modes) | **Important design point:** \f$J_{in}\f$ is the incident resource and stays one number; `SHADE_FACTOR` is the per-element *modulator* of it. Making both per-element gives two ways to say one thing, and they will disagree. |
| Air temperature, humidity, wind speed | **Global in the DECK, per-element through the API** — decided 2026-09-01, see §6. There is no deck syntax for per-element met at any phase. |

### 2.3 The new field: land-cover temperature

`RadiativeExchange.cpp` computes the land-cover longwave term from **air
temperature**, and `RadiativeExchange.hpp` records that as a known departure
from the reference — which carries a per-element `landCoverTemperature`. The
consequence is a daytime understatement: a sunlit canopy or a concrete wall
runs well above air temperature and radiates accordingly.

PE2 adds `LANDCOVER_TEMPERATURE` with a **sentinel default meaning "use air
temperature"**, so every existing model is bit-identical and a deck opts in
by naming a value or series. This is the `configured_source[]` pattern from
`[HEAT_SOURCES]`, one struct over: a defaulted value that is indistinguishable
from a deliberate one is how a model comes to look configured when it is not.

## 3. Decisions

**D-PE1 — identity is a token, not an index.** Flux evaluators take a
`HeatElement`:

```cpp
struct HeatElement {
    enum class Kind : std::uint8_t { NODE, LINK, SUBCATCH, LID } kind;
    int index;                       // into the kind's own arrays
};
```

ARD cells and LARD parcels do **not** get their own kind: both resolve to
their parent LINK for attribute lookup. Shading does not vary within a
conduit in any data a modeller can supply, and giving cells their own
attribute row would create a second, finer table nothing can fill. Recorded
so the next person does not "improve" it.

**D-PE2 — resolution is dense-on-demand, materialised at open.** Parsing
produces sparse override rows. `resolveHeatOverrides(ctx)` runs once at open,
after every component has applied, and — **only if at least one override row
exists for that struct** — materialises a dense
`std::vector<RadiativeConfig>` / `std::vector<SedimentConfig>` sized to the
element count, each entry starting as a copy of the global and then patched.

Consequences, each of which is the reason for the choice:
- Lookup in the step loop is `cfg = ov.empty() ? global : ov[i]` — one
  predictable branch, no hashing, no search. **The per-step cost of this
  feature on a model that does not use it is one branch.**
- A model with no overrides allocates nothing and passes the same global
  object it passes today, so byte-identity is structural rather than tested.
- Memory is `sizeof(RadiativeConfig) × n_links` = 72 B × N, ≈ 720 KB at
  10 000 links. Acceptable, and only paid by models that ask.

The rejected alternative — a hash map consulted per element per step — is
slower in the common case and its cost is invisible until a large model.

**D-PE3 — three scopes, most-specific-wins.** `GLOBAL` < `TAG` < element.

```
SHADE_FACTOR   GLOBAL          0.10
SHADE_FACTOR   TAG   RIPARIAN  0.85
SHADE_FACTOR   LINK  C7        0.95
```

`TAG` reads SWMM's own `[TAGS]` section (`ctx.links.tags`,
`ctx.nodes.tags` — already parsed, already round-tripped by `InpWriter`,
already exposed through `swmm_link_get_tag`). It is the answer to "I have
fifty shaded conduits": tag them once.

**This replaces the reference's `<fromElement> <toElement>` range**, which
does not map onto SWMM: a stormwater network is a graph with no linear
element ordering, so "from C3 to C19" names nothing well-defined. Recorded
divergence.

**Precedence must be gated, not assumed.** A deck that sets a value at all
three scopes has exactly one right answer and it is worth a test.

**D-PE4 — a conflicting row at the SAME scope is a parse ERROR, not
last-wins.** Two `LINK C7 SHADE_FACTOR` rows mean the user believes one of
them and cannot be told which won. This follows D-H6a-3 (the mutually
exclusive `SHORTWAVE` spellings) and the `[REACTION_QUALITY]` duplicate
refusal, both of which already refuse rather than repair.

**D-PE5 — unknown element names are FATAL at open.** `LINK C99` where C99
does not exist is a typo, and a silently ignored override is the failure mode
lessons 10/20 are about. Components apply after the full `.inp` parse
(D-RQ1), so `ctx.link_names` is complete and the diagnostic can quote the
row.

**D-PE6 — ranges and validity are enforced per scope, identically.** The
parser already refuses out-of-range fractions at GLOBAL scope
(`HeatComponent.cpp:126-132`) rather than clamping. Per-element rows go
through **the same validator**, called from one place. A validator duplicated
per scope is how a global refusal and a per-element clamp end up in one file.

**D-PE7 — per-element TIMESERIES is PE3, not PE2.** Seasonal shading is a
real requirement and the reference supports it. But it changes the per-step
cost model: every distinct series must be interpolated once per step, and
elements sharing a series must share the interpolation. PE3 adds a
`resolveHeatForcing(ctx)` pass — the `updateSolarForcing` pattern — that
walks the *distinct series* (not the elements), writes the interpolated
value, and patches the dense config columns before any flux call. Doing this
in PE2 would conflate "does the override reach the physics" with "is the
series resolved once per step".

**D-PE8 — save/render lands in the SAME round as the parser.** Lesson 201: a
config struct whose serializer arrives later loses every deck's configuration
on first save. `saveHeatConfig` renders per-element rows grouped by
attribute, GLOBAL first, then TAG, then element rows in index order — a
stable order so `SaveIsIdempotent` and the gen2==gen3 byte-equality gate
(`3e87868e`) both hold.

## 4. Deck surface

`model.heat`, existing sections extended; no new section for the radiative
and sediment attributes, because a second section describing the same
parameters is a second place to look.

```
[RADIATIVE_FLUXES]
SHORTWAVE              GLOBAL COMPUTED
ALBEDO                 GLOBAL 0.06
SHADE_FACTOR           GLOBAL 0.10
SHADE_FACTOR           TAG    RIPARIAN   0.85
SHADE_FACTOR           LINK   C7         0.95
SKY_VIEW               TAG    RIPARIAN   0.20
SKY_VIEW               LINK   C7         0.10
EMISS_LANDCOVER        TAG    CONCRETE   0.91
LANDCOVER_TEMPERATURE  TAG    CONCRETE   28.0      ; else: air temperature

[SEDIMENT_EXCHANGE]
GROUND_TEMPERATURE     GLOBAL 11.5
GROUND_DEPTH           GLOBAL 2.0
GROUND_DEPTH           TAG    TRUNK      4.5
GROUND_DEPTH           TAG    LATERAL    0.8
HYPORHEIC_VELOCITY     GLOBAL 0.0
HYPORHEIC_VELOCITY     TAG    UNLINED    2.0e-5
BED_THICKNESS          LINK   C12        0.40

[HEAT_FLUXES]
SURFACE_EXCHANGE       ON
WIND_FUNC_B            TAG    CULVERT    0.0        ; no wind in a barrel
```

**There is deliberately no `[HEAT_METEOROLOGY]` section, at any phase.** Air
temperature, humidity and wind stay one number per model in the deck; §6 is
where that is argued and where the API alternative lives.

The one-word scope token in position 2 is what makes this parseable without
ambiguity and keeps every row self-describing when read out of context.

## 5. Phases

> **STATUS 2026-09-01: PE1 + PE2 + PE4 VALIDATED AND COMMITTED** (one
> merged round; the check re-bisected it via §1 of the handoff's stub —
> PE1-only corpus 23/23, then the full patch 23/23). See the CHECK RECORD
> in `PE_PER_ELEMENT_HANDOFF_2026-09-01.md` for the six fixes (two REAL:
> both of the parsed-but-never-read family) and the A/B-rig repair
> (roadmap lesson 222). PE3 (per-element TIMESERIES), PE5 (computed
> shading — recommendation stands: don't build), PE6 (API + GUI table)
> and the wind-function coefficients remain OPEN.

### PE1 — identity plumbing, zero behaviour change

- `HeatElement` in `HeatData.hpp`; `netFluxOut`, `surfaceFluxOut`,
  `radiativeFluxOut` gain it as their first parameter.
- All five call sites pass a real token: `HeatFluxes.cpp` (nodes, links),
  `ArdEngine.cpp` (cells → parent link, node stores), `HeatWatershed.cpp`
  (subareas), `LagrangianSolver.cpp::applyFluxesAndBed` (nodes, links), and
  H6b's `relaxPair` call sites, which compute `j0`/`j1` and must compute them
  for the right element.
- Accessors `radiativeFor(ctx, elem)` / `sedimentFor(ctx, elem)` exist and
  **return the global unconditionally** — the override vectors do not exist
  yet.
- **Gate:** corpus 23/23 byte-identical, full suite green. This phase's only
  claim is that it changed nothing, and the corpus is the instrument for
  exactly that.
- **Falsifier:** make one binding pass a constant `HeatElement{LINK, 0}`
  instead of the real one. PE1's gates should NOT notice (everything returns
  the global) — which is precisely why PE1 cannot also land the physics, and
  PE2's gates must catch it.

### PE2 — per-element constants (the substance)

- `HeatOverrides` in `BedZoneData.hpp`/`HeatData.hpp`: sparse parsed rows +
  the dense materialised vectors + `resolveHeatOverrides(ctx)`.
- Parser: scope token, TAG expansion, D-PE4 duplicate refusal, D-PE5 unknown
  name fatality, D-PE6 shared validator.
- `LANDCOVER_TEMPERATURE` with the "use air" sentinel (§2.3).
- Accessors start returning per-element configs.
- `saveHeatConfig` renders every scope (D-PE8) + size pins on both structs.
- **Gates** (each must fail at base):
  1. Two conduits, identical but for `SHADE_FACTOR` (0.0 vs 0.95), sunlit
     deck → they must reach *different* temperatures, and the shaded one must
     be cooler. At base they are identical to the bit.
  2. Precedence: one attribute set at GLOBAL, TAG and LINK; the LINK element
     takes the LINK value, a tagged-but-unnamed element takes the TAG value,
     an untagged element takes GLOBAL. **One deck, three assertions** — this
     is D-PE3's whole content.
  3. `GROUND_DEPTH` per tag → a shallow-buried lateral and a deep trunk with
     identical water reach different temperatures via the ground conductance.
  4. `HYPORHEIC_VELOCITY` per tag → the unlined reach exchanges, the lined
     one does not, on the same deck.
  5. Duplicate row at the same scope → parse error quoting both rows.
  6. `LINK C99` (nonexistent) → parse error quoting the row.
  7. Round-trip: save/reopen preserves every scope; gen2 == gen3.
  8. **The no-override deck is byte-identical** — the structural claim of
     D-PE2, asserted rather than assumed.
- **Falsifiers:** (i) make `radiativeFor` ignore the override vector → gates
  1–4 fail, 5–8 pass; (ii) invert the precedence order → gate 2 alone fails;
  (iii) drop the TAG expansion → gates 2–4 fail, 1 passes (it uses LINK
  scope), which is what distinguishes the two mechanisms; (iv) PE1's constant
  -`HeatElement` falsifier → **must now fail**, closing PE1's blind spot.

### PE3 — per-element TIMESERIES (D-PE7)

`resolveHeatForcing(ctx)` before any flux call, walking distinct series.
Gate: two elements sharing one series interpolate identically and the series
is evaluated **once** (instrument the lookup count — the claim is about cost,
so the gate must measure cost, not just correctness).

### PE4 — per-element climate through the API (§6)

Four new `SWMM_ForcingType` channels on the EXISTING forcing API, plus
`sedimentFor`-style resolution in the flux evaluators. No parser, no
serializer, no deck surface. Design in §6.3.

### PE5 — computed shading (stretch, design not transcription)

Shade from sun position (already computed, H6a) plus per-element bank
vegetation height/offset and reach azimuth — the classic Chen/Boyd stream
shade formulation. `ShadeComponent` is an empty stub so there is nothing to
port. **Gate the decision, not the code:** this is only worth building if a
user has vegetation geometry, which most stormwater models do not. Prescribed
`SHADE_FACTOR` with a `TIMESERIES` (PE3) covers the seasonal case at a
fraction of the cost.

### PE6 — API + GUI

`swmm_heat_*_scoped` getters/setters following `swmm_heat_set_node_override`'s
shape; G4g's Heat Configuration dialog gains a per-element table with a scope
column. Blocked on PE2 only.

## 6. Climate: global in the deck, per-element through the API

**Decided 2026-09-01 (user).** Air temperature, relative humidity and wind
speed remain **one value per model in every deck**, and become
per-element **only** through the runtime API. This resolves the open question
the first draft of this plan carried, and it resolves it better than any of
the three options that draft offered.

### 6.1 Why the split is the right shape

`ClimateState` is read by hydrology, snowmelt and evaporation as well as by
heat transport. A *deck* that could set a per-conduit air temperature would
be asserting something about the atmosphere that the snowpack on the
subcatchment above disagrees with, permanently and invisibly, in a file the
user maintains by hand. Keeping the deck global keeps one atmosphere per
model — which is the honest statement for a stormwater network.

The API is a different contract. A caller pushing per-element air temperature
is a **coupled driver** — an MCP session, a calibration loop, or a
HydroCouple composition where an atmospheric or riparian-shade model owns
the near-surface climate field and hands it over element by element. That
caller is asserting the heterogeneity deliberately, on a per-step basis, and
can be held to it. This is exactly how the reference works: `RHEComponent`
and `CSHComponent` receive per-element meteorology through **exchange
items**, not through their input files.

It also means per-element climate costs nothing in the deck, the writer, the
round-trip gates or the GUI — three of the five phases of work that a deck
surface would have required simply do not exist.

### 6.2 The scope restriction that makes it safe

**Per-element climate forcing is accepted for LINK and NODE, and REFUSED for
SUBCATCH.**

The reason is geometric rather than defensive. A conduit or a node has **no
competing consumer** for air temperature: snowmelt runs on subcatchments,
evaporation on subcatchment and storage surfaces. Setting a per-link air
temperature can only reach heat transport, so no divergence between
subsystems is possible. A per-*subcatchment* air temperature would reach
snowmelt and evaporation too, and that is a distributed-climate feature
wearing a heat-transport hat — the third option of the original §6, and still
out of scope.

`SWMM_ERR_BADPARAM` with a message naming the reason, not silence.

### 6.3 API design — extend the existing forcing family, do not invent one

`openswmm_forcing.h` already carries the whole vocabulary this needs:
`SWMM_ForcingMode` (`OVERRIDE`/`ADD`), `SWMM_ForcingPersist`
(`RESET`/`PERSIST`), a `SWMM_ForcingType` channel enum, `swmm_forcing_clear`
and `swmm_forcing_clear_all`. Three climate channels already exist and are
documented as *"System-wide; idx ignored"*.

The per-element channels are the same idea with the index honoured:

```c
SWMM_FORCE_ELEM_AIR_TEMPERATURE = 13,  /* idx = element; scope by kind */
SWMM_FORCE_ELEM_HUMIDITY        = 14,
SWMM_FORCE_ELEM_WIND_SPEED      = 15,
SWMM_FORCE_ELEM_SHORTWAVE       = 16,  /* Jin, W/m2 — see 6.5 */

int swmm_forcing_element_climate(SWMM_Engine engine,
                                 int kind,      /* SWMM_HeatElemKind */
                                 int index,
                                 int variable,  /* the channel above */
                                 double value,
                                 int mode,      /* OVERRIDE | ADD */
                                 int persist);  /* RESET | PERSIST */

int swmm_forcing_element_climate_get(SWMM_Engine engine, int kind, int index,
                                     int variable, double* value,
                                     int* mode);
```

Reusing `mode` and `persist` is not economy for its own sake — it is what
makes the semantics already-decided and already-tested:

- **`ADD` is the coupled-driver mode that matters.** A riparian model that
  knows a reach is 2 °C cooler than the gauge pushes `ADD -2.0` and never has
  to know the gauge value. `OVERRIDE` replaces it outright.
- **`RESET` is the correct DEFAULT for a coupled driver, and the plan says
  so loudly.** Under `PERSIST`, a driver that pushes on some steps and not
  others silently reuses a stale field — the value looks like data and is
  hours old. Under `RESET` the field falls back to the global broadcast the
  moment the driver stops feeding it, which is both visible and recoverable.
  `PERSIST` remains available for the calibration case where one constant
  offset applies for a whole run.
- `swmm_forcing_clear(engine, SWMM_FORCE_ELEM_AIR_TEMPERATURE, idx)` and
  `clear_all` already do the right thing.

### 6.4 Storage and resolution

`ForcingData` gains, per variable, a **mode vector and a value vector sized
lazily on first use** — the D-PE2 dense-on-demand pattern again, and for the
same reason: a model that never calls these allocates nothing and the
per-step cost is one `.empty()` check.

Resolution happens exactly where the global forcing already resolves. The
existing `ForcingData` has a `resolve*` helper taking the broadcast value and
returning the effective one; the per-element form takes the element token
too:

```cpp
double effectiveAirTempF(const HeatElement& e, double broadcast) const;
```

`surfaceFluxOut(ctx, elem, t_w)` — which PE1 has already given the element —
calls it instead of reading `ctx.climate_state.temperature` directly. **This
is the payoff of PE1's threading**: per-element climate is a change to the
three lines that read climate state, not to any binding.

`resetPerStep()` already clears `RESET`-mode global forcings after each
step; the per-element vectors join that same sweep, so the reset semantics
cannot drift between the two.

### 6.5 Shortwave through the API — the one addition beyond met

`SWMM_FORCE_ELEM_SHORTWAVE` is included because it is what a coupled shade
model actually produces. `ShadeComponent` is an empty stub, so PE5's computed
shading may never be built in-tree — but an external model that computes
per-element insolation can push \f$J_{in}\f$ directly, which makes PE5
optional rather than owed.

This does **not** contradict §2.2's rule that `SHORTWAVE` stays global in the
deck. The deck states one incident resource and per-element `SHADE_FACTOR`
modulates it; the API lets a driver replace the resolved per-element value
outright. The precedence is stated once, in the resolver, and gated:
**API element forcing > deck per-element shade applied to the global
\f$J_{in}\f$ > global \f$J_{in}\f$.**

### 6.6 Gates for PE4

1. Two conduits on one deck; `ADD -5 °C` on one → they diverge, and the
   untouched one matches a no-forcing control **exactly**.
2. `RESET` semantics: push on step 1 only; step 2 must read the global
   broadcast, not the pushed value. **This is the gate that protects against
   the stale-field failure mode**, and it is the one most likely to be
   omitted because everything "works" without it.
3. `PERSIST` semantics: push once, unchanged for the rest of the run, cleared
   by `swmm_forcing_clear`.
4. `SUBCATCH` kind → `SWMM_ERR_BADPARAM`, and the message names the snowmelt
   reason (§6.2).
5. Snowmelt and evaporation are **unaffected** by a per-link push: a deck
   with a snowpack and a per-link air-temperature override produces the same
   snow results as the control. This is §6.2's whole claim and nothing else
   asserts it.
6. No-forcing byte-identity: the corpus is untouched, structurally (empty
   vectors) rather than by tolerance.

**Falsifier:** make `effectiveAirTempF` ignore the per-element vector →
gates 1–3 fail, 4–6 pass. And separately: wire the per-element value into
`ClimateState` itself instead of resolving at the flux call → **gate 5
fails**, which is the entire point of resolving late.

## 7. Risks

1. **The threading touches every binding.** Five call sites, four of which
   have their own geometry. Mitigation: PE1 is a separate round whose only
   gate is "nothing moved".
2. **A silently-ignored override.** The whole feature fails soft: an override
   that never reaches the physics produces a plausible answer. Mitigation:
   D-PE5's fatality, plus PE2 falsifier (i) which exists to prove the
   overrides are load-bearing.
3. **Deck-size explosion.** Per-link rows on a 20 000-link model make an
   unreadable file. Mitigation: TAG scope is the intended spelling and the
   manual should say so; per-element rows are for exceptions.
4. **`SHADE_FACTOR` and `SKY_VIEW` are independent keys that users set
   inconsistently** — already recorded in Chapter 9. Per-element multiplies
   the opportunity. Mitigation: a warning at open when an element sets one
   and not the other, since they describe the same obstruction from two
   sides.
5. **A stale per-element climate field under `PERSIST`.** The failure mode
   is silent and looks like data. Mitigation: `RESET` is documented as the
   default for coupled drivers, and §6.6 gate 2 exists for exactly this.
6. **Interaction with H6b's coupled stepper.** `relaxPair` receives `j0`/`j1`
   already computed; if the element token is threaded to the flux call but
   not to `bedCouplingFromContact`, the surface terms become per-element
   while the bed terms silently stay global. Mitigation: PE2 gate 3 (per-tag
   `GROUND_DEPTH`) exists specifically to catch that half-wiring.

## 8. Cost

| Phase | Rounds | Note |
|---|---|---|
| PE1 | 1 | Mechanical; the risk is breadth, not depth |
| PE2 | 2 | Parser + resolution + save + 8 gates |
| PE3 | 1 | |
| PE4 | 1 | API-only; no parser, no serializer, no GUI |
| PE5 | 2–3 | Stretch; may never be worth it |
| PE6 | 1 engine + 1 GUI | |

PE1+PE2 is the payload; everything after it is elaboration. **PE4 is
cheaper than the original plan costed it** — the deck-side work vanished with
§6's decision, and what remains is four enum values and one resolver that
PE1's threading has already made reachable.
