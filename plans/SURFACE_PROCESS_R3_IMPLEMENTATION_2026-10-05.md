# R3 mesh ET budget and defaults — implementation and migration

The user approved the concrete ET/default/GUI design with “proceed” on 2026-10-05. This implements the mesh ET/default tranche of the original surface–subsurface program. It does not qualify shared subcatchment/LID/snow area accounting or complete the original program. The existing LID model and optional semi-discrete Richards solver remain available as previously implemented.

## Numerical contract

Each active mesh aquifer cell records potential atmospheric demand `P = max(PET,0) A dt` at the actual surface source interval. It subtracts actual surface evaporation `E_surface`, including the existing surface donor sharing, and accumulates `P − E_surface` as pending soil demand. A groundwater firing consumes that past remainder once. Refreshing forcing or publishing infiltration does not reset or regenerate demand. Unspent demand after stress/donor limits expires as unused demand; it does not accumulate into a later interval. A final past interval may still be pending when a run ends.

For every cell the cumulative accounting identity is:

`potential = actual surface + actual soil + expired unused + pending`.

Actual surface plus soil withdrawal cannot exceed recorded potential. Potential demand is not water storage and is excluded from the water-continuity ledger. Capillary rise remains an internal saturated-to-unsaturated transfer and is excluded from atmospheric loss.

Stress is evaluated once: actual top-layer suction for SIGMA, actual bulk retention suction for CLOSED_FORM, and surface equilibrium suction `zs − hg` for ENSLAVED. The soil donor cap is independent of stress. CLOSED_FORM reserves positive downward drainage before ET. ENSLAVED uses its exact equilibrium storage relation and reserves committed node/lateral/conduit transfers before ET. Internal capillary rise similarly spends the saturated water remaining after committed exchanges. This fixes a measured node-side refund defect exposed by enabling BOTH: the aquifer may not disown water already delivered to a node. A few ulps are retained when a limiting internal flux binds to prevent a roundoff-only refund.

SIGMA reports the actual ET left after outgoing-flux positivity sharing, rather than its unscaled request. Its first-order upwind spatial scheme is unchanged. `M_LAYERS` is a spatial layer count; `LTS_TIERS` controls firing cadence. Neither setting is a proxy for the other. This round adds nine per-cell double arrays (72 bytes/cell) and constant work per surface booking; it does not introduce a new ODE solver or matrix solve.

## Migration

| Stored choice | Effective active mesh behavior | Compatibility |
|---|---|---|
| GW_ET absent / AUTO | BOTH | Intentional new default; requires climate or explicit mesh evaporation forcing. |
| GW_ET NONE | No soil ET or optional internal rise | Explicit opt-out preserved, including GeoPackage input. |
| BOUNDARY_ET / CAPILLARY_RISE / BOTH | Authored process choice | ET now shares past demand and uses one reviewed stress threshold. |
| LINK_SEEPAGE absent / DEFAULT | TWO_WAY | Intentional signed, head-driven default. |
| Legacy explicitly authored LINK_SEEPAGE AUTO | ONE_WAY | Old one-way law retained; canonical save spelling is ONE_WAY. |
| ONE_WAY / TWO_WAY / NONE | Authored choice | Explicit choices retained. NONE keeps legacy conduit loss outside aquifer delivery. |
| GROUNDWATER NO | Spatial aquifer inactive | Rows remain authored; mesh uses legacy surface configuration. |
| PER_SUBCATCH automatic controls | ET NONE, link ONE_WAY | Existing non-mesh default behavior retained. |

`WILTING_SUCTION AUTO` means 150 m (492.125984 ft), independently of the project unit system. A custom strictly positive finite value uses project length units and is converted once to SI on initialization. The automatic value preserves the former bulk threshold; applying it to SIGMA replaces its former soil-law proxy. This is a vegetation modeling assumption, not a universal calibration. Alpha and conductivity retain their existing conventions.

