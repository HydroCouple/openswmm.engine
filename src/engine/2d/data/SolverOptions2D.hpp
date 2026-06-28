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
    /// up and the implicit step collapse. Only affects sub-mm gradients, so real
    /// flow is untouched (mass balance and peak flows unchanged); raise it for
    /// extra robustness on very deep problems. 0 = bare √. Parsed from
    /// [2D_OPTIONS] FLUX_DH_EPS; env OPENSWMM_2D_FLUX_DH_EPS overrides.
    double flux_dh_eps       = 0.001;   ///< Diffusive-flux gradient floor (m)
    double coupling_cd       = 0.65;    ///< Default discharge coefficient
    int    max_krylov_dim    = 30;      ///< Max Krylov subspace dimension
    int    coupling_interval = 0;       ///< 0 = every SWMM step
    int    max_cvode_steps   = 500;     ///< Max CVODE steps per advance
    bool   report_2d         = true;    ///< Write 2D results to output

    // Time integrator. Default CVODE (validated path); ARKODE selects the
    // ARKStep IMEX solver. Parsed from [2D_OPTIONS] INTEGRATOR; env
    // OPENSWMM_2D_INTEGRATOR overrides at solver-construction time.
    IntegratorType     integrator      = IntegratorType::CVODE;

    // Surface-momentum closure. Default DW (diffusive wave). INERTIAL selects the
    // local-inertial scheme (per-edge q), honored only by ArkodeSurfaceSolver.
    // Parsed from [2D_OPTIONS] MOMENTUM; env OPENSWMM_2D_MOMENTUM overrides.
    MomentumType       momentum        = MomentumType::DW;

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
