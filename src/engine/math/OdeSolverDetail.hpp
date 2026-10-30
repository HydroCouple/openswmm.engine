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
 * @file      OdeSolverDetail.hpp
 * @brief     Shared Cash-Karp implementation for typed and erased derivative callbacks.
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */
#pragma once
#include "OdeSolver.hpp"
#include <cmath>
#include <utility>

namespace openswmm::ode {
namespace detail {
// Cash-Karp RK45 coefficients
static constexpr double a2 = 0.2, a3 = 0.3, a4 = 0.6, a5 = 1.0, a6 = 0.875;

static constexpr double b21 = 0.2;
static constexpr double b31 = 3.0 / 40.0, b32 = 9.0 / 40.0;
static constexpr double b41 = 0.3, b42 = -0.9, b43 = 1.2;
static constexpr double b51 = -11.0 / 54.0, b52 = 2.5, b53 = -70.0 / 27.0, b54 = 35.0 / 27.0;
static constexpr double b61 = 1631.0 / 55296.0, b62 = 175.0 / 512.0, b63 = 575.0 / 13824.0,
                        b64 = 44275.0 / 110592.0, b65 = 253.0 / 4096.0;

static constexpr double c1 = 37.0 / 378.0, c3 = 250.0 / 621.0, c4 = 125.0 / 594.0,
                        c6 = 512.0 / 1771.0;
static constexpr double dc1 = c1 - 2825.0 / 27648.0;
static constexpr double dc3 = c3 - 18575.0 / 48384.0;
static constexpr double dc4 = c4 - 13525.0 / 55296.0;
static constexpr double dc5 = -277.0 / 14336.0;
static constexpr double dc6 = c6 - 0.25;

struct OdeWorkspace {
    int nmax = 0;
    double* y = nullptr; // internal state buffer (matches legacy y[])
    double* yscal = nullptr;
    double* dydx = nullptr;
    double* yerr = nullptr;
    double* ytemp = nullptr;
    double* ak = nullptr; // flat n*5 (ak2..ak6) matching legacy

