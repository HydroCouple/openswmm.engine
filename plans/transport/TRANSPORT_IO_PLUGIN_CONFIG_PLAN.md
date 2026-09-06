# Transport I/O & Plugin Configuration Plan (External Component Config Files)

**Status:** Approved direction, 2026-08-12
**Parent:** `plans/transport/UNIFIED_TRANSPORT_MASTER_PLAN.md` (decision D-UT8,
recorded there; applies across Phases T0–T7)
**Precedents mirrored:**
- `plans/2d_external_mesh_file.md` — `[2D_MESH_FILE] FILE <path>` sidecar
  pattern (relative-path resolution, override rules, no recursion, fatal
  parse errors).
- `[PLUGINS]` section + `src/engine/input/handlers/PluginsHandler.cpp` —
  per-line library spec with `key="value"` arguments feeding
  `ctx.plugin_specs` / `PluginFactory`.
- HydroCouple component convention (CSH/HTS/RHE/GW components, 2019 report):
  **one `.inp`-style configuration file per component**, bracketed sections,
  `;;` comments, file paths relative to the component file, fed to the
  component as a `File`-type `IArgument`.

---

## 1. Requirement (user decision 2026-08-12)

The legacy `.inp` must stay clean. Every transport/process component —
built-in (Eulerian ARD, reactions, water age, heat, 2D transport, GW
transport) **and third-party HydroCouple components** — is configured by its
**own external configuration file**, referenced from the `.inp` by a single
registration line. No bulky `[REACTION_*]`/`[HEAT_*]`/`[WATER_AGE_*]`
section bodies in the legacy input file.

## 2. Registration: `[PROCESS_COMPONENTS]`

One new `.inp` section, modeled on `[PLUGINS]`:

```
[PROCESS_COMPONENTS]
;;ComponentId / library            Arguments (key="value")
;;================================================================
org.hydrocouple.openswmm.reactions      config="model.rxn"
org.hydrocouple.openswmm.transport.ard  config="model.ard"
org.hydrocouple.openswmm.transport.lard config="model.lard"
org.hydrocouple.openswmm.heat           config="model.heat"
org.hydrocouple.openswmm.waterage       config="model.age"
org.hydrocouple.openswmm.integrated2d   config="model.i2d"
./components/libmycomponent.so          config="custom.cfg" version="1.2"
```

Per **D-UT9 as amended** (master plan), 2D surface and two-zone groundwater
share the same mesh and are **one unified component** — surface
hydrodynamics, subsurface flow, and all transport/heat behind a single id
and a single line. Surface-only / groundwater-only / integrated runs are
capability toggles inside `model.i2d`, not separate registrations.

- First token: **component id** (resolved through the `PROCESS_COMPONENT`
  plugin registry, master plan §3.1) or an explicit library path exporting
  `hydrocouple_component_info()` — same dual convention as `[PLUGINS]`.
- `config=` is the component's initialization file, passed verbatim as its
  `File`-type `IArgument` (`ArgumentInputType::File`,
  `hydrocouple.h`). Additional `key="value"` pairs map to further named
  arguments (String-type), enabling third-party components with multiple
  arguments.
- **Registration implies enablement.** A registered component participates
  in the run; removal disables it. Coarse `[OPTIONS]` keys that remain in
  the `.inp` are only those that select among engines or gate whole
  subsystems: `QUALITY_SOLVER`, `WATER_AGE ON|OFF`, `HEAT_TRANSPORT ON|OFF`,
  `2D_TRANSPORT ON|OFF` (a handful of lines, consistent with existing
  option style). Conflict rule: an `[OPTIONS]` toggle ON with no registered
  component (or vice versa) is a validation error with a precise message —
  no silent defaults.
- Handler: `ProcessComponentsHandler.{hpp,cpp}` beside `PluginsHandler`,
  populating `ctx.process_component_specs`; instantiation via
  `PluginFactory` at engine open, argument delivery + `initialize()` per
  the HydroCouple lifecycle (master plan §3.3).

## 3. Component configuration files

### 3.1 Format

