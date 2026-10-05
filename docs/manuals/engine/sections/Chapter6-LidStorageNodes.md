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

Intercell retained-water drainage is a gravity-only Darcy–Buckingham
approximation: `q_i = Ks_i * clamp(theta_i / porosity_i, 0, 1)^n_i` above
field capacity, otherwise zero, and `Q_i = area_i * q_i`. It uses donor
properties, without a receiver matric-head gradient or an interface-averaged
conductivity. Aggregate cells have zero field capacity and the default
exponent three. Surface entry instead uses receiving conductivity and
suction: `q = K_b * (1 + suction_b * max(porosity_b - theta_b, 0) /
max(suction_b + theta_surface * thickness_surface, epsilon))`.

Explicit substeps are at most one second. Accepted volumes are bounded by
`Q_i * substep`, donor water above field capacity and receiving pore space.
All transfers use a common pre-update state and equal donor/receiver volumes.
A receiver intersected by the mobile water table instead transfers into
mobile storage without the retained pore-capacity bound; donors whose bottoms
are below that table skip free drainage. The closure omits matric-gradient
redistribution, including upward capillary flow and capillary barriers. It is
more restricted than the gravity-plus-diffusivity block formulation of
[Tu, Wadzuk and Traver (2020)](https://doi.org/10.1371/journal.pone.0235528);
their validation cannot be applied to this kernel. GUI T10 illustrates the
flux equations and interpretation limits.

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
anchors. Native V9 hotstarts also preserve retained pollutant masses alongside
moisture and clogging state. Older compatible hotstarts without layer mass
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

The pollutant-balance follow-up was checked with 25 LID cases and the quality,
treatment, hotstart, outfall-backflow, LID water-age and LID heat suites: 138
engine tests passed. The new full-chain fixture covers MEDIA/AGGREGATE,
free/backwater conditions and ZERO/LAST outfall quality, checking final
balances and conservative-tracer inventory at every routing step to 0.1%.
Focused cases cover sub-litre drainage and physical weir-crest anchors.

GUI T10 supplies six complete active-control examples. Twenty-six distinct
case/step combinations (0.5, 0.25, 0.1 and 0.025 seconds for all six cases,
plus 0.05 seconds for the passive cases) passed 0.5% water/pollutant
continuity acceptance without engine warnings; reported pollutant errors
were below 0.001%. Figures use the 0.025-second runs. The passive 0.1-second
results showed performance sensitivity despite good continuity; the
0.05- and 0.025-second passive runs agreed in half-export time at sampling
resolution and reacted fraction at report precision. These are test-case results, not a universal accuracy guarantee.
Check convergence of performance metrics as well as continuity. Rapid
surface overflow can alias a coarse output sampling interval; use cumulative
engine budgets for those losses. Inspect water and pollutant continuity and
repeat with a smaller step for the model being studied.

Three subsequent interface regressions cover perched surface-water withdrawal
at a nonzero node invert, one-ULP perturbations at cell interfaces versus
physical offsets, and consistent layer selection for mobile-outlet treatment.
The seven LID/quality/treatment/restart/backflow/water-age/heat suites now pass
141 tests. The corrected GUI T10 results and figures supersede the prior
overflow totals; their validation note records the rebuilt library and
additional routing-step refinement.
