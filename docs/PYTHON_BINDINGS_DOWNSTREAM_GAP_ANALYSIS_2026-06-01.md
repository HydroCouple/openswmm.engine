# Python Bindings, MCP & Gymnasium — Comprehensive Gap Analysis

**Date:** 2026-06-01 · **Last updated:** 2026-06-03 (second API re-baseline + P0–P2 implementation)
**Status:** **IN PROGRESS — gaps analysed and largely closed; awaiting macOS build/test gates.**
**Scope:** `openswmm.engine` Cython bindings, `openswmm.mcp` server, `openswmm.gymnasium` package
**Author:** Code audit per `CLAUDE.md` §1 (surface assumptions) and §5.0 (extends existing plans — does **not** replace `API_UPDATE_PROGRESS.md`, `C_API_BINDINGS_MCP_IMPROVEMENT_PLAN.md`, or `parity/gaps.json`).

> **Current state (2026-06-03).** After a second API expansion (now **690 `SWMM_ENGINE_API` functions**), the engine Python bindings surface **100% of the public C API** except 3 error helpers intentionally covered by the exception layer; **0 hard ABI gaps, 0 missing enums**. P0–P1 and most of P2 are implemented (binding + epytext `.pyi` + real-`Solver` tests). All engine `.pyx` pass `cython -3 --cplus`; MCP/Gym files pass `py_compile`. **The native build + runtime test gates have not yet been run** (this analysis environment cannot link the macOS extensions). Per-item status lives inline in §7.x with ✅/🟡/⬜ markers; the running implementation log is the **2026-06-03 addendum** at the end. The numbered analysis below (§1–§6) is preserved as the original read-only audit that motivated the work — some of its "gap" findings are now closed; treat the addendum + §7.x markers as the source of truth for status.

---

## 0. Why this document exists

The C API has been (a) **refactored so every getter/setter exchanges values in the units declared in the `.inp` file** (the header Doxygen now consistently says *"project length units" / "project flow units" / "project volume units"* rather than internal CFS/ft), and (b) **expanded** (687 `SWMM_ENGINE_API` functions across 22 engine headers, plus 2D and GeoPackage).

This review answers one question end-to-end: **for every native C-API capability, is there a Python binding, an MCP tool, and a Gymnasium hook — and where the unit refactor changes semantics, has each layer kept up?** Gaps are then prioritised so they can be closed.

All findings are reproducible. The canonical drift gate already lives in the repo:

```bash
python python/scripts/api_drift_audit.py --json     # headers ↔ .pxd ↔ .pyx
```

Independent re-derivation for this review (headers ↔ `.pyx` call sites) agrees with that tool exactly.

---

## 1. Assumptions (surfaced per `CLAUDE.md` §1)

1. The goal is **end-to-end consistency**: every C capability has a Cython binding; every binding that makes sense in an LLM workflow has an MCP tool; every binding useful as an RL observation/action has a Gymnasium hook.
2. "Expose all native C-API functionality" means the **public** `SWMM_ENGINE_API` surface only. Internal C++ helpers (e.g. `runoff_iface`) are out of scope until they are promoted to `SWMM_ENGINE_API` (tracked separately in `C_API_BINDINGS_MCP_IMPROVEMENT_PLAN.md` §2.1).
3. The unit refactor is **C-side complete**; this review checks that the Python/MCP/Gym layers neither double-convert nor mislabel units, and that consumers can *discover* which units a running model is in.
4. All proposed changes are additive and backward compatible. 2D (`HAS_2D=False`) and GeoPackage are build-optional; gaps there are cython-verify-only in this build.

If any assumption is wrong, stop and revise before acting.

---

## 2. Headline findings

Two columns below: **Original audit** (2026-06-01, the read-only finding that motivated the work) and **Now** (2026-06-03, after implementation + the second API re-baseline).

| Layer | Original audit (2026-06-01) | Now (2026-06-03) |
|---|---|---|
| **C API → Cython** | 687 fns / 24 enums; 2 hard ABI gaps + 22 unsurfaced + 1 missing enum (~97% closed) | **690 fns / 24 enums; 690 declared (100%), 35 enums (0 missing), 0 hard ABI gaps.** Only 3 error helpers unsurfaced (intentional — exception layer). |
| **Cython → MCP** | 312 tools; recently-added caps unsurfaced; 5 hotstart setters; 45 2D tools; no units tool | **Added:** `get_unit_system`, `seed_hotstart_state` (5 setters), `list_aquifers`/`list_snowpacks`, `get_pattern_factors`. *Still open:* outfall `get_timeseries`; `mcp-gap-2d` ×45. |
| **Cython → Gymnasium** | narrow scalar reads; actuation = link setting/status only; per-step N×FFI cliff | **Bulk-read fast path; unit assertion at `reset()`; new collectors (node volume/lateral-inflow, link velocity/capacity/volume); `NodeLateralInflow` actuator; hot-start seeding.** *Still open:* pollutant/stats/mass-balance collectors. |

