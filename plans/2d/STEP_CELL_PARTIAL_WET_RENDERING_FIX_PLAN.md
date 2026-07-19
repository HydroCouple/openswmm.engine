# Plan: Fix Non-Physical Water Surface at Step Cells (Partially Wet Vertex Interpolation)

**Status:** DRAFT — for review
**Date:** 2026-07-18
**Scope:** `openswmm.engine` (2D vertex reconstruction, output, API) + `openswmm.gui` (2D results layer, profile sampler)
**Symptom:** 2D surface rendering and mesh profile show the water surface climbing an adverse slope / bed step with no driving head. Partially wet cells near the step render as wet above the true shoreline.

---

## 1. Root-Cause Analysis

The rendering pipeline has two vertex-field paths, and the *preferred* one is the broken one.

### RC1 (primary): the solver's vertex head is exported as the rendering field, and it blends **dry-cell bed elevations** into shoreline vertices

- `reconstructVertexHeads()` (`src/engine/2d/mesh/VertexReconstruction.cpp`) evaluates the pseudo-Laplacian stencil (Kumar et al. 2009) over **all** incident cells of a vertex — wet or dry. Weights are purely geometric.
- A dry cell carries `head[i] = tri_cz[i]` (its bed elevation) — set at init (`SurfaceRouter2D.cpp:293`), maintained by the flat closure `η = z_c + V/A` (`CvodeSurfaceSolver.cpp:51–62`, `ArkodeSurfaceSolver.cpp:45–56`), and *relied upon* by the solver (`ActiveSetBuilder.cpp:31–43` explicitly documents "dry-cell head = bed elevation").
- At a step: wet cell below with η = 1.0 shares vertices with a dry cell on the crest with z_c = 5.0. The geometric blend puts vertex head ≈ 2–3 m — far above the true free surface. At the **low** vertex (z_v ≈ 0) this yields a spurious depth of 2–3 m; the rendered/profiled surface ramps up toward the step with no head to drive it.
- This contaminated field is exported on both consumer paths:
  - **HDF5:** `Default2DOutputPlugin.cpp:497` writes `state.vert_head` as `Mesh2_node_head`; GUI `HDF5Mesh2DSource::readVertexDepthsAt` (`swmm2dresultslayer.cpp:1070–1090`) converts it to `max(0, head − z_v)`.
  - **Live:** `Api2D.cpp:567/577` (`swmm_2d_vertex_get_heads_bulk`) → `simulationrunner.cpp:~508` → `EngineMesh2DSource::pushVertexHeads` (`swmm2dresultslayer.cpp:915–941`), same `max(0, ·)` conversion.
- When engine vertex heads are available, `applyCurrentDepths_()` (`swmm2dresultslayer.cpp:2498`) **prefers** them and bypasses the GUI's own wet-only reconstruction. The GUI's guard rails don't save us:
  - `clampToDrivingHead_()` (`:1970`) caps the blend at the max η among *wet* vertices — but a contaminated vertex classifies as wet (sd > 0) with an inflated η, so the cap is ineffective exactly where the bug lives.
  - The per-cell dry gate in `depthAtCellInterp()` only zeroes cells the solver marks dry; the artifact paints inside *wet* cells adjacent to the step.

### RC2: the `max(0, head − z_v)` clamp destroys the signed (VFR) depth

Both conversion sites clamp at zero. The GUI's sub-cell shoreline machinery (signed vertex depths → barycentric blend goes negative over the dry part of a partially wet cell → water line meets ground at the sub-cell intercept; see comments in `depthAtCellInterp()`) is therefore **disabled whenever the engine path is active** — the wet/dry line snaps to vertices/edges and the fill overreaches into dry territory.

### RC3: flat-cell closure misstates η for a partially wet (step-spanning) cell

