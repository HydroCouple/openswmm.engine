# cedar_glen — a synthetic surveyed trunk chained by virtual junctions

| | |
|---|---|
| Decks | `cedar_glen_fv.inp`, `cedar_glen_dw.inp`, `cedar_glen_dw_extran.inp` |
| Generator | `generate_model.py` (seeded 20260816; regeneration is byte-stable) |
| Metadata | `model_meta.json` — the trunk's chainage, invert and diameter, used by the figure |
| Copied from | `~/Downloads/epaswmm5_qa/article/cedar_glen/` (the SWMM paper's demonstration folder), 2026-09-20 |
| Used by | the Application Manual's virtual-junctions chapter; figures `workflow_ch4_cedar_glen_profile_hgl`, `workflow_ch4_cedar_glen_worst_node_head` |
| Changed on copy | nothing |

## Provenance of the model itself

**Every coordinate, elevation and name is invented by the generator.**
Nothing in this folder comes from any client dataset. The model is built
in the *style* of a GIS-derived raw-water conveyance — a long trunk of
short survey segments chained by virtual junctions, two lateral branches,
a terminal flow-regulating basin with orifice outlets — because that shape
is what makes virtual junctions matter, not because it reproduces a
particular system.

## What it contains

387 conduits, 370 of them joined by virtual junctions and 17 by real
junctions; a 25 884 ft trunk of 72, 78 and 90 in pipe with 48 and 42 in
branches; slopes of 0.10 to 0.30 %; a double-pocket reach between the two
tie-ins with a 1.55 % adverse riser to a crest at 455 ft; and a 24-hour
double surge-and-drain hydrograph whose second peak is 85 % of the first.
Junction `SurDepth` of 40 ft keeps the conveyance sealed.

The three decks are identical but for their routing options: finite
volume, dynamic wave with the Preissmann slot, and dynamic wave with
EXTRAN surcharge.

## Run times on this machine

About 102 s for the finite-volume deck and 29 s and 9 s for the two
dynamic wave decks, on an Apple M-series laptop. The figure caches its
runs; `scripts/build_manual_figures.py run-decks` runs all three against
a 600 s budget.
