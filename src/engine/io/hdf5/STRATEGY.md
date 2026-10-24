# HDF5 I/O Strategy for OpenSWMM

Companion to `src/engine/input/geopackage/STRATEGY.md`. Where that document
designs a *relational, queryable* container, this one designs a *columnar,
array-native* one: the same model, laid out the way HDF5, h5py, xarray and
MATLAB expect to find it.

## 1. Why a third format

`.inp` is the interchange format and the only one a legacy engine reads.
`.gpkg` is the spatial/relational store, good for GIS and SQL. Neither is a
good fit for the array-shaped consumers of this model — Python analysis,
notebooks, ML pipelines, or anything that wants to slice 10⁶ links without
parsing text or issuing SQL.

HDF5 also already carries this project's 2D results
(`src/engine/2d/output/Default2DOutputPlugin.cpp`), so the dependency, the
toolchain and the symbol-hiding constraints are established. This makes the
model side a natural extension rather than a new dependency.

## 2. Design principles

| Principle | Rationale |
|---|---|
| **Columnar, not relational** | One group per object class, one equal-length 1-D dataset per attribute. A reader gets `links/conduits/length` as a contiguous array — no joins, no row iteration. This mirrors the engine's own SoA layout (`LinkData`, `NodeData`), so read and write are both near-memcpy. |
| **AUTHORED units, exactly like the `.inp`** | See §4. This is the single most consequential decision in the format and it is deliberately *not* GeoPackage's. |
| **Self-describing** | Every group carries a `units` and `description` attribute per column. A file opened five years from now explains itself without this repository. |
| **Cover what GeoPackage drops** | The file-IO audit (`plans/FILE_IO_PARITY_AUDIT_2026-09-22.md` §6) lists fourteen `.inp` sections the `.gpkg` schema has no column for. That list is this schema's requirements document, not a precedent to copy. |
| **Versioned from day one** | Root attribute `schema_version`. The reader checks it and refuses a major it does not know, rather than silently misreading. |
| **Plugin-shaped** | `IInputPlugin` / `IOutputPlugin` / `IReportPlugin`, exactly like GeoPackage, so `swmm_model_write_with_plugin` needs no engine change. |

## 3. Layout

```
/                                 attrs: schema_version, engine_version,
                                         flow_units, unit_system, created_utc,
                                         title
/options                          attrs: one per option key (scalar, typed)
/report                           attrs: flags; datasets: subcatchments, nodes, links
/nodes/junctions                  name, invert_elev, full_depth, init_depth,
                                  sur_depth, ponded_area, tag, comment
/nodes/outfalls                   + bc_type, stage, curve, has_flap_gate, route_to
/nodes/dividers                   + div_link, method, cutoff, curve, qmin, dh_max, cd
/nodes/storage                    + shape, curve, a0, a1, a2, p1, p2, p3,
                                  evap_frac, exfil_*
/links/conduits                   name, node1, node2, length, roughness,
                                  offset1, offset2, q0, q_limit, barrels,
                                  culvert_code, loss_*, seep_rate, has_flap_gate
/links/pumps|orifices|weirs|outlets
/links/xsections                  link, shape, geom1..geom4       (see §5)
/subcatchments                    name, gage, outlet, area, pct_imperv, width,
                                  slope, curb_length, snowpack, + subarea and
                                  infiltration columns
/hydrology/aquifers|groundwater|gwf|snowpacks|lid_controls|lid_usage
/quality/pollutants|landuses|buildup|washoff|coverages|loadings|treatment
/loading/inflows|dwf|rdii|hydrographs
/tables/curves                    name, type, x, y  + an offsets index
/tables/timeseries                name, date, time, value + offsets
/tables/patterns                  name, type, factors
/geometry/transects|streets|inlets|inlet_usage
/geometry/coordinates|vertices|polygons|symbols|labels
/controls                         rule text, one string per clause
/plugins, /process_components, /user_flags
/results/<simulation_id>/...      written by the OUTPUT plugin (§7)
```

**Ragged tables** (a curve's ordinates, a pattern's factors, a transect's
stations) use the CSR idiom: one flat value dataset plus an `offsets` dataset
of length `n+1`. It keeps everything a contiguous array and needs no
variable-length HDF5 types, which are awkward from MATLAB and slow in h5py.

**Names** are HDF5 variable-length UTF-8 strings. **Indices are written as
names, not integers** — a file that says `node1 = "J1"` survives a renumbering
and is legible; resolution happens once on read, the same way the `.inp`
parser does it.

## 4. Units: authored, not internal

**Decision: the HDF5 file stores AUTHORED (display) units throughout — the
same values the `.inp` carries, under the file's own `flow_units`.**

GeoPackage chose the opposite: internal feet/cfs everywhere, with
`xsect_geom1..4` as a single display-unit island, gated on read by
`ctx.gpkg_units_internal`. That is a defensible choice for a canonical store,
but the audit found the island is a live trap — the one exception a new
format must replicate exactly or be wrong by 3.28084 on metric models — and
found a probable ELEVATION-offset double-subtraction that the same gating
made possible.

Storing authored units instead means:

* the write path is `convert_internal_to_display` + `convert_internal_to_authored`
  on a copy, which is **the same, already-tested pass the `.inp` writer uses**
  (`InpWriter.cpp`), including the adverse-slope un-reversal, the
  `LINK_OFFSETS=ELEVATION` restoration and the FILLED_CIRCULAR sediment bump;
