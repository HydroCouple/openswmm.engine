@page quality_ref_ch10_mesh_quality Chapter 10: Surface Quality and Transport on the 2D Mesh

@tableofcontents

## 10.1 The Transport Policy

This chapter describes what the water of the overland flow solver
(@ref hydraulics_ref_ch9_two_dimensional) carries. The mesh is a fourth
*domain* species can occupy, beside subcatchment runoff, the legacy aquifers
and the 1D network, and the first question is which classes travel there.

That question is answered in one place — the **transport policy** — rather
than re-derived by each solver. Four classes are recognised: pollutants
(`[POLLUTANTS]`), the reaction system's species (Chapter 8), water age
(`__WATER_AGE__`) and temperature (`__TEMPERATURE__`). For each domain, each
class resolves to **enabled** with a row count, **disabled by the user**
naming the key, or **unavailable** with the reason. For the 2D surface the
gates fire in this order: no mesh — unavailable; the class off at project
level ("no [POLLUTANTS]", "no reactions component", "WATER_AGE OFF",
"HEAT_TRANSPORT OFF") — unavailable; `[OPTIONS] IGNORE_2D YES` — disabled;
`IGNORE_QUALITY YES` — disabled, for **pollutants and MSX species only**,
since age and temperature are their own options on the mesh as in 1D; a
`WALL` species in the reaction system — MSX unavailable, since a
wall-attached species has no meaning on a sheet of water and the class is
left off with a warning rather than carried half; the matching
`[2D_OPTIONS]` key below set to `NO` — disabled, naming the key; otherwise
enabled.

| Key | Values | Default | Effect on the mesh |
|---|---|---|---|
| `TRANSPORT_POLLUTANTS` | `YES` or `NO` | `YES` | Carry the `[POLLUTANTS]` rows |
| `TRANSPORT_MSX` | `YES` or `NO` | `YES` | Carry the reaction system's species |
| `TRANSPORT_AGE` | `YES` or `NO` | `YES` | Carry `__WATER_AGE__` when `WATER_AGE ON` |
| `TRANSPORT_TEMPERATURE` | `YES` or `NO` | `YES` | Carry `__TEMPERATURE__` when `HEAT_TRANSPORT ON` |
| `DISPERSION` | m²/s, finite, non-negative | 0 | Isotropic dispersion (§10.2); negative is refused, not clamped |

The policy is *inherit-on* (@ref engine_manual_sect_2D_OPTIONS): a class
enabled at project level runs in every domain whose solver carries it unless
a key says otherwise. Rows take the canonical order the Eulerian 1D engine
uses — pollutants, MSX species in declaration order, the age row, the
temperature row **last**. Whenever a project declares any class, the
resolved matrix is printed in the `.rpt` header, one row per domain, with
`on(n)`, `off:KEY` or `n/a` per class and one explanatory line per `n/a`
cell; the C API and the GUI read the same matrix. Transport rows are
integrated by the CPU marcher; the Kokkos backends carry none, so a project
with any class enabled on the mesh runs on the CPU whatever the backend
setting.

<!-- FIGURE: quality_ch10_transport_policy — the Domain × Species-class matrix as a 4×4 grid; the 2D-surface row annotated with its gate ladder (mesh → project option → IGNORE_2D → IGNORE_QUALITY → WALL → TRANSPORT_* key), and the reserved rows in canonical order: pollutants, MSX, __WATER_AGE__, __TEMPERATURE__ -->

<!-- source: src/engine/transport/TransportPolicy.cpp:89-122, 206-238, 273-320; src/engine/transport/TransportPolicy.hpp:1-35; src/engine/2d/input/SectionHandlers2D.cpp:120-125, 355-364; src/engine/2d/data/SolverOptions2D.hpp:311-314, 412; src/engine/plugins/DefaultReportPlugin.cpp:562-572; src/engine/2d/SurfaceRouter2D.cpp:739-761, 850-855; src/engine/input/handlers/OptionsHandler.cpp:346-349 -->

## 10.2 Species Transport on the Mesh

### 10.2.1 Mass, not concentration

The hydraulic solver's integrated state is the cell **volume**; depth and head
are reconstructed from it. The transport state follows the same rule: what
is stored per cell is species **mass** \f$m_{s,i}\f$, species-major, and
concentration is derived,

| | | | |
|---|---|---|---|
| \f[c_{s,i} = \frac{m_{s,i}}{V_i}\f] | | (10-1) | |