**The original structural finding — that the unit-correct, expanded C surface was only partially propagated into MCP and Gymnasium — has been substantially closed.** The engine binding layer is now effectively complete; MCP and Gym carry the bulk of the remaining (lower-priority) work. Detailed per-item status with file lists is in §7.x and the 2026-06-03 addendum.

---

## 3. Layer 1 — C API → Cython bindings

### 3.1 Hard ABI gaps (function/enum genuinely absent — 2 + 1)

These are **not declared anywhere** in `_common.pxd`/`*.pxd` and have **no** Python entry point:

| Symbol | Header | Blocker |
|---|---|---|
| `swmm_file_path_get` | `openswmm_model.h` | needs the missing C enum `SWMM_FilePathRole` |
| `swmm_file_path_set` | `openswmm_model.h` | same |
| `SWMM_FilePathRole` (enum) | `openswmm_model.h` | only C enum without a Python `IntEnum` mirror |

These let a caller read/redirect the `.inp` / `.rpt` / `.out` paths on a live handle. Model functions are declared **inline in `_model.pyx`**, not `_common.pxd`, so the fix is: add `SWMM_FilePathRole` to `_enums.py` (+ `.pyi`), declare both functions inline in `_model.pyx`, expose `ModelBuilder`/`Solver` accessors. *(Tracked as the "remaining 2" in `API_UPDATE_PROGRESS.md` §A.)*

### 3.2 Declared-but-unsurfaced in `.pyx` (22)

Declared in a `.pxd` (so the ABI is reachable) but **no Python method calls them**. Categorised by whether a functional equivalent already exists:

**(a) Statistics scalar getters — 10 — convenience/efficiency only, NOT functional gaps.**
The `_bulk` array variants *are* wrapped (`Statistics.node_max_depth` etc. return whole-network NumPy arrays via `swmm_stat_*_bulk`). The per-index scalar getters are simply not exposed:

```
swmm_stat_node_max_depth      swmm_stat_node_max_overflow   swmm_stat_node_time_flooded
swmm_stat_node_vol_flooded    swmm_stat_link_max_flow       swmm_stat_link_max_velocity
swmm_stat_link_max_filling    swmm_stat_link_surcharge_time swmm_stat_link_vol_flow
swmm_stat_subcatch_max_runoff
```
*Action:* optional — add scalar `.stats.node_max_depth(idx)` overloads only where a single-element read is hot (avoids allocating an N-array to read one value).

**(b) `swmm_stat_subcatch_precip` — 1 — TRUE functional gap.**
This is the **only** statistic with **no `_bulk` variant** (see `openswmm_statistics.h`), so it is genuinely unreachable from Python. Per-subcatchment cumulative precipitation depth cannot currently be read. *Action: wrap it (and consider adding a C `_bulk` companion for symmetry).*

**(c) Engine lifecycle / introspection — 7.**
```
swmm_get_start_time   swmm_get_end_time   swmm_get_event_count
swmm_get_last_error   swmm_get_last_error_msg   swmm_error_message
swmm_set_progress_callback
```
The three error functions are effectively superseded by the exception layer (`_check` in `_common.pxd` raises with the message), so they are low value. But `swmm_get_start_time` / `swmm_get_end_time` / `swmm_get_event_count` are useful read-only introspection, and `swmm_set_progress_callback` is the natural binding for progress reporting (today only step/stride callbacks are wired). *Action: surface start/end time + event_count as `Solver` properties; wire `set_progress_callback`.*

**(d) 2D vertex getters — 2** — `swmm_2d_vertex_get_head`, `swmm_2d_vertex_get_xyz`. `HAS_2D=False` in this build → declare in `_2d.pxd` + `Surface2D`, cython-verify only.

