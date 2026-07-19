/**
 * @file SolverOptions2D.hpp
 * @brief Configuration options for the 2D surface routing solver.
 *
 * @see TWO_DIMENSIONAL_SURFACE_ROUTING_IMPLEMENTATION_STRATEGY.md §2.3
 * @ingroup engine_2d
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_ENGINE_2D_SOLVER_OPTIONS_HPP
#define OPENSWMM_ENGINE_2D_SOLVER_OPTIONS_HPP

#include <cstdint>
#include <string>

namespace openswmm::twoD {

/**
 * @brief Krylov linear solver selector for the BDF + Newton + Krylov stack.
 *
 * Phase 1 wires GMRES only; BICGSTAB and TFQMR are kept as enum values to
 * preserve the input-file parsing surface and to mark slots reserved for
 * possible Phase 2 work, but selecting them today triggers a clear
 * runtime error in CvodeSurfaceSolver::initialize().
 *
 * GMRES is the canonical choice for the elliptic-flavoured diffusive-wave
 * Jacobian and pairs cleanly with multigrid preconditioners (the Phase 2
 * BoomerAMG path); the other two Krylov methods would only earn their keep
 * for problem classes we do not currently solve.
 */
enum class LinearSolverType : int8_t {
    GMRES    = 0,   ///< Phase 1: WIRED (SUNLinSol_SPGMR).
    BICGSTAB = 1,   ///< Reserved; rejected at initialize() in Phase 1.
    TFQMR    = 2    ///< Reserved; rejected at initialize() in Phase 1.
};

/**
 * @brief Preconditioner selector for the Krylov inner solver.
 *
 * NONE (no preconditioning) and JACOBI (per-cell diagonal approximation
 * rebuilt each Jacobian refresh) are always available. AMG (hypre BoomerAMG)
 * is wired only when the engine is built with OPENSWMM_WITH_HYPRE; selecting
 * it otherwise triggers a clear runtime error in the solver's initialize().
 * ILU remains a reserved-but-rejected slot.
 *
 * Tier rationale (see also the Phase 1/2 discussion in
 * docs/2D_KNOWN_STIFFNESS_ISSUE.md):
 *
 *   - NONE   : baseline; useful for measuring how much the Jacobi heuristic
 *              actually buys at a given mesh size.
 *   - JACOBI : O(n) setup, O(n) apply, embarrassingly parallel. Effective
 *              while the Newton matrix M = I − γJ is diagonally dominant;
 *              expected to scale acceptably to ~10k–50k cells.
 *   - ILU    : O(nnz) setup + apply via KLU. Better convergence per
 *              Krylov iteration than JACOBI but still asymptotically
 *              non-scalable on this elliptic operator. *Not implemented*.
 *   - AMG    : O(n) setup amortised, near-constant Krylov iterations
 *              regardless of mesh size. The only scalable option past
 *              ~100k cells. Requires hypre/BoomerAMG. *Not yet wired.*
 */
enum class PreconditionerType : int8_t {
    NONE   = 0,     ///< WIRED (no preconditioning).
    JACOBI = 1,     ///< WIRED (diagonal heuristic).
    ILU    = 2,     ///< Reserved; rejected at initialize().
    AMG    = 3      ///< WIRED when built with OPENSWMM_WITH_HYPRE (BoomerAMG).
};

/**
 * @brief Time-integrator selector for the 2D surface ODE.
 *
 * CVODE is the default fully-implicit BDF integrator (CvodeSurfaceSolver).
 * ARKODE selects the ARKStep additive-Runge–Kutta IMEX integrator
 * (ArkodeSurfaceSolver) — the diffusion flux is integrated implicitly while the
 * non-stiff source forcing is explicit. See
 * docs/IMEX_LOCAL_INERTIAL_IMPLEMENTATION_PLAN.md. Orthogonal to the
 * serial/omp/gpu backend selector; ARKODE is CPU-only. The env var
 * OPENSWMM_2D_INTEGRATOR (cvode|arkode) overrides this field.
 */
