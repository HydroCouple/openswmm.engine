# Heat Transport Plan (HydroCouple CSH/HTS/RHE-Guided)

**Status:** Approved direction, 2026-08-12
**Parent:** `plans/transport/UNIFIED_TRANSPORT_MASTER_PLAN.md` (Phase T4; decision D-UT5)
**Reference implementation:** HydroCouple stream temperature stack — Buahin,
Neilson & Horsburgh (2019), *Channel and Sub-Surface Solute and Heat Transport
Modeling Using the HydroCouple Component-Based Modeling Framework*:
CSHComponent (§4), HTSComponent (§5), RHEComponent (§6), GWComponent (§7).
Repos under `/Users/calebbuahin/Documents/Projects/HydroCouple/{CSHComponent,
HTSComponent,STSComponent,RHEComponent,GWComponent,SolarRadiationModule,ShadeComponent}`.
**2D/GW heat:** `plans/transport/TWOD_TRANSPORT_PLAN.md` §4.5, §5.

---

## 1. Formulation (1D network)

Temperature is the reserved species `__TEMPERATURE__`
(`RESERVED_TEMPERATURE`), transported by whichever quality engine is active,
with heat source terms. Governing equation per CSHComponent eq. 4.1:

```
ρw cp ∂T/∂t = −ρw cp ∂(vT)/∂x + ρw cp ∂/∂x( D ∂T/∂x )
              + Σ J / Y  −  (Je + Jc) / Y  +  Σ S
```

where J = radiative fluxes at the water surface (W/m²), Je =
latent (evaporation/condensation), Jc = sensible (convection/conduction),
Y = depth, S = volumetric sources (J/m³/s: sediment, GW, HTS, user).

The transport operator (advection + dispersion + junction mixing) is
exactly the Eulerian ARD / LARD machinery — no separate heat solver. Note
precedence D-UT7 (master plan): CSHComponent guides the **physics** here
(flux formulations in §2, Fischer dispersion coefficient model), while the
transport **numerics** are the FV-kernel set of the Eulerian ARD engine
(HLLC-consistent upwinding, MUSCL/QUICKEST + FCT, implicit dispersion) —
CSH's scheme menu and per-element ODE solvers are not carried over.
Energy bookkeeping uses ρw cp (options `WATER_DENSITY`,
`WATER_SPECIFIC_HEAT_CAPACITY`, defaults 1000 kg/m³, 4184 J/kg/°C as in
CSHComponent Table 4.1).

## 2. Heat flux modules

Implemented as HydroCouple `IModelComponent`s under
`src/engine/transport/components/HeatFluxModules/` (master plan §3.2), each
independently toggleable, all summing into the per-element source term.

### 2.1 SurfaceExchange (latent + sensible) — CSH §4.4–4.5

- `Je = ρw Le E` with `Le = 1000(2499 − 2.36T)` (Martin & McCutcheon 1998).
- Mass-transfer evaporative rate `E = f(w)(e_s^w − e_a)` (Dingman 2008);
  `e_s^w = 0.61275 exp(17.27T/(237.3+T))`; `e_a = (H/100) e_s(Ta)`.
- Wind function `f(w) = a + b·w` — defaults a = 1.505e-8, b = 1.6e-8
  (Dunne & Leopold 1978), user-overridable
  (`WIND_FUNC_COEFF_A/B` options as in CSH Table 4.1).
- Sensible via Bowen ratio: `Jc = Br·Je`,
  `Br = CB (Pa/P) (T − Ta)/(e_s^w − e_a)`, `CB = 0.061 kPa/°C`,
  `PRESSURE_RATIO` option for elevation (CSH §4.5).
- Evaporated volume is **not** removed from hydraulics by this module in
  v1 (hydraulic evaporation stays with the existing climate machinery);
  only heat is exchanged. Revisit coupling after G-H4.

### 2.2 RadiativeExchange — RHE §6

- Net shortwave: `Jsn = (1 − Rs) Jin max(0, 1 − fs)` with albedo Rs and
  shade fraction fs. **GLOBAL scope**, like every other key in this
  section — per-element ranges (and a ShadeComponent-style calculator
  behind fs) are §7 work, not shipped.
- Longwave: back radiation `Jbr = εw σ Tw⁴`; atmospheric
  `Jan = εatm σ Ta⁴ (1 − RL) fsky` with Brunt (1932)
  `εatm = Aa + Ab√(e_a · 1000)`, e_a in kPa so the ×1000 makes it
  **PASCALS** (Aa 0.5–0.7, Ab ≈ 0.0027); land cover
  `Jlc = εlc (1 − fsky) σ Ta⁴` with sky-view factor fsky.
  **H6a multiplies `εatm` by a cloud factor** — see §2.5.

  > **The `fsky` on `Jan` and the ×1000 under the root are corrections**,
  > folded in here 2026-08-30. This section carried the uncorrected forms
  > from 2026-08-12 until then, while the code has been right since H3:
  > `fsky` splits one hemisphere between `Jan` and `Jlc` rather than
  > scaling them independently, and Brunt's root takes Pascals — in kPa the
  > term is understated by √1000 ≈ 31.6, landing on ~0.502 against a
  > correct 0.567, *which is still a plausible emissivity*. Both were found
  > by reading `RHEComponent/src/element.cpp:119-129` rather than this
  > summary, and are recorded in `RadiativeExchange.hpp` and
  > `H3_VALIDATION_HANDOFF_2026-08-17.md` §2. Gates 2 and 3 of
  > `test_heat_radiative_exchange.cpp` exist for them.
