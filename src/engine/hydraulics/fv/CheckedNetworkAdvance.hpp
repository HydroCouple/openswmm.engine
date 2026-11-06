// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin

/**
 * @file      CheckedNetworkAdvance.hpp
 * @brief     Validated advancement of finite-volume network trials.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#pragma once

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

#include "INetworkSolver.hpp"

namespace openswmm::fv {

/**
 * @brief Advance an FV backend and verify it reached the full requested time window.
 *
 * Throws invalid_argument for unordered/nonfinite times, runtime_error for
 * nonfinite/backward/partial reached time; backend exceptions propagate. Does not roll back backend
 * state or publish Router means/receipts.
 *
 * @param[in,out] solver Borrowed initialized backend; advance may mutate it even if reached-time
 *     validation later fails.
 * @param[in] start Finite accepted elapsed start seconds.
 * @param[in] target Finite target elapsed seconds>=start.
 * @param[in] forcing Borrowed boundary/source forcing valid for the requested window; lifetime
 *     covers advance.
 * @return Backend reached seconds after checking target agreement
 *     within16*epsilon*max(1,abs(start),abs(target)).
 */
inline double advanceNetworkChecked(INetworkSolver& solver, double start, double target,
                                    const FvStepForcing& forcing) {
    if (!std::isfinite(start) || !std::isfinite(target) || target < start)
        throw std::invalid_argument("FV routing times must be finite and ordered.");
    const double reached = solver.advance(start, target, forcing);
    const double tolerance = 16.0 * std::numeric_limits<double>::epsilon() *
                             std::max({1.0, std::fabs(start), std::fabs(target)});
    if (!std::isfinite(reached) || reached < start || std::fabs(reached - target) > tolerance)
        throw std::runtime_error("FV backend did not reach the requested routing time; "
                                 "partial results were not published.");
    return reached;
}

} // namespace openswmm::fv