enum class IntegratorType : int8_t {
    CVODE  = 0,     ///< Default: fully-implicit BDF (CvodeSurfaceSolver).
    ARKODE = 1      ///< ARKStep IMEX additive-RK (ArkodeSurfaceSolver).
};

/**
 * @brief Surface-momentum closure for the 2D flux.
 *
 * DW (default) is the Manning diffusive wave (no inertia; state = cell volume
 * only). INERTIAL adds the LISFLOOD-FP local-inertial momentum: a prognostic
 * per-edge discharge q with implicit gravity + friction, integrated by the
 * ARKStep IMEX solver. See docs/IMEX_LOCAL_INERTIAL_IMPLEMENTATION_PLAN.md §2.
 * Only honored by ArkodeSurfaceSolver; env OPENSWMM_2D_MOMENTUM (dw|inertial)
 * overrides this field.
 */
enum class MomentumType : int8_t {
    DW       = 0,   ///< Manning diffusive wave (default).
    INERTIAL = 1    ///< Local-inertial (LISFLOOD-FP) with per-edge q.
};

/**
 * @brief Volume → free-surface closure for a 2D cell.
 *
 * FLAT (default, legacy) reconstructs η = tri_cz + V/A — exact only for a
 * fully wetted cell. On a partially wet (slope/step-spanning) cell it
 * overstates η by up to two-thirds of the cell relief, which is the driver of
 * the water-climbs-uphill artifact (spurious head pushes thin films upslope;
 * lake-at-rest is not a steady state at shorelines).
 *
 * VFR reconstructs η from the exact stage–storage relation of the plane bed
 * through the cell's three vertex elevations (Begnudelli & Sanders 2006/2007
 * volume/free-surface relationships), C¹-regularized for the implicit solvers
 * by a wetted-area-fraction floor (VFR_MIN_WET_FRAC). Restores the C-property
 * at shorelines. CPU solvers (CVODE/ARKODE) only; the Kokkos GPU backends
 * degrade to FLAT with a one-line notice until ported.
 *
 * Parsed from [2D_OPTIONS] CELL_CLOSURE (FLAT|VFR).
 * See plans/2d/2D_VFR_SOLVER_CLOSURE_PLAN.md.
 */
enum class CellClosure2D : int8_t {
    FLAT = 0,   ///< Legacy flat-cell closure η = tri_cz + V/A (default).
    VFR  = 1    ///< Planar-bed VFR closure (regularized), CPU solvers only.
};

/**
 * @brief Effective conveyance depth at a shared edge for the diffusive-wave flux.
 *
 * MEAN (default, legacy) uses the upwind cell's MEAN depth V/A — blind to
 * where the waterline sits relative to the edge, so a cell with water pooled
 * in its low corner can discharge across an edge whose bed is entirely above
 * the waterline (uphill creep), and drainage strands water on slopes.
 *
 * VFR_FACE reconstructs the depth at the edge from the upwind free surface
 * and the edge's two endpoint bed elevations (Begnudelli & Sanders 2007,
 * Eq. 14, adapted as the Manning conveyance depth): zero when the upwind
 * surface is below the whole edge (no flow — the wetting gate), the exact
 * partially-submerged mean when the waterline crosses the edge. C¹ in η.
 *
 * Parsed from [2D_OPTIONS] FACE_RECONSTRUCTION (MEAN|VFR_FACE).
 * See plans/2d/2D_VFR_SOLVER_CLOSURE_PLAN.md.
 */
enum class FaceDepth2D : int8_t {
    MEAN     = 0,   ///< Legacy: upwind cell-mean depth (default).
    VFR_FACE = 1    ///< B&S Eq. 14 face depth + wetting gate.
};