reported as zero when \f$V_i\f$ lies below the hydraulics' dry-volume
threshold. A cell that empties keeps what it held, and no concentration has
to be extrapolated against a vanishing volume. The temperature row is the
one **signed** row — water below 0 °C is a state — so every non-negativity
guard in the marcher and the coupling queue skips it, as in 1D (Chapter 9).

### 10.2.2 The governing equation

Per cell \f$i\f$ and species \f$s\f$, in mass form:

| | | | |
|---|---|---|---|
| \f[\frac{dm_{s,i}}{dt} = -\sum_{e \in \partial i} Q_e\, c_{s,\mathrm{donor}(e)} + \sum_{e \in \partial i} \frac{D\, h_e\, \xi_e}{d_e}\,(c_{s,j(e)} - c_{s,i}) + V_i\, r_s(\mathbf{c}_i, T_i) + S_{s,i}\f] | | (10-2) | |

with \f$Q_e\f$ the face volume flux the marcher computed, \f$D\f$ the
isotropic dispersion coefficient, \f$h_e\f$ the face depth, \f$\xi_e\f$ the
face length, \f$d_e\f$ the normal centroid distance, \f$r_s\f$ the reaction
rate and \f$S_{s,i}\f$ the sources of §10.2.5 — Chapter 7's equation for a
cell of the mesh, differing chiefly in how the first term is evaluated.

### 10.2.3 Advection rides the face fluxes

The advective flux is booked **inside the same face firing** that moves the
water. When the marcher commits a volume \f$\Delta M\f$ across a face —
after the Froude cap and the positivity share, so it is the volume that
actually moves — the mass \f$\Delta M \cdot c_{\mathrm{donor}}\f$ leaves the
donor and arrives at the receiver, with the donor concentration read at that
substep against the same published volume the share was budgeted from.
First-order donor-cell upwinding is the only scheme on the mesh; the limited
second-order reconstructions of the 1D engines are not offered (§10.11).

Two properties follow: the species flux cannot disagree with the volume flux
about cadence, direction or magnitude, so a uniform field stays uniform
under arbitrary flow, across local-time-stepping tiers and through wet/dry
fronts; and the scheme conserves to round-off, one writer per face. The
donor read guards on \f$V > 0\f$ alone, not on the dry depth, so a face that
fires at a wet/dry front exports at the exporter's true concentration.

### 10.2.4 Dispersion

`[2D_OPTIONS] DISPERSION` sets one isotropic coefficient in m²/s. The
exchange is an explicit two-point flux booked on the same face, at the same
cadence, into the same accumulators as advection:

| | | | |
|---|---|---|---|
| \f[\Delta m_{a \to b} = D\, h_e\, \xi_e\, \frac{\Delta t_e}{d_e}\,(c_a - c_b)\f] | | (10-3) | |

An explicit parabolic term has its own stability limit,
\f$D\,\Delta t / d_e^2 \lesssim \tfrac{1}{2}\f$, and the marcher's substep is
set by gravity waves, not by \f$D\f$. Rather than couple the global step to a
local term the exchange is **limited** so the pair can never cross: at most
one third (one quarter for a quadrilateral receiver) of the amount that would
equalise the two concentrations — pairwise bounds do not compose across a
cell's faces — and at most the \f$\beta\f$ share of the giver's mass the
volume flux is held to; the signed temperature row takes the equalisation
bound alone. A face on which the limiter binds is **counted** and reported
as a warning at the end of the run: the physics stayed bounded, but the
dispersion rate there was less than asked for. With `DISPERSION 0` the block
is not entered.

### 10.2.5 Sources, sinks and the reserved rows

| Pathway | Pollutants and MSX | Age | Temperature |
|---|---|---|---|
| Rainfall | At the `[POLLUTANTS]` rain concentration; MSX rows rain in clean | Age 0 | The `RAINFALL` source temperature of `[HEAT_SOURCES]` |
| Evaporation | Removes water and **no** solute — the concentration rises | Removed at the cell's mean age | Removed at the cell's mean temperature |
| Infiltration | Removes mass at the cell concentration; booked to a named ledger | Same | Same |
| Boundary outflow | Leaves at the cell concentration | Same | Same |
| Boundary inflow | At the `[2D_BOUNDARY_QUALITY]` value, else clean | Same | Same |
| 1D coupling | §10.4 | §10.4 | §10.4 |
| Washoff | §10.7 | Does not build up | Does not build up |

