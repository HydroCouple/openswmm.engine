@page engine_manual_lid_storage CHAPTER 6 — Storage-node LIDs: Implementation and Interfaces

## Scope and model structure

A storage node may reference a LID control without becoming a new node type.
The network still connects to ordinary storage-node ports. `NODE` controls
allow an arbitrary ordered stack: SURFACE, repeated MEDIA/AGGREGATE rows,
and an optional BOTTOM seepage boundary. Supported conventional controls can
also be normalized to a storage-node stack. Layer-specific treatment is
configured on explicit NODE layers. Use Dynamic Wave routing; pollutant
routing with storage-node LIDs currently requires `QUALITY_SOLVER LEGACY`.
This implementation does not provide separate layer reactors for ARD, MSX
or heat transport.

The storage geometry supplies the footprint; layer thicknesses establish the
vertical extent and synchronize the storage maximum depth. Physical layer
numbers are one-based from the top and include SURFACE. BOTTOM is a boundary,
not an outlet or treatment layer. A control can be shared by several storage
nodes; editing it updates those assignments.

## Multiple connections and independent controls

A conventional subcatchment LID control has one underdrain definition, one
opening/closing threshold pair and an optional head-based discharge multiplier
curve. That definition can represent several physical drain pipes but supplies
one combined drain response. A storage-node LID accepts an arbitrary number
of ordinary hydraulic links at different elevations. Each controllable link
has its own control-rule actions. An orifice representing a valve accepts
settings from 0 (closed) through partial openings to 1 (fully open); this
specifies opening, not a linear fraction of discharge.

Use separate links for a basal drawdown valve, an intermediate treatment outlet
and a high-level emergency overflow. Set their offsets or layer anchors
independently. Dynamic Wave evaluates the head at both endpoints and permitted
flow reversal. The extension connects layered LID physics to existing SWMM
network controls; it does not replace the conventional underdrain thresholds.

## Configuration and units

The following stack has a total thickness of 24 inches (2 ft):

```text
[LID_CONTROLS]
Stack NODE
Stack SURFACE 6 .1
Stack MEDIA 12 .45 .2 .08 .01 10 3
Stack AGGREGATE 6 .4 100

[LID_NODES]
S Stack 10

[LID_NODE_OUTLETS]
D 1 BOTTOM

[LID_LAYER_TREATMENT]
Stack 1 TSS 25 1 R = 0.2
Stack 2 TSS 10 2 C = C * 0.9
```

Define storage S, its link D and pollutant TSS elsewhere in the model. These
records alone are not a runnable model. The GUI manual's T9 tutorial ships
`docs/manual/tutorials/models/lid_storage_treatment.inp`, a complete example.

| Row | Parameters after the kind |
|---|---|
| SURFACE | Thickness; vegetation fraction |
| MEDIA | Thickness; porosity; field capacity; wilting point; saturated conductivity; conductivity slope; suction head |
| AGGREGATE | Thickness; porosity; conductivity |
| BOTTOM | Seepage rate; clogging factor |

Thickness and suction are inches in US projects or millimetres in SI projects.
Conductivity/seepage use inches/hour or millimetres/hour. Porosity and moisture
parameters are fractions. Initial saturation in `[LID_NODES]` is a percentage.
The engine validates layer order, finite parameters and admissible moisture
bounds. Outlet anchors are `Link Layer TOP|BOTTOM`; D above is located at the
bottom of SURFACE, 1.5 ft above the invert. Anchors track geometry edits, including the physical crest of weirs and
rating outlets. Use
explicit offsets for links with two LID endpoints because this syntax selects
only one LID endpoint. See @ref engine_manual_ch2_input_file for the input reference.

## Hydraulic representation and conservative coupling

`LidNodeData.hpp` separates authored layers from runtime cells. MEDIA is
subdivided into five numerical cells; these are not additional user layers.
The moisture profile stores retained water, while the network storage solver
handles mobile water. Its depth/volume relationship includes available pore
space and surface ponding. Reported node volume includes both water stores.

