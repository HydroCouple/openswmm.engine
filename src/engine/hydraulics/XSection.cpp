/**
 * @file XSection.cpp
 * @brief Cross-section geometry — faithful 1:1 port of legacy xsect.c.
 *
 * @details All formulas, lookup tables, constants, and dispatch logic are
 *          direct translations from src/legacy/engine/xsect.c (SWMM 5.2.x).
 *          Table data comes from xsect_tables.hpp. Numerical parity with the
 *          legacy engine is verified shape-by-shape by
 *          tests/unit/engine/test_xsect_parity.cpp.
 *
 *          Field-name mapping (legacy TXsect -> XSectParams):
 *            yFull->y_full  wMax->w_max  ywMax->yw_max  aFull->a_full
 *            rFull->r_full  sFull->s_full  sMax->s_max
 *            yBot->y_bot  aBot->a_bot  sBot->s_bot  rBot->r_bot
 *
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#include "XSectBatch.hpp"
#include "xsect_tables.hpp"

#include <cmath>
#include <algorithm>

namespace openswmm {
namespace xsect {

// Constants matching legacy consts.h / xsect.c exactly (for bit-faithful parity).
static constexpr double TINY    = 1.0e-6;
static constexpr double PI      = 3.141592654;
static constexpr double GRAVITY = 32.2;

static constexpr double RECT_ALFMAX        = 0.97;
static constexpr double RECT_TRIANG_ALFMAX = 0.98;
static constexpr double RECT_ROUND_ALFMAX  = 0.98;

// Forward declarations of dispatchers used by helpers.
double getAofY(const XSectParams& xs, double y);
double getWofY(const XSectParams& xs, double y);
double getRofA(const XSectParams& xs, double a);
double getSofA(const XSectParams& xs, double a);
double getdSdA(const XSectParams& xs, double a);
double getAmax(const XSectParams& xs);
double getAofY(const XSectParams& xs, double y);
double getWofY(const XSectParams& xs, double y);

// ============================================================================
// Root finders — faithful ports of legacy findroot.c (Numerical Recipes).
// ============================================================================

// Newton-Raphson + bisection. func(x, &f, &df). Root bracketed in [x1, x2].
template <class Func>
static int findroot_Newton(double x1, double x2, double* rts, double xacc, Func func) {
    constexpr int MAXIT = 60;
    int n = 0;
    double df, dx, dxold, f, x, temp, xhi, xlo;

    x = *rts;
    xlo = x1;
    xhi = x2;
    dxold = std::fabs(x2 - x1);
    dx = dxold;
    func(x, &f, &df);
    n++;
    for (int j = 1; j <= MAXIT; ++j) {
        if (((x - xhi) * df - f) * ((x - xlo) * df - f) >= 0.0 ||
            (std::fabs(2.0 * f) > std::fabs(dxold * df))) {
            dxold = dx;
            dx = 0.5 * (xhi - xlo);
            x = xlo + dx;
            if (xlo == x) break;
        } else {
            dxold = dx;
            dx = f / df;
            temp = x;
            x -= dx;
            if (temp == x) break;
        }
        if (std::fabs(dx) < xacc) break;
        func(x, &f, &df);
        n++;
        if (f < 0.0) xlo = x;
        else         xhi = x;
    }
    *rts = x;
    return (n <= MAXIT) ? n : 0;
}

// Ridder's method. func(x) -> double. Root bracketed in [x1, x2].
template <class Func>
static double findroot_Ridder(double x1, double x2, double xacc, Func func) {
    constexpr int MAXIT = 60;
    auto SIGN = [](double a, double b) { return (b >= 0.0) ? std::fabs(a) : -std::fabs(a); };

    double ans, fhi, flo, fm, fnew, s, xhi, xlo, xm, xnew;
    flo = func(x1);
    fhi = func(x2);
    if (flo == 0.0) return x1;
    if (fhi == 0.0) return x2;
    ans = 0.5 * (x1 + x2);
    if ((flo > 0.0 && fhi < 0.0) || (flo < 0.0 && fhi > 0.0)) {
        xlo = x1;
        xhi = x2;
        for (int j = 1; j <= MAXIT; ++j) {
            xm = 0.5 * (xlo + xhi);
            fm = func(xm);
            s = std::sqrt(fm * fm - flo * fhi);
            if (s == 0.0) return ans;
            xnew = xm + (xm - xlo) * ((flo >= fhi ? 1.0 : -1.0) * fm / s);
            if (std::fabs(xnew - ans) <= xacc) break;
            ans = xnew;
            fnew = func(ans);
            if (SIGN(fm, fnew) != fm)      { xlo = xm; flo = fm; xhi = ans; fhi = fnew; }
            else if (SIGN(flo, fnew) != flo) { xhi = ans; fhi = fnew; }
            else if (SIGN(fhi, fnew) != fhi) { xlo = ans; flo = fnew; }
            else return ans;
            if (std::fabs(xhi - xlo) <= xacc) return ans;
        }
        return ans;
    }
    return -1.e20;
}

// ============================================================================
// Lookup helpers — identical to legacy lookup()/invLookup()/locate()
// ============================================================================

int locate(double y, const double* table, int jLast) {
    // Bisection: highest index j with table[j] <= y (legacy locate()).
    int j1 = 0;
    int j2 = jLast;
    if (y <= table[0])     return 0;
    if (y >= table[jLast]) return jLast;
    while (j2 - j1 > 1) {
        int j = (j1 + j2) >> 1;
        if (y >= table[j]) j1 = j;
        else               j2 = j;
    }
    return j1;
}

double lookup(double x, const double* table, int n_items) {
    double delta = 1.0 / static_cast<double>(n_items - 1);
    int i = static_cast<int>(x / delta);
    if (i >= n_items - 1) return table[n_items - 1];

    double x0 = i * delta;
    double x1 = (static_cast<double>(i) + 1.0) * delta;
    double y = table[i] + (x - x0) * (table[i + 1] - table[i]) / delta;

    // Quadratic refinement for low x (legacy: i < 2)
    if (i < 2) {
        double y2 = y + (x - x0) * (x - x1) / (delta * delta) *
                    (table[i] / 2.0 - table[i + 1] + table[i + 2] / 2.0);
        if (y2 > 0.0) y = y2;
    }
    if (y < 0.0) y = 0.0;
    return y;
}

double invLookup(double y, const double* table, int n_items) {
    double dx = 1.0 / static_cast<double>(n_items - 1);
    int n = n_items;

    // Truncate item count if last 2 entries are decreasing (section-factor tables)
    if (table[n - 3] > table[n - 1]) n = n - 2;

    int i;
    if (n < n_items && y > table[n_items - 1]) {
        if (y >= table[n_items - 3]) return static_cast<double>(n - 1) * dx;
        if (y <= table[n_items - 2]) i = n_items - 2;
        else                         i = n_items - 3;
    } else {
        i = locate(y, table, n - 1);
    }
    if (i >= n - 1) return static_cast<double>(n - 1) * dx;

    double x0 = i * dx;
    double dy = table[i + 1] - table[i];
    double x;
    if (dy == 0.0) x = x0;
    else           x = x0 + (y - table[i]) * dx / dy;
    if (x < 0.0) x = 0.0;
    if (x > 1.0) x = 1.0;
    return x;
}

// ============================================================================
// Circular small-area special functions (legacy getYcircular/getScircular/...)
// ============================================================================

static double getThetaOfAlpha(double alpha) {
    double theta, theta1, ap, d;
    if (alpha > 0.04) theta = 1.2 + 5.08 * (alpha - 0.04) / 0.96;
    else              theta = 0.031715 - 12.79384 * alpha + 8.28479 * std::sqrt(alpha);
    theta1 = theta;
    ap = (2.0 * PI) * alpha;
    for (int k = 1; k <= 40; ++k) {
        d = -(ap - theta + std::sin(theta)) / (1.0 - std::cos(theta));
        if (d > 1.0) d = std::copysign(1.0, d);
        theta = theta - d;
        if (std::fabs(d) <= 0.0001) return theta;
    }
    return theta1;
}

static double getThetaOfPsi(double psi) {
    double theta, theta1, ap, tt, tt23, t3, d;
    if      (psi > 0.90)  theta = 4.17 + 1.12 * (psi - 0.90) / 0.176;
    else if (psi > 0.5)   theta = 3.14 + 1.03 * (psi - 0.5) / 0.4;
    else if (psi > 0.015) theta = 1.2 + 1.94 * (psi - 0.015) / 0.485;
    else                  theta = 0.12103 - 55.5075 * psi + 15.62254 * std::sqrt(psi);
    theta1 = theta;
    ap = (2.0 * PI) * psi;
    for (int k = 1; k <= 40; ++k) {
        theta = std::fabs(theta);
        tt = theta - std::sin(theta);
        tt23 = std::pow(tt, 2.0 / 3.0);
        t3 = std::pow(theta, 1.0 / 3.0);
        d = ap * theta / t3 - tt * tt23;
        d = d / (ap * (2.0 / 3.0) / t3 - (5.0 / 3.0) * tt23 * (1.0 - std::cos(theta)));
        theta = theta - d;
        if (std::fabs(d) <= 0.0001) return theta;
    }
    return theta1;
}

double getYcircular(double alpha) {
    double theta;
    if (alpha >= 1.0) return 1.0;
    if (alpha <= 0.0) return 0.0;
    if (alpha <= 1.0e-5) {
        theta = std::pow(37.6911 * alpha, 1.0 / 3.0);
        return theta * theta / 16.0;
    }
    theta = getThetaOfAlpha(alpha);
    return (1.0 - std::cos(theta / 2.0)) / 2.0;
}

double getScircular(double alpha) {
    double theta;
    if (alpha >= 1.0) return 1.0;
    if (alpha <= 0.0) return 0.0;
    if (alpha <= 1.0e-5) {
        theta = std::pow(37.6911 * alpha, 1.0 / 3.0);
        return std::pow(theta, 13.0 / 3.0) / 124.4797;
    }
    theta = getThetaOfAlpha(alpha);
    return std::pow((theta - std::sin(theta)), 5.0 / 3.0) / (2.0 * PI) /
           std::pow(theta, 2.0 / 3.0);
}

static double getAcircular(double psi) {
    double theta;
    if (psi >= 1.0) return 1.0;
    if (psi <= 0.0) return 0.0;
    if (psi <= 1.0e-6) {
        theta = std::pow(124.4797 * psi, 3.0 / 13.0);
        return theta * theta * theta / 37.6911;
    }
    theta = getThetaOfPsi(psi);
    return (theta - std::sin(theta)) / (2.0 * PI);
}

// ============================================================================
// Generic / tabular section-factor derivatives (legacy)
// ============================================================================

static double generic_getdSdA(const XSectParams& xs, double a) {
    double alpha = a / xs.a_full;
    double alpha1 = alpha - 0.001;
    double alpha2 = alpha + 0.001;
    if (alpha1 < 0.0) alpha1 = 0.0;
    double a1 = alpha1 * xs.a_full;
    double a2 = alpha2 * xs.a_full;
    return (getSofA(xs, a2) - getSofA(xs, a1)) / (a2 - a1);
}

static double tabular_getdSdA(const XSectParams& xs, double a,
                              const double* table, int n_items) {
    double alpha = a / xs.a_full;
    double delta = 1.0 / static_cast<double>(n_items - 1);
    int i = static_cast<int>(alpha / delta);
    if (i >= n_items - 1) i = n_items - 2;
    double dSdA = (table[i + 1] - table[i]) / delta;
    return dSdA * xs.s_full / xs.a_full;
}

// ============================================================================
// Circular
// ============================================================================

static double circ_getAofY(const XSectParams& xs, double y) {
    double y_norm = y / xs.y_full;
    return xs.a_full * lookup(y_norm, xsect_tables::A_Circ, xsect_tables::N_A_Circ);
}

static double circ_getYofA(const XSectParams& xs, double a) {
    double alpha = a / xs.a_full;
    if (alpha < 0.04) return xs.y_full * getYcircular(alpha);
    return xs.y_full * lookup(alpha, xsect_tables::Y_Circ, xsect_tables::N_Y_Circ);
}

static double circ_getSofA(const XSectParams& xs, double a) {
    double alpha = a / xs.a_full;
    if (alpha < 0.04) return xs.s_full * getScircular(alpha);
    return xs.s_full * lookup(alpha, xsect_tables::S_Circ, xsect_tables::N_S_Circ);
}

static double circ_getAofS(const XSectParams& xs, double s) {
    double psi = s / xs.s_full;
    if (psi == 0.0) return 0.0;
    if (psi >= 1.0) return xs.a_full;
    if (psi <= 0.015) return xs.a_full * getAcircular(psi);
    return xs.a_full * invLookup(psi, xsect_tables::S_Circ, xsect_tables::N_S_Circ);
}

static double circ_getdSdA(const XSectParams& xs, double a) {
    double alpha = a / xs.a_full;
    if (alpha <= 1.0e-30) return 1.0e-30;
    if (alpha < 0.04) {
        double theta = getThetaOfAlpha(alpha);
        double p = theta * xs.y_full / 2.0;
        double r = a / p;
        double dPdA = 4.0 / xs.y_full / (1.0 - std::cos(theta));
        return (5.0 / 3.0 - (2.0 / 3.0) * dPdA * r) * std::pow(r, 2.0 / 3.0);
    }
    return tabular_getdSdA(xs, a, xsect_tables::S_Circ, xsect_tables::N_S_Circ);
}

// ============================================================================
// Filled circular (yBot/aBot/sBot/rBot = filled bottom: depth/area/width/perim)
// ============================================================================

static double filled_circ_getAofY(const XSectParams& xs, double y) {
    // Temporarily restore the unfilled full circle (legacy expands yFull/aFull).
    XSectParams t = xs;
    t.y_full += xs.y_bot;
    t.a_full += xs.a_bot;
    double a = circ_getAofY(t, y + xs.y_bot);
    return a - xs.a_bot;
}

static double filled_circ_getYofA(const XSectParams& xs, double a) {
    XSectParams t = xs;
    t.y_full += xs.y_bot;
    t.a_full += xs.a_bot;
    double y = circ_getYofA(t, a + xs.a_bot);
    return y - xs.y_bot;
}

static double filled_circ_getRofY(const XSectParams& xs, double y) {
    XSectParams t = xs;
    t.y_full += xs.y_bot;
    t.a_full += xs.a_bot;
    double yy = y + xs.y_bot;
    double a = circ_getAofY(t, yy);
    double r = 0.25 * t.y_full *
               lookup(yy / t.y_full, xsect_tables::R_Circ, xsect_tables::N_R_Circ);
    double p = (a / r);
    a = a - xs.a_bot;
    p = p - xs.r_bot + xs.s_bot;   // rBot = filled perimeter, sBot = filled width
    return a / p;
}

// ============================================================================
// Rectangular closed / open
// ============================================================================

static double rect_closed_getRofA(const XSectParams& xs, double a) {
    if (a <= 0.0) return 0.0;
    double p = xs.w_max + 2.0 * a / xs.w_max;
    if (a / xs.a_full > RECT_ALFMAX)
        p += (a / xs.a_full - RECT_ALFMAX) / (1.0 - RECT_ALFMAX) * xs.w_max;
    return a / p;
}

static double rect_closed_getSofA(const XSectParams& xs, double a) {
    if (a / xs.a_full > RECT_ALFMAX)
        return xs.s_max + (xs.s_full - xs.s_max) *
               (a / xs.a_full - RECT_ALFMAX) / (1.0 - RECT_ALFMAX);
    return a * std::pow(rect_closed_getRofA(xs, a), 2.0 / 3.0);
}

static double rect_closed_getdSdA(const XSectParams& xs, double a) {
    double alpha = a / xs.a_full;
    if (alpha > RECT_ALFMAX)
        return (xs.s_full - xs.s_max) / ((1.0 - RECT_ALFMAX) * xs.a_full);
    if (alpha <= 1.0e-30) return generic_getdSdA(xs, a);
    double r = rect_closed_getRofA(xs, a);
    return (5.0 / 3.0 - (2.0 / 3.0) * (2.0 / xs.w_max) * r) * std::pow(r, 2.0 / 3.0);
}

static double rect_open_getSofA(const XSectParams& xs, double a) {
    double y = a / xs.w_max;
    double r = a / ((2.0 - xs.s_bot) * y + xs.w_max);
    return a * std::pow(r, 2.0 / 3.0);
}

static double rect_open_getdSdA(const XSectParams& xs, double a) {
    if (a / xs.a_full <= 1.0e-30) return generic_getdSdA(xs, a);
    double r = getRofA(xs, a);
    double dPdA = (2.0 - xs.s_bot) / xs.w_max;
    return (5.0 / 3.0 - (2.0 / 3.0) * dPdA * r) * std::pow(r, 2.0 / 3.0);
}

// ============================================================================
// Rect-triangular (triangular bottom + rectangular top)
// ============================================================================

static double rect_triang_getAofY(const XSectParams& xs, double y) {
    if (y <= xs.y_bot) return y * y * xs.s_bot;
    return xs.a_bot + (y - xs.y_bot) * xs.w_max;
}

static double rect_triang_getWofY(const XSectParams& xs, double y) {
    if (y <= xs.y_bot) return 2.0 * xs.s_bot * y;
    return xs.w_max;
}

static double rect_triang_getYofA(const XSectParams& xs, double a) {
    if (a <= xs.a_bot) return std::sqrt(a / xs.s_bot);
    return xs.y_bot + (a - xs.a_bot) / xs.w_max;
}

static double rect_triang_getRofA(const XSectParams& xs, double a) {
    if (a <= 0.0) return 0.0;
    double y = rect_triang_getYofA(xs, a);
    if (y <= xs.y_bot) return a / (2.0 * y * xs.r_bot);
    double p = 2.0 * xs.y_bot * xs.r_bot + 2.0 * (y - xs.y_bot);
    double alf = (a / xs.a_full) - RECT_TRIANG_ALFMAX;
    if (alf > 0.0) p += alf / (1.0 - RECT_TRIANG_ALFMAX) * xs.w_max;
    return a / p;
}

static double rect_triang_getRofY(const XSectParams& xs, double y) {
    if (y <= xs.y_bot) return y * xs.s_bot / (2.0 * xs.r_bot);
    double a = xs.a_bot + (y - xs.y_bot) * xs.w_max;
    double p = 2.0 * xs.y_bot * xs.r_bot + 2.0 * (y - xs.y_bot);
    double alf = (a / xs.a_full) - RECT_TRIANG_ALFMAX;
    if (alf > 0.0) p += alf / (1.0 - RECT_TRIANG_ALFMAX) * xs.w_max;
    return a / p;
}

static double rect_triang_getSofA(const XSectParams& xs, double a) {
    double alfMax = RECT_TRIANG_ALFMAX;
    if (a / xs.a_full > alfMax)
        return xs.s_max + (xs.s_full - xs.s_max) *
               (a / xs.a_full - alfMax) / (1.0 - alfMax);
    return a * std::pow(rect_triang_getRofA(xs, a), 2.0 / 3.0);
}

static double rect_triang_getdSdA(const XSectParams& xs, double a) {
    double alfMax = RECT_TRIANG_ALFMAX;
    double alpha = a / xs.a_full;
    if (alpha > alfMax)
        return (xs.s_full - xs.s_max) / ((1.0 - alfMax) * xs.a_full);
    if (alpha <= 1.0e-30) return generic_getdSdA(xs, a);
    double dPdA;
    if (a > xs.a_bot) dPdA = 2.0 / xs.w_max;
    else              dPdA = xs.r_bot / std::sqrt(a * xs.s_bot);
    double r = rect_triang_getRofA(xs, a);
    return (5.0 / 3.0 - (2.0 / 3.0) * dPdA * r) * std::pow(r, 2.0 / 3.0);
}

// ============================================================================
// Rect-round (circular invert + rectangular top)
// ============================================================================

static double rect_round_getYofA(const XSectParams& xs, double a) {
    if (a > xs.a_bot) return xs.y_bot + (a - xs.a_bot) / xs.w_max;
    double alpha = a / (PI * xs.r_bot * xs.r_bot);
    if (alpha < 0.04) return (2.0 * xs.r_bot) * getYcircular(alpha);
    return (2.0 * xs.r_bot) * lookup(alpha, xsect_tables::Y_Circ, xsect_tables::N_Y_Circ);
}

static double rect_round_getAofY(const XSectParams& xs, double y) {
    if (y > xs.y_bot) return xs.a_bot + (y - xs.y_bot) * xs.w_max;
    double theta1 = 2.0 * std::acos(1.0 - y / xs.r_bot);
    return 0.5 * xs.r_bot * xs.r_bot * (theta1 - std::sin(theta1));
}

static double rect_round_getWofY(const XSectParams& xs, double y) {
    if (y > xs.y_bot) return xs.w_max;
    return 2.0 * std::sqrt(y * (2.0 * xs.r_bot - y));
}

static double rect_round_getRofA(const XSectParams& xs, double a) {
    if (a <= 0.0) return 0.0;
    if (a > xs.a_bot) {
        double y1 = (a - xs.a_bot) / xs.w_max;
        double theta1 = 2.0 * std::asin(xs.w_max / 2.0 / xs.r_bot);
        double p = xs.r_bot * theta1 + 2.0 * y1;
        double arg = (a / xs.a_full) - RECT_ROUND_ALFMAX;
        if (arg > 0.0) p += arg / (1.0 - RECT_ROUND_ALFMAX) * xs.w_max;
        return a / p;
    }
    double y1 = rect_round_getYofA(xs, a);
    double theta1 = 2.0 * std::acos(1.0 - y1 / xs.r_bot);
    double p = xs.r_bot * theta1;
    return a / p;
}

static double rect_round_getRofY(const XSectParams& xs, double y) {
    if (y <= 0.0) return 0.0;
    if (y > xs.y_bot) return rect_round_getRofA(xs, rect_round_getAofY(xs, y));
    double theta1 = 2.0 * std::acos(1.0 - y / xs.r_bot);
    return 0.5 * xs.r_bot * (1.0 - std::sin(theta1)) / theta1;
}

static double rect_round_getSofA(const XSectParams& xs, double a) {
    double alfMax = RECT_ROUND_ALFMAX;
    if (a / xs.a_full > alfMax)
        return xs.s_max + (xs.s_full - xs.s_max) *
               (a / xs.a_full - alfMax) / (1.0 - alfMax);
    if (a > xs.a_bot)
        return a * std::pow(rect_round_getRofA(xs, a), 2.0 / 3.0);
    double aFull = PI * xs.r_bot * xs.r_bot;
    double alpha = a / aFull;
    double sFull = xs.s_bot;
    if (alpha < 0.04) return sFull * getScircular(alpha);
    return sFull * lookup(alpha, xsect_tables::S_Circ, xsect_tables::N_S_Circ);
}

static double rect_round_getdSdA(const XSectParams& xs, double a) {
    double alfMax = RECT_ROUND_ALFMAX;
    if (a / xs.a_full > alfMax)
        return (xs.s_full - xs.s_max) / ((1.0 - alfMax) * xs.a_full);
    if (a > xs.a_bot) {
        double r = rect_round_getRofA(xs, a);
        double dPdA = 2.0 / xs.w_max;
        return (5.0 / 3.0 - (2.0 / 3.0) * dPdA * r) * std::pow(r, 2.0 / 3.0);
    }
    return generic_getdSdA(xs, a);
}

// ============================================================================
// Modified baskethandle (rectangular bottom + circular-arc top).
// Note: rBot/yBot/aBot/sBot refer to the CIRCULAR TOP portion.
// ============================================================================

static double mod_basket_getYofA(const XSectParams& xs, double a) {
    if (a <= xs.a_full - xs.a_bot) return a / xs.w_max;
    double alpha = (xs.a_full - a) / (PI * xs.r_bot * xs.r_bot);
    double y1;
    if (alpha < 0.04) y1 = getYcircular(alpha);
    else              y1 = lookup(alpha, xsect_tables::Y_Circ, xsect_tables::N_Y_Circ);
    y1 = 2.0 * xs.r_bot * y1;
    return xs.y_full - y1;
}

static double mod_basket_getAofY(const XSectParams& xs, double y) {
    if (y <= xs.y_full - xs.y_bot) return y * xs.w_max;
    double y1 = xs.y_full - y;
    double theta1 = 2.0 * std::acos(1.0 - y1 / xs.r_bot);
    double a1 = 0.5 * xs.r_bot * xs.r_bot * (theta1 - std::sin(theta1));
    return xs.a_full - a1;
}

static double mod_basket_getWofY(const XSectParams& xs, double y) {
    if (y <= 0.0) return 0.0;
    if (y <= xs.y_full - xs.y_bot) return xs.w_max;
    double y1 = xs.y_full - y;
    return 2.0 * std::sqrt(y1 * (2.0 * xs.r_bot - y1));
}

static double mod_basket_getRofA(const XSectParams& xs, double a) {
    if (a <= xs.a_full - xs.a_bot)
        return a / (xs.w_max + 2.0 * a / xs.w_max);
    double y1 = xs.y_full - mod_basket_getYofA(xs, a);
    double theta1 = 2.0 * std::acos(1.0 - y1 / xs.r_bot);
    double p = (xs.s_bot - theta1) * xs.r_bot;   // sBot = full circular opening angle
    y1 = xs.y_full - xs.y_bot;
    p = p + 2.0 * y1 + xs.w_max;
    return a / p;
}

static double mod_basket_getdSdA(const XSectParams& xs, double a) {
    if (a <= xs.a_full - xs.a_bot && a / xs.a_full > 1.0e-30) {
        double r = a / (xs.w_max + 2.0 * a / xs.w_max);
        double dPdA = 2.0 / xs.w_max;
        return (5.0 / 3.0 - (2.0 / 3.0) * dPdA * r) * std::pow(r, 2.0 / 3.0);
    }
    return generic_getdSdA(xs, a);
}

// ============================================================================
// Trapezoidal (yBot = bottom width, sBot = avg side slope, rBot = side length/depth)
// ============================================================================

static double trapez_getAofY(const XSectParams& xs, double y) {
    return (xs.y_bot + xs.s_bot * y) * y;
}

static double trapez_getWofY(const XSectParams& xs, double y) {
    return xs.y_bot + 2.0 * y * xs.s_bot;
}

static double trapez_getYofA(const XSectParams& xs, double a) {
    if (xs.s_bot == 0.0) return a / xs.y_bot;
    return (std::sqrt(xs.y_bot * xs.y_bot + 4.0 * xs.s_bot * a) - xs.y_bot) /
           (2.0 * xs.s_bot);
}

static double trapez_getRofA(const XSectParams& xs, double a) {
    return a / (xs.y_bot + trapez_getYofA(xs, a) * xs.r_bot);
}

static double trapez_getRofY(const XSectParams& xs, double y) {
    if (y == 0.0) return 0.0;
    return trapez_getAofY(xs, y) / (xs.y_bot + y * xs.r_bot);
}

static double trapez_getdSdA(const XSectParams& xs, double a) {
    if (a / xs.a_full <= 1.0e-30) return generic_getdSdA(xs, a);
    double r = trapez_getRofA(xs, a);
    double dPdA = xs.r_bot / std::sqrt(xs.y_bot * xs.y_bot + 4.0 * xs.s_bot * a);
    return (5.0 / 3.0 - (2.0 / 3.0) * dPdA * r) * std::pow(r, 2.0 / 3.0);
}

// ============================================================================
// Triangular
// ============================================================================

static double triang_getAofY(const XSectParams& xs, double y) {
    return y * y * xs.s_bot;
}

static double triang_getWofY(const XSectParams& xs, double y) {
    return 2.0 * xs.s_bot * y;
}

static double triang_getYofA(const XSectParams& xs, double a) {
    return std::sqrt(a / xs.s_bot);
}

static double triang_getRofA(const XSectParams& xs, double a) {
    return a / (2.0 * triang_getYofA(xs, a) * xs.r_bot);
}

static double triang_getRofY(const XSectParams& xs, double y) {
    return (y * xs.s_bot) / (2.0 * xs.r_bot);
}

static double triang_getdSdA(const XSectParams& xs, double a) {
    if (a / xs.a_full <= 1.0e-30) return generic_getdSdA(xs, a);
    double r = triang_getRofA(xs, a);
    double dPdA = xs.r_bot / std::sqrt(a * xs.s_bot);
    return (5.0 / 3.0 - (2.0 / 3.0) * dPdA * r) * std::pow(r, 2.0 / 3.0);
}

// ============================================================================
// Parabolic (rBot = 1/sqrt(c) where y = c*x^2)
// ============================================================================

static double parab_getAofY(const XSectParams& xs, double y) {
    return (4.0 / 3.0) * xs.r_bot * y * std::sqrt(y);
}

static double parab_getWofY(const XSectParams& xs, double y) {
    return 2.0 * xs.r_bot * std::sqrt(y);
}

static double parab_getYofA(const XSectParams& xs, double a) {
    return std::pow((3.0 / 4.0) * a / xs.r_bot, 2.0 / 3.0);
}

// Analytical wetted perimeter (legacy parab_getPofY).
static double parab_getPofY(const XSectParams& xs, double y) {
    double x = 2.0 * std::sqrt(y) / xs.r_bot;
    double t = std::sqrt(1.0 + x * x);
    return 0.5 * xs.r_bot * xs.r_bot * (x * t + std::log(x + t));
}

static double parab_getRofY(const XSectParams& xs, double y) {
    if (y <= 0.0) return 0.0;
    return parab_getAofY(xs, y) / parab_getPofY(xs, y);
}

static double parab_getRofA(const XSectParams& xs, double a) {
    if (a <= 0.0) return 0.0;
    return a / parab_getPofY(xs, parab_getYofA(xs, a));
}

// ============================================================================
// Power function (sBot = 1/exponent, rBot = coefficient)
// ============================================================================

static double powerfunc_getAofY(const XSectParams& xs, double y) {
    return xs.r_bot * std::pow(y, xs.s_bot + 1.0);
}

static double powerfunc_getWofY(const XSectParams& xs, double y) {
    return (xs.s_bot + 1.0) * xs.r_bot * std::pow(y, xs.s_bot);
}

static double powerfunc_getYofA(const XSectParams& xs, double a) {
    return std::pow(a / xs.r_bot, 1.0 / (xs.s_bot + 1.0));
}

// Numerical wetted perimeter (legacy powerfunc_getPofY — stepwise integration).
static double powerfunc_getPofY(const XSectParams& xs, double y) {
    double dy1 = 0.02 * xs.y_full;
    double h = (xs.s_bot + 1.0) * xs.r_bot / 2.0;
    double m = xs.s_bot;
    double p = 0.0, y1 = 0.0, x1 = 0.0;
    double x2, y2, dx, dy;
    do {
        y2 = y1 + dy1;
        if (y2 > y) y2 = y;
        x2 = h * std::pow(y2, m);
        dx = x2 - x1;
        dy = y2 - y1;
        p += std::sqrt(dx * dx + dy * dy);
        x1 = x2;
        y1 = y2;
    } while (y2 < y);
    return 2.0 * p;
}

static double powerfunc_getRofY(const XSectParams& xs, double y) {
    if (y <= 0.0) return 0.0;
    return powerfunc_getAofY(xs, y) / powerfunc_getPofY(xs, y);
}

static double powerfunc_getRofA(const XSectParams& xs, double a) {
    if (a <= 0.0) return 0.0;
    return a / powerfunc_getPofY(xs, powerfunc_getYofA(xs, a));
}

// ============================================================================
// Main dispatch: getAofY
// ============================================================================

double getAofY(const XSectParams& xs, double y) {
    if (y <= 0.0) return 0.0;
    double y_norm = y / xs.y_full;

    switch (static_cast<XSectShape>(xs.type)) {
        case XSectShape::FORCE_MAIN:
        case XSectShape::CIRCULAR:
            return xs.a_full * lookup(y_norm, xsect_tables::A_Circ, xsect_tables::N_A_Circ);
        case XSectShape::FILLED_CIRCULAR: return filled_circ_getAofY(xs, y);
        case XSectShape::EGGSHAPED:
            return xs.a_full * lookup(y_norm, xsect_tables::A_Egg, xsect_tables::N_A_Egg);
        case XSectShape::HORSESHOE:
            return xs.a_full * lookup(y_norm, xsect_tables::A_Horseshoe, xsect_tables::N_A_Horseshoe);
        case XSectShape::GOTHIC:
            return xs.a_full * invLookup(y_norm, xsect_tables::Y_Gothic, xsect_tables::N_Y_Gothic);
        case XSectShape::CATENARY:
            return xs.a_full * invLookup(y_norm, xsect_tables::Y_Catenary, xsect_tables::N_Y_Catenary);
        case XSectShape::SEMIELLIPTICAL:
            return xs.a_full * invLookup(y_norm, xsect_tables::Y_SemiEllip, xsect_tables::N_Y_SemiEllip);
        case XSectShape::BASKETHANDLE:
            return xs.a_full * lookup(y_norm, xsect_tables::A_Baskethandle, xsect_tables::N_A_Baskethandle);
        case XSectShape::SEMICIRCULAR:
            return xs.a_full * invLookup(y_norm, xsect_tables::Y_SemiCirc, xsect_tables::N_Y_SemiCirc);
        case XSectShape::HORIZ_ELLIPSE:
            return xs.a_full * lookup(y_norm, xsect_tables::A_HorizEllipse, xsect_tables::N_A_HorizEllipse);
        case XSectShape::VERT_ELLIPSE:
            return xs.a_full * lookup(y_norm, xsect_tables::A_VertEllipse, xsect_tables::N_A_VertEllipse);
        case XSectShape::ARCH:
            return xs.a_full * lookup(y_norm, xsect_tables::A_Arch, xsect_tables::N_A_Arch);
        case XSectShape::RECT_CLOSED:
        case XSectShape::RECT_OPEN:    return y * xs.w_max;
        case XSectShape::RECT_TRIANG:  return rect_triang_getAofY(xs, y);
        case XSectShape::RECT_ROUND:   return rect_round_getAofY(xs, y);
        case XSectShape::MOD_BASKET:   return mod_basket_getAofY(xs, y);
        case XSectShape::TRAPEZOIDAL:  return trapez_getAofY(xs, y);
        case XSectShape::TRIANGULAR:   return triang_getAofY(xs, y);
        case XSectShape::PARABOLIC:    return parab_getAofY(xs, y);
        case XSectShape::POWERFUNC:    return powerfunc_getAofY(xs, y);
        default: return 0.0;
    }
}

// ============================================================================
// Main dispatch: getWofY
// ============================================================================

double getWofY(const XSectParams& xs, double y) {
    double y_norm = y / xs.y_full;

    switch (static_cast<XSectShape>(xs.type)) {
        case XSectShape::FORCE_MAIN:
        case XSectShape::CIRCULAR:
            return xs.w_max * lookup(y_norm, xsect_tables::W_Circ, xsect_tables::N_W_Circ);
        case XSectShape::FILLED_CIRCULAR: {
            double yn = (y + xs.y_bot) / (xs.y_full + xs.y_bot);
            return xs.w_max * lookup(yn, xsect_tables::W_Circ, xsect_tables::N_W_Circ);
        }
        case XSectShape::EGGSHAPED:
            return xs.w_max * lookup(y_norm, xsect_tables::W_Egg, xsect_tables::N_W_Egg);
        case XSectShape::HORSESHOE:
            return xs.w_max * lookup(y_norm, xsect_tables::W_Horseshoe, xsect_tables::N_W_Horseshoe);
        case XSectShape::GOTHIC:
            return xs.w_max * lookup(y_norm, xsect_tables::W_Gothic, xsect_tables::N_W_Gothic);
        case XSectShape::CATENARY:
            return xs.w_max * lookup(y_norm, xsect_tables::W_Catenary, xsect_tables::N_W_Catenary);
        case XSectShape::SEMIELLIPTICAL:
            return xs.w_max * lookup(y_norm, xsect_tables::W_SemiEllip, xsect_tables::N_W_SemiEllip);
        case XSectShape::BASKETHANDLE:
            return xs.w_max * lookup(y_norm, xsect_tables::W_BasketHandle, xsect_tables::N_W_BasketHandle);
        case XSectShape::SEMICIRCULAR:
            return xs.w_max * lookup(y_norm, xsect_tables::W_SemiCirc, xsect_tables::N_W_SemiCirc);
        case XSectShape::HORIZ_ELLIPSE:
            return xs.w_max * lookup(y_norm, xsect_tables::W_HorizEllipse, xsect_tables::N_W_HorizEllipse);
        case XSectShape::VERT_ELLIPSE:
            return xs.w_max * lookup(y_norm, xsect_tables::W_VertEllipse, xsect_tables::N_W_VertEllipse);
        case XSectShape::ARCH:
            return xs.w_max * lookup(y_norm, xsect_tables::W_Arch, xsect_tables::N_W_Arch);
        case XSectShape::RECT_CLOSED:
            if (y_norm == 1.0) return 0.0;
            return xs.w_max;
        case XSectShape::RECT_OPEN:    return xs.w_max;
        case XSectShape::RECT_TRIANG:  return rect_triang_getWofY(xs, y);
        case XSectShape::RECT_ROUND:   return rect_round_getWofY(xs, y);
        case XSectShape::MOD_BASKET:   return mod_basket_getWofY(xs, y);
        case XSectShape::TRAPEZOIDAL:  return trapez_getWofY(xs, y);
        case XSectShape::TRIANGULAR:   return triang_getWofY(xs, y);
        case XSectShape::PARABOLIC:    return parab_getWofY(xs, y);
        case XSectShape::POWERFUNC:    return powerfunc_getWofY(xs, y);
        default: return 0.0;
    }
}

// ============================================================================
// Main dispatch: getRofY
// ============================================================================

double getRofY(const XSectParams& xs, double y) {
    double y_norm = y / xs.y_full;

    switch (static_cast<XSectShape>(xs.type)) {
        case XSectShape::FORCE_MAIN:
        case XSectShape::CIRCULAR:
            return xs.r_full * lookup(y_norm, xsect_tables::R_Circ, xsect_tables::N_R_Circ);
        case XSectShape::FILLED_CIRCULAR:
            if (xs.y_bot == 0.0)
                return xs.r_full * lookup(y_norm, xsect_tables::R_Circ, xsect_tables::N_R_Circ);
            return filled_circ_getRofY(xs, y);
        case XSectShape::EGGSHAPED:
            return xs.r_full * lookup(y_norm, xsect_tables::R_Egg, xsect_tables::N_R_Egg);
        case XSectShape::HORSESHOE:
            return xs.r_full * lookup(y_norm, xsect_tables::R_Horseshoe, xsect_tables::N_R_Horseshoe);
        case XSectShape::BASKETHANDLE:
            return xs.r_full * lookup(y_norm, xsect_tables::R_Baskethandle, xsect_tables::N_R_Baskethandle);
        case XSectShape::HORIZ_ELLIPSE:
            return xs.r_full * lookup(y_norm, xsect_tables::R_HorizEllipse, xsect_tables::N_R_HorizEllipse);
        case XSectShape::VERT_ELLIPSE:
            return xs.r_full * lookup(y_norm, xsect_tables::R_VertEllipse, xsect_tables::N_R_VertEllipse);
        case XSectShape::ARCH:
            return xs.r_full * lookup(y_norm, xsect_tables::R_Arch, xsect_tables::N_R_Arch);
        case XSectShape::RECT_TRIANG:  return rect_triang_getRofY(xs, y);
        case XSectShape::RECT_ROUND:   return rect_round_getRofY(xs, y);
        case XSectShape::TRAPEZOIDAL:  return trapez_getRofY(xs, y);
        case XSectShape::TRIANGULAR:   return triang_getRofY(xs, y);
        case XSectShape::PARABOLIC:    return parab_getRofY(xs, y);
        case XSectShape::POWERFUNC:    return powerfunc_getRofY(xs, y);
        default:  // RECT_CLOSED, RECT_OPEN, MOD_BASKET, tabulated S-only shapes
            return getRofA(xs, getAofY(xs, y));
    }
}

// ============================================================================
// Main dispatch: getYofA
// ============================================================================

double getYofA(const XSectParams& xs, double a) {
    if (a <= 0.0) return 0.0;
    double alpha = a / xs.a_full;

    switch (static_cast<XSectShape>(xs.type)) {
        case XSectShape::FORCE_MAIN:
        case XSectShape::CIRCULAR:        return circ_getYofA(xs, a);
        case XSectShape::FILLED_CIRCULAR: return filled_circ_getYofA(xs, a);
        case XSectShape::EGGSHAPED:
            return xs.y_full * lookup(alpha, xsect_tables::Y_Egg, xsect_tables::N_Y_Egg);
        case XSectShape::HORSESHOE:
            return xs.y_full * lookup(alpha, xsect_tables::Y_Horseshoe, xsect_tables::N_Y_Horseshoe);
        case XSectShape::GOTHIC:
            return xs.y_full * lookup(alpha, xsect_tables::Y_Gothic, xsect_tables::N_Y_Gothic);
        case XSectShape::CATENARY:
            return xs.y_full * lookup(alpha, xsect_tables::Y_Catenary, xsect_tables::N_Y_Catenary);
        case XSectShape::SEMIELLIPTICAL:
            return xs.y_full * lookup(alpha, xsect_tables::Y_SemiEllip, xsect_tables::N_Y_SemiEllip);
        case XSectShape::BASKETHANDLE:
            return xs.y_full * lookup(alpha, xsect_tables::Y_BasketHandle, xsect_tables::N_Y_BasketHandle);
        case XSectShape::SEMICIRCULAR:
            return xs.y_full * lookup(alpha, xsect_tables::Y_SemiCirc, xsect_tables::N_Y_SemiCirc);
        case XSectShape::HORIZ_ELLIPSE:
            return xs.y_full * invLookup(alpha, xsect_tables::A_HorizEllipse, xsect_tables::N_A_HorizEllipse);
        case XSectShape::VERT_ELLIPSE:
            return xs.y_full * invLookup(alpha, xsect_tables::A_VertEllipse, xsect_tables::N_A_VertEllipse);
        case XSectShape::ARCH:
            return xs.y_full * invLookup(alpha, xsect_tables::A_Arch, xsect_tables::N_A_Arch);
        case XSectShape::RECT_CLOSED:
        case XSectShape::RECT_OPEN:    return a / xs.w_max;
        case XSectShape::RECT_TRIANG:  return rect_triang_getYofA(xs, a);
        case XSectShape::RECT_ROUND:   return rect_round_getYofA(xs, a);
        case XSectShape::MOD_BASKET:   return mod_basket_getYofA(xs, a);
        case XSectShape::TRAPEZOIDAL:  return trapez_getYofA(xs, a);
        case XSectShape::TRIANGULAR:   return triang_getYofA(xs, a);
        case XSectShape::PARABOLIC:    return parab_getYofA(xs, a);
        case XSectShape::POWERFUNC:    return powerfunc_getYofA(xs, a);
        default: return 0.0;
    }
}

// ============================================================================
// Main dispatch: getSofA
// ============================================================================

double getSofA(const XSectParams& xs, double a) {
    double alpha = a / xs.a_full;

    switch (static_cast<XSectShape>(xs.type)) {
        case XSectShape::FORCE_MAIN:
        case XSectShape::CIRCULAR:     return circ_getSofA(xs, a);
        case XSectShape::EGGSHAPED:
            return xs.s_full * lookup(alpha, xsect_tables::S_Egg, xsect_tables::N_S_Egg);
        case XSectShape::HORSESHOE:
            return xs.s_full * lookup(alpha, xsect_tables::S_Horseshoe, xsect_tables::N_S_Horseshoe);
        case XSectShape::GOTHIC:
            return xs.s_full * lookup(alpha, xsect_tables::S_Gothic, xsect_tables::N_S_Gothic);
        case XSectShape::CATENARY:
            return xs.s_full * lookup(alpha, xsect_tables::S_Catenary, xsect_tables::N_S_Catenary);
        case XSectShape::SEMIELLIPTICAL:
            return xs.s_full * lookup(alpha, xsect_tables::S_SemiEllip, xsect_tables::N_S_SemiEllip);
        case XSectShape::BASKETHANDLE:
            return xs.s_full * lookup(alpha, xsect_tables::S_BasketHandle, xsect_tables::N_S_BasketHandle);
        case XSectShape::SEMICIRCULAR:
            return xs.s_full * lookup(alpha, xsect_tables::S_SemiCirc, xsect_tables::N_S_SemiCirc);
        case XSectShape::RECT_CLOSED:  return rect_closed_getSofA(xs, a);
        case XSectShape::RECT_OPEN:    return rect_open_getSofA(xs, a);
        case XSectShape::RECT_TRIANG:  return rect_triang_getSofA(xs, a);
        case XSectShape::RECT_ROUND:   return rect_round_getSofA(xs, a);
        default: {
            if (a == 0.0) return 0.0;
            double r = getRofA(xs, a);
            if (r < TINY) return 0.0;
            return a * std::pow(r, 2.0 / 3.0);
        }
    }
}

// ============================================================================
// getRofA — hydraulic radius from area
// ============================================================================

double getRofA(const XSectParams& xs, double a) {
    if (a <= 0.0) return 0.0;
    switch (static_cast<XSectShape>(xs.type)) {
        case XSectShape::HORIZ_ELLIPSE:
        case XSectShape::VERT_ELLIPSE:
        case XSectShape::ARCH:
        case XSectShape::IRREGULAR:
        case XSectShape::FILLED_CIRCULAR:
        case XSectShape::CUSTOM:
        case XSectShape::STREET_XSECT:
            return getRofY(xs, getYofA(xs, a));
        case XSectShape::RECT_CLOSED:  return rect_closed_getRofA(xs, a);
        case XSectShape::RECT_OPEN:
            return a / (xs.w_max + (2.0 - xs.s_bot) * a / xs.w_max);
        case XSectShape::RECT_TRIANG:  return rect_triang_getRofA(xs, a);
        case XSectShape::RECT_ROUND:   return rect_round_getRofA(xs, a);
        case XSectShape::MOD_BASKET:   return mod_basket_getRofA(xs, a);
        case XSectShape::TRAPEZOIDAL:  return trapez_getRofA(xs, a);
        case XSectShape::TRIANGULAR:   return triang_getRofA(xs, a);
        case XSectShape::PARABOLIC:    return parab_getRofA(xs, a);
        case XSectShape::POWERFUNC:    return powerfunc_getRofA(xs, a);
        default: {
            double s = getSofA(xs, a);
            if (s < TINY || a < TINY) return 0.0;
            return std::pow(s / a, 3.0 / 2.0);
        }
    }
}

// ============================================================================
// getdSdA — derivative of section factor w.r.t. area
// ============================================================================

double getdSdA(const XSectParams& xs, double a) {
    switch (static_cast<XSectShape>(xs.type)) {
        case XSectShape::FORCE_MAIN:
        case XSectShape::CIRCULAR:     return circ_getdSdA(xs, a);
        case XSectShape::EGGSHAPED:
            return tabular_getdSdA(xs, a, xsect_tables::S_Egg, xsect_tables::N_S_Egg);
        case XSectShape::HORSESHOE:
            return tabular_getdSdA(xs, a, xsect_tables::S_Horseshoe, xsect_tables::N_S_Horseshoe);
        case XSectShape::GOTHIC:
            return tabular_getdSdA(xs, a, xsect_tables::S_Gothic, xsect_tables::N_S_Gothic);
        case XSectShape::CATENARY:
            return tabular_getdSdA(xs, a, xsect_tables::S_Catenary, xsect_tables::N_S_Catenary);
        case XSectShape::SEMIELLIPTICAL:
            return tabular_getdSdA(xs, a, xsect_tables::S_SemiEllip, xsect_tables::N_S_SemiEllip);
        case XSectShape::BASKETHANDLE:
            return tabular_getdSdA(xs, a, xsect_tables::S_BasketHandle, xsect_tables::N_S_BasketHandle);
        case XSectShape::SEMICIRCULAR:
            return tabular_getdSdA(xs, a, xsect_tables::S_SemiCirc, xsect_tables::N_S_SemiCirc);
        case XSectShape::RECT_CLOSED:  return rect_closed_getdSdA(xs, a);
        case XSectShape::RECT_OPEN:    return rect_open_getdSdA(xs, a);
        case XSectShape::RECT_TRIANG:  return rect_triang_getdSdA(xs, a);
        case XSectShape::RECT_ROUND:   return rect_round_getdSdA(xs, a);
        case XSectShape::MOD_BASKET:   return mod_basket_getdSdA(xs, a);
        case XSectShape::TRAPEZOIDAL:  return trapez_getdSdA(xs, a);
        case XSectShape::TRIANGULAR:   return triang_getdSdA(xs, a);
        default: return generic_getdSdA(xs, a);
    }
}

// ============================================================================
// getAofS — area from section factor
// ============================================================================

double getAofS(const XSectParams& xs, double s) {
    double psi = s / xs.s_full;
    if (s <= 0.0) return 0.0;
    if (s > xs.s_max) s = xs.s_max;

    switch (static_cast<XSectShape>(xs.type)) {
        case XSectShape::DUMMY: return 0.0;
        case XSectShape::FORCE_MAIN:
        case XSectShape::CIRCULAR:     return circ_getAofS(xs, s);
        case XSectShape::EGGSHAPED:
            return xs.a_full * invLookup(psi, xsect_tables::S_Egg, xsect_tables::N_S_Egg);
        case XSectShape::HORSESHOE:
            return xs.a_full * invLookup(psi, xsect_tables::S_Horseshoe, xsect_tables::N_S_Horseshoe);
        case XSectShape::GOTHIC:
            return xs.a_full * invLookup(psi, xsect_tables::S_Gothic, xsect_tables::N_S_Gothic);
        case XSectShape::CATENARY:
            return xs.a_full * invLookup(psi, xsect_tables::S_Catenary, xsect_tables::N_S_Catenary);
        case XSectShape::SEMIELLIPTICAL:
            return xs.a_full * invLookup(psi, xsect_tables::S_SemiEllip, xsect_tables::N_S_SemiEllip);
        case XSectShape::BASKETHANDLE:
            return xs.a_full * invLookup(psi, xsect_tables::S_BasketHandle, xsect_tables::N_S_BasketHandle);
        case XSectShape::SEMICIRCULAR:
            return xs.a_full * invLookup(psi, xsect_tables::S_SemiCirc, xsect_tables::N_S_SemiCirc);
        default: {
            // Newton-Raphson on S(a) = s, bracketed in [a1, a2] (legacy generic_getAofS).
            // a2 = absolute area at max flow = aFull * Amax-ratio.
            double a1, a2;
            double a_max = xs.a_full * getAmax(xs);
            if ((s <= xs.s_max && s >= xs.s_full) && xs.s_max != xs.s_full) {
                a1 = xs.a_full;   // sFull < sMax: root lies between aFull and aMax
                a2 = a_max;
            } else {
                a1 = 0.0;
                a2 = a_max;
            }
            double a = 0.5 * (a1 + a2);
            double tol = 0.0001 * xs.a_full;
            findroot_Newton(a1, a2, &a, tol, [&](double aa, double* f, double* df) {
                *f = getSofA(xs, aa) - s;
                *df = getdSdA(xs, aa);
            });
            return a;
        }
    }
}

// ============================================================================
// getAmax — ratio of area at max flow to full area
// ============================================================================

double getAmax(const XSectParams& xs) {
    if (xs.type >= 0 && xs.type <= 25)
        return xsect_tables::Amax[xs.type];
    return 1.0;
}

// ============================================================================
// getYcrit — critical depth for a given flow rate (legacy xsect_getYcrit)
// ============================================================================

double getYcrit(const XSectParams& xs, double q) {
    if (q <= 0.0) return 0.0;
    double q2g = q * q / GRAVITY;
    if (q2g == 0.0) return 0.0;

    double y;
    switch (static_cast<XSectShape>(xs.type)) {
        case XSectShape::DUMMY: return 0.0;
        case XSectShape::RECT_OPEN:
        case XSectShape::RECT_CLOSED:
            y = std::pow(q2g / (xs.w_max * xs.w_max), 1.0 / 3.0);
            break;
        case XSectShape::TRIANGULAR:
            y = std::pow(2.0 * q2g / (xs.s_bot * xs.s_bot), 1.0 / 5.0);
            break;
        case XSectShape::PARABOLIC:
            y = std::pow(27.0 / 32.0 * q2g / (xs.r_bot * xs.r_bot), 1.0 / 4.0);
            break;
        case XSectShape::POWERFUNC:
            y = 1.0 / (2.0 * xs.s_bot + 3.0);
            y = std::pow(q2g * (xs.s_bot + 1.0) / (xs.r_bot * xs.r_bot), y);
            break;
        default: {
            // Critical flow function Q_c(yc) - qTarget (legacy getQcritical).
            auto qCritical = [&](double yc, double qTarget) -> double {
                double a = getAofY(xs, yc);
                double w = getWofY(xs, yc);
                if (w > 0.0) return a * std::sqrt(GRAVITY * a / w) - qTarget;
                return -qTarget;
            };

            // Initial estimate from equivalent circular conduit.
            double y0 = 1.01 * std::pow(q2g / xs.y_full, 0.25);
            if (y0 >= xs.y_full) y0 = 0.97 * xs.y_full;

            // Ratio of conduit area to equivalent circular area.
            double r = xs.a_full / (PI / 4.0 * xs.y_full * xs.y_full);

            if (r >= 0.5 && r <= 2.0) {
                // --- interval enumeration (legacy getYcritEnum), 25 increments
                constexpr int N_INC = 25;
                double dy = xs.y_full / N_INC;
                int i1 = static_cast<int>(y0 / dy);
                double q0 = qCritical(i1 * dy, 0.0);
                if (q0 < q) {
                    y = xs.y_full;
                    for (int i = i1 + 1; i <= N_INC; ++i) {
                        double qc = qCritical(i * dy, 0.0);
                        if (qc >= q) {
                            y = ((q - q0) / (qc - q0) + static_cast<double>(i - 1)) * dy;
                            break;
                        }
                        q0 = qc;
                    }
                } else {
                    y = 0.0;
                    for (int i = i1 - 1; i >= 0; --i) {
                        double qc = qCritical(i * dy, 0.0);
                        if (qc < q) {
                            y = ((q - qc) / (q0 - qc) + static_cast<double>(i)) * dy;
                            break;
                        }
                        q0 = qc;
                    }
                }
            } else {
                // --- Ridder's method (legacy getYcritRidder)
                double y1 = 0.0;
                double y2 = 0.99 * xs.y_full;
                double q2 = qCritical(y2, 0.0);
                if (q2 < q) { y = xs.y_full; break; }
                double q0 = qCritical(y0, 0.0);
                double q1 = qCritical(0.5 * xs.y_full, 0.0);
                if (q0 > q) {
                    y2 = y0;
                    if (q1 < q) y1 = 0.5 * xs.y_full;
                } else {
                    y1 = y0;
                    if (q1 > q) y2 = 0.5 * xs.y_full;
                }
                y = findroot_Ridder(y1, y2, 0.001,
                                    [&](double yc) { return qCritical(yc, q); });
            }
            break;
        }
    }
    return std::min(y, xs.y_full);
}

// ============================================================================
// isOpen — returns true for open shapes
// ============================================================================

bool isOpen(int type) {
    switch (static_cast<XSectShape>(type)) {
        case XSectShape::RECT_OPEN:
        case XSectShape::TRAPEZOIDAL:
        case XSectShape::TRIANGULAR:
        case XSectShape::PARABOLIC:
        case XSectShape::POWERFUNC:
        case XSectShape::IRREGULAR:
            return true;
        default:
            return false;
    }
}

// ============================================================================
// setParams — compute full-depth properties from raw geometry inputs.
//
// Faithful 1:1 port of legacy xsect_setParams. This is the SINGLE source of
// truth for cross-section full-flow geometry: PostParseResolver delegates to it
// for every self-contained shape. Returns 0 on success, -1 on invalid geometry.
//
// IRREGULAR / CUSTOM / STREET_XSECT derive their geometry from transect / curve
// / street tables (resolved by PostParseResolver) and are left untouched here.
// ============================================================================

int setParams(XSectParams& xs, int type, const double p[], double ucf) {
    xs.type = type;
    auto shape = static_cast<XSectShape>(type);

    switch (shape) {
        case XSectShape::CIRCULAR: {
            double d = p[0] / ucf;
            xs.y_full = d;
            xs.w_max  = d;
            xs.yw_max = d / 2.0;
            xs.a_full = PI / 4.0 * d * d;
            xs.r_full = d / 4.0;
            xs.s_full = xs.a_full * std::pow(xs.r_full, 2.0 / 3.0);
            xs.s_max  = 1.08 * xs.s_full;
            return 0;
        }
        case XSectShape::FORCE_MAIN: {
            double d = p[0] / ucf;
            xs.y_full = d;
            xs.w_max  = d;
            xs.yw_max = d / 2.0;
            xs.a_full = PI / 4.0 * d * d;
            xs.r_full = d / 4.0;
            xs.s_full = xs.a_full * std::pow(xs.r_full, 0.63);  // Hazen-Williams
            xs.s_max  = 1.06949 * xs.s_full;
            xs.r_bot  = p[1];   // C-factor / roughness stored in rBot
            return 0;
        }
        case XSectShape::RECT_CLOSED: {
            double y = p[0] / ucf;
            double w = p[1] / ucf;
            xs.y_full = y;
            xs.w_max  = w;
            xs.yw_max = y;
            xs.a_full = y * w;
            xs.r_full = xs.a_full / (2.0 * (y + w));
            xs.s_full = xs.a_full * std::pow(xs.r_full, 2.0 / 3.0);
            double a_max = RECT_ALFMAX * xs.a_full;
            xs.s_max = a_max * std::pow(rect_closed_getRofA(xs, a_max), 2.0 / 3.0);
            return 0;
        }
        case XSectShape::RECT_OPEN: {
            double y = p[0] / ucf;
            double w = p[1] / ucf;
            xs.y_full = y;
            xs.w_max  = w;
            xs.yw_max = y;
            xs.s_bot  = p[2];   // # of sides to ignore (0, 1, or 2)
            xs.a_full = y * w;
            xs.r_full = xs.a_full / ((2.0 - xs.s_bot) * y + w);
            xs.s_full = xs.a_full * std::pow(xs.r_full, 2.0 / 3.0);
            xs.s_max  = xs.s_full;
            return 0;
        }
        case XSectShape::TRAPEZOIDAL: {
            double y = p[0] / ucf;
            double yb = p[1] / ucf;         // bottom width
            double m1 = p[2], m2 = p[3];    // side slopes
            xs.y_full = y;
            xs.yw_max = y;
            xs.y_bot  = yb;
            xs.s_bot  = (m1 + m2) / 2.0;
            xs.r_bot  = std::sqrt(1.0 + m1 * m1) + std::sqrt(1.0 + m2 * m2);
            xs.w_max  = yb + y * (m1 + m2);
            xs.a_full = (yb + xs.s_bot * y) * y;
            xs.r_full = xs.a_full / (yb + y * xs.r_bot);
            xs.s_full = xs.a_full * std::pow(xs.r_full, 2.0 / 3.0);
            xs.s_max  = xs.s_full;
            return 0;
        }
        case XSectShape::TRIANGULAR: {
            double y = p[0] / ucf;
            double w = p[1] / ucf;
            xs.y_full = y;
            xs.w_max  = w;
            xs.yw_max = y;
            xs.s_bot  = w / y / 2.0;
            xs.r_bot  = std::sqrt(1.0 + xs.s_bot * xs.s_bot);
            xs.a_full = y * y * xs.s_bot;
            xs.r_full = xs.a_full / (2.0 * y * xs.r_bot);
            xs.s_full = xs.a_full * std::pow(xs.r_full, 2.0 / 3.0);
            xs.s_max  = xs.s_full;
            return 0;
        }
        case XSectShape::PARABOLIC: {
            double y = p[0] / ucf;
            double w = p[1] / ucf;
            xs.y_full = y;
            xs.w_max  = w;
            xs.yw_max = y;
            xs.r_bot  = w / 2.0 / std::sqrt(y);
            xs.a_full = (2.0 / 3.0) * y * w;
            xs.r_full = parab_getRofY(xs, y);
            xs.s_full = xs.a_full * std::pow(xs.r_full, 2.0 / 3.0);
            xs.s_max  = xs.s_full;
            return 0;
        }
        case XSectShape::POWERFUNC: {
            double y = p[0] / ucf;
            double w = p[1] / ucf;
            xs.y_full = y;
            xs.w_max  = w;
            xs.yw_max = y;
            xs.s_bot  = 1.0 / p[2];
            xs.r_bot  = w / (xs.s_bot + 1.0) / std::pow(y, xs.s_bot);
            xs.a_full = y * w / (xs.s_bot + 1.0);
            xs.r_full = powerfunc_getRofY(xs, y);
            xs.s_full = xs.a_full * std::pow(xs.r_full, 2.0 / 3.0);
            xs.s_max  = xs.s_full;
            return 0;
        }
        case XSectShape::FILLED_CIRCULAR: {
            if (p[1] >= p[0]) return -1;           // fill must be below crown
            xs.y_full = p[0] / ucf;
            xs.w_max  = xs.y_full;
            xs.a_full = PI / 4.0 * xs.y_full * xs.y_full;
            xs.r_full = 0.25 * xs.y_full;
            xs.y_bot  = p[1] / ucf;
            xs.a_bot  = circ_getAofY(xs, xs.y_bot);
            xs.s_bot  = getWofY(xs, xs.y_bot);     // type==FILLED here (legacy)
            xs.r_bot  = xs.a_bot / (xs.r_full *
                        lookup(xs.y_bot / xs.y_full, xsect_tables::R_Circ, xsect_tables::N_R_Circ));
            xs.a_full -= xs.a_bot;
            xs.r_full  = xs.a_full / (PI * xs.y_full - xs.r_bot + xs.s_bot);
            xs.s_full  = xs.a_full * std::pow(xs.r_full, 2.0 / 3.0);
            xs.s_max   = 1.08 * xs.s_full;
            xs.y_full -= xs.y_bot;
            xs.yw_max  = 0.5 * xs.y_full;
            return 0;
        }

        // --- Fixed-geometry tabulated shapes (full props from constants) ---
        case XSectShape::EGGSHAPED:
            xs.y_full = p[0] / ucf; xs.a_full = 0.5105 * xs.y_full * xs.y_full;
            xs.r_full = 0.1931 * xs.y_full; xs.w_max = 2.0/3.0 * xs.y_full;
            xs.yw_max = 0.64 * xs.y_full;
            xs.s_full = xs.a_full * std::pow(xs.r_full, 2.0/3.0); xs.s_max = 1.065 * xs.s_full;
            return 0;
        case XSectShape::HORSESHOE:
            xs.y_full = p[0] / ucf; xs.a_full = 0.8293 * xs.y_full * xs.y_full;
            xs.r_full = 0.2538 * xs.y_full; xs.w_max = 1.0 * xs.y_full;
            xs.yw_max = 0.5 * xs.y_full;
            xs.s_full = xs.a_full * std::pow(xs.r_full, 2.0/3.0); xs.s_max = 1.077 * xs.s_full;
            return 0;
        case XSectShape::GOTHIC:
            xs.y_full = p[0] / ucf; xs.a_full = 0.6554 * xs.y_full * xs.y_full;
            xs.r_full = 0.2269 * xs.y_full; xs.w_max = 0.84 * xs.y_full;
            xs.yw_max = 0.45 * xs.y_full;
            xs.s_full = xs.a_full * std::pow(xs.r_full, 2.0/3.0); xs.s_max = 1.065 * xs.s_full;
            return 0;
        case XSectShape::CATENARY:
            xs.y_full = p[0] / ucf; xs.a_full = 0.70277 * xs.y_full * xs.y_full;
            xs.r_full = 0.23172 * xs.y_full; xs.w_max = 0.9 * xs.y_full;
            xs.yw_max = 0.25 * xs.y_full;
            xs.s_full = xs.a_full * std::pow(xs.r_full, 2.0/3.0); xs.s_max = 1.05 * xs.s_full;
            return 0;
        case XSectShape::SEMIELLIPTICAL:
            xs.y_full = p[0] / ucf; xs.a_full = 0.785 * xs.y_full * xs.y_full;
            xs.r_full = 0.242 * xs.y_full; xs.w_max = 1.0 * xs.y_full;
            xs.yw_max = 0.15 * xs.y_full;
            xs.s_full = xs.a_full * std::pow(xs.r_full, 2.0/3.0); xs.s_max = 1.045 * xs.s_full;
            return 0;
        case XSectShape::BASKETHANDLE:
            xs.y_full = p[0] / ucf; xs.a_full = 0.7862 * xs.y_full * xs.y_full;
            xs.r_full = 0.2464 * xs.y_full; xs.w_max = 0.944 * xs.y_full;
            xs.yw_max = 0.2 * xs.y_full;
            xs.s_full = xs.a_full * std::pow(xs.r_full, 2.0/3.0); xs.s_max = 1.06078 * xs.s_full;
            return 0;
        case XSectShape::SEMICIRCULAR:
            xs.y_full = p[0] / ucf; xs.a_full = 1.2697 * xs.y_full * xs.y_full;
            xs.r_full = 0.2946 * xs.y_full; xs.w_max = 1.64 * xs.y_full;
            xs.yw_max = 0.15 * xs.y_full;
            xs.s_full = xs.a_full * std::pow(xs.r_full, 2.0/3.0); xs.s_max = 1.06637 * xs.s_full;
            return 0;

        case XSectShape::RECT_TRIANG: {
            if (p[1] <= 0.0 || p[2] <= 0.0) return -1;
            xs.y_full = p[0] / ucf;
            xs.w_max  = p[1] / ucf;
            xs.y_bot  = p[2] / ucf;          // triangle height
            xs.yw_max = xs.y_full;
            xs.a_bot  = xs.y_bot * xs.w_max / 2.0;
            xs.s_bot  = xs.w_max / xs.y_bot / 2.0;
            xs.r_bot  = std::sqrt(1.0 + xs.s_bot * xs.s_bot);
            xs.a_full = xs.w_max * (xs.y_full - xs.y_bot / 2.0);
            xs.r_full = xs.a_full / (2.0 * xs.y_bot * xs.r_bot +
                        2.0 * (xs.y_full - xs.y_bot) + xs.w_max);
            xs.s_full = xs.a_full * std::pow(xs.r_full, 2.0 / 3.0);
            double a_max = RECT_TRIANG_ALFMAX * xs.a_full;
            xs.s_max = a_max * std::pow(rect_triang_getRofA(xs, a_max), 2.0 / 3.0);
            return 0;
        }
        case XSectShape::RECT_ROUND: {
            if (p[1] <= 0.0) return -1;
            xs.y_full = p[0] / ucf;
            xs.w_max  = p[1] / ucf;
            xs.r_bot  = std::max(p[2], p[1] / 2.0) / ucf;    // invert radius
            double theta = 2.0 * std::asin(xs.w_max / 2.0 / xs.r_bot);
            xs.a_bot  = xs.r_bot * xs.r_bot / 2.0 * (theta - std::sin(theta));
            xs.s_bot  = PI * xs.r_bot * xs.r_bot * std::pow(xs.r_bot / 2.0, 2.0 / 3.0);
            xs.y_bot  = xs.r_bot * (1.0 - std::cos(theta / 2.0));
            if (xs.y_bot > xs.y_full) return -1;
            xs.yw_max = xs.y_full;
            xs.a_full = xs.w_max * (xs.y_full - xs.y_bot) + xs.a_bot;
            xs.r_full = xs.a_full / (xs.r_bot * theta +
                        2.0 * (xs.y_full - xs.y_bot) + xs.w_max);
            xs.s_full = xs.a_full * std::pow(xs.r_full, 2.0 / 3.0);
            double a_max = RECT_ROUND_ALFMAX * xs.a_full;
            xs.s_max = a_max * std::pow(rect_round_getRofA(xs, a_max), 2.0 / 3.0);
            return 0;
        }
        case XSectShape::MOD_BASKET: {
            if (p[1] <= 0.0) return -1;
            xs.y_full = p[0] / ucf;
            xs.w_max  = p[1] / ucf;
            xs.r_bot  = std::max(p[2], p[1] / 2.0) / ucf;    // arc radius
            double theta = 2.0 * std::asin(xs.w_max / 2.0 / xs.r_bot);
            xs.s_bot  = theta;
            xs.y_bot  = xs.r_bot * (1.0 - std::cos(theta / 2.0));
            if (xs.y_bot > xs.y_full) return -1;
            xs.yw_max = xs.y_full - xs.y_bot;
            xs.a_bot  = xs.r_bot * xs.r_bot / 2.0 * (theta - std::sin(theta));
            xs.a_full = (xs.y_full - xs.y_bot) * xs.w_max + xs.a_bot;
            xs.r_full = xs.a_full / (xs.r_bot * theta +
                        2.0 * (xs.y_full - xs.y_bot) + xs.w_max);
            xs.s_full = xs.a_full * std::pow(xs.r_full, 2.0 / 3.0);
            xs.s_max  = getSofA(xs, getAmax(xs) * xs.a_full);
            return 0;
        }

        case XSectShape::HORIZ_ELLIPSE: {
            double code = p[2];
            if (p[1] == 0.0) code = p[0];
            if (code > 0.0) {                        // standard ellipse pipe
                int idx = static_cast<int>(std::floor(code)) - 1;
                if (idx < 0 || idx >= xsect_tables::NumCodesEllipse) return -1;
                xs.y_full = xsect_tables::MinorAxis_Ellipse[idx] / 12.0;
                xs.w_max  = xsect_tables::MajorAxis_Ellipse[idx] / 12.0;
                xs.a_full = xsect_tables::Afull_Ellipse[idx];
                xs.r_full = xsect_tables::Rfull_Ellipse[idx];
            } else {
                if (p[1] < 0.0) return -1;
                xs.y_full = p[0] / ucf;
                xs.w_max  = p[1] / ucf;
                xs.a_full = 1.2692 * xs.y_full * xs.y_full;
                xs.r_full = 0.3061 * xs.y_full;
            }
            xs.s_full = xs.a_full * std::pow(xs.r_full, 2.0 / 3.0);
            xs.s_max  = xs.s_full;
            xs.yw_max = 0.48 * xs.y_full;
            return 0;
        }
        case XSectShape::VERT_ELLIPSE: {
            double code = p[2];
            if (p[1] == 0.0) code = p[0];
            if (code > 0.0) {                        // standard ellipse pipe
                int idx = static_cast<int>(std::floor(code)) - 1;
                if (idx < 0 || idx >= xsect_tables::NumCodesEllipse) return -1;
                xs.y_full = xsect_tables::MajorAxis_Ellipse[idx] / 12.0;
                xs.w_max  = xsect_tables::MinorAxis_Ellipse[idx] / 12.0;
                xs.a_full = xsect_tables::Afull_Ellipse[idx];
                xs.r_full = xsect_tables::Rfull_Ellipse[idx];
            } else {
                if (p[1] < 0.0) return -1;
                xs.y_full = p[0] / ucf;
                xs.w_max  = p[1] / ucf;
                xs.a_full = 1.2692 * xs.w_max * xs.w_max;
                xs.r_full = 0.3061 * xs.w_max;
            }
            xs.s_full = xs.a_full * std::pow(xs.r_full, 2.0 / 3.0);
            xs.s_max  = xs.s_full;
            xs.yw_max = 0.48 * xs.y_full;
            return 0;
        }
        case XSectShape::ARCH: {
            double code = p[2];
            if (p[1] == 0.0) code = p[0];
            if (code > 0.0) {                        // standard arch pipe
                int idx = static_cast<int>(std::floor(code)) - 1;
                if (idx < 0 || idx >= xsect_tables::NumCodesArch) return -1;
                xs.y_full = xsect_tables::Yfull_Arch[idx] / 12.0;  // inches
                xs.w_max  = xsect_tables::Wmax_Arch[idx] / 12.0;
                xs.a_full = xsect_tables::Afull_Arch[idx];
                xs.r_full = xsect_tables::Rfull_Arch[idx];
            } else {
                if (p[1] < 0.0) return -1;
                xs.y_full = p[0] / ucf;
                xs.w_max  = p[1] / ucf;
                xs.a_full = 0.7879 * xs.y_full * xs.w_max;
                xs.r_full = 0.2991 * xs.y_full;
            }
            xs.s_full = xs.a_full * std::pow(xs.r_full, 2.0 / 3.0);
            xs.s_max  = xs.s_full;
            xs.yw_max = 0.28 * xs.y_full;
            return 0;
        }

        default:
            // IRREGULAR / CUSTOM / STREET_XSECT / DUMMY — geometry comes from
            // transect/curve/street tables resolved externally; leave fields.
            break;
    }
    return 0;
}

} // namespace xsect
} // namespace openswmm
