# Street inlets and the inlet junction

`street_inlet_junction.inp` is a five-link model that puts the two ways of
attaching a street inlet side by side on one street reach, so their report
output can be compared directly.

```
J_TOP --ST_A--> J_MID --ST_B--> [IJ1] --ST_C--> OUT_ST   street, on grade
          |            capture     |  capture
          +--> MH1 --SW_1--> MH2 --+--SW_2--> OUT_SEW    sewer, buried
```

A two-hour gutter hydrograph (peak 12 cfs) enters at `J_TOP`. Both inlets are
on grade; neither is flow-limited; both are 10 % clogged.

| | Attachment | Design | Captures to |
|---|---|---|---|
| Conduit-attribute inlet | `[INLET_USAGE]` on conduit `ST_A` | `Combo1` (2 inlets) | `MH1` |
| Inlet junction | `[INLET_JUNCTIONS]` node `IJ1`, between `ST_B` and `ST_C` | `Curb1` (1 inlet) | `MH2` |

## What the two forms differ in

A conduit-attribute inlet is an *attribute of a link*: capture is booked at the
host conduit's downstream node, and the bypass simply stays there. There is no
object at the inlet — you cannot give it a coordinate, a flood volume, or a
ponded depth of its own.

An inlet junction is a *node*. It sits at a real point on the street, so the
gutter depth at the inlet is the node depth, it can flood above the curb, and
the momentum of the street flow passes through it (it is a virtual junction:
zero storage, exactly two conduits of the same cross-section, zero offsets,
DYNWAVE only). Backflow from a surcharging capture node re-enters the street
at that point rather than at an abstract node.

A conduit attached to an inlet junction may not also carry an `[INLET_USAGE]`
row — that would capture the same water twice (rule 629). That is why the
conduit-attribute inlet in this deck sits on `ST_A`, upstream of the pair.

## Running it

```
openswmm-cli examples/inlets/street_inlet_junction.inp street_inlet_junction.rpt
```

## What to look for in the report

**Street Flow Summary** — one row per STREET conduit, with peak flow, maximum
spread (the width of water on the pavement) and maximum depth.

* `ST_A` carries the full hydrograph. Its peak spread is the widest in the
  model; compare it to the street's `Tcrown` of 20 ft to see how close the
  water gets to the crown.
* `ST_B` carries what `Combo1` did not capture, so its peak flow is lower than
  `ST_A`'s by the peak capture reported below, and its spread is narrower.
* `ST_C` carries what `Curb1` did not capture, and is narrower again. The
  three rows read as a staircase down the reach — that staircase is the whole
  point of putting inlets on a street.

**Street Inlet Flow Summary** — one row per inlet placement. The conduit
inlet is listed under its host link name (`ST_A`); the inlet junction is
listed under its node name with a `(node)` marker (`IJ1 (node)`).

* *Peak Flow* is the approach flow the inlet saw; *Peak Capture Pcnt* is the
  capture efficiency at that peak, and *Avg. Capture Pcnt* the average over
  every period with capture. A curb-opening inlet on grade loses efficiency
  as flow rises, so expect the peak-flow efficiency to sit below the average.
* *Bypass Flow Pcnt* is how often capture was partial. On a on-grade inlet
  with a rising hydrograph it should approach 100 %.
* *Back Flow Pcnt* is how often the capture node pushed water back up onto the
  street. It is 0 in this deck — the sewer never surcharges. To make it
  non-zero, shrink `SW_2` to `1.0` ft or raise the hydrograph peak, then watch
  `IJ1`'s backflow column and `ST_C`'s spread grow together.
* *Vol. Captured* / *Vol. Bypassed* close against the street continuity: the
  volume `ST_A` delivered equals what `Combo1` captured plus what it passed to
  `ST_B`.

**Node Depth / Flooding summaries** — `IJ1` appears as an ordinary junction.
Its `MaxDepth` of 0.5 ft is the flood threshold at the curb, so a peak that
exceeds the inlet's capacity floods `IJ1` rather than backing up into a
fictitious storage volume. `J_MID`, the conduit inlet's capture point, has no
such threshold of its own: that is the modelling difference the two forms make.

## Variations worth trying

* **Swap in the custom capture curve.** `Custom1` (a `CUSTOM` design reading
  the `DIV_CAP` `DIVERSION` curve — captured flow as a function of approach
  flow) is defined but unplaced. Put it on the inlet junction by changing the
  `Inlet` column of the `[INLET_JUNCTIONS]` row from `Curb1` to `Custom1`, and
  the HEC-22 curb-opening equations are replaced by a table lookup. The
  `[INLETS]` `CUSTOM` line carries no kind token — the curve's own `[CURVES]`
  type decides whether it is read as a diversion curve (approach flow) or a
  rating curve (ponded depth).
* **Use the unplaced grate.** `Grate1` is a plain `GRATE` design; swapping it
  for `Combo1` on the `[INLET_USAGE]` row shows how much of the combination
  inlet's capture came from the curb opening rather than the grate.
* **Make the inlet junction a sag.** Raise `OUT_ST` above `IJ1` so both `ST_B`
  and `ST_C` fall toward the node, and set the `Placement` column to
  `AUTOMATIC`. The inlet switches to the sag (weir/orifice) equations and the
  capture becomes a function of the ponded depth at `IJ1`.
* **Fuse the inlet junction away.** Through the editing API,
  `editor.fuse_inlet_junction("IJ1")` merges `ST_B` and `ST_C` back into one
  conduit and drops the usage row; `editor.split_conduit_inlet(...)` is the
  inverse.
