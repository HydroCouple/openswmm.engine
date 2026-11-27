/**
 * @file      PondedDepthExact.hpp
 * @brief     Exact solution of the subarea ponded-depth equation (RUNOFF_ODE_SOLVER EXACT).
 *
 * @details The nonlinear reservoir dd/dt = i - alpha (d - Ds)^m, with the net inflow i, the
 *          overland coefficient alpha and the depression storage Ds constant over a runoff
 *          step, scales to du/dtau = sign(i) - u^m with u = (d - Ds)/xe and tau = t |i| / xe,
 *          where xe = (|i|/alpha)^(1/m) is the equilibrium excess depth. The scaled equation has
 *          one universal solution per sign, tabulated once per process (quintic Hermite on
 *          geometric grids; see PondedDepthExact.cpp), so a step is two table lookups, no step
 *          control and no rejected trials. For i = 0 the solution is closed. Error is near
 *          1e-10 relative against a tight reference; the legacy RK45 at tolerance 1e-4 differs
 *          from both by about 1e-4 relative. Track 1C, 2026-11-26.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_PONDED_DEPTH_EXACT_HPP
#define OPENSWMM_PONDED_DEPTH_EXACT_HPP

namespace openswmm {
namespace runoff {

/**
 * @brief The exact ponded-depth step and its per-process tables.
 */
class ExactPondedDepth {
public:
    /// Manning exponent of the overland outflow, exactly 5/3 (legacy's literal 1.6666667 is
    /// 2e-8 away; the difference is invisible next to the solver error LEGACY carries).
    static constexpr double kExponent = 5.0 / 3.0;
    /// Geometric-grid resolution of the tables (nodes per octave).
    static constexpr int kNodesPerOctave = 16;

    /// One-entry cache of |i|^(1/m), keyed on the bits of |i|: subareas sharing a gage and no
    /// infiltration see the same net inflow, so the one pow of a step is mostly skipped. One
    /// per thread; a hit returns the same bits a miss would, so results do not depend on it.
    struct ScaleCache {
        double key = -1.0;
        double value = 0.0;
    };

    /// @return alpha^(-1/m), the per-subarea factor of the equilibrium excess depth; 0 for a
    ///     nonpositive alpha.
    static double alphaScale(double alpha) noexcept;

    /// Builds the tables now (they are otherwise built on the first step). Call once from the
    /// runoff initialization so the first parallel step does not pay for it.
    static void prepare();

    /**
     * @brief Advance one subarea's ponded depth over a runoff step.
     *
     * Same contract as RunoffSolver::updatePondedDepth: fills depression storage first, then
     * integrates the excess depth over the remaining time; a nonpositive alpha or no remaining
     * time accumulates the inflow linearly; the result is floored at zero.
     *
     * @param[in] depth Ponded depth at the start of the step (ft).
     * @param[in] inflow Net inflow i after evaporation and infiltration (ft/s); may be negative.
     * @param[in] alpha Overland coefficient (ft^(1-m)/s), nonnegative.
     * @param[in] alpha_scale alphaScale(alpha) for this subarea.
     * @param[in] dStore Depression storage (ft).
     * @param[in] dt Step (s), positive.
     * @param[out] t_runoff Time the depth spent above storage (legacy tx).
     * @param[out] outflow alpha (depth_new - Ds)^m at the end of the step, 0 at or below
     *     storage: the caller's runoff rate without its pow.
     * @param[in,out] cache This thread's ScaleCache.
     * @return The ponded depth at the end of the step (ft).
     */
    static double step(double depth, double inflow, double alpha, double alpha_scale, double dStore,
                       double dt, double& t_runoff, double& outflow, ScaleCache& cache) noexcept;
};

} // namespace runoff
} // namespace openswmm

#endif // OPENSWMM_PONDED_DEPTH_EXACT_HPP
