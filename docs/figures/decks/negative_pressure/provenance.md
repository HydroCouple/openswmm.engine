# negative_pressure — Vasconcelos, Wright and Roe (2006) siphon experiment

| | |
|---|---|
| Deck | `negative_pressure.inp` |
| Copied from | `studies/mixed_flow_closures/decks/e3_negative_pressure.inp` (untracked study folder), 2026-09-20 |
| Study plan | `plans/MIXED_FLOW_CLOSURES_TPA_UF_PLAN_2026-08-29.md`, experiment e3; engine issue #156 |
| Used by | the Application Manual's sub-atmospheric chapter; figures `workflow_ch3_negative_pressure_head_14p1`, `workflow_ch3_velocity_9p9` |
| Changed on copy | nothing: byte-identical to the study deck |

## The experiment

The pipeline of `rapid_fill` with its centre raised 0.15 m — 2 % up to the
centre, 2 % down from it — both ends at elevation zero. The system starts
full and at rest, filled to 0.30 m at the fill box, which puts the crown at
the centre (0.244 m) under water. At t = 0 a siphon withdrawal of 0.45 L/s
starts at the fill box, the fill-box level falls, and the piezometric
pressure at the centre goes **negative**.

That is the discriminator this deck exists for. A Preissmann slot cannot
represent sub-atmospheric pressure in a full pipe: the slot is open to the
atmosphere, so the water surface it invents simply drops into the pipe. The
two-component pressure approach carries a pressurized cell's pressure as a
second component and can take it below zero.

The comparison is valid to about t = 40 s. After that an air cavity intrudes
— two-phase flow the model does not contain — so `END_TIME` is set at that
boundary rather than past it.

The gauge stations are **sealed junctions** (`SUR_DEPTH 30`) rather than
virtual junctions: a plain junction is an atmosphere contact and would vent
the pressurized latch, correctly for a manhole and wrongly for a transducer
tapping on a sealed pipeline.

## Observed data

| File | Quantity | Station |
|---|---|---|
| `e3_pressure_14p1.csv` | pressure head, m gauge | x = 14.1 m |
| `e3_velocity_9p9.csv` | velocity, m/s | x = 9.9 m |

Digitised from the published figures; each file's header records the source
figure and any datum shift. The paper's measured acoustic celerity for this
pipeline is a = 300 m/s.

Vasconcelos, J. G., Wright, S. J. and Roe, P. L. (2006). Improved simulation
of flow regime transition in sewers: the two-component pressure approach.
*Journal of Hydraulic Engineering* 132(6), 553–562.