`LidNode.cpp` prepares infiltration, percolation, evaporation and seepage and
couples layer ports to the Dynamic Wave solve. Transfers from media to mobile
storage are internal transfers: retained water is debited when mobile water
is credited. Percolation must not be counted a second time as external inflow.
Port availability and final accepted port transfers reflect the hydraulic
iteration. Inlet or outlet elevations determine which physical region receives
or supplies water. Supply rainfall through contributing subcatchments or an
external inflow; assigning a control does not add an independent rainfall load.

Physical layer interfaces belong to the cell above them. Port selection allows
only roundoff-sized elevation differences and shares this rule between
hydraulic head, accepted water withdrawals and pollutant outlet treatment.
Crests authored relative to the upstream node are converted using the
difference between node datums before adding the crest offset. This avoids
misclassifying a surface weir as a media outlet after floating-point
cancellation at a nonzero invert. Surface ponding supplies its local head
even when the mobile water table remains below the crest.

### Legacy conductivity and surface infiltration

MEDIA drainage uses the legacy SWMM LID exponential conductivity law:
`q_i = Ks_i * exp(-slope_i * (porosity_i - theta_i))` above field capacity,
otherwise zero; `Q_i = area_i * q_i`. Conductivity slope is dimensionless,
nonnegative and is not a power exponent. Zero slope is valid and gives Ks
above field capacity. AGGREGATE drains at its specified conductivity with
zero field capacity. These are gravity-only, unit-gradient closures; the
receiver's matric head and conductivity do not form an interface gradient.
For example, Ks = 10 mm/h, porosity = 0.45, theta = 0.30 and slope = 10
give 2.23 mm/h. The old node kernel interpreted slope as an exponent;
existing node-model results must be rerun after this correction.

SURFACE → first MEDIA entry reuses `infil::grnampt_getInfil` in
`MOD_GREEN_AMPT` mode. Its ponded instantaneous capacity for F > 0 is
`f_cap = Ks * (1 + (suction + h_surface) * IMD / F)`; the shared routine
handles F = 0, integrated infiltration and supply-limited transitions.
Actual ponded depth is `theta_surface * surface_thickness / surface_porosity`.
Only accepted infiltration advances F and upper-zone wetness Fu. A cloned
history supplies the potential flux before donor/receiver limits. Dry recovery
is bounded by actual physical wetness and F cannot become negative.
An empty surface explicitly enters the dry recovery branch when the finite
upper zone is free of backwater. Ks = 0 is impermeable and bypasses the
Green–Ampt calculation. SURFACE → AGGREGATE entry uses aggregate conductivity.
Repeated/deeper MEDIA layers use the drainage law, not separate fronts.

Explicit substeps are at most one second. Accepted volumes are bounded by
`Q_i * substep`, donor water above field capacity and receiving pore space.
All transfers use a common pre-update state and equal donor/receiver volumes.
A receiver intersected by the mobile water table instead transfers into
mobile storage without the retained pore-capacity bound; donors whose bottoms
are below that table skip free drainage. These explicit cell balances differ
from legacy SWMM's lumped LID soil-layer integration, so reuse of its laws
is not a claim of identical conventional-LID results.

### Reverse flow, resaturation and recession

Signed hydraulic port transfers remain authoritative. Accepted reverse inflow
enters at its physical port; media receipts fill retained capacity before
excess enters mobile storage. The mobile water table saturates the submerged
fraction of each cell. Fully submerged cells retain field-capacity moisture
on recession, with each retained/mobile adjustment booked conservatively.

The modified Green–Ampt upper-zone depth follows the shared empirical Ks
relationship, bounded by the thickness of the first authored MEDIA layer.
Its maximum deficit is porosity minus wilting point. Initial deficit and
wetness come from actual retained moisture plus submerged pore volume.
After rising backwater or accepted media-port wetting, the finite-zone
physical deficit reduces IMD and raises Fu; that wetting does not add to F.
Full submergence of the media top sets IMD = 0, Fu = Fumax and clears the old
front. Gradual recession tracks remaining moisture until accepted surface entry
starts a new approximate front.
Dry recovery is suppressed when the empty surface overlies an upper zone
intersected by backwater. History is reconciled after accepted transfers;
hydraulic trial/reset calls never advance it.

