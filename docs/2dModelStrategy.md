# Two-Dimensional Surface Routing — Implementation Strategy

## Overview

This document details the implementation strategy for an optional 2D surface routing module coupled to the OpenSWMM engine. The module implements the **second-order accurate, semi-discrete finite volume formulation** from Kumar, Duffy, and Salvage (2009) — initially limited to the **diffusion-wave surface flow** component. The design follows the same data-oriented, cache-friendly, Structure-of-Arrays (SoA) patterns used throughout the engine.

**Scope — Phase 1 (this document):**
- 2D diffusion-wave surface routing on a triangular mesh
- Coupling to SWMM nodes/junctions via orifice equation and forcing API
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

| Section | Purpose |
|---------|---------|
| `[2D_MESH_FILE]` | *(optional)* Reference an external file containing 2D mesh and configuration sections |
| `[2D_OPTIONS]` | Solver options, tolerances, time-stepping parameters |
| `[2D_VERTICES]` | Mesh vertex coordinates |
| `[2D_TRIANGLES]` | Triangle connectivity and surface roughness |
| `[2D_VERTEX_NODE_MAP]` | Vertex-to-SWMM-node coupling |
| `[2D_TRIANGLE_NODE_MAP]` | Triangle-centroid-to-SWMM-node coupling |
| `[2D_BOUNDARY_CONDITIONS]` | *(future)* Boundary types on mesh edges |
| `[2D_INITIAL_CONDITIONS]` | *(future)* Per-cell initial water depth |
| `[2D_INFILTRATION]` | *(future)* Per-cell or per-zone infiltration params |

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

```
;; Solver and timestepping options for the 2D surface routing module
;;
;; Parameter              Value
;; ---------------------- ------
MAX_TIMESTEP              10.0       ;; Maximum CVODE internal step (seconds)
MIN_TIMESTEP              0.001      ;; Minimum CVODE internal step (seconds)
REL_TOLERANCE             1.0e-4     ;; CVODE relative tolerance
ABS_TOLERANCE             1.0e-6     ;; CVODE absolute tolerance (metres of depth)
LINEAR_SOLVER             GMRES      ;; GMRES | BICGSTAB | TFQMR
PRECONDITIONER            NONE       ;; NONE | JACOBI | ILU (future)
MAX_KRYLOV_DIM            30         ;; Maximum Krylov subspace dimension
DRY_DEPTH                 0.001      ;; Depth below which cell is considered dry (m)
COUPLING_INTERVAL         0          ;; 0 = every SWMM routing step (default)
REPORT_2D                 YES        ;; Write 2D results to output
```

### 1.4 `[2D_VERTICES]`

Each row defines a mesh vertex. Vertices are indexed in order of appearance (0-based).

```
;; X          Y          Z          TAG (optional)
100.0        200.0      10.5
100.5        200.5      10.3       inlet_region
101.0        200.0      10.1
```

- **X, Y** — Horizontal coordinates (same CRS as SWMM coordinates)
- **Z** — Ground surface elevation (project length units)
- **TAG** — Optional string tag for grouping/reference

### 1.5 `[2D_TRIANGLES]`

Each row defines a triangle by referencing three vertex indices (0-based) and a Manning's roughness coefficient.

```
;; V1   V2   V3   MANNINGS_N   TAG (optional)
0    1    2    0.035
3    4    5    0.025          road_surface
```

- **V1, V2, V3** — Vertex indices (0-based)
- **MANNINGS_N** — Manning's roughness coefficient (s/m^{1/3})
- **TAG** — Optional string tag

### 1.6 `[2D_VERTEX_NODE_MAP]`

Maps mesh vertices to SWMM coupling nodes. Flow exchange occurs at these points via an orifice equation.

```
;; VERTEX_INDEX_OR_TAG    SWMM_NODE_NAME
5                         J1
inlet_region              J2
```

### 1.7 `[2D_TRIANGLE_NODE_MAP]`

Maps triangle centroids to SWMM coupling nodes. Flow exchange is distributed over the triangle area.

```
;; TRIANGLE_INDEX_OR_TAG  SWMM_NODE_NAME
0                         J3
road_surface              J4
```

---

## 2. Data Structures (SoA Layout)

