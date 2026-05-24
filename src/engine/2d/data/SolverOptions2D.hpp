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
    NONE     = 0,
    JACOBI   = 1,
    ILU      = 2,   // future
    SPECTRAL = 3    // spectral Laplacian (SpectralPrecond2D)
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

    int spectral_num_modes = 10;   ///< modes for PreconditionerType::SPECTRAL

    LinearSolverType   linear_solver   = LinearSolverType::GMRES;
    PreconditionerType preconditioner  = PreconditionerType::NONE;

    // ---- Spectral ROM uncertainty sidecar ----
    bool   enable_rom        = false; ///< Run SpectralROM ensemble alongside CVODE.
    int    rom_members       = 50;    ///< Ensemble size M.
    int    rom_modes         = 10;    ///< Eigenmodes retained for ROM basis.
    double rom_mannings_pert = 0.20;  ///< Manning's n half-range: n ∈ [1±p].
    double rom_rainfall_pert = 0.20;  ///< Rainfall half-range: r ∈ [1±p].
    double rom_k_eff         = 10.0;  ///< Effective diffusivity estimate (m^{4/3}/s).

    double rom_wet_reseed_fraction     = 0.05;   ///< Reseed ROM basis when wet-cell count changes by > this fraction of n_tri.
    double rom_wet_reseed_min_interval = 60.0;   ///< Min simulation time (s) between ROM basis reseeds.
    bool   rom_parametric_tails        = false;  ///< Use log-normal parametric upper tail in computeQuantiles.
    double rom_mode_drop_threshold     = 1.0e-10; ///< Drop mode j if E_j < threshold AND rainfall forcing < threshold (transient; reactivated by rain).

    // ---- Spatial uncertainty (Phase 2) ----
    /// Exponential correlation length for spatially-varying Manning's n (m).
    /// 0 = scalar mode (mannings_mult uniform over all cells, fast path).
    double rom_mannings_corr_len = 0.0;
    /// Exponential correlation length for spatially-varying rainfall (m).
    /// 0 = scalar mode.
    double rom_rainfall_corr_len = 0.0;

    /// Path from [2D_MESH_FILE] FILE token. Empty = mesh is inline in main .inp.
    std::string mesh_file;
};

} // namespace openswmm::twoD

#endif // OPENSWMM_ENGINE_2D_SOLVER_OPTIONS_HPP
