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
 * @file      OdeSolver.cpp
 * @brief     RK45 Cash-Karp ODE integrator — numerically identical to legacy.
 * @ingroup engine_math
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include "OdeSolver.hpp"
#include <cmath>
#include <cstdlib>
#include <algorithm>
#include <type_traits>
#include <vector>

namespace openswmm {
namespace ode {

namespace detail {
// ============================================================================
// Pre-allocated workspace matching legacy odesolve.c global arrays.
//
// Legacy uses file-scope globals: y[], yscal[], dydx[], yerr[], ytemp[], ak[].
// We use a thread-local struct to remain reentrant while avoiding per-call
// heap allocation. ensureWorkspace() plays the role of legacy odesolve_open();
// the destructor plays the role of odesolve_close() (legacy runoff.c:135).
//
// The destructor must stay declared even though the members are raw pointers:
// it is what makes the type non-trivially destructible, and only that gets a
// thread-exit handler registered. Without it, thread exit reclaims the TLS
// block holding these pointers and strands the buffers they point at, leaking
// one workspace per thread that ever integrated.
// ============================================================================

OdeWorkspace::~OdeWorkspace() {
        std::free(y);
        std::free(yscal);
        std::free(dydx);
        std::free(yerr);
        std::free(ytemp);
        std::free(ak);
        // Reset to the constructed state, as legacy odesolve_close() does.
        // nmax = 0 matters: ensureWorkspace() early-returns while nmax >= n, so
        // leaving it set would hand freed pointers to any integrate() that ran
        // after teardown (thread_local destruction order is not guaranteed).
        y = yscal = dydx = yerr = ytemp = ak = nullptr;
        nmax = 0;
    }

// Pins the property the destructor exists for. If it is ever removed or
// =default'd, the leak returns silently on every platform whose leak checker
// is unavailable (Darwin arm64 among them) -- fail the build instead.
static_assert(!std::is_trivially_destructible<OdeWorkspace>::value,
              "OdeWorkspace needs a non-trivial destructor so thread exit frees "
              "its buffers; see tests/manual/ode_workspace_leak");

static thread_local OdeWorkspace ws_;

/**
 * @brief Grow this thread's reusable solver arrays for n equations.
 * @param[in] n Positive requested capacity; n * 5 must fit in int.
 * @details A request within current capacity does nothing. Growing releases
 * all old arrays and allocates zero-initialized replacements, including five
 * stage arrays. Allocation failures are not checked by this legacy helper.
 */
OdeWorkspace& workspace(int n) {
    if (ws_.nmax >= n)
        return ws_;
    // Free old
    std::free(ws_.y);
    std::free(ws_.yscal);
    std::free(ws_.dydx);
    std::free(ws_.yerr);
    std::free(ws_.ytemp);
    std::free(ws_.ak);
    // Allocate fresh
    int n5 = n * 5;
    ws_.y = static_cast<double*>(std::calloc(n, sizeof(double)));
    ws_.yscal = static_cast<double*>(std::calloc(n, sizeof(double)));
    ws_.dydx = static_cast<double*>(std::calloc(n, sizeof(double)));
    ws_.yerr = static_cast<double*>(std::calloc(n, sizeof(double)));
    ws_.ytemp = static_cast<double*>(std::calloc(n, sizeof(double)));
    ws_.ak = static_cast<double*>(std::calloc(n5, sizeof(double)));
    ws_.nmax = n;
    return ws_;
}
} // namespace detail

// ============================================================================
// integrate — matching legacy odesolve_integrate exactly
// ============================================================================

// Contract: openswmm::ode::integrate.
int integrate(double* ystart, int n, double x1, double x2, double eps, double h1,
              const DerivFunc& derivs) {
    return detail::integrate(ystart, n, x1, x2, eps, h1, derivs);
}

// ============================================================================
// Batch 2-equation integrator (for groundwater SoA)
// ============================================================================

// Contract: openswmm::ode::integrate_batch_2eq.
int integrate_batch_2eq(double* y0, double* y1, int n_sys, double x1, double x2, double eps,
                        double h1, const BatchDerivFunc& derivs) {
    static thread_local std::vector<double> dy0_buf, dy1_buf;
    dy0_buf.resize(static_cast<std::size_t>(n_sys));
    dy1_buf.resize(static_cast<std::size_t>(n_sys));

    double dt = x2 - x1;
    derivs(x1, y0, y1, dy0_buf.data(), dy1_buf.data(), n_sys);

    for (int i = 0; i < n_sys; ++i) {
        y0[i] += dy0_buf[i] * dt;
        y1[i] += dy1_buf[i] * dt;
    }

    (void)eps;
    (void)h1;
    return ODE_OK;
}

} // namespace ode
} // namespace openswmm
