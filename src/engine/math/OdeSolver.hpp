// SPDX-License-Identifier: Apache-2.0
//
// Copyright 2026 Caleb Buahin
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/**
 * @file      OdeSolver.hpp
 * @brief     Runge-Kutta-Cash-Karp (RK45) ODE integrator with adaptive step control.
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
 *          or use integrate_batch_2eq() for a single explicit Euler step over
 *          SoA state arrays; the batch helper does not use adaptive RK45.
 *
 * @note Legacy reference: src/legacy/engine/odesolve.c
 * @ingroup engine_math
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
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

constexpr int MAXSTP = 10000;      ///< Maximum integration steps
constexpr double TINY = 1.0e-30;   ///< Underflow protection
constexpr double SAFETY = 0.9;     ///< Step adjustment safety factor
constexpr double PGROW = -0.2;     ///< Exponent for step increase
constexpr double PSHRNK = -0.25;   ///< Exponent for step decrease
constexpr double ERRCON = 1.89e-4; ///< = (5/SAFETY)^(1/PGROW)

// ============================================================================
// Return codes
// ============================================================================

constexpr int ODE_OK = 0;        ///< Success
constexpr int ODE_TOO_MANY = 1;  ///< n > allocated max
constexpr int ODE_UNDERFLOW = 2; ///< Step size underflowed
constexpr int ODE_MAX_STEPS = 3; ///< Exceeded MAXSTP

// ============================================================================
// Derivative callback
// ============================================================================

/**
 * @brief Synchronous derivative evaluator for an n-equation ODE system.
 * @param[in] x Independent variable, commonly elapsed time in seconds.
 * @param[in] y Borrowed, non-null array of n states at x; do not modify or retain.
 * @param[out] dydx Borrowed, non-null array of n derivatives dy_i/dx;
 * write every component before returning and do not retain the pointer.
 * @note Units of each derivative are the state units divided by x units.
 */
using DerivFunc = std::function<void(double x, const double* y, double* dydx)>;

// ============================================================================
// Per-element integrator
// ============================================================================

/**
 * @brief Advance an ODE system with adaptive Cash-Karp RK45 steps.
 * @param[in,out] y Non-null array of n initial states at x1. Replaced with
 * states at x2 only on ODE_OK; error returns leave the caller's array unchanged.
 * @param[in] n Positive number of equations; controls all callback array lengths.
 * @param[in] x1 Finite start of the integration interval, in caller-selected units.
 * @param[in] x2 Finite end in the same units; forward and reverse intervals
 * are supported when h1 has the corresponding sign.
 * @param[in] eps Positive dimensionless local error tolerance, relative to
 * abs(y_i) + abs(h * dy_i/dx) + TINY at each attempted step.
 * @param[in] h1 Nonzero initial step guess, in x units, directed from x1 to x2.
 * @param[in] derivs Nonempty derivative callback. Called synchronously with
 * borrowed workspace arrays; not retained, and exceptions propagate.
 * @return ODE_OK on success, ODE_UNDERFLOW if a proposed next step is zero,
 * or ODE_MAX_STEPS after exhaustion or an underflow inside step rejection.
 * ODE_TOO_MANY is returned if workspace capacity remains below n.
 * @details Workspace is reused per thread and released at thread exit.
 * The solver is not reentrant on the same thread: derivs must not recursively
 * call integrate(). Allocation failures are not translated into an ODE status.
 */
int integrate(double* y, int n, double x1, double x2, double eps, double h1,
              const DerivFunc& derivs);

// ============================================================================
// Batch integrator (SoA, for vectorized subcatchment processing)
// ============================================================================

/**
 * @brief Synchronous derivative evaluator for a batch of two-state systems.
 * @param[in] x Independent variable, in caller-selected units.
 * @param[in] y0 Borrowed array of n_sys first-state values.
 * @param[in] y1 Borrowed array of n_sys second-state values.
 * @param[out] dy0 Borrowed array to fill with n_sys first-state derivatives.
 * @param[out] dy1 Borrowed array to fill with n_sys second-state derivatives.
 * @param[in] n_sys Number of independent systems; nonnegative.
 * @note Derivative units are state units divided by x units. No array may
 * be retained by the callback.
 */
using BatchDerivFunc = std::function<void(double x, const double* y0, const double* y1, double* dy0,
                                          double* dy1, int n_sys)>;

/**
 * @brief Advance independent two-state systems with one explicit Euler step.
 * @param[in,out] y0 Array of n_sys first-state values; updated by dy0 * (x2 - x1).
 * @param[in,out] y1 Distinct array of n_sys second-state values; updated by
 * dy1 * (x2 - x1). Arrays must be writable when n_sys > 0.
 * @param[in] n_sys Nonnegative number of systems and length of each array.
 * @param[in] x1 Finite start coordinate, also the sole derivative evaluation time.
 * @param[in] x2 Finite end coordinate in the same units as x1.
 * @param[in] eps Unused; this helper performs no tolerance or error control.
 * @param[in] h1 Unused; the only step has length x2 - x1.
 * @param[in] derivs Nonempty callback invoked once for the entire batch.
 * Pointers are borrowed for that call; callback exceptions propagate.
 * @return ODE_OK after all states are updated. Workspace allocation may throw.
 * @note This is not the adaptive RK45 algorithm used by integrate().
 * Thread-local derivative buffers make recursive calls on one thread unsafe.
 */
int integrate_batch_2eq(double* y0, double* y1, int n_sys, double x1, double x2, double eps,
                        double h1, const BatchDerivFunc& derivs);

} // namespace ode
} // namespace openswmm

#endif // OPENSWMM_ODE_SOLVER_HPP
