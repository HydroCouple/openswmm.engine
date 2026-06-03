# Two-Dimensional Surface Routing — Implementation Strategy

## Overview

This document details the implementation strategy for an optional 2D surface routing module coupled to the OpenSWMM engine. The module implements the **second-order accurate, semi-discrete finite volume formulation** from Kumar, Duffy, and Salvage (2009) — initially limited to the **diffusion-wave surface flow** component. The design follows the same data-oriented, cache-friendly, Structure-of-Arrays (SoA) patterns used throughout the engine.

**Scope — Phase 1 (this document):**
- 2D diffusion-wave surface routing on a triangular mesh
- Coupling to SWMM nodes/junctions via orifice equation with
  surcharge gate and dedicated `coupling_inflow[]` channel (see §6)
- Rainfall from system rain gages
- Time integration via SUNDIALS CVODE (BDF) with GMRES linear solver

**Future phases (strategy outlined, not implemented):**
- Subsurface (Richards' equation) coupling
- Infiltration (Green-Ampt / Horton / SCS)
- Evapotranspiration, snowmelt
- Natural neighbour interpolation for rainfall spatial distribution

---

## 1. Input File Sections

The 2D model is specified via optional sections in the `.inp` file. All section names are prefixed with `2D_` for clarity and to avoid collisions with existing SWMM sections. The sections are registered via `SectionRegistry::register_custom()`.

### 1.1 Section Names

| Section | Status | Purpose |
|---------|--------|---------|
| `[2D_MESH_FILE]` | implemented | Reference an external file containing 2D mesh and configuration sections |
| `[2D_OPTIONS]` | implemented | Solver options, tolerances, time-stepping parameters, HDF5 output path |
| `[2D_VERTICES]` | implemented | Mesh vertex coordinates |
| `[2D_TRIANGLES]` | implemented | Triangle connectivity and surface roughness |
| `[2D_VERTEX_NODE_MAP]` | implemented | Vertex-to-SWMM-node coupling (with optional `Cd` / area) |
| `[2D_TRIANGLE_NODE_MAP]` | implemented | Triangle-centroid-to-SWMM-node coupling (with optional `Cd` / area) |
| `[2D_BOUNDARY_CONDITIONS]` | implemented (storage + parser) | Per-edge boundary type and parameter |
| `[2D_EDGE_CONVEYANCE]` | implemented | Per-edge `[0, 1]` multiplier on the diffusion-wave flux (§11A) |
| `[2D_INITIAL_CONDITIONS]` | *(future)* | Per-cell initial water depth |
| `[2D_INFILTRATION]` | *(future)* | Per-cell or per-zone infiltration params |

Registration of all implemented sections happens through
`openswmm::twoD::register2DSections` (`src/engine/2d/input/SectionHandlers2D.cpp`),
called from `SWMMEngine::open` under `#ifdef OPENSWMM_HAS_2D`.

> **Optional header comments.** Any line starting with `;;` is treated as
> a comment by the standard tokenizer. Two such comments are recognised
> by the engine's pre-scan (see §2A Unit Conversion Strategy):
> `;; UNITS: <unit>` and `;; SOURCE_CRS: <tag>`. They are not
> required, but producers (GUI, hand-edited files) should emit them so
> the file is self-describing and the engine can correctly handle
> SI-on-disk meshes.

### 1.2 `[2D_MESH_FILE]`

An optional section that redirects the engine to read all 2D mesh and configuration sections from a separate file instead of (or in addition to) the main `.inp`.

```
[2D_MESH_FILE]
FILE <path>
```

| Token | Type | Description |
|-------|------|-------------|
| `FILE` | keyword | Case-insensitive keyword |
| `<path>` | string | Absolute path, or path relative to the directory of the parent `.inp` file |

**Rules:**

- Only one `FILE` line is read; any additional lines in the section are ignored.
- If `[2D_MESH_FILE]` is absent, mesh sections are read inline from the main `.inp` (existing behaviour, unchanged).
- The external file may contain any combination of `[2D_OPTIONS]`, `[2D_VERTICES]`, `[2D_TRIANGLES]`, `[2D_VERTEX_NODE_MAP]`, and `[2D_TRIANGLE_NODE_MAP]`.
- If `[2D_OPTIONS]` appears in both the main `.inp` and the external file, the external file values are applied **after** the main file values (external file wins).
- The external file is parsed with the same section parser as the main file. `[2D_MESH_FILE]` is **not** registered in the sub-parser — recursive references are not supported.
- A missing or unreadable external file is a fatal parse error.

**Example — relative path:**

```
[2D_MESH_FILE]
FILE meshes/city_basin.2dm
```

**Example — absolute path:**

```
[2D_MESH_FILE]
FILE /data/shared_meshes/city_basin.2dm
```

**Typical external file layout:**

```
;; OpenSWMM 2D Mesh File — city_basin.2dm

[2D_OPTIONS]
MAX_TIMESTEP    10.0
DRY_DEPTH       0.001
REPORT_2D       YES

[2D_VERTICES]
;;  X        Y        Z       TAG
    0.0      0.0      10.5    V0
    10.0     0.0      10.3    V1
    5.0      8.66     10.1    V2

[2D_TRIANGLES]
;;  V1  V2  V3  Manning_n  TAG
    0   1   2   0.030      T0

[2D_VERTEX_NODE_MAP]
;;  VERTEX  NODE  [CD]   [AREA]
    V0      J1    0.65

[2D_TRIANGLE_NODE_MAP]
;;  TRIANGLE  NODE  [CD]   [AREA]
    T0        ST1   0.60
```

---

### 1.3 `[2D_OPTIONS]`

Parsed by `openswmm::twoD::parse2DOptionsLine`. One `KEY VALUE` pair per
line. Unknown keys are an error (the parser refuses silently dropped
options to avoid mis-typed keys being ignored).

```
;; Solver and timestepping options for the 2D surface routing module
;;
;; Parameter              Value      ;; Notes
;; ---------------------- ---------- ;; --------------------------------
MAX_TIMESTEP              10.0       ;; Max CVODE internal step (s)
MIN_TIMESTEP              0.001      ;; Min CVODE internal step (s)
REL_TOLERANCE             1.0e-4     ;; CVODE relative tolerance
ABS_TOLERANCE             1.0e-6     ;; CVODE absolute tolerance (m of depth)
DRY_DEPTH                 0.001      ;; Wet/dry threshold (m of depth)
LIMITER_EPSILON           1.0e-6     ;; Jawahar-Kamath slope-limiter ε
COUPLING_CD               0.65       ;; Default discharge coefficient (orifice)
LINEAR_SOLVER             GMRES      ;; GMRES (wired) | BICGSTAB / TFQMR (reserved)
PRECONDITIONER            NONE       ;; NONE | JACOBI (wired) | ILU (reserved)
MAX_KRYLOV_DIM            30         ;; Max Krylov subspace dim (GMRES restart)
MAX_CVODE_STEPS           500        ;; Max CVODE internal steps per advance call
COUPLING_INTERVAL         0          ;; 0 = couple every SWMM routing step
REPORT_2D                 YES        ;; YES/NO — toggles internal 2D reporting
OUTPUT_FILE               run.h5     ;; Optional HDF5 output path; relative paths
                                     ;; resolve against the .inp directory
```

**Defaults** (see `SolverOptions2D.hpp`): `MAX_TIMESTEP=10.0`,
`MIN_TIMESTEP=0.001`, `REL_TOLERANCE=1e-4`, `ABS_TOLERANCE=1e-6`,
`DRY_DEPTH=0.001`, `LIMITER_EPSILON=1e-6`, `COUPLING_CD=0.65`,
`LINEAR_SOLVER=GMRES`, `PRECONDITIONER=NONE`, `MAX_KRYLOV_DIM=30`,
`MAX_CVODE_STEPS=500`, `COUPLING_INTERVAL=0`, `REPORT_2D=YES`,
`OUTPUT_FILE` empty (no 2D output).

**Phase-1 reservation:** `BICGSTAB` / `TFQMR` for `LINEAR_SOLVER` and
`ILU` for `PRECONDITIONER` are accepted by the parser but rejected at
`CvodeSurfaceSolver::initialize` with a clear runtime error. The slots
are reserved for the planned hypre/BoomerAMG integration; see
`docs/2D_KNOWN_STIFFNESS_ISSUE.md` for the rationale.

### 1.4 `[2D_VERTICES]`

Parsed by `parse2DVertexLine`. Each row defines a mesh vertex. Vertices
are indexed in order of appearance (0-based). Format:

```
;; X          Y          Z          TAG (optional)
100.0        200.0      10.5
100.5        200.5      10.3       inlet_region
101.0        200.0      10.1
```

- **X, Y** — Horizontal coordinates. **Unit policy (see §2A):** the
  default contract is "project length units" — feet when SWMM
  `FLOW_UNITS` is US, metres otherwise. The engine multiplies XY/Z by
  0.3048 at load time for US projects (`SurfaceRouter2D::initialize`).
  A producer that has already written SI metres can declare
  `;; UNITS: SI (m)` near the top of the file, and the engine will
  skip the load-time scaling.
- **Z** — Ground surface elevation, in the same unit policy as XY.
- **TAG** — Optional string tag. Referenced by `[2D_VERTEX_NODE_MAP]`
  and `[2D_TRIANGLE_NODE_MAP]` as an alternative to the 0-based index.

### 1.5 `[2D_TRIANGLES]`

Parsed by `parse2DTriangleLine`. Each row defines a triangle by
referencing three vertex indices (0-based) and a Manning's roughness
coefficient.

```
;; V1   V2   V3   MANNINGS_N   TAG (optional)
0    1    2    0.035
3    4    5    0.025          road_surface
```

- **V1, V2, V3** — Vertex indices (0-based) into `[2D_VERTICES]`.
- **MANNINGS_N** — Manning's roughness coefficient (s/m^{1/3}).
  Default if unset: 0.035. **No unit conversion is applied** — the
  coefficient is dimensionally consistent under both SI and US
  Manning's equations because the engine's internal solver runs in SI
  and the constant 1.486 conversion lives entirely in the 1D side.
- **TAG** — Optional string tag.

### 1.6 `[2D_VERTEX_NODE_MAP]`

Parsed by `parse2DVertexNodeMapLine`. Maps mesh vertices to SWMM
coupling nodes — exchange flow uses an orifice equation at each
mapped point.

```
;; VERTEX_INDEX_OR_TAG    SWMM_NODE_NAME    [CD]    [AREA]
5                         J1
inlet_region              J2                0.70    1.5
```

- **VERTEX_INDEX_OR_TAG** — 0-based integer OR a tag string defined in
  `[2D_VERTICES]`. Tag form is more robust to mesh edits that
  renumber vertices.
- **SWMM_NODE_NAME** — Name of an existing SWMM `[JUNCTIONS]`,
  `[OUTFALLS]`, or `[STORAGE]` node. Resolved to a node index at
  `SurfaceRouter2D::initialize` time; an unknown name is a fatal
  error.
- **CD** *(optional)* — Discharge coefficient. Default 0.65.
- **AREA** *(optional)* — Effective exchange area (m² in SI projects,
  ft² in US projects — same unit policy as the mesh XY, scaled by
  0.3048² at load time when the mesh declares non-SI units).
  Default 1.0.

### 1.7 `[2D_TRIANGLE_NODE_MAP]`

Parsed by `parse2DTriangleNodeMapLine`. Maps triangle centroids to
SWMM coupling nodes. Same parameter set and unit policy as
`[2D_VERTEX_NODE_MAP]`.

```
;; TRIANGLE_INDEX_OR_TAG  SWMM_NODE_NAME    [CD]    [AREA]
0                         J3
road_surface              J4                0.60    4.0
```

### 1.8 `[2D_BOUNDARY_CONDITIONS]`

Parsed by `parse2DBoundaryConditionsLine`. Per-edge boundary type and
parameter, indexed by `(TRI, EDGE)` where `EDGE ∈ {0,1,2}` is the
local edge index opposite the corresponding vertex. Rows are
accumulated into a pending-rows buffer during parsing
(`SurfaceRouter2D::PendingBoundaryRow`) and drained into the per-edge
`BoundaryData` SoA inside `SurfaceRouter2D::initialize` once the mesh
has been sized.

```
;; TRI   EDGE   TYPE              PARAM_1          PARAM_2   GROUP
;; ---   ----   ---------------   --------------   -------   --------
   12    0      NORMAL_FLOW       0.0020           *         *
   12    1      SPECIFIED_STAGE   95.4             *         *
   45    2      TS_STAGE          DownstreamTS     *         Outlet
   77    0      SPECIFIED_FLOW    0.150            *         *
   77    1      TS_FLOW           Hydrograph_A     *         Inlet
   90    2      RATING_CURVE      Outfall_Q_H      *         Outlet
```

**TYPE** is one of:

| Token | `BoundaryType` enum | PARAM_1 meaning |
|-------|---------------------|-----------------|
| `WALL` | `WALL` (default) | unused — zero-flux |
| `NORMAL_FLOW` | `NORMAL_FLOW` | bed slope (≥ 0, dimensionless) |
| `SPECIFIED_STAGE` | `SPECIFIED_STAGE` | constant total head (m, SI) |
| `TS_STAGE` | `SPECIFIED_STAGE` | timeseries name (head varies in time) |
| `SPECIFIED_FLOW` | `SPECIFIED_FLOW` | discharge per metre of edge (m³/s/m, outward positive) |
| `TS_FLOW` | `SPECIFIED_FLOW` | timeseries name (per-metre flow varies in time) |
| `RATING_CURVE` | `RATING_CURVE` | curve registry name (stage → flow) |

**PARAM_2** is currently reserved (always `*`). **GROUP** is an
optional named group (`*` = none) used by GUI workflows for bulk edits;
the engine stores it but does not act on it today.

> **Current solver behaviour.** Per the comment on
> `BoundaryType` in `BoundaryData.hpp`: storage, parsing, and the C
> API for all five types are implemented. **The FV-SWE flux integration
> for non-Wall BCs is deferred to a follow-up slice (V-E-FLUX)** — the
> flux calculator in `SurfaceFluxCalculator.cpp` (line 131) currently
> treats every boundary edge as Wall regardless of declared type. This
> is intentional: it lets GUI / I/O round-trip work proceed in parallel
> with the FV-SWE BC integration.

### 1.9 `[2D_EDGE_CONVEYANCE]`

Parsed by `parse2DEdgeConveyanceLine`. Per-edge multiplicative factor
in `[0, 1]` that attenuates the diffusion-wave flux across the named
edge. Default 1.0 (unrestricted) for every edge not listed. Motivating
use cases: culverted embankments, partially-permeable hedgerows,
perforated fences, vegetation strips, "leaky" internal weirs.

The row format mirrors SWMM `[CONDUITS]` `From-Node` / `To-Node`
convention but identifies a **mesh edge** by its endpoint vertex
indices. The pair is unordered — swapping `FROM_VERTEX` and
`TO_VERTEX` does not change the value (Q3 silent partner-slot
mirroring; see §11A).

```
;; Per-edge conveyance multiplier in [0, 1]. Default 1.0 (unrestricted).
;; FROM/TO is the (unordered) pair of mesh vertex indices at the edge's
;; endpoints. Authoring an obstruction as a polyline is the GUI's job:
;; walk the polyline, emit one row per shared interior edge.
[2D_EDGE_CONVEYANCE]

;; FROM_VERTEX   TO_VERTEX   CONVEYANCE
;; -----------   ---------   ----------
   17            18          0.40        ;; hedgerow segment
   18            19          0.40
   42            87          0.30        ;; culverted embankment
   55            61          0.00        ;; fully blocked → equivalent to Wall
```

Parser rules (5d):

- Exactly three tokens per row: `FROM_VERTEX TO_VERTEX CONVEYANCE`.
- `FROM_VERTEX` and `TO_VERTEX` are non-negative integers in
  `[0, n_vertices)`, **must differ**. Equal endpoints raise a
  parse-time error; out-of-range endpoints raise an init-time error.
- `CONVEYANCE` parses as a `double`; values outside `[0, 1]` are
  rejected at parse time (Q1 strict clamp).
- Rows accumulate into `SurfaceRouter2D::pendingEdgeConveyanceRows()`
  during parsing. `SurfaceRouter2D::initialize` drains them after
  `buildMeshTopology`, builds a one-shot vertex-pair → slot lookup
  in one O(n_triangles) pass, and writes the factor into every slot
  the edge resolves to (interior = 2 slots, boundary = 1).
- Duplicate rows naming the same edge: last-write-wins.
- A vertex pair that does not form a real mesh edge raises a fatal
  init-time error.

The factor is applied in `SurfaceFluxCalculator::computeEdgeFluxes`
immediately before `state.edge_flux[idx] = F_e`, **after** the
wet/dry Hermite shutoff. Today it has no effect on boundary edges
(the calculator early-returns at `nbr < 0`); once the V-E-FLUX slice
lands the factor will also multiply BC-derived boundary fluxes.

C API: `swmm_2d_get_edge_conveyance` / `swmm_2d_set_edge_conveyance`
/ `swmm_2d_get_edge_conveyance_bulk` / `swmm_2d_reset_edge_conveyance`.
`set` clamps to `[0, 1]` and silently mirrors to the partner slot
for interior edges. Safe between routing steps; calling DURING a
routing step is undefined (the CVODE sub-stepper holds a const
reference to the mesh).

---

## 2. Data Structures (SoA Layout)

All 2D data structures follow the existing OpenSWMM SoA pattern: parallel `std::vector` arrays, a single `resize()` method, `save_state()` / `reset_state()` lifecycle methods, and flat 2D arrays where needed.

### 2.1 File: `src/engine/2d/data/MeshData.hpp`

The actual struct (abbreviated; comments preserved):

```cpp
namespace openswmm::twoD {

struct MeshData {

    // Vertex arrays — indexed by vertex index [0, n_vertices)
    std::vector<double>      vx, vy, vz;             // coords + ground elevation
    std::vector<std::string> vtag;                   // optional tag

    // Triangle connectivity (3 vertex indices per triangle)
    std::vector<int> tri_v0, tri_v1, tri_v2;
    // Neighbour connectivity (3 adjacent triangle indices, -1 = boundary)
    std::vector<int> tri_nbr0, tri_nbr1, tri_nbr2;

    // Precomputed cell geometry
    std::vector<double> tri_area;     // Planimetric area (m²)
    std::vector<double> tri_cx, tri_cy, tri_cz;  // Centroid xyz

    // Edge geometry — flat 2D, [tri * 3 + edge_local]
    std::vector<double> edge_length;
    std::vector<double> edge_nx, edge_ny;     // outward unit normal
    std::vector<double> edge_mx, edge_my, edge_mz;  // edge midpoint xyz

    // §11A — per-edge conveyance factor in [0,1] (default 1.0).
    // Mirrored across interior edges for mass conservation.
    std::vector<double> edge_conveyance;

    // Surface properties
    std::vector<double>      mannings_n;
    std::vector<std::string> tri_tag;

    // Vertex reconstruction stencil — CSR (pseudo-Laplacian weights)
    std::vector<int>    vert_stencil_ptr;
    std::vector<int>    vert_stencil_idx;
    std::vector<double> vert_stencil_wt;

    // Coupling maps (resolved indices, -1 = none)
    std::vector<int>    vert_coupled_node;
    std::vector<int>    tri_coupled_node;

    // Coupling parameters per coupling point
    std::vector<double> vert_coupling_cd;     // Discharge coefficient (default 0.65)
    std::vector<double> vert_coupling_area;   // Effective exchange area
    std::vector<double> tri_coupling_cd;
    std::vector<double> tri_coupling_area;

    // Deferred-resolution names (cleared by SurfaceRouter2D::initialize)
    std::vector<std::string> vert_coupled_node_name;
    std::vector<std::string> tri_coupled_node_name;

    int n_vertices()  const noexcept { return static_cast<int>(vx.size()); }
    int n_triangles() const noexcept { return static_cast<int>(tri_v0.size()); }

    void resize_vertices(int nv);   // also resizes vert_coupled_node[_name],
                                    // vert_coupling_cd / area
    void resize_triangles(int nt);  // also resizes edge_*, mannings_n,
                                    // tri_coupled_node[_name], tri_coupling_cd / area
};

} // namespace openswmm::twoD
```

> **Implementation note.** Topology and stencil construction are NOT
> member functions of `MeshData`. They live in free functions
> `openswmm::twoD::buildMeshTopology(MeshData&)` (in `mesh/MeshBuilder.cpp`)
> and `openswmm::twoD::buildVertexStencils(MeshData&)` (in
> `mesh/VertexReconstruction.cpp`), called from
> `SurfaceRouter2D::initialize` in the order: optional ft→m mesh scaling
> → `buildMeshTopology` → `validateMesh` → `buildVertexStencils`.

### 2.2 File: `src/engine/2d/data/SurfaceStateData.hpp`

All values below are in the **2D solver's SI internal units** (m, m/s,
m²/s, etc.). Any conversion from the SWMM 1D side (which runs in
project units) happens at the boundary; see §2A Unit Conversion
Strategy.