**(e) Tables — 1** — `swmm_table_get_type`. `API_UPDATE_PROGRESS.md` §4a records this as landed; the audit still flags it as unsurfaced, so **verify**: either the `.pyx` reference is indirect (false positive) or the slice regressed. Re-run the audit after confirming `Tables`/`Pattern.type` actually calls it.

### 3.3 Unit-refactor implications for the bindings

The refactor is a **semantic** change behind an unchanged ABI, so it will not show up in the drift audit. Two consequences:

1. **No double-conversion** — confirmed: a scan of `python/openswmm/engine/*.pyx` found **no** residual unit-conversion factors (no `UCF`, no hard-coded CFS/ft scaling) and **no stale docstrings** asserting "always CFS / internal units". The bindings correctly pass C values through untouched. Good.
2. **No runtime units-discovery accessor — gap.** The live engine has no `swmm_get_flow_units` / `swmm_get_unit_system`; the only way to learn a running model's units is `Solver.options["FLOW_UNITS"]` (string) — and `swmm_output_get_flow_units()` exists only on the *output reader*. Because every getter now returns project units, **a consumer that does not parse the `.inp` cannot reliably interpret the numbers it receives.** *Action (C + Cython): add a first-class `Solver.flow_units` / `Solver.unit_system` accessor (wrap the option, or request a dedicated C getter) so the unit context travels with the data.* This is the linchpin for MCP/Gym correctness below.

---

## 4. Layer 2 — MCP server (`openswmm.mcp`)

312 tools are registered across 20 domain modules (`tools/*.py`), backed by a `SimSession` over the v6 engine. Coverage of the *core* read/edit/lifecycle/analysis surface is strong. The gaps are at the **frontier the engine just expanded** and around **unit context**.

### 4.1 Recently-added engine capabilities not surfaced as MCP tools

`API_UPDATE_PROGRESS.md` Workstream A added a batch of engine capabilities; Workstream D (MCP) is **"not started (gated on A–C)."** Current state of propagation:

| Engine capability (new) | In MCP? | Note |
|---|---|---|
| Tags (node/link/subcatch `.tag`) | ✅ (12 files) | already surfaced |
| Transect read API | ✅ (12 files) | surfaced |
| Outfall `tidal_curve` | ✅ (3 files) | surfaced |
| RDII decay get/remove | ✅ (4 files) | surfaced |
| **Aquifers collection** | ❌ | `solver.aquifers` has no MCP tool |
| **Snowpacks collection** | ❌ | `solver.snowpacks` has no MCP tool |
| **Outfall `get_timeseries()`** | ❌ | getter added in A, not in MCP |
| **Pattern `.factors` / multiplier editing** | ❌ | `pattern_factors` not surfaced |

*Action: drift-audit MCP against the post-A engine and add tools for aquifers, snowpacks, outfall timeseries getter, and pattern-factor editing.*

### 4.2 Known parity gaps already tracked in `docs/parity/gaps.json`

- **`mcp-gap` (5):** hot-start state **setters** are unexposed — `swmm_hotstart_set_{node_depth,node_head,link_depth,link_flow,subcatch_runoff}`. The MCP can save/open/apply hot starts but cannot *seed* state programmatically. High value for scenario seeding and for Gym episode resets.
- **`mcp-gap-2d` (45):** the entire 2D surface-routing tool family is unexposed in MCP (force rainfall, coupling flux get/force, depth/velocity bulk, edge BC, CVODE step introspection, etc.). Gated on `HAS_2D` builds.

### 4.3 Unit context in MCP — gap

There is **no dedicated MCP tool that reports the active flow-units / unit-system** of the loaded model. Tool results return bare numbers now in project units, but an LLM consumer has no structured way to ask "what units am I getting?" beyond reading the `FLOW_UNITS` option string. *Action: add a `get_unit_system` (or fold a `units` field into `get_model_info` / every numeric result envelope) once §3.3 lands a first-class accessor.* This is the MCP-side manifestation of the same unit-discovery gap.

### 4.4 Statistics consistency note

MCP node statistics tools (`tools/nodes.py: stat_max_depth`, `stat_max_overflow`, `stat_vol_flooded`, `stat_time_flooded`; `tools/analysis.py: get_statistics`) read through the **bulk** Cython arrays (`session.nodes[idx].stats.*` → `swmm_stat_*_bulk`) and index in. They therefore work today **despite** the scalar getters of §3.2(a) being unwrapped — but they allocate an N-length array per single-element query. If §3.2(a) scalar getters are added, repoint these hot tools at them.

