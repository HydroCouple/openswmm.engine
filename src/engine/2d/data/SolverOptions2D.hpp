/**
 * @file SolverOptions2D.hpp
 * @brief Configuration options for the 2D surface routing solver.
 *
 * @see TWO_DIMENSIONAL_SURFACE_ROUTING_IMPLEMENTATION_STRATEGY.md §2.3
 * @ingroup engine_2d
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_ENGINE_2D_SOLVER_OPTIONS_HPP
#define OPENSWMM_ENGINE_2D_SOLVER_OPTIONS_HPP

#include <cstdint>
#include <string>

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
};

} // namespace openswmm::twoD

#endif // OPENSWMM_ENGINE_2D_SOLVER_OPTIONS_HPP
