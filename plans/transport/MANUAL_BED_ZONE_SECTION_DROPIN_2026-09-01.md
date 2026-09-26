# Manual drop-in — the bed / hyporheic zone (H6b)

**For the docs owner.** Chapter 9 (`Chapter9-WaterAgeAndHeatTransport.md`)
is untracked, in-progress work in the shared tree, so this section is
delivered as a drop-in rather than edited into your file (shared-tree
protocol: one author per uncommitted file). Suggested placement: a new
§9.3.11 before the parameter reference, which then renumbers to 9.3.12+.
The Chapter 7 note at the bottom is one paragraph for its transient-storage
discussion. Everything below describes VALIDATED behaviour (engine
`89310068`, evidence in `tests/output/h6b_bed_exchange/`).

---

### 9.3.11 The bed / hyporheic zone (`SEDIMENT_EXCHANGE`)

Beneath each conduit the model can carry a second, immobile body — a layer
of saturated sediment with its own temperature and its own dissolved
concentration of every transported species. Enabling `SEDIMENT_EXCHANGE`
under `[HEAT_FLUXES]` activates three couplings:

1. **Bed conduction.** Heat conducts between the water column and the bed
   across their contact area. The contact area is the **wetted perimeter**
   times the conduit length (times barrels) — not the free-surface width —
   so a full or surcharged pipe, whose free surface is zero, still
   exchanges with the ground around it. This is deliberately different
   from open-channel transient-storage models, where the two areas
   coincide.
2. **Deep-ground conduction.** The bed conducts to a boundary at depth
   `GROUND_DEPTH` held at `GROUND_TEMPERATURE` (a constant, or a
   `TIMESERIES` for a seasonally varying deep-soil temperature). This is
   the only true source or sink in the system: every other term moves
   energy between two bodies the model owns, and their sum is conserved.
   For a buried conduit this is often the largest term in the energy
   balance, which is why an unstated `GROUND_TEMPERATURE` draws a warning.
3. **Hyporheic exchange.** Water cycles between the channel and the bed
   pore space at `Q = HYPORHEIC_VELOCITY × A_bed`, carrying heat and every
   dissolved species with it. With the velocity at its default of zero the
   bed is conduction-only and solutes exchange by diffusion alone
   (`SOLUTE_DIFFUSIVITY`); with both zero the bed is a purely thermal
   feature.

The water/bed pair is integrated as one coupled system by the exact
matrix exponential of its linearization — the two-body extension of the
semi-implicit step of §9.3.9. The pair cannot oscillate and cannot
overshoot at any routing step, and with the bed conductances at zero it
reduces exactly to the single-body step. Solute exchange is solved in
(total, difference) coordinates, so the summed mass of channel and bed is
conserved by construction.

The bed zone is available under the LEGACY quality solver. Under
`EULERIAN_ARD` and `LAGRANGIAN` the configuration is refused with a named
warning at open (the mapping of a per-link bed onto cells or parcels is a
modelling decision still owed); no silent partial application occurs.

`[SEDIMENT_EXCHANGE]` parameters (all `GLOBAL <value>` rows):

| key | default | units | notes |
|---|---|---|---|
| `THERMAL_DIFFUSIVITY` | 1.0e-6 | m²/s | saturated-sand value; > 0 |
| `SOLUTE_DIFFUSIVITY` | 1.0e-9 | m²/s | 0 = advective exchange only |
| `BED_THICKNESS` | 0.20 | m | conduction length AND bed heat capacity; > 0 |
| `GROUND_DEPTH` | 2.0 | m | bed to deep-ground boundary; > 0 |
| `GROUND_TEMPERATURE` | 12.0 | °C | constant or `TIMESERIES <name>`; warned when unstated |
| `HYPORHEIC_VELOCITY` | 0.0 | m/s | exchange velocity across the interface; ≥ 0 |
| `SEDIMENT_DENSITY` | 1670 | kg/m³ | streambed bulk density |
| `SEDIMENT_SPECIFIC_HEAT` | 1807 | J/kg/K | streambed sediment |
| `INITIAL_TEMPERATURE` | = ground | °C | bed's starting temperature |

Zero or negative values for lengths, diffusivities and material
properties are refused at parse — each is a divisor or a capacity, and a
silently repaired zero produces an infinite conductance or a massless bed
that tracks the water exactly, both of which look like a working model.

Example:

```
[HEAT_FLUXES]
SEDIMENT_EXCHANGE      ON

[SEDIMENT_EXCHANGE]
GROUND_TEMPERATURE     GLOBAL 5.0
HYPORHEIC_VELOCITY     GLOBAL 1.0e-4
```

---

### Chapter 7 note (transient storage)

For the §7 dispersion discussion: with `SEDIMENT_EXCHANGE` on, the bed
zone acts as a transient-storage zone for solutes as well as a thermal
mass — dissolved constituents diffuse (and, with a hyporheic velocity,
advect) into the bed pore water and return, which adds the long-tail
retention that pure in-channel dispersion cannot reproduce. The bed's
storage volume is `wetted perimeter × length × BED_THICKNESS` per
conduit, and its exchange discharge is
`SOLUTE_DIFFUSIVITY·A_bed/BED_THICKNESS + HYPORHEIC_VELOCITY·A_bed`.