`reconstructFromVolume()` uses `η = z_c + V/A` — exact only for a fully wetted, effectively flat cell. For a cell whose bed spans the step, a small pooled volume physically sits on the low portion with η possibly *below* the centroid elevation; the flat closure reports η ≥ z_c. Every consumer of per-cell η (engine vertex reconstruction, GUI fallback contribution `h·(z_c + h)` in `reconstructVertexSignedDepths()`, `swmm2dresultslayer.cpp:2116–2155`) inherits the overestimate → water half-way up the step.

### RC4 (minor): GUI fallback marks no-wet-neighbor vertices as depth 0, not "dry with η = unknown"

`reconstructVertexSignedDepths()` writes 0 for a vertex with no wet incident cell, which implies η_v = z_v. Blending η from a wet vertex to a high dry vertex then relies entirely on `clampToDrivingHead_` to avoid painting up the slope. It mostly works; it should become exact once RC1–RC3 are fixed, but the invariant should be tested.

**Key design constraint:** `state.vert_head` is a *solver* field — it feeds gradient reconstruction, limiters and edge fluxes (`CvodeSurfaceSolver.cpp:110`, `ArkodeSurfaceSolver.cpp:119`), and the active-set seed pass depends on dry head = bed. **Its semantics must not change.** The fix is a separate, render-oriented reconstruction.

---

## 2. Proposed Fix

### Phase 0 — Reproduce and pin the behavior (engine + GUI)

1. Add a minimal **step flume** regression input: structured strip mesh, bed z = 0 for x < L/2 and z = 1 for x ≥ L/2 (one column of cells spanning the step), pond filled to η = 0.5 at rest.
2. Characterization test asserting today's defect (then inverted into the acceptance test):
   - no vertex with z_v > η_true + tol may carry positive rendered depth;
   - reconstructed η_v ≤ max incident **wet**-cell η (no-new-maxima);
   - lake-at-rest renders a flat surface intersecting the step face.
3. Single-triangle partial-wet test: vertices at z = {0, 0, 1}, small volume → assert the (Phase 3) η(V) inversion against quadrature.

### Phase 1 — Engine: wet-masked render reconstruction (fixes RC1)

New function in `VertexReconstruction.{hpp,cpp}` — e.g. `reconstructVertexFreeSurfaceForOutput(mesh, state, dry_depth, ...)` — writing a **new** array (e.g. `state.vert_eta_render` + implied signed depth), leaving `vert_head` untouched:

