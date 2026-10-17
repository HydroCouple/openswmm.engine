# Mesh thermal model

The coupled mesh thermal model is explicitly selected with `HEAT_MODEL COUPLED`
in `[2D_OPTIONS]`. An absent key means `TRANSPORT_ONLY`: existing temperature
transport and existing 1D/lumped heat calculations retain their behavior.
`HEAT_TRANSPORT` and mesh temperature transport must also be enabled. Child
settings remain saved when a parent is off.

The current coupled runtime is experimental. Restart, reciprocal snow/LID
ownership and porous-ground coupling are not yet qualified. Active unsupported
owner combinations are refused. Query `swmm_heat_get_2d_status` for the actual
activation and qualification reason; a saved switch is not runtime evidence.

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
active aquifer. `GROUND_COLUMN` is reserved for the porous-ground integration
and currently causes an actionable initialization refusal.

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

Balanced hyporheic recirculation adds no net water. It is eligible by default
only on `channel:*` cells created by channel burning. A positive explicit
`HYPORHEIC_VELOCITY` at `MESH_TAG` or `CELL2D` scope opts other cells in. A
resolved zero velocity disables recirculation while retaining conduction.
Burned reaches are 2D-owned; they must not receive an additional routed 1D bed.

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
The diagnostic residual is bed-energy change plus the water thermal increment
minus atmospheric and deep energy. Water advection is excluded from that local
operator residual. Internal water/bed exchange is never an additional external
source. Missing/inactive bed values produce a lifecycle error.

Dry-water HOLD/AIR/DEFAULT affects the reported value only; it cannot overwrite
retained bed energy. No temperature floor changes conserved energy.