```cpp
namespace openswmm::twoD {

struct SurfaceStateData {

    // State variables — per triangle [0, n_triangles)
    std::vector<double> depth;          // Overland flow depth ψ_o (m)
    std::vector<double> head;           // Total head h_o = z_s + ψ_o (m)

    // Gradient fields (per triangle)
    std::vector<double> grad_hx,     grad_hy;       // unlimited
    std::vector<double> grad_hx_lim, grad_hy_lim;   // Jawahar-Kamath limited

    // Reconstructed head at vertices — [0, n_vertices)
    std::vector<double> vert_head;

    // Cell-centred velocity (RT0 reconstruction from edge fluxes)
    std::vector<double> face_vx, face_vy;           // (m/s)

    // Per-cell continuity residual (m³/s; ≈0 when conservative)
    std::vector<double> cell_continuity_err;

    // Edge fluxes — flat 2D: [tri * 3 + edge]
    std::vector<double> edge_flux;

    // Source / sink terms — per triangle (m/s of depth)
    std::vector<double> rainfall;       // from rain gages
    std::vector<double> coupling_flux;  // exchange with SWMM nodes (+ = into 2D)
    std::vector<double> net_source;     // accumulated source / sink

    // -----------------------------------------------------------------------
    // Forcing-override channels (C API — external control)
    // mode: 0=computed, 1=override, 2=add; persist: 0=reset, 1=persist
    // -----------------------------------------------------------------------
    std::vector<int8_t> rainfall_forced, rainfall_persist;
    std::vector<double> rainfall_force_val;
    std::vector<int8_t> coupling_forced, coupling_persist;
    std::vector<double> coupling_force_val;

    // Previous step state (for restart / Picard reset)
    std::vector<double> old_depth;

    // Cumulative statistics — per cell
    std::vector<double> stat_max_depth;
    std::vector<double> stat_cum_volume;

    void resize(int n_triangles, int n_vertices);  // assigns all arrays
    void save_state()  noexcept;                   // depth → old_depth
    void reset_state() noexcept;                   // old_depth → depth
    void clear_reset_forcings() noexcept;          // honour persist=0 flags
    void update_statistics(const std::vector<double>& tri_area,
                           double dt) noexcept;
};

} // namespace openswmm::twoD
```

### 2.3 File: `src/engine/2d/data/SolverOptions2D.hpp`

```cpp
namespace openswmm::twoD {

enum class LinearSolverType : int8_t {
    GMRES    = 0,   // Phase 1 — WIRED
    BICGSTAB = 1,   // Reserved; initialize() rejects
    TFQMR    = 2    // Reserved; initialize() rejects
};

enum class PreconditionerType : int8_t {
    NONE   = 0,     // Phase 1 — WIRED
    JACOBI = 1,     // Phase 1 — WIRED (diagonal heuristic)
    ILU    = 2      // Reserved; initialize() rejects
};

struct SolverOptions2D {
    // CVODE / linear-solver knobs (parsed from [2D_OPTIONS]; see §1.3)
    double max_timestep      = 10.0;    // Max CVODE internal step (s)
    double min_timestep      = 0.001;   // Min CVODE internal step (s)
    double rel_tolerance     = 1.0e-4;
    double abs_tolerance     = 1.0e-6;
    double dry_depth         = 0.001;   // Wet/dry threshold (m)
    double limiter_epsilon   = 1.0e-6;  // Slope-limiter ε
    double coupling_cd       = 0.65;    // Default discharge coefficient
    int    max_krylov_dim    = 30;
    int    coupling_interval = 0;       // 0 = couple every SWMM step
    int    max_cvode_steps   = 500;
    bool   report_2d         = true;

    LinearSolverType   linear_solver   = LinearSolverType::GMRES;
    PreconditionerType preconditioner  = PreconditionerType::NONE;

    // File paths from input
    std::string mesh_file;     // [2D_MESH_FILE] FILE token (may be relative)
    std::string output_file;   // [2D_OPTIONS] OUTPUT_FILE (HDF5; may be relative)

    // -----------------------------------------------------------------------
    // Unit-system bridge — NOT parsed from input; computed in
    // SurfaceRouter2D::initialize() from the project FLOW_UNITS.
    // The 2D solver runs internally in SI; these convert at the coupling
    // boundary (see NodeCoupling.cpp). SI projects (CMS/LPS/MLD) yield 1.0.
    // -----------------------------------------------------------------------
    double len_1d_to_2d  = 1.0;  // ft → m (= 0.3048 for US)
    double len_2d_to_1d  = 1.0;  // m  → ft
    double vol_1d_to_2d  = 1.0;  // ft³ → m³
    double flow_1d_to_2d = 1.0;  // ft³/s → m³/s
    double flow_2d_to_1d = 1.0;  // m³/s → ft³/s

    // -----------------------------------------------------------------------
    // ;; UNITS: header flag — set by prescan2DUnitsHeader, NOT parsed from
    // [2D_OPTIONS]. When true the mesh on disk is already SI (m), so
    // SurfaceRouter2D::initialize SKIPS the ft→m mesh scaling.  The
    // coupling-side factors above stay FLOW_UNITS-driven because they
    // describe the 1D side of the boundary, not the mesh.
    // -----------------------------------------------------------------------
    bool mesh_units_si = false;
};

} // namespace openswmm::twoD
```

### 2.4 File: `src/engine/2d/data/BoundaryData.hpp`

Per-edge boundary-condition SoA, flat-indexed `[tri * 3 + edge_local]`
to match `edge_flux`, `edge_length`, etc. Sized to `n_triangles * 3`
inside `SurfaceRouter2D::initialize` and initialised to `WALL` defaults;
parsed rows from `[2D_BOUNDARY_CONDITIONS]` are then drained from the
`PendingBoundaryRow` scratch buffer into the appropriate slot.

```cpp
namespace openswmm::twoD {

enum class BoundaryType : int8_t {
    WALL            = 0,   // Zero-flux wall (default)
    NORMAL_FLOW     = 1,   // Manning outflow using bed slope
    SPECIFIED_STAGE = 2,   // Prescribed water surface elevation (constant or TS)
    SPECIFIED_FLOW  = 3,   // Per-metre discharge (constant or TS)
    RATING_CURVE    = 4    // Stage → flow lookup
};

struct BoundaryData {

    std::vector<int8_t> edge_bc_type;       // BoundaryType cast

    // NORMAL_FLOW
    std::vector<double> edge_bed_slope;     // dimensionless, ≥ 0

    // SPECIFIED_STAGE
    std::vector<double> edge_bc_head;       // total head (m)
    std::vector<int>    edge_bc_tseries;    // -1 const, -2 unresolved name, ≥0 table idx
    std::vector<std::string> edge_bc_tseries_name;

    // SPECIFIED_FLOW
    std::vector<double> edge_bc_flow;       // per-metre discharge (m³/s/m, outward+)
    std::vector<int>    edge_bc_flow_tseries;
    std::vector<std::string> edge_bc_flow_tseries_name;

    // RATING_CURVE
    std::vector<int>    edge_bc_rating_curve;       // -1, -2, or curve index
    std::vector<std::string> edge_bc_rating_curve_name;

    // Cumulative boundary flux (m³, outflow positive) — mass-balance
    std::vector<double> edge_bc_cum_flux;

    void resize(int n_edges);
    int  size() const noexcept;
};

} // namespace openswmm::twoD
```

> **Current solver behaviour.** Storage + parsing + C API are wired for
> all five `BoundaryType` values, but
> `SurfaceFluxCalculator::computeEdgeFluxes` treats every boundary edge
> as `WALL` regardless of declared type. The FV-SWE non-Wall flux
> integration is deferred to slice V-E-FLUX. `edge_bc_cum_flux` is
> updated only when that slice lands; today it stays at 0 for non-Wall
> edges.

---

## 2A. Unit Conversion Strategy

This section is referenced from §1.4 / §1.6 / §2.3 / §6 / §8. It is the
single authoritative description of how units flow between the SWMM 1D
engine, the 2D mesh on disk, and the 2D solver's internal SI world.

### 2A.1 The two clocks

| Side | Internal unit system | Where it lives |
|------|----------------------|----------------|
| 1D SWMM engine | **Project units** — feet / ft³ / ft³·s⁻¹ for US `FLOW_UNITS` (CFS, GPM, MGD); metres / m³ / m³·s⁻¹ for SI `FLOW_UNITS` (CMS, LPS, MLD). Manning's *g* = 32.2 ft·s⁻², *φ* = 1.486 for US. | `ctx.nodes.*`, `ctx.links.*`, `ctx.options.flow_units`. |
| 2D solver | **SI** — metres / m² / m³ / m³·s⁻¹, *g* = 9.80665 m·s⁻². | `mesh_`, `state_`, `NodeCoupling.cpp` after multiplication. |

The 1D engine is unit-aware throughout — it converts to display units
only at output. The 2D solver is intentionally unit-naive: every double
inside `MeshData`, `SurfaceStateData`, and the Kumar et al. (2009) FV
math is in SI. The bridge between the two systems lives in two narrow
places: (a) the mesh-load scaling in `SurfaceRouter2D::initialize`,
and (b) the per-quantity multiplications in `NodeCoupling.cpp`.

### 2A.2 The five bridge factors

Defined in `SolverOptions2D`, computed once at the top of
`SurfaceRouter2D::initialize` from `ctx.options.flow_units`:

```cpp
const int    us      = ucf::getUnitSystem(static_cast<int>(ctx.options.flow_units));
const double ft_to_m = (us == 0) ? 0.3048 : 1.0;   // us==0 → US
options_.len_1d_to_2d  = ft_to_m;
options_.len_2d_to_1d  = 1.0 / ft_to_m;
options_.vol_1d_to_2d  = ft_to_m * ft_to_m * ft_to_m;
options_.flow_1d_to_2d = options_.vol_1d_to_2d;
options_.flow_2d_to_1d = 1.0 / options_.vol_1d_to_2d;
```

For SI projects every factor is 1.0 — no work happens at the boundary.

### 2A.3 Mesh load: optional one-shot ft→m scaling

Immediately after the factors are computed:

```cpp
if (!options_.mesh_units_si && options_.len_1d_to_2d != 1.0) {
    const double f  = options_.len_1d_to_2d;
    const double f2 = f * f;
    for (auto& v : mesh_.vx) v *= f;
    for (auto& v : mesh_.vy) v *= f;
    for (auto& v : mesh_.vz) v *= f;
    for (auto& a : mesh_.vert_coupling_area) a *= f2;
    for (auto& a : mesh_.tri_coupling_area)  a *= f2;
}
```