- Reuse the existing CSR vertex stencil, but gather only cells with `depth[i] ≥ dry_depth`.
- **Weighting (decision point, §5):** recommend depth-weighted averaging `η_v = Σ h_i·η_i / Σ h_i` over the wet subset — identical in spirit to the GUI fallback (its rationale: deep fully-wet cells dominate shoreline vertices instead of thin transiently-wet cells dragging the surface up a wall). Alternative: renormalize the pseudo-Laplacian weights over the wet subset (higher-order on smooth interiors, but negative-ish behavior at one-sided shoreline stencils is exactly what we're fighting).
- Clamp `η_v ≤ max(η_i over wet stencil cells)` — hard guarantee the reconstruction can never manufacture head above what any wet neighbor supplies (this alone kills "climbing the adverse slope").
- No wet incident cell → vertex is dry: emit signed depth (η_v − z_v) as a **negative/zero** value via η_v = z_v, plus (optionally) a wet mask so consumers can distinguish "dry" from "depth exactly 0".
- Run it once per output/snapshot tick (not per RHS evaluation) in `SurfaceRouter2D` where snapshots are refreshed — cost is one CSR gather, negligible. Honor the active-set mask the same way `reconstructVertexHeads` does (frozen cells are dry ⇒ excluded by the wet gate anyway).

### Phase 2 — Engine: outputs and API (fixes RC2 at the interface)

- `Default2DOutputPlugin`: add dataset `Mesh2_node_depth` (signed vertex depth, float) — keep writing `Mesh2_node_head` for backward compatibility. (Storing *depth* not head in float also preserves precision at high datums — same rationale already documented in `pushVertexHeads`.)
- `SimulationSnapshot`: add `surface_vert_depth_signed` alongside `surface_vert_head`.
- `Api2D`: add `swmm_2d_vertex_get_render_depths_bulk()` (signed). Keep `swmm_2d_vertex_get_heads_bulk` as-is (documented as the solver field).

### Phase 3 — Engine + GUI: partial-wet cell closure for η used in *rendering* (fixes RC3)

- Add a small shared helper (engine: `mesh/` utility; GUI mirrors it): closed-form stage–storage inversion `η(V)` for a planar triangular cell from its three vertex elevations (piecewise: cubic-root branch while the waterline crosses the cell, linear once fully wet; degenerate/flat cells fall back to `z_c + V/A`). Unit-test against numerical quadrature.
- Use it to compute the per-cell η fed into the Phase-1 vertex reconstruction, and in the GUI fallback `reconstructVertexSignedDepths()` (replacing the `z_c + h` contribution) and the max-envelope path so animation and envelope stay one arithmetic (CLAUDE.md §4.01 invariant).
- **Out of scope here (follow-up plan):** adopting the same closure inside the solver (`reconstructFromVolume`/`volumeFromHead`) so *fluxes* see physical heads on step cells. That changes numerical results, the Jacobian, and the smooth dry-limit conductance argument — it should be its own flagged work item, not ride along with a rendering fix.

### Phase 4 — GUI: consume the signed field, stop clamping (fixes RC2/RC4)

- `IMesh2DSource::readVertexDepthsAt` semantics change to **signed** depths:
  - `HDF5Mesh2DSource`: prefer `Mesh2_node_depth`; drop `max(0,·)`.
  - `EngineMesh2DSource::pushVertexHeads` → renamed/paired with the new bulk-depth API; drop `max(0,·)`.
- `applyCurrentDepths_()`: remove the `d < 0 → 0` sanitization for the source path (keep the NaN guard); the fallback path already produces signed values, so `dv0/dv1/dv2`, marching-triangles bands/isolines, Gouraud fill and the QSG renderer already handle negatives.
- **Legacy files / older engines** (only `Mesh2_node_head` present, known-contaminated): do **not** trust it. Either (a) ignore it and use the GUI wet-only fallback (recommended — strictly better than the contaminated field, zero new code), or (b) sanitize it by clamping each vertex η against the max incident wet-cell η computed from the per-cell depths we already load. Decision point, §5.
- Verify `clampToDrivingHead_()` and `depthAtCellInterp()` invariants still hold with the fixed field (they become belt-and-suspenders rather than the only defense) and that `meshprofilesampler.cpp` needs no change (it samples through `depthAtSceneInterp`, so it inherits the fix — WSE = ground + depth on a shared barycentric basis).

### Phase 5 — Verification

Engine unit tests (`tests/unit/engine/test_2d_surface_routing.cpp` + new file):

1. **Lake at rest at a step** (C-property for rendering): flat η across all wet vertices; every vertex above the shoreline reports signed depth ≤ 0; profile intercept lands at the sub-cell waterline within tolerance.
2. **No-new-maxima:** for random wet/dry patterns, `η_v ≤ max incident wet η` for every vertex.
3. **Fully wet mesh:** new reconstruction ≈ old `vert_head` (within stencil-order tolerance) — no regression on smooth interiors; existing `VertexReconstruction` tests (constant-field exactness, weight partition of unity) unchanged.
4. **η(V) inversion:** round-trip V → η → V and quadrature comparison for tilted, near-flat, and degenerate triangles.
5. **Output/API:** `Mesh2_node_depth` present, signed, consistent with snapshot and bulk API; `Mesh2_node_head` byte-identical to before (solver field untouched); `2d_complete_example` regression outputs unchanged except the added dataset.

GUI tests:

6. Sampler/profile test on the step fixture: water line monotone across the pond, meets ground at the sub-cell intercept, exactly zero beyond it; max-depth envelope equals the per-vertex temporal max of the animated field (existing invariant).
7. Visual before/after: 2D fill + profile screenshots on the step flume attached to the PR.

---

## 3. Affected Files (expected)

**openswmm.engine**
- `src/engine/2d/mesh/VertexReconstruction.{hpp,cpp}` — new wet-masked render reconstruction (+ η(V) helper, possibly its own TU)
- `src/engine/2d/data/SurfaceStateData.hpp` — `vert_eta_render` / signed depth array
- `src/engine/2d/SurfaceRouter2D.cpp` — invoke per snapshot/output tick
- `src/engine/2d/output/Default2DOutputPlugin.{hpp,cpp}` — `Mesh2_node_depth` dataset
- `src/engine/2d/api/Api2D.{hpp,cpp}` (+ C API header) — signed bulk getter
- `include/openswmm/plugin_sdk/SimulationSnapshot.hpp`
- `tests/unit/engine/…` — new fixtures/tests

**openswmm.gui**
- `src/layers/swmm2dresultslayer.cpp` / `include/layers/swmm2dresultslayer.h` — source semantics, clamp removal, fallback closure upgrade
- `src/io/mesh2dh5reader.{cpp,h}` — read `Mesh2_node_depth`
- `src/simulation/simulationrunner.cpp` — call new bulk API
- `src/plot/meshprofilesampler.cpp` — verification only (no expected change)

---

## 4. Risks & Mitigations

- **Solver behavior must be bit-identical.** All Phase 1–4 changes are output-side; `vert_head`, gradients, fluxes untouched. Regression-diff HDF5 face fields to prove it.
- **Format/API compatibility.** New dataset + new API entry, old ones retained; GUI probes and falls back (pattern already exists for files without `Mesh2_node_head`).
- **OpenMP determinism.** New reconstruction is a per-vertex CSR gather, `schedule(static)` — same bit-exactness argument as the existing one.
- **Shoreline flicker with depth weighting.** Thin-film cells near `dry_depth` entering/leaving the wet set can flicker vertex η; mitigate with the max-η clamp and (if needed) a smooth weight ramp `w = h·smoothstep(h/dry_depth)`.
- **Envelope/animation drift.** Keep one shared helper for both (existing CLAUDE.md §4.01 discipline).

---

## 5. Decision Points for Review

1. **Vertex weighting for the render field:** depth-weighted mean over wet cells (recommended; matches GUI fallback, robust at fronts) vs wet-renormalized pseudo-Laplacian (higher order on interiors). Could also do pseudo-Laplacian with depth-taper hybrid.
2. **Legacy `Mesh2_node_head` files:** ignore in favor of GUI fallback (recommended) vs GUI-side sanitization clamp.
3. **Dry-vertex encoding:** signed depth with η_v = z_v (simplest, keeps sub-cell intercept via negative blends from neighbors) vs explicit wet mask dataset.
4. **Scope of RC3:** rendering-only closure now (recommended), solver-side V(η) closure as a separate flagged follow-up plan.
5. Naming: `Mesh2_node_depth` vs `Mesh2_node_depth_signed`; API name `swmm_2d_vertex_get_render_depths_bulk`.

---

## 6. Execution Order & Sizing

| Step | Work | Size |
|------|------|------|
| 0 | Step-flume fixture + failing characterization tests | S |
| 1 | Engine wet-masked reconstruction + clamp | M |
| 2 | Dataset / snapshot / API plumbing | S–M |
| 3 | η(V) closure helper + wire into engine render path and GUI fallback/envelope | M |
| 4 | GUI signed-depth consumption, clamp removal, legacy handling | M |
| 5 | Test suite green + visual before/after | S–M |

Phases 1–2 (engine) and 4 (GUI) land as separate PRs; GUI PR degrades gracefully against an older engine via the existing fallback probe.