- Incoming shortwave Jin — **§2.5**. H3 ships only the constant; the
  timeseries and computed paths are H6a.

### 2.3 SedimentExchange (optional) — HTS §5

Two-layer sediment/hyporheic storage per element (HTSComponent eq. 5.1):

```
ρsed cp,sed dT_HTS/dt · V = ρsed cp,sed αsed B Δx (T_ch − T_HTS)/Y_HTS
                          + ρsed cp,sed αsed B Δx (T_gr − T_HTS)/Y_gr
                          + ρw cp Q_HTS (T_ch − T_HTS) + Σ S
```

with conductive exchange to channel and ground zones plus advective
hyporheic exchange coefficient Q_HTS. Ground-zone temperature T_gr fixed or
timeseries in v1 (GW coupling replaces it in Phase T7). Solute analogue
(diffusive + advective HTS exchange for species) ships with it — matches
HTSComponent, and provides transient-storage capability for tracers.

### 2.4 Met forcing

Air temperature, wind speed: already in the climate state
(`ctx_.climate_state`, `src/engine/hydrology/Climate.cpp`). Additions:
relative humidity and incoming shortwave as first-class climate inputs
(`[TEMPERATURE]`-section adjacent `[EVAPORATION]`-style extensions +
runtime forcing API `forcing_set_climate_*` parity), per-element overrides
via `[HEAT_METEOROLOGY]` (CSH `[METEOROLOGY]` semantics: variable,
element range, VALUE|TIMESERIES).

**H6a adds cloud fraction `C ∈ [0,1]`** as a climate input on the same
footing (constant or timeseries). It belongs here and not in
`RadiativeConfig` because one fraction drives **two** modules — shortwave
attenuation and the longwave emissivity correction (§2.5) — and a parameter
that two modules read from separate copies is free to drift. D-H5e is the
nearest precedent in kind, though not in mechanism: there the duplication was
two relaxation call sites, here it would be two config copies.

### 2.5 SolarRadiation — incoming shortwave Jin (H6a)

Three ways a deck supplies `Jin`, and they are **mutually exclusive by parse
error, not by precedence** (D-H6a-3). A deck naming more than one is
refused, the same way `[RADIATIVE_FLUXES]` already refuses an out-of-range
fraction instead of clamping it (`HeatComponent.cpp:126-132`).

1. `SHORTWAVE GLOBAL <W/m²>` — the H3 constant. Unchanged, still the default.
2. `SHORTWAVE GLOBAL TIMESERIES <name>` — a measured pyranometer record,
   interpolated on the routing clock through the existing timeseries
   machinery. This is the same lookup `[HEAT_SOURCES] TIMESERIES` is
   currently deferred to (`HeatComponent.cpp:265-270`); H6a is where that
   deferral is paid for shortwave, and the two should land on one helper.
3. `SHORTWAVE GLOBAL COMPUTED` — solar position + clear-sky + cloud, below.

#### Solar position — NREL SPA

Reda & Andreas, *Solar Position Algorithm for Solar Radiation Applications*,
NREL/TP-560-34302. Chosen over a port of HydroCouple's
`SolarRadiationModule` for one reason specific to this program: SPA ships
**published test vectors** (the paper's worked example; stated ±0.0003°
over 2000 BC–6000 AD). That gives this module the external parity reference
that conduction never had — its gates assert against NREL's numbers rather
than against this engine's own output, which is the weakness recorded for
H5b in `PROGRESS.md` §2.5.

**What the engine already has.** Unlike D-H5b, this is not new physics
without precedent — the pieces are present and must be reused rather than
duplicated:

| Need | Already in tree |
|---|---|
| Latitude | `ClimateState::latitude` (deg), set from `ctx_.options.snow_lat` (`SWMMEngine.cpp:6370`) |
| Solar-time correction | `ClimateState::dtlong` (hrs), from `snow_dtlong` minutes (`SWMMEngine.cpp:6379`) |
| Site elevation → pressure | `ClimateState::elev` (ft); the psychrometric constant already derives pressure from it |
| Declination, hour angle, `Ra` | `hargreaves()` (`Climate.cpp:67-102`) |
| Sunrise/sunset, day length | `updateTempTimes` → `hrsr/hrss/hrday/dhrdy/dydif` (`Climate.cpp:178-206`) |

**Do not fold `hargreaves` and SPA into one path.** The ET path is
parity-locked against legacy SWMM; SPA supersedes it *for this module only*.
Sharing a declination routine between them is how a heat change becomes an
evaporation regression.

**⚠ Trap — latitude is a SNOWMELT field.** `snow_lat` defaults to 0 and is
written only by decks carrying a `[TEMPERATURE]` SNOWMELT line. A deck
asking for `COMPUTED` without one would silently model equatorial noon.
The COMPUTED branch must therefore **require** latitude and longitude
explicitly and error when absent — never default. Note this is *stricter*
than D-H5c, which does default (to HOLD) when omitted and errors only on a
bad spelling: a dry-element policy has a defensible default, an unstated
latitude does not. The governing rule is the silent-bypass one (lessons
10/20) — every configuration in which a table reaches nothing says so.