/**
 * @brief How raingage rainfall is mapped onto the 2D mesh cells.
 *
 * NATURAL_NEIGHBOUR (default) spatially interpolates the located raingages onto
 * every cell centroid — natural-neighbour (Laplace) weights inside the convex
 * hull of the gages, inverse-distance (power 2) extrapolation outside it. The
 * weights are precomputed once in SurfaceRouter2D::initialize() (gage positions
 * are static for a run) and applied each step as a sparse weighted sum.
 *
 * SYSTEM applies one uniform value to all cells: the arithmetic mean of every
 * gage's current rainfall. It is also the automatic fallback when no gage has a
 * map location (no [SYMBOLS] coordinate), since interpolation is then undefined.
 *
 * Parsed from [2D_OPTIONS] RAINFALL_MODE; env OPENSWMM_2D_RAINFALL_MODE
 * (natural|system) overrides at initialize().
 */
enum class RainfallMode : int8_t {
    NATURAL_NEIGHBOUR = 0,  ///< Default: spatial interpolation across all gages.
    SYSTEM            = 1,  ///< Uniform = mean of all gages.
    NONE              = 2   ///< No rain on the mesh. Use when subcatchments
                            ///< already capture the rainfall (runoff → nodes) —
                            ///< rain-on-mesh would double-count the same storm.
};

/**
 * @brief Configuration for the 2D surface routing CVODE solver.
 *
 * Populated from [2D_OPTIONS] input section. Defaults are chosen for
 * typical urban drainage surface routing problems.
 */
struct SolverOptions2D {
    double max_timestep      = 10.0;    ///< Max CVODE internal step (s)
    double min_timestep      = 0.001;   ///< Min CVODE internal step (s)
    double rel_tolerance     = 1.0e-4;  ///< CVODE relative tolerance
    double abs_tolerance     = 1.0e-6;  ///< CVODE absolute tolerance (m)
    double dry_depth         = 0.001;   ///< Dry cell threshold (m)
    double limiter_epsilon   = 1.0e-6;  ///< Slope limiter epsilon
    /// Head-difference regularization (m) for the diffusive-wave flux √|Δη|.
    /// Below this gradient the flux is linearized (C¹) so the transmissivity
    /// stays bounded as the water surface flattens — without it, deep near-level
    /// ponding (e.g. a large design storm draining) makes the flux Jacobian blow
    /// up and the implicit step collapse. Only affects millimeter-scale
    /// gradients, so bulk flow is preserved; raise it for extra robustness on
    /// very deep problems. Default 4 mm; 0 = bare √. Parsed from
    /// [2D_OPTIONS] FLUX_DH_EPS; env OPENSWMM_2D_FLUX_DH_EPS overrides.
    double flux_dh_eps       = 0.004;   ///< Diffusive-flux gradient floor (m)
    double coupling_cd       = 0.65;    ///< Default discharge coefficient
    int    max_krylov_dim    = 30;      ///< Max Krylov subspace dimension
    int    coupling_interval = 0;       ///< 0 = every SWMM step
    /// 2D advance window in SECONDS of simulation time. −1 = AUTO (window =
    /// the nominal [OPTIONS] ROUTING_STEP, clamped to MAX_TIMESTEP); 0 =
    /// advance every routing step; > 0 = explicit window length. A time-based
    /// window is immune to 1D variable-step collapse — a step-count
    /// COUPLING_INTERVAL silently shrinks with the routing step, which is
    /// exactly the regime where the 2D advance cost explodes. An explicit
    /// COUPLING_WINDOW takes precedence over COUPLING_INTERVAL; AUTO defers to
    /// COUPLING_INTERVAL > 1 for backward compatibility. Parsed from
    /// [2D_OPTIONS] COUPLING_WINDOW; env OPENSWMM_2D_COUPLING_WINDOW overrides.
    double coupling_window   = -1.0;
    int    max_cvode_steps   = 500;     ///< Max CVODE steps per advance

    /// Dry-cell active-set masking: restrict the RHS pipeline to wet/sourced
    /// cells plus an ACTIVE_SET_HALO-ring neighbourhood; frozen cells get
    /// ydot ≡ 0 (exactly their unmasked value — dry, source-free, walled).
    /// The CVODE system stays full size. Exactly OFF-able; default OFF until
    /// field-validated. Parsed from [2D_OPTIONS] ACTIVE_SET (YES/NO); env
    /// OPENSWMM_2D_ACTIVE_SET (0/1) overrides. CVODE+DW only.
    bool   active_set        = false;
    /// BFS halo ring count around the wet/sourced seed set (≥1); auto-doubled
    /// (capped) when a front crosses the whole halo within one window. Parsed
    /// from [2D_OPTIONS] ACTIVE_SET_HALO; env OPENSWMM_2D_ACTIVE_SET_HALO.
    int    active_set_halo   = 2;
    bool   report_2d         = true;    ///< Write 2D results to output

