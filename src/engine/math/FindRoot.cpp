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
 * @file FindRoot.cpp
 * @brief Newton-Raphson + Ridder — numerically identical to legacy findroot.c.
 * @ingroup engine_math
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "FindRoot.hpp"
#include <cmath>

namespace openswmm {
namespace findroot {

int newton(double x1, double x2, double* rts, double xacc, const NewtonFunc& func) {
    double xlo, xhi, f, df, dx, dxold, temp;

    // Orient bracket so f(xlo) < 0
    double fl, fh, dfl, dfh;
    func(x1, &fl, &dfl);
    func(x2, &fh, &dfh);

    if (fl < 0.0) { xlo = x1; xhi = x2; }
    else          { xhi = x1; xlo = x2; }

    double x = *rts;
    dxold = std::fabs(x2 - x1);
    dx = dxold;
    func(x, &f, &df);

    for (int j = 1; j <= MAXIT; ++j) {
        // Use bisection if Newton out of bracket or converging slowly
        if (((x - xhi) * df - f) * ((x - xlo) * df - f) >= 0.0 ||
            std::fabs(2.0 * f) > std::fabs(dxold * df)) {
            dxold = dx;
            dx = 0.5 * (xhi - xlo);
            x = xlo + dx;
            if (xlo == x) { *rts = x; return j; }
        } else {
            dxold = dx;
            dx = f / df;
            temp = x;
            x -= dx;
            if (temp == x) { *rts = x; return j; }
        }

        if (std::fabs(dx) < xacc) { *rts = x; return j; }

        func(x, &f, &df);
        if (f < 0.0) xlo = x;
        else         xhi = x;
    }

    *rts = x;
    return 0;  // exceeded MAXIT
}

double ridder(double x1, double x2, double xacc, const RidderFunc& func) {
    // legacy findroot_Ridder, op for op — including the evaluation SEQUENCE,
    // which the culvert's Form-1 flow depends on (it reads the last
    // form1Eqn evaluation, not the root), the initial estimate 0.5*(x1+x2),
    // the return of the PREVIOUS estimate when the new one is within xacc
    // (`break` then `return ans`), the SIGN-based bracket updates and the
    // -1e20 no-bracket return.
    auto SIGN = [](double x, double y) { return (y >= 0.0) ? std::fabs(x) : -std::fabs(x); };
    double flo = func(x1);
    double fhi = func(x2);
    if (flo == 0.0) return x1;
    if (fhi == 0.0) return x2;
    double ans = 0.5 * (x1 + x2);
    if ((flo > 0.0 && fhi < 0.0) || (flo < 0.0 && fhi > 0.0)) {
        double xlo = x1;
        double xhi = x2;
        for (int j = 1; j <= MAXIT; ++j) {
            const double xm = 0.5 * (xlo + xhi);
            const double fm = func(xm);
            const double s = std::sqrt(fm * fm - flo * fhi);
            if (s == 0.0) return ans;
            const double xnew = xm + (xm - xlo) * ((flo >= fhi ? 1.0 : -1.0) * fm / s);
            if (std::fabs(xnew - ans) <= xacc) break;
            ans = xnew;
            const double fnew = func(ans);
            if (SIGN(fm, fnew) != fm) {
                xlo = xm;  flo = fm;
                xhi = ans; fhi = fnew;
            } else if (SIGN(flo, fnew) != flo) {
                xhi = ans; fhi = fnew;
            } else if (SIGN(fhi, fnew) != fhi) {
                xlo = ans; flo = fnew;
            } else {
                return ans;
            }
            if (std::fabs(xhi - xlo) <= xacc) return ans;
        }
        return ans;
    }
    return -1.0e20;
}

} // namespace findroot
} // namespace openswmm