The default assumption is that `[2D_VERTICES]` / `[2D_TRIANGLES]` and
the optional `[CD] [AREA]` columns on `[2D_*_NODE_MAP]` are in project
length units (feet for US, metres for SI). The scaling brings them into
SI before `buildMeshTopology` runs, so every derived quantity
(`tri_area`, `edge_length`, centroids, midpoint Z) is computed in SI.

When `options_.mesh_units_si == true` the loop is skipped entirely —
the mesh is already SI and applying the factor a second time would
scale a US project's mesh down by 0.3048 (a 10× error in area).

### 2A.4 Header pre-scan — opting into SI

`mesh_units_si` is **not** parsed from `[2D_OPTIONS]`. It is set by
`openswmm::twoD::prescan2DUnitsHeader(path, opts)` in
`src/engine/2d/input/SectionHandlers2D.cpp`. The helper opens the file,
walks `;;`-prefixed comment lines, and looks for:

```
;; UNITS: <value>
;; SOURCE_CRS: <tag>     ;; informational only; engine ignores
```

If `<value>` matches one of `SI (m)`, `m`, `metre`, `metres`, `meter`,
`meters` (case-insensitive), the flag is set to `true`. Any other value
— including an explicit `ft`, `foot`, `feet`, or absent header —
leaves the flag at its prior value.

Call sites:

| Caller | File | When |
|--------|------|------|
| `SWMMEngine::open` | `src/engine/core/SWMMEngine.cpp` (~L142) | Inline `.inp`, right after `register2DSections`. |
| `load2DMeshExternalFile` | `src/engine/2d/input/SectionHandlers2D.cpp` (~L472) | External `.2dm`, after path resolution, before parsing. |

The external file scan runs *after* the inline scan, so an external
`.2dm` declaration overrides whatever the main `.inp` claimed. This
matches how `[2D_OPTIONS]` works (external file wins on key
collisions).

### 2A.5 1D ⇄ 2D coupling boundary (per-quantity)

Every conversion in `NodeCoupling.cpp` traces back to one of the five
factors above. The full list, in the order it appears in
`computeCouplingExchange` / `updateOutfallBoundaries` /
`transferOutfallDischarges`:

| Quantity | Direction | Source | Multiplication | Result unit |
|----------|-----------|--------|----------------|-------------|
| 1D node head (`nodes.head[ni]`) | 1D → 2D | SWMM | `× len_1d_to_2d` | m |
| 1D node depth (`nodes.depth[ni]`) | 1D → 2D | SWMM | `× len_1d_to_2d` | m |
| 1D rim/cap elevation (`invert_elev + full_depth + sur_depth`) | 1D → 2D | SWMM | `× len_1d_to_2d` | m |
| 1D node available volume (`full_volume − volume`) | 1D → 2D | SWMM | `× vol_1d_to_2d` | m³ |
| Head difference `dh = h_2d − h_1d` | computed | — | both already in m | m |
| Orifice `Q = Cd·A·sign(dh)·√(2g·│dh│)`, *g*=9.80665 | computed | — | inputs in SI | m³·s⁻¹ |
| 2D → 1D coupling flow (`coupling_inflow[ni]`) | 2D → 1D | NodeCoupling | `× flow_2d_to_1d` | ft³·s⁻¹ (US) or m³·s⁻¹ (SI) |
| 2D outfall head feedback (`outfall_2d_head[ni]`) | 2D → 1D | NodeCoupling | `× len_2d_to_1d` | ft (US) or m (SI) |
| 1D outfall discharge (`nodes.outflow[ni]`) | 1D → 2D | NodeCoupling | `× flow_1d_to_2d` | m³·s⁻¹ |

The 2D side never sees feet. The 1D side never sees an unconverted SI
value entering a continuity accumulator.

### 2A.6 Manning's *n*

Manning's *n* is the only mesh-side quantity that is **not** multiplied
at load time. The Manning equation has a built-in *φ* = 1.486
unit-conversion constant in US engines that absorbs the foot/metre
mismatch; in the SI 2D solver the constant is 1.0 and the same numeric
*n* value gives the same physical roughness. Conventional values (e.g.
*n* = 0.035 for natural channels) are unit-agnostic.

### 2A.7 What this leaves unprotected

The bridge assumes the mesh's XY linear unit and the SWMM
`FLOW_UNITS`-implied length unit *agree* in the legacy
(header-absent) case. Two failure modes the engine does **not** guard
against today:

1. SWMM `FLOW_UNITS = CFS` with a mesh stored in metres (no header).
   → Engine multiplies metres by 0.3048 → mesh becomes 30.48% of true
   size.
2. SWMM `FLOW_UNITS = CMS` with a mesh stored in feet (no header).
   → Engine leaves values untouched → solver treats feet as metres.

Both are fixable by adding `;; UNITS:` to the producer side. A future
project-open warning that cross-checks the project CRS linear unit
against `FLOW_UNITS` is tracked in
`docs/MESH_CRS_UNIT_CONVERSION_PLAN.md` follow-up F2.

---

## 3. Mathematical Formulation — Diffusion-Wave Surface Flow

### 3.1 Governing Equation

The 2D diffusion-wave approximation of St. Venant's equation (Kumar et al., 2009, Eq. [1]):

```
∂ψ_o/∂t = ∇·(ψ_o · K(ψ_o) · ∇h_o) - Q_og + Q_ss
```

Where:
- `ψ_o` = overland flow depth (m)
- `h_o = z_s + ψ_o` = total overland flow head (m)
- `z_s` = ground surface elevation (m)
- `K(ψ_o)` = diffusive conductance (m/s)
- `Q_og` = vertical flux exchange with subsurface (m/s) — **zero in Phase 1**
- `Q_ss` = sources/sinks (rainfall, evaporation) (m/s)

### 3.2 Diffusive Conductance

```
K(ψ_o) = (ψ_o^{2/3}) / (n · |∂h_o/∂s|^{1/2})
```

Where `n` is Manning's roughness and `s` is the direction of maximum slope.

### 3.3 Semi-Discrete Finite Volume Form (Eq. [10])

For each triangular cell `i`:

```
A_i · dψ_o/dt = Σ_{j=1}^{3} n_j · F_j + Q_ss · V_i
```

Where:
- `A_i` = planimetric area of triangle `i`
- `F_j` = lateral flux vector on edge `j`
- `n_j` = outward normal to edge `j`
- The coupling flux `G_k` (vertical flux to subsurface) is zero in Phase 1

### 3.4 Lateral Flux Calculation (Eq. [15a])

```
n_j · F_j = UW[ψ_o · K(ψ_o) · ∇h_o]_ξ · ξ_ij
```

Where `UW[]` is the upwind function (flux computed at the upstream cell face) and `ξ_ij` is the edge length.

### 3.5 Second-Order Accuracy Components

1. **Edge Gradient Calculation** (Eq. [16]–[18]): Green-Gauss theorem on variational triangles
2. **Vertex Reconstruction** (Eq. [19]–[21]): Pseudo-Laplacian weighted interpolation from cell centres to vertices
3. **Linear Reconstruction at Edges** (Eq. [22]): `h_ξ = h_c + r · ∇h_l`
4. **Limited Gradient** (Eq. [23]–[24]): Jawahar-Kamath multidimensional limiter with weights based on L2 norms of unlimited gradients
5. **Unlimited Gradient** (Eq. [25]–[26]): Area-weighted average of edge gradients

---

## 4. Solver Architecture

### 4.1 File Organization

```
src/engine/2d/
├── CMakeLists.txt
├── data/
│   ├── MeshData.hpp
│   ├── SurfaceStateData.hpp
│   └── SolverOptions2D.hpp
├── mesh/
│   ├── MeshBuilder.hpp             // Topology, neighbours, edge geometry
│   ├── MeshBuilder.cpp
│   ├── VertexReconstruction.hpp    // Pseudo-Laplacian stencil weights
│   └── VertexReconstruction.cpp
├── solver/
│   ├── SurfaceFluxCalculator.hpp   // Edge flux, gradient, limiter
│   ├── SurfaceFluxCalculator.cpp
│   ├── CvodeSurfaceSolver.hpp      // CVODE wrapper for surface ODE system
│   ├── CvodeSurfaceSolver.cpp
│   └── DiffusiveConductance.hpp    // K(ψ_o) computation
├── coupling/
│   ├── NodeCoupling.hpp            // Orifice-equation exchange with SWMM
│   └── NodeCoupling.cpp
├── input/
│   ├── SectionHandlers2D.hpp       // Input section parsers
│   └── SectionHandlers2D.cpp
├── SurfaceRouter2D.hpp             // Top-level orchestrator
└── SurfaceRouter2D.cpp
```

### 4.2 CVODE Integration

**Dependency:** SUNDIALS (via vcpkg: `sundials[cvode]`)

The surface routing ODE system is:

```
dy/dt = f(t, y)
```

where `y[i] = ψ_o[i]` (overland flow depth at triangle `i`), and `f(t, y)` computes the right-hand side from the semi-discrete finite volume formulation.

#### CVODE Setup

```cpp
class CvodeSurfaceSolver {
public:
    /// Initialize CVODE with the ODE system of size n_triangles
    void initialize(const MeshData& mesh, const SolverOptions2D& opts);

    /// Advance the solution from t_current to t_target
    /// Returns actual time reached
    double advance(double t_current, double t_target,
                   SurfaceStateData& state, const MeshData& mesh);

    /// Clean up CVODE memory
    void finalize();

private:
    void* cvode_mem_ = nullptr;     // CVODE memory block
    SUNLinearSolver ls_ = nullptr;  // GMRES (or alternative)
    N_Vector y_ = nullptr;          // State vector (wraps state.depth)
    SUNContext ctx_ = nullptr;      // SUNDIALS context

    /// RHS function: f(t, y, ydot)
    /// Registered as CVRhsFn callback
    static int rhs_fn(sunrealtype t, N_Vector y, N_Vector ydot, void* user_data);
};
```

#### RHS Function Pseudocode

```
rhs_fn(t, y, ydot, user_data):
    solver_ctx = (SolverContext*)user_data
    mesh = solver_ctx->mesh
    state = solver_ctx->state
    opts = solver_ctx->opts

    // 1. Copy y into state.depth, compute head = z + depth
    for i in 0..n_triangles:
        state.depth[i] = max(y[i], 0)
        state.head[i]  = mesh.tri_cz[i] + state.depth[i]

    // 2. Reconstruct head at vertices (pseudo-Laplacian, Eq. [19])
    reconstruct_vertex_heads(mesh, state)

    // 3. Compute unlimited gradients (Eq. [25]-[26])
    compute_unlimited_gradients(mesh, state)

    // 4. Apply slope limiter (Eq. [23]-[24])
    compute_limited_gradients(mesh, state)

    // 5. Compute edge fluxes (Eq. [15a], [22], [30])
    for i in 0..n_triangles:
        for e in 0..3:
            compute_edge_flux(i, e, mesh, state, opts)

    // 6. Assemble RHS: A_i * dψ/dt = Σ fluxes + sources
    for i in 0..n_triangles:
        rhs = 0
        for e in 0..3:
            rhs += state.edge_flux[i * 3 + e]
        rhs += state.rainfall[i] * mesh.tri_area[i]
        rhs += state.coupling_flux[i] * mesh.tri_area[i]
        ydot[i] = rhs / mesh.tri_area[i]
```

#### Why CVODE with GMRES

- **CVODE (BDF)** is designed for stiff ODE systems. The nonlinearity of Manning's equation (depth-dependent conductance) and the potentially stiff coupling between wet and dry cells makes this an appropriate choice.
- **GMRES** (Generalized Minimal Residual) is a Krylov iterative solver that avoids forming the full Jacobian matrix. CVODE uses a difference-quotient approximation for Jacobian-vector products, so the Jacobian is never explicitly stored.
- **Alternatives available:** SUNDIALS also provides BiCGStab (`SUNLinearSolver_SPBCGS`) and TFQMR (`SUNLinearSolver_SPTFQMR`) — exposed via `[2D_OPTIONS]`.

#### vcpkg Integration

Add to `vcpkg.json`:
```json
{
    "name": "sundials",
    "version>=": "7.0.0",
    "features": ["cvode"]
}
```

CMake:
```cmake
option(OPENSWMM_BUILD_2D "Build optional 2D surface routing module" OFF)

if(OPENSWMM_BUILD_2D)
    find_package(SUNDIALS REQUIRED COMPONENTS cvode)
    target_link_libraries(openswmm_engine PRIVATE SUNDIALS::cvode)
    target_compile_definitions(openswmm_engine PRIVATE OPENSWMM_HAS_2D=1)
endif()
```

The 2D module is **compile-time optional** — when `OPENSWMM_BUILD_2D=OFF`, no SUNDIALS dependency is required and all 2D code is excluded via `#ifdef OPENSWMM_HAS_2D`.

---

## 5. Mesh Processing Pipeline

### 5.1 Topology Construction (`MeshBuilder`)

After parsing `[2D_VERTICES]` and `[2D_TRIANGLES]`:

1. **Build edge-neighbour adjacency** — For each triangle, find the adjacent triangle sharing each edge. Use a hash map keyed by sorted vertex-pair `(min(va, vb), max(va, vb))` → first triangle sets the entry, second triangle completes the pair.

2. **Compute edge geometry** — For each edge of each triangle:
   - Edge length `ξ_ij`
   - Outward unit normal `(nx, ny)` — perpendicular to edge, pointing away from cell centre
   - Edge midpoint `(mx, my, mz)`

3. **Compute cell geometry** — For each triangle:
   - Planimetric area `A_i` via cross product
   - Centroid `(cx, cy, cz)` as average of vertex coordinates

4. **Identify boundary edges** — Edges with no neighbour (`nbr = -1`) are domain boundaries. Default: zero-flux (wall). Future: configurable via `[2D_BOUNDARY_CONDITIONS]`.

### 5.2 Vertex Reconstruction Stencils (`VertexReconstruction`)

For each vertex `b`, build the pseudo-Laplacian reconstruction stencil (Eq. [19]–[21]):

1. Collect all triangles sharing vertex `b` → stencil cells `{1, ..., M}`
2. Compute moments: `I_xx`, `I_yy`, `I_xy`, `R_x`, `R_y`
3. Compute Lagrange multipliers: `λ_x`, `λ_y`
4. Compute weights: `ω_i = 1 + λ_x(x_i - x_b) + λ_y(y_i - y_b)`
5. Clip extraneous weights at boundaries (Jawahar & Kamath, 2000)
6. Store in CSR format: `vert_stencil_ptr`, `vert_stencil_idx`, `vert_stencil_wt`

---

## 6. Coupling to SWMM

This section reflects the as-built implementation in
`openswmm.engine/src/engine/2d/coupling/NodeCoupling.cpp`. See also
§2A on unit conversion and `docs/1D_2D_COUPLING_GATE_REVIEW.md`
for the design rationale behind the surcharge gate (C1/C2) and the
ponded-area suppression (C3a).

### 6.1 Coupling Philosophy

- The 2D module communicates with SWMM **via a dedicated
  `nodes.coupling_inflow[]` channel** (not the generic forcing API).
  The earlier design that routed 2D coupling through
  `forcing.node_lat_inflow_value` with `OVERRIDE+PERSIST` was reverted
  per review §11: it conflated 2D coupling with user-API forcing and
  silently dropped the negative (1D→2D) half from routing continuity.
- `coupling_inflow[ni]` is signed: positive = 2D → 1D (drain into
  pipe); negative = 1D → 2D (surcharge spill). The value is drained
  inside `assembleLateralInflows` at the start of the next routing
  step, where the sign is split into `routing_external` (positive
  side) and `routing_flooding` (negative side, |Q|) for mass balance.
- The forcing API remains free for user-controlled inflows and is
  unaffected by 2D coupling.

### 6.2 Coupling at Nodes — Orifice Equation with Gates