**⚠ Trap — `dtlong` is not a longitude.** It is a correction in *minutes*
carrying a sentinel: 0 means "use true solar time", which is a different
statement from "longitude 0". Deriving a longitude from it is wrong in both
directions. H6a adds explicit `LATITUDE` / `LONGITUDE` / `TIMEZONE` keys
rather than overloading the snowmelt pair, and leaves `snow_lat`/`dtlong`
alone.

**⚠ Cost.** SPA is roughly two orders of magnitude more expensive than the
declination one-liner already in `hargreaves`. It is affordable **only
because shortwave is GLOBAL scope**: compute once per routing step, cache on
the step, never per element. If per-element ranges arrive later (§7), this
becomes a per-step position plus a per-element geometry term — not a
per-element SPA call.

#### Clear-sky — Bird & Hulstrom

Bird & Hulstrom (1981), direct + diffuse on a horizontal surface from air
mass, with turbidity, precipitable water, ozone and ground-albedo
parameters. Defaults from the paper's standard atmosphere, each
user-overridable in the RHE/CSH manner. Bird also publishes tabulated
output — a second external gate, on the same footing as SPA's.

#### Cloud cover — modulates shortwave AND longwave (D-H6a-2)

- **Shortwave:** `Jin = Jclear (1 − k C^n)`, Kasten–Czeplak form;
  defaults k = 0.75, n = 3.4.
- **Longwave:** cloudiness raises atmospheric emissivity,
  `εatm = εatm,clear (1 + k_lw C²)` (Bolz), k_lw ≈ 0.17, applied **inside**
  `atmosphericEmissivity` so there is one place that knows the correction —
  the §6.3 rule.

**⚠ This reaches into the H3-validated longwave path.** `atmosphericEmissivity`
is currently gated against RHE for identical forcing. Two obligations
follow, and they are H6a's own regression gate:

1. The cloud factor must reduce to **exactly** identity at `C = 0` — not
   approximately; the multiply must be skipped or provably `×1.0`.
2. The `heat_parity` corpus deck must come back **byte-identical** with
   cloud unconfigured. If it moves, the H3 baseline is gone and there is no
   reference left to re-establish it from.

#### What this phase does NOT do

SolarRadiation produces `Jin` in W/m² and hands it to H3's existing
`netShortwave`. It adds **no flux family**, touches no sign convention, and
introduces no new element state. That is precisely why it is separable from
the sediment layer: H6b forces the node/link merge decision and this does
not.

## 3. Sources, boundaries, watershed and LID temperature

- Source attribution (master plan §4.3) carries enthalpy
  `ρw cp V T_source`. **Per D-UT10 this is a PARALLEL accumulator, not a
  fourth tuple member**: a `node_enthalpy_in` rate in the heat state struct,
  filled by the same five loaders plus external inflows, mirroring
  `water_age_state.node_age_vol_in` exactly (A1a, `7c322a6c`). Sources:
  DWF temperature (pattern-able), GW temperature
  (subcatchment aquifer state or fixed), RDII temperature, external
  inflow temperature, interface files. INP: `[HEAT_SOURCES]` mirroring
  `[WATER_AGE_SOURCES]` layout; runoff temperature from a subcatchment
  surface-temperature state (below). Direct heat injection (J/s/m over an
  element range) via `[TRANSPORT_SOURCES] ... HEAT` — CSH `[SOURCES]`
  convention.
- **Watershed:** per-subcatchment runoff temperature state — rainfall
  enters at wet-bulb or user temperature (`RAINFALL_TEMP` option),
  equilibrates toward air temperature over ponded storage with a
  first-order exchange (surface energy balance stretch goal H6); snowmelt
  enters at 0 °C by default. Reuses the §4.3 tuple path into nodes.
- **LID:** per-layer temperature states on the generic per-layer species
  block introduced by the age plan (§4 there); conductive relaxation
  toward soil temperature + advective inter-layer transport. Drain
  outflow delivers its temperature to the receiving node.
- Boundary conditions: `[TRANSPORT_BOUNDARIES] ... TEMPERATURE`
  (VALUE|TIMESERIES per node), tidal outfall reverse-flow temperature.

## 4. Engine bindings

| Engine | Mechanism |
|---|---|
| LEGACY qualroute | T as CSTR-mixed species + flux modules applied per node/link volume (coarse; documented) |
| Eulerian ARD | full eq. 4.1 per cell on the FV-kernel mesh under all routing models (reference configuration; engine plan rev. 2) |
| LARD | T as species on segments; flux modules per segment |
| 2D / GW | `TWOD_TRANSPORT_PLAN.md` |

Reactions may reference `__TEMPERATURE__` (Arrhenius kinetics) — reaction
module pre-registers it (reactions plan §2).

## 5. Options and API