Age and temperature are carried as age-volume and temperature-volume, so for
them the evaporative sink is applied at the cell's own mean — leaving either
untouched while the volume fell would age or heat the cell by evaporating it.

### 10.2.6 Aging and reactions

After each advance the age row gains exactly \f$\Delta t\,V_i\f$, so every
wet cell's mean age grows by the step — Chapter 9's law. The reaction stage
then runs on a concentration view of the **wet** cells (volume above
`DRY_DEPTH`·area) through the same `reactArdStage` the Eulerian 1D engine
uses: pollutant first-order decay by exact exponential, booked to the reacted
ledger, and multi-species kinetics in **pipe** scope — an overland cell is a
flowing element, not a tank — against the cell's own temperature row where
heat is on. Dry cells are skipped; the age and temperature rows do not react.

<!-- source: src/engine/2d/data/SurfaceTransportState.hpp:17-50, 69-100, 137-143, 178-222; src/engine/2d/solver/ExplicitInertialSolver.cpp:158-168, 301-316, 318-400, 1163-1275, 1611-1637; src/engine/2d/SurfaceRouter2D.cpp:764-794, 1789-1840, 1892-1903; src/engine/2d/input/SectionHandlers2D.cpp:120-125; plans/transport/OVERLAND_TRANSPORT_HEAT_MSX_PLAN_2026-09-01.md:51-125, 139-169 -->

## 10.3 Initial and Boundary Quality

Two sections seed and force the rows; both may be placed in the `.inp` or in
the external mesh file named by `[2D_MESH_FILE]`.

**`[2D_INITIAL_QUALITY]`** (@ref engine_manual_sect_2D_INITIAL_QUALITY) sets
the starting concentration by scope — `*` for the whole mesh, `TAG name`,
or `CELL n` with a **1-based** index, triangles first then quadrilaterals. A
species is named as it appears in the row list: a pollutant id, a reaction
species id, `__WATER_AGE__` (authored in **hours**, carried in seconds) or
`__TEMPERATURE__` (°C). Negative and non-finite values are refused rather
than clamped; a species the mesh does not carry is an open-time error.
Without a row, pollutant, MSX and age rows start at zero and the temperature
row at the `INITIAL_STATE` source temperature, as in 1D.

**`[2D_BOUNDARY_QUALITY]`** (@ref engine_manual_sect_2D_BOUNDARY_QUALITY)
gives the concentration carried by water **entering** through a boundary
edge:

```
[2D_BOUNDARY_QUALITY]
;; cell  edge  species          conc
   812   1     TSS              35.0
   812   1     __TEMPERATURE__  14.0
```

The edge is addressed by its owning cell and local edge index (0–2 on a
triangle, 0–3 on a quadrilateral). The cell index here is **0-based** — the
`TRI` column of `[2D_BOUNDARY_CONDITIONS]`, which the row mirrors — unlike
the 1-based `CELL n` scope of the other sections. The edge must be a
non-`WALL` boundary edge; a wall admits no water, so a row on one is refused
at initialization. Only inflow carries the value; outflow always leaves at the
cell's concentration. An edge with no row admits clean water — zero
concentration, age zero, zero temperature-volume. Rows in a model that
carries no transport rows are an error, not a silent no-op.

<!-- source: src/engine/2d/input/SectionHandlers2D.cpp:1121-1167, 1170-1197, 1211-1221, 1388-1393; src/engine/2d/SurfaceRouter2D.cpp:764-774, 795-822; src/engine/2d/solver/ExplicitInertialSolver.cpp:205-228, 1611-1637; docs/manuals/engine/sections/Chapter2-InputFileReference.md ([2D_BOUNDARY_QUALITY]) -->

## 10.4 Exchange with the 1D Network

Everywhere the coupling moves water between a node and the mesh, a species
tuple moves with it at the **donor's** value.

**Mesh to network.** A 2D→1D drain leaves at the cell's concentration,
accumulated per coupling point over the exchange window. At the window's end
the router hands each node the drained mass through the same queue the
volume takes, in the 1D engines' units, so mass over volume is still the
cell's concentration exactly. Rows route by kind: pollutant rows into the
node's species queue, the age row into the age-volume queue, the temperature
row into the temperature-volume queue. **MSX rows go nowhere** — the 1D node
has no reaction-species inflow accumulator for any loader yet — and their
drained mass stays in the surface's coupling-loss ledger (§10.11). A rim that
flip-flops within one window would hand the node gross mass with net water
and make a junction fed at \f$c_0\f$ read above \f$c_0\f$, so the router
pairs the window's spill against its drain first, at the node's published
value, and leaves only the net-negative remainder to the node's volume drop,
which removes it at the mixed concentration.

