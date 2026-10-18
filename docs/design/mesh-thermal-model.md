# Mesh thermal model

The coupled mesh thermal model is explicitly selected with `HEAT_MODEL COUPLED`
in `[2D_OPTIONS]`. An absent key means `TRANSPORT_ONLY`: existing temperature
transport and existing 1D/lumped heat calculations retain their behavior.
`HEAT_TRANSPORT` and mesh temperature transport must also be enabled. Child
settings remain saved when a parent is off.

The current coupled runtime is experimental. Restart and reciprocal snow/LID
ownership are not yet qualified. The fixed-grid ground and shared surface-column
paths have focused numerical/runtime tests; the full release matrix remains open. Active unsupported
owner combinations are refused. Query `swmm_heat_get_2d_status` for the actual
activation and qualification reason; a saved switch is not runtime evidence.

`[GW_TRANSPORT_OPTIONS] THERMAL_MODEL POROUS_EQUILIBRIUM` explicitly selects
finite soil-plus-water storage beneath an active spatial aquifer. Its default
is `WATER_ONLY`, including old files with `CONDUCTION YES`. New thermal layers
have fixed depths; their count is `THERMAL_LAYERS`, independently of the hydraulic
SIGMA count. Moisture is sampled by overlap for SIGMA and uniformly within the
bulk unsaturated store for CLOSED_FORM/ENSLAVED. Accepted water transfers move
their paired sensible heat; geometric water-table handover creates no energy.
The fixed-grid vertical flux reconstruction uses layer continuity, giving a
documented piston attribution for bulk hydraulic closures.

Porous temperature-volume rows are water-property views of the canonical soil
energy. They are not extra energy stores. Material density and specific heat in
`GW_TRANSPORT_PARAMS` describe grains. Conductivity uses arithmetic or geometric
phase mixing and resistance-weighted interfaces. Solute molecular diffusivity
does not act as a second thermal conductivity; mechanical dispersion still rides
the water phase. `CONDUCTION NO` retains porous storage and accepted sensible
heat advection. `THERMAL_MAX_STEP` bounds the existing hydraulic owner clock.

`SOIL_ET_HEAT LATENT_SURFACE` debits latent heat once at bare-soil evaporation's
top boundary, using actual accepted soil evaporation. `SENSIBLE_ONLY` retains
the extraction's sensible heat and supports externally owned transpiration
energy. If surface temperature transport is disabled, infiltrating water is an
external prescribed-temperature reservoir using the existing rainfall source
temperature. Returning aquifer water and heat leave the modeled thermal domain
together. A full surface-water heat budget requires its temperature row.

`LATERAL_CONDUCTION YES` adds saturated-only conduction on the existing groundwater
face clock, using actual overlapping saturated elevations and normal distances
from each cell centroid to the face. Resistance weighting uses each material's
phase conductivity. The two-point normal approximation is not generally second
order on skew/nonorthogonal meshes. An explicit bound based on solid capacity
and maximum phase conductivity limits the sum of contact weights to at most
one half; LTS side receipts remain pending until the receiving owner fires.
No unsaturated lateral conduction is implied. The default is off.

## Surface exchange and water losses

`HEAT_SURFACE_EXCHANGE AUTO|YES|NO` and
`HEAT_RADIATIVE_EXCHANGE AUTO|YES|NO` inherit the existing global modules at
`AUTO`. Explicit choices apply only to the mesh. The radiation evaluator uses
the existing shared weather/solar inputs and mesh element forcing.

The hydrological solver remains the only owner of evaporated water. Mesh latent
cooling uses its accepted evaporation volume, including water limitation. It
does not run a competing aerodynamic evaporation water model. Sensible exchange
uses the existing wind-transfer/Bowen formulation in its continuous form at zero
vapour deficit. Radiation, sensible and latent joules are reported separately.

The thermal update runs after incoming water mixes and before source sinks
withdraw their intensive properties. It uses the existing cell source clock.
This ordering preserves the thermal donor when a cell dries during a step.
Transport already uses the existing first-order fallback for RK2 hydraulics.
Full cell area is not automatically a free-water area: the surface contact uses
the wetted area reconstructed by the hydraulic closure.

Coupled heat and runtime water heat inputs use the configured water density
times specific heat, in J/(m³ K), for both conversion and receipts. Water-only
compatibility mode retains its historical 4,186,000 J/(m³ K) runtime conversion.
Temperatures and temperature-volume rows remain signed Celsius quantities.