```
[OPTIONS]  HEAT_TRANSPORT ON|OFF*            ; legacy .inp — coarse toggle only (D-UT8)

; --- heat component config file (model.heat), registered via [PROCESS_COMPONENTS]:
[HEAT_OPTIONS]  EVAPORATION YES|NO  CONVECTION YES|NO  RADIATION YES|NO
           SEDIMENT_EXCHANGE YES|NO  WATER_DENSITY  WATER_SPECIFIC_HEAT_CAPACITY
           WIND_FUNC_COEFF_A/B  BOWENS_COEFF
           PRESSURE_RATIO  ALBEDO  EMISSIVITY_WATER  BRUNT_A/B  SKY_VIEW
[HEAT_METEOROLOGY] / [HEAT_SOURCES] / [RADIATIVE_FLUXES] / [SEDIMENT_EXCHANGE]

; --- H6a (§2.5). SHORTWAVE takes exactly ONE of three spellings:
[RADIATIVE_FLUXES]  SHORTWAVE GLOBAL 250.0              ; constant (H3)
                    SHORTWAVE GLOBAL TIMESERIES sw_ts   ; measured
                    SHORTWAVE GLOBAL COMPUTED           ; SPA + Bird + cloud
; naming two of them is a parse ERROR, not a precedence question (D-H6a-3)

; Every row below is '<param> GLOBAL <value>', matching [RADIATIVE_FLUXES].
[SOLAR_RADIATION]   ; consulted ONLY under SHORTWAVE ... COMPUTED
           LATITUDE   GLOBAL  41.7      ; REQUIRED, no default. NOT the
           LONGITUDE  GLOBAL -111.8     ; [TEMPERATURE] SNOWMELT pair —
           TIMEZONE   GLOBAL -7.0       ; §2.5 traps 1 and 2
           ELEVATION  GLOBAL  1400      ; metres; omit to use climate elev
           TURBIDITY_380 / TURBIDITY_500 / PRECIP_WATER / OZONE /
           GROUND_ALBEDO                ; Bird atmosphere; paper defaults
[CLOUD_COVER]
           FRACTION   GLOBAL  0.5                  ; a FRACTION, not a %
           FRACTION   GLOBAL  TIMESERIES cloud_ts  ; ...or a series
           SW_ATTEN_K / SW_ATTEN_N / LW_CLOUD_K    ; 0.75 / 3.4 / 0.17
```

Out-of-range values are **refused, not clamped**, in both the parser and
`openswmm_heat.h` — two entry points into one configuration that disagree
about what is legal is how a deck and a GUI come to describe different
models.

Placement per `TRANSPORT_IO_PLUGIN_CONFIG_PLAN.md` §3.2 — the CSH-style
sections live in `model.heat`, keeping the legacy `.inp` clean; the same
file drives the flux modules under HydroCoupleComposer.

```python
sim.options.heat_transport = True
ht = sim.heat
ht.evaporation = True; ht.radiation = True
ht.set_met(element_range=("C1","C40"), variable="RELATIVE_HUMIDITY",
           timeseries="rh_ts")
ht.set_radiation(element_range=("C1","C40"), timeseries="sw_ts")
ht.set_solar(latitude=41.7, longitude=-111.8, timezone=-7)   # H6a
ht.set_cloud_cover(timeseries="cloud_ts")                    # H6a
ht.set_source_temperature("DWF", node="N12", value=16.0)
ht.set_sediment(element_range=("C1","C40"), depth=0.3,
                thermal_diffusivity=4.5e-7, advection_coeff=1e-4)
ht.link_temperature("C7"); ht.node_temperature("N12")
```

C API `openswmm_heat.h`; MCP `heat_*`; `.out` gains a temperature variable
(and per-element sidecar under `TRANSPORT_DETAILED_OUTPUT`).

**GUI:** heat options group on the "Quality & Transport" page; met forcing,
radiation, source-temperature, and sediment tables in `HeatTransportDialog`;
Climatology dialog gains RH/shortwave sub-sections when §2.4 lands — see
`openswmm.gui/workplans/TRANSPORT_QUALITY_GUI_PLAN_2026-08-12.md` §1, §3.5.
Temperature surfaces in results pickers via the dynamic result descriptors
(that plan, D-G1).

## 6. Implementation phases

