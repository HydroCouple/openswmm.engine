# OpenSWMM 2D Surface Model & 1D–2D Coupling — Code Review

**Date:** 2026-07-18
**Scope:** `src/engine/2d/` and its touchpoints in `src/engine/core/SWMMEngine.cpp`, `src/engine/hydraulics/{Routing,DynamicWave,Outfall}.cpp`.
**Method:** This document is derived entirely from the current source code (not from prior plan documents). File references are relative to the `openswmm.engine` repository root.

> **⚠️ PARTIALLY SUPERSEDED (D2 retirement, 2026-07-29).** This review
> predates the retirement of the CVODE/ARKODE 2D stack. The explicit
> local-inertial marcher (`ExplicitInertialSolver` /
> `ExplicitKokkosSurfaceSolver`) is now the **only** 2D integrator, and
> SUNDIALS/hypre are no longer dependencies. Everything below describing
> CVODE/ARKODE integration, `ActiveSetBuilder` masking (§4.6), the
> quiescence short-circuit, Krylov/preconditioner options, or the
> `MAX_CVODE_STEPS` failure path describes **retired machinery**; the
> corresponding `[2D_OPTIONS]` keys are now hard parse errors. The mesh,
> BC table (§4.2), coupling, rainfall, and mass-balance descriptions
> remain broadly accurate. Boundary conditions are enforced per substep
> by `SurfaceFluxCalculator::boundaryEdgeFlux` in the explicit marcher.

---

## 1. Architecture Overview

The 2D module is an optional overland-flow solver that lives in `src/engine/2d/` and is orchestrated by a single class, `twoD::SurfaceRouter2D` (`src/engine/2d/SurfaceRouter2D.{hpp,cpp}`), which `SWMMEngine` owns as a member (`surface_router_`). The build flag `OPENSWMM_HAS_2D` gates the SUNDIALS-backed solvers; without it the mesh, parsing, and rainfall/statistics code still compile but no time integration occurs.

```
src/engine/2d/
├── SurfaceRouter2D.{hpp,cpp}      Orchestrator: lifecycle, macro-step windows, mass balance
├── api/Api2D.cpp                  C API surface (runtime queries, forcing)
├── coupling/NodeCoupling.{hpp,cpp} 1D↔2D exchange: orifice law, outfall BC feedback
├── data/
│   ├── MeshData.hpp               SoA mesh (vertices, triangles, edges, coupling maps)
│   ├── SurfaceStateData.hpp       SoA state (V, η, h̄, gradients, fluxes, forcings, stats)
│   ├── BoundaryData.hpp           Per-edge boundary conditions
│   ├── ActiveSetData.hpp          Dry-cell mask data
│   ├── SolverOptions2D.hpp        All configuration ([2D_OPTIONS] + runtime factors)
│   ├── PendingRows2D.hpp          Parse-time row buffers (BCs, edge conveyance)
│   └── Serialize2D.hpp            Serialization collectors
├── input/SectionHandlers2D.{hpp,cpp}  .inp section parsers + external mesh loader
├── mesh/
│   ├── MeshBuilder.cpp            Topology (neighbours, areas, edge normals), validation
│   ├── VertexReconstruction.cpp   Pseudo-Laplacian vertex stencils + vertex heads
│   └── RainfallInterpolator.cpp   Natural-neighbour / IDW gage→cell weights
├── solver/
│   ├── ISurfaceSolver.hpp         Backend-neutral integrator interface
│   ├── CvodeSurfaceSolver.cpp     Default: CVODE BDF + Newton + GMRES (serial CPU)
│   ├── ArkodeSurfaceSolver.cpp    ARKStep IMEX (DW split, or local-inertial)
│   ├── SurfaceFluxCalculator.cpp  Gradients, DW edge fluxes, RHS assembly, diagnostics
│   ├── InertialEdges.cpp          Unique-edge structure for local-inertial q DOFs
│   ├── ActiveSetBuilder.cpp       Wet/sourced seed + BFS halo mask
│   ├── SurfaceJacobian.cpp        Sparse Jacobian helper (AMG path)
│   ├── HypreAmgPreconditioner.cpp BoomerAMG preconditioner (OPENSWMM_WITH_HYPRE)
│   ├── SurfaceSolverFactory.cpp   Runtime backend selection (serial / GPU plugin)
│   └── GpuPluginAbi.h             C ABI for GPU backend plugins
├── gpu/                            Kokkos CVODE/ARKODE solvers + CUDA/HIP/SYCL/OMP plugins
└── output/Default2DOutputPlugin.cpp  CF-1.11 / UGRID-1.0 HDF5 writer
```

**Key design decisions visible in the code:**

- The 2D solver runs internally in **SI units** (metres, m³, g = 9.80665) while the 1D engine always computes in **US feet internally** (even for SI projects). All 1D⇄2D conversion factors are therefore the fixed ft⇄m constants, independent of `FLOW_UNITS` (`SurfaceRouter2D::initialize`, lines ~177–191). The *mesh*, by contrast, is authored in project display units, so its one-time scaling to SI **is** driven by `FLOW_UNITS` (unless the mesh file declares `;; UNITS: SI (m)`).
- The integrated state is the **cell water volume V (m³)**, not depth or head. Head η and mean depth h̄ are reconstructed per cell through the flat-cell closure `h̄ = max(V,0)/A`, `η = z_c + h̄`. This makes error control physical (per-cell depth tolerance × area) and lets the `h^(5/3)` conductance vanish smoothly at the dry limit with no explicit wet/dry switch.
- The 1D and 2D domains are advanced by **operator splitting with explicit (loose) coupling**: exchange terms are computed between the 1D routing step and the 2D advance, and each side holds the other's state frozen during its own solve. An opt-in "live" path evaluates the junction exchange inside the 2D implicit RHS instead (§6.4).

---

## 2. Configuration

### 2.1 Input sections

Registered in `register2DSections` (`input/SectionHandlers2D.cpp`) and wired by `SWMMEngine::open` (SWMMEngine.cpp ~line 155). All sections may live inline in the main `.inp` or in an external mesh file referenced by `[2D_MESH_FILE]`.

| Section | Line format | Notes |
|---|---|---|
| `[2D_OPTIONS]` | `KEY VALUE` | See table §2.2 |
| `[2D_VERTICES]` | `X Y Z [TAG]` | Vertex coordinates + ground elevation, project units |
| `[2D_TRIANGLES]` | `V1 V2 V3 MANNINGS_N [TAG]` | 0-based vertex indices; per-cell Manning's n (default 0.035) |
| `[2D_VERTEX_NODE_MAP]` | `VERTEX_IDX_OR_TAG NODE [CD] [AREA]` | Vertex-coupled point; Cd default 0.65, area default 1.0 (project length², scaled to m²) |
| `[2D_TRIANGLE_NODE_MAP]` | `TRI_IDX_OR_TAG NODE [CD] [AREA]` | Centroid-coupled point |
| `[2D_BOUNDARY_CONDITIONS]` | `TRI EDGE TYPE [PARAM_1 [PARAM_2 [GROUP]]]` | TYPE ∈ WALL, NORMAL_FLOW, SPECIFIED_STAGE / TS_STAGE, SPECIFIED_FLOW / TS_FLOW, RATING_CURVE. PARAM_1 is slope / head / per-metre flow / TS-or-curve name by type; `*` = none. PARAM_2 reserved. |
| `[2D_EDGE_CONVEYANCE]` | `FROM_VERTEX TO_VERTEX CONVEYANCE` | Per-edge factor strictly in [0,1]; 0 = wall. Mirrored to both slots of interior edges to preserve flux antisymmetry (mass conservation). |
| `[2D_MESH_FILE]` | `FILE <path>` | First FILE token only; loaded after the main `.inp` parses (external `.2dm`-style file may carry its own 2D sections and a `;; UNITS:` header). |