## Local bed and hyporheic transient storage

`BED_HEAT_MODEL NONE|LOCAL_STORAGE|GROUND_COLUMN` defaults to `NONE`.
`LOCAL_STORAGE` supplies a finite saturated bed under each mesh cell without an
active aquifer. `GROUND_COLUMN` uses the existing aquifer columns on covered
cells and standalone fixed saturated columns on uncovered cells. An aquifer
contact requires `POROUS_EQUILIBRIUM` and groundwater temperature transport.
There is no additional bed capacity over aquifer-owned soil: bed fields are
overlap-weighted views of its near-surface layers. Standalone columns span
`BED_THICKNESS + GROUND_DEPTH` below terrain, with the latter measured below
the bed; their porewater is fixed and saturated. Accepted infiltration displaces
equal water through the bottom, transferring heat through finite layers with
implicit upwind advection. The fixed moisture assumption is not a groundwater
flow solution.

Mesh bed parameters use the existing `[SEDIMENT_EXCHANGE]` authoring table.
For the new mesh model, sediment density and specific heat describe the solid
phase. `MESH_POROSITY` is a fraction in [0,1], default 0.30. Its local capacity is

```
C_b = area * bed_thickness * [(1-n) rho_s c_s + n rho_w c_w]   J/K
```

This saturated porewater storage is fixed; it is a parameterized transient
store, not resolved bedform circulation. The existing 1D bed retains its
historical effective-capacity interpretation. To compare the models, match
their capacities and conductances rather than just their raw material values.

The water/bed conductance is `k A_wet / bed_thickness + rho_w c_w v_h A_wet`,
where `k = thermal_diffusivity * sediment_density * sediment_specific_heat`.
The full bed retains energy when surface water disappears. Deep exchange uses
`k A_bed / ground_depth` against the existing prescribed ground temperature or
series. The water and bed advance together with the existing exact linearized
two-body operator; integrated atmospheric/deep receipts use the same system.

For `GROUND_COLUMN`, grain conductivity comes from the same diffusivity conversion
on standalone cells and `GW_TRANSPORT_PARAMS` on aquifer cells. The porous phase
mixing then supplies the conductive interfaces. Water, the fixed vertical chain,
actual porewater hyporheic contacts, atmosphere and deep boundaries share one
backward-Euler finite contact solve; it is first order in time. The aquifer's
separate conduction step is disabled for surface-owned columns. `CONDUCTION NO`
disables molecular conduction while retaining sensible advection and explicitly
configured hyporheic recirculation. The exposed part of a cell receives its top
boundary; the wetted part instead contacts finite water. `SURFACE_WATER` requires
this actual contact and never treats a sampled water temperature as a reservoir.
AIR uses `SURFACE_THERMAL_RESISTANCE`; fixed/series temperatures, flux/flux-series,
adiabatic and geothermal boundaries use the existing project series system.
Missing temperature-series coverage is a runtime error.

Balanced hyporheic recirculation adds no net water. It is eligible by default
only on `channel:*` cells created by channel burning. A positive explicit
`HYPORHEIC_VELOCITY` at `MESH_TAG` or `CELL2D` scope opts other cells in. A
resolved zero velocity disables recirculation while retaining conduction.
Burned reaches are 2D-owned; they must not receive an additional routed 1D bed.

## Routed-channel mesh contacts (experimental)

The existing sediment authoring table adds `GROUND_SOURCE GLOBAL|TAG|LINK`
with `PRESCRIBED` (absent/default) or `MESH_CONTACTS`. Explicit global
`PRESCRIBED` remains distinguishable from an absent key. The generic override
API uses values 0/1. Mesh-cell, mesh-tag and node scopes do not apply to this
channel selector. `SEDIMENT_EXCHANGE` and overall heat transport are its
parents; selecting a source does not enable either.

Mesh contacts require an active spatial `POROUS_EQUILIBRIUM` aquifer. The full
node/vertices polyline is clipped against every crossed triangle/quad, with
stable link/cell IDs and along-reach intervals. Shared-edge ownership is split
only on that interval. Missing coverage, coordinates, out-of-profile invert
levels, beds extending below the aquifer bottom, and explicit closed-pipe requests receive named errors. Global selection
applies to eligible open channels. Burned channels have already left 1D routing.