Same dialect as the legacy `.inp` and the HydroCouple components: bracketed
`[SECTIONS]`, whitespace-delimited rows, `;;` comments. Rationale: (a) users
already know it; (b) the identical file drives the component under
HydroCoupleComposer (it is just the component's File argument there —
one config, two hosts); (c) the engine's section-parsing infrastructure is
reused (each component file gets its own section registry, per the
2D-mesh-file precedent rule 5 — no recursion, no `[PROCESS_COMPONENTS]`
inside component files).

### 3.2 Canonical section placement (supersedes earlier suite docs)

Sections previously drafted as legacy-`.inp` sections **move** to the
component files. The suite docs are amended to point here; this table is
the source of truth:

| File (suggested ext) | Component | Sections |
|---|---|---|
| `model.rxn` | reactions | `[REACTION_OPTIONS] [REACTION_SPECIES] [REACTION_COEFFICIENTS] [REACTION_TERMS] [REACTION_PIPES] [REACTION_TANKS] [REACTION_SUBCATCHMENTS] [REACTION_SOURCES] [REACTION_QUALITY] [REACTION_PARAMETERS] [REACTION_PATTERNS] [REACTION_REPORT]` |
| `model.ard` | Eulerian ARD | `[TRANSPORT_OPTIONS]` (scheme/limiter/dispersion/`ARD_TARGET_DX` — aliases of the `ARD_*`/`FV_*` keys) + the shared 1D sections below |
| `model.lard` | Lagrangian (LARD) | `[LARD_OPTIONS]` (`DISPERSION OFF\|RWPT`, `MAX_SEGMENTS_PER_LINK`, RNG seed, integrator prefs per the LARD plan) + the shared 1D sections below |
| *(shared 1D schema)* | consumed by whichever 1D engine `QUALITY_SOLVER` selects | `[TRANSPORT_BOUNDARIES] [TRANSPORT_SOURCES] [CONDUIT_DISPERSION] [STORAGE_MIXING]` — **identical section schema in both files** (engine-agnostic model data, not numerics), so switching engines never requires re-authoring boundary/source data, only moving the block |
| `model.heat` | heat (1D flux modules) | `[HEAT_OPTIONS] [HEAT_METEOROLOGY] [HEAT_SOURCES] [RADIATIVE_FLUXES] [SEDIMENT_EXCHANGE]` |
| `model.age` | water age | `[WATER_AGE_OPTIONS] [WATER_AGE_SOURCES]` |
| `model.i2d` | **integrated2d (unified, D-UT9)** | **everything on the shared mesh folded into one file**: capability toggles (`SURFACE_ROUTING`, `GROUNDWATER`, `TRANSPORT`, `HEAT`); the shared mesh (`[2D_VERTICES] [2D_TRIANGLES] [2D_VERTEX_NODE_MAP] [2D_TRIANGLE_NODE_MAP]`); surface hydraulics/parameterization (`[2D_OPTIONS]`, Manning/roughness, coupling params, `[2D_BOUNDARY_CONDITIONS]`, edge conveyance); subsurface flow parameterization (the `[2D_AQUIFER]`-lineage sections of the two-zone plan: zonation, closures, K, Sy, bed-layer params — on the same triangles); transport for all domains (`[2D_TRANSPORT_OPTIONS] [2D_TRANSPORT_BC] [2D_INITIAL_QUALITY] [GW_TRANSPORT_OPTIONS] [GW_TRANSPORT_PARAMS]`: dispersivity, D_m, ρs/Kd/λ); heat (`[2D_HEAT_OPTIONS]`, per-cell met overrides, sediment thermal props) |

**Fold-in rule (D-UT9, meshes and parameterization included):** `model.i2d`
is the *complete* home for the integrated surface–subsurface component —
one shared mesh defined once, surface and aquifer parameterization
side-by-side on it, transport and heat for every domain — one file, one
component. Large meshes may be kept in a separate file referenced from
within the component config (`[2D_MESH_FILE] FILE meshes/basin.2dm` inside
`model.i2d` — same rules, one level, no recursion) so text diffs of
parameters aren't drowned by vertex tables. `GROUNDWATER`-only
configurations (including `PER_SUBCATCH` degenerate columns) omit the
surface sections; surface-only omit the aquifer sections.

