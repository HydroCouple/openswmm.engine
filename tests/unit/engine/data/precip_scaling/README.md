# Precipitation-scaling test models

Hand-checkable fixtures for the per-subcatchment precipitation scale factors and
the gage snow catch factor (SCF). Consumed by
`tests/unit/engine/test_subcatch_scale_factors.cpp` and mirrored into
`tests/regression/data/` for the legacy-vs-C++ regression suite.

Semantics under test (both engines must agree):

```
rainfall[j] = gage_intensity * Gage.scaleFactor * Subcatch.rainScaleFactor
snowfall[j] = gage_intensity * Gage.scaleFactor * Gage.SCF * Subcatch.snowScaleFactor
```

All four factors default to `1.0`, so a model that names none of them is
bit-identical to stock SWMM.

## `rain_scaled.inp` — warm, rainfall scale only

- Air temp pinned to **50 °F** (> 34 °F threshold) ⇒ every drop is rain.
- No snow pack: token 8 is the `*` placeholder, so token 9 (`RainScale = 0.5`)
  is still reachable — the same convention `[RAINGAGES]` uses for an omitted
  FILE start date.
- Gage SCF = 1.0, rain scale = **0.5**.

Expected: subcatchment rainfall is **exactly half** of the same run with the
rain scale forced back to 1.0. Runoff continuity closes in both runs.

## `snow_scaled.inp` — cold, all three factors non-default

- Air temp pinned to **20 °F** (< 34 °F) ⇒ every drop is snow.
- Gage SCF = **0.7**, subcatch snow scale = **1.3**, rain scale = 0.8 (inert
  while it is snowing, present only to prove both trailing tokens round-trip).
- Snow pack `SP1` assigned (token 8), so tokens 9/10 follow a real pack name.

Effective snowfall entering the pack:

```
1.0 in/hr * SCF 0.7 * snowScale 1.3 = 0.910 in/hr equivalent
```

Expected: the run completes and the snow-pack **runoff continuity still closes**
(this is the D3 accumulation-vs-melt hazard — `snow_plowSnow` and `getNetPrecip`
must scale snowfall identically or the pack mass balance diverges).