All 2D data structures follow the existing OpenSWMM SoA pattern: parallel `std::vector` arrays, a single `resize()` method, `save_state()` / `reset_state()` lifecycle methods, and flat 2D arrays where needed.

### 2.1 File: `src/engine/2d/data/MeshData.hpp`

```cpp
namespace openswmm::twoD {

struct MeshData {

    // -----------------------------------------------------------------------
    // Vertex arrays — indexed by vertex index [0, n_vertices)
    // -----------------------------------------------------------------------

    std::vector<double> vx;             // Vertex X coordinate
    std::vector<double> vy;             // Vertex Y coordinate
    std::vector<double> vz;             // Vertex Z (ground elevation)
    std::vector<std::string> vtag;      // Optional vertex tag

    // -----------------------------------------------------------------------
    // Triangle static properties — indexed by triangle index [0, n_triangles)
    // -----------------------------------------------------------------------

    // Connectivity (3 vertex indices per triangle)
    std::vector<int> tri_v0;            // Vertex 0 index
    std::vector<int> tri_v1;            // Vertex 1 index
    std::vector<int> tri_v2;            // Vertex 2 index

    // Neighbour connectivity (3 adjacent triangle indices, -1 = boundary)
    std::vector<int> tri_nbr0;          // Neighbour across edge opposite v0
    std::vector<int> tri_nbr1;          // Neighbour across edge opposite v1
    std::vector<int> tri_nbr2;          // Neighbour across edge opposite v2

    // Precomputed geometry
    std::vector<double> tri_area;       // Planimetric area (m²)
    std::vector<double> tri_cx;         // Centroid X
    std::vector<double> tri_cy;         // Centroid Y
    std::vector<double> tri_cz;         // Centroid Z (avg of vertex elevations)

    // Edge geometry — flat 2D: [tri * 3 + edge]
    std::vector<double> edge_length;    // Length of each edge
    std::vector<double> edge_nx;        // Outward normal X component
    std::vector<double> edge_ny;        // Outward normal Y component
    std::vector<double> edge_mx;        // Edge midpoint X
    std::vector<double> edge_my;        // Edge midpoint Y
    std::vector<double> edge_mz;        // Edge midpoint Z (interpolated)

    // Surface properties
    std::vector<double> mannings_n;     // Manning's roughness coefficient
    std::vector<std::string> tri_tag;   // Optional triangle tag

    // -----------------------------------------------------------------------
    // Vertex reconstruction stencil (pseudo-Laplacian weights)
    // -----------------------------------------------------------------------
    // Stored as CSR (compressed sparse row) for variable stencil sizes
    std::vector<int>    vert_stencil_ptr;   // [n_vertices + 1] row pointers
    std::vector<int>    vert_stencil_idx;   // Column indices (triangle indices)
    std::vector<double> vert_stencil_wt;    // Pseudo-Laplacian weights

    // -----------------------------------------------------------------------
    // Coupling maps
    // -----------------------------------------------------------------------

    // Vertex-to-SWMM-node coupling
    std::vector<int> vert_coupled_node;     // SWMM node index (-1 = none)

    // Triangle-to-SWMM-node coupling
    std::vector<int> tri_coupled_node;      // SWMM node index (-1 = none)

    // Deferred resolution names (populated during parsing)
    std::vector<std::string> vert_coupled_node_name;
    std::vector<std::string> tri_coupled_node_name;

    // -----------------------------------------------------------------------
    // Capacity
    // -----------------------------------------------------------------------

    int n_vertices()  const noexcept { return static_cast<int>(vx.size()); }
    int n_triangles() const noexcept { return static_cast<int>(tri_v0.size()); }

    void resize_vertices(int nv);
    void resize_triangles(int nt);
    void build_topology();          // Compute neighbours, edges, areas
    void build_vertex_stencils();   // Build pseudo-Laplacian weights
};

} // namespace openswmm::twoD
```

### 2.2 File: `src/engine/2d/data/SurfaceStateData.hpp`