This is a finite-zone adaptation for a network-connected facility, not a
solution of upward capillary flow or colliding wetting fronts. The closure
omits matric-gradient redistribution, capillary barriers and retention
hysteresis. It is more restricted than the gravity-plus-diffusivity block
formulation of [Tu, Wadzuk and Traver (2020)](https://doi.org/10.1371/journal.pone.0235528);
their HYDRUS comparison cannot validate this kernel. GUI T10 illustrates the
flux equations, resaturation and interpretation limits. The hydraulic formulation is documented in
@ref hydraulics_ref_lid_storage_formulation and the mass and treatment
formulation in @ref quality_ref_lid_storage_formulation.

The runtime moisture profile reports cell layer number, bottom/top elevations
and volumetric moisture. Elevations are relative to the storage invert in
project length units. A profile is available after initialization.

## Pollutant transport and treatment

`LidNodeTreatment.cpp` replays the accepted internal water transfers with
pollutant mass. Retained cells store a mass for each pollutant; mobile water
uses the existing storage-node quality reactor. Transfers carry the source
concentration, debit the source mass and credit the receiving compartment.
Evaporation removes water and leaves solute. Native quality routing uses the
layer-specific outlet concentration when a port supplies water; received
backflow is returned to the appropriate retained compartment. Returning
outfall water carrying LAST quality is also booked as an external pollutant
source; ZERO backflow quality supplies clean water. Zero-volume connections
to LIDs resolve the current mobile mixtures consistently between donor and
recipient, including water that percolates and drains below the ordinary
node dry-volume cutoff within one routing step. Provisional mixture iterations
restore inventories and balance counters; layer-exit treatment is booked once.
Failure to converge produces a warning and requires inspection of continuity
and routing-step sensitivity.

Each treatment row is:

```text
Control Layer Pollutant RemovalPercent DecayPerDay [Expression]
```

There can be one rule per pollutant per physical layer. Removal is 0–100%;
decay is nonnegative in 1/day. The expression is optional (blank or `-`).
Rules reference existing pollutants and cannot target the BOTTOM boundary.

1. At an authored-layer exit, fixed removal reduces the incoming concentration.
2. An optional `R = ...` expression gives a removal fraction of the remainder;
   `C = ...` gives the effluent concentration. Results are bounded so treatment
   cannot create pollutant mass. For example, 25% followed by `R = 0.2` removes
   40% in total. Expressions use the existing treatment parser, including
   pollutant concentration and removal references.
3. Resident retained mass decays exponentially, with the layer rate added to
   background pollutant decay: `M_after = M_before * exp(-k * dt)` using a
   consistent time unit. Removal/expressions do not repeat at the five internal
   MEDIA-cell interfaces.

Submerged pore volumes contribute volume-weighted layer decay to the shared
mobile reactor. Saturated outlets use the removal rule at their physical
layer. A surface bypass does not pass through all underlying media rules;
serial saturated plug-flow reactors are not implied by the layer table.
Treatment losses enter the reacted-mass accounting.

Expression context: concentration uses pollutant units; `DT` is seconds,
`HRT` is water volume/flow in hours (a step-duration fallback is used at zero
flow), `Q` is project flow, `D` is full authored layer thickness in project
length and `AREA` is project area. `V` retains the treatment engine's internal
cubic-foot convention. Co-treatment references should be acyclic; recursive
removal references are guarded against unbounded recursion.

## C and Python interfaces

`openswmm_infrastructure.h` exposes layer, assignment, outlet-anchor and
runtime-profile accessors. `SWMM_LidLayerTreatment` contains `layer`,
`pollutant`, `removal_percent`, `decay_per_day` and `expression`.
`swmm_lid_node_treatment_count` and `swmm_lid_node_treatment_get` enumerate rules;
`swmm_lid_node_configure` validates and replaces geometry plus treatment
atomically. Returned string pointers are borrowed; copy them before mutating
or destroying the engine. Configure before initialization.

Python exposes typed `LidNodeLayer`, `LidNodeLayerKind` and `LidLayerTreatment`
objects, including type stubs. On an opened model containing Stack and TSS:

```python
from openswmm.engine import LidLayerTreatment
lids = solver.infrastructure.lids
layers = lids.get_layers("Stack")
lids.set_layers("Stack", layers, treatments=[
    LidLayerTreatment(2, "TSS", removal_percent=10,
                      decay_per_day=1.25, expression="R = 0.35"),
])
assert lids.get_treatments("Stack")[0].layer == 2
```

`treatments=None` preserves existing rules by ordinal position and kind and
rejects replacements that would orphan them. Pass the complete treatment list
when reordering layers; `treatments=[]` explicitly clears all rules. A rejected
edit preserves prior geometry, treatment, node depths and outlet offsets.
`assign_node`, `node_assignment`, `remove_node`, `set_outlet_anchor` and
`node_profile` cover assignment and inspection. The Python infrastructure guide
contains a complete stack-construction example.

## Persistence and lifecycle

INP and GeoPackage round trips preserve layer geometry, rules, assignments and
anchors. Native V10 hotstarts preserve modified Green–Ampt history and the last
reconciled head, along with retained pollutant masses, moisture and clogging
state. Compatible pre-V10 LID hotstarts reconstruct missing infiltration
history from restored moisture and warn that exact continuation is unavailable.
V10 rejects incompatible infiltration parameters or geometry. Older compatible hotstarts without layer mass
initialize that state from the available node concentration. Configuration
files do not preserve runtime mass. Legacy SWMM 5 export cannot represent
storage-node LIDs and warns when omitting the extension.

Pollutant renames update layer rules and expression references. Pollutant
deletion handles affected rules and retained-state dimensions. Geometry and
rules remain owned by the control; runtime moisture/mass remain per node.

## Validation and practical limits

`tests/unit/engine/test_lid_nodes.cpp` covers geometry, conservative transfers,
outlet behavior, treatment, validation and persistence. Python's
`tests/engine/test_lid_nodes.py` covers typed configuration, atomic replacement
and treatment round trips. GUI layer-model and editor tests cover arbitrary
counts, reordering, numeric delegates, expression validation and Apply/reload.

The revised hydrology passes 241 engine tests in nine suites: LID nodes
(40), conventional LID (55), infiltration (33), quality (21), treatment (32),
hotstart (39), outfall backflow (5), LID water age (6) and LID heat (10).
The new tests check the analytical conductivity law, legacy Green–Ampt
potential flux and ponding response, accepted-only infiltration history,
impermeable/zero-multiplier limits, dry recovery, partial/full backwater
wetting, repeated hydraulic trials, recession, second-event entry and V10
exact continuation. A converted V9 fixture checks explicit reconstruction
and warning behavior. A six-minute routed tracer case checks water balance
and tracer inventory through reversal, resaturation, recession and a second
storm, without warnings. Existing full-chain regressions cover
MEDIA/AGGREGATE × free/backwater × ZERO/LAST boundary quality, including
per-step conservative-tracer closure to 0.1%.

GUI T10 supplies six active-control examples plus a separate six-minute
resaturation example. The 24-hour decks are rerun at 0.1, 0.05 and 0.025 s;
the article's validation note records binary provenance, continuity,
performance sensitivity and cumulative overflow budgets. These synthetic
tests establish conservative implementation behavior, not field validity.
Check timing, peaks and treatment convergence as well as continuity; rapid
surface overflow can alias coarse snapshots. Use cumulative engine budgets
for those losses and calibrate against measurements for field applications.