The per-coupling-point exchange is built up in six pieces inside
`computeCouplingExchange`. Numbered identifiers (C1, C2, C3a) match
`docs/1D_2D_COUPLING_GATE_REVIEW.md`.

**1. Heads, in SI.** All 1D quantities are multiplied by the unit
bridge factors of §2A:

```
h_1d           = nodes.head[ni]            * opts.len_1d_to_2d   // m
depth_1d_avail = nodes.depth[ni]           * opts.len_1d_to_2d   // m
z_top          = (invert_elev + full_depth + sur_depth)
                                            * opts.len_1d_to_2d   // m
```

`h_2d` and `z_2d` come from `state.vert_head[v]` / `mesh.vz[v]`
(vertex coupling) or `state.head[ci]` / `mesh.tri_cz[ci]` (triangle
centroid coupling) — both already SI.

**2. Available depth (max over the vertex stencil).** For
vertex-coupled points the 2D-side available depth is the **maximum**
depth across every cell in the vertex's reconstruction stencil. Two
failure modes this resolves:

- (a) Spurious positive `vert_head − vert_z` on a fully dry mesh when
  the vertex sits at a local low spot. The pseudo-Laplacian
  reconstruction averages neighbour cell heads (all equal to their
  centroid z), so the reconstructed head exceeds the vertex z by the
  bed-relief amount alone.
- (b) Spurious zero `state.depth[first_tri_containing_v]` on a wet
  bowl when the vertex sits below every touching cell's centroid: the
  FV grid has no cell at the bowl bottom, so each touching cell's
  depth = max(0, head − centroid_z) stays at 0 even when surrounding
  cells hold water.

Taking the max over the stencil ramps to 1 as soon as any neighbour
holds water; truly dry → every stencil cell has depth 0 → ramp 0.

**3. Orifice + smooth wet/dry ramp.**

```
dh    = h_2d - h_1d
Q_raw = Cd * A_eff * sign(dh) * sqrt(2 * g * |dh|)        // g = 9.80665
Q     = Q_raw * wetRamp(source-side available depth)
```

The wet/dry ramp is a Hermite C¹ ramp,
`t² · (3 − 2t)` with `t = clamp(d / opts.dry_depth, 0, 1)`. The
ramp is applied to the source side (2D side when `Q > 0`, 1D side
when `Q < 0`). A hard wet/dry cutoff at `opts.dry_depth` would
introduce a step-discontinuity in `ydot` that breaks CVODE's BDF
corrector.

**4. Surcharge gate (C1/C2) — capped vs. uncapped nodes.**

- **C1 (effective-area widening).** Below `z_top` the exchange uses
  `A_inlet` (the user-supplied `[CD] [AREA]`). Above `z_top` the area
  widens to `A_manhole = 2 · A_inlet` over a 5 cm transition — the
  manhole bolt has yielded and water reaches the surface.
- **C2 (surcharge gate ramp).** The orifice flow is additionally
  multiplied by a direction-symmetric Hermite ramp on
  `surcharge_excess = max(h_1d, h_2d) − z_top`. For an uncapped node
  (`sur_depth = 0`) `z_top` reduces to the rim elevation, so the gate
  opens the instant water reaches the rim and the ramp is a no-op
  thereafter. For a capped node (`sur_depth > 0`) the gate stays
  shut until either side rises above the cap.

```
A_eff   = effectiveArea(h_max, z_top, full_depth, A_inlet, A_inlet * 2)
capRamp = hermite( (h_max - z_top) / 0.05 )
Q       = Q * capRamp
```

**5. Volume throttle when the 1D node is full (Q > 0 only).** Drain
flow into a node already at full volume is capped at
`(full_volume − volume) / dt`. If the node has zero remaining capacity,
Q is allowed only when it still represents surcharge drain-back
(`h_1d ≥ h_2d` → set Q = 0; otherwise pass it through so a
pressurised pipe can spill out).

```
available = (nodes.full_volume[ni] - nodes.volume[ni]) * opts.vol_1d_to_2d;
if (Q > 0 && available > 0 && dt > 0) {
    Q = std::min(Q, available / dt);
} else if (Q > 0 && available <= 0 && h_1d >= h_2d) {
    Q = 0.0;   // node full; no drain-in allowed
}
```

**6. Inject — both sides of the bridge.**

```cpp
// 1D side — drained into routing_external / routing_flooding next step.
// SI Q (m³/s) → 1D units (ft³/s for US).
nodes.coupling_inflow[ni] += Q * opts.flow_2d_to_1d;

// 2D side — sink for the cell beneath this coupling point.
state.coupling_flux[ci]  += -Q / mesh.tri_area[ci];   // m/s
```

`coupling_flux` accumulates over multiple coupling points sharing the
same cell. The CVODE RHS picks it up as the per-cell source/sink.

### 6.3 Coupling at Outfalls — Dynamic Tailwater + Flap Gates

Outfalls take two distinct paths through `NodeCoupling.cpp`:

**Pre-routing (`updateOutfallBoundaries`).** Cache the 2D head at the
outfall coupling cell into `nodes.outfall_2d_head[ni]` so
`Outfall::setAllOutfallDepths` can apply
`max(h_standard, h_2d)` on every Picard iteration. The cached value
is in 1D units (`× opts.len_2d_to_1d`) because the consumer compares
against `h_standard` in feet for US projects.

```cpp
double depth_2d = h_2d - bed_z;
if (depth_2d > 1.0e-4) {                 // wet — 0.1 mm threshold
    nodes.outfall_2d_head[ni] = h_2d * opts.len_2d_to_1d;
} else {
    nodes.outfall_2d_head[ni] = -1.0e30; // sentinel — no override
}
```

The dry-mesh sentinel is essential: a naïve check `h_2d > z_inv`
inside `setAllOutfallDepths` is true whenever `bed_z > z_inv`, which
is the common physical case (outfall pipe enters underground beneath
the surface mesh). Without the sentinel, every dry outfall would
fire the override and the 2D bed elevation would be backflowing into
1D.

**Post-routing (`transferOutfallDischarges`).** The 1D outflow
computed during routing is injected as a source on the outfall's
coupling cell:

```cpp
double Q_outfall = nodes.outflow[ni] * opts.flow_1d_to_2d;  // ft³/s → m³/s
if (Q_outfall > 0.0) {
    state.coupling_flux[ci] += Q_outfall / mesh.tri_area[ci];  // m/s source
}
```

Flap-gate logic lives inside `setAllOutfallDepths` itself (not in
`NodeCoupling.cpp`) so it can see the just-computed `h_standard` for
the iteration. `updateOutfallBoundaries` caches the raw `h_2d`; the
gate decision happens at consumption time.

### 6.4 Coupling Point Construction

`buildCouplingPoints(mesh, ctx)` is run once at the end of
`SurfaceRouter2D::initialize`. For each resolved
`vert_coupled_node[v] ≥ 0` it creates a `CouplingPoint` with the
vertex index, a representative containing-triangle index (first hit
wins), the per-vertex Cd / area from `[2D_VERTEX_NODE_MAP]`, and the
outfall flags from `ctx.nodes.type` / `outfall_has_flap_gate`. The
triangle-centroid path is identical but uses `tri_coupled_node[t]`
and `tri_coupling_*`.

### 6.5 Coupling Sequence per Routing Step

This is the actual sequence orchestrated by `SurfaceRouter2D` and
`SWMMEngine`. Compare with the older revisions of this section — they
listed a forcing-API path that no longer exists.

```
Pre-routing  (SurfaceRouter2D::updateOutfallsPreRouting):
  1. For each outfall coupling point, cache state.head[cell] into
     ctx.nodes.outfall_2d_head[ni].  Outfall::setAllOutfallDepths
     then applies max(h_standard, h_2d) inside the 1D Picard loop.

1D routing  (SWMM core, unchanged):
  2. assembleLateralInflows drains nodes.coupling_inflow[] from the
     previous step into routing_external / routing_flooding.
  3. DW solver advances by dt.
  4. nodes.head / depth / volume / outflow updated.

Post-routing  (SurfaceRouter2D::advancePostRouting):
  5. computeCouplingExchange — per coupling point, build orifice Q
     with wet/dry ramp + surcharge gate + volume throttle, deposit
     signed Q into ctx.nodes.coupling_inflow[ni] (× flow_2d_to_1d)
     and into state.coupling_flux[ci] (m/s sink, negated).
  6. transferOutfallDischarges — push outfall outflow back into the
     mesh as a positive coupling_flux source.
  7. updateRainfall — read ctx.gages.rainfall, convert to m/s, apply
     any C-API rainfall overrides.
  8. cvode_solver_.advance(t, t+dt, state, mesh) — BDF + GMRES /
     JACOBI inner solve, picks up coupling_flux + rainfall as
     per-cell source terms.
  9. update_statistics + accumulateMassBalance.
```

Operator-splitting note: the 1D routing step in (2)–(4) uses the
*previous* step's `coupling_inflow`, while the 2D advance in (8) uses
the *current* step's `coupling_flux`. This is a first-order operator
split. Mass conservation is maintained because the signed Q is
recorded on both sides at the same instant (step 5), and the 1D side
only picks it up after a full `dt` has elapsed.

---

## 7. Rainfall

### 7.1 Phase 1: System Rainfall

Each 2D triangle receives rainfall from the nearest rain gage (or a user-specified gage assignment):

```cpp
// Simple: use first available gage's current rainfall for all cells
double rain_intensity = ctx.gages.rainfall[0]; // user units (in/hr or mm/hr)
double rain_m_per_s = convert_to_m_per_s(rain_intensity, ctx.options);

for (int i = 0; i < mesh.n_triangles(); ++i) {
    state.rainfall[i] = rain_m_per_s;
}
```

### 7.2 Future: Natural Neighbour Interpolation

For spatially distributed rainfall across multiple gages:

1. Compute Voronoi diagram of gage locations
2. For each triangle centroid, find its natural neighbour weights among gages
3. Interpolate rainfall intensity using natural neighbour weights

This provides smooth, data-adaptive spatial interpolation that:
- Reproduces exact values at gage locations
- Provides C1-continuous interpolation between gages
- Adapts automatically to irregular gage spacing

Implementation will use the gage coordinates from `ctx.spatial.gage_x/y` and rainfall from `ctx.gages.rainfall[]`.

---

## 8. Integration into the Engine

This section reflects how the 2D module is actually wired into
`SWMMEngine`. It does **not** match earlier revisions that placed
`MeshData` / `SurfaceStateData` directly on `SimulationContext`.

### 8.1 Ownership

2D state lives on a dedicated `openswmm::twoD::SurfaceRouter2D`
member of `SWMMEngine`, not on `SimulationContext`. The router owns:

```cpp
class SurfaceRouter2D {
    MeshData         mesh_;
    SurfaceStateData state_;
    SolverOptions2D  options_;
    BoundaryData     boundary_;
    std::vector<PendingBoundaryRow> pending_bc_rows_;   // parse-time scratch
    std::vector<CouplingPoint>      coupling_points_;
    bool active_ = false;
#ifdef OPENSWMM_HAS_2D
    CvodeSurfaceSolver cvode_solver_;
#endif
    // ...
};
```

The mesh / state / options are exposed via `mesh()`, `state()`,
`options()`, `boundary()`, `pendingBCRows()` accessors so input
parsers and the C API can populate them by reference.

