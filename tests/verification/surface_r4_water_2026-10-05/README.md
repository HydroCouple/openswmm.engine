# R4 source-group water qualification — 2026-10-05

The [implementation note](../../../plans/SURFACE_PROCESS_R4_SOURCE_WATER_2026-10-05.md) records scope, intentional completed-path behavior changes and remaining activation gates. **167 cases pass across nine isolated suites**, including **18 new water-driver cases**. Runtime surface ownership remains gated; this evidence does not activate general recharge.

## Reviewable evidence

- `baseline.json`: current parent commit and before-edit hashes; the parent includes the independent disconnected-outfall fix `cf242ea1`.
- `source-audit.json`: exact task-only source tree, working and isolated hashes, and reused build-source match. The working test CMake file also contains pre-existing unrelated registrations; the isolated/committed file contains only this task's addition.
- `isolated-tests.log`, `isolated-results.json`, `case-counts.json`, `isolated-final-cases/`: successful nine-suite run and individual cases. Build uses the preconfigured Release/2D/GeoPackage/HDF5 build with LTO disabled. Experimental libraries are not installed into the app/SDK.
- `mesh-adapter.json`: actual all-cell marcher source horizon, source clock horizon, shared receiver settlement and source balance. Mesh donors are absent; `production_caller:false` is intentional.
- `group-refinement.json`: 4/2/1 s group-water intervals compared with 0.25 s, outlet-volume differences and balance residuals. This is not a production performance measurement.

Fixtures are in `tests/unit/engine/test_source_water_driver.cpp`: programmatic US-unit sources with explicitly reviewed SI non-LID areas, 1 m² source footprints and unit-count × area LID footprints. LID state and all volume histories stay private until water/clock commit. Covered barrels use the existing kernel flag through a matching initial snapshot because model authoring does not yet retain COVERED. The report-isolation case writes and verifies a visible report sentinel under this folder's `models/` directory in the qualified source/build tree. Runtime fixtures and isolated archives/build logs are ignored; selected evidence files are committed.

The existing runoff/LID paths, receiving transactions, aquifer receiving, ET budgets, ownership gate, forcing clocks and explicit marcher suites pass alongside the new driver. No whole-model legacy census, constituent/rejection/restart qualification, generalized LID-store qualification, inside/outside soil partition or pre-debit mesh competition is claimed. Those remain required before R4 runtime activation.
