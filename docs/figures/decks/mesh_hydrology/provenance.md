# mesh_hydrology — rain, infiltration and an aquifer on a small mesh

| | |
|---|---|
| Deck | `mesh_hydrology.inp` |
| Written for | the Application Manual's mesh-hydrology chapter; figures `workflow_ch7_rain_and_infiltration` and `workflow_ch7_mesh_groundwater`. The Hydrology Reference's `hydrology_ch9_sigma_column` runs variants of it. |
| Composed from | the shapes of `tests/output/aquifer_2d/dest_ok.inp` and `tests/unit/engine/data/infil2d_out/g11_dest.inp`, 2026-09-20 |
| Changed | 2026-11-21, see below |

## What it is

A 20 × 20 m patch of ground meshed as eight triangles over nine vertices,
falling gently toward a drain at the centre which is coupled to a junction
and a short pipe. Two rain gages stand west and east reading 20 and
5 mm/hr, so every cell takes a different rainfall. With two gages there is
no triangle of gages to interpolate inside, so the engine weights them by
inverse distance in every cell. Three soil tags — `LAWN`, `PAVED`, `WOODS`
— carry Horton, constant-rate and Green-Ampt rows, and a mesh-wide
two-layer aquifer sits beneath them. Because the aquifer row covers every
cell, the aquifer owns every cell's infiltration and the tag rows are
skipped; the report says so for each row.

One hour of rain, two hours of recession, three-hour run.

## Why it was written rather than copied

No fixture exercised all three of the chapter's questions at once —
several gages, several infiltration methods, and a destination for the
infiltrated water. The two fixtures named above supply the shapes: the
aquifer deck's `[2D_AQUIFER]` row and the infiltration deck's tagged
triangles.

## Three things the deck deliberately carries

`AREA 0.07` on the coupling row is the drain pipe's own area. An earlier
version used 1.0 m² and the engine warned that the orifice could inject
fourteen times what the pipe can convey; the chapter quotes that warning.

The gage interval is `1:00`, matching the rainfall series. With a 5-minute
interval against hourly series values the engine reads only the given
instants and the storm loses most of its volume — which is a real trap,
not an engine defect.

`[COORDINATES] J1 10.0 10.0` places the junction on the drain vertex it is
coupled to. `NODE_ENROLMENT AUTO` gives a bed only to a node whose
coordinates fall in a cell, so without this row the aquifer has nothing to
exchange with.

## Changes

2026-11-21:

- Removed `[2D_OPTIONS] INFIL_DESTINATION AQUIFER_2D`. Since 2026-10-05 the
  engine refuses that value when the run opens ("INFIL_DESTINATION
  AQUIFER_2D is obsolete; remove this option. Aquifer-owned cells compute
  their receiving capacity", `src/engine/2d/SurfaceRouter2D.cpp:1682-1684`),
  so the deck no longer ran. A cell an aquifer row covers belongs to the
  aquifer whatever the destination says (`SurfaceRouter2D.cpp:1668-1681`).
- Added `[COORDINATES]` for `J1`, for the node bed described above, between
  a new `;//! [coordinates]` marker pair.
- Rewrote the header comment: the rainfall weights are inverse-distance
  (two gages), and the tag rows do not apply under the aquifer.

## Snippet markers

`;//! [options]`, `[gages]`, `[twod]`, `[infiltration]`, `[aquifer]` and
`[coordinates]` bracket the blocks the chapter quotes with `\snippet`.
