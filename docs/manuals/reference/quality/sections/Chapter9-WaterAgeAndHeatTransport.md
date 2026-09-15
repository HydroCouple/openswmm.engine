@page quality_ref_ch9_age_heat Chapter 9: Water Age and Heat Transport

@tableofcontents

## 9.1 Introduction

Water age and temperature are not pollutants, but both are scalar fields
carried by the same water, mixed by the same junctions and advected by the
same flows. OpenSWMM therefore transports them through the same machinery
that carries pollutants and reaction species, as **reserved species** — rows
on the transport state that no `[POLLUTANTS]` declaration creates and that
obey slightly different laws.

Treating them as reserved rows rather than as separate subsystems is what
lets a single mixing formula serve all of them: when two flows meet, the
volume-weighted mean is the correct answer for a concentration, for an age
and for a temperature alike. What differs is not the mixing but the source
terms and the constraints, and those are stated explicitly below rather than
left to fall out of the code.

Both are enabled by an `[OPTIONS]` key and are inert otherwise:

| Key | Reserved species | Internal unit | Reported unit |
|---|---|---|---|
| `WATER_AGE` | `__WATER_AGE__` | seconds | hours |
| `HEAT_TRANSPORT` | `__TEMPERATURE__` | °C | °C |

## 9.2 Water Age

### 9.2.1 Definition and aging

Water age is the time elapsed since a parcel of water entered the system.
Every element's age advances by exactly the routing step before transport is
applied:

\f[a(t + \Delta t) = a(t) + \Delta t\f]

and then mixes by volume weight wherever flows combine, exactly as a
concentration does:

\f[a_{mix} = \frac{\sum_i a_i V_i}{\sum_i V_i}\f]

Age therefore has no decay and no reaction. It grows without bound in water
that never leaves, which is the intended reading: a storage unit whose age
climbs steadily is a storage unit that is not turning over.

### 9.2.2 Sources

`[WATER_AGE_SOURCES]` sets the age carried by water entering the system, by
scope. The common cases are:

- **`INITIAL_STATE`** — the age assigned to water already in the network at
  \f$t = 0\f$. Setting this to zero models a system flushed at the start;
  setting it to a nonzero value models one that has been standing.
- **Per-pathway sources** — the age of water arriving by dry-weather flow,
  groundwater, RDII, external inflow, and the remaining loader pathways.

Water entering with age zero is fresh; water entering with a nonzero age is
treated as having already spent that long in transit elsewhere, which is how
a boundary at the edge of a modelled area represents the network beyond it.

**Example.** A collection system fed by an upstream trunk that is not
modelled, whose travel time is known to be about four hours:

```
[WATER_AGE_SOURCES]
INITIAL_STATE     GLOBAL   0.0
EXTERNAL_INFLOW   GLOBAL   4.0
DWF               GLOBAL   0.0
```

Ages here are in **hours**, the reported unit. Water arriving through
`[INFLOWS]` is treated as four hours old on arrival; dry-weather flow enters
fresh; the network starts empty of aged water.

### 9.2.3 State and reporting

Age is held in seconds internally and reported in **hours**, which is the
scale users read. The distinction matters when reading the output file
directly rather than through the reporting layer.

A **dry** element keeps aging in state while its reported age is masked.
Those are deliberately separate: the state must keep advancing because the
element may refill and the water in it has genuinely been there, while
reporting an age for an element holding no water would be meaningless. A
restart therefore carries the aged state, not the masked report.

## 9.3 Heat Transport

### 9.3.1 Temperature as a transported scalar

Temperature is carried as a reserved species and mixed by volume weight,
which is the correct mixing rule when the specific heat of water is taken as
constant across the modelled range — mixing enthalpies then reduces to mixing
temperatures:

\f[T_{mix} = \frac{\sum_i T_i V_i}{\sum_i V_i}\f]

Unlike age, temperature does **not** advance with time by itself, does not
decay, and is **not** constrained to be non-negative: water below 0 °C is a
legitimate model state, and the non-negativity clamp applied to
concentrations is deliberately not applied to this row.

Inlet temperatures are set per source by `[HEAT_SOURCES]`, with a global
value for each of the seven water sources and node-scope overrides where a
particular inflow point differs:

```
[HEAT_SOURCES]
;;source           scope   [name]  degC
RAINFALL           GLOBAL           12.0
DWF                GLOBAL           18.5
GW                 GLOBAL           11.0
INITIAL_STATE      GLOBAL           15.0
DWF                NODE     J12     22.0
```

