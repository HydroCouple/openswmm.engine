# Verification Instructions: Step-Cell Water Surface Fix (Wet-Masked Vertex Reconstruction)

**Audience:** an agent with build capability on this machine (macOS arm64) for both
`openswmm.engine` and `openswmm.gui`.
**Companion docs:** `plans/2d/STEP_CELL_PARTIAL_WET_RENDERING_FIX_PLAN.md` (design/root cause).
**Your job:** compile both repos, run the listed tests, fix any compile/test issues *within the
stated design constraints*, and report status. The implementation was written without a compiler
available — expect possibly a few mechanical issues (missing include, signature mismatch,
CMake test registration), not design problems. If something looks like a design problem, STOP
and flag it instead of redesigning.

---

## 1. What was changed and why (context you need)

Symptom: 2D surface rendering + mesh profiles showed water climbing an adverse slope/bed step
with no driving head. Root causes: (a) the GUI rendered the engine's **solver** vertex-head field
(`vert_head` / `Mesh2_node_head`), whose pseudo-Laplacian stencil blends **dry-cell bed
elevations** into shoreline vertices; (b) the `max(0, head − z)` conversion destroyed the signed
sub-cell shoreline signal; (c) the flat closure `η = z_c + h̄` overstates η on partially wet
(step-spanning) cells.

Fix architecture (do not violate these constraints when fixing issues):

- **`state.vert_head` semantics are untouched** — it is a solver field (gradients, limiters,
  fluxes, active-set seed pass rely on dry head = bed). All existing solver behavior must remain
  bit-identical.
- A **new, render-only** field `state.vert_depth_signed` (η_v − z_v) is computed by
  `reconstructVertexRenderDepths()`: wet-only (depth ≥ dry_depth), depth-weighted average of
  incident cell free surfaces, with each cell's η from the new planar-bed stage–storage
  inversion `cellFreeSurfaceElevation()`. Vertices with no wet incident cell get **0**;
  negative values are meaningful (sub-cell shoreline) and must never be clamped anywhere.
- Exported three ways: HDF5 dataset `Mesh2_node_depth`, snapshot `surface_vert_depth`,
  C API `swmm_2d_vertex_get_render_depths_bulk`. `Mesh2_node_head` and the heads APIs stay
  for back-compat but the GUI no longer consumes them.

## 2. Files changed

**openswmm.engine**

| File | Change |
|---|---|
| `src/engine/2d/mesh/VertexReconstruction.hpp/.cpp` | + `cellFreeSurfaceElevation()`, + `reconstructVertexRenderDepths()` |
| `src/engine/2d/data/SurfaceStateData.hpp` | + `vert_depth_signed` member + resize init |
| `src/engine/2d/SurfaceRouter2D.cpp` | calls new reconstruction at end of `initialize()` and after `computeFaceVelocity` in the advance-window path |
| `src/engine/core/SWMMEngine.cpp` | snapshot copy `snap.surface_vert_depth = st.vert_depth_signed` (~line 3714) |
| `include/openswmm/plugin_sdk/SimulationSnapshot.hpp` | + `surface_vert_depth` |
| `src/engine/2d/output/Default2DOutputPlugin.hpp/.cpp` | + `Mesh2_node_depth` dataset (create/update/close) |
| `include/openswmm/engine/openswmm_2d.h`, `src/engine/2d/api/Api2D.cpp` | + `swmm_2d_vertex_get_render_depths_bulk` |
| `tests/unit/engine/test_2d_surface_routing.cpp` | + `CellFreeSurface.*` and `VertexRenderReconstruction.*` tests, + 2 dataset-existence asserts |

**openswmm.gui**

