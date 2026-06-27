# QA Phase 3 (Exfiltration storage-geometry) — Validation Report
## ExfilSolver::init storage geometry reads → side-table

**Date:** 2026-06-20
**Validator:** automated QA agent (build/run/measure only; no production source modified)
**Repo:** `/Users/calebbuahin/Documents/Projects/cbuahin_github/openswmm.engine`
**Branch:** `swmm6_rel`  ·  stacks on `9e371421` (3 cold storage config) and the full relational chain
**Build tree:** `build-arm64-osx` (Ninja, Debug, **NDEBUG undefined → asserts live**)

---

## Verdict: PASS — EXACT parity on both storage shapes (functional + tabular). Gate is met.

This is the **final Phase-3 read-migration increment**. `ExfilSolver::init` now reads storage
**geometry** from the dense `StorageData` side-table (via the `sr = storage_row(i)` already computed
in the cold-config increment), with wide-array fallback. After this, **every** `nodes.storage_*` /
`nodes.exfil_*` *read* in `ExfilSolver::init` is side-table-backed; the only remaining wide reference
is the mutable `storage_exfil_loss` **write** (a per-step output, deferred to the Phase-4 writer-move).

---

## 1. The change (1 file, source audit — PASS)
`src/engine/hydraulics/Exfiltration.cpp`, `ExfilSolver::init` (runs after `node_subtypes.build()`):
- **TABULAR:** `curve_idx` now `(sr>=0) ? storages.curve[sr] : nodes.storage_curve[ui]` (@~94).
- **FUNCTIONAL:** `a_coeff/b_coeff/c_coeff` now `(sr>=0) ? storages.{a,b,c}[sr] : nodes.storage_{a,b,c}[ui]` (@~139–143).

`sr` is the same row index used for the cold-config reads in 3-cold; reused, not recomputed. Geometry
is static during a run and the side-table is an exact mirror (Phase 1 `verify_mirror`), so the read
is bit-for-bit. The fallback covers the unbuilt/non-storage path (e.g. `test_exfiltration`, which
never builds the side-table). No simulation behavior changes. Scope matches the handoff's "1 file"
exactly — the worktree's `SWMMEngine.cpp` (2D output sign-flip) and `Node.cpp` (lazy-geometry-fetch
reorder) are **other streams**, deliberately left out of this commit.

## 2. Build (PASS — assertions-enabled)
- Incremental Debug build: **exit 0.** No-op rebuild confirms up-to-date (exit 0).
- **No new warnings from `Exfiltration.cpp`.** The 2 warnings in the log are pre-existing and not from
  this diff: `TableData.hpp:652 missing 'cursor' initializer` (a header pulled in via `Node.hpp`,
  untouched) and `ld: ignoring duplicate libraries '-ldl' '-lm'` (linker). Logs: `qa_build_phase3_exfilgeom*.log`.

## 3. ctest (PASS — 86/86)
- `ctest -j8`: **100% passed, 86/86, 0 failed, no aborts**, no fixture race. `verify_mirror` holds.
  Log: `qa_ctest_phase3_exfilgeom.log`.
- As noted in the handoff, `test_exfiltration` validates the GA benchmark but only exercises the
  **fallback** (never builds `node_subtypes`) — which is why the side-table geometry path is covered
  by the harness in §5.

## 4. Simulation parity: clean A/B isolation (PASS — EXACT)
Toggled **only** `Exfiltration.cpp` (`git stash push -- <that file>` → pre-exfilgeom = HEAD `9e371421`),
rebuilt, recompiled the harness against the rebuilt engine, re-ran identical models, byte-compared.

| Case | Storage shape | Geometry read path | `.out` exfilgeom vs pre-exfilgeom |
|------|---------------|--------------------|-----------------------------------|
| `func_exfil` (storage_kinwave_seep.inp) | **FUNCTIONAL** | `storages.a/b/c` | **EXACT** |
| `tab_exfil` (storage_tabular_exfil.inp, authored) | **TABULAR** | `storages.curve` | **EXACT** |

## 5. Exfiltration geometry side-table path (harness — the handoff's key focus)
Exfiltration is not configurable via `.inp` (no parser writes `exfil_*`), and ctest only hits the
fallback. `exfilgeom_run.cpp` generalizes the cold-config harness to take the model path as an arg,
so the SAME driver exercises **both** geometry branches: it sets exfil (`suction=6.0, ksat=4.32,
imd=0.2`) + seep on `STOR1` via the C-API, then drives a **full simulation** (open → set → initialize
→ start → step → close). This makes the run path `build()` the side-table and `ExfilSolver::init`
read the geometry from it.
- **Exfil is consequential** (not a no-op test): tabular `tab_exfil.out` (exfil set) **differs** from
  a no-exfil CLI run of the same model (`tab_noexfil.out`) → the exfil geometry path is genuinely hit.
- **Both shapes** are then byte-identical exfilgeom-vs-baseline → the geometry side-table reads are
  bit-for-bit. Output: `A/run.log`, `B/run.log`.

## Gate
EXACT parity (tabular + functional exfil) → **met**. This closes the Phase-3 **read** migration:
all compute + C-API + Exfiltration reads of storage/outfall/divider config & geometry are now
side-table-backed (wide-array fallback). Remaining before the wide arrays can be dropped is all
**writer-moves + IO**, bundled into the Phase-4 atomic cutover: mutable loss outputs
(`storage_evap_loss`/`storage_exfil_loss`), `outfall_2d_head` writer move, the INP writer, and
GeoPackage reads (Stage C), then deletion of the wide arrays (Stage D).

## Artifacts (`docs/relational/qa_runs/phase_3_exfilgeom/`)
- `exfilgeom_run.cpp` + compiled `exfilgeom_run` — generalized exfil geometry A/B harness.
- `storage_tabular_exfil.inp` (authored, TABULAR + `[CURVES]`), `storage_kinwave_seep.inp` (copied, FUNCTIONAL).
- `A/` (exfilgeom) & `B/` (pre-exfilgeom) `.out` for both shapes; `A/tab_noexfil.out` (exfil-consequential sanity).
- `qa_build_phase3_exfilgeom*.log`, `qa_ctest_phase3_exfilgeom.log`, `qa_harness_compile.log`.
- `QA_PHASE3_exfilgeom_VALIDATION_REPORT.md` — this report.