```
H1  __TEMPERATURE__ registry species + the enthalpy ACCUMULATOR through all
    loaders (D-UT10 — parallel to qual_mass_in, mirroring the age channel;
    this absorbs what the roadmap called T0b) + LEGACY engine transport of T
    (no fluxes) + reporting.
    → verify: conservative T mixing analytics (two-inflow junction).
H2  SurfaceExchange module (Je, Jc) + met forcing plumbing (RH, wind).
    → verify: nighttime cooling column test vs closed-form; Bowen ratio
      unit tests vs CSH published values.
H3  RadiativeExchange module (Jsn, Jan, Jbr, Jlc).
    → verify: energy-balance closure on a static pool; component-level
      parity vs RHEComponent outputs for identical forcing (golden files
      generated from the HydroCouple repo).
H4  Eulerian ARD binding — full CSH eq. 4.1; Fischer dispersion for heat.
    → verify: G-UT3 — reproduce a CSHComponent validation case
      (advected diurnal temperature wave) within documented tolerance.
H5a Watershed temperature — per-SUBAREA state (the A3 mirror) + run-on
    carrying temperature + SURFACE ENERGY BALANCE on ponded subareas
    (D-H5a). DWF/GW/RDII source temperatures are ALREADY DELIVERED by H1
    (all seven `HeatSource` pathways are consumed at the loader seam in
    `QualityRouting.cpp` — verified, not assumed).
    GW temperature state rides the two-zone PER_SUBCATCH column (master
    plan G-track, G1) if landed; otherwise fixed GW source temperature
    only — no interim aquifer-temperature structure built twice.
    → verify: runoff temperature equilibration test (now REACHABLE — see
      D-H5a); dry-subarea policy gate (D-H5c).
H5b LID layer temperature — `LidSpecies::TEMPERATURE = 1` on A4's existing
    per-(unit, layer, species) block + VERTICAL CONDUCTION between layers
    (D-H5b) + the drain leaving at the storage temperature, retiring the
    `HeatSource::RAINFALL` "(and LID drains until H5)" marker.
    → verify: LID column conduction test (now has a term to test).
H6a Incoming shortwave forcing (§2.5, D-H6a): Spencer/NOAA solar position
    (SPA behind the swap point — D-H6a-4 as amended) + Bird clear-sky +
    cloud-cover parameterization on BOTH shortwave and longwave +
    `SHORTWAVE ... TIMESERIES`, plus `openswmm_heat.h` (the G4g blocker).
    Adds NO flux family — it feeds `Jin` into H3's existing `netShortwave`
    — so it does NOT force the node/link merge decision and is independent
    of H6b.
    → verify: (a) declination against the ASTRONOMICAL cardinal points
      (±23.44° at solstices, ~0° at equinoxes) and against the engine's own
      `Climate.cpp:180` fit where that fit is trustworthy; (b) Bird against
      the 1981 paper's tabulated output — **pinned but NOT yet re-derived,
      handoff §2**; (c) the three SHORTWAVE spellings are mutually
      exclusive — a two-spelling deck errors; (d) COMPUTED without
      LATITUDE/LONGITUDE errors rather than defaulting; (e) **C = 0 reduces
      to H3 exactly — `heat_parity` deck byte-identical with cloud
      unconfigured**; (f) the C API refuses exactly what the parser
      refuses, and a refused write does not half-apply.
H6b Stretch: SedimentExchange (HTS), and with it H3's deliberately-omitted
    shortwave bed split (`exp(−extinction·depth)`), which belongs with the
    state that receives the energy. Shade factors. **Adds a fifth flux
    family — this is the deadline on the node/link merge decision.**
    **Surface energy balance on subcatchments MOVED to H5a** by D-H5a.
H7  LARD + FV bindings (with T5), API/MCP/docs/parity registries.
```

## 6.1 H5 scoping decisions (user, 2026-08-19)

The H5 line as originally written was internally inconsistent: its two verify
criteria — a *runoff temperature equilibration* test and a *LID column
conduction* test — both presuppose mechanisms the scope line did not list and
H6 deferred. Neither criterion could have been met by the phase as scoped.
Raised before implementation; resolved as follows.

**D-H5a — surface energy balance comes FORWARD from H6 into H5a.**
Without it a ponded subarea holds the rain temperature forever and never
equilibrates, so the plan's own H5 criterion was unreachable. H2's and H3's
flux modules are reused unchanged; only the *binding* is new.

*Precedent to follow, and the trap in it:* heat exchanges exactly where
evaporation does. On a subcatchment that is the **ponded subarea**, whose
area is `RunoffSoA::area × fraction` — and `RunoffSoA::area` is the
subcatchment area **minus the LID footprint** (`Runoff.cpp:197-199`), not
`ctx.subcatches.area`. Using the latter double-counts the LID footprint,
which H5b then exchanges over again. Fractions are
`f0 = fi·pctZero`, `f1 = fi·(1−pctZero)`, `fp = 1−fi` (`Runoff.cpp:318-322`).

*Reachability, verified:* the met writes (`SWMMEngine.cpp:1379` air
temperature, `:1424` wind, `:1431` humidity) all happen inside `stepRunoff`
**before** `runoff_.execute` at `:1648`, so the forcing is fully resolved at
the watershed stage. Today's flux calls sit in `HeatLegacy.cpp:101-102` on
the *routing* clock; H5a's binding runs on the *runoff* clock and must pass
its own `dt`. **Shortwave is a static config constant**
(`RadiativeConfig::shortwave_wm2`, `HeatComponent.cpp:136`) — there is no
diurnal solar path anywhere, so an "equilibration" gate must not assume one.

**D-H5b — vertical conduction between LID layers is IN, and it is new
physics with no in-engine precedent.**
Grepped: the engine has **no** thermal conductivity, no soil-temperature
state and no conduction operator; every "conductivity" in `src/engine/` is
*hydraulic*. The parameters are therefore invented here, and must follow
HydroCouple's naming and defaults the way `SurfaceExchange` and
`RadiativeExchange` followed CSH/RHE.