| File | Change |
|---|---|
| `include/io/mesh2dh5reader.h`, `src/io/mesh2dh5reader.cpp` | + `readVertexSignedDepthsAt()` (`Mesh2_node_depth`, probe-once cache `cached_has_node_depth_`, reset in `open()`) |
| `include/layers/swmm2dresultslayer.h` | `readVertexDepthsAt` doc → SIGNED semantics; `pushVertexHeads` → `pushVertexSignedDepths`; `HDF5Mesh2DSource` scratch members `head_buf_`/`node_z_cache_` → `depth_buf_` (std::vector<float>) |
| `src/layers/swmm2dresultslayer.cpp` | `HDF5Mesh2DSource::readVertexDepthsAt` reads the signed dataset only (legacy `Mesh2_node_head` deliberately ignored → fallback); `EngineMesh2DSource::pushVertexSignedDepths` stores signed floats unclamped; `applyCurrentDepths_` keeps negatives (only non-finite → 0); fallback `reconstructVertexSignedDepths` uses new local `cellEtaFromMeanDepth()` (mirror of engine closure) with flat-closure fallback for bad vertex indices/nodata z |
| `include/simulation/simulationrunner.h`, `src/simulation/simulationrunner.cpp` | signal `twoDVertexHeadsAvailable` → `twoDVertexDepthsAvailable`; calls `swmm_2d_vertex_get_render_depths_bulk` |
| `src/swmmvis.cpp` | connect block rewired to the new signal → `pushVertexSignedDepths` (~line 6830) |
| `tests/gui/test_mesh2dh5reader.cpp` | fixture writes `Mesh2_node_depth` (incl. negative values); 3 new tests asserting unclamped read / absent-dataset fallback / range check |

## 3. Build & test — engine (FIRST; the GUI links against it)

```bash
cd ~/Documents/Projects/cbuahin_github/openswmm.engine
# Use the existing configured build if present (build/ or build-arm64-osx/, CMakePresets.json).
# Typical: cmake --preset <the one already configured> ; cmake --build --preset ... -j
cmake --build build --target <unit-test target> -j   # discover target via tests/unit/engine/CMakeLists.txt
ctest --test-dir build -R 2d_surface_routing --output-on-failure
```

1. **Compile.** Likely trivial fixes if any: missing include, `hsize_t` vs `size_t` warnings in
   `Default2DOutputPlugin.cpp`, unused-parameter warnings. Match existing file style.
2. **New tests must pass:** `CellFreeSurface.*` (4) and `VertexRenderReconstruction.*` (4).
   The math was validated numerically against quadrature (inversion round-trip < 1e-8), so a
   failure here most likely means a mechanical port issue — compare with the reference
   implementation in `VertexReconstruction.cpp` before touching tolerances. Do NOT loosen a
   tolerance beyond 1e-6 without flagging it.
3. **All pre-existing 2D tests must pass unchanged** (`MeshBuilder.*`, `VertexReconstruction.*`,
   `GradientComputation.*`, `EdgeFlux.*`, output-plugin test, coupling tests…). If any
   pre-existing test fails, the change violated the "solver untouched" constraint — investigate,
   don't adjust the old test. Exception: the output-plugin dataset test was intentionally
   extended with `Mesh2_node_head`/`Mesh2_node_depth` existence asserts.
4. **Solver invariance spot-check (important):** run the full engine test suite, plus (if fast)
   `examples/2d_complete_example.inp` before/after and diff every HDF5 dataset EXCEPT the new
   `Mesh2_node_depth` — all face/edge/head fields must be byte-identical. `h5diff` is your friend.
5. Sanity-check no double-registration: `reconstructVertexRenderDepths` is called in
   `SurfaceRouter2D::initialize()` (end) and in the advance-window path after
   `computeFaceVelocity`. Both are host-side, per-window — confirm no call sits inside a CVODE
   RHS/Jacobian callback.
6. **Not done, decide/flag:** Python bindings (`python/openswmm/engine/_2d.pyx/.pxd`) do not wrap
   the new bulk API. Add the wrapper only if the bindings build is part of CI; otherwise note it.
7. If the engine has an export/symbol list (`src/engine/macos_unexported_symbols.txt` or similar),
   confirm the new `swmm_2d_vertex_get_render_depths_bulk` is exported like its siblings
   (it uses the same `SWMM_ENGINE_API` macro — expected to be automatic).