    // Time integrator. Default CVODE (validated path); ARKODE selects the
    // ARKStep IMEX solver. Parsed from [2D_OPTIONS] INTEGRATOR; env
    // OPENSWMM_2D_INTEGRATOR overrides at solver-construction time.
    IntegratorType     integrator      = IntegratorType::CVODE;

    // Surface-momentum closure. Default DW (diffusive wave). INERTIAL selects the
    // local-inertial scheme (per-edge q), honored only by ArkodeSurfaceSolver.
    // Parsed from [2D_OPTIONS] MOMENTUM; env OPENSWMM_2D_MOMENTUM overrides.
    MomentumType       momentum        = MomentumType::DW;

    // Rainfall→mesh mapping. Default NATURAL_NEIGHBOUR (spatial interpolation
    // across all located gages); SYSTEM applies the uniform all-gage mean.
    // Parsed from [2D_OPTIONS] RAINFALL_MODE; env OPENSWMM_2D_RAINFALL_MODE
    // (natural|system) overrides.
    RainfallMode       rainfall_mode   = RainfallMode::NATURAL_NEIGHBOUR;

    // Volume → free-surface cell closure. Default VFR (Phase-5 rollout of
    // plans/2d/2D_VFR_SOLVER_CLOSURE_PLAN.md, 2026-07-19): the planar-bed
    // volume/free-surface relation restores the C-property at shorelines and
    // removes the flat-closure water-climbs-uphill artifact. FLAT (legacy
    // η = tri_cz + V/A) remains selectable for A/B. Parsed from
    // [2D_OPTIONS] CELL_CLOSURE (FLAT|VFR).
    CellClosure2D      cell_closure    = CellClosure2D::VFR;

    // Effective conveyance depth at shared edges. Default VFR_FACE (Phase-5):
    // the B&S Eq. 14 face depth + wetting gate stops flow across an edge whose
    // bed sits above the upwind surface (the second half of the artifact fix).
    // MEAN (legacy upwind cell-mean depth) remains selectable. Parsed from
    // [2D_OPTIONS] FACE_RECONSTRUCTION (MEAN|VFR_FACE). Independent of
    // CELL_CLOSURE so the two pieces can be A/B'd separately.
    FaceDepth2D        face_reconstruction = FaceDepth2D::VFR_FACE;

    /// Wetted-area-fraction floor ε of the regularized VFR closure: below wet
    /// fraction ε the η(V) relation continues linearly (slope 1/(εA)), bounding
    /// dη/dV for the implicit solvers' Newton/Jacobian path. Exact elsewhere.
    /// Only used when CELL_CLOSURE = VFR. Parsed from [2D_OPTIONS]
    /// VFR_MIN_WET_FRAC; valid range (0, 0.5].
    double             vfr_min_wet_frac = 0.01;

    LinearSolverType   linear_solver   = LinearSolverType::GMRES;
    // Default to AMG (hypre BoomerAMG): the only preconditioner with
    // near-mesh-independent Krylov counts on the elliptic diffusive-wave
    // operator, so it is the right default at every scale (and essential past
    // ~100k cells). The value is unconditional here (keeping one struct layout
    // across all TUs); a build WITHOUT hypre resolves AMG → JACOBI at solver
    // initialize with a one-line notice, so the portable base build still runs.
    // See docs/2D_KNOWN_STIFFNESS_ISSUE.md.
    PreconditionerType preconditioner  = PreconditionerType::AMG;

    /// Path from [2D_MESH_FILE] FILE token. Empty = mesh is inline in main .inp.
    std::string mesh_file;