**⚠ CORRECTION (2026-08-19, found while implementing H5b — lesson 90).**
This section originally recorded `sedDensity = 2650` kg/m³ and
`sedCp = 880` J/kg/°C from `GWComponent/include/gwmodel.h:870-871`. **Those
are dead in-class initializers.** `GWModel`'s constructor member-init list
overrides both — `m_sedDensity(1970)`, `m_sedCp(2758)`
(`GWComponent/src/gwmodel.cpp:51-52`). The constructor wins, so the effective
values differ by a factor of **2.3 in `ρ·cp`** (5.43e6 vs 2.33e6 J/m³/K).
This is lesson 69's exact shape — a declaration is not a value — landing on
the very parameters this decision told the implementer to follow.

**Effective HydroCouple values, taken from the writes:**

| quantity | value | source | note |
|---|---|---|---|
| water thermal conductivity | 0.606 W/m/K | `gwmodel.h:885` | no ctor override; the header value stands |
| sediment thermal conductivity | 2.6 W/m/K | `gwmodel.h:886` | no ctor override |
| sediment density | **1970** kg/m³ | `gwmodel.cpp:51` | header says 2650 — **overridden** |
| sediment specific heat | **2758** J/kg/°C | `gwmodel.cpp:52` | header says 880 — **overridden** |

`HTSComponent` sets **1670 / 1807** (`htsmodel.cpp:50-51`) — different again,
and correctly so: that is streambed sediment, not an aquifer matrix. **H5b
takes GWComponent's**, a porous soil/gravel column being the closer
analogue to a bioretention or permeable-pavement stack. Recorded as a choice
rather than an inheritance, because three defensible pairs exist in the
reference and nothing in it picks one for a LID.

*The stacking-order landmine:* the LID solver's own layer constants are
`SURF=0, SOIL=1, STOR=2, PAVE=3` (`LID.hpp:73-77`) — **not** the physical
stack. A4's `LidLayer` enum (`SURFACE=0, PAVEMENT=1, SOIL=2, STORAGE=3`) IS
the physical top-to-bottom order. Conduction couples *physically adjacent*
layers, so it must iterate `LidLayer`, and any code touching both
orderings needs the mapping stated at the boundary. Layer thicknesses and
void fractions are all confirmed written at parse (`LID.cpp:278-291`,
`:310-327`); an absent layer keeps thickness 0, which is the existing
absence test (`LID.cpp:333,337,484,514,522`) and is also the conduction
term's own guard.

**D-H5c — the dry/absent-element temperature is a DECK-SELECTABLE policy,
not a hard-coded constant.**
A4 zeroes a layer that holds no water ("no water, no age"), which is right
for age and wrong for temperature: 0 °C is a real temperature, which is why
H1 chose `kDefaultTemp = 20.0` rather than 0. The three defensible answers
serve different studies, so the deck picks one:

| policy | behaviour | for |
|---|---|---|
| `HOLD` | freeze the last wet value; rewetting mixes against it | continuity studies; mirrors the dry-element age precedent (state runs, the mask is at the report boundary) |
| `AIR` | track `climate_state.temperature` | the physical answer for an exposed dry surface; forcing already plumbed |
| `DEFAULT` | fall to `HeatConfigData::kDefaultTemp` | reproducibility; the only policy whose gates need no forcing series |

Parsed as a named-mode key in the `ArdConfig.cpp:116-137` `SCALAR_SCHEME`
idiom — token-count guard, if/else-if ladder, and an `errors.push_back`
naming all three legal spellings. **No silent clamp**: `HeatComponent`
resets `ctx.heat_config` at `:95` and rolls back wholesale at `:300-303`, so
a bad value must travel through `errors`. Default is `HOLD` (it invents no
number and needs no forcing).

## 6.2 D-H5d — the integrator (user, 2026-08-19)

**The defect this answers.** H5a's validation found the surface energy
balance diverging to NaN: forward Euler with no stability limit, and heat
capacity is `ρ·cp·V`, so a thin film has almost none. A 0.52 ft³ film over
27,226 ft² takes a **+862 °C step in 60 s**; the flux re-evaluates at 182 °C
and the sequence runs `5 → 182 → −1.8e4 → −3.9e9 → inf → NaN`. Not
deck-specific: the *passing* decks took a +1388 °C first-wet-step excursion
and survived only because rain deepened the surface fast enough.

**Why we inherited it.** CSH solves these same flux formulas as a stiff ODE
at a **1e-4 s** timestep with a selectable solver (`RK4 / RKQS / ADAMS /
BDF / EULER`, `cshmodelio.cpp:3260`; base step `cshmodel.cpp:33`). We adopted
its physics at a 60 s hydraulic step and not its integrator — six orders of
magnitude apart. H2 and H3 are correct as *flux formulations* and were never
wrong; only the stepping was.

**Decision: semi-implicit exponential relaxation.** Linearize the net
outward flux about the current temperature and integrate the linear ODE
exactly:

```
ρ cp V dT/dt = −A·J(T),   J(T) ≈ J₀ + J′·(T − T₀)
k    = A·J′ / (ρ cp V)              [1/s]
T_eq = T₀ − J₀/J′
T(t+dt) = T_eq + (T₀ − T_eq)·exp(−k·dt)
ΔT   = (J₀/J′)·expm1(−k·dt)
```