**Network to mesh.** A junction spill arrives at the **node's published row
values**, frozen for the batch like the node head. All three 1D engines
publish `nodes.conc`, the reaction system its node concentrations, the age
and heat modules their node states, so the coupling reads one array and is
engine-independent; nothing is queued back, since the node's CSTR mix already
takes the spill at the mixed concentration. An outfall discharging onto the
mesh carries its published values through a per-cell mass-rate density that
scatters exactly as its volume does. Water arriving this way, or through an
inflow boundary, is not runoff the cell produced (§10.7). Every pathway is
ledgered per species.

<!-- source: src/engine/2d/solver/ExplicitInertialSolver.cpp:318-347, 386-400, 1719-1766; src/engine/2d/SurfaceRouter2D.cpp:775-783, 1339-1366, 1487-1549, 1761-1787; src/engine/2d/data/SurfaceTransportState.hpp:102-121, 145-176, 224-241; plans/transport/OVERLAND_TRANSPORT_HEAT_MSX_PLAN_2026-09-01.md:90-100 -->

## 10.5 Coverages

Chapters 3 and 4 (@ref quality_ref_ch3_pollutant_buildup,
@ref quality_ref_ch4_surface_washoff) describe buildup and washoff on a
subcatchment through its land-use coverages. The mesh adopts the same
convention: land uses are **not** redefined; a cell only says which of the
model's `[LANDUSES]` cover it and by what percent, and the relations come
from the same `[BUILDUP]` and `[WASHOFF]` tables.

```
[2D_COVERAGES]
;; scope          LANDUSE      PERCENT  [LANDUSE PERCENT] ...
*                 Residential  60       Commercial 25
TAG  street       Street       100
CELL 812          Park         100
```

The ladder is `*`, then `TAG`, then `CELL` (1-based), later scopes
overriding earlier ones (@ref engine_manual_sect_2D_COVERAGES). A row
**replaces** the whole coverage set for its scope rather than merging —
exactly as a subcatchment's `[COVERAGES]` line does — so `CELL 812` above
carries `Park` alone. Percentages per scope sum to at most 100; the
remainder carries no land use. An unknown land use, a tag matching no cell,
or a cell beyond the mesh is an open-time error, as is a model with no
`[LANDUSES]` or no transported rows. The rows are **inert with a warning**
under `[2D_OPTIONS] RAINFALL_MODE NONE` — no rain falls on the mesh, so the
subcatchments own the surface — and when loadings or curb lengths are given
without any coverage.

**Ownership.** A subcatchment whose footprint is meshed and rained on already
sends its own washoff to its outlet; cells under it that also carry coverages
would load the same storm twice. At open, every covered cell whose centroid
lies inside a subcatchment polygon that itself has `[COVERAGES]` is reported
in **one** warning naming the subcatchments, and the run proceeds.

<!-- FIGURE: quality_ch10_cell_buildup_washoff — one mesh cell with two land-use slices (60 % / 25 % / 15 % bare); buildup stores drawn per slice; arrows for runoff-step accrual, a sweeping event, and washoff into the cell's species row, with the row then advecting to a coupled node -->

<!-- source: src/engine/2d/quality/SurfaceQuality2D.hpp:17-64, 100-115; src/engine/2d/quality/SurfaceQuality2D.cpp:93-137, 166-189, 238-327, 384-427; plans/transport/OVERLAND_TRANSPORT_HEAT_MSX_PLAN_2026-09-01.md:329-356, 388-393 -->

## 10.6 Buildup

Buildup is held per (cell, land use, surface species) in **user mass per
normalizer unit**, the quantity a subcatchment's store holds, where the
normalizer is the covered land area or the covered curb length:

| | | | |
|---|---|---|---|
| \f[N_{c,l} = f_{c,l}\,A_c \quad (\text{per area}), \qquad N_{c,l} = f_{c,l}\,L_{c} \quad (\text{per curb})\f] | | (10-4) | |

