# QA Phase 4 (relational cutover) — Validation Report

## Side-tables become the sole store; wide subtype arrays deleted from NodeData

**Date:** 2026-06-21
**Driver:** build-equipped agent (implemented + verified end-to-end with the build loop)
**Repo:** `/Users/calebbuahin/Documents/Projects/cbuahin_github/openswmm.engine`
**Branch:** `swmm6_rel`  ·  base (pre-Phase-4) = `feac4ce2`
**Build tree:** `build-arm64-osx` (Ninja, Debug, **NDEBUG undefined → asserts live**)

---

## Verdict: PASS — full cutover complete, bit-for-bit parity, memory win realized.

The dense per-subtype side-tables (`StorageData`/`OutfallData`/`DividerData` in
`NodeSubtypes.hpp`, joined to base nodes by `node_idx`) are now the **single source
of truth** for all node subtype config + mutable state. The wide `storage_*` /
`outfall_*` / `divider_*` / `exfil_*` arrays have been **removed from `NodeData`**.
The mirror machinery (`build`-from-wide / `verify_mirror` / `mark_dirty` /
`ensure_fresh`) is gone. Per the cutover plan this was executed as one branch
session with parity verified at each stage boundary; it lands as a single squashed
commit.

---

## Stage-by-stage execution + gates

### Stage A — side-tables authoritative (writers flip; mirror removed)
- `NodeSubtypes.hpp` rewritten: incremental maintenance (`set_node_type`,
  `erase_node`, `add_default`, `rebuild_index`) keeping each SoA's `node_idx`
  strictly ascending (so per-row iteration matches a base-node-ascending scan
  bit-for-bit). Removed `build`/`verify_mirror`/`mark_dirty`/`ensure_fresh`/`dirty_`/`push_from`.
- Parse writers (NodesHandler, GeoPackageReader, PostParseResolver name-resolution
  + open-time `convert_inputs_to_internal`), edit writers (C-API setters,
  TypeConverter, ObjectDeleter), and the build-from-wide call sites
  (PostParseResolver, SWMMEngine `initHydraulics`) all flipped to the side-table.
- **Audit caught readers the Phase-3 migration left on the wide arrays** (safe
  while wide was a mirror, broken once it isn't): SWMMEngine init/hotstart/
  full-volume `getVolume`, FIXED-outfall initial depth, `outfall_route_to`
  accumulation, DynamicWave flap-gate, NodeCoupling flap-gate, `buildOutfallLinkMap`,
  C-API `depth_from_volume` — all repointed.
- **2 bugs found + fixed by the build loop:** `swmm_node_add` / `swmm_node_pop_last`
  weren't maintaining the side-table (programmatic adds → setters hit the wide
  fallback). Routed both through `set_node_type`/`erase_node`.
- **Gate:** build (asserts on) clean; **ctest 86/86**; **byte parity 15/15 EXACT**
  vs `feac4ce2` (storage func/tab + seep + evap + exfil, outfall FREE/FIXED/NORMAL/
  TIDAL/TIMESERIES, divider cutoff/overflow/tabular/weir, extran2/10, test1, user4);
  C-API edit-then-get harness 0 failures.
- **Adversarial review** (6-lens workflow + verify): 23 findings, 16 verified,
  **0 confirmed bugs** (low-severity notes were cosmetic or confirmations).

### Stage B — mutable live state to the side-table
- `storage_evap_loss`/`storage_exfil_loss` (Routing loss block incl. the cross-step
  exfil hand-off, Exfiltration producer) and `outfall_2d_head` (NodeCoupling writer,
  Outfall reader) moved to `storages.evap_loss/exfil_loss` and `outfalls.head_2d`.
  `reset_state` touches only the aggregate `nodes.losses` (not these), so the
  side-table's resize-default init matches — no special per-run reset. HotStartManager
  has no references (no-op).
- **Gate:** ctest 86/86 (incl. 2D-coupling tests) + byte parity 15/15 EXACT
  (exfil harness + storage-loss models).

### Stage C — IO onto the side-table
- `convert_internal_to_display` (the save-path unit conversion on a ctx copy),
  `InpWriter`, and `GeoPackageWriter` now read subtype config from the side-table.
- **Gate:** ctest 86/86; **INP save parity 10/10 byte-identical** Phase-4-save vs
  baseline-save (storage func/tab/metric, outfall tidal/timeseries, divider ×3,
  extran2, user4); metric-model round-trip confirmed correct display values
  (`convert_internal_to_display` on the side-table). (A pre-existing OUTFALLS
  writer/parser column-order quirk is present identically in both arms — not a
  Phase-4 change.)

### Stage D — delete the wide arrays (the memory win)
- Removed all `storage_*`/`outfall_*`/`divider_*`/`exfil_*` vectors from `NodeData`
  + their `resize`/`grow_to`/`erase_at`/`shrink_to_fit` entries. The compiler then
  flagged every remaining dead fallback (140 sites across 8 engine files); each
  collapsed to the side-table read or a literal resize-default.
- 4 unit tests (`test_exfiltration`, `test_routing`, `test_gap_fixes`,
  `test_geopackage`) that built bare `NodeData`s migrated to the side-table API.
- **Gate:** clean build, **zero references** to the wide subtype arrays; **ctest
  86/86**; **final byte parity 15/15 EXACT** (full cutover vs `feac4ce2`).
- **Memory (60k-junction synthetic model, peak RSS, min-of-3 Debug runs):**
  baseline ≈ **179.6 MB** → Phase 4 ≈ **148.4 MB**, a **~31 MB (~17%) reduction**
  (~540 B/junction — the deleted wide subtype arrays). Load+run wall time unchanged
  (~1.6–1.7 s).

## Frozen-API check
The public C header `include/openswmm/engine/openswmm_nodes.h` is unchanged
(byte-for-byte); only function bodies changed. Python/GUI/MCP consumers are
unaffected.

## Artifacts (`docs/relational/qa_runs/phase_4_stage{A,B,C,D}/`)
- Stage A: build/ctest logs; A/ (Phase 4) & B/ (baseline) `.out` for 15 models;
  `edit_then_get_od` harness log.
- Stage B: build/ctest logs.
- Stage C: `save_inp` harness; `save_p4/` vs `save_base/` INP saves (10 models);
  `storage_metric.inp` + metric round-trip saves.
- Stage D: build/ctest logs; `junction_heavy.inp` (memory benchmark) + `time_*`
  RSS captures; `base/` vs `p4/` `.out` for the final 15-model parity.
- This report.