Temperatures are °C and must lie between −50 and 100. A source with no row
takes 20 °C. NODE-scope overrides apply to `DWF` and `EXTERNAL_INFLOW`; other
sources take their global value.

Transport alone — advection and mixing, with no flux modules enabled — is the
default and is often enough: it answers "where does the warm water go?" A
model that additionally needs "how does it warm or cool on the way?" enables
one or more of the flux modules below.

### 9.3.2 Meteorological forcing

The flux modules are driven by the model's **existing climate data**, not by
a heat-specific meteorology section. Air temperature, wind speed and relative
humidity come from `[TEMPERATURE]`, `[WINDSPEED]` and the humidity series
already used by evaporation and snowmelt, which means a model that has been
calibrated for hydrology is already carrying most of what heat transport
needs.

| Forcing | Source | Native unit | Used as |
|---|---|---|---|
| Air temperature \f$T_a\f$ | `[TEMPERATURE]` | °F | °C |
| Wind speed \f$w\f$ | `[WINDSPEED]` | mph | m/s |
| Relative humidity \f$RH\f$ | climate series | % | fraction |
| Incoming shortwave \f$J_{in}\f$ | `[RADIATIVE_FLUXES]` / computed | W/m² | W/m² |
| Cloud fraction \f$C\f$ | `[CLOUD_COVER]` | 0–1 | 0–1 |

The unit conversions are worth stating because they are a common source of
confusion when reading intermediate values: the climate state stores air
temperature in **°F** and wind in **mph** for historical reasons, while every
heat formulation is written in SI. The conversion happens in exactly one
place so the two cannot drift apart.

### 9.3.3 The surface energy balance

With `[HEAT_FLUXES] SURFACE_EXCHANGE ON` and/or `RADIATIVE_EXCHANGE ON`, the
free surface exchanges heat with the atmosphere. The modules use a consistent
sign convention throughout:

> **Positive flux is heat leaving the water.**

The net flux out of the surface is the sum of the enabled modules' terms:

\f[J_{net} = \underbrace{J_e + J_h}_{\text{surface exchange}} + \underbrace{J_{br} - J_{sn} - J_{an} - J_{lc}}_{\text{radiative exchange}}\f]

where \f$J_e\f$ is latent, \f$J_h\f$ sensible, \f$J_{br}\f$ back longwave
radiated by the water, \f$J_{sn}\f$ net shortwave absorbed, \f$J_{an}\f$
atmospheric longwave absorbed, and \f$J_{lc}\f$ land-cover longwave absorbed.
Three radiative terms warm the water and one cools it, which is why only
\f$J_{br}\f$ carries a positive sign in the sum.

The two modules are independent. A model may enable surface exchange without
radiation (evaporative cooling only), radiation without surface exchange
(a covered channel), or both.

### 9.3.4 Latent heat — evaporation and condensation

Latent exchange follows the mass-transfer formulation given for surface-water
quality modelling by Martin and McCutcheon (1999). Saturation vapour pressure
over water uses the Magnus form, in kPa:

\f[e_s(T) = 0.61275\,\exp\!\left(\frac{17.27\,T}{237.3 + T}\right)\f]

The air's actual vapour pressure follows from relative humidity evaluated at
the **air** temperature, while the water surface is saturated at the **water**
temperature:

\f[e_a = \frac{RH}{100}\,e_s(T_a), \qquad e_w = e_s(T_w)\f]

The evaporative mass flux is the vapour-pressure deficit driven by a linear
wind function:

\f[E = f(w)\,(e_w - e_a), \qquad f(w) = a + b\,w\f]

with \f$E\f$ in m/s and \f$f(w)\f$ in m/s/kPa. The coefficients \f$a\f$ and
\f$b\f$ are the calibration handles of this module. The latent heat flux is
then

\f[J_e = \rho_w\,L_v(T_w)\,E, \qquad L_v(T) = 1000\,(2499 - 2.36\,T)\ \text{J/kg}\f]

**\f$E\f$ is signed.** When \f$e_a > e_w\f$ — humid air over cold water — the
deficit is negative, \f$E\f$ is negative, and the term becomes condensation
*warming* the water rather than evaporation cooling it. The formulation is
not clamped to evaporation only, because condensation onto a cold surface is
a real process and suppressing it would bias the balance in one direction.