* the read path is the ordinary `convert_inputs_to_internal`, with no gate and
  no exception;
* there is no island, so no rule for a future maintainer to forget;
* the numbers in the file match the numbers in the `.inp` and in the GUI, which
  is what anyone opening it in h5py expects.

The cost is that the file is not the engine's internal representation. That is
the right trade: this is an interchange format, not a memory image.

Every dataset carries a `units` attribute naming the actual unit
(`"ft"`, `"m"`, `"cfs"`, `"in/hr"`, `"dimensionless"`), resolved against the
root `unit_system`.

## 5. Cross sections

The audit's §3 matrix is the specification. Two rules the format must not blur:

1. **Geom1–Geom4 are stored raw, as authored**, never as the resolver's derived
   `y_full`/`w_max`. Their meaning is shape-dependent — a length, a
   dimensionless side slope, a Manning's-n-like C-factor, or a *lookup-table
   index* — and only the raw values reconstruct the section. The engine already
   retains them for exactly this reason (`LinkData::xsect_geom1`).
2. **The ellipse/arch standard-size-code convention is preserved verbatim.**
   `ARCH 12 0 0 0` means standard arch code 12, not a 12-ft rise
   (`xsect.c:612-623`). A writer that "helpfully" substituted real dimensions
   would silently change the geometry, and on an SI model the metres would be
   re-read as a size code. The `shape` column plus the raw geoms is enough to
   round-trip this; nothing is normalised on the way out.

`xsections/shape` is written as the **legacy keyword string**
(`"RECT_TRIANGULAR"`, not an enum ordinal). Two enums in this codebase disagree
on ordering and an `enum + 1` translation between them was once the single
largest parity bug in the project; a format that stores the keyword cannot
reproduce that class of error.

The culvert code gets its own column. It was the defect that opened the audit.

## 6. Model read/write plugin

`Hdf5InputPlugin : IInputPlugin` — `read()` and `write()` delegate to
`Hdf5ModelReader` / `Hdf5ModelWriter`, exactly as `GeoPackageInputPlugin`
delegates. Registered as a built-in in `PluginFactory::register_builtin_infos()`
beside GeoPackage, so

```c
swmm_model_write_with_plugin(e, "model.h5", "org.hydrocouple.openswmm.plugins.hdf5");
```

works with **no engine change**. `file_filters()` advertises `*.h5` / `*.hdf5`
for all three roles.

Note there is no extension→plugin resolution in the engine today: nothing
consumes `file_filters()`, and `swmm_model_write(e, "model.h5")` still writes
an `.inp`. Adding an `IInputPlugin::can_write(path)` sniff mirroring
`IStateIOPlugin::can_read` would fix that for every format at once. Proposed
separately — it changes dispatch behaviour for existing callers and should not
ride in on a new format.

## 7. Results and report plugins

`Hdf5OutputPlugin : IOutputPlugin` writes `/results/<simulation_id>/` with the
same unlimited-dimension, chunked, gzip-compressed append the 2D plugin already
uses (`createUnlimitedDataset`): `/time` plus `[nTime, nObject]` datasets per
variable, per object class. Multiple runs coexist under distinct
`simulation_id` groups, the same isolation GeoPackage gets from its
`simulations` table.

`Hdf5ReportPlugin : IReportPlugin` writes `/results/<id>/summary/` — the
continuity tables, flow-stats and per-object summaries — as columnar tables
rather than formatted text, so they can be read back without parsing a `.rpt`.

A `.h5` is **never byte-identical** between runs (HDF5 embeds timestamps and
allocates freelists nondeterministically). Compare with `h5diff`, never `cmp`.

## 8. Build and linkage constraints

Non-negotiable, all already load-bearing in this tree:

* **Raw C HDF5 only** (`hdf5.h`, `hid_t`). No HighFive, no C++ bindings —
  matching `Default2DOutputPlugin`, which is the only HDF5 code here today.
* **HDF5 is PRIVATE-linked with hidden dynamic symbols**
  (`src/engine/CMakeLists.txt:352-359`). A consumer that links its own HDF5 —
  the Qt GUI does — otherwise crashes in `H5_term_library → H5I_clear_type` on
  foreign global state. New symbols go in `macos_unexported_symbols.txt`.
* HDF5 is reachable today only under `OPENSWMM_BUILD_2D`. The model plugin
  takes its own `OPENSWMM_WITH_HDF5` option, defaulting ON when 2D is on,
  following the `OPENSWMM_WITH_GEOPACKAGE` pattern, so a 2D-less build can
  still read and write `.h5` models.

## 9. Testing

| Level | Scope |
|---|---|
| Unit | table round-trip per group; ragged CSR encode/decode; string columns; schema-version refusal |
| Integration | `.inp` → `.h5` → reopen → **identical simulation results** — the end-to-end check GeoPackage currently lacks (audit §6) |
| Cross section | every shape in the audit's §3 matrix, including both ellipse/arch idioms and the culvert column |
| Units | the same model in CFS and CMS; authored values identical in both files modulo the unit factor |
| Coverage | a deck exercising each section GeoPackage drops, asserting none is lost |
| Comparison | `h5diff`, never `cmp` |
