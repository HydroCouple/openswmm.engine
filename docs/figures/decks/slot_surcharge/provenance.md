# slot_surcharge — three overloaded pipes for the surcharge treatments

| | |
|---|---|
| Deck | `slot_surcharge.inp` |
| Copied from | `tests/regression/data/slot_surcharge.inp`, last changed in commit `1d4b4d9b` (2026-08-23), copied 2026-11-21 |
| Used by | Hydraulics Reference Chapter 3, figure `hydraulics_ch3_surcharge_methods` (Figure 3-8) |
| Changed on copy | nothing: the file is byte-identical to the regression deck (SHA-256 `acaf1bce…d28`) |
| Run time | under 0.1 s per run on the build machine (four runs, one per `SURCHARGE_METHOD`) |

## What it is

The engine's Preissmann-slot regression model (issue #144): three 400 ft
circular pipes of 1 ft diameter from junction J1 through J2 and J3 to a
free outfall, falling 2 ft in all. The inflow at J1 ramps from 0 to
12 cfs over the first 10 minutes, holds to 2:30 and is off by 3:00,
against a full-pipe capacity near 1 cfs, so the chain is surcharged for
most of the four-hour run. The junctions are 30 ft deep, which holds the
water in the pipes instead of flooding it away at once; J1 still floods
at its rim. Dynamic wave routing, a 5 s routing step and 1 minute
reporting.

## How the figure uses it

The deck names `SURCHARGE_METHOD SLOT`. The generator runs it four times
through `sink.run`, upserting `SURCHARGE_METHOD` to `EXTRAN`, `SLOT`,
`DYNAMIC_SLOT` and `TPA` and changing nothing else, so the dynamic slot
and TPA run with their default `DPS_*` and `TPA_CELERITY` values.