    /// HDF5 output file path from [2D_OPTIONS] OUTPUT_FILE token. Empty =
    /// no 2D output is written. Resolved relative to the parent .inp directory
    /// by the section handler.
    std::string output_file;

    // -----------------------------------------------------------------------
    // Unit-system coupling factors — NOT parsed from input. Computed once in
    // SurfaceRouter2D::initialize() from the project FLOW_UNITS.
    //
    // The 2D solver runs internally in SI (metres, m², m³, m³/s, g=9.80665).
    // The 1D SWMM engine ALWAYS computes internally in FEET (g=32.2, PHI=1.486)
    // — for EVERY project, US or SI: its reader converts metric inputs to feet
    // on load and only converts back at the display/output boundary. So these
    // coupling factors are ALWAYS the feet⇄metres conversion, independent of
    // FLOW_UNITS. SurfaceRouter2D::initialize() overwrites the 1.0 defaults
    // with the real ft⇄m factors; the defaults only stand when 2D is inactive
    // (no coupling occurs). The MESH scaling factor is separate and IS driven
    // by FLOW_UNITS (the mesh is authored in project units) — see initialize().
    // -----------------------------------------------------------------------
    double len_1d_to_2d  = 1.0;  ///< 1D length → 2D length (ft→m, 0.3048)
    double len_2d_to_1d  = 1.0;  ///< 2D length → 1D length (m→ft, 3.2808)
    double vol_1d_to_2d  = 1.0;  ///< 1D volume → 2D volume (ft³→m³, 0.02832)
    double flow_1d_to_2d = 1.0;  ///< 1D flow → 2D flow (ft³/s→m³/s, 0.02832)
    double flow_2d_to_1d = 1.0;  ///< 2D flow → 1D flow (m³/s→ft³/s, 35.315)

    /*! Runtime-only: resolved OpenMP thread count for the embarrassingly-
     *  parallel 2D per-cell / per-vertex loops (RHS pipeline, Jacobi
     *  preconditioner, post-step diagnostics). Set in
     *  SurfaceRouter2D::initialize() from SimulationOptions::num_threads (the
     *  global THREADS option) using the same min(N,max) + size-gate
     *  DWSolver::setNumThreads applies. 1 = serial. The parallelised loops use
     *  schedule(static) and write only their own cell/vertex slot, so any
     *  thread count is bit-identical to serial. Never parsed/persisted. */
    int num_threads = 1;

    /*! When true, the inline `.inp` or referenced `.2dm` declared
     *  `;; UNITS: SI (m)` (or an equivalent metric keyword). The mesh on
     *  disk is already in SI metres, so SurfaceRouter2D::initialize
     *  SKIPS the FLOW_UNITS-based mesh scaling (vx/vy/vz and the
     *  coupling areas).  The 1D⇄2D coupling factors (len_1d_to_2d,
     *  vol_1d_to_2d, flow_*) are unaffected — they are always the
     *  feet⇄metres conversion (the 1D side is always feet), not the mesh
     *  scaling. */
    bool mesh_units_si = false;

    /*! Runtime-only: true after SurfaceRouter2D::initialize() applied the
     *  FLOW_UNITS ft→m in-place mesh scaling (vx/vy/vz, coupling areas).
     *  Lets serialization (InpWriter, GeoPackage) un-scale back to the
     *  authored units, and makes a repeated initialize() idempotent
     *  against double-scaling. Never parsed from input, never persisted. */
    bool mesh_scaled_to_si = false;

    /*! Runtime-only: true after SurfaceRouter2D::initialize() drained the
     *  pending [2D_BOUNDARY_CONDITIONS] / [2D_EDGE_CONVEYANCE] rows into
     *  BoundaryData / MeshData::edge_conveyance. Serialization collectors
     *  (Serialize2D.hpp) switch to the drained arrays once this is set —
     *  they are the live state that post-initialize API mutations edit;
     *  the retained pending rows would be stale. Never parsed/persisted. */
    bool pending_rows_drained = false;
};

} // namespace openswmm::twoD

#endif // OPENSWMM_ENGINE_2D_SOLVER_OPTIONS_HPP