### 9.3.5 Sensible heat — the Bowen ratio

Sensible exchange is obtained from the latent flux through the Bowen ratio
rather than by an independent transfer coefficient, which is the standard
closure and keeps the two terms consistent by construction:

\f[B_r = \gamma\,\frac{p}{p_0}\,\frac{T_w - T_a}{e_w - e_a}, \qquad J_h = B_r\,J_e\f]

where \f$\gamma\f$ is the Bowen coefficient (kPa/°C) and \f$p/p_0\f$ the
ratio of site to sea-level pressure.

The ratio has a removable singularity: as the vapour-pressure deficit
approaches zero the quotient diverges, but the product \f$B_r J_e\f$ remains
finite because \f$J_e\f$ carries the same deficit as a factor. The
implementation returns \f$B_r = 0\f$ when \f$|e_w - e_a| < 10^{-12}\f$, which
is the correct value of the limiting product and not an error suppression.

### 9.3.6 Radiative exchange

Four terms are computed when `RADIATIVE_EXCHANGE ON`. All longwave terms use
the Stefan–Boltzmann law with absolute temperature.

**Net absorbed shortwave.** Incident shortwave, reduced by surface reflection
and by shading:

\f[J_{sn} = (1 - R_s)\,J_{in}\,\max(0,\,1 - f_s)\f]

The \f$\max\f$ guard means an over-unity shade factor shades completely rather
than inverting the sign into negative insolation.

**Back longwave radiated by the water:**

\f[J_{br} = \varepsilon_w\,\sigma\,T_w^4\f]

**Atmospheric longwave absorbed.** Clear-sky atmospheric emissivity uses the
Brunt (1932) form, with vapour pressure in pascals:

\f[\varepsilon_a = A_a + 0.0027\,\sqrt{1000\,e_a}\f]

\f[J_{an} = \sigma\,T_a^4\,\varepsilon_a\,(1 - R_L)\,f_{sky}\f]

where \f$R_L\f$ is the longwave reflectance of the water surface and
\f$f_{sky}\f$ the sky-view fraction.

**Land-cover longwave absorbed.** The complement of the sky view is occupied
by vegetation or structures, which radiate toward the water:

\f[J_{lc} = \varepsilon_{lc}\,\sigma\,(1 - f_{sky})\,T_a^4\f]

@note This term uses **air** temperature as the land-cover temperature. A
canopy is usually warmer than the air during the day, so this understates
daytime longwave gain from the surroundings. It is a recorded, deliberate
simplification: carrying a separate land-cover temperature state would
require a canopy energy balance of its own.

### 9.3.7 Shortwave forcing

`[RADIATIVE_FLUXES] SHORTWAVE` selects where \f$J_{in}\f$ comes from. The
three spellings are mutually exclusive:

| Spelling | Source |
|---|---|
| `SHORTWAVE GLOBAL <W/m²>` | A constant |
| `SHORTWAVE GLOBAL TIMESERIES <name>` | A measured record, interpolated |
| `SHORTWAVE GLOBAL COMPUTED` | Solar position and a clear-sky model |

Under `COMPUTED`, solar position is evaluated from the Fourier-series
representation of Spencer (1971) — declination, equation of time, and hour
angle — giving the solar zenith angle \f$\theta_z\f$. Relative optical air
mass then follows Kasten and Young (1989):

\f[m = \frac{1}{\cos\theta_z + 0.50572\,(96.07995 - \theta_z)^{-1.6364}}\f]

with \f$\theta_z\f$ in degrees. This form is used rather than the simple
\f$\sec\theta_z\f$ because the secant diverges at the horizon, where a
low-sun model spends a meaningful part of every day.

Station pressure is derived from site elevation by the standard-atmosphere
relation

\f[p = p_0\,(1 - 2.25577 \times 10^{-5}\,z)^{5.25588}\f]

and clear-sky global horizontal irradiance from the Bird and Hulstrom (1981)
model, which resolves direct-beam and diffuse components through separate
transmittances for Rayleigh scattering, ozone, mixed gases, water vapour and
aerosols. Site parameters — latitude, longitude, elevation, turbidity,
precipitable water, ozone — are set in `[SOLAR_RADIATION]`.

```
[SOLAR_RADIATION]
LATITUDE      41.88
LONGITUDE    -87.63
TIMEZONE       -6
ELEVATION     181.0
```