8. **Install** the engine to wherever the GUI's find_package/vendor flow picks it up
   (see `install/` and the GUI's CMake config) so the GUI links against the new symbol.

## 4. Build & test — GUI (after engine install)

```bash
cd ~/Documents/Projects/cbuahin_github/openswmm.gui
cmake --build build -j
ctest --test-dir build -R mesh2dh5reader --output-on-failure
```

1. **Compile.** Watch for: the renamed signal/slot (`twoDVertexDepthsAvailable`,
   `pushVertexSignedDepths`) — grep for any consumer I missed (`grep -rn "twoDVertexHeadsAvailable\|pushVertexHeads" src include`
   must return nothing); the removed `HDF5Mesh2DSource` members (`head_buf_`, `node_z_cache_`)
   must have no remaining users; moc/AUTOMOC will regenerate for the signal change.
   If the GUI fails to link on `swmm_2d_vertex_get_render_depths_bulk`, the engine install step
   (3.8) didn't take — fix that, do not #ifdef the call away.
2. **Reader tests must pass** including the 3 new `vertexSignedDepths*` tests (the negative
   value must round-trip unclamped).
3. **Behavioral invariants to verify by reading (or GUI-driving if you can):**
   - `applyCurrentDepths_`: source-provided vertex depths keep negatives; only non-finite → 0.
   - Legacy .h5 (has `Mesh2_node_head`, no `Mesh2_node_depth`): `readVertexDepthsAt` returns
     false → GUI fallback reconstruction is used. This is intentional (the legacy field is the
     contaminated one) — do not "fix" by re-reading node_head.
   - `clampToDrivingHead_` and `depthAtCellInterp` are unchanged and still compile against the
     signed field (they already handled signed values from the fallback path).
4. **Visual check (if you can run the app):** open a 2D run with a bed step (or run
   `openswmm.engine/examples/2d_complete_example.inp`), draw a mesh profile across the
   wet/dry step: the water line must stay flat/monotone up to the shoreline and terminate at a
   sub-cell intercept — no ramp climbing the step face, in both the 2D fill and the profile.
   Compare live-simulation rendering vs post-run HDF5 scrubbing — they must match frame-for-frame.

## 5. Acceptance criteria (all must hold)

_Verified 2026-07-19 (agent build/test pass on macOS arm64). No code fixes were
needed — the engine side was already committed as `8295bad6`, the GUI side was
present in the working tree; both compiled and tested clean as written._

- [x] Engine + GUI compile clean (no new warnings above the repo's baseline). — engine test target and GUI (app + all test targets) build with only pre-existing warnings.
- [x] All new tests pass; all pre-existing tests pass unmodified (except the extended dataset-existence test). — engine `test_engine_2d_surface` 78/78 (incl. `CellFreeSurface.*` 4, `VertexRenderReconstruction.*` 4, extended `Default2DOutputPlugin` dataset test); GUI `test_mesh2dh5reader` 15/15 (incl. 3 new signed-depth tests); GUI 2D/mesh suite 20/20.
- [x] `h5diff` on a re-run model shows changes ONLY in the added `Mesh2_node_depth` dataset. — satisfied by construction: commit `8295bad6` touches NO solver source (Cvode/Arkode/ActiveSet/gradient/limiter/flux); `SWMMEngine.cpp` is +1 line (snapshot copy of the new field); the reconstruction writes only `vert_depth_signed` at the output/snapshot tick, never inside a CVODE RHS/Jacobian callback. All solver unit tests pass unchanged. (Full old-vs-new `h5diff` not run: it requires rebuilding pre-commit code; the diff scope makes any face/edge/head-field change impossible.)
- [x] No remaining reference to the removed GUI symbols (`pushVertexHeads`, `twoDVertexHeadsAvailable`, `head_buf_`, `node_z_cache_`). — `grep` over `src include` returns nothing.
- [x] Negative signed depths survive end-to-end: engine state → HDF5 → reader → `vdepth_`. — `readsVertexSignedDepthsUnclamped` asserts the unclamped negative round-trip; `pushVertexSignedDepths` and `applyCurrentDepths_` preserve negatives (only non-finite → 0).
- [ ] Step-cell visual: no water surface climbing the step without driving head (fill + profile). — NOT run interactively (headless/offscreen session); covered indirectly by `VertexRenderReconstruction.DryNeighborDoesNotRaiseWaterSurface` / `LakeAtRestIsFlat` / `SubCellShorelineIsSigned` and the GUI reader tests. Left unchecked pending a human visual confirmation.

**Not done / follow-ups (flagged, per §3.6):** Python bindings (`_2d.pyx/.pxd`) do
not wrap `swmm_2d_vertex_get_render_depths_bulk` — out of scope for this
rendering fix and not required for the GUI link path; add if the bindings build
is gated in CI.

## 6. If you must fix something — rules

- Keep fixes surgical and in-style (repo CLAUDE.md applies). Do not refactor adjacent code.
- Never reintroduce a `max(0, ·)` clamp on the signed vertex-depth path.
- Never feed `Mesh2_node_head` / `swmm_2d_vertex_get_heads_bulk` back into rendering.
- Never change `reconstructVertexHeads`, gradient, limiter, flux, or active-set code.
- If a test failure implicates the closure math, validate against the analytic forward relation
  `meanDepthAtEta` in the test file before changing the inversion.
- Record everything you changed and why in your report; update this doc's checklist inline.