with \f$f_{c,l}\f$ the coverage fraction, \f$A_c\f$ the cell area in acres
or hectares and \f$L_c\f$ its curb length in project units. The functions
are Chapter 3's, unchanged: power, exponential and saturation accrue through
the inverse-days form the subcatchment step uses — the store is converted
back to an equivalent age, the step added, the function re-evaluated — and
`EXTERNAL` integrates its series rate, capped at the maximum. Accrual runs at
the **runoff step**, right after the subcatchment surface-quality step, so
cells and subcatchments share one calendar.

The initial store follows the subcatchment rule. A `[2D_LOADINGS]` row
(@ref engine_manual_sect_2D_LOADINGS), user mass per unit land area in the
same scope grammar, seeds the store directly — for a per-curb land use it is
converted by the cell's area-to-curb ratio. Without a row, the buildup
function is evaluated at `[OPTIONS] DRY_DAYS`. A loading may name a pollutant
or a reaction species; age and temperature do not build up. A land use whose
buildup is `PER_CURB` needs a curb length on every cell it covers, from
`[2D_CURB_LENGTH]` (@ref engine_manual_sect_2D_CURB_LENGTH), same scope
ladder, length in project units; a covered cell without one is an
**open-time error**, refused rather than silently given zero curb.

<!-- source: src/engine/2d/quality/SurfaceQuality2D.cpp:69-77, 139-164, 195-236, 329-382, 445-471, 543-569; src/engine/2d/quality/SurfaceQuality2D.hpp:105-115, 146-148 -->

## 10.7 Washoff

The washoff laws of Chapter 4 need a **runoff rate** for the cell, and the
right definition took two attempts. The one implemented is the cell's *net*
outflow over the runoff step, per unit area:

| | | | |
|---|---|---|---|
| \f[q_c = \frac{\max\!\left(0,\ V_{\mathrm{out},c} - V_{\mathrm{in},c}\right)}{A_c\,\Delta t_{\mathrm{runoff}}}\f] | | (10-5) | |

where \f$V_{\mathrm{out}}\f$ is what left across the cell's faces, open
boundary edges and drains to nodes, and \f$V_{\mathrm{in}}\f$ what arrived
across faces, from node spills, outfall discharge, prescribed inflow and
inflow boundaries — accumulated by the marcher at every site the species
ledgers already book, so the water side and the runoff side cannot disagree.
Rainfall is *generation*, not inflow, and is not subtracted; infiltration is
not runoff and is not added. By continuity the positive part is rain excess
plus storage release — the runoff the cell **itself** produced, the analogue
of the subcatchment's outflow over area: zero on a still pond, zero on a cell
that only passes upstream water through, and independent of how finely a
plane is meshed. A gross outward face flux was tried and withdrawn — it
counts the same water once per cell it crosses — and rainfall excess alone
rejected because it stops washing during the recession.

Each covering land use then applies its own relation per surface species,
with the subcatchment step's unit conventions and minimum-runoff threshold:
**`EXP`**, \f$W = C_1\, q_c^{\,C_2}\, B\, N\f$; **`RC`**,
\f$W = C_1\,(q_c A_c)^{C_2}\f$; **`EMC`**, \f$W = C_{\mathrm{emc}}\, q_c A_c\f$.
`EXP` and `RC` are capped at the buildup available; `EMC` is not, and where
no buildup function exists the mass it removes is booked to the buildup
ledger so the closure holds. BMP removal is taken before delivery. What
remains is **added to the cell's species row** and booked to the washoff
ledger, from where it advects with the following steps' water and reaches
the 1D node through the tuple of §10.4; every law reduces the store. The
washoff of step \f$n\f$ is computed from the runoff accumulated *during* step
\f$n\f$ and enters the rows at its end, one runoff step behind the water
that produced it, and a hydrograph's last washoff stays on the drying cell
until the next event. Co-pollutant fractions are not mirrored on cells.

<!-- source: src/engine/2d/quality/SurfaceQuality2D.hpp:42-61, 182-184; src/engine/2d/quality/SurfaceQuality2D.cpp:52-55, 489-521, 571-600; src/engine/2d/solver/ExplicitInertialSolver.cpp:325-328, 386-392, 1179-1188, 1621-1624, 1746-1749; src/engine/2d/data/SurfaceTransportState.hpp:152-166; plans/transport/OVERLAND_TRANSPORT_HEAT_MSX_PLAN_2026-09-01.md:357-367, 419 -->

## 10.8 Street Sweeping