Properties that decided it: unconditionally stable for `k > 0`; **|ΔT| is
bounded by |T_eq − T₀|, so the step can never overshoot equilibrium** (which
is a gate needing no reference value — the lesson-72 shape); no iteration and
therefore no cap to tune; and it degrades to forward Euler as `dt → 0`
(`expm1(−x) ≈ −x`), so small-step behaviour is unchanged.

`J′` comes from one central probe at `T₀ + h`, `h = 1e-3 °C` — each binding
already evaluates its enabled flux modules, so this costs one extra
evaluation. `k ≤ 0` is anti-damping (no fixed point to relax onto) and falls
back to the bounded explicit step.

**Rejected, with reasons.** *Adaptive explicit sub-stepping* — closest to
CSH, but the pathological film needs hundreds of sub-steps per routing step
and an iteration cap is another tunable that can be wrong. *Extracting a
generic stepper from R3* — `ReactionIntegrator::step` takes
`const ReactionData&` and evaluates the compiled expression VM, so it is not
reusable without refactoring validated reaction code (§3). *Clamping to a
driving temperature* — rejected during validation because it either freezes
the surface at air temperature or oscillates, and **both look like physics**.

**Sequencing (user).** A **standalone commit before H5b**, covering all
three bindings — `applySurfaceExchange` and `applyRadiativeExchange` at
nodes and links (which carry the identical unbounded step, unexposed only
because those volumes are large) and H5a's subarea binding. Not folded into
H5b, so a falsifier sweep can tell a defect fix from a design change.

## 6.3 D-H5e — one binding, one relaxation (user, 2026-08-19)

**The defect D-H5d created.** `applySurfaceExchange` and
`applyRadiativeExchange` were separate entry points called back to back from
`routeLegacyHeat`. Under forward Euler that was harmless: the two increments
were linear and added exactly. **Relaxations do not commute** — each
sub-step relaxes FULLY toward its own module's equilibrium — so the pair
overshoots the true combined one and the answer depends on module order.
Measured, two equal modules with equilibria at 30 °C and 10 °C, true combined
20 °C, from 5 °C:

| k·dt | split | combined |
|---|---|---|
| 4.1e-3 | 5.061317 | 5.061359 |
| 0.41 | 9.700850 | 10.044256 |
| 39.4 | **10.000000** | 20.000000 |

At large `k·dt` the split lands exactly on the last module's equilibrium: the
first module's contribution is erased. Not a regression — that regime
diverged outright before — and it needs `k·dt ≳ 0.4`.

**The lesson that outlives the instance:** replacing an integrator underneath
an existing operator split silently changes what the split *means*. Forward
Euler's linearity was load-bearing, and nobody had written that down.

**Decision: one `applyHeatFluxes`, modules become flux terms.** A single
entry point (`HeatFluxModules/HeatFluxes.{hpp,cpp}`) owns the node and link
traversal, sums every enabled family through `netFluxOut(ctx, T)`, and
relaxes once. `applySurfaceExchange` / `applyRadiativeExchange` are gone;
each module now exposes only `surfaceFluxOut` / `radiativeFluxOut`, which
return 0 when their own `[HEAT_FLUXES]` toggle is off so callers sum
unconditionally.

*Three of four bindings were already correct* — `ArdEngine.cpp` cells and
`HeatWatershed.cpp` subareas each summed both modules before relaxing. Only
the LEGACY node/link path relaxed twice. All four now share `netFluxOut`
rather than keeping four hand-rolled copies of the same sum, since copies of
exactly that sum are what let this diverge in the first place.

**H6b's `SEDIMENT_EXCHANGE` is one added term in `netFluxOut` and nothing
else.** ~~There is no longer a shape in which a flux family could acquire a
binding of its own.~~ **CORRECTED 2026-08-31 (H6b round): this prediction was
wrong, in the expensive direction.** The bed (1) acts on the wetted perimeter,
not the free surface — a different area, nonzero exactly when the surface area
is zero (full pipe) — so it cannot be a `netFluxOut` term; and (2) is a second
state variable, so `relaxT`'s fixed equilibrium does not exist for it. H6b is
a coupled water/bed pair stepped by the exact 2×2 matrix exponential
(`BedExchange.hpp`), solved in ONE step with the surface families for the
D-H5e non-commutation reason. A future SURFACE flux family is still one term
in `netFluxOut`; a family with its own state or its own area is not.

