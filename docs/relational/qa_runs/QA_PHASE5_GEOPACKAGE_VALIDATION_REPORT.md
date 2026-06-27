# QA Phase 5 (relational GeoPackage schema) — Validation Report

## Normalize the on-disk GeoPackage node schema + byte-exact round-trip

**Date:** 2026-06-21
**Driver:** build-equipped agent (implemented + verified end-to-end with the build loop)
**Repo:** `openswmm.engine`  ·  **Branch:** `swmm6_rel`
**Build tree:** `build-arm64-osx` (Ninja, Debug, NDEBUG undefined → asserts live)

---

## Verdict: PASS — relational node schema complete; GeoPackage round-trip is byte-exact across the entire QA corpus (CFS + CMS).

The on-disk GeoPackage `nodes` table is now a **true relational schema**: a base
`nodes` table (common columns + discriminator + geometry) plus 1:1
`storages` / `outfalls` / `dividers` child tables joined by a hard FK on
`(simulation_id, node_id)` with `ON DELETE/UPDATE CASCADE`. The flat
NULL-padded subtype columns are gone. `nodes` keeps the GeoPackage-required
`fid INTEGER PRIMARY KEY AUTOINCREMENT`; the child FK targets the existing
`UNIQUE(simulation_id, node_id)` key.

Verifying the refactor required the GeoPackage save/reload round-trip to be
correct first; it was **broken in many ways unrelated to nodes**, all now fixed.

---

## The handoff's premise was wrong: the GPKG round-trip was broken before any schema change

Empirically (harness `docs/relational/qa_runs/phase_5/gpkg_roundtrip.cpp`: open
INP → `swmm_model_write_with_plugin` → `.gpkg` → open via GPKG input plugin →
run → byte-compare `.out` vs the INP golden), **0 of the QA models round-tripped
byte-exact** at the start. Root causes found + fixed:

1. **Gage crash** — `read_rain_gages` under-sized the gage SoA (only 7 of ~25
   columns), leaving `ts_name` etc. unsized; `resolve_cross_references` then read
   OOB → `EXC_BAD_ACCESS`. Fixed with `grow_to` + store `ts_name`.
2. **Object read ordering** — every feature/data read used `SELECT … WHERE
   simulation_id=?` with no `ORDER BY`, so SQLite returned rows in
   `UNIQUE`-index (id-sorted) order, not insertion order → `.out` object order
   scrambled. Fixed with `ORDER BY fid` on nodes/links/subcatch/gages/curves/
   timeseries/patterns.
3. **`[INFLOWS]` / `[DWF]` not persisted** — no tables at all; 9/12 QA models ran
   with zero inflow → trivial output. Added `inflows` + `dwf_inflows` tables +
   writer + reader.
4. **`[CONTROLS]` not persisted** — control rules dropped; controlled pumps
   (extran10) ran uncontrolled. Added `control_rules` table.
5. **`[TRANSECTS]` not persisted** — IRREGULAR channels lost all geometry
   (user2/user5). Added the full `transects` round-trip + IRREGULAR/STREET/CUSTOM
   transect-name round-trip (the writer had stored a garbage curve name from the
   transect index).
6. **Cross-section round-trip lossy** — stored the *derived* y_full/w_max/a_full/
   y_bot instead of the raw `[XSECTIONS]` geom1-4, destroying TRAPEZOIDAL bottom
   width + side slopes. Now stores raw geom1-4 and rebuilds.
7. **Orifice/weir/outlet type dropped** — `links.param1/param2/orate` not
   persisted; a SIDE orifice (extran9) loaded as BOTTOM. Added them.
8. **Conduit `direction` dropped** — adverse-slope DW conduits are stored
   already-reversed (positive slope) so `direction` can't be re-derived; without
   it the `.out` offset-direction echo + routing diverged (user2/user5). Now
   persisted.
9. **Metric (CMS) unit round-trip** — the writer stored internal feet while the
   reader re-applied the display→internal conversion, compounding ×3.2808 per
   cycle (the GUI "metric-save inflation" bug). **Root-caused + fixed** by making
   the `.gpkg` a canonical internal-unit store: `write_model` writes internal
   as-is; `read_model` sets `ctx.gpkg_units_internal` and
   `resolve_cross_references` skips the (non-invertible ×0.3048) conversion. The
   only display-unit fields written are the link xsect raw geom1-4, which the
   reader converts in lock-step with `convert_inputs_to_internal`'s xsect block.
10. **Float precision** — timeseries timestamps stored as relative-hours at
   `%.6f` (rounded 5-min steps) and option dates/steps via `std::to_string`
   (6 dp). Now timeseries store the raw OADate and option doubles store at
   `%.17g`.

---

## Gates

- **Build** (asserts on): clean. **ctest: 86/86** (incl. a new
  `NodeSubtypeChildTablesRoundTrip` test covering the divider subtype — no QA
  model exercises dividers — and every previously-dropped lossless field:
  storage seep/evap/exfil, outfall route_to, divider cd/max_depth/curve/link).
- **`.out` byte-parity vs INP golden: EXACT for all 18 QA models present**
  (`extran1-4,6,7,9,10`, `test1-5`, `user1-5`; `extran5`/`extran8` have no
  `.inp`), covering CFS and CMS (`extran9`, `user1`, `user3`).
- **FK integrity:** `PRAGMA foreign_key_check` returns empty; deleting a node
  cascades its child row (storages 28→27); renaming a node_id cascades to the
  child (UPDATE CASCADE).
- **Non-conformant rejection:** opening a pre-relational flat `.gpkg` (no
  `storages`/`outfalls`/`dividers`) returns a clean error (no crash, no silent
  misread); a conformant file still opens.
- **Frozen API:** public C header `include/openswmm/engine/openswmm_nodes.h`
  unchanged.

## Files changed (engine)
`GeoPackageSchema.cpp` (relational node DDL + inflows/dwf/control_rules/transects/
links param cols), `GeoPackageWriter.cpp`, `GeoPackageReader.cpp`,
`PostParseResolver.cpp` (skip-convert for gpkg), `SimulationContext.hpp`
(`gpkg_units_internal` flag), `DefaultOutputPlugin.cpp` (unchanged logic; the
`direction` echo is now fed correct data), `tests/unit/engine/test_geopackage.cpp`.

## Artifacts (`docs/relational/qa_runs/phase_5/`)
`gpkg_roundtrip.cpp` + `run_roundtrip.sh` (round-trip harness), `dump_model.cpp`
(field-level diff harness), `BASELINE_FINDINGS.md` (diagnosis log), this report.
