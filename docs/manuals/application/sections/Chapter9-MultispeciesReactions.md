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

## What this release does not report

`CL2` itself does not appear in Figure 9-1, and it cannot: **the reaction
system's species are computed but not written to the output file.** The
section that would select them for reporting, `[REACTION_REPORT]`, is
\status{Planned} (@ref quality_ref_ch8_msx_reactions "Quality 8, §8.2").
The output file carries the `[POLLUTANTS]` rows and the reserved rows for
water age and temperature, and nothing else.

Four other sections of a reaction configuration are also planned and are
**refused**, not ignored: `[REACTION_SOURCES]`, `[REACTION_PARAMETERS]`,
`[REACTION_PATTERNS]` and `[REACTION_SUBCATCHMENTS]`. A configuration
carrying one of them is rejected as a whole, with the phase that will
deliver it named in the message. That is deliberate — a silently ignored
source term would be worse than a refused file.

So a reaction system today is worth building when the species drive
something you *can* see, or when you are preparing a model for the release
that reports them. The quality reference chapter states the same limits
from the theory side.

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