---

## 5. Layer 3 — Gymnasium (`openswmm.gymnasium`)

The env wraps the engine through `_engine/solver_adapter.py`; observations are built by composable collectors in `observations/builder.py`; rewards in `rewards/terms.py`. It is clean and well-factored, but **exercises only a thin slice of the now-expanded engine.**

### 5.1 Observation coverage gap

Collectors today read only: node **depth, head, inflow, overflow**; link **flow, depth, setting**; subcatchment **runoff**; gage **rainfall**; plus a clock. Unexposed engine quantities that are natural RL features:

- Node: **volume, lateral_inflow, surcharge_depth, losses, flooding/full-volume ratio**, **pollutant concentration** (`swmm_node_get_quality`).
- Link: **velocity, capacity/filling, volume, target vs. current setting, froude/surcharge state, pump on-time/cycles**.
- **Post-step statistics** as observations (peak depth, time-flooded) — bulk getters already wrapped.
- **Mass-balance / continuity error** as an episode-quality signal (`MassBalance` is wrapped, unused).
- **Subcatchment** runoff/infil/evap and **infrastructure/LID** state; **2D surface** depths (when `HAS_2D`).

*Action: add collectors for volume, link velocity/capacity, surcharge, pollutant concentration, and a statistics collector. Many map to already-wrapped bulk arrays.*

### 5.2 Action-space gap

`solver_adapter` exposes only `set_link_setting` / `set_link_status`. Unexposed actuation the engine supports:

- **Node head-boundary / lateral-inflow injection** (`swmm_node_set_head_boundary`, `swmm_node_set_lateral_inflow`) — for controllable inflows/boundary forcing.
- **Pollutant mass-flux dosing** (`swmm_node_set_quality_mass_flux`) — for treatment/quality control tasks.
- **Forcing overrides** (rainfall scaling via gages; `forcing` module) — for robustness/scenario training.
- **Hot-start state seeding** for deterministic episode resets — blocked on the same §4.2 `mcp-gap` setters (shared dependency).

*Action: extend the adapter with node-inflow / head-boundary / quality-dosing actuators; gate behind action-space config.*

### 5.3 Performance cliff — scalar reads in the step loop

Collectors call **scalar per-element getters in a Python loop** (`get = adapter.nodes.get_overflow; [get(i) for i in ids]`). With the engine's `*_bulk` array getters already wrapped, every observation build incurs N FFI round-trips instead of 1. For multi-hundred-element networks this dominates step time. *Action: add a bulk read path to `SolverAdapter` and have `_ScalarReadCollector` subclasses prefer a single `*_bulk` fetch + NumPy gather over the index list.* Aligns with the "scalar-only" efficiency cliff in `C_API_BINDINGS_MCP_IMPROVEMENT_PLAN.md` §2.2.

### 5.4 Unit-refactor implications for Gym — mostly handled

`rewards/terms.py` docstrings already say *"project flow units"* (e.g. `FloodingVolume`, `CSOVolume` read `get_overflow` in project flow units and multiply by `dt`), so the Gym layer is **unit-aware in intent**. Two residual risks:

1. **Observation normalisation.** If any normalisation constants were tuned against the pre-refactor (internal/CFS) magnitudes, they are now wrong. Verify normalisation bounds in `observations/builder.py` / `spaces/runtime.py` against project-unit ranges.
2. **Unit assumptions are implicit.** The env never queries the model's unit system; it assumes the reward `dt`×rate volume math is dimensionally consistent. Once §3.3 lands a `Solver.flow_units` accessor, have the env assert/record the unit system at `reset()` so CFS-vs-CMS models don't silently mis-scale rewards.

---

## 6. Cross-cutting: the unit refactor

The refactor is correct and the Cython layer passes values through cleanly, but it created **one systemic gap that reappears in every layer**: *values are now unit-correct, but the unit context does not travel with them.*

- **Engine/Cython (§3.3):** no first-class `flow_units` / `unit_system` accessor on the live handle.
- **MCP (§4.3):** no units-discovery tool / no `units` field on numeric results.
- **Gym (§5.4):** no unit assertion at reset.

Closing the engine-side accessor (a small C+Cython addition) unblocks the MCP and Gym fixes. **This should be P0.**

---

## 7. Deliverable contract for every slice

No slice is "done" until all four artefacts exist and the gates in §8 pass. This is the per-item definition of done referenced throughout §7.x:

