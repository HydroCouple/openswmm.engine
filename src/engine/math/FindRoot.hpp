/**
 * @file FindRoot.hpp
 * @brief Newton-Raphson and Ridder root finders.
 *
 * @details Numerically identical to legacy findroot.c. Used by:
 *   - Kinematic wave continuity equation solver
 *   - Cross-section depth-from-area inversion
 *   - Green-Ampt infiltration F2 solve
 *
 * @note Legacy reference: src/legacy/engine/findroot.c
 * @ingroup engine_math
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_FIND_ROOT_HPP
#define OPENSWMM_FIND_ROOT_HPP

#include <functional>

namespace openswmm {
namespace findroot {

constexpr int MAXIT = 60;  ///< Maximum iterations

/**
 * @brief Newton-Raphson with bisection fallback.
 *
 * @details Finds x in [x1, x2] such that f(x) = 0. Requires f(x1) and f(x2)
 *          to have opposite signs. Uses Newton steps when reliable, falls back
 *          to bisection when Newton would exit the bracket or converge slowly.
 *
 * @param x1    Left bracket.
 * @param x2    Right bracket.
 * @param rts   [in/out] Initial guess → root.
 * @param xacc  Convergence tolerance (|dx| < xacc).
 * @param func  Callback: func(x, &f, &df) sets f=f(x), df=f'(x).
 * @returns Number of function evaluations, or 0 if exceeded MAXIT.
 */
using NewtonFunc = std::function<void(double x, double* f, double* df)>;

int newton(double x1, double x2, double* rts, double xacc, const NewtonFunc& func);

/**
 * @brief Ridder's method (derivative-free).
 *
 * @param x1    Left bracket.
 * @param x2    Right bracket.
 * @param xacc  Convergence tolerance.
 * @param func  Callback: func(x) returns f(x).
 * @returns Root value, or -1.0e10 if failed.
 */
using RidderFunc = std::function<double(double x)>;

double ridder(double x1, double x2, double xacc, const RidderFunc& func);

} // namespace findroot
} // namespace openswmm

#endif // OPENSWMM_FIND_ROOT_HPP