**Answer movement (user's sequencing choice): land it with a `dt` sweep as
evidence.** The merge moves answers again wherever both modules are on. The
discriminator is the one D-H5d's round established: sweep the routing step
and show the gap shrinks with it — local `O((k·dt)²)` accumulated over
`t/dt` steps is first order globally, so **a gap that does NOT shrink is the
error**, and the magnitude alone says nothing.

## 6.4 D-H6a — solar radiation forcing (user, 2026-08-30)

The original H6 line bundled "SedimentExchange (HTS), solar position/clear-sky
module, shade factors" into one stretch phase. Those are not one phase: the
sediment layer adds a **fifth flux family** and a new element state, while the
radiation work adds **neither** — it only changes where `Jin` comes from.
Bundling them makes the cheap, well-referenced half wait on the decision the
expensive half forces. Raised before implementation; resolved as follows.

**D-H6a-1 — H6 splits into H6a (radiation forcing) and H6b (sediment).**
H6a lands first and independently. Numbering is kept rather than renamed to
H8 so existing cross-references (`PROGRESS.md` §2.5 "H6–H7 ⬜",
`UNIFIED_PLAN_STATUS_2026-08-29.md` §6) still resolve to the right work.

**D-H6a-2 — cloud cover modulates shortwave AND longwave.**
Attenuating `Jin` alone is the smaller change but it is not the physical one:
clouds both block sun and re-radiate, and a shortwave-only treatment cools a
fully overcast night far too fast — precisely the regime a stormwater heat
model is used in. The cost is that this reaches into H3's RHE-gated
`atmosphericEmissivity`, which is why §2.5 makes `C = 0 →` exact identity and
a byte-identical `heat_parity` deck an explicit verify criterion rather than
an assumption.

**D-H6a-3 — the three SHORTWAVE spellings are mutually exclusive; a conflict
is a parse ERROR.**
A precedence ladder (timeseries > computed > constant) was the alternative.
Rejected because a ladder makes a deck that configures two sources *run*, and
run plausibly, while silently discarding one of them — the failure mode
lessons 10/20 are about. The parser already refuses rather than repairs
(out-of-range fractions, `HeatComponent.cpp:126-132`); this follows it.

**D-H6a-4 — Bird from the literature; solar position by Spencer/NOAA, with
SPA behind a swap point.**
`SolarRadiationModule` in the HydroCouple tree is the obvious source, and it
is what §2.2 originally named. The decision goes elsewhere for a reason this
program has already paid for once: **conduction has no external parity
reference** (`PROGRESS.md` §2.5 — "CSH and RHE model a streambed, not a
layered LID"), so its gates are one-sided property tests. Bird ships
published tabulated output; choosing it buys a real external gate.

> **⚠ AMENDED 2026-08-30, at implementation.** This decision originally
> specified **NREL SPA** for the position, on the same "published test
> vectors" argument. That argument does not survive contact with the work.
>
> A faithful SPA needs ~260 rows of periodic-term constants (truncated
> VSOP87 L/B/R plus the IAU 1980 nutation series). Writing them without the
> report in reach means the **tables and the test vector are both
> recalled** — and a consistently misremembered pair gates green on wrong
> physics. That is the H3 Brunt-in-kPa failure (a plausible wrong number
> that only a true reference catches) at 260× the surface area, with the
> reference itself compromised. Buying an "external" gate that is actually
> a second copy of the same memory is worse than not claiming one.
>
> **H6a therefore ships Spencer (1971) / NOAA**: ~40 lines, no constant
> tables, verifiable by inspection, and cross-checkable against the
> engine's own `Climate.cpp:180` declination. Accuracy ~0.1° against SPA's
> ±0.0003°.
>
> **That error is not the binding one.** A 0.1° zenith error moves
> clear-sky GHI by under 0.1%; the cloud fraction multiplying it is a
> whole-number guess. Refining the small term under the large one, with 400
> lines of unverifiable constants, is the wrong trade.
>
> **SPA is not abandoned — it is sequenced.** `solarPosition()` is the only
> function that knows how a position is obtained; everything downstream
> consumes `SolarPosition`. Landing SPA is a second implementation of that
> one function plus a selector, no caller changes, and it should be done
> **with NREL's published C source open so the tables are diffed rather
> than recalled.** Until then no gate and no doc may claim SPA accuracy.

*Consequence to accept:* this is a specification, not a transcription. Where
`SolarRadiationModule` and the papers disagree, the papers win and the
difference gets recorded in the module header, the way H3's two RHE
corrections are recorded in `RadiativeExchange.hpp`. **Bird's coefficients
carry the same recall caveat as SPA's tables, at 1/20th the volume** — the
validation handoff §2 makes re-deriving them from the 1981 paper the
checking agent's first job, and `test_heat_solar_radiation.cpp` gate 5 is
labelled a regression pin rather than a parity gate until that is done.

## 7. Open items

- Freezing: clamp T ≥ 0 °C with latent-heat buffering or plain clamp
  (v1: plain clamp + warning).
- Whether heat participates in `analysis_get_mass_balance` as energy (J)
  ledger — proposal: yes, separate energy table.
- Pipe wall conduction for buried conduits (soil temperature model) —
  deferred; HTS module covers streambed-style exchange.
- **Per-element shortwave ranges.** `[RADIATIVE_FLUXES]` is GLOBAL-scope
  only and H6a keeps it that way — the SPA cost argument in §2.5 depends on
  it. RHE's element-range semantics need their own phase, and the shape is
  a per-step solar position plus a per-element geometry/shade term, never a
  per-element SPA call.
- **Does cloud fraction belong to the hydrologic evaporation path too?**
  H6a gives the engine a cloudiness input that the existing
  evaporation/snowmelt machinery has never had. Deliberately NOT wired
  there — that path is parity-locked against legacy SWMM. Worth revisiting
  only with a stated willingness to break that parity.
- **Shortwave units at the API boundary.** W/m² is SI and the engine is
  foot-second internally; `[RADIATIVE_FLUXES]` currently takes W/m² in both
  unit systems. Confirm that stays true for the TIMESERIES path before
  H6a ships, or a US-units deck will read a solar record as something else.