`SimulationContext` carries no 2D-specific fields — it stays
unit-naïve and unaware of the 2D module. The 2D module reads from
`ctx.nodes`, `ctx.gages`, `ctx.options.flow_units`, and
`ctx.node_names`; it writes back into `ctx.nodes.coupling_inflow[]`
and `ctx.nodes.outfall_2d_head[]` (both pre-existing fields used by
SWMM's routing and outfall code).

### 8.2 Section Registration

`openswmm::twoD::register2DSections` (in
`src/engine/2d/input/SectionHandlers2D.cpp`) wires all seven `[2D_*]`
sections into the `DefaultInputPlugin` registry. The wrappers use a
`makeSectionHandler` lambda factory that tokenises each body line and
delegates to the per-line `parse2D…Line` functions, surfacing errors
through `ctx.error_message`.

```cpp
#ifdef OPENSWMM_HAS_2D
if (auto* dip = dynamic_cast<DefaultInputPlugin*>(input_plugin)) {
    twoD::register2DSections(surface_router_.mesh(),
                             surface_router_.options(),
                             surface_router_.pendingBCRows(),
                             dip->registry());
}
#endif
```

The registered set: `2D_OPTIONS`, `2D_VERTICES`, `2D_TRIANGLES`,
`2D_VERTEX_NODE_MAP`, `2D_TRIANGLE_NODE_MAP`,
`2D_BOUNDARY_CONDITIONS`, `2D_MESH_FILE`. `2D_MESH_FILE` only
captures the `FILE <path>` token into `options.mesh_file`; the
external file is loaded later (see §8.3 step 4).

### 8.3 Engine Lifecycle — `SWMMEngine::open`

Real call sequence under `#ifdef OPENSWMM_HAS_2D` (paraphrased from
`src/engine/core/SWMMEngine.cpp` ~L120–L210):

```
1. register2DSections(mesh_, options_, pending_bc_rows_, dip->registry())
2. prescan2DUnitsHeader(inp_path, options_)                  // sets mesh_units_si
3. input_plugin->read(inp_path, ctx_)                        // parses all sections
4. if (options_.mesh_file non-empty):
      load2DMeshExternalFile(mesh_, options_, pending_bc_rows_,
                              mesh_file, base_dir)
         └─ prescan2DUnitsHeader(resolved_path, options_)    // external overrides inline
         └─ second InputReader pass on the resolved .2dm
5. resolve_cross_references(ctx_)
6. if (options_.output_file non-empty):
      add_output_plugin(new Default2DOutputPlugin(resolved_path))
```

### 8.4 Engine Lifecycle — `SurfaceRouter2D::initialize`

Called from `SWMMEngine::initialize` after the 1D side is initialised
(`src/engine/2d/SurfaceRouter2D.cpp`):

```
1. Guard:  if (n_vertices() < 3 || n_triangles() < 1) → active_ = false; return.
2. Compute unit bridge:
       ft_to_m       = (US FLOW_UNITS) ? 0.3048 : 1.0
       len_1d_to_2d  = ft_to_m
       len_2d_to_1d  = 1.0 / ft_to_m
       vol_1d_to_2d  = ft_to_m^3
       flow_1d_to_2d = ft_to_m^3
       flow_2d_to_1d = 1.0 / ft_to_m^3
3. Optional mesh ft→m scaling — only if (!mesh_units_si && len_1d_to_2d != 1):
       vx, vy, vz       *= ft_to_m
       vert_coupling_area, tri_coupling_area *= ft_to_m^2
4. buildMeshTopology(mesh_)                  // neighbours, edges, areas
5. validateMesh(mesh_)                        // throws on degenerate input
6. buildVertexStencils(mesh_)                 // pseudo-Laplacian weights
7. Resolve deferred coupling node names:
       vert_coupled_node[v] = ctx.node_names.find(vert_coupled_node_name[v])
       tri_coupled_node[t]  = ctx.node_names.find(tri_coupled_node_name[t])
   Unknown name throws.
8. state_.resize(n_triangles, n_vertices)
9. boundary_.resize(n_triangles * 3)          // all WALL defaults
10. Drain pending_bc_rows_ into boundary_:
        for each row → set edge_bc_type and the type-specific param
        (slope for NORMAL_FLOW, head for SPECIFIED_STAGE, etc.)
        Out-of-range rows silently skipped.
10a. §11A — drain pending_edge_conveyance_rows_ into
        mesh_.edge_conveyance.  Builds a one-shot vertex-pair → slot
        map (O(n_triangles)), then writes each parsed factor into
        every slot the (FROM, TO) pair resolves to (interior = 2
        slots, boundary = 1).  Out-of-range vertices or non-existent
        edges throw a runtime_error.
11. Set initial heads: state_.head[i] = mesh_.tri_cz[i]   (dry bed)
12. coupling_points_ = buildCouplingPoints(mesh_, ctx)
13. For each coupled node: warn on sur_depth > 0 / ponded_area > 0
    and force ponded_area = 0 (C3a).
14. cvode_solver_.initialize(mesh_, state_, options_)
15. active_ = true
16. Seed 2D mass-balance init_storage from the initial cell volumes.
```

### 8.5 Engine Lifecycle — per step / finalize

```
step(ctx, dt, t):
   1. updateOutfallsPreRouting(ctx)
      └─ (SWMM core runs assembleLateralInflows + DW routing here)
   2. advancePostRouting(ctx, dt, t)
      └─ computeCouplingExchange
      └─ transferOutfallDischarges
      └─ updateRainfall
      └─ cvode_solver_.advance(t, t+dt, state_, mesh_)
      └─ state_.update_statistics + accumulateMassBalance
      └─ state_.clear_reset_forcings()

finalize():
   1. cvode_solver_.finalize()
   2. active_ = false
```

---

## 9. Timestep Synchronization

The 2D solver and the 1D SWMM solver operate on different timescales and must be carefully synchronized. CVODE internally sub-steps within the SWMM routing interval, while coupling exchange must remain consistent across both solvers.

### 9.1 Two-Clock Architecture

```
SWMM clock:  |----dt_swmm----|----dt_swmm----|----dt_swmm----|
             t0              t1              t2              t3

CVODE clock: |--Δt--|--Δt--|--Δt--|-Δt-|--Δt--|--Δt--|--Δt--|
             t0                    t1                        t2

             CVODE sub-steps internally to reach each t_swmm boundary
```

- **SWMM routing step (`dt_swmm`):** Determined by `TimestepController::compute_next()` as the minimum of CFL, output boundary, control events, and user max step. Typically 1–30 seconds.
- **CVODE internal steps:** Variable-order, variable-step BDF steps taken internally by CVODE. CVODE is called with `CVode(cvode_mem, t_target, ...)` where `t_target = t_current + dt_swmm`. CVODE sub-steps as needed to meet error tolerances, but guarantees arrival at `t_target` exactly.

### 9.2 Synchronization Modes

Two modes are supported, controlled by `coupling_interval` in `[2D_OPTIONS]`:

#### Mode A: Tight Coupling (default, `coupling_interval = 0`)

The 2D solver advances **every SWMM routing step**. This is the most accurate but most expensive mode.

```
for each SWMM routing step dt_swmm:
    1. SWMM computes dt_swmm via TimestepController::compute_next()
    2. SWMM saves state, applies forcings, advances 1D routing by dt_swmm
    3. TimestepController::advance(ctx, dt_swmm)
    4. Read SWMM node heads at t_new
    5. Compute coupling exchange Q via orifice equation
    6. Update 2D rainfall from current gage state
    7. Set coupling_flux[] in 2D state (held constant over CVODE sub-steps)
    8. CVODE advances 2D from t to t + dt_swmm (internal sub-stepping)
    9. Inject exchange Q into nodes.coupling_inflow[ni] (× flow_2d_to_1d)
       for next SWMM step's assembleLateralInflows drain
    10. If output_due: snapshot includes both 1D and 2D state
```

#### Mode B: Subcycled Coupling (`coupling_interval = N`)

The 2D solver advances every `N` SWMM routing steps. Coupling exchange is computed once per `N` steps and held constant. This reduces computational cost for cases where the 2D domain evolves slowly relative to the pipe network.

```
coupling_counter = 0

for each SWMM routing step dt_swmm:
    1. SWMM routing step (normal)
    2. coupling_counter++
    3. if coupling_counter >= N:
        a. Compute accumulated dt_2d = sum of last N dt_swmm values
        b. Read SWMM node heads
        c. Compute coupling exchange
        d. CVODE advances 2D from t to t + dt_2d
        e. Inject exchange Q into nodes.coupling_inflow[ni]
           (× flow_2d_to_1d)
        f. coupling_counter = 0
```

### 9.3 Coupling Exchange Timing — Operator Splitting

The coupling uses **sequential operator splitting** (Lie splitting):

```
t_n → t_{n+1}:
    Step 1:  Advance SWMM 1D:    y^{1D}_{n+1} = S_{1D}(dt, y^{1D}_n, Q^{2D→1D}_n)
    Step 2:  Read h^{1D}_{n+1}
    Step 3:  Compute Q^{exchange}_{n+1} = orifice(h^{2D}_n, h^{1D}_{n+1})
    Step 4:  Advance 2D surface:  y^{2D}_{n+1} = S_{2D}(dt, y^{2D}_n, Q^{exchange}_{n+1})
    Step 5:  Set Q^{2D→1D}_{n+1} for next SWMM step
```

The exchange flow computed at Step 3 uses the **latest SWMM head** (post-routing) but the **previous 2D head** (pre-advance). This is first-order in time for the coupling but avoids implicit coupling iterations. For small `dt_swmm` (as enforced by CFL), the splitting error is small.

**Alternative (future):** Strang splitting (half-step 1D → full-step 2D → half-step 1D) would give second-order coupling accuracy but requires two 1D half-steps per coupling cycle.

### 9.4 Interaction with TimestepController

The 2D solver does **not** modify `TimestepController::compute_next()`. Instead:

1. **CFL from 2D** can optionally constrain `dt_swmm`. The 2D solver computes a CFL-like stability estimate:
   ```
   dt_cfl_2d = min over all cells: (cell_diameter / max_wave_speed)
   ```
   This is passed as an additional constraint:
   ```cpp
   double dt_cfl_1d = dynwave.compute_cfl_step(ctx);
   double dt_cfl_2d = ctx.has_2d ? surface_router.compute_cfl_hint(ctx) : 1e30;
   double dt_cfl = std::min(dt_cfl_1d, dt_cfl_2d);
   double dt_next = TimestepController::compute_next(ctx, dt_cfl);
   ```
   Note: Since CVODE handles its own sub-stepping adaptively, this CFL hint is **advisory** — it prevents the coupling interval from being too large, not the internal CVODE steps.

2. **Output alignment** is unchanged. Both 1D and 2D states are snapshotted when `TimestepController::output_due()` returns true, since the 2D solver has already been advanced to the same time.

3. **Simulation end** is unchanged. When `TimestepController::simulation_complete()` returns true, the 2D solver finalizes.

### 9.5 CVODE Internal Stepping Details

CVODE is configured to:

```cpp
// Set CVODE to stop exactly at t_target (no overshooting)
CVodeSetStopTime(cvode_mem, t_target);

// Advance — CVODE takes as many internal steps as needed
int flag = CVode(cvode_mem, t_target, y, &t_reached, CV_NORMAL);
// t_reached == t_target (guaranteed by SetStopTime)
```

Key CVODE settings:
- **Method:** BDF (backward differentiation formula) — appropriate for stiff systems
- **Max internal steps:** Configurable, default 500 per call (CVODE's default)
- **Min/max internal step size:** From `[2D_OPTIONS]` `MIN_TIMESTEP` / `MAX_TIMESTEP`
- **Order:** Dynamically adjusted by CVODE between 1 and 5 for optimal efficiency
- **Error control:** Per-cell relative + absolute tolerance

During each CVODE internal step, the coupling flux and rainfall are **held constant** (they are frozen at the values computed at the start of the coupling interval). This is consistent with the operator-splitting approach.

### 9.6 Mass Conservation at the Coupling Interface

To ensure global mass conservation across the 1D↔2D boundary:

```
Volume removed from 2D = Volume added to 1D (and vice versa)
```

The same `Q_exchange` value (in m³/s on the 2D side) is:
- Subtracted from the 2D cell as `state.coupling_flux[i] += -Q / A_i`
  (m/s sink) — see §6.2 step 6.
- Added to the SWMM node as
  `nodes.coupling_inflow[j] += Q * opts.flow_2d_to_1d` (m³/s in SI
  projects, ft³/s in US projects). On the next routing step,
  `assembleLateralInflows` drains the signed value into
  `routing_external` (positive part) and `routing_flooding`
  (negative part, |Q|), preserving global continuity.

Both use the same `dt_swmm` interval. The CVODE solver integrates the
coupling flux as a constant source/sink over its internal sub-steps,
which preserves the total volume exchange. `coupling_inflow[ni]` is
reset to zero by `computeCouplingExchange` itself at the start of each
coupling cycle for every coupled junction, so there is no double-count
between cycles.

### 9.7 Handling Mismatched Timescales

| Scenario | Behaviour |
|----------|-----------|
| 2D is stiff (small CVODE steps) | CVODE takes many internal sub-steps within `dt_swmm`. No impact on SWMM step. |
| 2D CFL < SWMM CFL | `dt_cfl_2d` constrains `dt_swmm` via `compute_next()`. Both solvers use the smaller step. |
| SWMM step limited by output boundary | 2D solver advances to the same output boundary. Both snapshots are synchronized. |
| SWMM step limited by control rules | 2D solver advances to the control-event time. Coupling is recomputed at the new state. |
| 2D goes dry everywhere | CVODE converges in 1–2 internal steps (trivial RHS). Minimal overhead. |
| Large 2D domain, small pipe network | Use `coupling_interval > 0` to subcycle. 2D advances less frequently. |

---

## 10. Numerical Implementation Details

### 10.1 Dry Cell Handling

Cells with `depth < dry_depth` require special treatment to avoid division by zero in Manning's equation:

```cpp
inline double diffusive_conductance(double depth, double mannings_n,
                                     double grad_h_mag, double dry_depth) {
    if (depth < dry_depth) return 0.0;
    double denom = mannings_n * std::sqrt(std::max(grad_h_mag, 1e-12));
    return std::pow(depth, 2.0/3.0) / denom;
}
```

### 10.2 Upwind Flux Selection

The upwind function `UW[]` selects the cell from which flow exits through the edge:

```cpp
// For edge between cell L and cell R:
double h_L = state.head[L];
double h_R = (R >= 0) ? state.head[R] : h_boundary;

int upstream = (h_L >= h_R) ? L : R;
// Compute flux using upstream cell's depth and gradient
```

### 10.3 Slope Limiter (Jawahar-Kamath)

The continuously differentiable limiter (Eq. [23]–[24]):

```cpp
// g1, g2, g3 = squared L2 norms of unlimited gradients in cell and its neighbours
double eps2 = epsilon * epsilon;
double denom = g1*g1 + g2*g2 + g3*g3 + 3.0 * eps2;
double w1 = (g2*g3 + eps2) / denom;
double w2 = (g3*g1 + eps2) / denom;
double w3 = (g1*g2 + eps2) / denom;

grad_lim_x = w1 * grad_u1_x + w2 * grad_u2_x + w3 * grad_u3_x;
grad_lim_y = w1 * grad_u1_y + w2 * grad_u2_y + w3 * grad_u3_y;
```

When all three gradients are equal, the weights reduce to 1/3 each (no limiting).

### 10.4 C-Property Preservation

To satisfy the C-property (still water on non-flat bed produces zero flux), reconstruct **total head** at edges, not depth:

```cpp
double h_edge = h_center + r_dot_grad_h_limited;
double depth_edge = std::max(h_edge - z_edge, 0.0);
```

---

## 11. Future Extension Strategy

### 11.1 Subsurface Flow (Richards' Equation)

Add vertical prismatic layers below each triangle. The subsurface state vector extends the CVODE system:

```
y = [ψ_o(1), ..., ψ_o(N), ψ(1,1), ..., ψ(N,M)]
```

Where `ψ(i,m)` is the pressure head in triangle `i`, layer `m`. Coupling between surface and subsurface follows Eq. [4] and [14] from the paper.

**Data structure:** Add `SubsurfaceStateData` with per-layer arrays following the same SoA pattern.

### 11.2 Infiltration

Per-cell infiltration models (Green-Ampt, Horton, SCS Curve Number) computed as a source/sink term in the surface ODE:

```
Q_infil[i] = infiltration_model(depth[i], soil_params[i], t)
net_source[i] = rainfall[i] - Q_infil[i]
```

**Data structure:** Add infiltration parameters to `MeshData` or a new `InfiltrationData2D` SoA struct.

### 11.3 Evapotranspiration

Per-cell ET as a sink term, using the system-level ET rate from SWMM options:

```
Q_et[i] = min(et_rate, depth[i] / dt)
```

### 11.4 Snowmelt

Per-cell snowpack tracking following SWMM's existing snow model but applied to 2D cells. Would require a `SnowState2D` SoA struct.

### 11.5 Anisotropic Roughness

The formulation already supports full-tensor anisotropy (Eq. [28]–[30]). To enable:
- Add `aniso_k1`, `aniso_k2`, `aniso_angle` arrays to `MeshData`
- Modify flux calculation to use Eq. [30] instead of scalar conductance

---

## 11A. Edge Conveyance Factor — Implemented

Status: **IMPLEMENTED 2026-05-30.** Q1-Q6 resolved as follows (see
§11A.3 / §11A.10 for full discussion). This section is kept as the
authoritative reference for the feature; §1.9 documents the input
syntax, §2.1 documents the data field, and the C API surface is in
`include/openswmm/engine/openswmm_2d.h` as documented in §11A.7.

| Q | Decision |
|---|----------|
| Q1 (range)        | Strict `[0, 1]` clamp at parse + C-API time. Out-of-range raises an error. |
| Q2 (boundary edges) | Factor applies to non-Wall boundary edges once the V-E-FLUX slice lands. Today it has no effect on boundary edges (early-return in the flux loop). |
| Q3 (symmetry)     | Silent partner-slot mirroring inside `SurfaceRouter2D::initialize`. |
| Q4 (order)        | Conveyance multiplies LAST in `computeEdgeFluxes`, after the wet/dry Hermite shutoff. |
| Q5 (input format) | **5d** — SWMM-style `FROM_VERTEX TO_VERTEX CONVEYANCE` rows in `[2D_EDGE_CONVEYANCE]`. Order of From / To does not affect the value. |
| Q6 (C API)        | Mutable at runtime: `swmm_2d_get / set / reset_edge_conveyance`. |

This section lays out the design for a per-edge scalar in [0, 1] that
attenuates the diffusion-wave flux across that edge. The motivating
use case is "leaky" linear features in the floodplain: culverted
embankments, partially-permeable hedgerows, perforated fences,
buildings with garage openings, vegetation strips, internal weir
structures — any obstruction that reduces but does not eliminate
overland conveyance.

### 11A.1 Naming choice

"Leaky" is descriptive but unanchored to 2D-hydraulics convention.
Established names from the porosity-SWE literature (Sanders 2008;
Bruwier et al. 2017; Soares-Frazão; Guinot) and from groundwater /
civil-engineering practice:

| Candidate | Notes |
|-----------|-------|
| **`edge_conveyance` factor** *(recommended)* | "Conveyance" is the standard civil-engineering term for the capacity of a section to carry flow (Manning's `K = (1/n)·A·R^(2/3)`). A multiplicative `[0,1]` factor reads naturally as "this edge carries `c × 100%` of its physics-derived conveyance". Default `1.0` is intuitively "unrestricted". |
| `edge_porosity` / `edge_transmissivity` ψ | Academic / IPSW (Integral-Porosity Shallow-Water) convention. `ψ = 1` is unobstructed; `ψ = 0` is wall. Most precedent in the literature, but the symbol ψ collides with the depth symbol already used in this doc. |
| `edge_permeability` | Groundwater analog; suggests dimensional permeability `k` (m²) which it is not. Misleading. |
| `edge_blockage` (= `1 − conveyance`) | Inverts the polarity. Natural for "this edge is X% blocked", but defaults flip to 0.0 instead of 1.0 and the multiplication site becomes `flux *= (1 − blockage)`. Less surgical. |
| `edge_attenuation` | Polarity-correct but ambiguous about whether it's the loss fraction or the surviving fraction. |

**Recommendation:** adopt `edge_conveyance` as the field name,
`[2D_EDGE_CONVEYANCE]` as the section, `CONVEYANCE` as the per-row
token. Cross-reference the porosity-SWE term `ψ` in code comments for
academic readers.

### 11A.2 What it is and is not

| It IS | It IS NOT |
|-------|-----------|
| A static per-edge mesh property (set at parse time, immutable during a run unless the C API mutates it). | A boundary condition. The existing `[2D_BOUNDARY_CONDITIONS]` system handles domain-edge inflow/outflow types; the conveyance factor multiplies the *computed* interior flux. |
| Symmetric across the edge: if interior edge `(A,k)` is shared with `(B,k')`, both slots in the flat `[tri*3+edge]` array MUST carry the same factor — otherwise antisymmetry breaks and the FV scheme stops being conservative. | Direction-dependent. A 0.4 factor attenuates inflow and outflow identically. (Direction-asymmetric obstructions — e.g., flap-gated culverts — would require a separate signed-flux mechanism not in scope for this plan.) |
| Dimensionless, scaling the entire physical flux: `F_e_eff = c · F_e_physics`. | A discharge coefficient `C_d` on an orifice equation. The orifice equation is what `[2D_VERTEX_NODE_MAP]` uses for SWMM coupling (§6.2); this factor is for *overland* edge fluxes between cells. |
| Default 1.0 for every edge (no behavioural change unless the user opts in). | Required input. Sections, fields, and C API all stay backward-compatible. |

### 11A.3 Decision questions for review

These need answers before code starts (per CLAUDE.md §1).

- **Q1. Range.** You said `[0, 1]`. Confirm `> 1` is also forbidden — i.e., the factor cannot *amplify* conveyance. (Amplification ≥ 1 would imply preferential channels carrying more than the Manning equation predicts, which would be unphysical without a separate calibration story. Recommend hard-clamping to [0, 1] with a parse-time error.)
- **Q2. Boundary edges.** Today boundary edges are handled by the BC system and the flux-calculator early-returns `0.0` at `nbr < 0` before any conveyance multiplication would run. Should the conveyance factor also apply to *non-Wall* boundary edges once the V-E-FLUX slice lands (so a partially-blocked outflow weir-edge has both a `SPECIFIED_FLOW` BC and a 0.3 conveyance)? Recommend YES for orthogonality, but the question deserves an explicit decision because it widens the cross-feature interaction surface.
- **Q3. Symmetry enforcement.** When the user declares `TRI=A EDGE=k CONVEYANCE=0.4`, should the parser (a) silently mirror the value to the partner slot on neighbour `B`, (b) require the user to declare both slots and error if they disagree, or (c) accept either and let `SurfaceRouter2D::initialize` fill in the partner from a "first-touch wins" rule? Recommend (a) — silent mirroring keeps the input concise and removes a footgun. Mention (b) as a pedantic-validation toggle if needed later.
- **Q4. Interaction with the wet/dry Hermite shutoff.** Today the shutoff multiplies `F_e` *before* the value is stored. The conveyance factor would multiply *after* the shutoff (or before — both give the same result mathematically, but the code order matters for clarity). Recommend applying conveyance as the last step before `state.edge_flux[idx] = F_e`. This keeps a `c = 0` edge bit-identical to a wall and the multiplication is visibly "the last thing that happens to F_e".
- **Q5. Input-format granularity.** A `TRI EDGE CONVEYANCE` row and a
  `V_A V_B CONVEYANCE` row both have the shape `int int double`, so a
  bare two-form dispatch is ambiguous (the early draft of this plan
  got this wrong — see the discussion below). Three coherent options:
  - **5a (recommend) — TRI/EDGE only.** Engine accepts only
    `TRI EDGE CONVEYANCE`. Authoring an obstruction by polyline is
    the GUI's job: it knows the mesh, walks the polyline, looks up
    each shared interior edge, and emits the corresponding TRI/EDGE
    rows. Engine parser stays trivial; no ambiguity possible.
  - **5b — both forms with mandatory leading keyword.** Every row
    starts with `TRI` or `V`: `TRI 42 1 0.40` or `V 17 18 0.30`.
    Unambiguous, slightly more verbose, scales cleanly if a future
    form (e.g., `EDGE_ID 123 0.40` for a global edge ID) is added.
  - **5c — separate sections.** `[2D_EDGE_CONVEYANCE]` for the
    triangle/edge form, `[2D_EDGE_CONVEYANCE_VERTEX]` for the
    vertex-pair form. Each section's parser is unambiguous on its
    own. Two section names for one feature is awkward but workable.

  > **Why "both, dispatched by parse result" does not work.** Almost
  > every form-A row has `EDGE ∈ {0, 1, 2}`, which is also a valid
  > vertex index in any non-trivial mesh, so a row like `42 1 0.40`
  > could be either form. A row like `17 18 0.30` is unambiguously
  > form B only because `EDGE = 18` is out of range for form A — but
  > that signal only fires for the small minority of form-B rows
  > whose second vertex happens to be > 2. Most rows collide.
- **Q6. C API.** Mutable through the C API like `coupling_inflow[]`, or read-only like static mesh geometry? Recommend mutable — supports time-varying obstructions (e.g., a flap-gate opening over the course of a storm) at near-zero implementation cost, since the value is just a multiplier looked up every flux evaluation.

If any of Q1–Q6 lands differently, the §11A.4–11A.8 sections below
need to be revised before any code is written.

### 11A.4 Proposed data model

Single new field on `MeshData`, mirroring the existing edge SoA layout:

```cpp
struct MeshData {
    // ... existing fields ...

    // Per-edge conveyance factor in [0, 1] (default 1.0 = unrestricted).
    // Flat 2D: [tri * 3 + edge_local]. Symmetric across interior edges —
    // resize_triangles initialises to 1.0 and SurfaceRouter2D::initialize
    // mirrors any user-declared partial conveyance to the matching slot
    // on the neighbour triangle so interior flux antisymmetry is
    // preserved (see §11A.6 mass-conservation argument).
    //
    // Cross-reference: corresponds to ψ in the Integral-Porosity SWE
    // literature (Sanders 2008; Bruwier et al. 2017).
    std::vector<double> edge_conveyance;
};
```

`resize_triangles(nt)` adds `edge_conveyance.assign(nt * 3, 1.0);`
right next to the existing `edge_length.resize(...)` line.

No new struct, no new lifecycle method. The factor is mesh geometry,
so it belongs on `MeshData` rather than `BoundaryData` (which is for
edge boundary conditions) or `SurfaceStateData` (which is for time-
varying state).

### 11A.5 Proposed input format

New optional section `[2D_EDGE_CONVEYANCE]`. Parsed by a new
`parse2DEdgeConveyanceLine` function in
`src/engine/2d/input/SectionHandlers2D.cpp`, registered the same way
as the other `[2D_*]` sections in `register2DSections` and
`load2DMeshExternalFile`. Per Q5 → 5d, the row format mirrors SWMM
`[CONDUITS]` `From-Node` / `To-Node` convention.

```
;; Per-edge conveyance multiplier in [0, 1]. Default 1.0 (unrestricted)
;; for every interior edge that is NOT listed here.  Multiplies the
;; diffusion-wave flux across the edge.
;;
;; FROM_VERTEX / TO_VERTEX is the (unordered) pair of mesh vertex
;; indices at the edge's endpoints — the conveyance is direction-
;; symmetric, so swapping FROM and TO does not change the value.
;;
;; Authoring obstructions as a polyline (a hedgerow that crosses many
;; edges) is the GUI's job: walk the polyline, emit one row per shared
;; mesh edge.
[2D_EDGE_CONVEYANCE]

;; FROM_VERTEX   TO_VERTEX   CONVEYANCE
;; -----------   ---------   ----------
   17            18          0.40        ;; hedgerow segment
   18            19          0.40
   42            87          0.30        ;; culverted embankment
   55            61          0.00        ;; fully blocked → equivalent to a Wall
```

Parser rules (5d):

- Exactly three tokens per row: `FROM_VERTEX TO_VERTEX CONVEYANCE`.
- `FROM_VERTEX` and `TO_VERTEX` must be non-negative integers in
  `[0, n_vertices)` and must differ. Out-of-range or equal vertices
  raise a clear error at parse time (range) or at
  `SurfaceRouter2D::initialize` time (vertex-pair does not form a
  real mesh edge).
- `CONVEYANCE` must parse as a `double` in `[0.0, 1.0]` (Q1, strict
  clamp at parse time); out-of-range raises a clear error.
- Vertex-pair resolution (Q3 silent mirror) happens in
  `SurfaceRouter2D::initialize`: an edge-key hash map built in one
  O(n_triangles) pass over the mesh maps `(min(v_from, v_to),
  max(v_from, v_to))` → list of `(tri, edge_local)` slot indices.
  An interior edge resolves to two slots (one in each adjacent
  triangle); a boundary edge resolves to one. The factor is written
  to every matching slot.
- Duplicate rows naming the same edge: last-write-wins, with a
  one-shot warning per duplicate.
- A vertex-pair that does not form a mesh edge raises a fatal
  init-time error (the obstruction reference is wrong; fail loudly
  rather than silently dropping).

**Alternative parsers (not implemented, retained for reference).**

- **5a (TRI / EDGE explicit).** `TRI EDGE CONVEYANCE` per row. Author
  must know triangle indices.
- **5b (leading keyword).** First token is `TRI` or `V`; rest of the
  row is `<int> <int> <double>` interpreted accordingly.
- **5c (separate sections).** Sibling
  `[2D_EDGE_CONVEYANCE]` (TRI/EDGE) and
  `[2D_EDGE_CONVEYANCE_VERTEX]` (V_A/V_B) sections.

### 11A.6 Proposed math integration

Single insertion in `SurfaceFluxCalculator::computeEdgeFluxes` —
multiply once, immediately before the flux is stored:

```cpp
// Cubic Hermite wet/dry shutoff on the source-side depth (unchanged) …
if (depth_up < opts.dry_depth) {
    double t = depth_up / opts.dry_depth;
    F_e *= t * t * (3.0 - 2.0 * t);
}

// NEW — per-edge conveyance factor.  No-op when c == 1.0.
//
// Mass-conservation argument:
//   - For an interior edge shared by cells A and B, this loop writes
//     edge_flux[A*3 + e_A] and edge_flux[B*3 + e_B] independently.
//     The two slots compute the SAME F_e (up to sign) because the FV
//     scheme uses centroid-to-centroid Δh / Δx, which is antisymmetric
//     in (A ↔ B).  Multiplying BOTH slots by the SAME edge_conveyance
//     preserves antisymmetry → no spurious mass source.
//   - SurfaceRouter2D::initialize is responsible for the "same factor
//     in both slots" invariant (§11A.4 mirroring).  computeEdgeFluxes
//     trusts it and does not re-look up the partner.
//   - c == 0 → F_e == 0, identical to the boundary early-return.  An
//     interior edge with conveyance 0 is a wall in everything but its
//     storage location (still in the interior edge list, still has a
//     neighbour, but carries no flux).
F_e *= mesh.edge_conveyance[idx];

state.edge_flux[idx] = F_e;
```

That is the entire mathematical change. The downstream consumers
(`assembleRHS`, `face_vx / face_vy` reconstruction,
`cell_continuity_err`, `edge_bc_cum_flux`) see the attenuated flux
and integrate it correctly without further modification.

CVODE Jacobian: the diffusion-wave Jacobian is currently approximated
by GMRES Jacobian-vector products with no explicit assembly. A
per-edge constant multiplier does not change the sparsity pattern,
just the coefficient, so the existing Krylov + JACOBI preconditioner
keeps working. The conditioning is *better* on heavily-blocked
meshes (smaller off-diagonal entries → more diagonally dominant).

### 11A.7 Proposed C API surface

Three new functions in `include/openswmm/engine/openswmm_2d.h`,
matching the style of the existing `swmm_2d_set_coupling_*` family:

```c
/* Read the per-edge conveyance factor for one edge.
 * tri ∈ [0, n_triangles), edge ∈ {0,1,2}.
 * Returns 1.0 for out-of-range arguments and emits a warning.
 */
double swmm_2d_get_edge_conveyance(SWMM_Engine eng, int tri, int edge);

/* Set the per-edge conveyance factor for one edge.
 * Value is clamped to [0, 1]; out-of-range arguments are no-ops with
 * a warning.  When the edge is interior (has a neighbour) the value
 * is mirrored to the partner slot so antisymmetry is preserved.
 * Safe to call between routing steps; calling DURING a routing step
 * is undefined (the CVODE sub-stepper holds a const reference).
 */
int swmm_2d_set_edge_conveyance(SWMM_Engine eng, int tri, int edge, double c);

/* Bulk reset: every edge → 1.0.  O(n_triangles). */
int swmm_2d_reset_edge_conveyance(SWMM_Engine eng);
```

Cython / Python bindings follow the existing pattern in
`python/openswmm/engine/_2d.pxd / _2d.pyx`.

### 11A.8 Validation plan

| Step | Verification |
|------|--------------|
| 1. `edge_conveyance` defaults to 1.0 after `resize_triangles` | Unit test on `MeshData` directly. |
| 2. Parser round-trips form A and form B | Unit test on `parse2DEdgeConveyanceLine` with both syntaxes; assert the same internal state. |
| 3. Out-of-range value → parse error | Test with `CONVEYANCE = -0.1` and `CONVEYANCE = 1.5`; assert non-empty error. |
| 4. Mirroring across interior edges | After `SurfaceRouter2D::initialize`, walk every interior edge and assert `edge_conveyance[A*3+e_A] == edge_conveyance[B*3+e_B]`. |
| 5. `c = 0` ≡ Wall | Generate a test case where one interior edge is fully blocked; compare against the same case with that edge moved to the boundary (`tri_nbr = -1`). Depth and flux fields must agree to solver tolerance. |
| 6. Partial attenuation | Steady-state diffusion problem with an analytical solution; verify the flux through a `c = 0.5` edge is exactly half the unattenuated reference. |
| 7. Mass balance | A 12-hour rainfall run with a mixed-conveyance domain (some edges at 1.0, some at 0.3, some at 0.0); assert `accumulateMassBalance` continues to balance within the existing tolerance (no spurious source / sink from antisymmetry breaking). |
| 8. C API round-trip | `set` → `get` returns the clamped value; `set` on an interior edge mirrors to the partner. |

### 11A.9 Affected files (forecast)

If Q1–Q6 land per recommendation:

- `src/engine/2d/data/MeshData.hpp` — add `edge_conveyance` vector, init in `resize_triangles`.
- `src/engine/2d/input/SectionHandlers2D.hpp / .cpp` — add `parse2DEdgeConveyanceLine`; register in `register2DSections` and `load2DMeshExternalFile`.
- `src/engine/2d/SurfaceRouter2D.cpp` — mirror form-A / form-B declarations to partner slots inside `initialize()`, immediately after `buildMeshTopology` (the neighbour table is then available).
- `src/engine/2d/solver/SurfaceFluxCalculator.cpp` — one-line multiplication before `state.edge_flux[idx] = F_e`.
- `include/openswmm/engine/openswmm_2d.h` + impl — three new C API entry points.
- `python/openswmm/engine/_2d.pxd / _2d.pyx` — Cython / Python bindings.
- `tests/` — unit + verification + mass-balance tests per §11A.8.
- `docs/2dModelStrategy.md` §1 — once approved, promote this plan into §1.9 `[2D_EDGE_CONVEYANCE]` and a §2.5 `MeshData` field; flip the status banner above to "implemented".
- `docs/2d_external_mesh_file.md` — extend the affected-sections list with `[2D_EDGE_CONVEYANCE]`.

Estimated diff size: ~150 lines of engine source, ~80 lines of tests,
~60 lines of docs. No new dependencies.

### 11A.10 Decision log

Q1-Q6 resolved 2026-05-30 — see status banner at the top of §11A.
Q5 changed from the original 5a recommendation to 5d after the
ambiguity in the bare two-form dispatch was identified and the
SWMM-style `FROM_VERTEX` / `TO_VERTEX` convention was proposed as a
cleaner alternative.

---

## 12. Testing Strategy

### 12.1 Unit Tests

| Test | Validates |
|------|-----------|
| `test_mesh_builder` | Topology, neighbours, edge normals, areas |
| `test_vertex_reconstruction` | Pseudo-Laplacian weights sum to 1, linear exactness |
| `test_gradient_calculation` | Green-Gauss gradients exact for linear fields |
| `test_slope_limiter` | Reduces to 1/3 weights for uniform gradients |
| `test_diffusive_conductance` | Correct K(ψ) values, dry cell handling |
| `test_orifice_coupling` | Correct Q for various head differences |
| `test_backflow_prevention` | Zero flow when flap gate active and h_swmm > h_2d |
| `test_input_parsing` | Correct parse of all 2D sections |
| `test_section_registration` | Custom sections registered and dispatched |

### 12.2 Verification Tests (from Kumar et al., 2009)

| Test | Reference | Description |
|------|-----------|-------------|
| Still water (C-property) | — | Zero flux on non-flat bed with uniform head |
| Tilted plane | Analytical | Steady-state flow on constant-slope plane |
| Dam break | Analytical | 1D dam break on flat bed (Ritter solution) |
| Abdul & Gillham (1984) | Lab data | Coupled hillslope surface-subsurface flow (Phase 2) |

### 12.3 Benchmark Tests

| Benchmark | Purpose |
|-----------|---------|
| `bench_flux_calculation` | Throughput of edge flux computation |
| `bench_vertex_reconstruction` | Stencil evaluation performance |
| `bench_cvode_advance` | Full solver step timing |

---

## 13. Lateral Exchange — Uncapped Nodes and Surcharge Feedback

### 13.1 Problem Statement

When the 1D pipe network surcharges, water rises above the node crown and "caps" at the ground surface in a conventional SWMM simulation (ponding or flooding). With a coupled 2D surface model, **uncapped nodes** must allow bidirectional exchange: surcharge water spills onto the 2D surface, and 2D overland flow can drain back into the pipe network when capacity is available.

The challenge is ensuring **consistent, mass-conservative, numerically stable feedback** between the 1D node head (which can exceed ground elevation during surcharge) and the 2D surface head at the coupling point.

### 13.2 Node Classification for 2D Coupling

Each coupled SWMM node falls into one of these categories:

| Category | Condition | 2D Exchange Behaviour |
|----------|-----------|----------------------|
| **Sub-surface** | `h_1D < z_ground` | Normal orifice exchange; 2D surface can drain into node |
| **At-grade** | `h_1D ≈ z_ground` | Transition zone; exchange approaches zero as heads equalize |
| **Surcharged (uncapped)** | `h_1D > z_ground` | Surcharge spills onto 2D surface; bidirectional exchange |
| **Flooded (capped, no 2D)** | `h_1D > z_ground`, no 2D coupling | Legacy SWMM flooding/ponding (unchanged) |

For coupled nodes, the `ponded_area` parameter is effectively replaced by the 2D surface domain — water that would pond in 1D instead flows onto the 2D mesh.

### 13.3 Uncapped Node Exchange Equation

The orifice exchange equation from §6.2 is extended for uncapped nodes:

```
Q_exchange = C_d · A_eff(h) · sign(Δh) · sqrt(2g · |Δh|)
```

Where `A_eff(h)` is a **head-dependent effective area** that transitions smoothly between regimes:

```cpp
double effective_area(double h_1d, double h_2d, double z_ground,
                      double z_invert, double A_inlet, double A_manhole) {
    double h_max = std::max(h_1d, h_2d);
    
    if (h_max < z_ground) {
        // Sub-surface: flow through inlet grate/opening
        return A_inlet;
    } else {
        // Surcharged: full manhole opening area
        // Smooth transition over a small depth range
        double d_trans = 0.05;  // 5 cm transition depth
        double frac = std::min((h_max - z_ground) / d_trans, 1.0);
        return A_inlet + frac * (A_manhole - A_inlet);
    }
}
```

### 13.4 Surcharge Spill Dynamics

When `h_1D > z_ground + ψ_2D` (1D surcharge head exceeds 2D surface head):

1. **Spill flow** is computed via the orifice equation with `Δh = h_1D - h_2D`
2. The spill is injected as a **negative coupling flux** into the 2D cell: `coupling_flux[i] = +Q / A_tri`
3. The same flow is removed from the 1D node via `forcing.node_lat_inflow -= Q`

When `h_2D > h_1D` (2D surface head exceeds 1D node head):

1. **Return flow** drains from 2D surface back into the pipe network
2. Subject to capacity: if the node is full (`volume >= full_volume`), return flow is throttled
3. Throttling: `Q_return = min(Q_orifice, (full_volume - volume) / dt)`

### 13.5 Ponding Suppression for Coupled Nodes

For nodes coupled to the 2D domain, the engine must **suppress the default SWMM ponding/flooding behaviour**:

```cpp
// In initNodeFlows(), skip overflow computation for 2D-coupled nodes
if (is_2d_coupled[i]) {
    nodes.overflow[i] = 0.0;  // 2D surface handles the excess
    // Do NOT cap depth at full_depth for coupled nodes
} else {
    // Standard SWMM overflow logic
    if (nodes.volume[ui] > nodes.full_volume[ui] && dt > 0.0) {
        nodes.overflow[ui] = (nodes.volume[ui] - nodes.full_volume[ui]) / dt;
    }
}
```

This is critical: without suppression, the 1D solver would flood water that should instead exchange with the 2D surface, causing double-counting.

### 13.6 Head Clamping Strategy

To prevent numerical instability during surcharge, the 1D node head for coupled nodes is allowed to **exceed `z_ground + full_depth`** — the 2D surface acts as the "cap" instead of the ponded area:

```cpp
// In DWSolver: for 2D-coupled surcharged nodes, do NOT clamp head
if (is_2d_coupled[node_idx]) {
    // Head is free to rise — 2D coupling will drain excess
    // Still enforce a safety maximum to prevent runaway
    double safety_max = z_ground + 2.0 * full_depth;
    nodes.head[node_idx] = std::min(nodes.head[node_idx], safety_max);
}
```

---

## 14. Outfall Boundary Feedback with 2D Surface

### 14.1 Problem Statement

Outfall nodes define downstream boundary conditions for the 1D pipe network. When a 2D surface domain is present, **outfalls at the domain boundary** must account for the 2D water level as a dynamic boundary condition rather than using a fixed or tidal stage.

### 14.2 Outfall Types and 2D Interaction

| Outfall Type | Without 2D | With 2D Coupling |
|-------------|-----------|-----------------|
| **FREE** | `h = z + min(yNorm, yCrit)` | 2D head at outfall vertex; if 2D head > critical depth, use 2D head as tailwater |
| **NORMAL** | `h = z + yNorm` | Same, but check if 2D head creates backwater exceeding normal depth |
| **FIXED** | `h = z_fixed` | Max of fixed stage and 2D surface head (2D can raise tailwater above fixed stage) |
| **TIDAL** | `h = tidal_curve(t)` | Max of tidal stage and 2D head (tidal flooding propagates through 2D) |
| **TIMESERIES** | `h = ts(t)` | Max of timeseries stage and 2D head |

### 14.3 Dynamic Tailwater from 2D Surface

For each outfall coupled to a 2D vertex or triangle:

```cpp
void setOutfallDepthWith2D(SimulationContext& ctx, int outfall_idx,
                           const MeshData& mesh, const SurfaceStateData& state) {
    auto& nodes = ctx.nodes;
    int vert_idx = outfall_2d_vertex[outfall_idx];  // -1 if not coupled
    
    if (vert_idx < 0) {
        // No 2D coupling — use standard outfall logic
        outfall::setOutfallDepth(ctx, outfall_idx, ctx.current_date);
        return;
    }
    
    // Get 2D surface head at the outfall coupling point
    double h_2d = state.vert_head[vert_idx];
    double z_inv = nodes.invert_elev[outfall_idx];
    
    // Compute standard outfall depth (without 2D)
    double h_standard = computeStandardOutfallHead(ctx, outfall_idx);
    
    // The effective boundary head is the MAXIMUM of standard and 2D
    // This ensures the 2D surface can raise the tailwater (backwater effect)
    // but cannot lower it below the standard boundary condition
    double h_effective = std::max(h_standard, h_2d);
    
    nodes.depth[outfall_idx] = std::max(h_effective - z_inv, 0.0);
    nodes.head[outfall_idx]  = z_inv + nodes.depth[outfall_idx];
}
```

### 14.4 Backflow Prevention at Outfalls

When a 2D-coupled outfall has a **flap gate**, the gate prevents backflow from the 2D surface into the pipe network:

```cpp
if (nodes.outfall_has_flap_gate[outfall_idx] && h_2d > h_pipe) {
    // Flap gate closed: no backflow from 2D → pipe
    // Outfall acts as a wall boundary for the pipe network
    // The 2D surface still receives spill from uncapped upstream nodes
    Q_exchange = 0.0;
    
    // Set outfall depth to critical/normal (independent of 2D)
    nodes.depth[outfall_idx] = computeStandardOutfallDepth(ctx, outfall_idx);
}
```

Without a flap gate, the outfall allows **bidirectional flow**:
- **Pipe → 2D**: Normal pipe discharge enters the 2D surface domain
- **2D → Pipe**: High 2D water levels push water back into the pipe (backwater effect)

### 14.5 Mass Balance at Outfall Boundaries

Outfall coupling must preserve mass balance across the 1D↔2D boundary:

```
Volume leaving pipe at outfall = Volume entering 2D at outfall vertex/triangle
```

The outfall discharge `Q_outfall` (computed by the 1D solver based on boundary head) becomes a **positive source** in the 2D cell containing the outfall coupling point:

```cpp
// After 1D routing step, transfer outfall discharge to 2D
for (auto& ocp : outfall_coupling_points) {
    double Q_pipe = computeOutfallDischarge(ctx, ocp.outfall_idx);
    
    // Inject pipe outflow as source into 2D cell
    state.coupling_flux[ocp.cell_idx] += Q_pipe / mesh.tri_area[ocp.cell_idx];
    
    // Track in mass balance
    ctx.mass_balance.outfall_to_2d_volume += Q_pipe * dt;
}
```

### 14.6 Outfall Coupling Sequence

```
Per SWMM routing step:

1. Read 2D surface heads at all outfall coupling points
2. Compute effective outfall boundary heads:
   h_boundary = max(h_standard, h_2d)  [unless flap gate blocks backflow]
3. Set outfall depths using effective heads (before 1D routing)
4. Run 1D routing with updated outfall boundaries
5. Compute outfall discharges from 1D solution
6. Inject outfall discharges into 2D coupling cells
7. Advance 2D solver (outfall cells receive pipe discharge as source)
8. Repeat
```

This creates a **feedback loop**: 2D surface levels influence outfall boundary conditions → outfall boundaries affect pipe flows → pipe flows discharge into 2D → 2D levels change → next step boundary conditions update.

---

## 15. C API for 2D Module (`openswmm_2d.h`)

The 2D module exposes a complete C API following the same conventions as the existing engine API. This enables external orchestration of the entire 2D workflow via CFFI/ctypes/Cython without requiring C++ knowledge.

### 15.1 Design Principles

1. **Opaque handle pattern** — all 2D state is accessed through the engine handle
2. **Index-based access** — vertices, triangles, and coupling points are accessed by 0-based index
3. **Bulk operations** — array get/set for efficient data transfer across FFI boundary
4. **Lifecycle-aware** — functions check engine state and return error codes
5. **Optional** — all functions return `SWMM_ERR_BADPARAM` if 2D module is not active

### 15.2 API Header: `include/openswmm/engine/openswmm_2d.h`

```c
/**
 * @file openswmm_2d.h
 * @brief Optional 2D surface routing module — C API.
 *
 * @details Provides query and control of the optional 2D surface routing
 *          module coupled to the 1D SWMM pipe network. The 2D module is
 *          active when [2D_VERTICES] and [2D_TRIANGLES] sections are present
 *          in the input file and the engine was compiled with OPENSWMM_BUILD_2D.
 *
 *          All functions require the engine to be in SWMM_STATE_RUNNING
 *          unless otherwise noted. Functions return SWMM_ERR_BADPARAM if
 *          the 2D module is not active.
 *
 * @defgroup engine_2d 2D Surface Routing API
 * @ingroup  engine_api
 */

#ifndef OPENSWMM_2D_H
#define OPENSWMM_2D_H

#include "openswmm_callbacks.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * 2D Module Status
 * ========================================================================= */

/** @brief Check whether the 2D module is active for this simulation.
 *  @param engine Engine handle.
 *  @param active Output: 1 if 2D is active, 0 otherwise.
 *  @returns SWMM_OK or error code.
 *  @note Valid after SWMM_STATE_INITIALIZED. */
SWMM_ENGINE_API int swmm_2d_is_active(SWMM_Engine engine, int* active);

/* =========================================================================
 * Mesh Geometry — Query (read-only after initialization)
 * ========================================================================= */

/** @brief Get the number of mesh vertices. */
SWMM_ENGINE_API int swmm_2d_vertex_count(SWMM_Engine engine, int* count);

/** @brief Get the number of mesh triangles. */
SWMM_ENGINE_API int swmm_2d_triangle_count(SWMM_Engine engine, int* count);

/** @brief Get vertex coordinates.
 *  @param idx Vertex index (0-based).
 *  @param x,y,z Output coordinates. */
SWMM_ENGINE_API int swmm_2d_vertex_get_xyz(SWMM_Engine engine, int idx,
                                             double* x, double* y, double* z);

/** @brief Bulk get vertex coordinates.
 *  @param x,y,z Output arrays (must be pre-allocated to vertex_count). */
SWMM_ENGINE_API int swmm_2d_vertex_get_xyz_bulk(SWMM_Engine engine,
                                                  double* x, double* y, double* z);

/** @brief Get triangle connectivity (3 vertex indices).
 *  @param idx Triangle index (0-based).
 *  @param v0,v1,v2 Output vertex indices. */
SWMM_ENGINE_API int swmm_2d_triangle_get_vertices(SWMM_Engine engine, int idx,
                                                    int* v0, int* v1, int* v2);

/** @brief Get triangle area.
 *  @param idx Triangle index.
 *  @param area Output area (m² or ft²). */
SWMM_ENGINE_API int swmm_2d_triangle_get_area(SWMM_Engine engine, int idx,
                                                double* area);

/** @brief Get triangle centroid coordinates. */
SWMM_ENGINE_API int swmm_2d_triangle_get_centroid(SWMM_Engine engine, int idx,
                                                    double* cx, double* cy, double* cz);

/** @brief Get triangle Manning's roughness. */
SWMM_ENGINE_API int swmm_2d_triangle_get_mannings(SWMM_Engine engine, int idx,
                                                    double* n);

/** @brief Get triangle neighbour indices (-1 = boundary edge).
 *  @param n0,n1,n2 Adjacent triangle indices across edges opposite v0,v1,v2. */
SWMM_ENGINE_API int swmm_2d_triangle_get_neighbours(SWMM_Engine engine, int idx,
                                                      int* n0, int* n1, int* n2);

/* =========================================================================
 * Coupling Map — Query
 * ========================================================================= */

/** @brief Get the number of vertex-to-node coupling points. */
SWMM_ENGINE_API int swmm_2d_vertex_coupling_count(SWMM_Engine engine, int* count);

/** @brief Get the number of triangle-to-node coupling points. */
SWMM_ENGINE_API int swmm_2d_triangle_coupling_count(SWMM_Engine engine, int* count);

/** @brief Get vertex coupling: which SWMM node is coupled to this vertex.
 *  @param vertex_idx Vertex index.
 *  @param node_idx Output: SWMM node index, or -1 if uncoupled. */
SWMM_ENGINE_API int swmm_2d_vertex_get_coupled_node(SWMM_Engine engine,
                                                      int vertex_idx, int* node_idx);

/** @brief Get triangle coupling: which SWMM node is coupled to this triangle.
 *  @param tri_idx Triangle index.
 *  @param node_idx Output: SWMM node index, or -1 if uncoupled. */
SWMM_ENGINE_API int swmm_2d_triangle_get_coupled_node(SWMM_Engine engine,
                                                        int tri_idx, int* node_idx);

/* =========================================================================
 * 2D State — Per-Triangle (read during RUNNING)
 * ========================================================================= */

/** @brief Get water depth at a triangle.
 *  @param idx Triangle index.
 *  @param depth Output depth (m or ft). */
SWMM_ENGINE_API int swmm_2d_get_depth(SWMM_Engine engine, int idx, double* depth);

/** @brief Get total head at a triangle (z + depth). */
SWMM_ENGINE_API int swmm_2d_get_head(SWMM_Engine engine, int idx, double* head);

/** @brief Get coupling exchange flux at a triangle (m/s, + = into 2D). */
SWMM_ENGINE_API int swmm_2d_get_coupling_flux(SWMM_Engine engine, int idx,
                                                double* flux);

/** @brief Get rainfall intensity at a triangle (m/s). */
SWMM_ENGINE_API int swmm_2d_get_rainfall(SWMM_Engine engine, int idx,
                                           double* rainfall);

/** @brief Get net source/sink rate at a triangle (m/s). */
SWMM_ENGINE_API int swmm_2d_get_net_source(SWMM_Engine engine, int idx,
                                             double* net_source);

/** @brief Bulk get depths for all triangles.
 *  @param depths Output array (pre-allocated to triangle_count). */
SWMM_ENGINE_API int swmm_2d_get_depths_bulk(SWMM_Engine engine, double* depths);

/** @brief Bulk get heads for all triangles. */
SWMM_ENGINE_API int swmm_2d_get_heads_bulk(SWMM_Engine engine, double* heads);

/** @brief Bulk get coupling fluxes for all triangles. */
SWMM_ENGINE_API int swmm_2d_get_coupling_fluxes_bulk(SWMM_Engine engine,
                                                       double* fluxes);

/* =========================================================================
 * 2D State — Per-Vertex (reconstructed heads)
 * ========================================================================= */

/** @brief Get reconstructed head at a vertex. */
SWMM_ENGINE_API int swmm_2d_vertex_get_head(SWMM_Engine engine, int idx,
                                              double* head);

/** @brief Bulk get reconstructed heads at all vertices. */
SWMM_ENGINE_API int swmm_2d_vertex_get_heads_bulk(SWMM_Engine engine,
                                                    double* heads);

/* =========================================================================
 * 2D Solver Statistics
 * ========================================================================= */

/** @brief Get the maximum depth across all triangles. */
SWMM_ENGINE_API int swmm_2d_get_max_depth(SWMM_Engine engine, double* max_depth);

/** @brief Get total 2D surface volume (sum of depth * area). */
SWMM_ENGINE_API int swmm_2d_get_total_volume(SWMM_Engine engine, double* volume);

/** @brief Get total exchange flow rate (sum of all coupling flows, m³/s).
 *  Positive = net flow from 2D into 1D network. */
SWMM_ENGINE_API int swmm_2d_get_total_exchange_flow(SWMM_Engine engine,
                                                      double* flow);

/** @brief Get number of CVODE internal steps taken in the last advance. */
SWMM_ENGINE_API int swmm_2d_get_cvode_steps(SWMM_Engine engine, long* steps);

/** @brief Get CVODE last internal step size. */
SWMM_ENGINE_API int swmm_2d_get_cvode_last_step(SWMM_Engine engine, double* h_last);

/** @brief Get per-triangle max depth statistics (cumulative).
 *  @param max_depths Output array (pre-allocated to triangle_count). */
SWMM_ENGINE_API int swmm_2d_get_stat_max_depths(SWMM_Engine engine,
                                                  double* max_depths);

/* =========================================================================
 * 2D Forcing — Override rainfall or coupling for external control
 * ========================================================================= */

/** @brief Force rainfall on a specific triangle.
 *  @param idx Triangle index.
 *  @param value Rainfall rate (m/s).
 *  @param mode SWMM_FORCING_OVERRIDE or SWMM_FORCING_ADD.
 *  @param persist SWMM_FORCING_RESET or SWMM_FORCING_PERSIST. */
SWMM_ENGINE_API int swmm_2d_force_rainfall(SWMM_Engine engine, int idx,
                                             double value, int mode, int persist);

/** @brief Force rainfall on all triangles (uniform). */
SWMM_ENGINE_API int swmm_2d_force_rainfall_uniform(SWMM_Engine engine,
                                                     double value, int mode,
                                                     int persist);

/** @brief Force coupling flux on a specific triangle (override computed exchange).
 *  @param value Flux rate (m/s, + = into 2D). */
SWMM_ENGINE_API int swmm_2d_force_coupling_flux(SWMM_Engine engine, int idx,
                                                  double value, int mode,
                                                  int persist);

/** @brief Clear all 2D forcings. */
SWMM_ENGINE_API int swmm_2d_force_clear_all(SWMM_Engine engine);

/* =========================================================================
 * 2D Solver Options — Query/Modify (valid after INITIALIZED)
 * ========================================================================= */

/** @brief Get the dry depth threshold (m). */
SWMM_ENGINE_API int swmm_2d_get_dry_depth(SWMM_Engine engine, double* dry_depth);

/** @brief Set the dry depth threshold (m). */
SWMM_ENGINE_API int swmm_2d_set_dry_depth(SWMM_Engine engine, double dry_depth);

/** @brief Get CVODE relative tolerance. */
SWMM_ENGINE_API int swmm_2d_get_rel_tolerance(SWMM_Engine engine, double* rtol);

/** @brief Set CVODE relative tolerance. */
SWMM_ENGINE_API int swmm_2d_set_rel_tolerance(SWMM_Engine engine, double rtol);

/** @brief Get CVODE absolute tolerance. */
SWMM_ENGINE_API int swmm_2d_get_abs_tolerance(SWMM_Engine engine, double* atol);

/** @brief Set CVODE absolute tolerance. */
SWMM_ENGINE_API int swmm_2d_set_abs_tolerance(SWMM_Engine engine, double atol);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* OPENSWMM_2D_H */
```

### 15.3 Cython Declarations (`python/openswmm/engine/_2d.pxd`)

```cython
cdef extern from "openswmm_2d.h":
    # Status
    int swmm_2d_is_active(void* engine, int* active)
    
    # Mesh geometry
    int swmm_2d_vertex_count(void* engine, int* count)
    int swmm_2d_triangle_count(void* engine, int* count)
    int swmm_2d_vertex_get_xyz(void* engine, int idx,
                                double* x, double* y, double* z)
    int swmm_2d_vertex_get_xyz_bulk(void* engine,
                                     double* x, double* y, double* z)
    int swmm_2d_triangle_get_vertices(void* engine, int idx,
                                       int* v0, int* v1, int* v2)
    int swmm_2d_triangle_get_area(void* engine, int idx, double* area)
    int swmm_2d_triangle_get_centroid(void* engine, int idx,
                                       double* cx, double* cy, double* cz)
    int swmm_2d_triangle_get_mannings(void* engine, int idx, double* n)
    int swmm_2d_triangle_get_neighbours(void* engine, int idx,
                                         int* n0, int* n1, int* n2)
    
    # Coupling
    int swmm_2d_vertex_coupling_count(void* engine, int* count)
    int swmm_2d_triangle_coupling_count(void* engine, int* count)
    int swmm_2d_vertex_get_coupled_node(void* engine, int vidx, int* nidx)
    int swmm_2d_triangle_get_coupled_node(void* engine, int tidx, int* nidx)
    
    # State
    int swmm_2d_get_depth(void* engine, int idx, double* depth)
    int swmm_2d_get_head(void* engine, int idx, double* head)
    int swmm_2d_get_coupling_flux(void* engine, int idx, double* flux)
    int swmm_2d_get_rainfall(void* engine, int idx, double* rainfall)
    int swmm_2d_get_depths_bulk(void* engine, double* depths)
    int swmm_2d_get_heads_bulk(void* engine, double* heads)
    int swmm_2d_get_coupling_fluxes_bulk(void* engine, double* fluxes)
    
    # Vertex state
    int swmm_2d_vertex_get_head(void* engine, int idx, double* head)
    int swmm_2d_vertex_get_heads_bulk(void* engine, double* heads)
    
    # Statistics
    int swmm_2d_get_max_depth(void* engine, double* max_depth)
    int swmm_2d_get_total_volume(void* engine, double* volume)
    int swmm_2d_get_total_exchange_flow(void* engine, double* flow)
    int swmm_2d_get_cvode_steps(void* engine, long* steps)
    int swmm_2d_get_cvode_last_step(void* engine, double* h_last)
    
    # Forcing
    int swmm_2d_force_rainfall(void* engine, int idx,
                                double value, int mode, int persist)
    int swmm_2d_force_rainfall_uniform(void* engine,
                                        double value, int mode, int persist)
    int swmm_2d_force_coupling_flux(void* engine, int idx,
                                     double value, int mode, int persist)
    int swmm_2d_force_clear_all(void* engine)
    
    # Options
    int swmm_2d_get_dry_depth(void* engine, double* dry_depth)
    int swmm_2d_set_dry_depth(void* engine, double dry_depth)
    int swmm_2d_get_rel_tolerance(void* engine, double* rtol)
    int swmm_2d_set_rel_tolerance(void* engine, double rtol)
    int swmm_2d_get_abs_tolerance(void* engine, double* atol)
    int swmm_2d_set_abs_tolerance(void* engine, double atol)
```

### 15.4 Python Wrapper (`python/openswmm/engine/_2d.pyx`)

```python
# High-level Python class wrapping the 2D C API
cimport numpy as np
import numpy as np

cdef class Surface2D:
    """Read-only view of the 2D surface routing state."""
    
    cdef void* _engine
    
    def __init__(self, engine_handle):
        self._engine = <void*><uintptr_t>engine_handle
    
    @property
    def is_active(self):
        cdef int active = 0
        _check(swmm_2d_is_active(self._engine, &active))
        return bool(active)
    
    @property
    def n_vertices(self):
        cdef int count = 0
        _check(swmm_2d_vertex_count(self._engine, &count))
        return count
    
    @property
    def n_triangles(self):
        cdef int count = 0
        _check(swmm_2d_triangle_count(self._engine, &count))
        return count
    
    def get_depths(self):
        """Return depths for all triangles as a numpy array."""
        cdef int n = self.n_triangles
        cdef np.ndarray[double, ndim=1] arr = np.empty(n, dtype=np.float64)
        _check(swmm_2d_get_depths_bulk(self._engine, &arr[0]))
        return arr
    
    def get_heads(self):
        """Return total heads for all triangles as a numpy array."""
        cdef int n = self.n_triangles
        cdef np.ndarray[double, ndim=1] arr = np.empty(n, dtype=np.float64)
        _check(swmm_2d_get_heads_bulk(self._engine, &arr[0]))
        return arr
    
    def get_vertex_coords(self):
        """Return (x, y, z) arrays for all vertices."""
        cdef int n = self.n_vertices
        cdef np.ndarray[double, ndim=1] x = np.empty(n, dtype=np.float64)
        cdef np.ndarray[double, ndim=1] y = np.empty(n, dtype=np.float64)
        cdef np.ndarray[double, ndim=1] z = np.empty(n, dtype=np.float64)
        _check(swmm_2d_vertex_get_xyz_bulk(self._engine, &x[0], &y[0], &z[0]))
        return x, y, z
    
    @property
    def total_volume(self):
        cdef double vol = 0.0
        _check(swmm_2d_get_total_volume(self._engine, &vol))
        return vol
    
    @property
    def total_exchange_flow(self):
        cdef double flow = 0.0
        _check(swmm_2d_get_total_exchange_flow(self._engine, &flow))
        return flow
```

### 15.5 End-to-End CFFI Workflow Example

The C API enables a complete external orchestration workflow:

```c
/* Example: External Python/C driver controlling the full 2D workflow */

SWMM_Engine e = swmm_engine_create();
swmm_engine_open(e, "model_with_2d.inp", "model.rpt", "model.out", NULL);
swmm_engine_initialize(e);

/* Check if 2D is active */
int has_2d = 0;
swmm_2d_is_active(e, &has_2d);

int n_tri = 0, n_vert = 0;
if (has_2d) {
    swmm_2d_triangle_count(e, &n_tri);
    swmm_2d_vertex_count(e, &n_vert);
}

swmm_engine_start(e, 1);

double elapsed = 0.0;
double* depths = malloc(n_tri * sizeof(double));

while (swmm_engine_step(e, &elapsed) == SWMM_OK && elapsed > 0.0) {
    if (has_2d) {
        /* Read 2D state after each step */
        swmm_2d_get_depths_bulk(e, depths);
        
        /* Optionally override rainfall for scenario testing */
        swmm_2d_force_rainfall_uniform(e, 0.001,  /* 1 mm/s */
            SWMM_FORCING_OVERRIDE, SWMM_FORCING_RESET);
        
        /* Query coupling statistics */
        double total_exchange = 0.0;
        swmm_2d_get_total_exchange_flow(e, &total_exchange);
        
        /* Query solver diagnostics */
        long cvode_steps = 0;
        swmm_2d_get_cvode_steps(e, &cvode_steps);
    }
}

free(depths);
swmm_engine_end(e);
swmm_engine_report(e);
swmm_engine_close(e);
swmm_engine_destroy(e);
```

---

## 16. Summary of Key Design Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Mesh type | Constrained Delaunay triangles | Boundary-fitting, adaptive resolution, matches paper |
| Solver | CVODE (BDF) + GMRES | Handles stiffness from Manning's nonlinearity, Jacobian-free |
| Accuracy | Second-order (linear reconstruction + limiter) | Paper formulation, avoids first-order numerical diffusion |
| Coupling mechanism | Forcing API | Clean separation, no modifications to core SWMM solver |
| Exchange equation | Orifice | Standard for manhole/inlet exchange, handles bidirectional flow |
| Uncapped nodes | Suppress ponding, allow head overshoot | 2D surface replaces ponded area for coupled nodes |
| Outfall feedback | max(h_standard, h_2d) | Dynamic tailwater from 2D raises boundary without lowering it |
| C API | `openswmm_2d.h` with opaque handles | Consistent with engine API, enables CFFI/ctypes/Cython |
| Dependency management | vcpkg (SUNDIALS) | Consistent with existing project infrastructure |
| Compile-time optional | `OPENSWMM_BUILD_2D` CMake flag | No penalty for users who don't need 2D |
| Data layout | SoA (parallel vectors) | Matches existing engine pattern, cache-friendly |
| Section naming | `[2D_*]` prefix | Clear, avoids collisions, extensible |

---

## References

- Kumar, M., Duffy, C.J., and Salvage, K.M. (2009). "A Second-Order Accurate, Finite Volume–Based, Integrated Hydrologic Modeling (FIHM) Framework for Simulation of Surface and Subsurface Flow." *Vadose Zone Journal*, doi:10.2136/vzj2009.0014.
- Jawahar, P. and Kamath, H. (2000). "A high-resolution procedure for Euler and Navier-Stokes computations on unstructured grids." *J. Comput. Phys.*, 164:165–203.
- Abdul, A.S. and Gillham, R.W. (1984). "Laboratory studies of the effects of the capillary fringe on streamflow generation." *Water Resour. Res.*, 20:691–698.
- Cohen, S.D. and Hindmarsh, A.C. (1994). "CVODE user guide." Technical Rep. UCRL-MA-118618. Lawrence Livermore National Lab.
