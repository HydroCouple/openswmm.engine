@page application_manual_ch3_subatmospheric Chapter 3: A Sub-Atmospheric Transient — the Slot against the Two-Component Pressure Approach

## Problem

A full pipe over a summit, drawn down from one end, develops pressure
*below* atmospheric at the high point. Inverted siphons do it, pumped
mains do it when a pump trips, and a sewer does it whenever a downstream
control drains a surcharged reach faster than the upstream end can follow.

A Preissmann slot cannot represent that. The slot is a narrow shaft added
above the crown so the free-surface equations keep their form when the
pipe fills; it is open to the atmosphere by construction, so the water
surface inside it cannot describe a pressure lower than the air above it.
The two-component pressure approach carries the pressurised part of the
state as a second component instead, and that component is free to go
negative.

This chapter runs the experiment that separates them.

## Deck

Deck: `docs/figures/decks/negative_pressure/negative_pressure.inp`

The pipeline of *Chapter 2* with its centre raised 0.15 m — 2 % up to the
summit, 2 % down from it — both ends at elevation zero. The system starts
full and at rest, filled to 0.30 m at the upstream box, which puts the
summit's crown under water. At t = 0 a siphon withdrawal of 0.45 L/s
starts, the upstream level falls, and the pressure at the summit follows
it down.

The gauge stations are **sealed junctions** (`SUR_DEPTH 30`), not plain
junctions: a plain junction is an atmosphere contact and would vent the
pressurised state — right for a manhole, wrong for a transducer tapping on
a sealed pipeline. The comparison window ends at 40 s, where an air cavity
intrudes in the laboratory and the single-phase model stops being a model
of the experiment.

## Options that select the formulation

```
[OPTIONS]
FLOW_ROUTING         FV
FV_PRESSURE_CLOSURE  TPA          ; SLOT in the second run
FV_SLOT_CELERITY     300          ; the pipeline's measured acoustic celerity
REPORT_SIGNED_HEADS  YES
```

and, for the dynamic wave runs,

```
FLOW_ROUTING         DYNWAVE
SURCHARGE_METHOD     TPA          ; SLOT in the fourth run
TPA_CELERITY         300
```

`REPORT_SIGNED_HEADS` is what lets the report carry a head below the pipe
invert rather than clamping it. Without it the interesting part of the
answer is silently truncated at zero.

## Results

![Figure 3-1](figures/png/workflow_ch3_negative_pressure_head_14p1.png)

*Figure 3-1 Piezometric head at x = 14.1 m and velocity at x = 9.9 m in the siphon experiment, under both closures on both routers*

![Figure 3-2](figures/png/workflow_ch3_velocity_9p9.png)

*Figure 3-2 The same four runs at the velocity transducer, where the closures diverge after the drawdown*

| | Gauge head at x = 14.1 m, t = 40 s | Routing continuity error |
|---|---|---|
| Measured | 0.099 m | — |
| `FV`, TPA | 0.103 m | −0.000 % |
| `FV`, static slot | **diverged** — ERROR 14 in the first substeps | — |
| `DYNWAVE`, TPA | 0.232 m | −10.434 % |
| `DYNWAVE`, slot | 0.233 m | −11.118 % |

*Table 3-1 The drawdown each formulation reports, against the measurement*

**The static slot does not produce a wrong answer here; it produces no
answer.** Under the finite-volume router with `FV_PRESSURE_CLOSURE SLOT`
the run stops in the first substeps with ERROR 14, a depth magnitude of
13 000 ft in the conduit downstream of the summit. That is the right
behaviour for a closure asked to represent a state it has no
representation of: the slot must keep a free surface, the state has none,
and the solution runs away. A closure that quietly returned zero would be
worse.

**With the right closure, the finite-volume run is the experiment.** It
tracks the measured drawdown through all four of its stages and ends
within 4 mm of the measurement after 40 s, with continuity at −0.000 %.
Panel (b) of Figure 3-1 shows why the summit is the interesting station:
the acoustic oscillation at 300 m/s is visible and decaying, and the
summit reaches zero gauge pressure at t = 31.6 s — the column separating,
which is the physical event the experiment was built to produce.

**The dynamic wave runs fail differently, and quietly.** Both closures
give the same answer to within a millimetre, and both drain the system
less than half as fast as the measurement: 0.232 m against 0.099 m at
40 s. TPA does not rescue the dynamic wave router here, because the
problem is not the pressurisation closure — it is that a single flow per
conduit cannot carry the acoustic wave that governs how fast a full pipe
responds. The continuity errors, −10.4 % and −11.1 %, say so plainly:
these runs are not conserving the water they were given.

**The velocity trace is the same story in a different variable.**
Figure 3-2: the finite-volume run follows the measured oscillation in
phase and in amplitude; the dynamic wave runs overshoot by roughly a
factor of two and drift out of phase by five seconds over the window.

**One caution.** `FV_PRESSURE_CLOSURE TPA` is \status{Experimental}, and
this chapter is exactly the case it exists for. Its caveats — the
integrator choice, the pinned cases, the measured behaviour under
different acoustic celerities — are in
@ref hydraulics_ref_ch8_finite_volume "Hydraulics 8, §8.4.5", and they
should be read before a TPA result is trusted on a system that is not
this one.

## Where to go next

- @ref application_manual_ch2_filling_pipe "Chapter 2" — the same pipeline
  filling rather than draining, where the static slot is the right choice
- @ref hydraulics_ref_ch8_finite_volume "Hydraulics 8, §8.4.5" — the TPA
  closure, its flag transitions and its recorded caveats
- @ref hydraulics_ref_ch3_dynamic_wave "Hydraulics 3, §3.3.11" — TPA under
  the dynamic wave router
- @ref hydraulics_ref_ch10_planned "Hydraulics 10, §10.7" — the
  high-celerity filling divergence, which this closure carried until
  2026-09-12 and no longer does
- @ref engine_manual_sect_OPTIONS — the grammar of every key named here
