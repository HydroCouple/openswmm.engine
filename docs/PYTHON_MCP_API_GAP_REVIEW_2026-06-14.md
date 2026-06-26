# C-API ↔ Python Bindings ↔ MCP Server — Gap Review (2026-06-14)

**Status:** FOR REVIEW — no code changes proposed yet. This document inventories
the current surface across all three layers and identifies gaps; it is intended
to be vetted before any implementation, per `CLAUDE.md` §5.0.

**Scope (confirmed with requester):** both layers (C-API → Cython bindings, and
Cython bindings → MCP tools); exhaustive per-function inventory; whole current
surface (not just the delta since the last closure).

**Builds on (does not replace):**
`docs/API_GAP_CLOSURE_PLAN_2026-06-10.md`,
`docs/PYTHON_BINDINGS_DOWNSTREAM_GAP_ANALYSIS_2026-06-01.md`.

---

## TL;DR

| Layer | Surface | Result |
|---|---|---|
| **C-API → Cython** | 703–724 `SWMM_ENGINE_API` functions | **0 unbound.** Every C symbol has a Cython reference. Independently confirmed by the repo's own `python/tests/test_api_coverage.py` (703 symbols, empty allowlist) and by this audit (724 symbols under a stricter signature-capturing regex). |
| **Cython → MCP** | 373 registered MCP tools | **94 functional gaps** — bound Python capabilities with no MCP tool. Plus 14 Pythonic-protocol members (`__len__`/`__iter__`/`__delitem__`, exposed instead via `*_count`/`*_remove` tools) and 11 lifecycle/error/memory functions that are intentionally session-internal. |

**Headline:** The Python binding layer is complete. The actionable work is
entirely at the **MCP layer**, and the gaps cluster in five areas:

1. **Inflows** (17) — runtime/edit accessors for DWF, external inflow, hydrograph
   (RTK/IA/gage), and RDII *get/remove/set-baseline/set-scale* operations.
2. **Infrastructure → transects** (14) — the entire transect station/geometry
   editing API (bank/encroachment stations, modifiers, comments, roughness read).
3. **Links** (16) — pump shutoff/startup depth, orifice open-close rate, outlet
   rating type/exponent, tags, and several bulk getters (target/control settings).
4. **Date/time utilities** (6) — encode/decode/diff helpers (low priority).
5. **GeoPackage low-level** (7), **Nodes** (7), **Subcatchments** (6),
   **Tables/patterns** (2), and scattered singletons elsewhere.

---

## Methodology & confidence

This is a **pure-text static audit** — it does not import or run the compiled
extension (consistent with `test_api_coverage.py`). Three signals were combined:

1. **C-API surface** — every `SWMM_ENGINE_API <ret> swmm_*(...)` declaration in
   `include/openswmm/engine/*.h`, captured with its full signature.
2. **Cython binding** — a C symbol is *bound* if it is referenced in any
   `python/openswmm/engine/*.{pxd,pyx}` file, and mapped to the enclosing
   `def`/`cpdef`/`property` member (the Python-visible name).