    /**
     * @brief Release this thread's six solver buffers and reset their pointers.
     * @note The nontrivial destructor registers thread-exit cleanup for reused
     * workspace. Buffers belong exclusively to this workspace.
     */
    ~OdeWorkspace();
};
OdeWorkspace& workspace(int n);

// ============================================================================
// rkck — One RK45 Cash-Karp step (matching legacy rkck exactly)
// ============================================================================

/**
 * @brief Evaluate one Cash-Karp trial step in the thread-local workspace.
 * @tparam F Callable with the DerivFunc argument convention.
 * @param[in] x Independent variable at the current state.
 * @param[in] n Positive equation count, no larger than allocated workspace.
 * @param[in] h Trial step in x units; may be negative.
 * @param[in] derivs Borrowed synchronous derivative evaluator, not retained.
 * @pre ws_.y and ws_.dydx hold the current state and its derivative.
 * @post ws_.ytemp contains the fifth-order trial state and ws_.yerr its
 * embedded error estimate. Current accepted ws_.y is unchanged.
 */
template <typename F> void rkck(OdeWorkspace& ws_, double x, int n, double h, F&& derivs) {
    double* ak2 = ws_.ak;
    double* ak3 = ws_.ak + n;
    double* ak4 = ws_.ak + n * 2;
    double* ak5 = ws_.ak + n * 3;
    double* ak6 = ws_.ak + n * 4;

    for (int i = 0; i < n; i++)
        ws_.ytemp[i] = ws_.y[i] + b21 * h * ws_.dydx[i];
    derivs(x + a2 * h, ws_.ytemp, ak2);

    for (int i = 0; i < n; i++)
        ws_.ytemp[i] = ws_.y[i] + h * (b31 * ws_.dydx[i] + b32 * ak2[i]);
    derivs(x + a3 * h, ws_.ytemp, ak3);

    for (int i = 0; i < n; i++)
        ws_.ytemp[i] = ws_.y[i] + h * (b41 * ws_.dydx[i] + b42 * ak2[i] + b43 * ak3[i]);
    derivs(x + a4 * h, ws_.ytemp, ak4);

    for (int i = 0; i < n; i++)
        ws_.ytemp[i] =
            ws_.y[i] + h * (b51 * ws_.dydx[i] + b52 * ak2[i] + b53 * ak3[i] + b54 * ak4[i]);
    derivs(x + a5 * h, ws_.ytemp, ak5);

    for (int i = 0; i < n; i++)
        ws_.ytemp[i] = ws_.y[i] + h * (b61 * ws_.dydx[i] + b62 * ak2[i] + b63 * ak3[i] +
                                       b64 * ak4[i] + b65 * ak5[i]);
    derivs(x + a6 * h, ws_.ytemp, ak6);

    for (int i = 0; i < n; i++)
        ws_.ytemp[i] = ws_.y[i] + h * (c1 * ws_.dydx[i] + c3 * ak3[i] + c4 * ak4[i] + c6 * ak6[i]);

    for (int i = 0; i < n; i++)
        ws_.yerr[i] =
            h * (dc1 * ws_.dydx[i] + dc3 * ak3[i] + dc4 * ak4[i] + dc5 * ak5[i] + dc6 * ak6[i]);
}

// ============================================================================
// rkqs — Quality-controlled RK step (matching legacy rkqs exactly)
// ============================================================================

/**
 * @brief Reject and shrink trial RK45 steps until local error is acceptable.
 * @tparam F Callable with the DerivFunc argument convention.
 * @param[in,out] x Non-null current coordinate; advanced only on acceptance.
 * @param[in] n Positive equation count covered by thread-local workspace.
 * @param[in] htry Initial attempted step, in x units.
 * @param[in] eps Positive dimensionless scaled-error tolerance.
 * @param[out] hdid Non-null slot for the accepted step; unchanged on underflow.
 * @param[out] hnext Non-null slot for the next proposed step; unchanged on underflow.
 * @param[in] derivs Borrowed synchronous derivative evaluator, not retained.
 * @return ODE_OK after committing ws_.y, or ODE_UNDERFLOW when a rejected
 * step becomes too small to change x. Callback exceptions propagate.
 * @pre ws_.y, ws_.dydx and ws_.yscal are initialized for x.
 */
template <typename F>
int rkqs(OdeWorkspace& ws_, double* x, int n, double htry, double eps, double* hdid, double* hnext,
                F&& derivs) {
    double err, errmax, h, htemp, xnew, xold = *x;

    h = htry;
    for (;;) {
        rkck(ws_, xold, n, h, derivs);

        errmax = 0.0;
        for (int i = 0; i < n; i++) {
            err = std::fabs(ws_.yerr[i] / ws_.yscal[i]);
            if (err > errmax)
                errmax = err;
        }
        errmax /= eps;

        if (errmax > 1.0) {
            htemp = SAFETY * h * std::pow(errmax, PSHRNK);
            if (h >= 0) {
                if (htemp > 0.1 * h)
                    h = htemp;
                else
                    h = 0.1 * h;
            } else {
                if (htemp < 0.1 * h)
                    h = htemp;
                else
                    h = 0.1 * h;
            }
            xnew = xold + h;
            if (xnew == xold)
                return ODE_UNDERFLOW;
            continue;
        } else {
            if (errmax > ERRCON)
                *hnext = SAFETY * h * std::pow(errmax, PGROW);
            else
                *hnext = 5.0 * h;
            *x += (*hdid = h);
            for (int i = 0; i < n; i++)
                ws_.y[i] = ws_.ytemp[i];
            return ODE_OK;
        }
    }
}

template <typename F>
int integrate(double* ystart, int n, double x1, double x2, double eps, double h1,
              F&& derivs) {
    auto& ws_ = workspace(n);
    if (ws_.nmax < n)
        return ODE_TOO_MANY;

    double hdid, hnext;
    double x = x1;
    double h = h1;

    for (int i = 0; i < n; i++)
        ws_.y[i] = ystart[i];

    for (int nstp = 1; nstp <= MAXSTP; nstp++) {
        derivs(x, ws_.y, ws_.dydx);
        for (int i = 0; i < n; i++)
            ws_.yscal[i] = std::fabs(ws_.y[i]) + std::fabs(ws_.dydx[i] * h) + TINY;
        if ((x + h - x2) * (x + h - x1) > 0.0)
            h = x2 - x;

        int errcode = rkqs(ws_, &x, n, h, eps, &hdid, &hnext, derivs);
        if (errcode)
            break;

        if ((x - x2) * (x2 - x1) >= 0.0) {
            for (int i = 0; i < n; i++)
                ystart[i] = ws_.y[i];
            return ODE_OK;
        }
        if (std::fabs(hnext) <= 0.0)
            return ODE_UNDERFLOW;
        h = hnext;
    }
    return ODE_MAX_STEPS;
}
} // namespace detail

template <typename F>
int integrateCallable(double* y, int n, double x1, double x2, double eps, double h1, F&& derivs) {
    return detail::integrate(y, n, x1, x2, eps, h1, std::forward<F>(derivs));
}
} // namespace openswmm::ode
