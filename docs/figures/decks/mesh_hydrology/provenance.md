# mesh_hydrology — rain, infiltration and an aquifer on a small mesh

| | |
|---|---|
| Deck | `mesh_hydrology.inp` |
| Written for | the Application Manual's mesh-hydrology chapter; figure `workflow_ch7_rain_and_infiltration` |
| Composed from | the shapes of `tests/output/aquifer_2d/dest_ok.inp` and `tests/unit/engine/data/infil2d_out/g11_dest.inp`, 2026-09-20 |

## What it is

A 20 × 20 m patch of ground meshed as eight triangles over nine vertices,
falling gently toward a drain at the centre which is coupled to a junction
and a short pipe. Two rain gages stand west and east reading 20 and
5 mm/hr, so the natural-neighbour interpolation produces a different
rainfall in every cell. Three soil tags — `LAWN`, `PAVED`, `WOODS` — give
the cells Horton, constant-rate and Green-Ampt infiltration respectively,
and a mesh-wide two-layer aquifer sits beneath them.

One hour of rain, two hours of recession, three-hour run.

## Why it was written rather than copied

No fixture exercised all three of the chapter's questions at once —
several gages, several infiltration methods, and a destination for the
infiltrated water. The two fixtures named above supply the shapes: the
aquifer deck's `[2D_AQUIFER]` row and the infiltration deck's tagged
triangles.

## Two things the deck deliberately carries

`AREA 0.07` on the coupling row is the drain pipe's own area. An earlier
version used 1.0 m² and the engine warned that the orifice could inject
fourteen times what the pipe can convey; the chapter quotes that warning.

The gage interval is `1:00`, matching the rainfall series. With a 5-minute
interval against hourly series values the engine reads only the given
instants and the storm loses most of its volume — which is a real trap,
not an engine defect.

## Snippet markers

`;//! [options]`, `[gages]`, `[twod]`, `[infiltration]` and `[aquifer]`
bracket the blocks the chapter quotes with `\snippet`.