3. **MCP reach** — a binding member is *MCP-reachable* if its name appears as a
   dotted attribute (`session.links.get_barrels`) **or** as a string token
   matching a real binding member (catches `getattr` / string-dispatch wiring,
   e.g. the 2D tools' `{"depth": "get_depths"}`) anywhere in the
   `openswmm_mcp` package (tools **and** `session.py` / `backends/` —
   lifecycle and error bindings are wired there, not in `tools/`).

**Per-function MCP status** uses five buckets:

| Icon | Meaning |
|---|---|
| ✅ `direct` | A binding member calling this C symbol is invoked by MCP. |
| ◑ `capability` | Not directly reached, but a sibling C function (a `_bulk`/scalar variant, or the `statistics.h` ↔ `nodes.h` duplicate-surface alias) **is** — so the *data* is reachable. |
| ⚙ `protocol` | Bound only through a Python dunder (`__len__`/`__iter__`/`__delitem__`/`__repr__`); the capability is exposed via a dedicated `*_count`/`*_remove` tool or Pythonic iteration. |
| 🔧 `internal` | Lifecycle/error/memory function (`swmm_engine_run`, `swmm_*_free`, `swmm_get_last_error`) wired through the session manager, intentionally not a standalone tool. |
| ❌ `gap` | Bound in Python, **no** MCP path. The actionable list. |

**Known limitations (why a human should vet the ❌ list):**

- Static name-matching can over-credit when an unrelated binding member shares a
  name (mitigated: string tokens are intersected against the real binding-member
  set), and can under-credit a capability that is delivered through an *aggregate*
  tool returning a property dict (`query.get_node_info`, `analysis_*`). Several
  ❌ getters below may already be readable through an aggregate — flagged where
  likely.
- The C-API carries **parallel surfaces** by design: scalar vs `_bulk`, and
  `statistics.h` (`swmm_stat_node_max_depth`) vs `nodes.h`
  (`swmm_node_get_stat_max_depth`). The ◑ bucket exists so these don't inflate
  the gap count — but if MCP should expose the *bulk* form specifically, the ◑
  rows are real ergonomics gaps.

The audit scripts (`api_gap_audit.py`, `gap_classify.py`, `gen_report.py`) are
attached so the numbers are reproducible.

---

## Layer 1 — C-API → Cython bindings: COMPLETE

Every `SWMM_ENGINE_API` symbol is bound. The repo's drift guard
(`python/tests/test_api_coverage.py::test_every_c_symbol_is_bound_or_allowlisted`)
passes with an **empty** `KNOWN_UNBOUND` allowlist. The C-API growth since the
2026-06-10 closure — runtime forcing (forcing/inflows/subcatchments), the P10
aquifer setter, RDII IA-model pinning, STREET cross-sections, and 2D outfall
coupling (`outfall_out`) — is all bound in Cython.

> **Stub/`.pyi` note (advisory):** this audit treats *any* `.pyx`/`.pxd`
> reference as "bound". A separate check found many bound members without a
> matching `def` in the sibling `.pyi` stub. Per repo convention (every binding
> ships a typed `.pyi`), a follow-up `.pyi` completeness pass is worth scheduling,
> but it is **not** a functional gap and is out of scope for this MCP-focused
> review.

---

## Recommendations (for vetting — phased, additive only)

Ordered by user-facing value. Each phase's definition of done follows repo
policy: an MCP tool **plus** a unit test exercising it against the **real**
handle-based engine (no mocks), with fixtures in user-reviewable locations
(`openswmm.mcp/tests/unit/...`).

**P1 — Inflows runtime/edit accessors (17 fns).** Highest value: agents can
already *add* DWF/external/hydrograph/RDII but cannot *read back, remove, or
rescale* them. Add to `tools/inflows.py`: `get`/`remove`/`set_baseline`/
`set_scale` for DWF and external inflow; hydrograph `set_rtk`/`set_ia`/
`set_gage`/`remove_entry`/`remove_group`/`rename_group`/`clear_group_months`;
RDII `remove` and decay `get`/`set`/`remove`. Mirrors the existing
`inflows_add_*` / `inflows_*_count` conventions.

**P2 — Transect editing (14 fns).** The transect station geometry API is fully
bound but unreachable: `get_params`/`get_station`/`station_count`,
bank/encroachment-station get+set, `get/set_modifiers`, `get/set_comments`,
`get_roughness` (read-back of the value the existing `set_transect_roughness`
tool writes), and `clear_stations`/`remove`. Add a transect sub-namespace to
`tools/infrastructure.py`.

**P3 — Link control/pump/outlet detail (16 fns).** `pump_shutoff_depth` /
`pump_startup_depth`, `orifice_open_close_rate`, `outlet_rating_type` /
`outlet_expon`, `tag` get/set, and the bulk getters
`get_control_settings_bulk` / `get_target_settings_bulk` /
`get_ids_bulk`. Several pair naturally with the existing
`links_set_*` / `links_get_*` tools.

**P4 — Node & subcatchment edit accessors (≈13 fns).** Node `tag`,
`set_head_boundary`, outfall `get_tidal_curve` / `get_timeseries` read-back,
`get_inflow`, `get_ids_bulk`; subcatchment `tag`,
`set_outlet_subcatchment`, `get_ids_bulk`; aquifer `get_param`/`set_param`.

**P5 — Tooling/utility & low-level (≈30 fns).** Lower priority or arguably
out-of-scope: datetime encode/decode/diff helpers (6); GeoPackage low-level
`query_int`/`query_double`/`register`/`is_registered`/`last_error`/
`topology_edge_count`/`write_observed_value` (7 — may be intentionally hidden
behind the higher-level `geopackage_*` tools); pollutant DWF concentration get/
set (2); mass-balance quality evap/seep loss (2); `options_*_report_start` (2);
`forcing_link_quality` (1 — runtime link-quality forcing); `control_validate_rule`
(1 — pre-flight rule syntax check); table `get_type` (1) and `pattern_remove` (1);
2D `vertex_get_head` / `edge_get_geometry_bulk` (2).

**Explicitly NOT gaps (no action):** the 14 ⚙ protocol functions
(count/iterate/delete via Pythonic `len()` / `del` plus the existing `*_count`
tools) and the 11 🔧 internal functions (engine create/destroy/run/report,
`*_free`, `get_last_error*`, per-step runoff-interface I/O used inside `run`).

---
## Coverage summary by domain

| Domain (header) | C fns | ✅ MCP direct | ◑ via sibling | ⚙ protocol | 🔧 internal | ❌ functional gap |
|---|--:|--:|--:|--:|--:|--:|
| 2D surface (`openswmm_2d.h`) | 66 | 63 | 1 | 0 | 0 | **2** |
| Controls (`openswmm_controls.h`) | 8 | 6 | 0 | 1 | 0 | **1** |
| Date/time utils (`openswmm_datetime.h`) | 6 | 0 | 0 | 0 | 0 | **6** |
| Editing (`openswmm_edit.h`) | 16 | 14 | 0 | 0 | 2 | **0** |
| Engine/lifecycle (`openswmm_engine.h`) | 41 | 27 | 0 | 1 | 8 | **5** |
| Forcing (`openswmm_forcing.h`) | 20 | 19 | 0 | 0 | 0 | **1** |
| Rain gages (`openswmm_gages.h`) | 17 | 14 | 1 | 0 | 0 | **2** |
| GeoPackage (`openswmm_geopackage.h`) | 27 | 19 | 0 | 0 | 1 | **7** |
| Hotstart (`openswmm_hotstart.h`) | 23 | 20 | 0 | 2 | 0 | **1** |
| Inflows (`openswmm_inflows.h`) | 35 | 18 | 0 | 0 | 0 | **17** |
| Infrastructure (LID/inlet/street/transect) (`openswmm_infrastructure.h`) | 40 | 22 | 0 | 4 | 0 | **14** |
| Links (`openswmm_links.h`) | 95 | 79 | 0 | 0 | 0 | **16** |
| Mass balance (`openswmm_massbalance.h`) | 9 | 7 | 0 | 0 | 0 | **2** |
| Model/options (`openswmm_model.h`) | 42 | 40 | 0 | 0 | 0 | **2** |
| Nodes (`openswmm_nodes.h`) | 74 | 67 | 0 | 0 | 0 | **7** |
| Output (.out reader) (`openswmm_output.h`) | 31 | 31 | 0 | 0 | 0 | **0** |
| Pollutants (`openswmm_pollutants.h`) | 25 | 22 | 0 | 1 | 0 | **2** |
| Water quality (`openswmm_quality.h`) | 15 | 14 | 0 | 1 | 0 | **0** |
| Spatial/geometry (`openswmm_spatial.h`) | 18 | 16 | 0 | 0 | 0 | **2** |
| Statistics (`openswmm_statistics.h`) | 23 | 11 | 12 | 0 | 0 | **0** |
| Subcatchments (`openswmm_subcatchments.h`) | 72 | 58 | 4 | 4 | 0 | **6** |
| Tables/curves/patterns/timeseries (`openswmm_tables.h`) | 21 | 17 | 3 | 0 | 0 | **1** |
| **TOTAL** | **724** | **584** | **21** | **14** | **11** | **94** |

## Functional MCP gaps by domain


### 2D surface (`openswmm_2d.h`) — 2 gap(s)

| C symbol | Python binding member | Signature |
|---|---|---|
| `swmm_2d_edge_get_geometry_bulk` | `get_edge_geometry_bulk` | `int swmm_2d_edge_get_geometry_bulk(SWMM_Engine engine, double* length, double* nx, double* ny)` |
| `swmm_2d_vertex_get_head` | `get_vertex_head` | `int swmm_2d_vertex_get_head(SWMM_Engine engine, int idx, double* head)` |

### Controls (`openswmm_controls.h`) — 1 gap(s)

| C symbol | Python binding member | Signature |
|---|---|---|
| `swmm_control_validate_rule` | `validate_message` | `int swmm_control_validate_rule(SWMM_Engine engine, const char* rule_text, char* errbuf, int buflen, int* line_out)` |

### Date/time utils (`openswmm_datetime.h`) — 6 gap(s)

| C symbol | Python binding member | Signature |
|---|---|---|
| `swmm_datetime_add_seconds` | `add_seconds` | `int swmm_datetime_add_seconds(double value, double seconds, double* out)` |
| `swmm_datetime_decode_date` | `decode_date` | `int swmm_datetime_decode_date(double value, int* year, int* month, int* day)` |
| `swmm_datetime_decode_time` | `decode_time` | `int swmm_datetime_decode_time(double value, int* hour, int* minute, int* second)` |
| `swmm_datetime_encode_date` | `encode_date` | `int swmm_datetime_encode_date(int year, int month, int day, double* out)` |
| `swmm_datetime_encode_time` | `encode_time` | `int swmm_datetime_encode_time(int hour, int minute, int second, double* out)` |
| `swmm_datetime_time_diff` | `time_diff` | `int swmm_datetime_time_diff(double value1, double value2, long* out)` |

### Engine/lifecycle (`openswmm_engine.h`) — 5 gap(s)

| C symbol | Python binding member | Signature |
|---|---|---|
| `swmm_events_count` | `_count` | `int swmm_events_count(SWMM_Engine engine, int* count)` |
| `swmm_set_progress_callback` | `set_progress_callback` | `int swmm_set_progress_callback (SWMM_Engine engine, SWMM_ProgressCallback callback, void* user_data)` |
| `swmm_set_step_begin_callback` | `set_step_begin_callback` | `int swmm_set_step_begin_callback(SWMM_Engine engine, SWMM_StepBeginCallback callback, void* user_data)` |
| `swmm_set_step_end_callback` | `set_step_end_callback` | `int swmm_set_step_end_callback (SWMM_Engine engine, SWMM_StepEndCallback callback, void* user_data)` |
| `swmm_set_warning_callback` | `set_warning_callback` | `int swmm_set_warning_callback (SWMM_Engine engine, SWMM_WarningCallback callback, void* user_data)` |

### Forcing (`openswmm_forcing.h`) — 1 gap(s)

| C symbol | Python binding member | Signature |
|---|---|---|
| `swmm_forcing_link_quality` | `link_quality` | `int swmm_forcing_link_quality( SWMM_Engine engine, int link_idx, int pollutant_idx, double value, int mode, int persist)` |

### Rain gages (`openswmm_gages.h`) — 2 gap(s)

| C symbol | Python binding member | Signature |
|---|---|---|
| `swmm_gage_get_scale_factor` | `scale_factor` | `int swmm_gage_get_scale_factor(SWMM_Engine engine, int idx, double* factor)` |
| `swmm_gage_set_scale_factor` | `scale_factor` | `int swmm_gage_set_scale_factor(SWMM_Engine engine, int idx, double factor)` |

### GeoPackage (`openswmm_geopackage.h`) — 7 gap(s)

| C symbol | Python binding member | Signature |
|---|---|---|
| `swmm_gpkg_is_registered` | `is_registered` | `int swmm_gpkg_is_registered(void)` |
| `swmm_gpkg_last_error` | `last_error` | `const char* swmm_gpkg_last_error(SWMM_Gpkg gpkg)` |
| `swmm_gpkg_query_double` | `query_double` | `int swmm_gpkg_query_double(SWMM_Gpkg gpkg, const char* sql, double* result)` |
| `swmm_gpkg_query_int` | `query_int` | `int swmm_gpkg_query_int(SWMM_Gpkg gpkg, const char* sql)` |
| `swmm_gpkg_register` | `register` | `int swmm_gpkg_register(const char* license_key, const char* organization, const char* contact_email, const char* deployment_id)` |
| `swmm_gpkg_topology_edge_count` | `topology_edge_count` | `int swmm_gpkg_topology_edge_count(SWMM_Gpkg gpkg, const char* simulation_id)` |
| `swmm_gpkg_write_observed_value` | `write_observed_value` | `int swmm_gpkg_write_observed_value(SWMM_Gpkg gpkg, int series_id, const char* timestamp, double value, const char* quality_flag)` |

### Hotstart (`openswmm_hotstart.h`) — 1 gap(s)

| C symbol | Python binding member | Signature |
|---|---|---|
| `swmm_hotstart_get_sim_time` | `sim_datetime` | `int swmm_hotstart_get_sim_time(SWMM_HotStart hs, double* sim_time)` |

### Inflows (`openswmm_inflows.h`) — 17 gap(s)

| C symbol | Python binding member | Signature |
|---|---|---|
| `swmm_dwf_get` | `get_dwf` | `int swmm_dwf_get(SWMM_Engine engine, int entry_idx, int* node_idx, char* constituent_buf, int constituent_buflen, double* avg_v...` |
| `swmm_dwf_remove` | `remove_dwf` | `int swmm_dwf_remove(SWMM_Engine engine, int entry_idx)` |
| `swmm_dwf_set_baseline` | `set_dwf_baseline` | `int swmm_dwf_set_baseline(SWMM_Engine engine, int entry_idx, double avg_value)` |
| `swmm_ext_inflow_get` | `get_external` | `int swmm_ext_inflow_get(SWMM_Engine engine, int entry_idx, int* node_idx, char* constituent_buf, int constituent_buflen, char* ...` |
| `swmm_ext_inflow_remove` | `remove_external` | `int swmm_ext_inflow_remove(SWMM_Engine engine, int entry_idx)` |
| `swmm_ext_inflow_set_baseline` | `set_external_baseline` | `int swmm_ext_inflow_set_baseline(SWMM_Engine engine, int entry_idx, double baseline)` |
| `swmm_ext_inflow_set_scale` | `set_external_scale` | `int swmm_ext_inflow_set_scale(SWMM_Engine engine, int entry_idx, double scale)` |
| `swmm_hydrograph_clear_group_months` | `clear_hydrograph_group_months` | `int swmm_hydrograph_clear_group_months(SWMM_Engine engine, const char* uh_name)` |
| `swmm_hydrograph_group_rename` | `rename_hydrograph_group` | `int swmm_hydrograph_group_rename(SWMM_Engine engine, int idx, const char* new_id)` |
| `swmm_hydrograph_remove_entry` | `remove_hydrograph_entry` | `int swmm_hydrograph_remove_entry(SWMM_Engine engine, const char* uh_name, int month, int response)` |
| `swmm_hydrograph_remove_group` | `remove_hydrograph_group` | `int swmm_hydrograph_remove_group(SWMM_Engine engine, const char* uh_name)` |
| `swmm_hydrograph_set_gage` | `set_hydrograph_gage` | `int swmm_hydrograph_set_gage(SWMM_Engine engine, const char* uh_name, const char* gage_name)` |
| `swmm_hydrograph_set_ia` | `set_hydrograph_ia` | `int swmm_hydrograph_set_ia(SWMM_Engine engine, const char* uh_name, int month, int response, double dmax, double drecov, double...` |
| `swmm_hydrograph_set_rtk` | `set_hydrograph_rtk` | `int swmm_hydrograph_set_rtk(SWMM_Engine engine, const char* uh_name, int month, int response, double r, double t, double k)` |
| `swmm_rdii_decay_remove` | `remove_rdii_decay` | `int swmm_rdii_decay_remove(SWMM_Engine engine, const char* uh_name, int response)` |
| `swmm_rdii_decay_set` | `set_rdii_decay` | `int swmm_rdii_decay_set(SWMM_Engine engine, const char* uh_name, int response, double k_dep, double k_0, double k_T, double T_r...` |
| `swmm_rdii_remove` | `remove_rdii` | `int swmm_rdii_remove(SWMM_Engine engine, int entry_idx)` |

### Infrastructure (LID/inlet/street/transect) (`openswmm_infrastructure.h`) — 14 gap(s)

| C symbol | Python binding member | Signature |
|---|---|---|
| `swmm_street_get_params` | `get_params` | `int swmm_street_get_params(SWMM_Engine engine, int idx, double* t_crown, double* h_curb, double* sx, double* n_road, double* gu...` |
| `swmm_transect_clear_stations` | `clear_stations` | `int swmm_transect_clear_stations(SWMM_Engine engine, int idx)` |
| `swmm_transect_get_bank_stations` | `get_bank_stations` | `int swmm_transect_get_bank_stations(SWMM_Engine engine, int idx, double* x_left, double* x_right)` |
| `swmm_transect_get_comments` | `get_comments` | `int swmm_transect_get_comments(SWMM_Engine engine, int idx, char* buf, int buflen)` |
| `swmm_transect_get_encroachment_stations` | `get_encroachment_stations` | `int swmm_transect_get_encroachment_stations(SWMM_Engine engine, int idx, double* x_left, double* x_right)` |
| `swmm_transect_get_modifiers` | `get_modifiers` | `int swmm_transect_get_modifiers(SWMM_Engine engine, int idx, double* x_factor, double* y_factor, double* length_factor)` |
| `swmm_transect_get_roughness` | `get_roughness` | `int swmm_transect_get_roughness(SWMM_Engine engine, int idx, double* n_left, double* n_right, double* n_channel)` |
| `swmm_transect_get_station` | `get_station` | `int swmm_transect_get_station(SWMM_Engine engine, int idx, int station_idx, double* station, double* elevation)` |
| `swmm_transect_get_station_count` | `station_count` | `int swmm_transect_get_station_count(SWMM_Engine engine, int idx)` |
| `swmm_transect_remove` | `remove` | `int swmm_transect_remove(SWMM_Engine engine, int idx)` |
| `swmm_transect_set_bank_stations` | `set_bank_stations` | `int swmm_transect_set_bank_stations(SWMM_Engine engine, int idx, double x_left, double x_right)` |
| `swmm_transect_set_comments` | `set_comments` | `int swmm_transect_set_comments(SWMM_Engine engine, int idx, const char* text)` |
| `swmm_transect_set_encroachment_stations` | `set_encroachment_stations` | `int swmm_transect_set_encroachment_stations(SWMM_Engine engine, int idx, double x_left, double x_right)` |
| `swmm_transect_set_modifiers` | `set_modifiers` | `int swmm_transect_set_modifiers(SWMM_Engine engine, int idx, double x_factor, double y_factor, double length_factor)` |

### Links (`openswmm_links.h`) — 16 gap(s)

| C symbol | Python binding member | Signature |
|---|---|---|
| `swmm_link_get_control_settings_bulk` | `control_settings` | `int swmm_link_get_control_settings_bulk(SWMM_Engine engine, double* buf, int count)` |
| `swmm_link_get_ids_bulk` | `_ids_list` | `int swmm_link_get_ids_bulk(SWMM_Engine engine, char* buf, int stride, int count)` |
| `swmm_link_get_orifice_open_close_rate` | `open_close_rate` | `int swmm_link_get_orifice_open_close_rate(SWMM_Engine engine, int idx, double* rate)` |
| `swmm_link_get_outlet_expon` | `expon` | `int swmm_link_get_outlet_expon(SWMM_Engine engine, int idx, double* expon)` |
| `swmm_link_get_outlet_rating_type` | `rating_type` | `int swmm_link_get_outlet_rating_type(SWMM_Engine engine, int idx, int* type)` |
| `swmm_link_get_pump_shutoff_depth` | `shutoff_depth` | `int swmm_link_get_pump_shutoff_depth(SWMM_Engine engine, int idx, double* depth)` |
| `swmm_link_get_pump_startup_depth` | `startup_depth` | `int swmm_link_get_pump_startup_depth(SWMM_Engine engine, int idx, double* depth)` |
| `swmm_link_get_tag` | `tag` | `int swmm_link_get_tag(SWMM_Engine engine, int idx, char* buf, int buflen)` |
| `swmm_link_get_target_settings_bulk` | `target_settings` | `int swmm_link_get_target_settings_bulk(SWMM_Engine engine, double* buf, int count)` |
| `swmm_link_get_xsect` | `_read` | `int swmm_link_get_xsect(SWMM_Engine engine, int idx, int* shape, double* geom1, double* geom2, double* geom3, double* geom4)` |
| `swmm_link_set_orifice_open_close_rate` | `open_close_rate` | `int swmm_link_set_orifice_open_close_rate(SWMM_Engine engine, int idx, double rate)` |
| `swmm_link_set_outlet_expon` | `expon` | `int swmm_link_set_outlet_expon(SWMM_Engine engine, int idx, double expon)` |
| `swmm_link_set_outlet_rating_type` | `rating_type` | `int swmm_link_set_outlet_rating_type(SWMM_Engine engine, int idx, int type)` |
| `swmm_link_set_pump_shutoff_depth` | `shutoff_depth` | `int swmm_link_set_pump_shutoff_depth(SWMM_Engine engine, int idx, double depth)` |
| `swmm_link_set_pump_startup_depth` | `startup_depth` | `int swmm_link_set_pump_startup_depth(SWMM_Engine engine, int idx, double depth)` |
| `swmm_link_set_tag` | `tag` | `int swmm_link_set_tag(SWMM_Engine engine, int idx, const char* tag)` |

### Mass balance (`openswmm_massbalance.h`) — 2 gap(s)

| C symbol | Python binding member | Signature |
|---|---|---|
| `swmm_get_quality_evap_loss` | `quality_evap_loss` | `int swmm_get_quality_evap_loss(SWMM_Engine engine, int pollutant_idx, double* mass)` |
| `swmm_get_quality_seep_loss` | `quality_seep_loss` | `int swmm_get_quality_seep_loss(SWMM_Engine engine, int pollutant_idx, double* mass)` |

### Model/options (`openswmm_model.h`) — 2 gap(s)

| C symbol | Python binding member | Signature |
|---|---|---|
| `swmm_options_get_report_start` | `report_start_datetime` | `int swmm_options_get_report_start(SWMM_Engine engine, double* value)` |
| `swmm_options_set_report_start` | `report_start_datetime` | `int swmm_options_set_report_start(SWMM_Engine engine, double value)` |

### Nodes (`openswmm_nodes.h`) — 7 gap(s)

| C symbol | Python binding member | Signature |
|---|---|---|
| `swmm_node_get_ids_bulk` | `_ids_list` | `int swmm_node_get_ids_bulk(SWMM_Engine engine, char* buf, int stride, int count)` |
| `swmm_node_get_inflow` | `inflow` | `int swmm_node_get_inflow(SWMM_Engine engine, int idx, double* inflow)` |
| `swmm_node_get_outfall_tidal` | `get_tidal_curve` | `int swmm_node_get_outfall_tidal(SWMM_Engine engine, int idx, int* curve_idx)` |
| `swmm_node_get_outfall_timeseries` | `get_timeseries` | `int swmm_node_get_outfall_timeseries(SWMM_Engine engine, int idx, int* ts_idx)` |
| `swmm_node_get_tag` | `tag` | `int swmm_node_get_tag(SWMM_Engine engine, int idx, char* buf, int buflen)` |
| `swmm_node_set_head_boundary` | `set_head_boundary` | `int swmm_node_set_head_boundary(SWMM_Engine engine, int idx, double head)` |
| `swmm_node_set_tag` | `tag` | `int swmm_node_set_tag(SWMM_Engine engine, int idx, const char* tag)` |

### Pollutants (`openswmm_pollutants.h`) — 2 gap(s)

| C symbol | Python binding member | Signature |
|---|---|---|
| `swmm_pollutant_get_dwf_conc` | `dwf_conc` | `int swmm_pollutant_get_dwf_conc(SWMM_Engine engine, int idx, double* conc)` |
| `swmm_pollutant_set_dwf_conc` | `dwf_conc` | `int swmm_pollutant_set_dwf_conc(SWMM_Engine engine, int idx, double conc)` |

### Spatial/geometry (`openswmm_spatial.h`) — 2 gap(s)

| C symbol | Python binding member | Signature |
|---|---|---|
| `swmm_spatial_set_gage_coord` | `set_gage_coord` | `int swmm_spatial_set_gage_coord(SWMM_Engine engine, int idx, double x, double y)` |
| `swmm_spatial_set_node_coords_bulk` | `set_node_coords` | `int swmm_spatial_set_node_coords_bulk(SWMM_Engine engine, const double* x_buf, const double* y_buf, int count)` |

### Subcatchments (`openswmm_subcatchments.h`) — 6 gap(s)

| C symbol | Python binding member | Signature |
|---|---|---|
| `swmm_aquifer_get_param` | `get_param` | `int swmm_aquifer_get_param(SWMM_Engine engine, int idx, int param, double* value)` |
| `swmm_aquifer_set_param` | `set_param` | `int swmm_aquifer_set_param(SWMM_Engine engine, int idx, int param, double value)` |
| `swmm_subcatch_get_ids_bulk` | `_ids_list` | `int swmm_subcatch_get_ids_bulk(SWMM_Engine engine, char* buf, int stride, int count)` |
| `swmm_subcatch_get_tag` | `tag` | `int swmm_subcatch_get_tag(SWMM_Engine engine, int idx, char* buf, int buflen)` |
| `swmm_subcatch_set_outlet_subcatch` | `set_outlet_subcatchment` | `int swmm_subcatch_set_outlet_subcatch(SWMM_Engine engine, int idx, int sc_idx)` |
| `swmm_subcatch_set_tag` | `tag` | `int swmm_subcatch_set_tag(SWMM_Engine engine, int idx, const char* tag)` |

### Tables/curves/patterns/timeseries (`openswmm_tables.h`) — 1 gap(s)

| C symbol | Python binding member | Signature |
|---|---|---|
| `swmm_pattern_remove` | `remove` | `int swmm_pattern_remove(SWMM_Engine engine, int idx)` |

## Protocol / Pythonic-only (not individually tool-wrapped)

- `swmm_control_count` — bound via `__len__`
- `swmm_events_remove` — bound via `__delitem__`
- `swmm_hotstart_saves_count` — bound via `__len__`
- `swmm_hotstart_saves_remove` — bound via `__delitem__`
- `swmm_inlet_count` — bound via `__len__`
- `swmm_landuse_count` — bound via `__iter__, __len__`
- `swmm_lid_count` — bound via `__len__`
- `swmm_pollutant_count` — bound via `__iter__, __len__, __repr__`
- `swmm_snowpack_add` — bound via `__repr__`
- `swmm_snowpack_count` — bound via `__repr__`
- `swmm_snowpack_id` — bound via `__repr__`
- `swmm_snowpack_index` — bound via `__repr__`
- `swmm_street_count` — bound via `__len__`
- `swmm_transect_count` — bound via `__len__`

## Lifecycle / error / memory (intentionally session-internal)

- `swmm_conversion_result_free`
- `swmm_engine_create`
- `swmm_engine_run_with_callback`
- `swmm_error_message`
- `swmm_get_last_error`
- `swmm_get_last_error_msg`
- `swmm_gpkg_open`
- `swmm_impact_report_free`
- `swmm_runoff_iface_close`
- `swmm_runoff_iface_read_step`
- `swmm_runoff_iface_save_step`

## Appendix — full per-function inventory


<details>
<summary><b>2D surface</b> — <code>openswmm_2d.h</code> (66 fns, 2 gap)</summary>

| C symbol | Cython | MCP | Binding member |
|---|:--:|:--:|---|
| `swmm_2d_boundary_edge_count` | ✅ | ✅ | boundary_edge_count |
| `swmm_2d_edge_get_geometry_bulk` | ✅ | ❌ | get_edge_geometry_bulk |
| `swmm_2d_force_clear_all` | ✅ | ✅ | force_clear_all |
| `swmm_2d_force_coupling_flux` | ✅ | ✅ | force_coupling_flux |
| `swmm_2d_force_evap` | ✅ | ✅ | force_evap |
| `swmm_2d_force_evap_uniform` | ✅ | ✅ | force_evap_uniform |
| `swmm_2d_force_rainfall` | ✅ | ✅ | force_rainfall |
| `swmm_2d_force_rainfall_uniform` | ✅ | ✅ | force_rainfall_uniform |
| `swmm_2d_get_abs_tolerance` | ✅ | ✅ | abs_tolerance |
| `swmm_2d_get_continuity_error` | ✅ | ✅ | continuity_error, get_mass_balance |
| `swmm_2d_get_coupling_flux` | ✅ | ✅ | get_coupling_flux |
| `swmm_2d_get_coupling_fluxes_bulk` | ✅ | ✅ | get_coupling_fluxes |
| `swmm_2d_get_cvode_last_step` | ✅ | ✅ | cvode_last_step |
| `swmm_2d_get_cvode_steps` | ✅ | ✅ | cvode_steps |
| `swmm_2d_get_depth` | ✅ | ✅ | get_depth |
| `swmm_2d_get_depths_bulk` | ✅ | ✅ | get_depths |
| `swmm_2d_get_dry_depth` | ✅ | ✅ | dry_depth |
| `swmm_2d_get_edge_bc_cum_flux` | ✅ | ✅ | get_edge_bc_cum_flux |
| `swmm_2d_get_edge_bc_flow` | ✅ | ✅ | get_edge_bc_flow |
| `swmm_2d_get_edge_bc_head` | ✅ | ✅ | get_edge_bc_head |
| `swmm_2d_get_edge_bc_slope` | ✅ | ✅ | get_edge_bc_slope |
| `swmm_2d_get_edge_bc_type` | ✅ | ✅ | get_edge_bc_type |
| `swmm_2d_get_edge_conveyance` | ✅ | ✅ | get_edge_conveyance |
| `swmm_2d_get_edge_conveyance_bulk` | ✅ | ✅ | get_edge_conveyance_bulk |
| `swmm_2d_get_edge_flux_bulk` | ✅ | ✅ | get_edge_flux_bulk |
| `swmm_2d_get_head` | ✅ | ✅ | get_head |
| `swmm_2d_get_heads_bulk` | ✅ | ✅ | get_heads |
| `swmm_2d_get_mass_balance` | ✅ | ✅ | get_mass_balance |
| `swmm_2d_get_max_depth` | ✅ | ✅ | max_depth |
| `swmm_2d_get_net_source` | ✅ | ✅ | get_net_source |
| `swmm_2d_get_rainfall` | ✅ | ✅ | get_rainfall |
| `swmm_2d_get_rel_tolerance` | ✅ | ✅ | rel_tolerance |
| `swmm_2d_get_stat_max_continuity_err` | ✅ | ✅ | get_stat_max_continuity_err |
| `swmm_2d_get_stat_max_depths` | ✅ | ✅ | get_stat_max_depths |
| `swmm_2d_get_stat_max_velocities` | ✅ | ✅ | get_stat_max_velocities |
| `swmm_2d_get_total_exchange_flow` | ✅ | ✅ | total_exchange_flow |
| `swmm_2d_get_total_volume` | ✅ | ✅ | total_volume |
| `swmm_2d_is_active` | ✅ | ✅ | is_active |
| `swmm_2d_reset_edge_conveyance` | ✅ | ✅ | reset_edge_conveyance |
| `swmm_2d_set_abs_tolerance` | ✅ | ✅ | abs_tolerance |
| `swmm_2d_set_dry_depth` | ✅ | ✅ | dry_depth |
| `swmm_2d_set_edge_bc_flow` | ✅ | ✅ | set_edge_bc_flow |
| `swmm_2d_set_edge_bc_flow_tseries_name` | ✅ | ✅ | set_edge_bc_flow_tseries_name |
| `swmm_2d_set_edge_bc_head` | ✅ | ✅ | set_edge_bc_head |
| `swmm_2d_set_edge_bc_rating_curve_name` | ✅ | ✅ | set_edge_bc_rating_curve_name |
| `swmm_2d_set_edge_bc_slope` | ✅ | ✅ | set_edge_bc_slope |
| `swmm_2d_set_edge_bc_tseries_name` | ✅ | ✅ | set_edge_bc_tseries_name |
| `swmm_2d_set_edge_bc_type` | ✅ | ✅ | set_edge_bc_type |
| `swmm_2d_set_edge_conveyance` | ✅ | ✅ | set_edge_conveyance |
| `swmm_2d_set_rel_tolerance` | ✅ | ✅ | rel_tolerance |
| `swmm_2d_set_vertex_z` | ✅ | ✅ | set_vertex_z |
| `swmm_2d_triangle_count` | ✅ | ✅ | get_edge_conveyance_bulk, n_triangles |
| `swmm_2d_triangle_coupling_count` | ✅ | ✅ | triangle_coupling_count |
| `swmm_2d_triangle_get_area` | ✅ | ✅ | get_triangle_area |
| `swmm_2d_triangle_get_centroid` | ✅ | ✅ | get_triangle_centroid |
| `swmm_2d_triangle_get_coupled_node` | ✅ | ✅ | get_triangle_coupled_node |
| `swmm_2d_triangle_get_mannings` | ✅ | ✅ | get_triangle_mannings |
| `swmm_2d_triangle_get_neighbours` | ✅ | ✅ | get_triangle_neighbours |
| `swmm_2d_triangle_get_vertices` | ✅ | ✅ | get_triangle_vertices |
| `swmm_2d_vertex_count` | ✅ | ✅ | n_vertices |
| `swmm_2d_vertex_coupling_count` | ✅ | ✅ | vertex_coupling_count |
| `swmm_2d_vertex_get_coupled_node` | ✅ | ✅ | get_vertex_coupled_node |
| `swmm_2d_vertex_get_head` | ✅ | ❌ | get_vertex_head |
| `swmm_2d_vertex_get_heads_bulk` | ✅ | ✅ | get_vertex_heads |
| `swmm_2d_vertex_get_xyz` | ✅ | ◑ | get_vertex_xyz |
| `swmm_2d_vertex_get_xyz_bulk` | ✅ | ✅ | get_vertex_coords |

</details>

<details>
<summary><b>Controls</b> — <code>openswmm_controls.h</code> (8 fns, 1 gap)</summary>

| C symbol | Cython | MCP | Binding member |
|---|:--:|:--:|---|
| `swmm_control_add_rule` | ✅ | ✅ | append |
| `swmm_control_clear_rules` | ✅ | ✅ | clear |
| `swmm_control_count` | ✅ | ⚙ | __len__ |
| `swmm_control_get_id` | ✅ | ✅ | __getitem__ |
| `swmm_control_get_rule` | ✅ | ✅ | __getitem__ |
| `swmm_control_set_link_setting` | ✅ | ✅ | set_link_setting |
| `swmm_control_set_link_status` | ✅ | ✅ | set_link_status |
| `swmm_control_validate_rule` | ✅ | ❌ | validate_message |

</details>

<details>
<summary><b>Date/time utils</b> — <code>openswmm_datetime.h</code> (6 fns, 6 gap)</summary>

| C symbol | Cython | MCP | Binding member |
|---|:--:|:--:|---|
| `swmm_datetime_add_seconds` | ✅ | ❌ | add_seconds |
| `swmm_datetime_decode_date` | ✅ | ❌ | decode_date |
| `swmm_datetime_decode_time` | ✅ | ❌ | decode_time |
| `swmm_datetime_encode_date` | ✅ | ❌ | encode_date |
| `swmm_datetime_encode_time` | ✅ | ❌ | encode_time |
| `swmm_datetime_time_diff` | ✅ | ❌ | time_diff |

</details>

<details>
<summary><b>Editing</b> — <code>openswmm_edit.h</code> (16 fns, 0 gap)</summary>

| C symbol | Cython | MCP | Binding member |
|---|:--:|:--:|---|
| `swmm_conversion_result_free` | ✅ | 🔧 | __repr__ |
| `swmm_gage_analyze_impact` | ✅ | ✅ | analyze_gage_impact |
| `swmm_gage_delete` | ✅ | ✅ | delete_gage |
| `swmm_impact_report_free` | ✅ | 🔧 | __repr__ |
| `swmm_link_analyze_impact` | ✅ | ✅ | analyze_link_impact |
| `swmm_link_convert` | ✅ | ✅ | convert_link |
| `swmm_link_delete` | ✅ | ✅ | delete_link |
| `swmm_node_analyze_impact` | ✅ | ✅ | analyze_node_impact |
| `swmm_node_convert` | ✅ | ✅ | convert_node |
| `swmm_node_delete` | ✅ | ✅ | delete_node |
| `swmm_subcatch_analyze_impact` | ✅ | ✅ | analyze_subcatch_impact |
| `swmm_subcatch_delete` | ✅ | ✅ | delete_subcatch |
| `swmm_table_analyze_impact` | ✅ | ✅ | analyze_table_impact |
| `swmm_table_delete` | ✅ | ✅ | delete_table |
| `swmm_transect_analyze_impact` | ✅ | ✅ | analyze_transect_impact |
| `swmm_transect_delete` | ✅ | ✅ | delete_transect |

</details>

<details>
<summary><b>Engine/lifecycle</b> — <code>openswmm_engine.h</code> (41 fns, 5 gap)</summary>

| C symbol | Cython | MCP | Binding member |
|---|:--:|:--:|---|
| `swmm_engine_close` | ✅ | ✅ | close |
| `swmm_engine_create` | ✅ | 🔧 | create |
| `swmm_engine_destroy` | ✅ | ✅ | destroy |
| `swmm_engine_end` | ✅ | ✅ | end |
| `swmm_engine_get_state` | ✅ | ✅ | state |
| `swmm_engine_initialize` | ✅ | ✅ | initialize |
| `swmm_engine_open` | ✅ | ✅ | open |
| `swmm_engine_report` | ✅ | ✅ | report |
| `swmm_engine_run` | ✅ | ✅ | run, run_with_callback |
| `swmm_engine_run_with_callback` | ✅ | 🔧 | run_with_callback |
| `swmm_engine_start` | ✅ | ✅ | start |
| `swmm_engine_step` | ✅ | ✅ | step, steps, until |
| `swmm_engine_stride` | ✅ | ✅ | stride |
| `swmm_error_message` | ✅ | 🔧 | — |
| `swmm_events_add` | ✅ | ✅ | append |
| `swmm_events_clear` | ✅ | ✅ | clear |
| `swmm_events_count` | ✅ | ❌ | _count |
| `swmm_events_get` | ✅ | ✅ | __getitem__ |
| `swmm_events_remove` | ✅ | ⚙ | __delitem__ |
| `swmm_events_set` | ✅ | ✅ | __setitem__ |
| `swmm_get_current_time` | ✅ | ✅ | current_datetime |
| `swmm_get_end_time` | ✅ | ✅ | sim_end_time |
| `swmm_get_event_count` | ✅ | ✅ | event_count |
| `swmm_get_flow_units` | ✅ | ✅ | flow_units |
| `swmm_get_last_error` | ✅ | 🔧 | — |
| `swmm_get_last_error_msg` | ✅ | 🔧 | — |
| `swmm_get_routing_step` | ✅ | ✅ | routing_step |
| `swmm_get_start_time` | ✅ | ✅ | sim_start_time |
| `swmm_get_steady_state_skip` | ✅ | ✅ | steady_state_skip |
| `swmm_get_unit_system` | ✅ | ✅ | unit_system |
| `swmm_is_between_events` | ✅ | ✅ | is_between_events |
| `swmm_runoff_iface_close` | ✅ | 🔧 | close_runoff_interface |
| `swmm_runoff_iface_open_read` | ✅ | ✅ | open_runoff_interface_read |
| `swmm_runoff_iface_open_write` | ✅ | ✅ | open_runoff_interface_write |
| `swmm_runoff_iface_read_step` | ✅ | 🔧 | read_runoff_step |
| `swmm_runoff_iface_save_step` | ✅ | 🔧 | save_runoff_step |
| `swmm_set_progress_callback` | ✅ | ❌ | set_progress_callback |
| `swmm_set_steady_state_skip` | ✅ | ✅ | steady_state_skip |
| `swmm_set_step_begin_callback` | ✅ | ❌ | set_step_begin_callback |
| `swmm_set_step_end_callback` | ✅ | ❌ | set_step_end_callback |
| `swmm_set_warning_callback` | ✅ | ❌ | set_warning_callback |

</details>

<details>
<summary><b>Forcing</b> — <code>openswmm_forcing.h</code> (20 fns, 1 gap)</summary>

| C symbol | Cython | MCP | Binding member |
|---|:--:|:--:|---|
| `swmm_climate_get_dry_only` | ✅ | ✅ | get_climate_dry_only |
| `swmm_climate_get_evap_rate` | ✅ | ✅ | climate_evap_rate |
| `swmm_climate_get_temperature` | ✅ | ✅ | get_climate_temperature |
| `swmm_climate_get_wind_speed` | ✅ | ✅ | get_climate_wind_speed |
| `swmm_climate_set_dry_only` | ✅ | ✅ | climate_dry_only |
| `swmm_forcing_clear` | ✅ | ✅ | clear |
| `swmm_forcing_clear_all` | ✅ | ✅ | clear_all |
| `swmm_forcing_climate_evap` | ✅ | ✅ | climate_evap |
| `swmm_forcing_climate_temperature` | ✅ | ✅ | climate_temperature |
| `swmm_forcing_climate_wind` | ✅ | ✅ | climate_wind |
| `swmm_forcing_gage_rainfall` | ✅ | ✅ | gage_rainfall |
| `swmm_forcing_link_flow` | ✅ | ✅ | link_flow |
| `swmm_forcing_link_quality` | ✅ | ❌ | link_quality |
| `swmm_forcing_link_setting` | ✅ | ✅ | link_setting |
| `swmm_forcing_node_head_boundary` | ✅ | ✅ | node_head_boundary |
| `swmm_forcing_node_lat_inflow` | ✅ | ✅ | node_lat_inflow |
| `swmm_forcing_node_quality` | ✅ | ✅ | node_quality |
| `swmm_forcing_subcatch_evap` | ✅ | ✅ | subcatchment_evap |
| `swmm_forcing_subcatch_rainfall` | ✅ | ✅ | subcatchment_rainfall |
| `swmm_forcing_subcatch_snowfall` | ✅ | ✅ | subcatchment_snowfall |

</details>

<details>
<summary><b>Rain gages</b> — <code>openswmm_gages.h</code> (17 fns, 2 gap)</summary>

| C symbol | Cython | MCP | Binding member |
|---|:--:|:--:|---|
| `swmm_gage_add` | ✅ | ✅ | add, add_gage |
| `swmm_gage_count` | ✅ | ✅ | __iter__, __len__, gage, gage_count, rainfalls |
| `swmm_gage_get_data_source` | ✅ | ✅ | data_source |
| `swmm_gage_get_rain_type` | ✅ | ✅ | rain_type |
| `swmm_gage_get_rainfall` | ✅ | ✅ | rainfall |
| `swmm_gage_get_rainfall_bulk` | ✅ | ◑ | rainfalls |
| `swmm_gage_get_scale_factor` | ✅ | ❌ | scale_factor |
| `swmm_gage_id` | ✅ | ✅ | __init__, get_id, id |
| `swmm_gage_index` | ✅ | ✅ | __init__, add, gage, get_index |
| `swmm_gage_rename` | ✅ | ✅ | rename |
| `swmm_gage_set_data_source` | ✅ | ✅ | data_source |
| `swmm_gage_set_filename` | ✅ | ✅ | set_file |
| `swmm_gage_set_rain_interval` | ✅ | ✅ | set_rain_interval |
| `swmm_gage_set_rain_type` | ✅ | ✅ | rain_type |
| `swmm_gage_set_rainfall` | ✅ | ✅ | rainfall |
| `swmm_gage_set_scale_factor` | ✅ | ❌ | scale_factor |
| `swmm_gage_set_timeseries` | ✅ | ✅ | set_timeseries |

</details>

<details>
<summary><b>GeoPackage</b> — <code>openswmm_geopackage.h</code> (27 fns, 7 gap)</summary>

| C symbol | Cython | MCP | Binding member |
|---|:--:|:--:|---|
| `swmm_gpkg_begin` | ✅ | ✅ | begin |
| `swmm_gpkg_close` | ✅ | ✅ | __dealloc__, close |
| `swmm_gpkg_commit` | ✅ | ✅ | commit |
| `swmm_gpkg_create_observed_series` | ✅ | ✅ | create_observed_series |
| `swmm_gpkg_gage_count` | ✅ | ✅ | object_counts |
| `swmm_gpkg_is_registered` | ✅ | ❌ | is_registered |
| `swmm_gpkg_last_error` | ✅ | ❌ | last_error |
| `swmm_gpkg_link_count` | ✅ | ✅ | object_counts |
| `swmm_gpkg_node_count` | ✅ | ✅ | object_counts |
| `swmm_gpkg_observed_series_count` | ✅ | ✅ | observed_series_count |
| `swmm_gpkg_observed_value_count` | ✅ | ✅ | observed_value_count, read_observed_values |
| `swmm_gpkg_open` | ✅ | 🔧 | __cinit__ |
| `swmm_gpkg_query_double` | ✅ | ❌ | query_double |
| `swmm_gpkg_query_int` | ✅ | ❌ | query_int |
| `swmm_gpkg_read_observed_values` | ✅ | ✅ | read_observed_values |
| `swmm_gpkg_read_result_ts` | ✅ | ✅ | read_result_ts |
| `swmm_gpkg_read_summary` | ✅ | ✅ | read_summary |
| `swmm_gpkg_register` | ✅ | ❌ | register |
| `swmm_gpkg_result_ts_count` | ✅ | ✅ | read_result_ts, result_ts_count |
| `swmm_gpkg_rollback` | ✅ | ✅ | rollback |
| `swmm_gpkg_simulation_count` | ✅ | ✅ | simulation_count, simulation_ids |
| `swmm_gpkg_simulation_id` | ✅ | ✅ | simulation_ids |
| `swmm_gpkg_subcatch_count` | ✅ | ✅ | object_counts |
| `swmm_gpkg_topology_edge_count` | ✅ | ❌ | topology_edge_count |
| `swmm_gpkg_variable_count` | ✅ | ✅ | variable_count |
| `swmm_gpkg_write_observed_value` | ✅ | ❌ | write_observed_value |
| `swmm_gpkg_write_observed_values` | ✅ | ✅ | write_observed_values |

</details>

<details>
<summary><b>Hotstart</b> — <code>openswmm_hotstart.h</code> (23 fns, 1 gap)</summary>

| C symbol | Cython | MCP | Binding member |
|---|:--:|:--:|---|
| `swmm_hotstart_apply` | ✅ | ✅ | apply |
| `swmm_hotstart_close` | ✅ | ✅ | __dealloc__, close |
| `swmm_hotstart_get_crs` | ✅ | ✅ | crs |
| `swmm_hotstart_get_sim_time` | ✅ | ❌ | sim_datetime |
| `swmm_hotstart_link_count` | ✅ | ✅ | link_count |
| `swmm_hotstart_node_count` | ✅ | ✅ | node_count |
| `swmm_hotstart_open` | ✅ | ✅ | open |
| `swmm_hotstart_save` | ✅ | ✅ | save_from |
| `swmm_hotstart_saves_add` | ✅ | ✅ | append |
| `swmm_hotstart_saves_clear` | ✅ | ✅ | clear |
| `swmm_hotstart_saves_count` | ✅ | ⚙ | __len__ |
| `swmm_hotstart_saves_get_datetime` | ✅ | ✅ | __getitem__ |
| `swmm_hotstart_saves_get_path` | ✅ | ✅ | __getitem__ |
| `swmm_hotstart_saves_remove` | ✅ | ⚙ | __delitem__ |
| `swmm_hotstart_saves_set_datetime` | ✅ | ✅ | __setitem__ |
| `swmm_hotstart_saves_set_path` | ✅ | ✅ | __setitem__ |
| `swmm_hotstart_set_link_depth` | ✅ | ✅ | set_link_depth |
| `swmm_hotstart_set_link_flow` | ✅ | ✅ | set_link_flow |
| `swmm_hotstart_set_node_depth` | ✅ | ✅ | set_node_depth |
| `swmm_hotstart_set_node_head` | ✅ | ✅ | set_node_head |
| `swmm_hotstart_set_subcatch_runoff` | ✅ | ✅ | set_subcatchment_runoff |
| `swmm_hotstart_warning` | ✅ | ✅ | warnings |
| `swmm_hotstart_warning_count` | ✅ | ✅ | warnings |

</details>

<details>
<summary><b>Inflows</b> — <code>openswmm_inflows.h</code> (35 fns, 17 gap)</summary>

| C symbol | Cython | MCP | Binding member |
|---|:--:|:--:|---|
| `swmm_dwf_add` | ✅ | ✅ | add_dwf |
| `swmm_dwf_count` | ✅ | ✅ | dwf_count |
| `swmm_dwf_get` | ✅ | ❌ | get_dwf |
| `swmm_dwf_remove` | ✅ | ❌ | remove_dwf |
| `swmm_dwf_set_baseline` | ✅ | ❌ | set_dwf_baseline |
| `swmm_ext_inflow_add` | ✅ | ✅ | add_external |
| `swmm_ext_inflow_count` | ✅ | ✅ | external_count |
| `swmm_ext_inflow_get` | ✅ | ❌ | get_external |
| `swmm_ext_inflow_remove` | ✅ | ❌ | remove_external |
| `swmm_ext_inflow_set_baseline` | ✅ | ❌ | set_external_baseline |
| `swmm_ext_inflow_set_scale` | ✅ | ❌ | set_external_scale |
| `swmm_hydrograph_add` | ✅ | ✅ | add_hydrograph |
| `swmm_hydrograph_add_gage` | ✅ | ✅ | add_hydrograph_gage |
| `swmm_hydrograph_clear_group_months` | ✅ | ❌ | clear_hydrograph_group_months |
| `swmm_hydrograph_count` | ✅ | ✅ | hydrograph_count |
| `swmm_hydrograph_gage_count` | ✅ | ✅ | hydrograph_gage_count |
| `swmm_hydrograph_get` | ✅ | ✅ | get_hydrograph |
| `swmm_hydrograph_get_gage` | ✅ | ✅ | get_hydrograph_gage |
| `swmm_hydrograph_group_count` | ✅ | ✅ | hydrograph_group_count |
| `swmm_hydrograph_group_id` | ✅ | ✅ | get_hydrograph_group_id |
| `swmm_hydrograph_group_rename` | ✅ | ❌ | rename_hydrograph_group |
| `swmm_hydrograph_remove_entry` | ✅ | ❌ | remove_hydrograph_entry |
| `swmm_hydrograph_remove_group` | ✅ | ❌ | remove_hydrograph_group |
| `swmm_hydrograph_set_gage` | ✅ | ❌ | set_hydrograph_gage |
| `swmm_hydrograph_set_ia` | ✅ | ❌ | set_hydrograph_ia |
| `swmm_hydrograph_set_rtk` | ✅ | ❌ | set_hydrograph_rtk |
| `swmm_rdii_add` | ✅ | ✅ | add_rdii |
| `swmm_rdii_count` | ✅ | ✅ | rdii_count |
| `swmm_rdii_decay_add` | ✅ | ✅ | add_rdii_decay |
| `swmm_rdii_decay_count` | ✅ | ✅ | rdii_decay_count |
| `swmm_rdii_decay_get` | ✅ | ✅ | get_rdii_decay |
| `swmm_rdii_decay_remove` | ✅ | ❌ | remove_rdii_decay |
| `swmm_rdii_decay_set` | ✅ | ❌ | set_rdii_decay |
| `swmm_rdii_get` | ✅ | ✅ | get_rdii |
| `swmm_rdii_remove` | ✅ | ❌ | remove_rdii |

</details>

<details>
<summary><b>Infrastructure (LID/inlet/street/transect)</b> — <code>openswmm_infrastructure.h</code> (40 fns, 14 gap)</summary>

| C symbol | Cython | MCP | Binding member |
|---|:--:|:--:|---|
| `swmm_inlet_add` | ✅ | ✅ | add |
| `swmm_inlet_count` | ✅ | ⚙ | __len__ |
| `swmm_inlet_id` | ✅ | ✅ | get_id |
| `swmm_inlet_index` | ✅ | ✅ | get_index |
| `swmm_inlet_set_params` | ✅ | ✅ | set_params |
| `swmm_lid_add` | ✅ | ✅ | add |
| `swmm_lid_count` | ✅ | ⚙ | __len__ |
| `swmm_lid_id` | ✅ | ✅ | get_id |
| `swmm_lid_index` | ✅ | ✅ | get_index |
| `swmm_lid_set_drain` | ✅ | ✅ | set_drain |
| `swmm_lid_set_soil` | ✅ | ✅ | set_soil |
| `swmm_lid_set_storage` | ✅ | ✅ | set_storage |
| `swmm_lid_set_surface` | ✅ | ✅ | set_surface |
| `swmm_lid_usage_add` | ✅ | ✅ | usage_add |
| `swmm_street_add` | ✅ | ✅ | add |
| `swmm_street_count` | ✅ | ⚙ | __len__ |
| `swmm_street_get_params` | ✅ | ❌ | get_params |
| `swmm_street_id` | ✅ | ✅ | get_id |
| `swmm_street_index` | ✅ | ✅ | get_index |
| `swmm_street_set_params` | ✅ | ✅ | set_params |
| `swmm_transect_add` | ✅ | ✅ | add |
| `swmm_transect_add_station` | ✅ | ✅ | add_station |
| `swmm_transect_clear_stations` | ✅ | ❌ | clear_stations |
| `swmm_transect_count` | ✅ | ⚙ | __len__ |
| `swmm_transect_get_bank_stations` | ✅ | ❌ | get_bank_stations |
| `swmm_transect_get_comments` | ✅ | ❌ | get_comments |
| `swmm_transect_get_encroachment_stations` | ✅ | ❌ | get_encroachment_stations |
| `swmm_transect_get_modifiers` | ✅ | ❌ | get_modifiers |
| `swmm_transect_get_roughness` | ✅ | ❌ | get_roughness |
| `swmm_transect_get_station` | ✅ | ❌ | get_station |
| `swmm_transect_get_station_count` | ✅ | ❌ | station_count |
| `swmm_transect_id` | ✅ | ✅ | get_id |
| `swmm_transect_index` | ✅ | ✅ | get_index |
| `swmm_transect_remove` | ✅ | ❌ | remove |
| `swmm_transect_rename` | ✅ | ✅ | rename |
| `swmm_transect_set_bank_stations` | ✅ | ❌ | set_bank_stations |
| `swmm_transect_set_comments` | ✅ | ❌ | set_comments |
| `swmm_transect_set_encroachment_stations` | ✅ | ❌ | set_encroachment_stations |
| `swmm_transect_set_modifiers` | ✅ | ❌ | set_modifiers |
| `swmm_transect_set_roughness` | ✅ | ✅ | set_roughness |

</details>

<details>
<summary><b>Links</b> — <code>openswmm_links.h</code> (95 fns, 16 gap)</summary>

| C symbol | Cython | MCP | Binding member |
|---|:--:|:--:|---|
| `swmm_link_add` | ✅ | ✅ | add, add_link |
| `swmm_link_count` | ✅ | ✅ | __iter__, __len__, _ids_list, capacities, control_settings, depths, flows, hyd_powers, link_count, link_max_filling, link_max_flow, link_max_velocity, link_surcharge_time, link_vol_flow, pump_stats, qualities, target_settings, velocities, volumes |
| `swmm_link_get_barrels` | ✅ | ✅ | barrels |
| `swmm_link_get_capacities_bulk` | ✅ | ✅ | capacities |
| `swmm_link_get_capacity` | ✅ | ✅ | capacity |
| `swmm_link_get_closed` | ✅ | ✅ | closed |
| `swmm_link_get_control_setting` | ✅ | ✅ | control_setting |
| `swmm_link_get_control_settings_bulk` | ✅ | ❌ | control_settings |
| `swmm_link_get_crest_height` | ✅ | ✅ | crest_height |
| `swmm_link_get_culvert_code` | ✅ | ✅ | culvert_code |
| `swmm_link_get_depth` | ✅ | ✅ | depth |
| `swmm_link_get_depths_bulk` | ✅ | ✅ | depths |
| `swmm_link_get_discharge_coeff` | ✅ | ✅ | discharge_coeff |
| `swmm_link_get_end_contractions` | ✅ | ✅ | end_contractions |
| `swmm_link_get_flap_gate` | ✅ | ✅ | flap_gate |
| `swmm_link_get_flow` | ✅ | ✅ | flow |
| `swmm_link_get_flows_bulk` | ✅ | ✅ | flows |
| `swmm_link_get_from_node` | ✅ | ✅ | from_node |
| `swmm_link_get_hyd_power` | ✅ | ✅ | hyd_power |
| `swmm_link_get_hyd_powers_bulk` | ✅ | ✅ | hyd_powers |
| `swmm_link_get_ids_bulk` | ✅ | ❌ | _ids_list |
| `swmm_link_get_initial_flow` | ✅ | ✅ | initial_flow |
| `swmm_link_get_length` | ✅ | ✅ | length |
| `swmm_link_get_loss_coeff` | ✅ | ✅ | loss_coeff |
| `swmm_link_get_max_flow` | ✅ | ✅ | max_flow |
| `swmm_link_get_offset_dn` | ✅ | ✅ | offset_dn |
| `swmm_link_get_offset_up` | ✅ | ✅ | offset_up |
| `swmm_link_get_orifice_open_close_rate` | ✅ | ❌ | open_close_rate |
| `swmm_link_get_orifice_type` | ✅ | ✅ | type |
| `swmm_link_get_outlet_expon` | ✅ | ❌ | expon |
| `swmm_link_get_outlet_rating_type` | ✅ | ❌ | rating_type |
| `swmm_link_get_pump_curve` | ✅ | ✅ | curve |
| `swmm_link_get_pump_init_state` | ✅ | ✅ | init_state |
| `swmm_link_get_pump_shutoff_depth` | ✅ | ❌ | shutoff_depth |
| `swmm_link_get_pump_startup_depth` | ✅ | ❌ | startup_depth |
| `swmm_link_get_pump_stats_bulk` | ✅ | ✅ | pump_stats |
| `swmm_link_get_quality` | ✅ | ✅ | quality |
| `swmm_link_get_quality_bulk` | ✅ | ✅ | qualities |
| `swmm_link_get_roughness` | ✅ | ✅ | roughness |
| `swmm_link_get_seep_rate` | ✅ | ✅ | seep_rate |
| `swmm_link_get_slope` | ✅ | ✅ | slope |
| `swmm_link_get_stat_max_filling` | ✅ | ✅ | max_filling |
| `swmm_link_get_stat_max_flow` | ✅ | ✅ | max_flow |
| `swmm_link_get_stat_max_velocity` | ✅ | ✅ | max_velocity |
| `swmm_link_get_stat_pump_cycles` | ✅ | ✅ | pump_cycles |
| `swmm_link_get_stat_pump_on_time` | ✅ | ✅ | pump_on_time |
| `swmm_link_get_stat_pump_volume` | ✅ | ✅ | pump_volume |
| `swmm_link_get_stat_surcharge_time` | ✅ | ✅ | surcharge_time |
| `swmm_link_get_stat_vol_flow` | ✅ | ✅ | vol_flow |
| `swmm_link_get_tag` | ✅ | ❌ | tag |
| `swmm_link_get_target_setting` | ✅ | ✅ | target_setting |
| `swmm_link_get_target_settings_bulk` | ✅ | ❌ | target_settings |
| `swmm_link_get_to_node` | ✅ | ✅ | to_node |
| `swmm_link_get_type` | ✅ | ✅ | type |
| `swmm_link_get_velocities_bulk` | ✅ | ✅ | velocities |
| `swmm_link_get_velocity` | ✅ | ✅ | velocity |
| `swmm_link_get_volume` | ✅ | ✅ | volume |
| `swmm_link_get_volumes_bulk` | ✅ | ✅ | volumes |
| `swmm_link_get_weir_type` | ✅ | ✅ | type |
| `swmm_link_get_xsect` | ✅ | ❌ | _read |
| `swmm_link_id` | ✅ | ✅ | __init__, get_id, id |
| `swmm_link_index` | ✅ | ✅ | __init__, add, get_index |
| `swmm_link_pop_last` | ✅ | ✅ | pop_last, pop_last_link |
| `swmm_link_rename` | ✅ | ✅ | rename |
| `swmm_link_set_barrels` | ✅ | ✅ | barrels |
| `swmm_link_set_closed` | ✅ | ✅ | closed |
| `swmm_link_set_control_setting` | ✅ | ✅ | control_setting |
| `swmm_link_set_crest_height` | ✅ | ✅ | crest_height |
| `swmm_link_set_culvert_code` | ✅ | ✅ | culvert_code |
| `swmm_link_set_discharge_coeff` | ✅ | ✅ | discharge_coeff |
| `swmm_link_set_end_contractions` | ✅ | ✅ | end_contractions |
| `swmm_link_set_flap_gate` | ✅ | ✅ | flap_gate |
| `swmm_link_set_flow` | ✅ | ✅ | flow |
| `swmm_link_set_flows_bulk` | ✅ | ✅ | flows |
| `swmm_link_set_initial_flow` | ✅ | ✅ | initial_flow |
| `swmm_link_set_length` | ✅ | ✅ | length, set_link_length |
| `swmm_link_set_loss_coeff` | ✅ | ✅ | loss_coeff |
| `swmm_link_set_max_flow` | ✅ | ✅ | max_flow |
| `swmm_link_set_nodes` | ✅ | ✅ | set_link_nodes, set_nodes |
| `swmm_link_set_offset_dn` | ✅ | ✅ | offset_dn |
| `swmm_link_set_offset_up` | ✅ | ✅ | offset_up |
| `swmm_link_set_orifice_open_close_rate` | ✅ | ❌ | open_close_rate |
| `swmm_link_set_orifice_type` | ✅ | ✅ | type |
| `swmm_link_set_outlet_expon` | ✅ | ❌ | expon |
| `swmm_link_set_outlet_rating_type` | ✅ | ❌ | rating_type |
| `swmm_link_set_pump_curve` | ✅ | ✅ | curve |
| `swmm_link_set_pump_init_state` | ✅ | ✅ | init_state |
| `swmm_link_set_pump_shutoff_depth` | ✅ | ❌ | shutoff_depth |
| `swmm_link_set_pump_startup_depth` | ✅ | ❌ | startup_depth |
| `swmm_link_set_roughness` | ✅ | ✅ | roughness, set_link_roughness |
| `swmm_link_set_seep_rate` | ✅ | ✅ | seep_rate |
| `swmm_link_set_tag` | ✅ | ❌ | tag |
| `swmm_link_set_target_setting` | ✅ | ✅ | target_setting |
| `swmm_link_set_weir_type` | ✅ | ✅ | type |
| `swmm_link_set_xsect` | ✅ | ✅ | __repr__, set_link_xsect, xsect |

</details>

<details>
<summary><b>Mass balance</b> — <code>openswmm_massbalance.h</code> (9 fns, 2 gap)</summary>

| C symbol | Cython | MCP | Binding member |
|---|:--:|:--:|---|
| `swmm_get_max_courant` | ✅ | ✅ | max_courant |
| `swmm_get_quality_continuity_error` | ✅ | ✅ | quality_continuity_error |
| `swmm_get_quality_evap_loss` | ✅ | ❌ | quality_evap_loss |
| `swmm_get_quality_seep_loss` | ✅ | ❌ | quality_seep_loss |
| `swmm_get_routing_continuity_error` | ✅ | ✅ | routing_continuity_error |
| `swmm_get_routing_stats` | ✅ | ✅ | routing_diagnostics |
| `swmm_get_routing_total` | ✅ | ✅ | routing_total |
| `swmm_get_runoff_continuity_error` | ✅ | ✅ | runoff_continuity_error |
| `swmm_get_runoff_total` | ✅ | ✅ | runoff_total |

</details>

<details>
<summary><b>Model/options</b> — <code>openswmm_model.h</code> (42 fns, 2 gap)</summary>

| C symbol | Cython | MCP | Binding member |
|---|:--:|:--:|---|
| `swmm_engine_new` | ✅ | ✅ | __init__ |
| `swmm_file_path_get` | ✅ | ✅ | get_file_path |
| `swmm_file_path_set` | ✅ | ✅ | set_file_path |
| `swmm_files_get` | ✅ | ✅ | files_get |
| `swmm_files_set` | ✅ | ✅ | files_set |
| `swmm_finalize_model` | ✅ | ✅ | finalize |
| `swmm_get_crs` | ✅ | ✅ | crs, get_crs |
| `swmm_model_write` | ✅ | ✅ | write |
| `swmm_model_write_with_plugin` | ✅ | ✅ | write_with_plugin |
| `swmm_options_get` | ✅ | ✅ | _options_get, get_option |
| `swmm_options_get_end_date` | ✅ | ✅ | end_datetime |
| `swmm_options_get_ext` | ✅ | ✅ | _options_ext_get, get_option_ext |
| `swmm_options_get_report_start` | ✅ | ❌ | report_start_datetime |
| `swmm_options_get_start_date` | ✅ | ✅ | start_datetime |
| `swmm_options_set` | ✅ | ✅ | _options_set, set_option |
| `swmm_options_set_end_date` | ✅ | ✅ | end_datetime |
| `swmm_options_set_ext` | ✅ | ✅ | _options_ext_set, set_option_ext |
| `swmm_options_set_report_start` | ✅ | ❌ | report_start_datetime |
| `swmm_options_set_start_date` | ✅ | ✅ | start_datetime |
| `swmm_plugin_get` | ✅ | ✅ | plugin_get |
| `swmm_plugin_remove` | ✅ | ✅ | plugin_remove |
| `swmm_plugin_set` | ✅ | ✅ | plugin_set |
| `swmm_plugins_count` | ✅ | ✅ | plugins_count |
| `swmm_title_add_line` | ✅ | ✅ | add_title_line |
| `swmm_title_clear` | ✅ | ✅ | clear_title |
| `swmm_title_get_count` | ✅ | ✅ | get_title_count |
| `swmm_title_get_line` | ✅ | ✅ | get_title_line |
| `swmm_title_set` | ✅ | ✅ | set_title |
| `swmm_userflag_def_count` | ✅ | ✅ | __len__, definitions, userflag_def_count |
| `swmm_userflag_def_get` | ✅ | ✅ | definitions, get_userflag_def |
| `swmm_userflag_define` | ✅ | ✅ | __setitem__, define, define_userflag |
| `swmm_userflag_get_bool` | ✅ | ✅ | __getitem__, get_userflag_bool |
| `swmm_userflag_get_int` | ✅ | ✅ | __getitem__, get_userflag_int |
| `swmm_userflag_get_real` | ✅ | ✅ | __getitem__, get_userflag_real |
| `swmm_userflag_set_bool` | ✅ | ✅ | __setitem__, set_userflag_bool |
| `swmm_userflag_set_int` | ✅ | ✅ | __setitem__, set_userflag_int |
| `swmm_userflag_set_real` | ✅ | ✅ | __setitem__, set_userflag_real |
| `swmm_userflag_undefine` | ✅ | ✅ | __delitem__, _options_ext_set, undefine, undefine_userflag |
| `swmm_userflag_value_clear` | ✅ | ✅ | clear_userflag_value, clear_value |
| `swmm_userflag_value_get` | ✅ | ✅ | __getitem__, get_userflag_value, get_value |
| `swmm_userflag_value_set` | ✅ | ✅ | __setitem__, set_userflag_value, set_value |
| `swmm_validate_model` | ✅ | ✅ | validate |

</details>

<details>
<summary><b>Nodes</b> — <code>openswmm_nodes.h</code> (74 fns, 7 gap)</summary>

| C symbol | Cython | MCP | Binding member |
|---|:--:|:--:|---|
| `swmm_node_add` | ✅ | ✅ | add, add_node |
| `swmm_node_count` | ✅ | ✅ | __iter__, __len__, _ids_list, depths, heads, inflows, lateral_inflows, losses, node_coords, node_count, node_max_depth, node_max_overflow, node_time_flooded, node_vol_flooded, outflows, overflows, qualities, set_lateral_inflows, set_node_coords, set_nodes, set_outlet_node, volumes |
| `swmm_node_get_crown_elev` | ✅ | ✅ | crown_elev |
| `swmm_node_get_degree` | ✅ | ✅ | degree |
| `swmm_node_get_depth` | ✅ | ✅ | depth |
| `swmm_node_get_depth_from_volume` | ✅ | ✅ | depth_from_volume |
| `swmm_node_get_depths_bulk` | ✅ | ✅ | depths |
| `swmm_node_get_divider_type` | ✅ | ✅ | type |
| `swmm_node_get_exfil_params` | ✅ | ✅ | exfil_params |
| `swmm_node_get_full_volume` | ✅ | ✅ | full_volume |
| `swmm_node_get_head` | ✅ | ✅ | head |
| `swmm_node_get_heads_bulk` | ✅ | ✅ | heads |
| `swmm_node_get_ids_bulk` | ✅ | ❌ | _ids_list |
| `swmm_node_get_inflow` | ✅ | ❌ | inflow |
| `swmm_node_get_inflows_bulk` | ✅ | ✅ | inflows |
| `swmm_node_get_initial_depth` | ✅ | ✅ | initial_depth |
| `swmm_node_get_invert_elev` | ✅ | ✅ | invert_elev |
| `swmm_node_get_lateral_inflow` | ✅ | ✅ | lateral_inflow |
| `swmm_node_get_lateral_inflows_bulk` | ✅ | ✅ | lateral_inflows |
| `swmm_node_get_losses` | ✅ | ✅ | losses |
| `swmm_node_get_losses_bulk` | ✅ | ✅ | losses |
| `swmm_node_get_max_depth` | ✅ | ✅ | max_depth |
| `swmm_node_get_outfall_flap_gate` | ✅ | ✅ | flap_gate |
| `swmm_node_get_outfall_param` | ✅ | ✅ | param |
| `swmm_node_get_outfall_route_to` | ✅ | ✅ | route_to |
| `swmm_node_get_outfall_tidal` | ✅ | ❌ | get_tidal_curve |
| `swmm_node_get_outfall_timeseries` | ✅ | ❌ | get_timeseries |
| `swmm_node_get_outfall_type` | ✅ | ✅ | type |
| `swmm_node_get_outflow` | ✅ | ✅ | outflow |
| `swmm_node_get_outflows_bulk` | ✅ | ✅ | outflows |
| `swmm_node_get_overflow` | ✅ | ✅ | overflow |
| `swmm_node_get_overflows_bulk` | ✅ | ✅ | overflows |
| `swmm_node_get_ponded_area` | ✅ | ✅ | ponded_area |
| `swmm_node_get_quality` | ✅ | ✅ | quality |
| `swmm_node_get_quality_bulk` | ✅ | ✅ | qualities |
| `swmm_node_get_stat_max_depth` | ✅ | ✅ | max_depth |
| `swmm_node_get_stat_max_overflow` | ✅ | ✅ | max_overflow |
| `swmm_node_get_stat_time_flooded` | ✅ | ✅ | time_flooded |
| `swmm_node_get_stat_vol_flooded` | ✅ | ✅ | vol_flooded |
| `swmm_node_get_storage_curve` | ✅ | ✅ | curve |
| `swmm_node_get_storage_functional` | ✅ | ✅ | functional |
| `swmm_node_get_storage_seep_rate` | ✅ | ✅ | seep_rate |
| `swmm_node_get_surcharge_depth` | ✅ | ✅ | surcharge_depth |
| `swmm_node_get_tag` | ✅ | ❌ | tag |
| `swmm_node_get_type` | ✅ | ✅ | type |
| `swmm_node_get_volume` | ✅ | ✅ | volume |
| `swmm_node_get_volumes_bulk` | ✅ | ✅ | volumes |
| `swmm_node_id` | ✅ | ✅ | __init__, get_id, id |
| `swmm_node_index` | ✅ | ✅ | __init__, add, get_index, set_nodes, set_outlet_node |
| `swmm_node_pop_last` | ✅ | ✅ | pop_last, pop_last_node |
| `swmm_node_rename` | ✅ | ✅ | rename |
| `swmm_node_set_depth` | ✅ | ✅ | depth |
| `swmm_node_set_depths_bulk` | ✅ | ✅ | depths |
| `swmm_node_set_divider_type` | ✅ | ✅ | type |
| `swmm_node_set_exfil_params` | ✅ | ✅ | exfil_params |
| `swmm_node_set_head_boundary` | ✅ | ❌ | set_head_boundary |
| `swmm_node_set_initial_depth` | ✅ | ✅ | initial_depth |
| `swmm_node_set_invert_elev` | ✅ | ✅ | invert_elev, set_node_invert |
| `swmm_node_set_lat_inflows_bulk` | ✅ | ✅ | set_lateral_inflows |
| `swmm_node_set_lateral_inflow` | ✅ | ✅ | lateral_inflow |
| `swmm_node_set_max_depth` | ✅ | ✅ | max_depth, set_node_max_depth |
| `swmm_node_set_outfall_flap_gate` | ✅ | ✅ | flap_gate |
| `swmm_node_set_outfall_route_to` | ✅ | ✅ | route_to |
| `swmm_node_set_outfall_stage` | ✅ | ✅ | set_stage |
| `swmm_node_set_outfall_tidal` | ✅ | ✅ | set_tidal_curve |
| `swmm_node_set_outfall_timeseries` | ✅ | ✅ | set_timeseries |
| `swmm_node_set_outfall_type` | ✅ | ✅ | type |
| `swmm_node_set_pond_area` | ✅ | ✅ | ponded_area |
| `swmm_node_set_quality_mass_flux` | ✅ | ✅ | set_quality_mass_flux |
| `swmm_node_set_storage_curve` | ✅ | ✅ | curve |
| `swmm_node_set_storage_functional` | ✅ | ✅ | functional |
| `swmm_node_set_storage_seep_rate` | ✅ | ✅ | seep_rate |
| `swmm_node_set_surcharge_depth` | ✅ | ✅ | surcharge_depth |
| `swmm_node_set_tag` | ✅ | ❌ | tag |

</details>

<details>
<summary><b>Output (.out reader)</b> — <code>openswmm_output.h</code> (31 fns, 0 gap)</summary>

| C symbol | Cython | MCP | Binding member |
|---|:--:|:--:|---|
| `swmm_output_close` | ✅ | ✅ | __dealloc__, close |
| `swmm_output_get_error_code` | ✅ | ✅ | error_code |
| `swmm_output_get_flow_units` | ✅ | ✅ | flow_units |
| `swmm_output_get_link_attribute` | ✅ | ✅ | link_attributes |
| `swmm_output_get_link_count` | ✅ | ✅ | link_count, link_result, period_times, subcatchment_ids |
| `swmm_output_get_link_id` | ✅ | ✅ | subcatchment_ids |
| `swmm_output_get_link_result` | ✅ | ✅ | link_result |
| `swmm_output_get_link_series` | ✅ | ✅ | link_series |
| `swmm_output_get_node_attribute` | ✅ | ✅ | node_attributes |
| `swmm_output_get_node_count` | ✅ | ✅ | node_count, node_result, period_times, subcatchment_ids |
| `swmm_output_get_node_id` | ✅ | ✅ | subcatchment_ids |
| `swmm_output_get_node_result` | ✅ | ✅ | node_result |
| `swmm_output_get_node_series` | ✅ | ✅ | node_series |
| `swmm_output_get_node_stat_max_depth` | ✅ | ✅ | max_depth |
| `swmm_output_get_node_stat_max_overflow` | ✅ | ✅ | max_overflow |
| `swmm_output_get_node_stat_time_flooded` | ✅ | ✅ | time_flooded |
| `swmm_output_get_node_stat_vol_flooded` | ✅ | ✅ | vol_flooded |
| `swmm_output_get_period_count` | ✅ | ✅ | link_series, node_series, period_count, period_times, subcatchment_series, system_series |
| `swmm_output_get_period_time` | ✅ | ✅ | period_times |
| `swmm_output_get_pollut_count` | ✅ | ✅ | pollutant_count |
| `swmm_output_get_report_step` | ✅ | ✅ | report_step |
| `swmm_output_get_start_date` | ✅ | ✅ | start_datetime |
| `swmm_output_get_subcatch_attribute` | ✅ | ✅ | subcatchment_attributes |
| `swmm_output_get_subcatch_count` | ✅ | ✅ | period_times, subcatchment_count, subcatchment_ids, subcatchment_result |
| `swmm_output_get_subcatch_id` | ✅ | ✅ | subcatchment_ids |
| `swmm_output_get_subcatch_result` | ✅ | ✅ | subcatchment_result |
| `swmm_output_get_subcatch_series` | ✅ | ✅ | subcatchment_series |
| `swmm_output_get_system_result` | ✅ | ✅ | system_result |
| `swmm_output_get_system_series` | ✅ | ✅ | system_series |
| `swmm_output_get_version` | ✅ | ✅ | version |
| `swmm_output_open` | ✅ | ✅ | __init__ |

</details>

<details>
<summary><b>Pollutants</b> — <code>openswmm_pollutants.h</code> (25 fns, 2 gap)</summary>

| C symbol | Cython | MCP | Binding member |
|---|:--:|:--:|---|
| `swmm_link_set_quality` | ✅ | ✅ | set_link_quality |
| `swmm_node_set_quality` | ✅ | ✅ | set_node_quality |
| `swmm_pollutant_add` | ✅ | ✅ | add |
| `swmm_pollutant_count` | ✅ | ⚙ | __iter__, __len__, __repr__ |
| `swmm_pollutant_get_co_pollutant` | ✅ | ✅ | co_pollutant |
| `swmm_pollutant_get_dwf_conc` | ✅ | ❌ | dwf_conc |
| `swmm_pollutant_get_gw_conc` | ✅ | ✅ | gw_conc |
| `swmm_pollutant_get_init_conc` | ✅ | ✅ | init_conc |
| `swmm_pollutant_get_kdecay` | ✅ | ✅ | kdecay |
| `swmm_pollutant_get_mwt` | ✅ | ✅ | mwt |
| `swmm_pollutant_get_rain_conc` | ✅ | ✅ | rain_conc |
| `swmm_pollutant_get_rdii_conc` | ✅ | ✅ | rdii_conc |
| `swmm_pollutant_get_snow_only` | ✅ | ✅ | snow_only |
| `swmm_pollutant_get_units` | ✅ | ✅ | units |
| `swmm_pollutant_id` | ✅ | ✅ | __init__, get_id, id |
| `swmm_pollutant_index` | ✅ | ✅ | __repr__, add, get_index |
| `swmm_pollutant_set_co_pollutant` | ✅ | ✅ | set_co_pollutant |
| `swmm_pollutant_set_dwf_conc` | ✅ | ❌ | dwf_conc |
| `swmm_pollutant_set_gw_conc` | ✅ | ✅ | gw_conc |
| `swmm_pollutant_set_init_conc` | ✅ | ✅ | init_conc |
| `swmm_pollutant_set_kdecay` | ✅ | ✅ | kdecay |
| `swmm_pollutant_set_mwt` | ✅ | ✅ | mwt |
| `swmm_pollutant_set_rain_conc` | ✅ | ✅ | rain_conc |
| `swmm_pollutant_set_rdii_conc` | ✅ | ✅ | rdii_conc |
| `swmm_pollutant_set_snow_only` | ✅ | ✅ | snow_only |

</details>

<details>
<summary><b>Water quality</b> — <code>openswmm_quality.h</code> (15 fns, 0 gap)</summary>

| C symbol | Cython | MCP | Binding member |
|---|:--:|:--:|---|
| `swmm_buildup_get` | ✅ | ✅ | get_buildup |
| `swmm_buildup_set` | ✅ | ✅ | set_buildup |
| `swmm_landuse_add` | ✅ | ✅ | add |
| `swmm_landuse_count` | ✅ | ⚙ | __iter__, __len__ |
| `swmm_landuse_get_sweep_interval` | ✅ | ✅ | sweep_interval |
| `swmm_landuse_get_sweep_removal` | ✅ | ✅ | sweep_removal |
| `swmm_landuse_id` | ✅ | ✅ | __iter__, get_id, id |
| `swmm_landuse_index` | ✅ | ✅ | add, get_index |
| `swmm_landuse_set_sweep_interval` | ✅ | ✅ | sweep_interval |
| `swmm_landuse_set_sweep_removal` | ✅ | ✅ | sweep_removal |
| `swmm_treatment_clear` | ✅ | ✅ | clear_treatment |
| `swmm_treatment_get` | ✅ | ✅ | get_treatment |
| `swmm_treatment_set` | ✅ | ✅ | set_treatment |
| `swmm_washoff_get` | ✅ | ✅ | get_washoff |
| `swmm_washoff_set` | ✅ | ✅ | set_washoff |

</details>

<details>
<summary><b>Spatial/geometry</b> — <code>openswmm_spatial.h</code> (18 fns, 2 gap)</summary>

| C symbol | Cython | MCP | Binding member |
|---|:--:|:--:|---|
| `swmm_spatial_get_crs` | ✅ | ✅ | crs |
| `swmm_spatial_get_gage_coord` | ✅ | ✅ | gage_coord |
| `swmm_spatial_get_link_coord` | ✅ | ✅ | link_coord |
| `swmm_spatial_get_link_vertex_count` | ✅ | ✅ | link_vertices |
| `swmm_spatial_get_link_vertices` | ✅ | ✅ | link_vertices |
| `swmm_spatial_get_node_coord` | ✅ | ✅ | node_coord |
| `swmm_spatial_get_node_coords_bulk` | ✅ | ✅ | node_coords |
| `swmm_spatial_get_subcatch_coord` | ✅ | ✅ | subcatchment_coord |
| `swmm_spatial_get_subcatch_polygon` | ✅ | ✅ | subcatchment_polygon |
| `swmm_spatial_get_subcatch_polygon_count` | ✅ | ✅ | subcatchment_polygon |
| `swmm_spatial_set_crs` | ✅ | ✅ | crs |
| `swmm_spatial_set_gage_coord` | ✅ | ❌ | set_gage_coord |
| `swmm_spatial_set_link_coord` | ✅ | ✅ | set_link_coord |
| `swmm_spatial_set_link_vertices` | ✅ | ✅ | set_link_vertices |
| `swmm_spatial_set_node_coord` | ✅ | ✅ | set_node_coord |
| `swmm_spatial_set_node_coords_bulk` | ✅ | ❌ | set_node_coords |
| `swmm_spatial_set_subcatch_coord` | ✅ | ✅ | set_subcatchment_coord |
| `swmm_spatial_set_subcatch_polygon` | ✅ | ✅ | set_subcatchment_polygon |

</details>

<details>
<summary><b>Statistics</b> — <code>openswmm_statistics.h</code> (23 fns, 0 gap)</summary>

| C symbol | Cython | MCP | Binding member |
|---|:--:|:--:|---|
| `swmm_stat_link_max_filling` | ✅ | ◑ | link_max_filling_at |
| `swmm_stat_link_max_filling_bulk` | ✅ | ✅ | link_max_filling |
| `swmm_stat_link_max_flow` | ✅ | ◑ | link_max_flow_at |
| `swmm_stat_link_max_flow_bulk` | ✅ | ✅ | link_max_flow |
| `swmm_stat_link_max_velocity` | ✅ | ◑ | link_max_velocity_at |
| `swmm_stat_link_max_velocity_bulk` | ✅ | ✅ | link_max_velocity |
| `swmm_stat_link_surcharge_time` | ✅ | ◑ | link_surcharge_time_at |
| `swmm_stat_link_surcharge_time_bulk` | ✅ | ✅ | link_surcharge_time |
| `swmm_stat_link_vol_flow` | ✅ | ◑ | link_vol_flow_at |
| `swmm_stat_link_vol_flow_bulk` | ✅ | ✅ | link_vol_flow |
| `swmm_stat_node_max_depth` | ✅ | ◑ | node_max_depth_at |
| `swmm_stat_node_max_depth_bulk` | ✅ | ✅ | node_max_depth |
| `swmm_stat_node_max_overflow` | ✅ | ◑ | node_max_overflow_at |
| `swmm_stat_node_max_overflow_bulk` | ✅ | ✅ | node_max_overflow |
| `swmm_stat_node_time_flooded` | ✅ | ◑ | node_time_flooded_at |
| `swmm_stat_node_time_flooded_bulk` | ✅ | ✅ | node_time_flooded |
| `swmm_stat_node_vol_flooded` | ✅ | ◑ | node_vol_flooded_at |
| `swmm_stat_node_vol_flooded_bulk` | ✅ | ✅ | node_vol_flooded |
| `swmm_stat_subcatch_max_runoff` | ✅ | ◑ | subcatchment_max_runoff_at |
| `swmm_stat_subcatch_max_runoff_bulk` | ✅ | ✅ | subcatchment_max_runoff |
| `swmm_stat_subcatch_precip` | ✅ | ◑ | subcatchment_precip |
| `swmm_stat_subcatch_runoff_vol` | ✅ | ◑ | subcatchment_runoff_vol_at |
| `swmm_stat_subcatch_runoff_vol_bulk` | ✅ | ✅ | subcatchment_runoff_vol |

</details>

<details>
<summary><b>Subcatchments</b> — <code>openswmm_subcatchments.h</code> (72 fns, 6 gap)</summary>

| C symbol | Cython | MCP | Binding member |
|---|:--:|:--:|---|
| `swmm_aquifer_add` | ✅ | ✅ | add |
| `swmm_aquifer_count` | ✅ | ✅ | add |
| `swmm_aquifer_get_param` | ✅ | ❌ | get_param |
| `swmm_aquifer_id` | ✅ | ✅ | add |
| `swmm_aquifer_index` | ✅ | ✅ | add |
| `swmm_aquifer_set_param` | ✅ | ❌ | set_param |
| `swmm_snowpack_add` | ✅ | ⚙ | __repr__ |
| `swmm_snowpack_count` | ✅ | ⚙ | __repr__ |
| `swmm_snowpack_id` | ✅ | ⚙ | __repr__ |
| `swmm_snowpack_index` | ✅ | ⚙ | __repr__ |
| `swmm_subcatch_add` | ✅ | ✅ | add, add_subcatchment |
| `swmm_subcatch_count` | ✅ | ✅ | __iter__, __len__, _ids_list, evaps, infils, qualities, rainfalls, runoffs, snow_depths, subcatch_count, subcatchment_max_runoff, subcatchment_precip, subcatchment_runoff_vol |
| `swmm_subcatch_get_area` | ✅ | ✅ | area |
| `swmm_subcatch_get_coverage` | ✅ | ✅ | __getitem__ |
| `swmm_subcatch_get_ds_imperv` | ✅ | ✅ | ds_imperv |
| `swmm_subcatch_get_ds_perv` | ✅ | ✅ | ds_perv |
| `swmm_subcatch_get_evap` | ✅ | ✅ | evap |
| `swmm_subcatch_get_evap_bulk` | ✅ | ◑ | evaps |
| `swmm_subcatch_get_gage` | ✅ | ✅ | gage |
| `swmm_subcatch_get_groundwater` | ✅ | ✅ | groundwater |
| `swmm_subcatch_get_gw_state` | ✅ | ✅ | get_gw_state |
| `swmm_subcatch_get_ids_bulk` | ✅ | ❌ | _ids_list |
| `swmm_subcatch_get_imperv_pct` | ✅ | ✅ | imperv_pct |
| `swmm_subcatch_get_infil` | ✅ | ✅ | infil |
| `swmm_subcatch_get_infil_bulk` | ✅ | ◑ | infils |
| `swmm_subcatch_get_infil_curve_number` | ✅ | ✅ | curve_number |
| `swmm_subcatch_get_infil_green_ampt` | ✅ | ✅ | green_ampt |
| `swmm_subcatch_get_infil_horton` | ✅ | ✅ | horton |
| `swmm_subcatch_get_infil_model` | ✅ | ✅ | model |
| `swmm_subcatch_get_n_imperv` | ✅ | ✅ | n_imperv |
| `swmm_subcatch_get_n_perv` | ✅ | ✅ | n_perv |
| `swmm_subcatch_get_outlet` | ✅ | ✅ | outlet |
| `swmm_subcatch_get_outlet_subcatch` | ✅ | ✅ | outlet |
| `swmm_subcatch_get_ponded_quality` | ✅ | ✅ | ponded_quality |
| `swmm_subcatch_get_quality` | ✅ | ✅ | quality |
| `swmm_subcatch_get_quality_bulk` | ✅ | ✅ | qualities |
| `swmm_subcatch_get_rainfall` | ✅ | ✅ | rainfall |
| `swmm_subcatch_get_rainfall_bulk` | ✅ | ◑ | rainfalls |
| `swmm_subcatch_get_runoff` | ✅ | ✅ | runoff |
| `swmm_subcatch_get_runoff_bulk` | ✅ | ✅ | runoffs |
| `swmm_subcatch_get_slope` | ✅ | ✅ | slope |
| `swmm_subcatch_get_snow_depth` | ✅ | ✅ | snow_depth |
| `swmm_subcatch_get_snow_depth_bulk` | ✅ | ◑ | snow_depths |
| `swmm_subcatch_get_snow_state` | ✅ | ✅ | get_snow_state |
| `swmm_subcatch_get_stat_max_runoff` | ✅ | ✅ | max_runoff |
| `swmm_subcatch_get_stat_precip` | ✅ | ✅ | precip |
| `swmm_subcatch_get_stat_runoff_vol` | ✅ | ✅ | runoff_vol |
| `swmm_subcatch_get_tag` | ✅ | ❌ | tag |
| `swmm_subcatch_get_width` | ✅ | ✅ | width |
| `swmm_subcatch_id` | ✅ | ✅ | __init__, get_id, id |
| `swmm_subcatch_index` | ✅ | ✅ | __init__, add, get_index |
| `swmm_subcatch_rename` | ✅ | ✅ | rename |
| `swmm_subcatch_set_area` | ✅ | ✅ | area |
| `swmm_subcatch_set_coverage` | ✅ | ✅ | __setitem__ |
| `swmm_subcatch_set_ds_imperv` | ✅ | ✅ | ds_imperv |
| `swmm_subcatch_set_ds_perv` | ✅ | ✅ | ds_perv |
| `swmm_subcatch_set_gage` | ✅ | ✅ | gage |
| `swmm_subcatch_set_gw_state` | ✅ | ✅ | set_gw_state |
| `swmm_subcatch_set_imperv_pct` | ✅ | ✅ | imperv_pct |
| `swmm_subcatch_set_infil_curve_number` | ✅ | ✅ | set_curve_number |
| `swmm_subcatch_set_infil_green_ampt` | ✅ | ✅ | set_green_ampt |
| `swmm_subcatch_set_infil_horton` | ✅ | ✅ | set_horton |
| `swmm_subcatch_set_n_imperv` | ✅ | ✅ | n_imperv |
| `swmm_subcatch_set_n_perv` | ✅ | ✅ | n_perv |
| `swmm_subcatch_set_outlet` | ✅ | ✅ | set_outlet_node |
| `swmm_subcatch_set_outlet_subcatch` | ✅ | ❌ | set_outlet_subcatchment |
| `swmm_subcatch_set_ponded_quality` | ✅ | ✅ | set_ponded_quality |
| `swmm_subcatch_set_rainfall` | ✅ | ✅ | rainfall |
| `swmm_subcatch_set_slope` | ✅ | ✅ | slope |
| `swmm_subcatch_set_snow_state` | ✅ | ✅ | set_snow_state |
| `swmm_subcatch_set_tag` | ✅ | ❌ | tag |
| `swmm_subcatch_set_width` | ✅ | ✅ | width |

</details>

<details>
<summary><b>Tables/curves/patterns/timeseries</b> — <code>openswmm_tables.h</code> (21 fns, 1 gap)</summary>

| C symbol | Cython | MCP | Binding member |
|---|:--:|:--:|---|
| `swmm_curve_add` | ✅ | ✅ | add_curve |
| `swmm_pattern_add` | ✅ | ✅ | add |
| `swmm_pattern_count` | ✅ | ✅ | __iter__, __len__, add |
| `swmm_pattern_get_factor` | ✅ | ✅ | factors |
| `swmm_pattern_get_factor_count` | ✅ | ✅ | factors |
| `swmm_pattern_get_type` | ✅ | ✅ | type |
| `swmm_pattern_id` | ✅ | ✅ | id |
| `swmm_pattern_index` | ✅ | ✅ | get_index |
| `swmm_pattern_remove` | ✅ | ❌ | remove |
| `swmm_pattern_rename` | ✅ | ✅ | rename |
| `swmm_pattern_set_factors` | ✅ | ✅ | set_factors |
| `swmm_table_add_point` | ✅ | ✅ | add_point |
| `swmm_table_clear` | ✅ | ✅ | clear |
| `swmm_table_count` | ✅ | ◑ | __iter__, __len__, table_count |
| `swmm_table_get_point` | ✅ | ◑ | _raw_points |
| `swmm_table_get_point_count` | ✅ | ◑ | __len__, _raw_points |
| `swmm_table_get_type` | ✅ | ✅ | get_type |
| `swmm_table_id` | ✅ | ✅ | __init__, get_id, id |
| `swmm_table_index` | ✅ | ✅ | __init__, add_curve, add_timeseries, get_index |
| `swmm_table_lookup` | ✅ | ✅ | lookup |
| `swmm_timeseries_add` | ✅ | ✅ | add_timeseries |

</details>

---

## Reproducing this audit

```
python3 api_gap_audit.py   # builds api_gap_rows.{json,csv}; prints C/Cython/MCP counts
python3 gap_classify.py     # categorises gap candidates, verifies against MCP source
python3 gen_report.py        # emits the tables in this document
```

All three scripts are pure-text and run without the compiled extension. The
per-symbol CSV (`api_gap_rows.csv`) is the machine-readable companion to the
appendix below.