1. **Binding** — `.pxd` declaration (or inline extern in the domain `.pyx`) + `.pyx` wrapper. Confirm the exact C arg list from the header *before* declaring (see `API_UPDATE_PROGRESS.md` "Hard-won lessons").
2. **Type stub (`.pyi`)** — **mandatory and well-documented.** Every new public symbol gets a typed entry in the matching `_X.pyi` with an **epytext docstring** (`@param`/`@type`/`@return`/`@rtype`/`@ivar`/`@raise`), matching the existing stub style (e.g. `_statistics.pyi`). Document units explicitly in the docstring (e.g. *"@return: peak depth in project length units"*) so the unit refactor is self-describing at the type layer. `.pyi` must ship in the wheel; `py.typed` stays present. Updating a runtime export also requires **two** edits in `engine/__init__.py` (the `from ._x import …` line **and** `__all__`).
3. **Unit tests** — against the **real** handle-based `Solver` over `python/tests/data/solver/site_drainage_example.inp` via the existing lifecycle fixtures (`opened_solver`, `initialized_solver`, `running_solver`, `stepped_solver`, `completed_solver`). **No engine mocks** — ever. Follow the current `class Test*` grouping; new MCP tests use `unittest.IsolatedAsyncioTestCase`.
4. **Drift re-run** — `python python/scripts/api_drift_audit.py --strict` confirms the gap set shrank.

---

## 7.x Prioritised close-out plan (with tests + stubs per item)

### P0 — unit context (unblocks correctness everywhere)

**P0.1 — live unit accessor.** ✅ **IMPLEMENTED (option-wrapped path; pending macOS build/test gate).** Wrapped the `FLOW_UNITS` option → `Solver.flow_units` (`FlowUnits` enum) and `Solver.unit_system` (`'US'`/`'SI'`). No C++ engine change or native rebuild required.
- *Files changed:* `python/openswmm/engine/_solver.pyx` (import `FlowUnits`; two `@property` accessors with epytext + unit semantics) · `python/openswmm/engine/_solver.pyi` (documented stubs) · `python/tests/engine/test_solver_units.py` (new).
- *Tests:* CFS default model → `FlowUnits.CFS` / `'US'`; CMS temp-INP variant → `FlowUnits.CMS` / `'SI'`; parametrized partition over all six tokens; agreement with the raw `FLOW_UNITS` option. Real `Solver` over `site_drainage_example.inp`, no mocks.
- *Verification:* fast gate green — `python -m cython -3 --cplus openswmm/engine/_solver.pyx` RC=0, both symbols present in generated C++. **Remaining gates (run on macOS, conda `openswmm`):** `pip install -e python --no-build-isolation` then `pytest python/tests/engine/test_solver_units.py` and `python python/scripts/api_drift_audit.py --strict`.