**Migration/back-compat:** existing embedded `[2D_*]` sections in the
`.inp` and the top-level `[2D_MESH_FILE]` section keep working
(deprecation warning); when `org.hydrocouple.openswmm.integrated2d` is
registered, its config file is authoritative and duplicated legacy
sections are a validation error (no silent merging). The GUI offers a
one-click "externalize to component config" migration. For all other
components the general rule stands: embedded sections accepted with a
style warning, external file wins on conflict (2D-mesh-file rule 4). The
GUI and `InpWriter` always write external.

### 3.3 Cross-references and path rules

- Element references (nodes, links, subcatchments, species) are by **name**,
  validated at component `initialize()`/`validate()` against the model —
  unresolved names are fatal with file+line diagnostics.
- Time series: reference model `[TIMESERIES]` by name, **or** external CSV
  per the CSH convention (`NAME  ./file.csv` rows in a local
  `[TIMESERIES]` section of the component file). Both resolve at init.
- Paths inside a component file resolve relative to **that file** (CSH/hcp
  convention); the `config=` path itself resolves relative to the `.inp`
  (2D-mesh-file §3). No recursion; fatal parse errors (rules 5–6).

## 4. Writers, containers, round-trip

- `InpWriter` emits `[PROCESS_COMPONENTS]` + the coarse toggles; each
  component serializes its own config through its `IArgument::saveData()` /
  `toString()` surface (HydroCouple contract) into its file. One writer per
  format, owned by the component — the engine never hand-writes another
  component's sections.
- **GeoPackage:** component configs are stored as text blobs in a
  `process_component_configs` table (id, version, blob, original filename)
  so a `.gpkg` remains a single-container hand-off; on open they are
  materialized to sidecar files beside the project (user-visible, per
  CLAUDE.md §4.1 transparent file I/O) and re-embedded on save. Decision
  **D-IO2**; extract-location override in `[FILES]`-style option.
  **Recorded tradeoff for the IO4 review (2026-08-16):** legacy pollutants
  remain FIRST-CLASS relational tables (`pollutants`, landuse/buildup/
  washoff/treatment — already shipped in GeoPackageWriter/Reader);
  component configs are blobs. Blob keeps one parser and one
  component-owned format but is opaque to SQL/GIS tools and the GUI's
  relational stack; first-class `reaction_*` tables would be
  queryable/editable relationally at the cost of a second parser and
  schema-sync across every section of every component. Default position:
  **blob canonical at IO4**, with optional read-only relational VIEWS
  (generated from the parsed config at save time, clearly marked derived)
  as a later enhancement if GUI/SQL use cases demand — never two writable
  representations of the same data.
- Round-trip gate: `.inp` + sidecars → engine → write → identical semantics
  (section-order/comment normalization allowed); `.gpkg` embed/extract
  cycle byte-stable for unchanged configs.

## 5. C API / Python / MCP

```
include/openswmm/engine/openswmm_process_components.h
  swmm_process_component_count / _get(id, version, config_path, enabled)
  swmm_process_component_register(id_or_lib, config_path, kv_args)
  swmm_process_component_remove(id)
  swmm_process_component_reload_config(id)      // re-init from file, staleness-guarded
  swmm_process_component_config_get/set(id, text)  // whole-file text surface for editors
```

Python `sim.components` (list/register/remove/reload, `component.config_path`,
`component.config_text`); MCP `process_components_*` tools (DSL/file-text
level only). Parity registries per G-UT6. The per-domain APIs already
planned (`sim.reactions`, `sim.transport`, `sim.heat`, `sim.water_age`)
remain the fine-grained programmatic path — they mutate the same in-engine
state the config files initialize, and `saveData()` persists either origin
to the component file.

## 6. GUI

Extends `openswmm.gui/workplans/TRANSPORT_QUALITY_GUI_PLAN_2026-08-12.md`:

