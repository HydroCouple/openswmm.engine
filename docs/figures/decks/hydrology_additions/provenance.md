# hydrology_additions — RDII with a physics-based recovery

| | |
|---|---|
| Deck | `rdii_decay.inp` |
| Written for | the Application Manual's long-term-hydrology chapter; figure `workflow_ch11_rdii_decay_vs_linear` |
| RTK and decay coefficients from | `python/tests/engine/test_rdii_advancement.py:250-266`, the values that test pins |

## What it is

One sanitary sewer node fed by an RTK unit hydrograph group with three
response terms, each carrying a `[RDII_DECAY]` row, over a 30-day January.
Two identical six-hour storms fall nineteen days apart, and the air
temperature series warms through the month.

The deck exists to show one thing: an RTK group alone answers both storms
identically, and the decay section makes the answer depend on what came
before and on how cold it has been.

## Two parameter notes

`T_ref` and `T_freeze` in `[RDII_DECAY]` are **degrees Celsius** whatever
the project's units are; this is a US deck, so its `[TEMPERATURE]` series
is Fahrenheit and the engine converts before evaluating the recovery rate.
The committed series runs 34 to 52 °F (1 to 11 °C) and the chapter's cold
variant runs 24 to 34 °F (−4 to +1 °C), which is mostly below the
`T_freeze` of 0 °C.

`[HYDROGRAPHS]` keeps `Dmax` and `D0` when a decay row is present — they
describe the abstraction reservoir — but its linear recovery rate `Drec`
is ignored for that response. This deck leaves `Drec` at zero, so running
it with the decay section removed gives a linear model that never recovers
and abstracts both storms entirely. That is the contrast the figure draws,
and it is a real trap rather than a contrived one.

## Snippet markers

`;//! [options]`, `[temperature]`, `[rtk]` and `[decay]` bracket the blocks
the chapter quotes.