Night is represented by \f$J_{in} = 0\f$ rather than by a special case, so no
branch distinguishes it. A **negative** internal sentinel, not zero, marks
"no resolved value, fall back to the configured constant" — because zero is a
legal night-time irradiance and must not read as unset.

### 9.3.8 Cloud cover

`[CLOUD_COVER]` supplies a cloud fraction \f$C \in [0,1]\f$, constant or as a
time series. Cloud attenuates **both** radiative directions, and the two
corrections are different functions:

**Shortwave**, following Kasten and Czeplak (1980):

\f[J_{in} \leftarrow J_{in}\,\max\!\left(0,\ 1 - k\,C^{\,n}\right)\f]

**Longwave**, following Bolz — cloud raises the effective atmospheric
emissivity:

\f[\varepsilon_a \leftarrow \varepsilon_a\,(1 + k_{lw}\,C^2)\f]

Clouds therefore reduce incoming solar radiation while *increasing* downward
longwave. Applying cloud to only one direction — the tempting simplification —
would bias the net balance systematically, which is why both corrections are
present and why the cloud fraction feeds two separate paths.

Both factors return a **literal** 1.0 when \f$C \le 0\f$, so a model with no
cloud configuration produces bit-identical results to one built before cloud
existed. This is stronger than "equal within rounding" and is deliberate.

### 9.3.9 Applying the flux to a water body

Given \f$J_{net}(T_w)\f$ over a surface area \f$A\f$ and volume \f$V\f$, the
naive explicit update is

\f[\Delta T_{expl} = -\,\frac{J_{net}(T_0)\,A\,\Delta t}{\rho_w c_p V}\f]

which is accurate for small steps but can **overshoot** — a shallow, fast-
cooling element can be driven past equilibrium and oscillate, or leave the
physical range entirely, when the routing step is long relative to the
thermal response time.

OpenSWMM therefore applies the flux with a **linearized semi-implicit
relaxation**. The flux is probed at \f$T_0\f$ and at \f$T_0 + h\f$ to obtain
its local slope,

\f[J' = \frac{J(T_0 + h) - J(T_0)}{h}, \qquad k = \frac{A\,J'}{\rho_w c_p V}\f]

and the update becomes an exponential relaxation toward the equilibrium
temperature \f$T_{eq} = T_0 - J_0/J'\f$:

