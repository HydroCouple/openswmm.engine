@page application_manual_ch5_street_inlets Chapter 5: Street Inlets and the Inlet Junction

## Problem

Water running down a street reaches a grate or a curb opening, some of it
goes into the sewer, and the rest carries on to the next inlet. A model
has to say how much goes in — which is a HEC-22 calculation from the
spread, the depth at the curb and the inlet's design — and where the rest
goes.

OpenSWMM offers two ways to attach an inlet, and they are not
interchangeable. A **conduit-attribute inlet** is a property of a street
conduit: `[INLET_USAGE]` names the conduit, the design and the node the
captured water is delivered to, and the capture is booked at the conduit's
downstream node. An **inlet junction** is a node of its own, sitting
between two street conduits. This chapter runs both on one street and
shows what each makes possible.

## Deck

Deck: `docs/figures/decks/street_inlets/street_inlet_junction.inp`

One street reach draining to a parallel sewer:

```
J_TOP --ST_A--> J_MID --ST_B--> [IJ1] --ST_C--> OUT_ST     street, at grade
          |            capture     |  capture
          +--> MH1 --SW_1--> MH2 --+--SW_2--> OUT_SEW      sewer, buried
```

`ST_A` carries a conduit-attribute inlet — a combination grate and curb
opening, two of them — whose capture is delivered to `MH1`. `IJ1` is an
inlet junction between `ST_B` and `ST_C` carrying a single curb opening,
delivering to `MH2`. The storm is a 2-hour event on a small catchment
above the street.

The deck is a copy of `examples/inlets/street_inlet_junction.inp`; its
header shows all four `[INLETS]` design grammars, including the two-line
combination encoding and a custom design pointing at a diversion curve.

## Options that select the formulation

An inlet junction is a virtual junction that also carries an inlet, so it
inherits every virtual-junction rule: exactly two conduits, the same
cross-section on both, zero offsets, and dynamic wave routing.

```
[INLET_JUNCTIONS]
;;Name  Elev   MaxDepth  InitDepth  Inlet  Node   Count  ...
IJ1     ...                                Curb1  MH2    1

[INLET_USAGE]
;;Conduit  Inlet   Node   Number  %Clogged  Qmax  aLocal  wLocal  Placement
ST_A       Combo1  MH1    2       0         0     0       0       AUTOMATIC
```

with `[STREETS]` giving the cross-section both conduits use and `[INLETS]`
the designs. A conduit attached to an inlet junction may **not** also
carry an `[INLET_USAGE]` row — that would capture the same water twice,
and the engine refuses it (rule 629). That is why the conduit-attribute
inlet sits on `ST_A` rather than on `ST_B`.

## Results

![Figure 5-1](figures/png/workflow_ch5_inlet_capture_vs_bypass.png)

*Figure 5-1 Capture and bypass along a street reach with a conduit-attribute inlet and an inlet junction*

| | Peak approach flow | Peak capture | Average capture | Volume captured | Volume bypassed |
|---|---|---|---|---|---|
| `ST_A`, conduit attribute | 11.937 cfs | 99.00 % | 98.22 % | 266.6 kgal | 2.70 kgal |
| `IJ1`, inlet junction | 0.133 cfs | 90.00 % | 90.00 % | 2.41 kgal | 0.28 kgal |

*Table 5-1 The Street Inlet Flow Summary for the two inlets, as the report prints it*

**Read the summary's columns carefully.** "Peak Flow" is the peak
*approach* flow the inlet saw, not the flow it captured. "Bypass Flow
Pcnt" — 88.90 % for `ST_A` — is the *frequency* of bypass over the periods
with capture, not a share of the volume; the volume share is the last two
columns, and by volume `ST_A` captured 99 % of what reached it. A reader
who takes the bypass column as a volume fraction will conclude the
opposite of what the run says.

**The first inlet does nearly all the work.** It sees 269 kgal over the
storm and passes 2.7 kgal on. The inlet junction then sees only what got
past the first one, captures 90 % of that, and leaves 0.28 kgal to reach
the downstream outfall. Panel (a) shows the same thing as flows: the
street flow falls from 11.9 cfs to 0.13 cfs to 0.012 cfs across the two
inlets, three orders of magnitude on a symmetric log scale.

**What the inlet junction adds.** The capture itself is the same HEC-22
calculation either way. The difference is that `IJ1` is a node, so it

- has a head of its own, which the two street conduits see, instead of
  the capture being booked at a neighbouring node's head;
- can flood above the curb and report it as a node would;
- takes `MH2`'s surcharge back onto the street as backflow, which a
  conduit attribute cannot represent at all;
- transmits momentum between `ST_B` and `ST_C` rather than terminating
  them.

**When the conduit attribute is the right answer.** When the inlet is one
of a run of many on a long reach, when the sewer below it will not
surcharge, and when the street's own hydraulics are not the question. It
is cheaper — no extra node, no virtual-junction constraints on the
cross-sections — and this run shows it doing the same capture job.

## Where to go next

- @ref hydraulics_ref_ch7_advanced_features "Hydraulics 7" — the HEC-22
  capture relations and the inlet-junction formulation
- @ref hydraulics_ref_ch5_cross_section "Hydraulics 5" — the street
  cross-section, its spread and its depth at the curb
- @ref engine_manual_sect_STREETS, @ref engine_manual_sect_INLETS,
  @ref engine_manual_sect_INLET_USAGE and
  @ref engine_manual_sect_INLET_JUNCTIONS — the grammar of each section
- @ref tutorial_street_inlets — the same model built in the GUI
- @ref application_manual_ch2_filling_pipe "Chapter 2" — what happens in
  the sewer below, once the captured water surcharges it