Sweeping follows the `[LANDUSES]` schedule — interval and removal per land
use — with one *days since last swept* counter per (cell, land use). The
counter advances by the runoff step on days that are **dry** and inside the
season given by `[OPTIONS] SWEEP_START` and `SWEEP_END` (a season wrapping
the year end is handled); when it reaches the interval it resets and a
sweeping event removes, per surface species,

| | | | |
|---|---|---|---|
| \f[\Delta B = B \cdot \frac{R_{\mathrm{removal}}}{100} \cdot \frac{E_{\mathrm{sweep}}}{100}\f] | | (10-6) | |

with \f$R_{\mathrm{removal}}\f$ the land use's removal percentage and
\f$E_{\mathrm{sweep}}\f$ the species' sweeping efficiency from `[WASHOFF]`,
booking the removed mass to the sweeping ledger. This is the subcatchment
rule with the cell index in place of the subcatchment index; both counters
start at zero, so a cell and a subcatchment with the same land use sweep on
the same day.

<!-- source: src/engine/2d/quality/SurfaceQuality2D.cpp:498-506, 522-534, 602-606; src/engine/core/SWMMEngine.cpp:2670-2722 -->

## 10.9 MSX Species on the Surface

Surface species are the **union** of the `[POLLUTANTS]` rows and the reaction
system's species. `[BUILDUP]`, `[WASHOFF]`, `[LOADINGS]` and `[2D_LOADINGS]`
accept a reaction species name as `[INITIAL_QUALITY]` and `[INFLOWS]` do,
and the cell store is indexed by surface species from the outset —
pollutants, then reaction species, the row order the policy fixes — so
nothing in the arithmetic is pollutant-shaped. The one species-dependent
input is the unit factor from concentration-mass to user mass: for a
pollutant, the `[POLLUTANTS]` units as in Chapter 4; for a reaction species,
its declared units — `MG` takes the mass conversion, `UG` a thousandth of
it, anything else (`MMOL`, counts, dimensionless) is carried as-is, the
pollutant `COUNTS` rule. Three pollutant-only mechanisms are deliberately
not mirrored: co-pollutant fractions, rain concentration (reaction species
rain in clean), and kinetics inside the dry store — a built-up reaction
species is inert until washed into water, where it reacts under the normal
pipe-scope expressions per cell. The `WALL` and node-accumulator limits of
§10.1 and §10.4 apply.

<!-- source: src/engine/quality/MsxSurfaceQuality.hpp:17-50; src/engine/quality/MsxSurfaceQuality.cpp:17-25, 64-71; src/engine/2d/quality/SurfaceQuality2D.cpp:262-285; src/engine/2d/SurfaceRouter2D.cpp:739-746, 1519-1523; plans/transport/OVERLAND_TRANSPORT_HEAT_MSX_PLAN_2026-09-01.md:429-496 -->

## 10.10 Reporting

**The `.rpt` file.** The transport matrix of §10.1 is printed in the header.
When coverages are active, a **2D Surface Washoff Summary** follows the
subcatchment summaries: one row per surface species with the initial
buildup, net buildup, mass washed off into the cell rows, mass swept, BMP
removal and the store still on the mesh, in user mass (a reaction species
declared in `UG` or as a count keeps its own unit), with the cell and
land-use counts and the per-acre or per-hectare basis in the heading;
"washed off" is what entered the rows, not a node load.

**The `.h5` results file.** The `REPORT_2D_VARIABLES` tokens `SPECIES` and
`BUILDUP`, both in the `DEFAULT` set, add two time-indexed variables;
`REPORT_2D_SPECIES` narrows the rows written and `REPORT_2D_STEP` sets the
cadence. The `.out` format has no 2D record.

| Variable | Shape | Units | Notes |
|---|---|---|---|
| `Mesh2_face_species_conc` | time × species × cell | species units | Created on the first step that carries rows; `species_names` lists the rows written; `__WATER_AGE__` in hours |
| `Mesh2_face_buildup` | time × species × cell | user mass per acre or hectare | Pollutant and reaction-species rows; `species_names` and `units` attributes |

**The C API.** Coverage, loading and curb-length rows are readable and
editable through `openswmm_sq2d.h`, sharing the file's parsers so the
grammar exists once, and per-cell buildup is read with
`swmm_2d_get_buildup_bulk`. Not in this release: a `Mesh2_face_washoff`
variable, hotstart carry of the store and sweeping counters, per-species
continuity rows for the mesh in the `.rpt` (the 2D continuity block reports
water only), and Python and MCP mirrors of the coverage API.

