# rapid_fill — Vasconcelos, Wright and Roe (2006) rapid pipe-filling experiment

| | |
|---|---|
| Deck | `rapid_fill.inp` |
| Copied from | `studies/mixed_flow_closures/decks/e2_rapid_fill_2006.inp` (untracked study folder), 2026-09-20 |
| Study plan | `plans/MIXED_FLOW_CLOSURES_TPA_UF_PLAN_2026-08-29.md`, experiment e2 (2006 parameterization) |
| Used by | the Application Manual's filling-pipe chapter; figures `workflow_ch2_rapid_fill_pressure_9p9`, `workflow_ch2_rapid_fill_front` |
| Changed on copy | nothing: byte-identical to the study deck |

## The experiment

An acrylic pipe 14.33 m long and 94 mm in diameter, horizontal, initially at
rest and 78 % full (0.073 m). A fill box 0.25 × 0.25 m feeds the upstream end
and a 0.19 m surge tank closes the downstream end. At t = 0 an inflow of
3.1 L/s is admitted at the fill box; a bore runs down the pipe, fills it, and
the surge tank oscillates.

The paper's own numerics are in the deck: 400 cells (Δx ≈ 0.036 m),
Courant 0.95, Manning n = 0.012, acoustic celerity a = 25 m/s.

Both gauge stations are **virtual junctions** — sealed splices, transparent to
the solution — so the deck doubles as a virtual-junction composition check.

## Observed data

| File | Quantity | Station |
|---|---|---|
| `e2_pressure_9p9.csv` | pressure head, m gauge | x = 9.9 m (node `VJ99`) |
| `e2_velocity_9p9.csv` | velocity, m/s | x = 9.9 m (link `C2`) |
| `e2_surgetank_level.csv` | water level, m | surge tank (node `ST`) |

All three were digitised from the figures of the published paper at native
raster resolution; each file's header records the figure, the extraction and
any datum shift applied. The pressure series is gauge head at the transducer:
the paper plots head *variation*, and the initial water level of 0.073 m has
been added back.

Vasconcelos, J. G., Wright, S. J. and Roe, P. L. (2006). Improved simulation
of flow regime transition in sewers: the two-component pressure approach.
*Journal of Hydraulic Engineering* 132(6), 553–562.
