@page application_manual_ch7_mesh_hydrology Chapter 7: Rain, Infiltration and Groundwater on the Mesh

## Problem

@ref application_manual_ch6_coupled_1d2d "Chapter 6" drove a mesh with
water spilled from the network. The other way to drive one is to rain on
it directly, and then the mesh is doing hydrology: catching rainfall,
losing part of it into the ground, running the rest downhill, and handing
what reaches a drain to the network.

That raises three questions a subcatchment never has to answer. Which
gage does a cell listen to, when there are several? What does each cell
do with the water that soaks in? And where does that water go — out of the
model, into the old per-subcatchment aquifer, or into the aquifer on the
mesh itself?

## Deck

Deck: `docs/figures/decks/mesh_hydrology/mesh_hydrology.inp`

A 20 × 20 m patch of ground, meshed as eight triangles over nine
vertices, falling gently toward a drain at the centre that is coupled to
a junction. Two rain gages stand at opposite sides reading 20 and
5 mm/hr, so every cell takes a different rainfall. Three soil tags — lawn,
paved and woods — carry three infiltration methods, and a two-layer
aquifer lies beneath the whole patch. The junction is located at the drain
vertex, so the aquifer can exchange with it. One hour of rain, two hours of
recession.

The deck is written for this chapter rather than copied: it is the
smallest model that exercises all three questions at once.

## Options that select the formulation

Two gages, each with coordinates, because an un-located gage cannot be
interpolated:

\snippet mesh_hydrology/mesh_hydrology.inp gages

The mesh's own options select the rainfall mode and switch on
infiltration and the aquifer:

\snippet mesh_hydrology/mesh_hydrology.inp twod

Infiltration rows resolve per cell, by tag, with a mesh-wide fallback:

\snippet mesh_hydrology/mesh_hydrology.inp infiltration

and the aquifer beneath is configured the same way:

\snippet mesh_hydrology/mesh_hydrology.inp aquifer

The aquifer's `*` row covers every cell, and that outranks the rows above
it. A cell that an aquifer row covers belongs to the aquifer, which
publishes how much water the cell can receive, and the infiltration rows
skip it. The report counts the skips for every row:

> 2D infiltration default '*': 0 applied, 8 skipped (aquifer-owned)

On this deck the tag rows therefore change nothing. They govern only the
cells that no aquifer row covers.

**One warning worth reading.** The first version of this deck gave the
coupling point an exchange area of 1 m² and the engine said:

> 2D-coupled node 'J1' has exchange AREA 1.000 m² — 14x the largest
> connected conduit area (0.0707 m²). The orifice can inject far more than
> the pipe can convey; expect the node to fill and spill back each window.

The deck now uses 0.07 m², the pipe's own area. An exchange area is the
size of the hole between the surface and the network, not the size of the
puddle above it.

## Results

![Figure 7-1](figures/png/workflow_ch7_rain_and_infiltration.png)

*Figure 7-1 Interpolated rainfall and infiltrated depth on each cell of a meshed patch of ground: (a) rainfall while it rains (mm/hr), weighted by inverse distance from the two gages; (b) water infiltrated over the storm (mm). Simulated, `mesh_hydrology/mesh_hydrology.inp`*

**Every cell takes its own rainfall.** Panel (a): the cells nearest the
20 mm/hr gage receive 17.6 mm/hr, those nearest the 5 mm/hr gage 7.4, and
the rest lie between. Natural-neighbour weights need at least three gages
that are not in a line, so with two the engine weights them by inverse
distance in every cell, and the report says so:

> 2D rainfall: inverse-distance fallback used for 8 cells outside the gage
> hull or with degenerate weights.

The weights are positive and sum to one, so no cell can receive more than
20 or less than 5. With three gages around the patch, the cells inside
their hull would take natural-neighbour weights, which reproduce a linear
rainfall field exactly. With one gage, or with `RAINFALL_MODE SYSTEM`, every
cell would receive the same depth and the whole distinction would vanish.

**Each cell takes in what reaches it.** Panel (b): every cell belongs to
the aquifer, which can receive 250 to 575 mm/hr here, more than fourteen
times the heaviest rain. The soil never limits the intake; the water on
the cell does. The intake applies in full only above `DRY_DEPTH` (1 mm) and
ramps off below it, so each cell holds a film of 0.08 to 0.16 mm, where the
intake balances what arrives. The cells take in between 6.5 and 16.9 mm:
what falls on them, less what runs downhill, plus what runs on from
upslope. The exception is cell 0, the lowest of the cells around the drain
vertex and the one the drain draws from. The drain never lets it hold
more than 0.0073 mm, and it takes in 0.08 mm of the 15.1 mm that falls on
it.