<!-- source: src/engine/plugins/DefaultReportPlugin.cpp:562-572, 1847-1884; src/engine/2d/data/Report2DVars.hpp:27-35, 61-90, 120-125; src/engine/2d/data/SolverOptions2D.hpp:186, 193, 196-201; src/engine/2d/output/Default2DOutputPlugin.cpp:740-760, 777-804, 834-855; include/openswmm/engine/openswmm_sq2d.h:57-140; src/engine/2d/SurfaceRouter2D.cpp:1896-1903; plans/transport/OVERLAND_TRANSPORT_HEAT_MSX_PLAN_2026-09-01.md:382-384 -->

## 10.11 Alternatives

| Formulation | Status | Where it stands |
|---|---|---|
| Mass-form state, donor-cell advection on the marcher's face fluxes (S1) | \status{Implemented} | The only advection scheme on the mesh |
| Isotropic explicit dispersion with the max-principle limiter (S2) | \status{Implemented} | `[2D_OPTIONS] DISPERSION`; binds counted and warned |
| Initial and boundary species; rainfall and evaporation rules (S2) | \status{Implemented} | §10.3, §10.2.5 |
| 1D↔2D species, age and temperature tuple (S3) | \status{Implemented} | §10.4; reaction-species drain not received by the node |
| Age, temperature and per-cell reactions (S4) | \status{Implemented} | §10.2.6 |
| Cell coverages, buildup, washoff and sweeping (S7) | \status{Implemented} | §10.5–10.8; the meshed-vs-lumped band gate is open |
| Reaction species that build up and wash off (BW-MSX) | \status{Implemented} | §10.9 |
| Limited second-order advection on the mesh | \status{Planned} | The plan placed it in S2 behind the 1D key names |
| Anisotropic dispersion tensor | \status{Planned} | Deferred until a field case shows the difference |
| Per-cell surface heat fluxes and shading (S5), then a cell-resolved bed layer | \status{Planned} | The `CELL2D` token exists; no flux evaluation on cells |
| GPU transport kernels (S6) | \status{Planned} | Rows run on the CPU marcher |
| `WALL` species on the mesh; node-side MSX inflow accumulator; kinetics in the built-up store | \status{Planned} | Recorded seams of §10.1, §10.4 and §10.9 |
| `Mesh2_face_washoff`, hotstart carry, 2D species continuity rows | \status{Planned} | §10.10 |

<!-- source: plans/transport/OVERLAND_TRANSPORT_HEAT_MSX_PLAN_2026-09-01.md:220-259, 276-302, 403-413; plans/transport/TWOD_TRANSPORT_PLAN.md:258-311; src/engine/data/HeatOverrideData.hpp:81-103; src/engine/2d/SurfaceRouter2D.cpp:850-855, 1519-1523 -->

## 10.12 Implementation

The transport state is `src/engine/2d/data/SurfaceTransportState.hpp`; the
species flux, dispersion, sinks and sources are booked inside the marcher,
`src/engine/2d/solver/ExplicitInertialSolver.cpp` (`bookFaceSpecies`,
`sinkMassAtCellConc`, `addRainMass`, `sinkIntensiveRowsWithEvap`,
`addCouplingSourceMass`). The router, `src/engine/2d/SurfaceRouter2D.cpp`,
sizes the rows from the policy, resolves the initial and boundary rows,
publishes the node row values, hands the drained tuple to the 1D queues and
runs the aging and reaction stage. The policy is
`src/engine/transport/TransportPolicy.cpp`. Coverages, buildup, washoff and
sweeping are `src/engine/2d/quality/SurfaceQuality2D.cpp`, stepped from
`SWMMEngine::stepRunoff` after the subcatchment and reaction-species surface
steps (`src/engine/quality/MsxSurfaceQuality.cpp`). The `.h5` variables are
written by `src/engine/2d/output/Default2DOutputPlugin.cpp`, the `.rpt`
blocks by `src/engine/plugins/DefaultReportPlugin.cpp`.

<!-- source: src/engine/core/SWMMEngine.cpp:143, 2243-2260; src/engine/2d/SurfaceRouter2D.hpp:262-263, 461; src/engine/2d/quality/SurfaceQuality2D.hpp:117-160; include/openswmm/engine/openswmm_sq2d.h:57-140 -->
