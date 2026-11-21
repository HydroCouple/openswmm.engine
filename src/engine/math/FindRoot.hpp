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
 * @file      FindRoot.hpp
 * @brief     Newton-Raphson and Ridder root finders.
 *
 * @details Numerically identical to legacy findroot.c. Used by:
 *   - Kinematic wave continuity equation solver
 *   - Cross-section depth-from-area inversion
 *   - Green-Ampt infiltration F2 solve
 *
 * @note Legacy reference: src/legacy/engine/findroot.c
 * @ingroup engine_math
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_FIND_ROOT_HPP
#define OPENSWMM_FIND_ROOT_HPP

#include <functional>

namespace openswmm {
namespace findroot {

constexpr int MAXIT = 60; ///< Maximum iterations

/**
 * @brief Synchronous function and derivative evaluator for newton().
 * @param[in] x Candidate root, in the caller's independent-variable units.
 * @param[out] f Non-null solver-owned slot for f(x).
 * @param[out] df Non-null solver-owned slot for df/dx.
 * @note The callback must fill both slots and must not retain their addresses.
 */
using NewtonFunc = std::function<void(double x, double* f, double* df)>;

/**
 * @brief Find a root with Newton steps and a bisection fallback.
 * @param[in] x1 Bracket endpoint where f(x1) < 0, regardless of coordinate order.
 * @param[in] x2 Bracket endpoint where f(x2) > 0.
 * @param[in,out] rts Non-null caller-owned initial guess inside the bracket;
 * overwritten with the final estimate on both convergence and iteration exhaustion.
 * @param[in] xacc Positive absolute step tolerance in the units of x1 and x2.
 * @param[in] func Nonempty synchronous callback producing f and df/dx for
 * each candidate. It is not retained; callback exceptions propagate.
 * @return Number of function evaluations on convergence or a stationary
 * floating-point estimate; zero when more than MAXIT evaluations are needed.
 * @details The caller must orient and validate the bracket. This function
 * does not pre-evaluate endpoints; preserving callback sequence matches
 * legacy findroot_Newton(). Newton steps outside the bracket, or steps
 * that converge too slowly, are replaced by bisection.
 */
int newton(double x1, double x2, double* rts, double xacc, const NewtonFunc& func);

/**
 * @brief Synchronous scalar function evaluator for ridder().
 * @param[in] x Candidate root in caller-selected units.
 * @return f(x); no derivative is required.
 */
using RidderFunc = std::function<double(double x)>;

/**
 * @brief Estimate a bracketed root with derivative-free Ridder iterations.
 * @param[in] x1 First finite bracket coordinate.
 * @param[in] x2 Second finite bracket coordinate; endpoint function values
 * must have opposite signs unless an endpoint is already a root.
 * @param[in] xacc Positive absolute tolerance in the units of x1 and x2,
 * applied to successive estimates and bracket width.
 * @param[in] func Nonempty synchronous evaluator, not retained. Exceptions
 * propagate; callback invocation order follows legacy findroot_Ridder().
 * @return An exact endpoint root, an approximate root, or -1.0e20 if the
 * endpoints do not straddle a root. On iteration exhaustion the last estimate
 * is returned without a distinct failure code.
 * @note When a new estimate differs by at most xacc, the previous estimate
 * is returned. The callback's most recent input need not equal the result.
 */
double ridder(double x1, double x2, double xacc, const RidderFunc& func);

} // namespace findroot
} // namespace openswmm

#endif // OPENSWMM_FIND_ROOT_HPP
