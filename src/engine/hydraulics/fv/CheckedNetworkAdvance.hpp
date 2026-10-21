// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
#pragma once

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

#include "INetworkSolver.hpp"

namespace openswmm::fv {

/// Check the backend's reached-time contract BEFORE the Router publishes
/// full-window means/receipts. Does not change the backend ABI or integrate
/// a partial result as if it had covered the requested routing duration.
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