| Term | Volume (m³) |
|---|---|
| Rainfall inflow | 5.000 |
| Infiltration loss | 3.996 |
| 2D → 1D drain outflow | 1.004 |
| Final stored volume | 0.000 |
| **Continuity error** | **0.000 %** |

*Table 7-1 The 2D surface water balance for the storm*

Four fifths of the rain went into the ground and one fifth reached the
drain. That split is the answer the mesh exists to give: a subcatchment
would have produced one runoff hydrograph with one infiltration loss, and
would not have said which part of the ground did what.

**Where an aquifer lies, the destination key does not decide.**
`INFIL_DESTINATION` routes the infiltration of the cells that no aquifer
row covers, and this deck has none. `LOST` and `SUBCATCH_AQUIFER` therefore
give the same run here, and the old value `AQUIFER_2D` is refused when the
run opens:

> 2D initialization failed: [2D_OPTIONS] INFIL_DESTINATION AQUIFER_2D is
> obsolete; remove this option. Aquifer-owned cells compute their
> receiving capacity.

**The aquifer's own state is in the same results file.** The `GROUNDWATER`
group of `REPORT_2D_VARIABLES` is part of `DEFAULT`, so a run with an
aquifer writes each cell's water-table elevation, saturated thickness,
unsaturated storage, recharge and exchange terms, the domain ledger
`groundwater_ledger`, and `groundwater_node_exchange_cum`, the cumulative
exchange through each node bed
(@ref hydrology_ref_ch9_mesh_groundwater "Hydrology 9", §9.10.3).
Figure 7-2 reads three of them back. An aquifer exchanges only with a node
it has enrolled, and `NODE_ENROLMENT AUTO` enrolls a node only when its
coordinates fall in a cell, so the deck places `J1` on the drain vertex:

\snippet mesh_hydrology/mesh_hydrology.inp coordinates

![Figure 7-2 The mesh aquifer during and after the storm](figures/png/workflow_ch7_mesh_groundwater.png)

*Figure 7-2 The mesh aquifer during and after the storm: (a) the water-table elevation of each cell (m); (b) the recharge from the unsaturated column to the table (mm/hr, positive down); (c) the aquifer's cumulative ledger (m³). Simulated, `mesh_hydrology/mesh_hydrology.inp`*

**The water that soaks in stays above the table, except at the drain.**
The aquifer took in 4.00 m³ during the hour of rain, about 10 mm over the
patch. The table starts 3 m below the ground (`ZS 4.0` with `HG0 1.0`), and
away from the drain almost all of that water is still in the unsaturated
column three hours later: recharge stays between −0.07 and +0.25 mm/hr, and
no other cell's table moves by more than 9 mm. On this deck the `AUTO` rule
resolves every cell to the σ column.

**The bed drains the aquifer into the pipe.** `J1` sits on a vertex that
six cells share, and the engine places its bed in one of them, cell 6. The
table there starts a meter above the junction's invert, so the exchange runs
out of the aquifer: 2.24 m³ reaches `J1` over the three hours, the cell's
table falls from 7.01 to 6.72 m, and its column drains toward the falling
table at up to 7.1 mm/hr. The ledger closes in panel (c): the change in
storage, 1.76 m³, is the infiltration less the exchange, and the residual
stays below 3 × 10⁻¹⁰ m³. Recharge (0.65 m³) moves water within a cell and
does not change the total.

## What this chapter does not show

The σ column's moisture profile, which the `GW_DETAILED` group writes, and
the closures and soil laws other than this deck's; the non-Gardner laws
carry \status{Experimental}. Their theory, state variables and continuity
ledger are in @ref hydrology_ref_ch9_mesh_groundwater "Hydrology 9".

## Where to go next

- @ref hydrology_ref_ch8_mesh_surface "Hydrology 8" — rainfall
  interpolation, per-cell infiltration and the mesh water balance
- @ref hydrology_ref_ch9_mesh_groundwater "Hydrology 9" — the two-layer
  aquifer this deck configures
- @ref application_manual_ch6_coupled_1d2d "Chapter 6" — the same kind of
  mesh driven by spill instead of rain
- @ref engine_manual_sect_2D_INFILTRATION_DEFAULTS,
  @ref engine_manual_sect_2D_INFILTRATION_OPTIONS,
  @ref engine_manual_sect_2D_AQUIFER_OPTIONS,
  @ref engine_manual_sect_2D_AQUIFER — the grammar of each section