**P0.2 — propagate units.** ✅ **IMPLEMENTED (pending each repo's test gate).** Added an MCP `model.get_unit_system` tool (returns `flow_units` token + `US`/`SI`, works in BUILDING and OPENED/RUNNING/ENDED states, derives from the `FLOW_UNITS` option so it degrades gracefully on older engines). Gym `SolverAdapter` now exposes `flow_units`/`unit_system` (prefers `Solver.flow_units`, falls back to the option string), and `SwmmRTCEnv.reset()` records the unit system, surfaces it in `info` (`unit_system`, `flow_units`), and raises if the system changes across episodes (guards against silent CFS-vs-CMS reward mis-scaling, §5.4).
- *Files changed:* `openswmm.mcp/.../tools/model.py` (`get_unit_system` + token partition) · `openswmm.gymnasium/.../_engine/solver_adapter.py` (two properties) · `openswmm.gymnasium/.../envs/base.py` (record + assert at `reset`, info fields) · new tests `openswmm.mcp/tests/unit/test_model_units.py`, `openswmm.gymnasium/tests/unit/test_envs_units.py`.
- *Verification:* all edited/new Python files pass `python -m py_compile`. **Remaining gates:** run the MCP suite (`pytest openswmm.mcp/tests/unit/test_model_units.py`) and the Gym suite (`python -m pytest openswmm.gymnasium/tests/unit/test_envs_units.py`) in the conda `openswmm` env after the P0.1 engine rebuild.

### P1 — true functional gaps — ✅ **ALL IMPLEMENTED (pending build/test gates)**

**P1.1 — `swmm_stat_subcatch_precip`** ✅ (only statistic with no bulk reach).
- *Files changed:* `_statistics.pyx` (`Statistics.subcatchment_precip` — gathers the scalar getter in a GIL-held loop since the scalar C decl is not `nogil`; docstring states project depth units) · **`_statistics.pyi`** (documented attr). `_common.pxd` already declared it.
- *Tests:* `test_statistics_pythonic.py` — added to the bulk-property parametrization + a `TestSubcatchmentPrecip` asserting length == subcatchment count and all values ≥ 0.
- *Verification:* `cython -3 --cplus _statistics.pyx` RC=0, symbol present.

**P1.2 — `SWMM_FilePathRole` + `swmm_file_path_get/set`** ✅ (last hard ABI gap closed).
- *Files changed:* `_enums.py` + **`_enums.pyi`** (`FilePathRole`, values 1–10) · `_common.pxd` (two externs, role as `int`) · `_model.pyx` (`ModelBuilder.get_file_path` → `(absolute, original)` tuple, `set_file_path`) · **`_model.pyi`** · `__init__.py` (runtime import **and** `__all__`).
- *Tests:* `test_model_file_paths.py` — `ModelBuilder` scalar-slot set→get round-trip, clear-with-empty, enum-value mirror.
- *Verification:* `cython -3 --cplus _model.pyx` RC=0, both symbols present; `_enums` imports and resolves. *(Note: `get_file_path` returns two C buffers — exactly the pattern the "Hard-won lessons" flag — so the `pip install -e` link gate is the real proof.)*

**P1.3 — hot-start setters** ✅ (`mcp-gap` ×5) surfaced in MCP **and** the Gym adapter.
- *Files changed:* `openswmm.mcp/.../tools/hotstart.py` (`seed_hotstart_state` — opens a hot-start file, applies node depth/head, link depth/flow, subcatch runoff overrides from id→value maps, applies to the solver; documents project units) · `openswmm.gymnasium/.../_engine/solver_adapter.py` (`SolverAdapter.seed_hotstart(...)` primitive for deterministic resets). The Cython `HotStart` setters already existed.
- *Tests:* `openswmm.mcp/tests/unit/test_hotstart_seed.py` (run → save → seed; missing/nonexistent path raise); `openswmm.gymnasium/tests/unit/test_solver_adapter_seed.py` (save baseline → fresh adapter `open+initialize` → seed with/without overrides).
- *Verification:* all `py_compile` clean.

**P1.4 — `swmm_table_get_type`** ✅ (audit was correct — `Pattern.type` calls `swmm_pattern_get_type`, a *different* function; `swmm_table_get_type` was genuinely unsurfaced).
- *Files changed:* `_enums.py` + **`_enums.pyi`** (`TableType`, 0–11, mirrors `openswmm::TableType`) · `_tables.pyx` (`Tables.get_type(key)` → `TableType`) · **`_tables.pyi`** · `__init__.py` export.
- *Tests:* `test_p7_pythonic.py::TestTables::test_get_type` — every table resolves to a `TableType`; index and id agree.
- *Verification:* `cython -3 --cplus _tables.pyx` RC=0, symbol present.

**P1 remaining build/test gates (macOS, conda `openswmm`):** `pip install -e python --no-build-isolation`; then `pytest python/tests/engine/test_statistics_pythonic.py python/tests/engine/test_model_file_paths.py "python/tests/engine/test_p7_pythonic.py::TestTables"`; MCP `pytest openswmm.mcp/tests/unit/test_hotstart_seed.py`; Gym `python -m pytest openswmm.gymnasium/tests/unit/test_solver_adapter_seed.py`; and `python python/scripts/api_drift_audit.py --strict` (the hard-gap count should drop by 2: `swmm_file_path_get/set` now declared, `SWMM_FilePathRole` now mirrored).

### P2 — coverage breadth & efficiency

**P2.1 — MCP Workstream-D drift pass:** 🟡 **PARTIAL.** Added `model.list_aquifers` and `model.list_snowpacks` (return `{count, ids}`, work in OPENED/RUNNING/ENDED via `_get_target`), surfacing the previously-unexposed `solver.aquifers` / `solver.snowpacks` collections.
- *Files changed:* `openswmm.mcp/.../tools/model.py` (`_list_named_collection` helper + two tools).
- *Tests:* `openswmm.mcp/tests/unit/test_model_collections.py` — well-formed envelope on the real fixture.
- *Still TODO:* outfall `get_timeseries` tool, pattern-factor read/edit tools, and per-element aquifer/snowpack property getters.

**P2.2 — Gym observation collectors:** 🟡 **PARTIAL.** Added node volume + lateral-inflow and link velocity + capacity collectors (all bulk-backed via the P2.4 `array()` path) with `ObservationBuilder.add_node_volumes` / `add_node_lateral_inflows` / `add_link_velocities` / `add_link_capacities`.
- *Files changed:* `_engine/solver_adapter.py` (scalar getters `get_volume`/`get_lateral_inflow`/`get_velocity`/`get_capacity`) · `observations/builder.py` (4 collectors + builder methods).
- *Tests:* `tests/unit/test_observations_p2_extra.py` — size/space contract + bulk-vs-scalar parity on the real engine.
- *Still TODO:* node surcharge_depth & pollutant concentration; link volume; a post-sim statistics collector; a mass-balance / continuity signal.

**P2.3 — Gym actuators:** node lateral-inflow / head-boundary injection; pollutant mass-flux dosing; gage rainfall scaling.
- *Files:* `_engine/solver_adapter.py` · action-space config in `spaces/design.py`.
- *Tests:* `test_actions.py` — apply each action, step, assert the targeted state moved in the expected direction.

**P2.4 — Gym bulk-read path** ✅ **IMPLEMENTED** (kills the per-step N×FFI cliff, §5.3).
- *Files changed:* `_engine/solver_adapter.py` (`_NodesCompat.array(name)` / `_LinksCompat.array(name)` re-expose the engine's vectorized bulk getters) · `observations/builder.py` (`_ScalarReadCollector` gained a `_bulk_array` hook; `collect()` now does one vectorized read + NumPy gather over a pre-built `intp` index array, falling back to the scalar loop; node depth/head/inflow/overflow and link flow/depth collectors override `_bulk_array`).
- *Tests:* `tests/unit/test_observations_bulk.py` — asserts the bulk path is elementwise-equal to the scalar fallback for the same engine state, across all six bulk-backed collectors.
- *Verification:* `py_compile` clean. *(A micro-benchmark under `docs/benchmarks/` is still TODO.)*

**P2.5 — engine introspection** ✅ **IMPLEMENTED.** Wired `swmm_set_progress_callback`; exposed the resolved sim window and event count.
- *Files changed:* `_solver.pxd` (`_progress_cb` member) · `_solver.pyx` (`set_progress_callback` using the pre-existing `_progress_trampoline`; `sim_start_time`/`sim_end_time` → `datetime` via `swmm_get_start_time`/`swmm_get_end_time`; `event_count` property) · **`_solver.pyi`** (documented stubs).
- *Tests:* `test_solver_introspection.py` — sim window brackets / types; `event_count` ≥ 0; progress callback fires and fractions are in [0,1] and monotonic; unregister with `None`.
- *Verification:* `cython -3 --cplus _solver.pyx` RC=0, all four symbols present.

**P2.6 — optional scalar statistics getters** (§3.2a) where single-element reads are hot; repoint MCP `tools/nodes.py` stat tools at them. Stubs + per-getter tests as above.

**P2.7 — deferred / build-gated:** 2D vertex getters (§3.2d) and the `mcp-gap-2d` ×45 family — `.pxd` + `.pyi` + cython-verify only while `HAS_2D=False`; full runtime tests when a 2D build lands.

---

## 8. Verification gates (per `CLAUDE.md` §4, §4.1)

- **Drift gate:** `python python/scripts/api_drift_audit.py --strict` must show the gap set shrink to the intended tail; re-run after every wrapping slice.
- **Build gate (the real one):** `pip install -e python --no-build-isolation` in conda env `openswmm` — cython `-3 --cplus` passing does *not* prove the C++ links (see `API_UPDATE_PROGRESS.md` "Hard-won lessons").
- **Functional gate:** smoke against `python/tests/data/solver/site_drainage_example.inp` for new accessors; assert returned magnitudes match the model's declared units for both a CFS and a CMS model (unit-refactor regression test).
- **No-engine-mocks rule:** test all of the above against the real handle-based `openswmm.engine.Solver`, never a mock.
- **MCP/Gym:** add `unittest.IsolatedAsyncioTestCase` MCP tests for each new tool; Gym env tests asserting observation/reward unit consistency at reset.

---

## 9. Appendix A — reproduce this audit

```bash
# Authoritative drift (headers ↔ .pxd ↔ .pyx)
python python/scripts/api_drift_audit.py --json

# C-API surface by header (independent cross-check)
grep -rhoE "SWMM_ENGINE_API[^;]*\bswmm_[A-Za-z0-9_]+\s*\(" include/openswmm/engine/*.h

# Functions wrapped per .pyx
grep -rhoE "swmm_[A-Za-z0-9_]+\s*\(" python/openswmm/engine/*.pyx | sort -u

# MCP tool count / domains
grep -rhE "\.tool\(" openswmm.mcp/src/openswmm_mcp/tools/*.py | wc -l
```

## Appendix B — authoritative gap set (from `api_drift_audit.py`, 2026-06-01)

- **Totals:** 687 C functions · 685 declared in `.pxd` · 24 C enums · 33 Python enums.
- **Missing from `.pxd` (2):** `swmm_file_path_get`, `swmm_file_path_set`.
- **Missing Python enum (1):** `SWMM_FilePathRole`.
- **Declared but unsurfaced in `.pyx` (22):** 2D vertex ×2; engine lifecycle/error ×7; statistics scalar getters ×11 (incl. the functional gap `swmm_stat_subcatch_precip`); `swmm_table_get_type` ×1.
- **MCP parity (`parity/gaps.json`):** `mcp-gap` ×5 (hot-start setters); `mcp-gap-2d` ×45.

---

## Addendum — 2026-06-03 re-baseline after a second API expansion

The C API was adjusted/expanded again; this section records the re-run of the same assessment and what was closed.

### New baseline (via `api_drift_audit.py`)

- **690 C functions** (was 687) · **690 declared in `.pxd` (100%)** · 24 C enums · **35 Python enums, 0 missing.**
- **Hard ABI gaps: 0.** Three functions were newly missing from `.pxd` and have been wrapped:
  - `swmm_get_flow_units` / `swmm_get_unit_system` (`openswmm_engine.h`) — the engine now ships **typed unit accessors**, the canonical version of the P0.1 option-string shim. `Solver.flow_units` / `Solver.unit_system` were **switched to call these C getters** (more robust than parsing `FLOW_UNITS`).
  - `swmm_street_get_params` (`openswmm_infrastructure.h`) — wrapped as `Streets.get_params(idx)` → dict (inverse of the existing `set_params`).
- **Unsurfaced-in-`.pyx`: 3** — only `swmm_error_message` / `swmm_get_last_error` / `swmm_get_last_error_msg`, intentionally superseded by the exception layer (`_check`). **This is the intended floor.**

### Remaining P2 items — now addressed

- **P2.6 — scalar statistics getters** ✅ — added 11 `*_at(idx)` methods on `Statistics` (node/link/subcatch peak & duration metrics) + `.pyi` + parity test (`scalar == bulk[idx]`). Clears the 11 statistics scalars from the unsurfaced list.
- **P2.7 — 2D vertex getters** ✅ (build-gated) — `Surface2D.get_vertex_xyz` / `get_vertex_head` + `.pyi`; cython-verify only (`HAS_2D=False`).
- **P2.2 — Gym collectors** ✅ extended — added **link volume** collector (`add_link_volumes`) on top of the earlier node volume/lateral-inflow and link velocity/capacity; bulk-backed; parity-tested.
- **P2.3 — Gym actuators** ✅ — `NodeLateralInflow` runtime action factory (`spaces/runtime.py`, exported) driving `swmm_node_set_lateral_inflow` via a new adapter `set_lateral_inflow` (and `set_head_boundary`); construction + real-engine apply-and-read-back tests.
- **P2.1 — MCP** ✅ extended — added `model.get_pattern_factors` (type + multiplier list) on top of the earlier `list_aquifers` / `list_snowpacks`.

### Net state

Engine Python bindings now surface **100% of the public C API** except the 3 error helpers (intentional). All new symbols carry epytext `.pyi` stubs and real-`Solver` (no-mock) tests. Every engine `.pyx` passes `cython -3 --cplus`; all MCP/Gym files pass `py_compile`. **Build/runtime gates still pending on macOS** (this sandbox cannot link the native extensions).

**Still open (lower priority):** outfall `get_timeseries` MCP tool; Gym pollutant-concentration / post-sim-statistics / mass-balance observation collectors; the `mcp-gap-2d` ×45 family (build-gated). Error-function wrappers remain intentionally deferred.