- **Files/Plugins tab** of Simulation Options gains a Process Components
  table (`PluginsTableModel` + `PathBrowseDelegate` precedent): component
  combo (discovered via `PROCESS_COMPONENT` registry), config-file path cell
  with browse, enable = row presence. `FileFilterRegistry` `FilterKind`s per
  config type (`.rxn`, `.ard`, `.heat`, `.age`, `.t2d`, `.gwt`).
- The domain editors (Reaction System, Water Age Sources, Heat, transport
  options groups) keep editing **engine state** (engine remains the model);
  Save writes through the component's `saveData()` to its config file and
  marks the project dirty. Editors show the bound config filename in the
  title bar; a missing registration offers "Create component + config file"
  in one step.
- New-model flow: enabling a toggle (e.g. `HEAT_TRANSPORT ON`) prompts to
  create + register the sidecar with defaults next to the `.inp`.

## 7. Implementation phases

```
IO1  [PROCESS_COMPONENTS] handler + spec plumbing + registry resolution +
     coarse-toggle/registration consistency validation.
     → verify: parse/validate matrix (registered+ON, registered+OFF,
       unregistered+ON, dup ids, bad lib path); third-party stub component
       loads via library path with String+File args.
IO2  Component-file section registries + relative-path resolution + embedded
     fallback with style warning + override rule.
     → verify: 2D-mesh-file rule parity tests (rules 1–6 analogues); same
       .rxn file initializes the reactions component under openswmm and
       under HydroCoupleComposer (G-UT5 extension).
IO3  Writers: per-component saveData(); round-trip gate. (InpWriter's
     registration-line round-trip landed with IO1, `64c831d6`.) Carried
     obligation from IO1 validation: `config_path` currently bypasses the
     Slice IO-4 path rebase on save-as — the IO3 fix must rebase the
     reference AND carry the config file alongside to the destination (the
     [2D_MESH_FILE] sidecar pattern), never rebase alone.
IO4  GeoPackage embed/extract (D-IO2) + materialization rules.
IO5  C API/Python/MCP surfaces + reload/staleness semantics + parity
     registries. Carried obligation from R1 validation: the apply-hook
     signature has an `errors` sink but no WARNINGS sink, so components
     push to `ctx.warnings` directly and the `push_report_warning`/
     `emit_warning` path never fires — .rpt sees component warnings, but
     API/GUI warning subscribers do not. IO5 amends the
     `ComponentConfigApply` signature (or supplies a ctx warning-emit
     helper components are required to use) so component warnings reach
     every subscriber channel.
IO6  GUI Files-tab table + FilterKinds + editor↔file binding (with GUI G1/G2).
```

Sequencing: IO1–IO2 land with master-plan Phase T0/T1 (the reactions
component is the first consumer); IO3–IO6 track the component that needs
them (heat file with T4, 2D with T6, GW with T7).

## 8. Open items

- Extension names above are suggestions; confirm before IO2 (avoid
  collisions with common tools; `.rxn` vs `.msx`-adjacent naming).
- ~~Whether `[2D_TRANSPORT_*]` rides the external 2D mesh file~~ —
  **resolved by D-UT9 (2026-08-12):** transport and heat sections live in
  the unified `model.i2d` component config, which owns the mesh too
  (optionally referencing a mesh-only file from within itself).
- Include-once guard vs multiple instances of the same component id
  (proposal: one instance per id per model in v1).
- Deprecation horizon for top-level `[2D_MESH_FILE]` / embedded `[2D_*]`
  once `integrated2d` registration is the norm (proposal: warn now, decide
  removal at the T6 release review).
- ARD/LARD coexistence: both may be registered, but `QUALITY_SOLVER`
  activates exactly one per run (the inactive registration is validated but
  dormant — validation info message, not an error — so users can flip
  engines by changing one `[OPTIONS]` line). Whether the shared 1D sections
  should instead live in a third, engine-neutral file both components read
  (eliminating the copy-on-switch) — decide at IO2 review with the LARD
  team hat on.
