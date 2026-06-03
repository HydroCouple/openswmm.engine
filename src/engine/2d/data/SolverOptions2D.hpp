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
 * Phase 1 wires NONE (no preconditioning) and JACOBI (per-cell diagonal
 * approximation rebuilt each Jacobian refresh). ILU and AMG are reserved
 * for the Phase 2 hypre/BoomerAMG integration; selecting them today
 * triggers a clear runtime error in CvodeSurfaceSolver::initialize().
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
    NONE   = 0,     ///< Phase 1: WIRED (no preconditioning).
    JACOBI = 1,     ///< Phase 1: WIRED (diagonal heuristic).
    ILU    = 2      ///< Reserved; rejected at initialize() in Phase 1.
    // AMG  = 3     ///< Reserved for Phase 2 (hypre BoomerAMG).
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
    double coupling_cd       = 0.65;    ///< Default discharge coefficient
    int    max_krylov_dim    = 30;      ///< Max Krylov subspace dimension
    int    coupling_interval = 0;       ///< 0 = every SWMM step
    int    max_cvode_steps   = 500;     ///< Max CVODE steps per advance
    bool   report_2d         = true;    ///< Write 2D results to output

    LinearSolverType   linear_solver   = LinearSolverType::GMRES;
    PreconditionerType preconditioner  = PreconditionerType::NONE;

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
    // The 1D SWMM engine computes internally in FEET for US flow units
    // (CFS/GPM/MGD; g=32.2, PHI=1.486) and only converts to display units at
    // output. These factors convert 1D quantities to/from the 2D SI internal
    // units at the coupling boundary (NodeCoupling) and the mesh load. All
    // default to 1.0, so an SI project (CMS/LPS/MLD) is a no-op.
    // -----------------------------------------------------------------------
    double len_1d_to_2d  = 1.0;  ///< 1D length → 2D length (ft→m = 0.3048 for US)
    double len_2d_to_1d  = 1.0;  ///< 2D length → 1D length (m→ft)
    double vol_1d_to_2d  = 1.0;  ///< 1D volume → 2D volume (ft³→m³ for US)
    double flow_1d_to_2d = 1.0;  ///< 1D flow → 2D flow (ft³/s→m³/s for US)
    double flow_2d_to_1d = 1.0;  ///< 2D flow → 1D flow (m³/s→ft³/s)

    /*! When true, the inline `.inp` or referenced `.2dm` declared
     *  `;; UNITS: SI (m)` (or an equivalent metric keyword). The mesh on
     *  disk is already in SI metres, so SurfaceRouter2D::initialize
     *  SKIPS the FLOW_UNITS-based mesh scaling (vx/vy/vz and the
     *  coupling areas).  The 1D⇄2D coupling factors (len_1d_to_2d,
     *  vol_1d_to_2d, flow_*) are still set from FLOW_UNITS because they
     *  describe the 1D side of the boundary, not the mesh. */
    bool mesh_units_si = false;
};

} // namespace openswmm::twoD

#endif // OPENSWMM_ENGINE_2D_SOLVER_OPTIONS_HPP