```cpp
namespace openswmm::twoD {

struct SurfaceStateData {

    // -----------------------------------------------------------------------
    // State variables — per triangle [0, n_triangles)
    // -----------------------------------------------------------------------

    std::vector<double> depth;          // Overland flow depth ψ_o (m)
    std::vector<double> head;           // Total head h_o = z_s + ψ_o (m)

    // Gradient fields (per triangle)
    std::vector<double> grad_hx;        // ∂h/∂x (unlimited gradient)
    std::vector<double> grad_hy;        // ∂h/∂y (unlimited gradient)
    std::vector<double> grad_hx_lim;    // Limited gradient X
    std::vector<double> grad_hy_lim;    // Limited gradient Y

    // Reconstructed head at vertices — flat 2D [vertex_idx]
    std::vector<double> vert_head;      // Head reconstructed at vertices

    // Fluxes — flat 2D: [tri * 3 + edge]
    std::vector<double> edge_flux;      // Normal flux through each edge

    // Source/sink terms
    std::vector<double> rainfall;       // Rainfall intensity (m/s)
    std::vector<double> coupling_flux;  // Exchange with SWMM node (m/s, + = into 2D)
    std::vector<double> net_source;     // Net source/sink per cell (m/s)

    // -----------------------------------------------------------------------
    // Previous step state
    // -----------------------------------------------------------------------

    std::vector<double> old_depth;

    // -----------------------------------------------------------------------
    // Cumulative statistics
    // -----------------------------------------------------------------------

    std::vector<double> stat_max_depth;
    std::vector<double> stat_cum_volume;    // Cumulative volume through cell

    // -----------------------------------------------------------------------
    // Lifecycle
    // -----------------------------------------------------------------------

    void resize(int n_triangles);
    void save_state() noexcept;
    void reset_state() noexcept;
};

} // namespace openswmm::twoD
```

### 2.3 File: `src/engine/2d/data/SolverOptions2D.hpp`

```cpp
namespace openswmm::twoD {

enum class LinearSolverType : int8_t {
    GMRES    = 0,
    BICGSTAB = 1,
    TFQMR    = 2
};

enum class PreconditionerType : int8_t {
    NONE   = 0,
    JACOBI = 1,
    ILU    = 2    // future
};

struct SolverOptions2D {
    double max_timestep      = 10.0;
    double min_timestep      = 0.001;
    double rel_tolerance     = 1.0e-4;
    double abs_tolerance     = 1.0e-6;
    double dry_depth         = 0.001;
    int    max_krylov_dim    = 30;
    int    coupling_interval = 0;       // 0 = every SWMM step
    bool   report_2d         = true;

    LinearSolverType   linear_solver   = LinearSolverType::GMRES;
    PreconditionerType preconditioner  = PreconditionerType::NONE;
};

} // namespace openswmm::twoD
```

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

### 6.1 Coupling Philosophy

- The 2D module communicates with SWMM **exclusively via the forcing API** (`ForcingData`).
- This keeps the 1D and 2D solvers cleanly separated.
- The 2D module injects lateral inflow into SWMM nodes; SWMM node heads determine backflow.

### 6.2 Coupling at Nodes — Orifice Equation

For each coupled vertex/triangle with SWMM node index `j`:

```
Q_exchange = C_d · A_orifice · sign(Δh) · sqrt(2g · |Δh|)
```

Where:
- `Δh = h_2d - h_swmm` (head difference)
- `h_2d` = 2D surface head at the coupling point
- `h_swmm = nodes.head[j]` = SWMM node head
- `C_d` = discharge coefficient (default 0.65, configurable)
- `A_orifice` = effective exchange area (configurable per coupling point)
- `g` = gravitational acceleration

**Flow direction:**
- `Δh > 0`: Flow from 2D surface → SWMM node (drainage into pipe network)
- `Δh < 0`: Flow from SWMM node → 2D surface (surcharge/flooding)

### 6.3 Coupling at Outfalls — Backflow Prevention

When a coupling node is an OUTFALL (`nodes.type[j] == NodeType::OUTFALL`):

1. **FREE outfall:** Allow unrestricted drainage. Set `h_swmm` = outfall invert elevation.
2. **FIXED outfall:** Use fixed stage as `h_swmm`. If `h_2d < h_fixed`, no flow (flap gate behaviour).
3. **TIDAL / TIMESERIES outfall:** Use time-varying stage as `h_swmm`. Enforce backflow prevention if `outfall_has_flap_gate[j]` is true.
4. **NORMAL outfall:** Use normal depth computation for `h_swmm`.

