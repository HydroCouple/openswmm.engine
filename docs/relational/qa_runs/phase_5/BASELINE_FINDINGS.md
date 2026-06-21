# Phase 5 — pre-existing GPKG round-trip baseline (before any Phase-5 change)

**Date:** 2026-06-21  ·  HEAD = `aab0bc17` (Phase 4)  ·  build-arm64-osx (Debug, asserts on)

Harness: `gpkg_roundtrip.cpp` — golden (INP→run→`golden.out`) vs round-trip
(open INP → `swmm_model_write_with_plugin` → `.gpkg` → open `.gpkg` via GPKG input
plugin → run → `rt.out`), byte-compare. Real production C-API paths.

## Result: the .out byte-parity gate is currently UNREACHABLE due to 3 pre-existing GPKG bugs (none introduced by Phase 5).

| model | flow | outcome |
|-------|------|---------|
| extran2 | CFS | runs; `.out` DIFF (object order only) |
| test1   | CFS | runs; `.out` DIFF (object order only) |
| user4   | CFS | **crash** in roundtrip-run (resolve) |
| user1   | CMS | **crash** in roundtrip-run (resolve) |
| user3   | CMS | **crash** in roundtrip-run (resolve) |

### Bug #1 — object read ordering (all feature tables)
`read_nodes` (and `read_links`/`read_subcatchments`/`read_rain_gages`) do
`SELECT ... WHERE simulation_id=?` with **no `ORDER BY`**. SQLite satisfies the
`WHERE` via the `UNIQUE(simulation_id, <id>)` index, so rows come back **sorted by
id**, not insertion (`fid`) order. The writer is correct (gpkg `fid` order =
definition order); the reader reorders.
- Proof: `SELECT node_id FROM nodes WHERE simulation_id='default'` → `10208,10309,15009,…`
  vs `… ORDER BY fid` → `80408,80608,81009,…` (the .inp definition order).
- Effect: `.out` lists objects in a different order → byte DIFF on every model.
- Fix: add `ORDER BY fid` to the feature-table reads. Node part is in Phase-5 scope.

### Bug #2 — unit asymmetry on write (the known "metric-save ft inflation")
`write_model` writes `ctx` **as-is, with no internal→display conversion** (only
`InpWriter` converts — see `InpWriter.cpp:490`: *"each save dumps internal feet and
the next open re-applies the m→ft factor, compounding ×3.28084 per cycle"*). The
GPKG writer never got that fix. `read` always runs `convert_inputs_to_internal`
(`SWMMEngine.cpp:202`, unconditional).
- Proof: user1 (CMS) node `01e43y44` invert = **16.08 m** in `.inp`, stored as
  **52.7559** in the gpkg → ratio **3.2808 = ft/m**. Re-read converts m→ft again → ×3.28.
- Effect: CFS (internal≡display) round-trips unit-correct; **SI inflates ×3.28/cycle**.
- Fix: mirror `InpWriter`'s copy-and-`convert_internal_to_display` in the GPKG writer.

### Bug #3 — resolve crash on subcatchment/raingage models (pre-existing, NOT node-related)
user1/user3/user4 crash (`EXC_BAD_ACCESS`, bad `std::string`) during the gpkg open's
`resolve_cross_references` — `read_model` completes, the resolve pass crashes. Only
on models carrying `[SUBCATCHMENTS]`/`[RAINGAGES]` (extran*/test* node-only models
round-trip fine). This is a subcatch/gage round-trip completeness gap, in the node
refactor's blast-radius only incidentally.

---

# Progress update (round-trip correctness grind)

**Bugs fixed so far (all real GeoPackage round-trip defects):**
1. Gage crash — `read_rain_gages` under-sized SoA (grow_to + store ts_name).
2. Object read ordering — `ORDER BY fid` on nodes/links/subcatch/gages/curves/timeseries/patterns (SQLite was returning index/alphabetical order).
3. `[INFLOWS]` + `[DWF]` persistence (new schema tables + writer + reader) — drove 9/12 QA models to zero-inflow before.
4. Unit conversion on write — `write_model` now `convert_internal_to_display` on a copy (the `.inp`-style display-unit store). **Root-causes + fixes the GUI metric-save ×3.2808 inflation.**
5. Link cross-section RAW geom1-4 persistence (was storing derived y_full/w_max/a_full/y_bot → lossy for TRAPEZOIDAL etc.).
6. `links.param1/param2/orate` persistence (orifice SIDE/BOTTOM, weir type, end-contractions, open/close rate) — a SIDE orifice was silently loading as BOTTOM.
7. `[TRANSECTS]` persistence (was never written/read → IRREGULAR channels lost all geometry).
8. IRREGULAR/STREET/CUSTOM link transect/curve NAME round-trip (writer wrote a garbage curve name from a transect index).
9. Timeseries timestamp precision — `%.6f` hours rounded sub-6dp times (e.g. 5-min steps) → switched to `%.17g`.

**QA round-trip status (.out byte-parity vs INP golden):**
- EXACT: extran2, test1, user4 (CFS), extran9 (CMS).
- DIFF (tiny LSB): user2, user5 (CFS — residual non-node field, TBD/findable), user1, user3 (CMS).

**Fundamental wall for guaranteed CMS byte-exactness:**
The display-unit storage convention requires `internal = display×(1/0.3048)` on read and `display = internal×0.3048` on write. `0.3048` is not a clean binary fraction, so `×0.3048` then `×(1/0.3048)` is NOT bit-invertible — some metric values lose 1–2 ULPs per round-trip, which the dynamic-wave solver amplifies. Guaranteeing byte-exact CMS requires switching the gpkg to **internal-unit canonical storage** + skipping `convert_inputs_to_internal` on gpkg read (invasive: touches `resolve_cross_references`/`SWMMEngine`, and reworks the xsect raw-geom handling). CFS models (len==1.0, convert is a no-op) are unaffected and remain achievable byte-exact.

---

# Update 2 — internal-unit storage (bit-identical metric) + remaining residual

Per user decision, switched the .gpkg to **canonical internal-unit storage**:
- write_model no longer converts to display (writes engine-internal ft/cfs as-is).
- SimulationContext.gpkg_units_internal flag → resolve_cross_references SKIPS
  convert_inputs_to_internal for gpkg loads (bit-exact; avoids non-invertible ×0.3048).
- The only display-unit fields written are the link cross-section RAW geom1-4; the
  reader applies the same xsect ×inv_len conversion convert would (lock-step).
- Timeseries timestamps now stored as RAW OADate at %.17g (was lossy relative-hours
  /date-string); option dates/steps/tolerances now %.17g (was std::to_string 6dp).

**QA .out byte-parity now: EXACT for extran2, test1, user4 (CFS) AND user1, user3,
extran9 (CMS) — all three metric models now bit-identical.**

Remaining: user2, user5 (CFS) — **reports numerically identical** (rpt diff is
cosmetic: TITLE + timestamps only). The .out differs by a few hundred–1700 bytes,
localized to the INPUT-ECHO section (before OutputPos): conduit offset1/offset2 appear
SWAPPED within ~1-2 conduit records (e.g. user5 704chan 11.32/14.19), even though the
post-init C-API getters (offset_up/offset_dn) are bit-exact and from/to nodes match.
i.e. NOT a stored-data round-trip gap — a subtle offset-orientation artifact in the
.out echo path that differs between the .inp and gpkg load paths. Below engineering
significance (results identical).