Mesh kernel cells use the reviewed stress/wilting controls in place of lumped UEF/LED and have no “infiltration must be positive” ET gate. Ordinary lumped `[GROUNDWATER]` retains its legacy parameterization. Models relying on soil withdrawal beneath continuously ponded cells will change: surface evaporation can exhaust the shared demand. Actual initial/final asynchronous bookings can also change explicit BOTH dry trajectories because the old path spent instantaneous demand independently.

## Authoring, restart and results

The atomic process-options C API validates all three choices before replacing them. Section-authoring provenance is restorable for Undo. Automatic values are not materialized as explicit per-cell rows. Option-only aquifer activation survives save/reload even when every value is automatic. Explicit NONE is written instead of being suppressed as a former default.

V13 hotstart appends pending/cumulative demand, actual surface/soil withdrawal, unused demand, held rates, stress, demand refresh, the surface held evaporation rate and its loss total. Dimensions are validated before state mutation; malformed V13 state is refused. V1–V12 remain readable; V12 warns that it cannot continue an ET budget it never stored. The controlled restart compares subsequent groundwater firings, not whole-network clock/forcing resumption across arbitrary periods.

Nine additive result fields provide pending demand, cumulative potential/surface/soil/unused amounts (m³), held surface actual/potential rates (m/s), stress (1), and interval refresh time (s). Existing `gw_et` remains actual soil ET; the groundwater ledger retains separate internal capillary-rise transfer. New snapshot fields are appended. Initial unreported rates/stress remain missing, while a reported zero remains a valid value. The existing GUI Results catalog, map scalars and selected-cell series discover these fields.

The GUI options page provides Automatic/custom wilting input with project units. Assign Groundwater offers “Review ET and seepage defaults,” showing stored/effective before/after settings over implicit whole-mesh coverage. Apply validates the aquifer snapshot, groundwater enable state, project units and mesh context, then writes one configuration and one Undo command. Cancel and stale preview write nothing. The general options dialog shares the same atomic settings but is not the migration Undo workflow.

## Verification

Evidence is under `tests/verification/surface_r3_2026-10-05/` and GUI `tests/gui/data/surface_r3_out/`.

- Fresh isolated committed R2 baseline plus only R3 changes: **136 engine tests** (10 ET, 16 receiving, 21 infiltration, 23 aquifer, 36 hotstart, 30 transport) and all groundwater gates pass. The shared engine passes **139**, including three unrelated local hotstart tests.
- ET gates cover all three closures and four soil laws, independent single-stress expectations, ponded/partial/dry donors, changed forcing/cadences, no future demand, US/SI custom suction, invalid atomic edits, exact equilibrium ET competing with a committed node debit, full-model independent `PET × area × duration`, and pending-demand restart without replay. Existing transport gates verify retained solute mass and water-age removal policy; bed exchange gates run under the new automatic BOTH default.
- **25/25 explicit corpus binary outputs** and **24/24 no-aquifer/infiltration-OFF API trajectories** remain byte-identical to the separately frozen R2 baseline. This is the current explicit manifest, not a historical recursive deck count.
- The **20-case aquifer manifest** records **10 intentional changes and 10 unchanged cases**. Six explicit old ET/seepage opt-outs and the explicit legacy one-way conduit retain exact measured metrics. Automatically enabled dry soil ET, shared ponded BOTH demand, single-stress/cadence changes, and the automatic gaining conduit are attributed separately. Every measured aquifer continuity residual meets its stated bound.
- **96 shared GUI checks** pass across assignment helpers/dialog, cell descriptors, HDF5 reader and options gates. These include one-command Apply/Undo/Redo, cancellation, stale aquifer/unit state, automatic/custom US/SI idempotency, discovery of actual native ET output and its accounting identity. Shared GUI checks include unrelated pre-existing cases; surgical commits exclude those changes. The review screenshot was inspected and clipped explanatory text repaired.

R2 conflict hatching/combined comparison-panel refinements remain pending. R4/R5 must review surface-water ownership and overlapping pervious/LID footprint partition before coupling those atmospheric budgets. UEB radiation/reference qualification, snow coverage and Richards aquifer-bottom adaptation remain later work.