Parse-time behaviour worth knowing: boundary-condition and edge-conveyance rows are buffered as **pending rows** (mesh topology doesn't exist yet at parse time) and drained into live per-edge arrays in `SurfaceRouter2D::initialize()` / `prepareForEdit()` (`drainPendingRows()`). The pending rows are *retained* after draining so `InpWriter`/GeoPackage can re-emit the authored form; `options_.pending_rows_drained` tells serializers which copy is live.

The 2D module activates simply when the parsed mesh has ≥ 3 vertices and ≥ 1 triangle; there is no separate on/off option.

### 2.2 `[2D_OPTIONS]` keys (SolverOptions2D defaults)

| Key | Default | Meaning |
|---|---|---|
| `MAX_TIMESTEP` | 10.0 s | Max internal integrator step |
| `MIN_TIMESTEP` | 0.001 s | Min internal integrator step |
| `REL_TOLERANCE` | 1e-4 | Integrator rtol (scales with cell volume) |
| `ABS_TOLERANCE` | 1e-6 m | Per-cell absolute **depth** tolerance (atol_i = value·A_i) |
| `DRY_DEPTH` | 0.001 m | Dry-cell threshold; also the wet/dry Hermite ramp scale in the coupling |
| `LIMITER_EPSILON` | 1e-6 | Jawahar–Kamath limiter regularization |
| `FLUX_DH_EPS` | 0.004 m | Head-difference floor for the √Δη flux regularization |
| `COUPLING_CD` | 0.65 | Default orifice discharge coefficient |
| `COUPLING_INTERVAL` | 0 | Legacy step-count macro-window (0/1 = every step; >1 = experimental, warns) |
| `COUPLING_WINDOW` | −1 (AUTO) | Time-based 2D advance window in seconds. AUTO = nominal `ROUTING_STEP` clamped to `MAX_TIMESTEP` (unless a legacy `COUPLING_INTERVAL>1` is set); 0 = advance every routing step; >0 explicit. Takes precedence over COUPLING_INTERVAL. |
| `MAX_KRYLOV_DIM` | 30 | GMRES subspace size |
| `MAX_CVODE_STEPS` | 500 | Max internal steps per advance (exceeded ⇒ failed window) |
| `ACTIVE_SET` | NO | Dry-cell masking of the RHS pipeline (CVODE+DW only) |
| `ACTIVE_SET_HALO` | 2 | BFS halo rings around wet/sourced seeds |
| `LINEAR_SOLVER` | GMRES | Only GMRES wired; BICGSTAB/TFQMR parse but are rejected at initialize |
| `PRECONDITIONER` | AMG | AMG (hypre BoomerAMG) when built with `OPENSWMM_WITH_HYPRE`, else silently degrades to JACOBI; NONE/JACOBI always available; ILU parses but is rejected |
| `RAINFALL_MODE` | NATURAL_NEIGHBOUR | NATURAL_NEIGHBOUR / SYSTEM (uniform gage mean) / NONE (avoid double counting when subcatchments already capture rain) |
| `REPORT_2D` | YES | Refresh output gradient fields per accepted window |
| `OUTPUT_FILE` | (empty) | HDF5 output path; empty = no 2D output. Resolved relative to the `.inp` directory. Presence auto-injects `Default2DOutputPlugin`. |

**Notable gap:** `SolverOptions2D` documents `INTEGRATOR` and `MOMENTUM` as "Parsed from [2D_OPTIONS]", but `parse2DOptionsLine` / `is2DOptionKey` have **no such keys** — the CVODE↔ARKODE integrator choice and the DW↔INERTIAL momentum closure are today selectable **only via environment variables** (`OPENSWMM_2D_INTEGRATOR`, `OPENSWMM_2D_MOMENTUM`). An `.inp` cannot request the inertial solver.

### 2.3 Environment-variable overrides

All resolved at `initialize()` / solver construction; env always wins over the `.inp` value.

| Variable | Effect |
|---|---|
| `OPENSWMM_2D_INTEGRATOR` | `cvode` \| `arkode` |
| `OPENSWMM_2D_MOMENTUM` | `dw` \| `inertial` (inertial forces ARKODE) |
| `OPENSWMM_2D_RAINFALL_MODE` | `natural` \| `system` \| `none` |
| `OPENSWMM_2D_FLUX_DH_EPS` | Override flux regularization (0 = bare √) |
| `OPENSWMM_2D_COUPLING_WINDOW` | Override advance window |
| `OPENSWMM_2D_ACTIVE_SET`, `_HALO`, `_EPS` | Masking on/off, halo rings, wet threshold |
| `OPENSWMM_2D_LIVE_COUPLING` | Opt-in live (in-RHS) junction coupling (§6.4) |
| `OPENSWMM_2D_BACKEND` | `cpu` \| `auto` \| `omp` \| `cuda` \| `hip` \| `sycl` |
| `OPENSWMM_2D_MIN_PARALLEL_CELLS` | Small-mesh gate for plugin backends (default 20 000 cells) |
| `OPENSWMM_GPU_PLUGIN_PATH` | Plugin search path (else next to the engine library, and `<libdir>/gpu`) |
| `OPENSWMM_2D_GRAVITY_IMPLICIT` | Inertial mode: force the Schur-coupled implicit-gravity comparison path |
| `OPENSWMM_2D_AMG_BYPASS_FRAC` | Active-set fraction below which AMG psetup falls back to Jacobi (default 0.05) |

### 2.4 Backend / integrator selection (`SurfaceSolverFactory.cpp`)

Selection order in `makeSurfaceSolver`:

1. **Inertial momentum requested** → prefer a Kokkos inertial GPU/OMP plugin (`openswmm_make_gpu_inertial_solver`, probing `cuda → hip → sycl → omp`), else serial `ArkodeSurfaceSolver`. Small-mesh gate applies in `auto` mode.
2. **ARKODE requested (DW split)** → serial `ArkodeSurfaceSolver` directly (ARKODE has no device plugin).
3. **Default (CVODE)** → in `auto` mode, below `OPENSWMM_2D_MIN_PARALLEL_CELLS` cells stay on serial `CvodeSurfaceSolver`; otherwise probe `cuda → hip → sycl → omp` shared-library plugins (C ABI: `openswmm_gpu_probe` + `openswmm_make_gpu_surface_solver`, ABI-version checked, device count > 0 required), falling back to serial CPU. Plugin handles are cached for process lifetime.

The base/portable build ships no plugins, so a stock install always runs the serial CVODE solver. OpenMP threading of the serial solver's per-cell loops is separate: `options_.num_threads` is resolved from the global `[OPTIONS] THREADS` with a small-mesh gate (`n_triangles < 4·threads ⇒ serial`), and all parallel loops are `schedule(static)` with per-cell writes only, so results are bit-identical to serial (the OpenMP `N_Vector` is used when threads > 1, changing only reduction order within solver tolerance).

### 2.5 Units

- **Coupling factors** (always ft⇄m, regardless of project units): `len_1d_to_2d = 0.3048`, `vol/flow_1d_to_2d = 0.3048³ ≈ 0.02832`, and inverses. Set unconditionally in `initialize()`.
- **Mesh scaling**: US projects get an in-place ft→m scale of `vx/vy/vz` and coupling areas (×0.3048²) *before* topology build; skipped for SI projects and for meshes declaring `;; UNITS: SI (m)` (`prescan2DUnitsHeader`). `mesh_scaled_to_si` makes repeat initialization idempotent and lets serializers un-scale.
- **Rainfall**: gage values converted in/hr or mm/hr → m/s before interpolation.
- **1D-side bookkeeping**: exchange volumes are carried in 1D internal ft³ (`nodes.coupling_volume`), converted at the boundary in both directions.

---

## 3. Mesh and Data Model

### 3.1 Mesh topology (`MeshBuilder.cpp`)

Unstructured triangle mesh in structure-of-arrays layout. `buildMeshTopology`:

- Neighbour adjacency by hashing sorted vertex pairs (edge `e` of a triangle is *opposite* local vertex `e`; `-1` = boundary edge).
- Cell geometry: centroid (arithmetic mean of vertices, including `tri_cz` = mean vertex z) and planimetric area via cross product.
- Edge geometry per (tri, edge) slot: midpoint, length, outward unit normal (orientation fixed against the centroid). Interior edges are stored **redundantly** — one slot per incident cell — which makes the RHS a race-free per-cell gather.

`validateMesh` rejects out-of-range/duplicate vertex indices, non-positive areas, and non-positive Manning's n.

### 3.2 Vertex reconstruction stencils (`VertexReconstruction.cpp`)

Each vertex gets a CSR stencil over its incident triangles with **pseudo-Laplacian weights** (Holmes–Connell style, with the Jawahar–Kamath negative-weight clipping + renormalization; uniform fallback for degenerate/collinear stencils). `reconstructVertexHeads` gathers `vert_head[v] = Σ w_k · head[cell_k]` each RHS call. Vertex heads drive the vertex-coupled exchange and HDF5 `Mesh2_node_head` output.

A documented pitfall (and the reason for several guards in the coupling): the reconstruction averages neighbour *cell* heads and ignores the vertex's own z, so a vertex carved below its neighbours reads a spuriously high head on a dry mesh, and a bowl-bottom vertex can read zero depth on a wet bowl. The coupling therefore never uses `vert_head − vz` as a wetness measure (§6).

### 3.3 State (`SurfaceStateData.hpp`)

Per-cell: `volume` (the integrated state), reconstructed `head`/`depth`, Green-Gauss gradients (raw + limited, output-only), RT0-reconstructed cell velocities, per-cell continuity residual, sources (`rainfall`, `evap_rate`, `coupling_flux` in m/s), per-slot `edge_flux`, forcing override triples (value/mode/persist for rainfall, evaporation, coupling), saved start-of-window state (`old_depth`, `old_volume`), and cumulative envelopes (max depth, max |v|, max |continuity error|, ∫V dt). Non-owning pointers attach the boundary-condition table, the active-set mask, and (live-coupling only) the frozen 1D `NodeData` + coupling-point list.

---

## 4. 2D Formulations

### 4.1 Governing equation — Manning diffusive wave (default)

Semi-discrete finite-volume continuity with the diffusive-wave (zero-inertia) closure:

```
dV_i/dt = Σ_e F_e  +  A_i · ( R_i + C_i − E_i )
```

- `V_i` cell volume (m³), `R_i` rainfall (m/s), `C_i` coupling source (m/s, + into 2D), `E_i` depth-limited evaporation sink.
- Momentum closure: `q = −K·h·∇H` with `K(h,|∇H|) = h^(2/3) / (n·√|∇H|)` (Manning).

**Edge flux** (`computeEdgeFluxes`, SurfaceFluxCalculator.cpp): substituting the two-point FD estimate `∇H·n_e ≈ −(η_L − η_R)/Δx` (Δx = centroid-to-centroid distance) collapses the flux to the well-balanced form

```
F_e = − h_up^(5/3) · sign(Δη) · √|Δη| · ξ_e / ( n_up · √Δx )        [m³/s, inflow-positive]
```

with **hydrostatic upwinding by total head**: the upstream cell (higher η) supplies both depth `h_up` and roughness `n_up`. Properties:

- C-property (flux → 0 as Δη → 0); the removable 1/√|∇H| singularity is cancelled analytically.
- No explicit wet/dry switch: a dry upstream cell has `h_up = 0` ⇒ flux 0; the `(V/A)^(5/3)` conductance shuts off C¹-smoothly as a cell empties.
- **Flat-water regularization** (`regSqrt`): below `FLUX_DH_EPS` (default 4 mm) the √|Δη| is replaced by a C¹ quadratic with finite slope at 0, bounding the transmissivity (and hence the Jacobian and Jacobi preconditioner diagonal) in deep, near-level ponding — the documented deep-water stiffness fix. `FLUX_DH_EPS 0` restores the bare √.
- **Per-edge conveyance** `edge_conveyance ∈ [0,1]` multiplies the flux last (the Integral-Porosity edge transmissivity ψ of Sanders 2008 / Bruwier 2017); mirrored across interior-edge slots so antisymmetry (mass conservation) is preserved; 0 turns an interior edge into a wall.

**RHS assembly** (`assembleRHS`): per-cell gather of the cell's own three edge slots plus `A_i·(rain + coupling − evapSink)`. Because V is the conserved state there is no 1/A anywhere in the flux term — interior fluxes telescope exactly. The IMEX variants `assembleImplicitRHS` (flux divergence only) and `assembleExplicitRHS` (sources only, side-effect-free depth reconstruction) reproduce `assembleRHS` exactly when summed.

**Gradients** (output/API only, not used by the flux): Green-Gauss unlimited gradients with edge-midpoint head averaging, then the Jawahar–Kamath (2000) weighted limiter over the cell + 3 neighbours with `LIMITER_EPSILON` regularization. Refreshed once per accepted window (`refreshOutputGradients`), not inside the RHS.

**Velocity reconstruction** (`computeFaceVelocity`): per-cell least-squares (2×2 normal equations) fit of the specific-discharge vector to the three edge normal fluxes (`b_e = F_e/ξ_e`, clamped to ±10 m²/s against wet/dry-front spikes), then `v = q/h`. Zeroed below `DRY_DEPTH`.

**Per-cell continuity residual** (`computeCellContinuity`): `(V − V_old)/dt − (ΣF + sources)` at the accepted end-of-window state — a first-order diagnostic written to output and tracked as an envelope.

### 4.2 Boundary conditions (`boundaryEdgeFlux`)

Applied at boundary edges when `state.boundary` is attached (else all boundaries are walls):

| Type | Flux into cell i |
|---|---|
| WALL | 0 |
| NORMAL_FLOW | `−(1/n)·h^(5/3)·√S · ξ` (outflow only; S from PARAM_1, S ≤ 0 or dry ⇒ 0) |
| SPECIFIED_FLOW / RATING_CURVE | `−q_bc·ξ` (`q_bc` = outward discharge per metre; rating curves are resolved host-side each step from the boundary cell's lagged stage, then treated as SPECIFIED_FLOW) |
| SPECIFIED_STAGE | Collapsed-Manning flux toward a ghost at `h_bc`, with `Δx_b = 2A/(3ξ)` (centroid→edge distance) and upwinding between the cell depth and `max(h_bc − z_c, 0)` |

Time-series names are resolved to table indices lazily on the first advance (`resolveBoundaryValues`, `-2` = pending, `-1` = not found ⇒ constant). Boundary outflow is integrated per window into `edge_bc_cum_flux` (end-of-window flux × dt, skipped for failed/quiescent windows) and feeds the global mass balance.

### 4.3 Evaporation and rainfall

- `evapSink(rate, depth, dry_depth)` is a depth-limited sink (Hermite ramp below `DRY_DEPTH`) so evaporation can never drive V negative. **Currently the 1D climate evaporation is not broadcast to the mesh** — `evap_rate` is zeroed each window and only the runtime forcing API can populate it (noted in `fireAdvanceWindow`).
- Rainfall: per-gage current intensity → m/s, then either the precomputed **natural-neighbour (Laplace) interpolation** at cell centroids (IDW power-2 extrapolation outside the gage hull; weights built once at initialize since gage positions are static) or the uniform all-gage mean (`SYSTEM`, also the automatic fallback when no gage has `[SYMBOLS]` coordinates), or `NONE`.

### 4.4 Time integration — CVODE BDF (default path)

`CvodeSurfaceSolver` wraps CVODE (SUNDIALS):

- **Method**: BDF with the default Newton corrector; matrix-free Jacobian (J·v by finite-difference of the RHS); linear solver SPGMR (left preconditioning), Krylov dim = `MAX_KRYLOV_DIM`.
- **State vector**: `[V_0..V_{nt−1}]` plus, on the live-coupling path, one `∫Q dt` quadrature accumulator per non-outfall coupling point (rows with identity preconditioner and 1e30 atol so they never constrain the step).
- **Error control**: `rtol = REL_TOLERANCE`, per-cell `atol_i = ABS_TOLERANCE·A_i` — i.e. control each cell's mean-depth error to `ABS_TOLERANCE`, with the relative term letting deep water relax (the code documents this as the fix that replaced the old head-state atol hack).
- **Step bounds**: `MIN_TIMESTEP` / `MAX_TIMESTEP` / `MAX_CVODE_STEPS`; `CVodeSetStopTime` guarantees exact window arrival. A negative CVode return leaves the state untouched and reports failure to the router.
- **Preconditioning**:
  - *Jacobi*: per-cell diagonal heuristic `D_i ≈ −(Σ_e T_e)/A_i` with per-edge transmissivity `T_e = |F_e| / max(|Δη_e|, 1e−9)` read from the last RHS evaluation; `psolve` applies `z_i = r_i/(1 − γD_i)` (m ≥ 1 since D ≤ 0). Lagged via CVODE's `jok` flag.
  - *AMG* (default when hypre is built): assembles `M = I − γJ` over the static diffusion sparsity and applies one BoomerAMG V-cycle; hierarchy rebuilt only when `jok = SUNFALSE`. With active-set masking on a mostly-dry mesh (< `OPENSWMM_2D_AMG_BYPASS_FRAC` of cells active) psetup falls back to the near-exact Jacobi diagonal.
- **RHS pipeline per evaluation**: reconstruct (η, h̄) from V → reconstruct vertex heads → edge fluxes → assemble RHS → (live path) evaluate + scatter node-coupling orifice fluxes and set the accumulator derivatives `dA_k/dt = Q_k`.

### 4.5 Time integration — ARKODE IMEX (opt-in)

`ArkodeSurfaceSolver` uses ARKStep (additive Runge–Kutta, order set to 3) with two RHS splits:

- **DW split**: implicit `F_I` = the stiff flux divergence (+ live coupling); explicit `F_E` = the non-stiff source forcing. Same preconditioners as CVODE.
- **Local-inertial (`MOMENTUM=INERTIAL`)** — LISFLOOD-FP style, state `[V(nt), q(ne), accumulators]` where q is a single prognostic discharge (m²/s) per **unique interior edge** (`InertialEdges`: canonical `cL=min, cR=max` orientation, per-cell CSR incidence with ±1 signs; boundary edges carry no q — walls in this phase).
  - Continuity: `dV_i/dt = −Σ_e sign_i(e)·q_e·ξ_e + sources`.
  - Momentum per edge, with flow depth `h_f = max(η_L, η_R) − z_face`, `z_face = max(z_c,L, z_c,R)`:
    - gravity: `−g·h_f·(η_R − η_L)/Δx`
    - friction: `−g·n̄²·q|q| / h_f^(7/3)` (n̄ = mean of the two cells' Manning n)
    - dry interface (`h_f ≤ DRY_DEPTH`): `dq/dt = −q` (bleed residual discharge).
  - **Default split** (explicit gravity): gravity + continuity transport are explicit, only the diagonal per-edge friction is implicit ⇒ the block preconditioner is *exact* and there is no global solve (O(n), documented to scale to ~1M cells). `OPENSWMM_2D_GRAVITY_IMPLICIT` switches to the fully coupled comparison path, preconditioned by an exact friction-damped q-diagonal `w_e = 1/(1+γR_e)`, `R_e = 2gn̄²|q_e|/h_f^(7/3)`, plus the *diagonal* of the Schur complement `S = I − γ²·D·diag(w)·Grad` on V.
  - After each advance, q is projected back onto the redundant `edge_flux` slots (`±q_e·ξ_e`) so continuity/velocity/mass-balance diagnostics stay consistent; the router skips its DW edge-flux recompute in this mode.

### 4.6 Dry-cell active set (opt-in, CVODE + DW only)

`ActiveSetBuilder` rebuilds a mask once per fired window: seeds = cells with `V > wet_eps·A`, nonzero rainfall or coupling flux, any non-WALL boundary edge, and (live-coupling only) every coupling stencil; then BFS halo expansion (`ACTIVE_SET_HALO` rings). Frozen cells get `ydot ≡ 0` — *exactly* their unmasked value (dry, source-free, walled) — so CVODE sees identical arithmetic and needs no reinitialization; a seed pass (`seedInactiveState`) pre-fills frozen cells/vertices with exact dry values (terrain gradients, bed-level vertex heads). An active→inactive edge is treated as a wall (conservative). The wet threshold sits an order of magnitude above `ABS_TOLERANCE` so solver noise films don't read as wet.

**Breach handling** (in `fireAdvanceWindow`): if any outer-ring cell wets within one window, the window is discarded, the start state restored, the halo doubled (cap 16), and the window redone once; a second breach keeps the walled (mass-safe) result and disables masking for the rest of the run.

### 4.7 CFL hint back to the 1D solver

The 2D interior is fully implicit and imposes no routing-step limit; only the **explicit exchange** does. `updateCflHint` therefore scans *only the coupling-stencil cells* (`cfl_cells_`) for `dt_i = √A_i / √(g·h_i)` (wet cells only) and caches the minimum once per advance. `SWMMEngine` (line ~898) takes `dt = min(dt_1D_CFL, hint)` before `TimestepController::compute_next` each routing step.

---

## 5. 2D Lifecycle Within the Engine

### 5.1 Initialization order (`SurfaceRouter2D::initialize`, called from `SWMMEngine::initialize`)

1. Activation check (≥3 vertices, ≥1 triangle) — else the module stays inactive.
2. Fixed ft⇄m coupling factors; FLOW_UNITS-driven mesh scaling to SI (skipped for SI-authored meshes; idempotent).
3. `buildMeshTopology` → `validateMesh` → `buildVertexStencils`.
4. Rainfall-mode env override; build static rainfall interpolation weights from `[SYMBOLS]` gage coordinates.
5. Resolve deferred coupling node names → indices (unknown name ⇒ hard error).
6. Size state; drain pending BC/conveyance rows; seed `head = tri_cz` (dry).
7. `buildCouplingPoints` (vertex- and triangle-coupled descriptors with outfall/flap-gate flags); build `cfl_cells_` from coupling stencils.
8. Opt-in live-coupling list published on the state (before solver init, which sizes the augmented vector).
9. **Coupled-junction preparation** (§6.1): flag `ctx.coupled_node`, override `ponded_area` with the median-dual 2D footprint, warn on overridden user `ponded_area`/`sur_depth`.
10. **Vertical-datum guard**: warn if a coupled node's rim (invert+MaxDepth, →m) sits outside the mesh elevation envelope ± max(10 m, 10× relief) — a datum/units mismatch would otherwise drive massive spurious exchange.
11. Resolve OpenMP thread count; attach boundary table; seed output gradients.
12. Construct the integrator via the factory; `solver_->initialize(...)`.
13. Seed the 2D mass-balance ledger (`ctx.mass_balance_2d`), the CFL hint, the active set (seed pass then enable), and resolve the effective advance window (`COUPLING_WINDOW` / AUTO / legacy interval).

### 5.2 Per-routing-step sequence (`SWMMEngine::stepRouting`, ~lines 2500–2660)

```
B2   clearInflowSources; compute inflows (external, DWF, RDII, iface)
B2c  assembleLateralInflows(dt):
        coupling_inflow[j] = coupling_volume[j] / dt ; coupling_volume[j] = 0
        lat_flow[j] += ... + user_lat_flow[j] + coupling_inflow[j]
        (positive coupling_inflow booked as routing_external; negative side
         accounted as routing_flooding in updateRoutingMassBalance)
B2c' surface_router_.updateOutfallsPreRouting(ctx)      ← cache h_2d & wet ramp per outfall
B2d  steady-state short-circuit (skips routing, not the 2D hooks below it)
B3   router_.step(...)  — 1D dynamic-wave Picard iteration
        (Outfall::setAllOutfallDepths inside each iteration applies the 2D
         tailwater blend; DynamicWave honours coupled_node ponding)
B3+  surface_router_.advancePostRouting(ctx, dt, t)     ← accumulate window / fire 2D advance
B3a… inlets, culverts, reporting, etc.
```

And at the top of every engine step, the cached 2D CFL hint clamps the adaptive routing step.

### 5.3 Macro-step windowing (`advancePostRouting` → `fireAdvanceWindow`)

Routing time is accumulated in `pending_dt_`; the 2D solver fires one `advance()` over the **whole accumulated window**:

- **Time-based gating** (default): fire when `pending_dt_ + 0.5·dt_routing ≥ effective_window_`. AUTO resolves the window to `min(ROUTING_STEP, MAX_TIMESTEP)`, so healthy models fire every step, while 1D variable-step collapse cannot drag the (expensive) 2D advance cadence down with it.
- **Legacy step-count gating**: `COUPLING_INTERVAL > 1` with no window set.
- Sources (coupling, outfall transfer, rainfall, forcings) are **held constant across the window**, and every downstream term uses the same accumulated dt, so the exchange is conservative over the macro step. The code explicitly warns that a >1 interval is an explicit coupling sub-cycle and CFL-limited.
- **Stability guard**: a failed advance halves `effective_window_` (floor 1 ms); 20 consecutive clean windows double it back toward the resolved target.
- **Quiescence short-circuit**: a window with no water, no nonzero source, and only WALL/NORMAL_FLOW boundaries skips the solver entirely (held exchanges are un-booked); dominant saving in continuous dry-weather simulation.
- **Failure handling**: if the integrator cannot reach the target (e.g. `MAX_CVODE_STEPS` exhausted), the 2D surface is held frozen for the window, the integrator is re-synced (`reinitialize`), and the held exchanges (junction `coupling_volume`, outfall `applied_q`) are **un-booked** so neither domain receives water the other never moved. Counted and reported at finalize.
- `finalize()` flushes any partial pending window so the 2D clock and ledgers end at simulation end; `prepareOneShotForcing` flushes then forces the next window so a RESET forcing applies to future time only.

### 5.4 Fired-window internals (order matters)

1. `save_state()` (start-of-window V, h̄ for continuity/rollback).
2. Junction exchange: held path `computeCouplingExchange` (writes `coupling_flux` on cells + `coupling_volume` on nodes) or live path (just zero `coupling_flux`).
3. `transferOutfallDischarges` (held, both paths) with withdrawal cap; clamp windows counted.
4. `updateRainfall`; zero `evap_rate`; apply per-cell runtime forcings (OVERRIDE/ADD for rainfall, evaporation, coupling).
5. `resolveBoundaryValues(t)` (time series, rating curves).
6. Quiescence check → maybe skip. Active-set rebuild.
7. `solver_->advance(t, t+dt)` (+ breach retry / failure freeze as above).
8. Live-path booking: `coupling_volume[node] += ∫Q dt · flow_2d_to_1d`.
9. Refresh output gradients; recompute edge fluxes at the accepted state (skipped in INERTIAL mode where q was projected instead) so fluxes, continuity and velocities are consistent with the reported solution.
10. Integrate boundary cumulative fluxes; `sim_time_ += dt`; per-cell continuity; velocities; statistics envelope; `accumulateMassBalance`; refresh CFL hint; clear RESET forcings.

---

## 6. 1D–2D Coupling Formulations (`coupling/NodeCoupling.cpp`)

### 6.1 Coupling points and coupled-node preparation

A `CouplingPoint` maps a mesh **vertex** (with a CSR stencil of incident cells) or a **triangle centroid** to a SWMM node, with a discharge coefficient `cd` (default 0.65) and an effective exchange area `area` (m² after scaling). Outfall nodes are flagged, along with their flap gates. Vertex coupling reads its head from the reconstructed `vert_head`; triangle coupling from the cell head.

At initialize, every coupled **junction** (non-outfall) gets:

- `ctx.coupled_node[j] = 1` — the dynamic-wave solver then treats the node as pond-capable regardless of the global `ALLOW_PONDING` option (checked at three sites in `DynamicWave.cpp`: surface-area baseline, `setNodeDepth` flooding logic, and depth-limit logic).
- `ponded_area` **overridden** with the median-dual footprint of the coupling stencil (Σ incident tri areas / 3 for a vertex; the cell area for a triangle), converted m²→ft². Rationale in-code: the 1D HGL must be able to rise above the crown to track the 2D surface so the exchange can fire; the median-dual share keeps the double-counted near-manhole storage minimal. User-set `ponded_area`/`sur_depth` trigger warnings.

### 6.2 Junction exchange — capped-pipe bidirectional orifice

Driving head: `Δh = h_2d − h_1d` (both in metres; `h_1d = nodes.head·0.3048`).

**Orifice law with C¹ regularization:**

```
Q = Cd · A_eff · sign(Δh) · √(2g) · φ(|Δh|)          [m³/s;  >0 = 2D→1D drain, <0 = 1D→2D spill]

φ(x) = √x                        for x ≥ ε   (ε = ORIFICE_H_EPS = 0.02 m)
φ(x) = (3/(2√ε))x − (1/(2ε^1.5))x²  for x < ε   (matches √ in value & slope at ε; finite slope at 0)
```

The regularization removes the `dQ/dΔh → ∞` singularity exactly in the weir-equilibrium regime where the two heads hover near-equal — the main stiffness/oscillation source for an explicitly-coupled exchange.

**Effective area** ramps from the inlet area to a "manhole" area = 2×`area` over a 5 cm band once `max(h_1d, h_2d)` exceeds the ground/crown (`effectiveArea`).

**Capped-pipe gate:** no exchange at all until water reaches the **crown** `z_top = (invert + full_depth)` — deliberately the same threshold at which the 1D dynamic wave engages its Preissmann slot, and deliberately *not* `crown + sur_depth` (that headroom stays available as slot storage). The gate is a C¹ Hermite smoothstep over a 5 cm band above the crown, applied on `max(h_1d, h_2d)`.

**Wet/dry self-limiting:** Q is multiplied by a Hermite ramp `smoothstep(depth_source / DRY_DEPTH)` on the *source* side — for drains, the max depth over the vertex stencil (robust against the two vertex-reconstruction failure modes documented in the code: false-positive depth at a low-spot vertex on a dry mesh, false-zero depth at a bowl-bottom vertex on a wet mesh); for spills, the 1D node depth.

**Hard caps (held path only — the discrete safeguards):**

- Drain (Q>0): capped by remaining node capacity `((full_volume − volume)·ft³→m³)/dt`; if the node is full, drain allowed only when `h_1d < h_2d` (surcharge drain-back). Also capped by the **signed** water volume actually present in the receiving stencil/cell `max(0, ΣV)/dt` — signed so an over-drawn negative cell tightens the cap and recovers instead of running away.
- Spill (Q<0): bounded by the node's **flooded store above the crown** `max(0, volume − full_volume)·ft³→m³ / dt` — in-line (below-crown) pipe flow is not spillable.

**Booking:** `nodes.coupling_volume[node] += Q · flow_2d_to_1d · dt` (a *volume*, ft³; multiple points on one node accumulate). The 2D side receives the mirror image via `scatterCouplingFlux(…, −Q)`.

**Stencil scatter:** triangle points inject into their single cell (`Q/A`). Vertex points spread Q across the stencil weighted by the **upwind HGL slope** from the vertex to each cell centroid — sources go downhill (`w_k ∝ max(0,(h_v−h_k)/d_k)`), sinks pull uphill — normalized to Σw=1 (exactly conservative), with the geometric partition-of-unity weights as the flat-surface fallback.

### 6.3 Exact 1D-node ↔ 2D exchange data path (junctions, held path)

This is the complete, code-exact lifecycle of one junction exchange — what each side reads, what each side writes, in what units, and when. File anchors: `coupling/NodeCoupling.cpp` (`computeCouplingExchange`, `scatterCouplingFlux`), `solver/SurfaceFluxCalculator.cpp` (`assembleRHS`), `core/SWMMEngine.cpp` (`assembleLateralInflows`, ~line 5265).

**Stage 0 — inputs sampled at window start.** When a 2D advance window fires (`fireAdvanceWindow`), `computeCouplingExchange` reads, per coupling point:

| Quantity | Source | Units | Notes |
|---|---|---|---|
| `h_2d` | `state.vert_head[v]` (vertex point) or `state.head[cell]` (triangle point) | m | End state of the *previous* 2D window |
| `h_1d` | `nodes.head[ni] · len_1d_to_2d` | ft → m | End state of routing step N; **frozen** for the whole window |
| `depth_2d_avail` | max depth over the vertex stencil (or the single cell) | m | Wetness for the drain-side ramp — never `vert_head − vz` (see §3.2 pitfalls) |
| `depth_1d_avail` | `nodes.depth[ni] · len_1d_to_2d` | ft → m | Wetness for the spill-side ramp |
| Capacity terms | `nodes.volume`, `nodes.full_volume` (·`vol_1d_to_2d`) | ft³ → m³ | For the drain capacity cap and the spill flooded-store cap |

**Stage 1 — compute Q (m³/s, SI).** The capped-pipe orifice of §6.2, evaluated once per window: `Q = Cd·A_eff·sign(Δh)·√(2g)·φ(|Δh|)`, gated at the crown, wet/dry ramped on the source side, then hard-capped (node capacity for drains; stencil water availability for drains; node flooded store for spills). Sign convention: **Q > 0 = 2D→1D drain into the pipe; Q < 0 = 1D→2D spill onto the surface.**

**Stage 2 — write to the 2D side (a held rate).** `scatterCouplingFlux(mesh, state, cp, −Q)` — note the sign flip: a drain (Q>0) is a *sink* on the surface. Triangle points put everything in one cell; vertex points distribute over the stencil with upwind-HGL-slope weights:

```
source into 2D (−Q > 0):  w_k ∝ max(0, +(h_vert − h_k)/d_k)   (downhill cells receive)
sink out of 2D (−Q < 0):  w_k ∝ max(0, −(h_vert − h_k)/d_k)   (uphill cells supply)
Σ w_k = 1 (normalized);   flat surface / no upwind cell ⇒ fall back to vert_stencil_wt
coupling_flux[k] += (−Q · w_k) / A_k        [m/s — a depth rate per cell]
```

Because Σw = 1, the injected volume over the window is exactly Q·dt regardless of stencil size. `coupling_flux` is then **held constant** for the entire window; every solver RHS evaluation adds `A_i · coupling_flux[i]` to `dV_i/dt` (`assembleRHS`), and the water spreads to further cells only through the ordinary interior edge fluxes.

**Stage 3 — write to the 1D side (a carried volume).** In the same pass:

```
nodes.coupling_volume[ni] += Q · flow_2d_to_1d · dt        [ft³ — a VOLUME, not a rate]
```

(`flow_2d_to_1d ≈ 35.31` m³/s → ft³/s; ·dt makes it ft³.) Points sharing a node accumulate via the `+=`; the array was zeroed for every coupled junction at the top of `computeCouplingExchange`.

**Stage 4 — delivery to the routing solver (smoothed queue, 2026-07-18).** At the end of the fired window, `fireAdvanceWindow` moves each node's `coupling_volume` into a delivery queue and stamps the window length:

```
coupling_queue[j] += coupling_volume[j] ;  coupling_volume[j] = 0
coupling_delivery_remaining = dt_window          (SimulationContext scalar)
```

On each following routing step, `SWMMEngine::assembleLateralInflows(dt_routing)` drains the queue at the **uniform rate** `queue / remaining` (flushing the whole remainder on the step where `remaining ≤ dt_routing`, then counting `remaining` down once per step):

```
coupling_inflow[j] = queue[j] / remaining ;  queue[j] -= coupling_inflow[j]·dt
lat_flow[j]       += … + user_lat_flow[j] + coupling_inflow[j]
```

The dynamic-wave solver sees the exchange purely as lateral inflow at the node — positive adds inflow, negative withdraws. Carrying a volume (not a rate) across the step boundary is the conservation mechanism (`VARIABLE_STEP`-safe), and the uniform drain spreads a multi-step macro window's volume over the window instead of dumping it into one routing step — the earlier one-step delivery arrived as a `window/routing_step`-sized pulse (×5 for a 10 s window over a 2 s step) that instantly flooded small junctions and drove a per-window drain/spill churn. When the 2D advance fires every routing step (the AUTO default), `remaining ≤ dt` on the first delivery step and the behaviour is byte-for-byte the legacy one-step delivery.

**Stage 5 — continuity accounting.** The positive (2D→1D) side of `coupling_inflow` folds into the `routing_external` inflow category; the negative side (1D→2D spill) is accumulated as `routing_flooding` in `updateRoutingMassBalance`. On the 2D ledger, `accumulateMassBalance` books the *same* per-node volume (deduped by node, ft³ → m³) as `coupling_2d_to_1d_out` / `coupling_1d_to_2d_in` — both ledgers see the identical clamped volume, delivered once. The code notes this replaced an earlier design that routed coupling through the user-forcing lateral-inflow API (which conflated user forcing with coupling and dropped the negative side from continuity).

**Lifecycle of one exchange (note the one-step lag):**

```
routing step N ends ──► fireAdvanceWindow (window N)
                          Q from (h_2d end of window N−1, h_1d end of step N)
                          2D side: coupling_flux held rate ──► integrated by solver over window N
                          1D side: coupling_volume += Q·dt (ft³)
routing step N+1 begins ─► assembleLateralInflows: lat_flow += coupling_volume/dt_{N+1}
                          1D DW solve consumes the exchange; coupling_volume cleared
```

The lag is inherent to the operator splitting and conservative by the volume-carry design; the un-booking on failed/quiescent windows (§5.3) guarantees a volume is never delivered to the 1D node when the 2D surface did not actually move it (and vice versa).

**Supporting mechanics at the node** (set up at initialize, §6.1): `ctx.coupled_node[j] = 1` lets the node pond above its crown regardless of `ALLOW_PONDING`, and `ponded_area` is overridden with the median-dual footprint of the coupling stencil (m² → ft²) — together these let the 1D HGL rise to track the overlying 2D surface, keeping the driving head Δh = h_2d − h_1d physical rather than pinned at the crown.

### 6.4 Live (in-RHS) junction coupling — opt-in `OPENSWMM_2D_LIVE_COUPLING`

Instead of a held per-window flux, `computeNodeCouplingQ` evaluates the same capped-pipe orifice **inside every solver RHS call** against the *live* 2D head (the 1D node head stays frozen for the window), and `scatterCouplingToYdot` adds it directly to the cell derivatives. The wet/dry ramp on the live source depth makes Q self-limit smoothly as a cell drains, so no discrete avail/dt caps are needed and the stiff exchange is integrated implicitly and stably over a large macro-window. Conservation is exact by construction: each point's `∫Q dt` is carried as an extra quadrature row in the state vector; after the advance the per-window delta is booked to `coupling_volume` (ft³) and to the 2D ledger.

Relative to the exact data path of §6.3, the live path changes stages 1–3: Q is re-evaluated at *every* RHS call from the live 2D head (`computeNodeCouplingQ`, no discrete avail/dt caps — the wet ramp self-limits), `scatterCouplingToYdot` adds `−Q·w_k` straight into the cell derivatives (m³/s, same upwind weights) instead of a held `coupling_flux` rate, and the accumulator rows integrate `dA_k/dt = Q`. Stage 3's booking moves to after the advance: `coupling_volume[ni] += (A_k(end) − A_k(start)) · flow_2d_to_1d`. Stages 4–5 (delivery via `assembleLateralInflows`, continuity split) are identical.

In-code caveat: the path is *correct but currently slow* — the orifice is stiff and the diffusion-stencil preconditioner does not capture its Jacobian, so CVODE cannot take the large steps; a coupling-aware preconditioner is called out as the missing piece. Outfalls always stay on the held path.

### 6.5 Outfall coupling

Outfalls couple through a **head boundary condition**, not the orifice:

**Pre-routing** (`updateOutfallBoundaries`, called before the 1D step): for each coupled outfall, cache into the outfall side-table (`NodeSubtypes.outfalls`):

- `head_2d` — the 2D stage at the coupling point, in ft. For vertex coupling this is deliberately **not** `vert_head` but the head of the *deepest wet stencil cell* (a dry stencil falls back to the vertex bed), avoiding the phantom-tailwater failure mode of the pseudo-Laplacian at carved-down outfall vertices.
- `ramp_2d` — a C¹ wet/dry blend factor `smoothstep((depth_2d − dry_depth)/dry_depth)`: zero at the resting film a draining cell leaves at ~`DRY_DEPTH` (so a dry outfall reverts to its legacy free-discharge stage instead of deadlocking at bed level), 1 once the cell holds ≳ 2·dry_depth of real water.

**Inside the 1D solve** (`Outfall::setAllOutfallDepths`, Outfall.cpp ~line 328): on every Picard iteration, if `h_2d > z_inv` and `ramp_2d > 0`, and the flap gate (if any) is not closed (`flap closed ⇔ h_2d > h_standard`), the prescribed stage becomes `h_standard + ramp_2d · max(0, h_2d − h_standard)` — at ramp 1 this reproduces `max(h_standard, h_2d)` dynamic tailwater. The design note in the code: writing `nodes.head` directly from the 2D module was a no-op because `setAllOutfallDepths` re-runs each iteration; hence the side-table + apply-at-use-site pattern. A user-prescribed HGL forcing still overrides everything.

**Post-routing** (`transferOutfallDischarges`, held path in every window): the *net* 1D result at the outfall, `Q_net = (inflow − outflow)·ft³/s→m³/s` (positive = pipe discharging onto the surface; negative = tailwater-driven backflow into the pipe, which the head BC produced naturally), is scattered into the coupling cell(s) with the same upwind stencil weighting. Withdrawal (`Q_net < 0`) is capped by the signed water available in the receiving stencil (`max(0, ΣV)/dt`); clamped windows are counted and reported at finalize. The **applied** (clamped) rates — not the raw 1D rates — are what the mass-balance ledger books (`outfall_applied_q_`).

### 6.6 Coupled mass balance (`accumulateMassBalance` + `MassBalance2D`)

Per fired window (skipped when the advance failed): rainfall in (Σ R·A·dt), evaporation out (depth-limited sink at accepted depths), junction exchange (from `coupling_volume`, deduped per node, ft³→m³ — booked as `coupling_2d_to_1d_out` / `coupling_1d_to_2d_in`), outfall exchange (from applied q·dt — `outfall_in`/`outfall_out`), boundary in/out (delta of Σ`edge_bc_cum_flux`), and final storage (ΣV). The ledger is symmetric with the 1D side because both book the *same* volumes (the applied/clamped exchange, delivered once), and the report carries a 2D continuity error. Diagnostics: outfall-clamp windows, failed windows, quiescent windows, active-set halo trips.

---

## 7. Output & Runtime API

- **`Default2DOutputPlugin`** (auto-injected when `OUTPUT_FILE` is set; no `[PLUGINS]` entry needed): CF-1.11/UGRID-1.0 HDF5. Static mesh topology once; per report step: time, depth, head, raw+limited gradients, rainfall, coupling flux, net source, cell velocities, per-cell continuity residual, per-slot edge fluxes, vertex heads; fixed-size overwritten envelopes (max depth / max |v| / max |continuity error|); a `/mass_balance_2d` group with continuity error at finalize. Readable in ParaView/QGIS.
- **`Api2D.cpp`** exposes mesh/state/statistics queries and the per-cell runtime forcing API (rainfall / evaporation / coupling with OVERRIDE/ADD × RESET/PERSIST semantics) used by `fireAdvanceWindow`; `prepareOneShotForcing` gives RESET forcings clean window semantics.
- Snapshot plumbing (`SWMMEngine` ~3690) publishes triangle/vertex counts and state arrays to output plugins each report step; `lastCvodeSteps`/`lastCvodeStepSize` surface integrator effort.

---

## 8. Review Observations

Items that stood out during this review — factual observations from the code, flagged for follow-up rather than fixed:

1. **`INTEGRATOR` / `MOMENTUM` are not parseable from `[2D_OPTIONS]`** despite `SolverOptions2D` doc-comments saying they are (§2.2). Env-only selection means a model file cannot declare the ARKODE or local-inertial configuration it was calibrated with.
2. **Doc-comment drift on live coupling**: `SurfaceStateData.hpp` says `node_coupling` is set "ONLY when the macro-step path is active (COUPLING_INTERVAL > 1)", but `SurfaceRouter2D::initialize` enables it purely on `OPENSWMM_2D_LIVE_COUPLING`, regardless of interval/window. Behaviourally fine (the held path is simply skipped), but the comments disagree with the code.
3. **Evaporation coupling is a stub**: `evap_rate` is zeroed every window pending the 1D `ClimateState.evap_rate` broadcast; only runtime forcing can evaporate the mesh today. The mass-balance term is wired and will be exact once the broadcast lands.
4. **`buildCouplingPoints` is O(n_vertices × n_triangles)** (linear scan per coupled vertex to find a containing triangle). Irrelevant at current mesh sizes but quadratic-ish if a large fraction of vertices couple on a big mesh; the vertex stencil (`vert_stencil_ptr/idx`) already has this information.
5. **Interior-edge fluxes are computed twice** (once per incident cell's slot) on the DW path. The redundancy is a deliberate race-free-gather choice and both slots agree analytically (antisymmetric formula, same conveyance mirrored), so this is cost, not correctness.
6. **One-step delivery lag** of the junction exchange (computed in window N, consumed by 1D starting at step N+1) is inherent to the sequencing and conservative by the volume-carry design, but worth remembering when interpreting near-threshold oscillations at short windows. *(2026-07-18: the related single-step pulse-delivery problem — a whole window's volume dumped into one routing step — was fixed by the `coupling_queue` uniform drain, §6.3 Stage 4. `SurfaceRouter2D::initialize` now also warns when a coupling point's exchange area exceeds 10× the largest connected conduit area, the configuration that guarantees drain/spill churn.)*
7. **The steady-state short-circuit** (`isInSteadyState`, B2d) returns before the 1D routing *and* before `advancePostRouting` — during a skipped-routing period the 2D module accumulates no window time. Consistent (no exchange can occur without routing), but the 2D clock advances only when routing runs.
8. **Robustness machinery is extensive and internally consistent**: every exchange path caps withdrawals by signed available volume; failed windows un-book exchanges on both sides; quiescent windows un-book runtime-forced zeroes; clamped outfall rates (not raw 1D rates) are booked. The datum guard, the deepest-stencil-cell outfall stage, and the crown-gated exchange each neutralize a specific failure mode that is documented inline with its history.

---

## 9. Quick Reference — Coupling Sequence Diagram

```
routing step N                                        (1D internal: feet; 2D internal: SI)
│
├─ assembleLateralInflows(dtN)
│     lat_flow += coupling_volume(from window N−1)/dtN     [2D→1D drain / 1D→2D spill]
│
├─ updateOutfallsPreRouting
│     outfalls.head_2d  = stage of deepest wet coupling cell (ft)
│     outfalls.ramp_2d  = C¹ wetness blend
│
├─ 1D dynamic-wave solve (Picard)
│     each iteration: setAllOutfallDepths blends free stage → 2D tailwater (flap-gated)
│     coupled junctions pond above crown via overridden ponded_area (coupled_node)
│
├─ advancePostRouting(dtN)
│     pending_dt += dtN;  fire when window reached:
│        computeCouplingExchange   Q = Cd·A_eff·√(2g)·φ(|Δh|) gated at crown,
│                                  wet-ramped, capacity/availability capped
│                                  → coupling_flux (2D cells), coupling_volume (1D nodes)
│        transferOutfallDischarges Q_net = inflow−outflow → 2D cells (withdrawal capped)
│        rainfall / forcings / boundary resolve
│        solver_->advance(window)  [CVODE BDF or ARKStep IMEX; V-state; live-coupling ∫Q dt]
│        book ledgers, refresh gradients/fluxes/velocities/stats, update CFL hint
│
└─ next step: dt clamped by min(1D CFL, 2D coupling-cell CFL hint)
```
