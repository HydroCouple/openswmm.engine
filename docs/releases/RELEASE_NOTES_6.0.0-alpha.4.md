# OpenSWMM Engine 6.0.0-alpha.4

**Pre-release.** APIs, file formats and defaults may still change before 6.0.0.

Covers everything merged since [`v6.0.0-alpha.3`](https://github.com/HydroCouple/openswmm.engine/releases/tag/v6.0.0-alpha.3) (2026-08-11) — 285 commits. See [`CHANGELOG.md`](https://github.com/HydroCouple/openswmm.engine/blob/main/CHANGELOG.md) for the itemized record.

---

## Licence change

The engine is **relicensed from MIT to the Apache License, Version 2.0** (#123, #122). Apache 2.0 keeps the permissive terms downstream users rely on and adds an explicit patent grant and contribution clause. If you redistribute the engine, review your attribution and NOTICE handling before upgrading.

## Highlights

### A finite-volume 1D routing solver

`FLOW_ROUTING FV` selects an explicit, conservative finite-volume solver alongside the existing dynamic-wave routing. It arrives with second-order MUSCL reconstruction (`FV_ORDER 2`), local time stepping (`FV_LTS`, on by default), semi-implicit node coupling (`FV_NODE_COUPLING`), `FV_TIME_INTEGRATION RK2`, cell-resolved Eulerian scalar transport on the FV mesh, DUMMY-link routing, and FV solver statistics in the report file. `RouteModel.FV` is exposed in the Python bindings and `FV` in the MCP server.

Several mass-balance defects found while validating it are fixed in this cycle: junction storage lost from the FV routing balance, storage-node evaporation and exfiltration charged to the wrong term, conduit seepage 43,200× too large under US units, and a model the mesher could not handle running with no routing at all rather than failing. The depth inversion is also 3.2× faster at bit-identical results, and published node stages no longer stand above the conduits they connect.

### Pressurized flow and unsteady friction

A **two-component pressure approach (TPA)** models sub-atmospheric pressurized flow, and **unsteady friction** (`UNSTEADY_FRICTION VITKOVSKY`, `UF_K3`) is available in both solvers. `REPORT_SIGNED_HEADS` writes true signed piezometric heads to the `.out` file, and virtual-junction initial state is now seeded under dynamic-wave routing.

### 2D: momentum closures and mixed meshes

`MOMENTUM_EQUATION FULL_SWE | DIFFUSIVE_WAVE` selects the 2D momentum closure, and the mesh may now mix triangles and quadrilaterals. `swmm_2d_get_run_stats` / `Surface2D.run_stats` report the solver backend, momentum closure and local-time-stepping tiers actually used for a run. A prescribed-discharge boundary under `FULL_SWE` now delivers exactly what it prescribes, per-cell cumulative rainfall volume is written to the 2D sidecar, and the 2D results file records what its coordinates mean (`Mesh2_node_x/y`). A performance regression introduced by the tri-quad and `FULL_SWE` work is fixed, with LOCAL_INERTIAL results byte-identical before and after.

### Street inlets

A legacy-parity **HEC-22 capture kernel**, `[INLET_JUNCTIONS]`, and an inlet editor round-trip. `[INLETS]` two-line combination inlets are now merged correctly, and `[INLET_USAGE]` — previously parsed but never written — round-trips.

### Transport and water quality

A **Lagrangian transport engine (LARD)** joins the quality solvers, MSX species now ride LARD segments and react, and water age and ARD write their own configuration files, completing the IO3 save work. `TEMP` is available in reaction expressions for temperature-dependent kinetics, and heat transport gains incoming shortwave forcing (`[RADIATIVE_FLUXES]`). A `[POLLUTANTS]` Kdecay defect that applied decay **86,400× too fast** is fixed.

### Round-trip integrity

A family of save-path defects is closed. Groundwater was **silently lost on every save** (`[AQUIFERS]`, `[GROUNDWATER]`); a `[GROUNDWATER]` receiving node declared later in the file was never resolved; links declared before the node sections orphaned silently; five writers emitted `*` where the name was known, three of them fatally; four external-file slots were written verbatim rather than as authored; and a `[2D_MESH_FILE]` reference dangled after Save As. The `.inp` writer now emits authored form for offsets and conduit orientation, and a GUI-editor round-trip API exists for `[GWF]` expressions.

### Interfaces and performance

**Live `.out` reading** — `swmm_output_open_live(path)` opens a binary results file while the run that is writing it is still going. The **C / Python / MCP parity matrix is at zero gaps in all three columns**, with Python bindings for the five transport-process headers and Preissmann slot / 2-D name and bulk accessors. Model load and initialization are **up to 17× faster**.

---

## Upgrading

- Review the licence change above before redistributing.
- `FLOW_ROUTING FV` and `MOMENTUM_EQUATION` are opt-in; existing models keep their previous routing.
- Models that relied on the Kdecay defect will now decay at the authored rate.
- Models with groundwater that were saved by an earlier build should be checked — `[AQUIFERS]` and `[GROUNDWATER]` may have been dropped on save.

## Acknowledgements

Thanks to everyone who filed the issues driving this cycle, in particular the pressurized-flow, inlet and groundwater reports.
