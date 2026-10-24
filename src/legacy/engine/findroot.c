//-----------------------------------------------------------------------------
//   findroot.c
//
//   Finds solution of func(x) = 0 using either the Newton-Raphson
//   method or Ridder's Method.
//   Based on code from Numerical Recipes in C (Cambridge University
//   Press, 1992).
//
//   Date:     11/19/13
//   Author:   L. Rossman
//-----------------------------------------------------------------------------

/**
 * @file      findroot.c
 * @brief     Numerical root finding used by legacy hydraulic calculations.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com> (OpenSWMM maintenance and extensions)
 * @copyright Copyright (c) 2026 Caleb Buahin. OpenSWMM contributions only; original EPA SWMM
 *            credits are retained.
 * @license   Apache-2.0 for OpenSWMM contributions; original EPA SWMM provenance is retained.
 */

#include <math.h>
#include "findroot.h"

#define SIGN(a, b) ((b) >= 0.0 ? fabs(a) : -fabs(a))
#define MAXIT 60

/**
 * @brief Find a bracketed root using Newton steps with bisection fallback.
 * @par Implementation arguments
 * - x1 (in): Bracket endpoint where the callback function is negative.
 * - x2 (in): Bracket endpoint where the callback function is positive;
 * coordinate order may be reversed to obtain this sign ordering.
 * - rts (in,out): Non-null initial guess within the bracket, overwritten
 * with the last estimate on convergence or iteration exhaustion.
 * - xacc (in): Positive absolute step tolerance in the coordinate units.
 * - func (in): Non-null synchronous callback filling f and df/dx; it
 * must not retain the solver's temporary output pointers.
 * - p (in): Borrowed opaque callback context, passed through unchanged;
 * may be NULL when the callback does not require it.
 * @return Evaluation count, or zero after more than MAXIT evaluations.
 * A stationary floating-point estimate also ends iteration.
 * @pre The caller validates/orients the bracket. Endpoints are not evaluated
 * before the initial guess, preserving the legacy callback sequence.
 */
int findroot_Newton(double x1, double x2, double* rts, double xacc, newton_callback func, void* p)
//
//  Using a combination of Newton-Raphson and bisection, find the root of a
//  function func bracketed between x1 and x2. The root, returned in rts,
//  will be refined until its accuracy is known within +/-xacc. func is a
//  user-supplied routine, that returns both the function value and the first
//  derivative of the function. p is a pointer to any auxilary data structure
//  that func may require. It can be NULL if not needed. The function returns
//  the number of function evaluations used or 0 if the maximum allowed
//  iterations were exceeded.
//
// NOTES:
// 1. The calling program must insure that the signs of func(x1) and func(x2)
//    are not the same, otherwise x1 and x2 do not bracket the root.
// 2. If func(x1) > func(x2) then the order of x1 and x2 should be
//    switched in the call to Newton.
//
{
    int j, n = 0;
    double df, dx, dxold, f, x;
    double temp, xhi, xlo;

    // Initialize the "stepsize before last" and the last step.
    x = *rts;
    xlo = x1;
    xhi = x2;
    dxold = fabs(x2 - x1);
    dx = dxold;
    func(x, &f, &df, p);
    n++;

    // Loop over allowed iterations.
    for (j = 1; j <= MAXIT; j++) {
        // Bisect if Newton out of range or not decreasing fast enough.
        if ((((x - xhi) * df - f) * ((x - xlo) * df - f) >= 0.0 ||
             (fabs(2.0 * f) > fabs(dxold * df)))) {
            dxold = dx;
            dx = 0.5 * (xhi - xlo);
            x = xlo + dx;
            if (xlo == x)
                break;
        }

        // Newton step acceptable. Take it.
        else {
            dxold = dx;
            dx = f / df;
            temp = x;
            x -= dx;
            if (temp == x)
                break;
        }

        // Convergence criterion.
        if (fabs(dx) < xacc)
            break;

        // Evaluate function. Maintain bracket on the root.
        func(x, &f, &df, p);
        n++;
        if (f < 0.0)
            xlo = x;
        else
            xhi = x;
    }
    *rts = x;
    if (n <= MAXIT)
        return n;
    else
        return 0;
};

/**
 * @brief Estimate a bracketed scalar root with derivative-free Ridder iterations.
 * @par Implementation arguments
 * - x1 (in): First bracket coordinate.
 * - x2 (in): Second bracket coordinate; endpoint function values must have
 * opposite signs unless an endpoint is already a root.
 * - xacc (in): Positive absolute tolerance in the coordinate units.
 * - func (in): Non-null synchronous evaluator; called at both endpoints
 * and successive trial points, without retaining the callback.
 * - p (in): Borrowed opaque callback context; may be NULL if unused.
 * @return Exact endpoint root, the last approximate root, or -1.0e20 for an
 * unbracketed root. Exhaustion has no separate failure status.
 * @note When a newly calculated root is within xacc of the previous estimate,
 * the previous estimate is returned; the last callback point may differ.
 */
double findroot_Ridder(double x1, double x2, double xacc, ridder_callback func, void* p) {
    int j;
    double ans, fhi, flo, fm, fnew, s, xhi, xlo, xm, xnew;

    flo = func(x1, p);
    fhi = func(x2, p);
    if (flo == 0.0)
        return x1;
    if (fhi == 0.0)
        return x2;
    ans = 0.5 * (x1 + x2);
    if ((flo > 0.0 && fhi < 0.0) || (flo < 0.0 && fhi > 0.0)) {
        xlo = x1;
        xhi = x2;
        for (j = 1; j <= MAXIT; j++) {
            xm = 0.5 * (xlo + xhi);
            fm = func(xm, p);
            s = sqrt(fm * fm - flo * fhi);
            if (s == 0.0)
                return ans;
            xnew = xm + (xm - xlo) * ((flo >= fhi ? 1.0 : -1.0) * fm / s);
            if (fabs(xnew - ans) <= xacc)
                break;
            ans = xnew;
            fnew = func(ans, p);
            if (SIGN(fm, fnew) != fm) {
                xlo = xm;
                flo = fm;
                xhi = ans;
                fhi = fnew;
            } else if (SIGN(flo, fnew) != flo) {
                xhi = ans;
                fhi = fnew;
            } else if (SIGN(fhi, fnew) != fhi) {
                xlo = ans;
                flo = fnew;
            } else
                return ans;
            if (fabs(xhi - xlo) <= xacc)
                return ans;
        }
        return ans;
    }
    return -1.e20;
}
