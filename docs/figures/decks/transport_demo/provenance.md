# transport_demo — five conduits, one tracer step, four process components

| | |
|---|---|
| Deck | `transport_demo.inp` with the sidecars `transport_demo.rxn`, `.ard`, `.age`, `.heat` |
| Copied from | `openswmm.gui/docs/manual/tutorials/models/` (SWMMVis manual tutorial T7), 2026-09-20 |
| Used by | the Application Manual's transport-engines, multi-species-reactions and age-and-heat chapters |
| Changed on copy | nothing; the GUI copy is the same five files |

## What the deck contains

Five junctions in a line, `J1` to `J5`, discharging to the free outfall
`OUT1` through five 100 m circular conduits. A constant 0.2 m³/s inflow
enters at `J1` from t = 0 carrying

- `TRACER`, a conservative pollutant at 10 mg/L,
- `DECAY`, a first-order pollutant at 20 mg/L with k = 2 /day,
- an inlet temperature of 18 °C, against air warming from 12 to 22 °C.

Because the inflow starts at t = 0 into a clean system, the tracer arrives
at each junction as a **step front**, which is what makes this deck a
comparison of how each transport engine carries a front.

Four process components are configured from the sidecars: a reaction
system with an Arrhenius temperature dependence, the Eulerian transport
options, the water-age source ages, and the heat sources with their flux
modules and radiative forcing.

## Snippet markers

The deck carries `;//! [name]` marker pairs around the blocks the manual
quotes — `options`, `temperature`, `pollutants`, `inflows`, `components` —
so the chapters can `\snippet` them rather than copy them. Keep the markers
in step with the GUI copy if either changes.

## A note on the reporting step

The deck reports every 5 minutes, which is right for reading the tutorial's
tables and too coarse for the front: it crosses the line in about five
minutes. The manual's figures upsert `REPORT_STEP 0:00:10` for that reason,
and the committed deck is left as the tutorial has it.
