@page application_manual_ch9_msx Chapter 9: Multi-Species Reactions

## Problem

A first-order decay coefficient on a `[POLLUTANTS]` row is enough for a
tracer study and not enough for chemistry. Chlorine decays at a rate that
depends on temperature; two species react with each other; a wall demand
differs from a bulk demand. Those need a **reaction system**: species,
coefficients and rate expressions the engine evaluates.

OpenSWMM configures one as a process component with its own file. This
chapter builds the smallest useful system — one species whose decay rate
follows the water temperature the heat component computes — and is candid
about what this release does and does not let you see.

## Deck

Deck: `docs/figures/decks/transport_demo/transport_demo.inp` with
`transport_demo.rxn`

The five-conduit line of
@ref application_manual_ch8_transport_engines "Chapter 8". The reaction
system adds one bulk species, `CL2`, decaying first order with an
Arrhenius correction:

```
[REACTION_OPTIONS]
SOLVER       RK5
COUPLING     NONE
RATE_UNITS   HR
TEMPERATURE  20

[REACTION_SPECIES]
;;Kind   Name   Units
BULK     CL2    MG

[REACTION_COEFFICIENTS]
;;Kind      Name    Value
CONSTANT    kb      0.30
CONSTANT    theta   1.07

[REACTION_PIPES]
;;Form   Species  Expression
RATE     CL2      -kb * theta ^ (TEMP - 20) * CL2

[REACTION_QUALITY]
;;Scope   Species  Value
GLOBAL    CL2      2.0
```

`TEMP` in that expression is a hydraulic variable available to every
expression: when the heat component is on it is the computed water
temperature, and when it is off it is the `[REACTION_OPTIONS] TEMPERATURE`
fallback. That single identifier is the whole coupling between the two
components.

## Options that select the formulation

\snippet transport_demo/transport_demo.inp components

A component is named by its identifier and given a configuration file.
Nothing else in the deck changes; remove the line and the system is gone.
The sections may also be embedded directly in the `.inp`, which is
supported as a convenience — but embedded sections are read and not
written back when the model is saved, and the engine warns before that
happens.

## Results

![Figure 9-1](figures/png/workflow_ch9_arrhenius_rate_from_temperature.png)

*Figure 9-1 What the computed water temperature does to a reaction system's rate, through the Arrhenius correction in its expression*

**The correction is the point of coupling heat to reactions.** At
θ = 1.07 the rate changes by 7 % per degree, so the half-life of `CL2`
runs from 230 minutes in 12 °C water to 139 minutes at 20 °C. A model that
leaves `HEAT_TRANSPORT` off evaluates the same expression at the fallback
temperature and gets the 20 °C answer everywhere, all year.

**On this deck the effect is small, and it is worth seeing why.** Panel
(b) is the run's own temperature: the line fills from empty, the nodes
pass through a cold-start transient in the first minutes, and by ten
minutes every node sits at the inflow temperature of 18 °C, where the
multiplier is 0.874. The deck is too short and too fast for the water to
warm; a reach with a real residence time is where the correction earns
its keep.

**Do not read the first minutes.** A node that is nearly dry and filling
carries a temperature computed from a vanishing volume — the 2 °C
excursion at `J5` at three minutes is that, not a physical result. The
same caution applies to every concentration during a cold start.

## Species output and remaining limits

Reaction species are written as named native result columns after pollutants,
water age and temperature. The original figure above illustrates the thermal
rate multiplier; it does not display the species concentrations. Species
columns are available to the output reader and GUI even when they are absent
from `[POLLUTANTS]`.

`[REACTION_REPORT]` can select a subset without changing what the engine
computes:

```
[REACTION_REPORT]
SPECIES ALL NO
SPECIES CL2 YES
```

`[REACTION_SOURCES]` accepts CONC, MASS, FLOWPACED and SETPOINT sources,
optionally modulated by cyclic `[REACTION_PATTERNS]`. MASS uses declared mass
units per second. FLOWPACED doses arriving water; SETPOINT raises the mixed
concentration during positive water exchange and never removes species.
`[REACTION_PARAMETERS]` overrides PARAMETER coefficients on nodes and links.

`[REACTION_SUBCATCHMENTS]` supplies global or named-subcatchment wet laws.
Its first-order split mixes retained mass with rain/run-on, reacts, then
allocates infiltration, runoff and storage. `[REACTION_BUILDUP]` independently
reacts dry land-use inventories, in declared mass per AREA_UNITS, before
accrual, sweeping and washoff. Check WET_STEP/DRY_STEP sensitivity.

`[REACTION_LIDS]` opts controls into separate BULK layer reactors and retained
inventories. Conventional units require full coverage and every unit to opt
in. Storage-node bucket/Richards reactors require LEGACY quality and a non-Mesh
bottom. Partial capture, covered barrels, WALL chemistry and 2D-owned
conventional units remain refused. Full native checkpoints carry these
inventories; state-only hotstarts refuse them.

`WALL_GEOMETRY YES` enables AV (AREA_UNITS/litre). Conduits derive contact
from wetted perimeter, physical length and barrels; storage uses explicit
`[REACTION_WALLS]` areas. WALL density refers to fixed reference area, with
wetted-fraction kinetics and retained dry mass. ARD falls back to LEGACY for
WALL systems; LAGRANGIAN refuses them. LEGACY/LAGRANGIAN tank kinetics skip
zero/sub-litre water; ARD retains its 1e-9 ft³ store threshold.

Exact native checkpoint/output continuation is checked for CPU 1D network,
wet/dry subcatchment and qualified LID chemistry. Heat/age, active 2D chemistry,
subsurface laws, pollutants and detailed ARD/LID result streams remain outside
this profile. Native hotstart inventory loading has a separate contract.

## Where to go next

- @ref quality_ref_ch8_msx_reactions "Quality 8" — the reaction system,
  its solver, its expression language and the status of every section
- @ref quality_ref_ch11_planned "Quality 11" — what is coming for
  reactions and reporting
- @ref application_manual_ch10_age_heat "Chapter 10" — the heat component
  whose temperature this chapter's expression reads
- @ref engine_manual_sect_PROCESS_COMPONENTS — how a component is named
  and configured
- @ref engine_manual_sect_REACTION_OPTIONS and the other
  `[REACTION_*]` entries — the grammar of each section
