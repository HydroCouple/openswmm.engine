@page application_manual_ch10_age_heat Chapter 10: Water Age and Heat

## Problem

Two questions a concentration cannot answer. *How long has this water been
in the system?* — which governs disinfectant decay, septicity and the
credibility of a first-flush argument. And *what temperature is it?* —
which governs reaction rates, dissolved oxygen and what the receiving
water gets.

OpenSWMM answers both with reserved species carried by the same transport
engine that carries the pollutants: `__WATER_AGE__` and
`__TEMPERATURE__`. Each is switched on by one `[OPTIONS]` key and
configured by its own component file.

## Deck

Deck: `docs/figures/decks/transport_demo/transport_demo.inp` with
`transport_demo.age` and `transport_demo.heat`

The five-conduit line of
@ref application_manual_ch8_transport_engines "Chapter 8", starting empty,
filled from t = 0 by a constant 0.2 m³/s inflow at 18 °C while the air
warms from 12 °C to 22 °C over six hours.

## Options that select the formulation

\snippet transport_demo/transport_demo.inp options

Water age needs its source ages:

```
[WATER_AGE_SOURCES]
EXTERNAL_INFLOW   GLOBAL   0.5
INITIAL_STATE     GLOBAL   6.0
```

Heat needs its sources and the flux modules to run:

```
[HEAT_SOURCES]
EXTERNAL_INFLOW   GLOBAL   18.0
INITIAL_STATE     GLOBAL   12.0

[HEAT_FLUXES]
SURFACE_EXCHANGE     ON
RADIATIVE_EXCHANGE   ON

[RADIATIVE_FLUXES]
SHORTWAVE   GLOBAL   COMPUTED
ALBEDO      GLOBAL   0.08
SKY_VIEW    GLOBAL   0.90
```

together with the deck's own meteorology:

\snippet transport_demo/transport_demo.inp temperature

The `[SOLAR_RADIATION]` block gives the site's latitude, longitude, time
zone and elevation, which is what `SHORTWAVE COMPUTED` needs to place the
sun.

## Results: water age

![Figure 10-1](figures/png/workflow_ch10_water_age_along_line.png)

*Figure 10-1 Water age along the line as the initial water is flushed out, and the travel-time profile it leaves*

| Node | Water age at the end of the run |
|---|---|
| J1 | 30.1 min |
| J2 | 31.3 min |
| J3 | 32.5 min |
| J4 | 33.6 min |
| J5 | 34.8 min |

*Table 10-1 The steady age profile — the declared source age plus travel time*

**Age is a clock the water carries, not a property of the pipe.** Every
node settles at the age its arriving water was given plus the time it took
to get there: 30 minutes at `J1`, which is the declared
`EXTERNAL_INFLOW 0.5 h`, rising by about 1.2 minutes per 100 m of pipe.
The gradient is the travel time, and it is the number a residence-time
argument actually needs.

**`INITIAL_STATE 6.0 h` never appears.** The pipes in this deck start
empty, so there is no standing water to carry the initial age. In a model
started from a hot-start file, or one with storage that begins full, that
source dominates the first hours and the flushing of it is the interesting
part of the answer. Declaring an initial age is cheap insurance; the value
only matters if there is water to carry it.

## Results: heat

![Figure 10-2](figures/png/workflow_ch10_heat_diurnal.png)

*Figure 10-2 Water temperature along the line against the air temperature driving it*

**Here, heat transport is transport.** The water arrives at 18 °C and
stays at 18.00 °C for the whole run while the air climbs to 22 °C. With
surface exchange and radiative exchange both on, a 35-minute residence in
500 m of pipe is far too short for the atmosphere to move the water
temperature measurably. The component is doing its job — it carried the
inlet temperature down the line and mixed it correctly at every node — and
the exchange terms have almost nothing to work with.

That is the general shape of the result for a conveyance system. Surface
exchange matters where water is slow and exposed: a detention pond, an
open channel, a long shallow reach, an LID surface. It does not matter
much in a 500 m sewer.

**Read the cold start with suspicion.** The excursions in the first
minutes of Figure 10-2 — down to 2 °C at `J5` — are nodes that are nearly
dry and filling fast. A temperature is the property of a volume of water,
and when that volume is a few litres arriving into an empty pipe, the
number is arithmetic rather than physics. Start the reporting after the
system has filled, or hot-start it.

## Where to go next

- @ref quality_ref_ch9_age_heat "Quality 9" — water age and the heat
  budget, including every flux module and its parameters
- @ref application_manual_ch9_msx "Chapter 9" — the reaction system that
  reads this chapter's temperature
- @ref application_manual_ch8_transport_engines "Chapter 8" — the engine
  that carries both reserved rows
- @ref engine_manual_sect_WATER_AGE_SOURCES,
  @ref engine_manual_sect_HEAT_SOURCES,
  @ref engine_manual_sect_HEAT_FLUXES — the grammar of each section
- @ref manual_climate — the meteorology in the GUI
