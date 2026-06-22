# Phase 7 — Normalized on-disk GeoPackage LINK schema — COMPLETE

Commit `6a99133e` on `swmm6_rel`. The link analogue of Phase 5 (nodes). The
relational refactor is now complete end to end: nodes + links, in-memory
(dense side-tables, wide arrays gone) and on-disk (FK-linked child tables).

## What changed
Flat NULL-padded `links` table → slim relational base `links` + five 1:1
FK-linked child tables (`conduits`/`pumps`/`orifices`/`weirs`/`outlets`),
PRIMARY KEY `(simulation_id, link_id)` + `FOREIGN KEY … REFERENCES links …
ON DELETE/UPDATE CASCADE`. The opaque `param1`/`param2` are gone — replaced by
NAMED columns (orifice `orientation` SIDE/BOTTOM, `weir_type`, outlet
`rating_type`).

`xsect_*` + `has_flap_gate` stay on the base `links` table (shared by
conduit/orifice/weir, mirroring base `LinkData` — they are not in any
`LinkSubtypes` side-table). `can_surcharge`/`discharge_coeff2`/orifice
`crest_height` are not side-table fields and were not carried (they were never
round-tripped by the flat schema either).

Files: `GeoPackageSchema.cpp` (DDL), `GeoPackageWriter.cpp` (base + per-type
child INSERT + code→str helpers), `GeoPackageReader.cpp` (base + per-child
SELECT passes + str→code, rejection gate), `docs/GEOPACKAGE_SCHEMA_ERD.md`,
`docs/openswmm_schema_review.sql`, `tests/unit/engine/test_geopackage.cpp`.

## Hard migration
`read_model` rejects any file missing the five link child tables (incl. a
Phase-5-era normalized-nodes/flat-links file) with an actionable error — no
crash, no silent misread, no compatibility view. The "schema version" is table
presence, not a numeric pragma.

## Gates — all green
| Gate | Result |
|------|--------|
| Build (asserts live) | clean |
| ctest | 86/86 (+ new `LinkSubtypeChildTablesRoundTrip`: pump/orifice/weir/outlet incl. TABULAR rating-curve name) |
| GPKG round-trip `.out` byte-parity | **EXACT ×18** epaswmm5_qa (incl. CMS/SI: extran9/user1/user3; structures: extran3 orifice, extran6 pump) |
| `PRAGMA foreign_key_check` | empty |
| FK cascade | delete link → child row cascades (verified extran6 pump 1→0) |
| Old-file rejection | flat-links file → rc≠OK, no crash |
| `openswmm_links.h` | byte-unchanged |

Round-trip harness reused: `docs/relational/qa_runs/phase_5/run_roundtrip.sh`
(it round-trips the whole model, links included). Run with all 18 model names.