**Flap gate logic:**
```
if outfall_has_flap_gate[j] and h_swmm > h_2d:
    Q_exchange = 0   // No backflow allowed
```

### 6.4 Forcing API Integration

Each coupling step:

```cpp
// Inject 2D→SWMM flow as lateral inflow
for (auto& cp : coupling_points) {
    double Q = compute_orifice_exchange(cp, mesh, state, ctx);

    // Positive Q = flow into SWMM node
    ctx.forcing.node_lat_inflow_mode[cp.node_idx]  = ForcingMode::ADD;
    ctx.forcing.node_lat_inflow_value[cp.node_idx] += Q;
    ctx.forcing.node_lat_inflow_persist[cp.node_idx] = ForcingPersist::RESET;

    // Record coupling flux back to 2D cell (negative = drainage out of 2D)
    state.coupling_flux[cp.cell_idx] = -Q / mesh.tri_area[cp.cell_idx];
}
```

### 6.5 Coupling Sequence per Timestep

```
1. SWMM saves state (nodes.save_state())
2. SWMM applies forcings (including 2D coupling from previous step)
3. SWMM advances hydraulic routing by dt_swmm
4. Read updated SWMM node heads
5. Compute 2D↔SWMM exchange flows (orifice equation)
6. Inject exchange flows via forcing API (for next SWMM step)
7. Advance 2D solver by dt_swmm using CVODE
8. Update 2D rainfall from system rain gages
```

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

## 8. Integration into SimulationContext

### 8.1 Context Extension

Add an optional 2D data member to `SimulationContext`:

```cpp
// In SimulationContext.hpp
#ifdef OPENSWMM_HAS_2D
#include "../2d/data/MeshData.hpp"
#include "../2d/data/SurfaceStateData.hpp"
#include "../2d/data/SolverOptions2D.hpp"

// Under the "Object data stores" section:
twoD::MeshData          mesh_2d;
twoD::SurfaceStateData  surface_2d;
twoD::SolverOptions2D   options_2d;
bool                    has_2d = false;  // True if [2D_VERTICES] was parsed
#endif
```

### 8.2 Section Registration

In the input reader setup (where built-in sections are registered):

```cpp
#ifdef OPENSWMM_HAS_2D
registry.register_custom("2D_OPTIONS",            twoD::handle_2d_options);
registry.register_custom("2D_VERTICES",            twoD::handle_2d_vertices);
registry.register_custom("2D_TRIANGLES",           twoD::handle_2d_triangles);
registry.register_custom("2D_VERTEX_NODE_MAP",     twoD::handle_2d_vertex_node_map);
registry.register_custom("2D_TRIANGLE_NODE_MAP",   twoD::handle_2d_triangle_node_map);
#endif
```

### 8.3 Engine Lifecycle Hooks

```
open():
    Parse input (including 2D sections if present)
    If 2D sections found:
        ctx.has_2d = true
        Build mesh topology (MeshBuilder)
        Build vertex stencils (VertexReconstruction)
        Resolve coupling node names → indices (PostParseResolver)

initialize():
    If ctx.has_2d:
        Initialize CVODE solver
        Set initial depths (from [2D_INITIAL_CONDITIONS] or zero)
        Compute initial heads

step():
    SWMM routing step (existing)
    If ctx.has_2d:
        Update coupling exchange flows
        Advance 2D surface solver
        Apply coupling forcings for next step

end():
    If ctx.has_2d:
        Finalize CVODE
        Report 2D statistics
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
    9. Inject exchange Q into forcing API for next SWMM step
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
        e. Inject exchange into forcing API
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

The same `Q_exchange` value (in m³/s) is:
- Subtracted from the 2D cell as `coupling_flux[i] = -Q / A_i` (m/s sink)
- Added to the SWMM node as `node_lat_inflow_value[j] += Q` (m³/s source)

Both use the same `dt_swmm` interval. The CVODE solver integrates the coupling flux as a constant source/sink over its internal sub-steps, which preserves the total volume exchange. The forcing API's `RESET` persistence ensures the coupling flow is cleared and recomputed each SWMM step.

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