\f[\Delta T = \frac{J_0}{J'}\left(e^{-k\,\Delta t} - 1\right)\f]

Because \f$J\f$ increases with \f$T_w\f$ over the physical range (a warmer
surface radiates and evaporates more), \f$J' > 0\f$ and \f$k > 0\f$, so the
step **cannot overshoot** \f$T_{eq}\f$ regardless of \f$\Delta t\f$: it
approaches it asymptotically. When \f$J' \le 0\f$ — outside the physical
regime, or where the probe is dominated by rounding — the scheme falls back
to the explicit form rather than dividing by a slope it does not trust.

### 9.3.10 LID layer conduction

With `[HEAT_FLUXES] LAYER_CONDUCTION ON`, vertical conduction is solved
between the layers of an LID control — surface, pavement, soil and storage —
so a bioretention or permeable-pavement column has a thermal profile rather
than a single mixed temperature.

Each layer's effective conductivity is the volume-weighted mean of its water
and solid fractions, with \f$\theta\f$ the water content:

\f[k_{eff} = \theta\,k_w + (1 - \theta)\,k_s\f]

Defaults are \f$k_w = 0.606\f$ W/m/K for water and \f$k_s = 2.6\f$ W/m/K for
the solid matrix, with a matrix density of 1970 kg/m³ and specific heat
2758 J/kg/K. A wet layer therefore conducts differently from a dry one, which
is the point: a saturated storage layer couples to the soil above it far more
strongly than an empty one.

Conductance across the interface between adjacent layers is the **series
resistance** of their half-thicknesses:

\f[U_{i,i+1} = \left(\frac{d_i}{2 k_i} + \frac{d_{i+1}}{2 k_{i+1}}\right)^{-1}\f]

and the resulting one-dimensional system is solved **implicitly** by a
tridiagonal (Thomas) solve, so a thin surface film — which has a very short
thermal time constant — does not force the whole model onto a short step.

A layer that is dry but present still participates in conduction: it has a
temperature and a thermal mass even with no water in it, and removing it from
the system would break the column into disconnected pieces.

### 9.3.11 The bed zone — conduction and hyporheic exchange

A buried conduit exchanges heat with more than its free surface. Its wetted
perimeter is in contact with the pipe wall and the sediment beyond it, that
sediment conducts to the deeper ground, and in an unlined or permeable
channel water itself moves between the channel and the pore space beneath it.
`[HEAT_FLUXES] SEDIMENT_EXCHANGE ON` adds all three.

The formulation follows the transient-storage zone of HydroCouple's
`HTSComponent`, whose lineage is the stream transient-storage literature
(Bencala and Walters, 1983; Runkel, 1998). The zone is a **second body**: a
bed of thickness \f$Y_{bed}\f$ beneath each conduit, with its own temperature
\f$T_b\f$ and its own thermal mass, exchanging with the water above and with
a deep-ground boundary below.

**Three conductances.** Writing \f$A_{bed}\f$ for the contact area and
\f$\rho_s c_s\f$ for the volumetric heat capacity of the saturated sediment:

\f[G_{cond} = \frac{\alpha_{sed}\,\rho_s c_s\, A_{bed}}{Y_{bed}}\f]

\f[G_{adv} = \rho_w c_p\, v_{hyp}\, A_{bed}\f]

\f[G_{bg} = \frac{\alpha_{sed}\,\rho_s c_s\, A_{bed}}{Y_{gr}}\f]

where \f$\alpha_{sed}\f$ is the bed thermal diffusivity (m²/s), \f$v_{hyp}\f$
the hyporheic exchange velocity across the interface (m/s), and \f$Y_{gr}\f$
the depth to the deep-ground boundary. The grouping \f$\alpha\rho c\f$ has
units of W/m/K and is an effective thermal conductivity; it is written this
way rather than as a conductivity \f$k\f$ because the diffusivity is the
parameter the deck supplies.

Conduction and hyporheic advection both move heat between the same two
bodies and are therefore summed into a single water–bed conductance,
\f$G_{wb} = G_{cond} + G_{adv}\f$. The pair of bodies then obeys

\f[C_w \frac{dT_w}{dt} = -A_s J(T_w) + G_{wb}(T_b - T_w)\f]

\f[C_b \frac{dT_b}{dt} = G_{wb}(T_w - T_b) + G_{bg}(T_{gr} - T_b)\f]

with \f$C_w = \rho_w c_p V_w\f$ and \f$C_b = \rho_s c_s A_{bed} Y_{bed}\f$.

**The exchange is conservative by construction.** Adding the two equations,
\f$G_{wb}\f$ cancels identically and leaves
\f$C_w \dot T_w + C_b \dot T_b = -A_s J + G_{bg}(T_{gr} - T_b)\f$: whatever
the water loses to the bed, the bed gains. The ground term is the only true
source or sink in the pair, which is why a bed that is well insulated from
the deep ground eventually equilibrates with the water rather than draining
it.

**Integration.** The two bodies are stepped **simultaneously**, not one after
the other. Each body's equilibrium moves as the other responds, so there is
no fixed target for a single-body relaxation (§9.3.9) to relax toward, and
stepping them in sequence makes the answer depend on which went last. The
linearized 2×2 system is advanced by its exact matrix exponential. Both
eigenvalues are real and non-positive — the discriminant is
\f$(a_{11}-a_{22})^2 + 4a_{12}a_{21}\f$ with non-negative off-diagonals — so
the pair can neither oscillate nor overshoot at any step length.

**Contact area is the wetted perimeter**, \f$P = A/R\f$, times length times
barrels. This is a deliberate departure from the open-channel reference,
which uses top width: the two coincide in a wide channel, but for a
surcharged circular pipe the top width is zero while the contact area is at
its maximum. Using top width would switch the module off exactly where a
buried pipe conducts most. The consequence worth knowing is that a **closed**
conduit, which has no free surface and therefore no latent, sensible or
radiative exchange at all, still exchanges with its bed — for much of a
buried network this is the only heat transfer there is.

**Solutes.** The same zone acts as transient storage for pollutants and
multi-species reactants. The exchange discharge is

\f[Q_{exch} = \frac{D_{sed} A_{bed}}{Y_{bed}} + v_{hyp} A_{bed}\f]

and the channel/bed concentration pair is advanced in total-and-difference
coordinates: the total mass is invariant and the difference decays as
\f$\exp(-\mu \Delta t)\f$ with
\f$\mu = Q_{exch}(1/V_w + 1/V_b)\f$. Solving in those coordinates makes mass
conservation structural rather than a bookkeeping step. This is what produces
the long concentration tail characteristic of a stream with a storage zone: a
tracer slug leaves mass behind in the bed and that mass returns slowly after
the peak has passed. There is deliberately **no** bed-to-ground solute
exchange, matching the reference.

**Parameters** (`[SEDIMENT_EXCHANGE]`, all `GLOBAL` scope):

| Key | Symbol | Default | Meaning |
|---|---|---|---|
| `THERMAL_DIFFUSIVITY` | \f$\alpha_{sed}\f$ | 1.0e-6 | Bed thermal diffusivity, m²/s |
| `SOLUTE_DIFFUSIVITY` | \f$D_{sed}\f$ | 1.0e-9 | Bed effective solute diffusivity, m²/s |
| `BED_THICKNESS` | \f$Y_{bed}\f$ | 0.20 | Bed layer thickness, m |
| `GROUND_DEPTH` | \f$Y_{gr}\f$ | 2.0 | Depth to the deep-ground boundary, m |
| `GROUND_TEMPERATURE` | \f$T_{gr}\f$ | 12.0 | Deep-ground temperature, °C (or `TIMESERIES`) |
| `HYPORHEIC_VELOCITY` | \f$v_{hyp}\f$ | 0.0 | Exchange velocity across the interface, m/s |
| `SEDIMENT_DENSITY` | \f$\rho_s\f$ | 1670 | Bed bulk density, kg/m³ |
| `SEDIMENT_SPECIFIC_HEAT` | \f$c_s\f$ | 1807 | Bed specific heat, J/kg/K |
| `INITIAL_TEMPERATURE` | — | \f$T_{gr}\f$ | Initial bed temperature, °C |

The default sediment properties describe **streambed sediment** and are
deliberately different from the `LAYER_CONDUCTION` defaults of §9.3.10, which
describe a bioretention soil column. They are different materials and a
single pair of numbers cannot stand for both.

Every length, diffusivity and material property is **refused** at zero or
below rather than clamped, because each is a divisor or a capacity: a
silently repaired zero produces either an infinite conductance or a massless
bed that tracks the water exactly, both of which look like a working model.
`SOLUTE_DIFFUSIVITY` and `HYPORHEIC_VELOCITY` accept zero, each selecting a
meaningful configuration (conduction-only transport, and conduction-only
exchange respectively).

`GROUND_TEMPERATURE` has a default but the engine **warns** when a deck
leaves it unstated: for a buried conduit it is frequently the largest term in
the balance, so the default is unlikely to be what was meant.

**A worked bed configuration** — a buried sewer in cool ground with modest
hyporheic exchange:

```
[HEAT_FLUXES]
SEDIMENT_EXCHANGE      ON

[SEDIMENT_EXCHANGE]
GROUND_TEMPERATURE     GLOBAL 11.5
GROUND_DEPTH           GLOBAL 2.0
BED_THICKNESS          GLOBAL 0.25
THERMAL_DIFFUSIVITY    GLOBAL 1.1e-6
HYPORHEIC_VELOCITY     GLOBAL 5.0e-6
SOLUTE_DIFFUSIVITY     GLOBAL 1.0e-9
```

### 9.3.12 Parameter reference

`[RADIATIVE_FLUXES]` parameters and their defaults:

| Key | Symbol | Default | Meaning |
|---|---|---|---|
| `SHORTWAVE` | \f$J_{in}\f$ | 0.0 | Incoming solar, W/m² (constant mode) |
| `ALBEDO` | \f$R_s\f$ | 0.0 | Shortwave reflectance of the water surface |
| `SHADE_FACTOR` | \f$f_s\f$ | 0.0 | Fraction of insolation blocked; 0 = unshaded |
| `SKY_VIEW` | \f$f_{sky}\f$ | 1.0 | Fraction of the hemisphere that is sky |
| `EMISS_WATER` | \f$\varepsilon_w\f$ | 0.97 | Longwave emissivity of water |
| `EMISS_LANDCOVER` | \f$\varepsilon_{lc}\f$ | 0.97 | Longwave emissivity of surroundings |
| `ATM_EMISS_COEFF` | \f$A_a\f$ | 0.5 | Brunt clear-sky coefficient |
| `ATM_LW_REFLECTION` | \f$R_L\f$ | 0.03 | Longwave reflectance of the surface |

A fully shaded, closed-canopy reach is \f$f_s \to 1\f$ with
\f$f_{sky} \to 0\f$: no direct insolation reaches the water, and the longwave
balance is dominated by the canopy term rather than the sky term.

### 9.3.13 A worked configuration

A shaded urban stream reach with computed solar forcing, partial cloud, and
both exchange modules active:

```
[PROCESS_COMPONENTS]
org.hydrocouple.openswmm.heat  config="model.heat"
```

```
;; model.heat
[HEAT_SOURCES]
DWF             GLOBAL   18.5
EXTERNAL_INFLOW GLOBAL   14.0
INITIAL_STATE   GLOBAL   15.0

[HEAT_FLUXES]
SURFACE_EXCHANGE    ON
RADIATIVE_EXCHANGE  ON

[RADIATIVE_FLUXES]
SHORTWAVE     GLOBAL COMPUTED
ALBEDO        0.06
SHADE_FACTOR  0.65
SKY_VIEW      0.35
EMISS_WATER   0.97

[SOLAR_RADIATION]
LATITUDE   41.88
LONGITUDE -87.63
TIMEZONE   -6
ELEVATION  181.0

[CLOUD_COVER]
FRACTION   0.4
```

Note that `SHADE_FACTOR 0.65` and `SKY_VIEW 0.35` are consistent with one
another — riparian canopy blocking roughly two-thirds of the sky both shades
the shortwave and replaces sky longwave with canopy longwave. They are
independent keys, and setting one without the other is a common way to
produce a reach that is shaded from the sun but still radiating to an open
sky.

## 9.4 Engine Coverage

Both reserved species are carried by all three transport engines:

| | `LEGACY` | `EULERIAN_ARD` | `LAGRANGIAN` |
|---|---|---|---|
| Water age | yes | yes | yes |
| Temperature | yes | yes | yes |
| Dispersion of either | n/a | yes | yes |
| Surface + radiative fluxes | yes | yes | yes |
| Bed zone (§9.3.11) | per link | per cell | per link |

The bed zone is resolved differently by engine. Under `EULERIAN_ARD` each
mesh cell has its own bed slice, so a bed beneath a long conduit resolves the
same along-length variation the water above it does. Under `LEGACY` and
`LAGRANGIAN` the bed is one well-mixed body per conduit; the Lagrangian
engine solves the exchange against the volume-weighted parcel mean and
applies the resulting increment uniformly, which preserves the along-link
profile the parcels carry.

Where dispersion is active, temperature and age are dispersed with the **same
coefficient** as the solutes. In shear-dominated open-channel flow the
dispersion coefficient is a property of the velocity field rather than of the
transported quantity (§7.3.3), so applying a separate coefficient to heat
would be less physical, not more conservative.

Temperature is **exempt** from three behaviours the other rows have, and each
exemption is deliberate:

- **No aging.** The \f$+\Delta t\f$ advance is the age row's law alone.
- **No decay.** First-order decay applies to pollutants, not to temperature.
- **No non-negativity clamp.** Sub-zero °C is a legal state — 0 is not
  special for temperature the way it is for a mass, so the floor that keeps
  concentrations non-negative is not applied to the temperature row in any
  engine.

## 9.5 Implementation

Water age is implemented in
`src/engine/transport/components/WaterAgeModule/`, heat in
`src/engine/transport/components/HeatModule/` (`HeatComponent.cpp` for the
configuration, `HeatLid.cpp` for layer conduction), and the flux modules in
`HeatFluxModules/` — `SurfaceExchange` for the latent and sensible terms and
the relaxation stepper, `RadiativeExchange` for the four radiative terms,
`SolarRadiation` for the Spencer, Kasten–Young and Bird–Hulstrom
computations, and `BedExchange` for the bed zone's conductances, its coupled
two-body stepper and the two-body solute exchange (`BedZoneData.hpp` holds
its configuration and state). Both reserved rows are allocated by the transport engines'
species-row layout: `ArdEngine.cpp` for the Eulerian mesh, and `rowLayout()`
in `LagrangianSolver.hpp` for the segment store.

C API surfaces are declared in @ref openswmm_waterage.h and
@ref openswmm_heat.h; the transport-engine configuration itself is in
@ref openswmm_transport.h.
