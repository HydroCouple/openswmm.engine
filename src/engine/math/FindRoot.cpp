/**
 * @file FindRoot.cpp
 * @brief Newton-Raphson + Ridder — numerically identical to legacy findroot.c.
 * @ingroup engine_math
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
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
    double fl = func(x1);
    double fh = func(x2);

    if (fl == 0.0) return x1;
    if (fh == 0.0) return x2;

    double ans = -1.0e10;
    double xlo = x1, xhi = x2;

    for (int j = 1; j <= MAXIT; ++j) {
        double xm = 0.5 * (xlo + xhi);
        double fm = func(xm);

        double s = std::sqrt(fm * fm - fl * fh);
        if (s == 0.0) return ans;

        double xnew = xm + (xm - xlo) * ((fl >= fh ? 1.0 : -1.0) * fm / s);

        if (std::fabs(xnew - ans) <= xacc) return xnew;
        ans = xnew;

        double fnew = func(ans);
        if (fnew == 0.0) return ans;

        // Update bracket
        if (fm * fnew < 0.0) {
            xlo = xm; fl = fm;
            xhi = ans; fh = fnew;
        } else if (fl * fnew < 0.0) {
            xhi = ans; fh = fnew;
        } else {
            xlo = ans; fl = fnew;
        }

        if (std::fabs(xhi - xlo) <= xacc) return ans;
    }

    return ans;
}

} // namespace findroot
} // namespace openswmm
