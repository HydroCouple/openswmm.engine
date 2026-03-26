/**
 * @file OdeSolver.hpp
 * @brief Runge-Kutta-Cash-Karp (RK45) ODE integrator with adaptive step control.
 *
 * @details Numerically identical to legacy odesolve.c. Uses 5th-order
 *          Runge-Kutta with embedded 4th-order error estimate for automatic
 *          step size control.
 *
 *          Used by:
 *          - Groundwater two-zone model (2 ODEs per subcatchment)
 *          - Subcatchment runoff ponded-depth integration
 *
 *          **Vectorization note:** Each subcatchment's ODE system is independent.
 *          For batch integration over N subcatchments, call integrate() N times
 *          or use integrate_batch() which processes all subcatchments in a
 *          single pass with SoA state arrays.
 *
 * @note Legacy reference: src/legacy/engine/odesolve.c
 * @ingroup engine_math
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_ODE_SOLVER_HPP
#define OPENSWMM_ODE_SOLVER_HPP

#include <functional>
#include <vector>

namespace openswmm {
namespace ode {

// ============================================================================
// Constants (matching legacy)
// ============================================================================

constexpr int    MAXSTP  = 10000;    ///< Maximum integration steps
constexpr double TINY    = 1.0e-30;  ///< Underflow protection
constexpr double SAFETY  = 0.9;      ///< Step adjustment safety factor
constexpr double PGROW   = -0.2;     ///< Exponent for step increase
constexpr double PSHRNK  = -0.25;    ///< Exponent for step decrease
constexpr double ERRCON  = 1.89e-4;  ///< = (5/SAFETY)^(1/PGROW)

// ============================================================================
// Return codes
// ============================================================================

constexpr int ODE_OK         = 0;  ///< Success
constexpr int ODE_TOO_MANY   = 1;  ///< n > allocated max
constexpr int ODE_UNDERFLOW  = 2;  ///< Step size underflowed
constexpr int ODE_MAX_STEPS  = 3;  ///< Exceeded MAXSTP

// ============================================================================
// Derivative callback
// ============================================================================

/**
 * @brief Derivative function signature.
 *
 * @param x     Independent variable (e.g., time).
 * @param y     Array of n dependent variables at x.
 * @param dydx  [out] Array where callback writes dy_i/dx.
 */
using DerivFunc = std::function<void(double x, const double* y, double* dydx)>;

// ============================================================================
// Per-element integrator
// ============================================================================

/**
 * @brief Integrate an ODE system from x1 to x2 using RK45 Cash-Karp.
 *
 * @param y       [in/out] Array of n dependent variables (initial → final).
 * @param n       Number of equations.
 * @param x1      Start of integration interval.
 * @param x2      End of integration interval.
 * @param eps     Desired accuracy (tolerance).
 * @param h1      Initial step size guess.
 * @param derivs  Derivative callback function.
 * @returns ODE_OK on success, or error code.
 */
int integrate(double* y, int n, double x1, double x2,
              double eps, double h1, const DerivFunc& derivs);

// ============================================================================
// Batch integrator (SoA, for vectorized subcatchment processing)
// ============================================================================

/**
 * @brief Integrate N independent 2-equation ODE systems in batch.
 *
 * @details Used by groundwater: each subcatchment has 2 state variables
 *          (theta, lower_depth). All are integrated over the same interval
 *          [x1, x2] but with independent derivative functions.
 *
 *          The batch version avoids per-subcatchment function call overhead
 *          and enables future SIMD parallelism of the RK stages.
 *
 * @param y0      [in/out] SoA: first state variable for all N systems.
 * @param y1      [in/out] SoA: second state variable for all N systems.
 * @param n_sys   Number of independent ODE systems.
 * @param x1      Start of interval.
 * @param x2      End of interval.
 * @param eps     Tolerance.
 * @param h1      Initial step size.
 * @param derivs  Batch derivative: derivs(x, y0[], y1[], dy0[], dy1[], n_sys).
 */
using BatchDerivFunc = std::function<void(double x,
    const double* y0, const double* y1,
    double* dy0, double* dy1, int n_sys)>;

int integrate_batch_2eq(double* y0, double* y1, int n_sys,
                        double x1, double x2, double eps, double h1,
                        const BatchDerivFunc& derivs);

} // namespace ode
} // namespace openswmm

#endif // OPENSWMM_ODE_SOLVER_HPP