Channel water and each contacted soil column advance in one finite implicit
system on the existing finest source clock. When overland ground-column heat
also contacts that soil, its mixed finite water joins the same solve before
intensive sinks. One soil energy state serves multiple reaches; there is no
additional legacy bed capacity on that footprint. LEGACY uses its link store,
ARD uses actual channel cells, and LARD retains its established mean-temperature
and uniform-parcel-increment binding. A refused channel update leaves both
thermal sides unchanged.

The accepted contact joules enter soil and leave channel water with opposite
signs. `SWMM_GROUND_HEAT_CHANNEL_CONTACT` exposes the soil receipt separately
from overland-water exchange. Mapped bed geometry requires reinitialization;
live bed OFF stops these contacts while retaining soil energy, and ON resumes
prepared contacts. A selector that was dormant at initialization cannot lazily
allocate a new contact model during a run. Independent atmospheric/ground
processes retain their own switches.

This is a heat-only experimental capability. Conservative bed tracer/age
partitioning, complete footprint/atmospheric ownership, wider geometry/transfer
refinement, reciprocal snow/LID, restart and GUI release qualification remain
open. Active unqualified bed tracer/age combinations are refused. Inspect the
canonical soil/layer results for its storage; old prescribed-bed fields are
not an additional mapped-soil energy budget.

`swmm_heat_get_channel_ground_status` distinguishes a saved source choice,
initialized mapping counts, and live exchange activity. Parent OFF retains
the mapping while pausing exchange. `swmm_heat_get_channel_ground_contact`
returns zero-based link/cell/layer IDs, full-polyline fractions, local edge
share and SI invert elevation. Contact reads use a caller-supplied index,
so a UI can load only the range it displays.

`swmm_heat_get_channel_ground_values` exposes the actual soil-derived bed
temperature, atmosphere-to-channel joules, channel-to-soil joules, water
thermal increments and interval counts. The bed temperature weights the
existing soil capacity over the bed's physical depth and reach overlap;
it remains available when the channel is dry. It is a diagnostic view,
with no additional capacity or energy store. Unmapped links return NaN;
invalid ranges leave all outputs untouched and completed results survive
run end. Python provides `heat.channel_ground_status()`,
`channel_ground_contact(index)` and `channel_ground_values(kind, first, count)`.

## Spatial values and diagnostics

`CELL2D <one-based native cell ID>` and `MESH_TAG <region name>` are scopes of
the existing heat override table and C/Python API. Mesh region tags form a
separate namespace from network `[TAGS]`. Precedence is global, mesh tag, cell.
Existing network scopes and effective-value source codes retain their values;
the new scopes use 6 and 7 because 4/5 already identify dynamic air/ground data.

`swmm_heat_get_2d_values` returns SI temperature, bed energy/capacity, accepted
evaporation and signed cumulative component joules. Its zero-based cell ranges
follow the bulk runtime API convention. Positive external energy enters the
thermal system; positive water-to-bed energy leaves water and enters bed.
For local storage the diagnostic residual is bed-energy change plus the water
thermal increment minus atmospheric and deep energy. For ground columns it
combines the canonical ground residual with the water operator residual, removing
accepted ground advection/sources and counting contact receipts once. Internal water/bed exchange is never an additional external
source. Missing/inactive bed values produce a lifecycle error.

Dry-water HOLD/AIR/DEFAULT affects the reported value only; it cannot overwrite
retained bed energy. No temperature floor changes conserved energy.

`swmm_heat_get_ground_status`, `swmm_heat_get_ground_values` and
`swmm_heat_get_ground_layers` (Python `heat.ground_status()`, `ground_values()`
and `ground_layers()`) expose the actual owner counts, fixed thermal depths,
solid-plus-water joules/capacity, moisture, conductivity and separated receipts.
An uncovered cell returns NaN in a bulk ground read. Layer ranges are checked
before writing any outputs. Add pending lateral joules to canonical stored
energy when auditing a domain between LTS owner firings. The legacy groundwater
water-temperature continuity table is not a soil-plus-water energy budget.

New selectors and finite material/capacity changes require reinitialization;
existing weather and AUTO-inherited heat module switches remain live. Atomic
hydrothermal authoring edits and heat snapshots include the mesh and aquifer
configuration. Native hotstart writes/loads explicitly refuse active new thermal
stores until the thermal state extension is qualified; an existing destination
is preserved. All of these capabilities remain experimental.
