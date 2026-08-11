/**
 * @file XSectKernels.hpp
 * @brief Portable cross-section geometry kernels — one implementation, three
 *        consumers (host dynamic wave, host finite volume, device finite volume).
 *
 * @details The cross-section machinery was host-only: the geometry tables are
 *          namespace-scope arrays in xsect_tables.hpp, and a device kernel
 *          cannot reach a host global. This header removes that barrier without
 *          duplicating a single formula — the bodies here were MOVED out of
 *          XSection.cpp verbatim, not transcribed, and XSection.cpp's public
 *          functions are now one-line forwarders onto them.
 *
 *          The only change to the bodies is where the tables come from. Instead
 *          of naming `xsect_tables::A_Circ` directly they read
 *          `tbl.A_Circ` — a pointer carried in an XsectTables struct. On the
 *          host that struct binds straight to the same namespace arrays, so the
 *          arithmetic, the operand order and the literals are unchanged and the
 *          result is bit-identical by construction rather than by testing. On a
 *          device it binds to the device copies of those arrays.
 *
 *          **Bit-exactness contract carried over from XSectLookup.hpp:** IEEE
 *          division rather than a precomputed reciprocal, legacy's exact
 *          operation grouping, and no FMA contraction. Device compilers default
 *          to aggressive contraction, so a device build MUST pin
 *          `-ffp-contract=off` (or the backend's equivalent) or the tables will
 *          diverge in the last ulp.
 *
 *          **Transect-backed shapes** (IRREGULAR, CUSTOM, STREET_XSECT) read
 *          their tables through pointers held in XSectParams itself. A device
 *          consumer must rebind those to device memory before use; the shared
 *          tables here are the ones that are the same for every model.
 *
 * @see plans/EXPLICIT_FV_KOKKOS_1D_SOLVER_PLAN.md §5.1
 * @note INTERNAL HEADER — not installed.
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_XSECT_KERNELS_HPP
#define OPENSWMM_XSECT_KERNELS_HPP

#include <algorithm>
#include <cmath>

#include "XSectLookup.hpp"
#include "../data/LinkData.hpp"

// Portable kernel-function marker — same convention as FvKernels.hpp and
// 2d/solver/InertialKernels.hpp. Host builds get plain `inline`; a device
// consumer defines this to KOKKOS_INLINE_FUNCTION *before* including.
#ifndef OPENSWMM_KERNEL_FN
#define OPENSWMM_KERNEL_FN inline
#endif

namespace openswmm::xsect {

// Constants matching legacy consts.h / xsect.c exactly (for bit-faithful
// parity). They moved here with the bodies that use them; setParams() in
// XSection.cpp reads the same definitions rather than a second copy.
inline constexpr double TINY    = 1.0e-6;
inline constexpr double PI      = 3.141592654;
inline constexpr double GRAVITY = 32.2;

inline constexpr double RECT_ALFMAX        = 0.97;
inline constexpr double RECT_TRIANG_ALFMAX = 0.98;
inline constexpr double RECT_ROUND_ALFMAX  = 0.98;

/**
 * @brief The analytic shape formulas — the single definition of each.
 *
 * These are the shapes whose geometry is a closed-form expression rather than a
 * table lookup. They live outside XsectEval because they need no tables at all,
 * which lets the two consumers that are shaped differently share them:
 *
 *   - `XsectEval`'s per-element methods (host DW scalar fallbacks, FV, device),
 *     which unpack an XSectParams; and
 *   - `xsect_batch`'s SoA loops (XSectBatch.cpp), which DWSolver's
 *     computeLinkGeometry STEP B/D runs on and which read parallel arrays.
 *
 * Both call the same function on the same operands, so the two paths cannot
 * drift — which is the point (plan §5.1, Phase 4b). Each body is legacy
 * `xsect.c` verbatim, including operand order and the guards: multiplication is
 * not associative in IEEE-754, so `y*y*sBot` and `sBot*y*y` are different
 * computations and only the first is legacy's.
 *
 * The `y`/`a` guards are legacy's own (`xsect_getRofA`'s `a <= 0` early return
 * for the rect family, `trapez_getRofY`'s `y == 0`). STEP A floors every conduit
 * depth at FUDGE before STEP B/D sees it, so on the DW path they are unreachable
 * either way; they are here so the shared body is complete rather than
 * context-dependent.
 */
namespace shape {

// ---- Area from depth ----

OPENSWMM_KERNEL_FN double rectAofY(double y, double w_max) {
    return y * w_max;
}

OPENSWMM_KERNEL_FN double trapezAofY(double y, double y_bot, double s_bot) {
    return (y_bot + s_bot * y) * y;
}

OPENSWMM_KERNEL_FN double triangAofY(double y, double s_bot) {
    return y * y * s_bot;
}

OPENSWMM_KERNEL_FN double parabAofY(double y, double r_bot) {
    return (4.0 / 3.0) * r_bot * y * std::sqrt(y);
}

OPENSWMM_KERNEL_FN double powerfuncAofY(double y, double s_bot, double r_bot) {
    return r_bot * std::pow(y, s_bot + 1.0);
}

// ---- Hydraulic radius ----

/// Legacy rect_closed_getRofA — including the near-full correction that grows
/// the wetted perimeter by the crown width past RECT_ALFMAX.
OPENSWMM_KERNEL_FN double rectClosedRofA(double a, double w_max, double a_full) {
    if (a <= 0.0) return 0.0;
    double p = w_max + 2.0 * a / w_max;
    if (a / a_full > RECT_ALFMAX)
        p += (a / a_full - RECT_ALFMAX) / (1.0 - RECT_ALFMAX) * w_max;
    return a / p;
}

/// Legacy xsect_getRofA's RECT_OPEN arm; s_bot is the count of banks excluded
/// from the perimeter (0, 1 or 2).
OPENSWMM_KERNEL_FN double rectOpenRofA(double a, double w_max, double s_bot) {
    if (a <= 0.0) return 0.0;
    return a / (w_max + (2.0 - s_bot) * a / w_max);
}

OPENSWMM_KERNEL_FN double trapezRofY(double y, double y_bot, double s_bot,
                                     double r_bot) {
    if (y == 0.0) return 0.0;
    return trapezAofY(y, y_bot, s_bot) / (y_bot + y * r_bot);
}

OPENSWMM_KERNEL_FN double triangRofY(double y, double s_bot, double r_bot) {
    return (y * s_bot) / (2.0 * r_bot);
}

// ---- Top width ----

/// Legacy getWofY RECT_CLOSED: the tabulated crown width is 0 exactly at
/// y == y_full, so the caller passes the normalized depth, not the depth.
OPENSWMM_KERNEL_FN double rectClosedWofY(double y_norm, double w_max) {
    if (y_norm == 1.0) return 0.0;
    return w_max;
}

OPENSWMM_KERNEL_FN double trapezWofY(double y, double y_bot, double s_bot) {
    return y_bot + 2.0 * y * s_bot;
}

OPENSWMM_KERNEL_FN double triangWofY(double y, double s_bot) {
    return 2.0 * s_bot * y;
}

OPENSWMM_KERNEL_FN double parabWofY(double y, double r_bot) {
    return 2.0 * r_bot * std::sqrt(y);
}

OPENSWMM_KERNEL_FN double powerfuncWofY(double y, double s_bot, double r_bot) {
    return (s_bot + 1.0) * r_bot * std::pow(y, s_bot);
}

} // namespace shape

/**
 * @brief Pointers to the shared geometry tables, in whichever memory space the
 *        consumer runs in.
 *
 * Plain POD: trivially copyable into a device kernel by value. The member names
 * are deliberately identical to the `xsect_tables::` symbols they bind to, so
 * the moved bodies differ from the originals by exactly one token.
 */
struct XsectTables {
    const double* A_Arch = nullptr;
    const double* A_Baskethandle = nullptr;
    const double* A_Circ = nullptr;
    const double* A_Egg = nullptr;
    const double* A_HorizEllipse = nullptr;
    const double* A_Horseshoe = nullptr;
    const double* A_VertEllipse = nullptr;
    const double* R_Arch = nullptr;
    const double* R_Baskethandle = nullptr;
    const double* R_Circ = nullptr;
    const double* R_Egg = nullptr;
    const double* R_HorizEllipse = nullptr;
    const double* R_Horseshoe = nullptr;
    const double* R_VertEllipse = nullptr;
    const double* S_BasketHandle = nullptr;
    const double* S_Catenary = nullptr;
    const double* S_Circ = nullptr;
    const double* S_Egg = nullptr;
    const double* S_Gothic = nullptr;
    const double* S_Horseshoe = nullptr;
    const double* S_SemiCirc = nullptr;
    const double* S_SemiEllip = nullptr;
    const double* W_Arch = nullptr;
    const double* W_BasketHandle = nullptr;
    const double* W_Catenary = nullptr;
    const double* W_Circ = nullptr;
    const double* W_Egg = nullptr;
    const double* W_Gothic = nullptr;
    const double* W_HorizEllipse = nullptr;
    const double* W_Horseshoe = nullptr;
    const double* W_SemiCirc = nullptr;
    const double* W_SemiEllip = nullptr;
    const double* W_VertEllipse = nullptr;
    const double* Y_BasketHandle = nullptr;
    const double* Y_Catenary = nullptr;
    const double* Y_Circ = nullptr;
    const double* Y_Egg = nullptr;
    const double* Y_Gothic = nullptr;
    const double* Y_Horseshoe = nullptr;
    const double* Y_SemiCirc = nullptr;
    const double* Y_SemiEllip = nullptr;
    const double* Amax = nullptr;
    int N_A_Arch = 0;
    int N_A_Baskethandle = 0;
    int N_A_Circ = 0;
    int N_A_Egg = 0;
    int N_A_HorizEllipse = 0;
    int N_A_Horseshoe = 0;
    int N_A_VertEllipse = 0;
    int N_R_Arch = 0;
    int N_R_Baskethandle = 0;
    int N_R_Circ = 0;
    int N_R_Egg = 0;
    int N_R_HorizEllipse = 0;
    int N_R_Horseshoe = 0;
    int N_R_VertEllipse = 0;
    int N_S_BasketHandle = 0;
    int N_S_Catenary = 0;
    int N_S_Circ = 0;
    int N_S_Egg = 0;
    int N_S_Gothic = 0;
    int N_S_Horseshoe = 0;
    int N_S_SemiCirc = 0;
    int N_S_SemiEllip = 0;
    int N_W_Arch = 0;
    int N_W_BasketHandle = 0;
    int N_W_Catenary = 0;
    int N_W_Circ = 0;
    int N_W_Egg = 0;
    int N_W_Gothic = 0;
    int N_W_HorizEllipse = 0;
    int N_W_Horseshoe = 0;
    int N_W_SemiCirc = 0;
    int N_W_SemiEllip = 0;
    int N_W_VertEllipse = 0;
    int N_Y_BasketHandle = 0;
    int N_Y_Catenary = 0;
    int N_Y_Circ = 0;
    int N_Y_Egg = 0;
    int N_Y_Gothic = 0;
    int N_Y_Horseshoe = 0;
    int N_Y_SemiCirc = 0;
    int N_Y_SemiEllip = 0;
};

/**
 * @brief The geometry layer itself, parameterized on where its tables live.
 *
 * Every function is a non-static member so it can reach `tbl` without threading
 * an extra argument through ~70 call sites — which is what keeps the moved
 * bodies textually identical to the originals.
 */
struct XsectEval {
    XsectTables tbl;

    // ============================================================================
    // Root finders — faithful ports of legacy findroot.c (Numerical Recipes).
    // ============================================================================

    // Newton-Raphson + bisection. func(x, &f, &df). Root bracketed in [x1, x2].
    template <class Func>
    OPENSWMM_KERNEL_FN int findroot_Newton(double x1, double x2, double* rts, double xacc, Func func) const {
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
    OPENSWMM_KERNEL_FN double findroot_Ridder(double x1, double x2, double xacc, Func func) const {
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

    OPENSWMM_KERNEL_FN int locate(double y, const double* table, int jLast) const {
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

    OPENSWMM_KERNEL_FN double lookup(double x, const double* table, int n_items) const {
        // The tables are defined on the normalized domain x in [0, 1]. Guard the
        // bounds BEFORE converting x to an int index: x can arrive non-finite when
        // a caller normalizes by a zero full-depth (e.g. a conduit finalized
        // before its cross-section geometry is set, so y/y_full == inf).
        // static_cast<int> of inf/NaN is undefined behavior — on x86 (int)inf is
        // INT_MIN, which slips past the upper-bound check below and indexes the
        // table wildly out of bounds (segfault); ARM saturates to INT_MAX and
        // happened to mask the bug. The legacy lookup() guarded x <= 0; restore
        // that and add the matching upper bound so non-finite x is handled safely.
        //
        // On the finite domain (0, 1] this is bit-identical to legacy lookup():
        //   - x <= 0 / NaN  -> table[0]  (legacy lookup(0) also returns table[0]).
        //   - x >  1 / +inf -> table[n-1] (legacy takes the i >= n-1 early-out).
        //     The bound is strict (`> 1.0`, not `>= 1.0`): at x == 1.0 legacy does
        //     NOT early-out — 1.0/delta rounds just below n-1 — so it interpolates
        //     the last segment. Deferring x == 1.0 to lookup_exact reproduces that;
        //     no representable double lies in (1.0, n-1 * delta), so the strict
        //     guard is exact.
        if (!(x > 0.0)) return table[0];             // x <= 0 or NaN
        if (x > 1.0)    return table[n_items - 1];    // x > 1 or +inf
        return lookup_exact(x, table, n_items);
    }

    OPENSWMM_KERNEL_FN double invLookup(double y, const double* table, int n_items) const {
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

    OPENSWMM_KERNEL_FN double getThetaOfAlpha(double alpha) const {
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

    OPENSWMM_KERNEL_FN double getThetaOfPsi(double psi) const {
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

    OPENSWMM_KERNEL_FN double getYcircular(double alpha) const {
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

    OPENSWMM_KERNEL_FN double getScircular(double alpha) const {
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

    OPENSWMM_KERNEL_FN double getAcircular(double psi) const {
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

    OPENSWMM_KERNEL_FN double generic_getdSdA(const XSectParams& xs, double a) const {
        double alpha = a / xs.a_full;
        double alpha1 = alpha - 0.001;
        double alpha2 = alpha + 0.001;
        if (alpha1 < 0.0) alpha1 = 0.0;
        double a1 = alpha1 * xs.a_full;
        double a2 = alpha2 * xs.a_full;
        return (getSofA(xs, a2) - getSofA(xs, a1)) / (a2 - a1);
    }

    OPENSWMM_KERNEL_FN double tabular_getdSdA(const XSectParams& xs, double a,
                                  const double* table, int n_items) const {
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

    OPENSWMM_KERNEL_FN double circ_getAofY(const XSectParams& xs, double y) const {
        double y_norm = y / xs.y_full;
        return xs.a_full * lookup(y_norm, tbl.A_Circ, tbl.N_A_Circ);
    }

    OPENSWMM_KERNEL_FN double circ_getYofA(const XSectParams& xs, double a) const {
        double alpha = a / xs.a_full;
        if (alpha < 0.04) return xs.y_full * getYcircular(alpha);
        return xs.y_full * lookup(alpha, tbl.Y_Circ, tbl.N_Y_Circ);
    }

    OPENSWMM_KERNEL_FN double circ_getSofA(const XSectParams& xs, double a) const {
        double alpha = a / xs.a_full;
        if (alpha < 0.04) return xs.s_full * getScircular(alpha);
        return xs.s_full * lookup(alpha, tbl.S_Circ, tbl.N_S_Circ);
    }

    OPENSWMM_KERNEL_FN double circ_getAofS(const XSectParams& xs, double s) const {
        double psi = s / xs.s_full;
        if (psi == 0.0) return 0.0;
        if (psi >= 1.0) return xs.a_full;
        if (psi <= 0.015) return xs.a_full * getAcircular(psi);
        return xs.a_full * invLookup(psi, tbl.S_Circ, tbl.N_S_Circ);
    }

    OPENSWMM_KERNEL_FN double circ_getdSdA(const XSectParams& xs, double a) const {
        double alpha = a / xs.a_full;
        if (alpha <= 1.0e-30) return 1.0e-30;
        if (alpha < 0.04) {
            double theta = getThetaOfAlpha(alpha);
            double p = theta * xs.y_full / 2.0;
            double r = a / p;
            double dPdA = 4.0 / xs.y_full / (1.0 - std::cos(theta));
            return (5.0 / 3.0 - (2.0 / 3.0) * dPdA * r) * std::pow(r, 2.0 / 3.0);
        }
        return tabular_getdSdA(xs, a, tbl.S_Circ, tbl.N_S_Circ);
    }

    // ============================================================================
    // Filled circular (yBot/aBot/sBot/rBot = filled bottom: depth/area/width/perim)
    // ============================================================================

    OPENSWMM_KERNEL_FN double filled_circ_getAofY(const XSectParams& xs, double y) const {
        // Temporarily restore the unfilled full circle (legacy expands yFull/aFull).
        XSectParams t = xs;
        t.y_full += xs.y_bot;
        t.a_full += xs.a_bot;
        double a = circ_getAofY(t, y + xs.y_bot);
        return a - xs.a_bot;
    }

    OPENSWMM_KERNEL_FN double filled_circ_getYofA(const XSectParams& xs, double a) const {
        XSectParams t = xs;
        t.y_full += xs.y_bot;
        t.a_full += xs.a_bot;
        double y = circ_getYofA(t, a + xs.a_bot);
        return y - xs.y_bot;
    }

    OPENSWMM_KERNEL_FN double filled_circ_getRofY(const XSectParams& xs, double y) const {
        XSectParams t = xs;
        t.y_full += xs.y_bot;
        t.a_full += xs.a_bot;
        double yy = y + xs.y_bot;
        double a = circ_getAofY(t, yy);
        double r = 0.25 * t.y_full *
                   lookup(yy / t.y_full, tbl.R_Circ, tbl.N_R_Circ);
        double p = (a / r);
        a = a - xs.a_bot;
        p = p - xs.r_bot + xs.s_bot;   // rBot = filled perimeter, sBot = filled width
        return a / p;
    }

    // ============================================================================
    // Rectangular closed / open
    // ============================================================================

    OPENSWMM_KERNEL_FN double rect_closed_getRofA(const XSectParams& xs, double a) const {
        return shape::rectClosedRofA(a, xs.w_max, xs.a_full);
    }

    OPENSWMM_KERNEL_FN double rect_closed_getSofA(const XSectParams& xs, double a) const {
        if (a / xs.a_full > RECT_ALFMAX)
            return xs.s_max + (xs.s_full - xs.s_max) *
                   (a / xs.a_full - RECT_ALFMAX) / (1.0 - RECT_ALFMAX);
        return a * std::pow(rect_closed_getRofA(xs, a), 2.0 / 3.0);
    }

    OPENSWMM_KERNEL_FN double rect_closed_getdSdA(const XSectParams& xs, double a) const {
        double alpha = a / xs.a_full;
        if (alpha > RECT_ALFMAX)
            return (xs.s_full - xs.s_max) / ((1.0 - RECT_ALFMAX) * xs.a_full);
        if (alpha <= 1.0e-30) return generic_getdSdA(xs, a);
        double r = rect_closed_getRofA(xs, a);
        return (5.0 / 3.0 - (2.0 / 3.0) * (2.0 / xs.w_max) * r) * std::pow(r, 2.0 / 3.0);
    }

    OPENSWMM_KERNEL_FN double rect_open_getSofA(const XSectParams& xs, double a) const {
        double y = a / xs.w_max;
        double r = a / ((2.0 - xs.s_bot) * y + xs.w_max);
        return a * std::pow(r, 2.0 / 3.0);
    }

    OPENSWMM_KERNEL_FN double rect_open_getdSdA(const XSectParams& xs, double a) const {
        if (a / xs.a_full <= 1.0e-30) return generic_getdSdA(xs, a);
        double r = getRofA(xs, a);
        double dPdA = (2.0 - xs.s_bot) / xs.w_max;
        return (5.0 / 3.0 - (2.0 / 3.0) * dPdA * r) * std::pow(r, 2.0 / 3.0);
    }

    // ============================================================================
    // Rect-triangular (triangular bottom + rectangular top)
    // ============================================================================

    OPENSWMM_KERNEL_FN double rect_triang_getAofY(const XSectParams& xs, double y) const {
        if (y <= xs.y_bot) return y * y * xs.s_bot;
        return xs.a_bot + (y - xs.y_bot) * xs.w_max;
    }

    OPENSWMM_KERNEL_FN double rect_triang_getWofY(const XSectParams& xs, double y) const {
        if (y <= xs.y_bot) return 2.0 * xs.s_bot * y;
        return xs.w_max;
    }

    OPENSWMM_KERNEL_FN double rect_triang_getYofA(const XSectParams& xs, double a) const {
        if (a <= xs.a_bot) return std::sqrt(a / xs.s_bot);
        return xs.y_bot + (a - xs.a_bot) / xs.w_max;
    }

    OPENSWMM_KERNEL_FN double rect_triang_getRofA(const XSectParams& xs, double a) const {
        if (a <= 0.0) return 0.0;
        double y = rect_triang_getYofA(xs, a);
        if (y <= xs.y_bot) return a / (2.0 * y * xs.r_bot);
        double p = 2.0 * xs.y_bot * xs.r_bot + 2.0 * (y - xs.y_bot);
        double alf = (a / xs.a_full) - RECT_TRIANG_ALFMAX;
        if (alf > 0.0) p += alf / (1.0 - RECT_TRIANG_ALFMAX) * xs.w_max;
        return a / p;
    }

    OPENSWMM_KERNEL_FN double rect_triang_getRofY(const XSectParams& xs, double y) const {
        if (y <= xs.y_bot) return y * xs.s_bot / (2.0 * xs.r_bot);
        double a = xs.a_bot + (y - xs.y_bot) * xs.w_max;
        double p = 2.0 * xs.y_bot * xs.r_bot + 2.0 * (y - xs.y_bot);
        double alf = (a / xs.a_full) - RECT_TRIANG_ALFMAX;
        if (alf > 0.0) p += alf / (1.0 - RECT_TRIANG_ALFMAX) * xs.w_max;
        return a / p;
    }

    OPENSWMM_KERNEL_FN double rect_triang_getSofA(const XSectParams& xs, double a) const {
        double alfMax = RECT_TRIANG_ALFMAX;
        if (a / xs.a_full > alfMax)
            return xs.s_max + (xs.s_full - xs.s_max) *
                   (a / xs.a_full - alfMax) / (1.0 - alfMax);
        return a * std::pow(rect_triang_getRofA(xs, a), 2.0 / 3.0);
    }

    OPENSWMM_KERNEL_FN double rect_triang_getdSdA(const XSectParams& xs, double a) const {
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

    OPENSWMM_KERNEL_FN double rect_round_getYofA(const XSectParams& xs, double a) const {
        if (a > xs.a_bot) return xs.y_bot + (a - xs.a_bot) / xs.w_max;
        double alpha = a / (PI * xs.r_bot * xs.r_bot);
        if (alpha < 0.04) return (2.0 * xs.r_bot) * getYcircular(alpha);
        return (2.0 * xs.r_bot) * lookup(alpha, tbl.Y_Circ, tbl.N_Y_Circ);
    }

    OPENSWMM_KERNEL_FN double rect_round_getAofY(const XSectParams& xs, double y) const {
        if (y > xs.y_bot) return xs.a_bot + (y - xs.y_bot) * xs.w_max;
        double theta1 = 2.0 * std::acos(1.0 - y / xs.r_bot);
        return 0.5 * xs.r_bot * xs.r_bot * (theta1 - std::sin(theta1));
    }

    OPENSWMM_KERNEL_FN double rect_round_getWofY(const XSectParams& xs, double y) const {
        if (y > xs.y_bot) return xs.w_max;
        return 2.0 * std::sqrt(y * (2.0 * xs.r_bot - y));
    }

    OPENSWMM_KERNEL_FN double rect_round_getRofA(const XSectParams& xs, double a) const {
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

    OPENSWMM_KERNEL_FN double rect_round_getRofY(const XSectParams& xs, double y) const {
        if (y <= 0.0) return 0.0;
        if (y > xs.y_bot) return rect_round_getRofA(xs, rect_round_getAofY(xs, y));
        double theta1 = 2.0 * std::acos(1.0 - y / xs.r_bot);
        return 0.5 * xs.r_bot * (1.0 - std::sin(theta1)) / theta1;
    }

    OPENSWMM_KERNEL_FN double rect_round_getSofA(const XSectParams& xs, double a) const {
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
        return sFull * lookup(alpha, tbl.S_Circ, tbl.N_S_Circ);
    }

    OPENSWMM_KERNEL_FN double rect_round_getdSdA(const XSectParams& xs, double a) const {
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

    OPENSWMM_KERNEL_FN double mod_basket_getYofA(const XSectParams& xs, double a) const {
        if (a <= xs.a_full - xs.a_bot) return a / xs.w_max;
        double alpha = (xs.a_full - a) / (PI * xs.r_bot * xs.r_bot);
        double y1;
        if (alpha < 0.04) y1 = getYcircular(alpha);
        else              y1 = lookup(alpha, tbl.Y_Circ, tbl.N_Y_Circ);
        y1 = 2.0 * xs.r_bot * y1;
        return xs.y_full - y1;
    }

    OPENSWMM_KERNEL_FN double mod_basket_getAofY(const XSectParams& xs, double y) const {
        if (y <= xs.y_full - xs.y_bot) return y * xs.w_max;
        double y1 = xs.y_full - y;
        double theta1 = 2.0 * std::acos(1.0 - y1 / xs.r_bot);
        double a1 = 0.5 * xs.r_bot * xs.r_bot * (theta1 - std::sin(theta1));
        return xs.a_full - a1;
    }

    OPENSWMM_KERNEL_FN double mod_basket_getWofY(const XSectParams& xs, double y) const {
        if (y <= 0.0) return 0.0;
        if (y <= xs.y_full - xs.y_bot) return xs.w_max;
        double y1 = xs.y_full - y;
        return 2.0 * std::sqrt(y1 * (2.0 * xs.r_bot - y1));
    }

    OPENSWMM_KERNEL_FN double mod_basket_getRofA(const XSectParams& xs, double a) const {
        if (a <= xs.a_full - xs.a_bot)
            return a / (xs.w_max + 2.0 * a / xs.w_max);
        double y1 = xs.y_full - mod_basket_getYofA(xs, a);
        double theta1 = 2.0 * std::acos(1.0 - y1 / xs.r_bot);
        double p = (xs.s_bot - theta1) * xs.r_bot;   // sBot = full circular opening angle
        y1 = xs.y_full - xs.y_bot;
        p = p + 2.0 * y1 + xs.w_max;
        return a / p;
    }

    OPENSWMM_KERNEL_FN double mod_basket_getdSdA(const XSectParams& xs, double a) const {
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

    OPENSWMM_KERNEL_FN double trapez_getAofY(const XSectParams& xs, double y) const {
        return shape::trapezAofY(y, xs.y_bot, xs.s_bot);
    }

    OPENSWMM_KERNEL_FN double trapez_getWofY(const XSectParams& xs, double y) const {
        return shape::trapezWofY(y, xs.y_bot, xs.s_bot);
    }

    OPENSWMM_KERNEL_FN double trapez_getYofA(const XSectParams& xs, double a) const {
        if (xs.s_bot == 0.0) return a / xs.y_bot;
        return (std::sqrt(xs.y_bot * xs.y_bot + 4.0 * xs.s_bot * a) - xs.y_bot) /
               (2.0 * xs.s_bot);
    }

    OPENSWMM_KERNEL_FN double trapez_getRofA(const XSectParams& xs, double a) const {
        return a / (xs.y_bot + trapez_getYofA(xs, a) * xs.r_bot);
    }

    OPENSWMM_KERNEL_FN double trapez_getRofY(const XSectParams& xs, double y) const {
        return shape::trapezRofY(y, xs.y_bot, xs.s_bot, xs.r_bot);
    }

    OPENSWMM_KERNEL_FN double trapez_getdSdA(const XSectParams& xs, double a) const {
        if (a / xs.a_full <= 1.0e-30) return generic_getdSdA(xs, a);
        double r = trapez_getRofA(xs, a);
        double dPdA = xs.r_bot / std::sqrt(xs.y_bot * xs.y_bot + 4.0 * xs.s_bot * a);
        return (5.0 / 3.0 - (2.0 / 3.0) * dPdA * r) * std::pow(r, 2.0 / 3.0);
    }

    // ============================================================================
    // Triangular
    // ============================================================================

    OPENSWMM_KERNEL_FN double triang_getAofY(const XSectParams& xs, double y) const {
        return shape::triangAofY(y, xs.s_bot);
    }

    OPENSWMM_KERNEL_FN double triang_getWofY(const XSectParams& xs, double y) const {
        return shape::triangWofY(y, xs.s_bot);
    }

    OPENSWMM_KERNEL_FN double triang_getYofA(const XSectParams& xs, double a) const {
        return std::sqrt(a / xs.s_bot);
    }

    OPENSWMM_KERNEL_FN double triang_getRofA(const XSectParams& xs, double a) const {
        return a / (2.0 * triang_getYofA(xs, a) * xs.r_bot);
    }

    OPENSWMM_KERNEL_FN double triang_getRofY(const XSectParams& xs, double y) const {
        return shape::triangRofY(y, xs.s_bot, xs.r_bot);
    }

    OPENSWMM_KERNEL_FN double triang_getdSdA(const XSectParams& xs, double a) const {
        if (a / xs.a_full <= 1.0e-30) return generic_getdSdA(xs, a);
        double r = triang_getRofA(xs, a);
        double dPdA = xs.r_bot / std::sqrt(a * xs.s_bot);
        return (5.0 / 3.0 - (2.0 / 3.0) * dPdA * r) * std::pow(r, 2.0 / 3.0);
    }

    // ============================================================================
    // Parabolic (rBot = 1/sqrt(c) where y = c*x^2)
    // ============================================================================

    OPENSWMM_KERNEL_FN double parab_getAofY(const XSectParams& xs, double y) const {
        return shape::parabAofY(y, xs.r_bot);
    }

    OPENSWMM_KERNEL_FN double parab_getWofY(const XSectParams& xs, double y) const {
        return shape::parabWofY(y, xs.r_bot);
    }

    OPENSWMM_KERNEL_FN double parab_getYofA(const XSectParams& xs, double a) const {
        return std::pow((3.0 / 4.0) * a / xs.r_bot, 2.0 / 3.0);
    }

    // Analytical wetted perimeter (legacy parab_getPofY).
    OPENSWMM_KERNEL_FN double parab_getPofY(const XSectParams& xs, double y) const {
        double x = 2.0 * std::sqrt(y) / xs.r_bot;
        double t = std::sqrt(1.0 + x * x);
        return 0.5 * xs.r_bot * xs.r_bot * (x * t + std::log(x + t));
    }

    OPENSWMM_KERNEL_FN double parab_getRofY(const XSectParams& xs, double y) const {
        if (y <= 0.0) return 0.0;
        return parab_getAofY(xs, y) / parab_getPofY(xs, y);
    }

    OPENSWMM_KERNEL_FN double parab_getRofA(const XSectParams& xs, double a) const {
        if (a <= 0.0) return 0.0;
        return a / parab_getPofY(xs, parab_getYofA(xs, a));
    }

    // ============================================================================
    // Power function (sBot = 1/exponent, rBot = coefficient)
    // ============================================================================

    OPENSWMM_KERNEL_FN double powerfunc_getAofY(const XSectParams& xs, double y) const {
        return shape::powerfuncAofY(y, xs.s_bot, xs.r_bot);
    }

    OPENSWMM_KERNEL_FN double powerfunc_getWofY(const XSectParams& xs, double y) const {
        return shape::powerfuncWofY(y, xs.s_bot, xs.r_bot);
    }

    OPENSWMM_KERNEL_FN double powerfunc_getYofA(const XSectParams& xs, double a) const {
        return std::pow(a / xs.r_bot, 1.0 / (xs.s_bot + 1.0));
    }

    // Numerical wetted perimeter (legacy powerfunc_getPofY — stepwise integration).
    OPENSWMM_KERNEL_FN double powerfunc_getPofY(const XSectParams& xs, double y) const {
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

    OPENSWMM_KERNEL_FN double powerfunc_getRofY(const XSectParams& xs, double y) const {
        if (y <= 0.0) return 0.0;
        return powerfunc_getAofY(xs, y) / powerfunc_getPofY(xs, y);
    }

    OPENSWMM_KERNEL_FN double powerfunc_getRofA(const XSectParams& xs, double a) const {
        if (a <= 0.0) return 0.0;
        return a / powerfunc_getPofY(xs, powerfunc_getYofA(xs, a));
    }

    // ============================================================================
    // Main dispatch: getAofY
    // ============================================================================

    OPENSWMM_KERNEL_FN double getAofY(const XSectParams& xs, double y) const {
        if (y <= 0.0) return 0.0;
        // A section with no (or not-yet-set) full depth has no area. Guard the
        // division so a degenerate cross-section — e.g. a conduit finalized before
        // its geometry is assigned — yields 0 instead of normalizing to inf/NaN.
        if (xs.y_full <= 0.0) return 0.0;
        double y_norm = y / xs.y_full;

        switch (static_cast<XSectShape>(xs.type)) {
            case XSectShape::FORCE_MAIN:
            case XSectShape::CIRCULAR:
                return xs.a_full * lookup(y_norm, tbl.A_Circ, tbl.N_A_Circ);
            case XSectShape::FILLED_CIRCULAR: return filled_circ_getAofY(xs, y);
            case XSectShape::EGGSHAPED:
                return xs.a_full * lookup(y_norm, tbl.A_Egg, tbl.N_A_Egg);
            case XSectShape::HORSESHOE:
                return xs.a_full * lookup(y_norm, tbl.A_Horseshoe, tbl.N_A_Horseshoe);
            case XSectShape::GOTHIC:
                return xs.a_full * invLookup(y_norm, tbl.Y_Gothic, tbl.N_Y_Gothic);
            case XSectShape::CATENARY:
                return xs.a_full * invLookup(y_norm, tbl.Y_Catenary, tbl.N_Y_Catenary);
            case XSectShape::SEMIELLIPTICAL:
                return xs.a_full * invLookup(y_norm, tbl.Y_SemiEllip, tbl.N_Y_SemiEllip);
            case XSectShape::BASKETHANDLE:
                return xs.a_full * lookup(y_norm, tbl.A_Baskethandle, tbl.N_A_Baskethandle);
            case XSectShape::SEMICIRCULAR:
                return xs.a_full * invLookup(y_norm, tbl.Y_SemiCirc, tbl.N_Y_SemiCirc);
            case XSectShape::HORIZ_ELLIPSE:
                return xs.a_full * lookup(y_norm, tbl.A_HorizEllipse, tbl.N_A_HorizEllipse);
            case XSectShape::VERT_ELLIPSE:
                return xs.a_full * lookup(y_norm, tbl.A_VertEllipse, tbl.N_A_VertEllipse);
            case XSectShape::ARCH:
                return xs.a_full * lookup(y_norm, tbl.A_Arch, tbl.N_A_Arch);
            case XSectShape::RECT_CLOSED:
            case XSectShape::RECT_OPEN:    return shape::rectAofY(y, xs.w_max);
            case XSectShape::RECT_TRIANG:  return rect_triang_getAofY(xs, y);
            case XSectShape::RECT_ROUND:   return rect_round_getAofY(xs, y);
            case XSectShape::MOD_BASKET:   return mod_basket_getAofY(xs, y);
            case XSectShape::TRAPEZOIDAL:  return trapez_getAofY(xs, y);
            case XSectShape::TRIANGULAR:   return triang_getAofY(xs, y);
            case XSectShape::PARABOLIC:    return parab_getAofY(xs, y);
            case XSectShape::POWERFUNC:    return powerfunc_getAofY(xs, y);
            case XSectShape::IRREGULAR:
            case XSectShape::CUSTOM:
            case XSectShape::STREET_XSECT:
                // Tabulated transect (legacy xsect_getAofY IRREGULAR case).
                if (xs.area_tbl)
                    return xs.a_full * lookup(y_norm, xs.area_tbl, xs.transect_tbl_size);
                return 0.0;
            default: return 0.0;
        }
    }

    // ============================================================================
    // Main dispatch: getWofY
    // ============================================================================

    OPENSWMM_KERNEL_FN double getWofY(const XSectParams& xs, double y) const {
        double y_norm = y / xs.y_full;

        switch (static_cast<XSectShape>(xs.type)) {
            case XSectShape::FORCE_MAIN:
            case XSectShape::CIRCULAR:
                return xs.w_max * lookup(y_norm, tbl.W_Circ, tbl.N_W_Circ);
            case XSectShape::FILLED_CIRCULAR: {
                double yn = (y + xs.y_bot) / (xs.y_full + xs.y_bot);
                return xs.w_max * lookup(yn, tbl.W_Circ, tbl.N_W_Circ);
            }
            case XSectShape::EGGSHAPED:
                return xs.w_max * lookup(y_norm, tbl.W_Egg, tbl.N_W_Egg);
            case XSectShape::HORSESHOE:
                return xs.w_max * lookup(y_norm, tbl.W_Horseshoe, tbl.N_W_Horseshoe);
            case XSectShape::GOTHIC:
                return xs.w_max * lookup(y_norm, tbl.W_Gothic, tbl.N_W_Gothic);
            case XSectShape::CATENARY:
                return xs.w_max * lookup(y_norm, tbl.W_Catenary, tbl.N_W_Catenary);
            case XSectShape::SEMIELLIPTICAL:
                return xs.w_max * lookup(y_norm, tbl.W_SemiEllip, tbl.N_W_SemiEllip);
            case XSectShape::BASKETHANDLE:
                return xs.w_max * lookup(y_norm, tbl.W_BasketHandle, tbl.N_W_BasketHandle);
            case XSectShape::SEMICIRCULAR:
                return xs.w_max * lookup(y_norm, tbl.W_SemiCirc, tbl.N_W_SemiCirc);
            case XSectShape::HORIZ_ELLIPSE:
                return xs.w_max * lookup(y_norm, tbl.W_HorizEllipse, tbl.N_W_HorizEllipse);
            case XSectShape::VERT_ELLIPSE:
                return xs.w_max * lookup(y_norm, tbl.W_VertEllipse, tbl.N_W_VertEllipse);
            case XSectShape::ARCH:
                return xs.w_max * lookup(y_norm, tbl.W_Arch, tbl.N_W_Arch);
            case XSectShape::RECT_CLOSED:
                return shape::rectClosedWofY(y_norm, xs.w_max);
            case XSectShape::RECT_OPEN:    return xs.w_max;
            case XSectShape::RECT_TRIANG:  return rect_triang_getWofY(xs, y);
            case XSectShape::RECT_ROUND:   return rect_round_getWofY(xs, y);
            case XSectShape::MOD_BASKET:   return mod_basket_getWofY(xs, y);
            case XSectShape::TRAPEZOIDAL:  return trapez_getWofY(xs, y);
            case XSectShape::TRIANGULAR:   return triang_getWofY(xs, y);
            case XSectShape::PARABOLIC:    return parab_getWofY(xs, y);
            case XSectShape::POWERFUNC:    return powerfunc_getWofY(xs, y);
            case XSectShape::IRREGULAR:
            case XSectShape::CUSTOM:
            case XSectShape::STREET_XSECT:
                // Tabulated transect (legacy xsect_getWofY IRREGULAR case).
                if (xs.width_tbl)
                    return xs.w_max * lookup(y_norm, xs.width_tbl, xs.transect_tbl_size);
                return 0.0;
            default: return 0.0;
        }
    }

    // ============================================================================
    // Main dispatch: getRofY
    // ============================================================================

    OPENSWMM_KERNEL_FN double getRofY(const XSectParams& xs, double y) const {
        double y_norm = y / xs.y_full;

        switch (static_cast<XSectShape>(xs.type)) {
            case XSectShape::FORCE_MAIN:
            case XSectShape::CIRCULAR:
                return xs.r_full * lookup(y_norm, tbl.R_Circ, tbl.N_R_Circ);
            case XSectShape::FILLED_CIRCULAR:
                if (xs.y_bot == 0.0)
                    return xs.r_full * lookup(y_norm, tbl.R_Circ, tbl.N_R_Circ);
                return filled_circ_getRofY(xs, y);
            case XSectShape::EGGSHAPED:
                return xs.r_full * lookup(y_norm, tbl.R_Egg, tbl.N_R_Egg);
            case XSectShape::HORSESHOE:
                return xs.r_full * lookup(y_norm, tbl.R_Horseshoe, tbl.N_R_Horseshoe);
            case XSectShape::BASKETHANDLE:
                return xs.r_full * lookup(y_norm, tbl.R_Baskethandle, tbl.N_R_Baskethandle);
            case XSectShape::HORIZ_ELLIPSE:
                return xs.r_full * lookup(y_norm, tbl.R_HorizEllipse, tbl.N_R_HorizEllipse);
            case XSectShape::VERT_ELLIPSE:
                return xs.r_full * lookup(y_norm, tbl.R_VertEllipse, tbl.N_R_VertEllipse);
            case XSectShape::ARCH:
                return xs.r_full * lookup(y_norm, tbl.R_Arch, tbl.N_R_Arch);
            case XSectShape::RECT_TRIANG:  return rect_triang_getRofY(xs, y);
            case XSectShape::RECT_ROUND:   return rect_round_getRofY(xs, y);
            case XSectShape::TRAPEZOIDAL:  return trapez_getRofY(xs, y);
            case XSectShape::TRIANGULAR:   return triang_getRofY(xs, y);
            case XSectShape::PARABOLIC:    return parab_getRofY(xs, y);
            case XSectShape::POWERFUNC:    return powerfunc_getRofY(xs, y);
            case XSectShape::IRREGULAR:
            case XSectShape::CUSTOM:
            case XSectShape::STREET_XSECT:
                // Tabulated transect (legacy xsect_getRofY IRREGULAR case). Must be
                // explicit: the generic default below would call getRofA→getRofY
                // and recurse forever for these shapes.
                if (xs.hrad_tbl)
                    return xs.r_full * lookup(y_norm, xs.hrad_tbl, xs.transect_tbl_size);
                return 0.0;
            default:  // RECT_CLOSED, RECT_OPEN, MOD_BASKET, tabulated S-only shapes
                return getRofA(xs, getAofY(xs, y));
        }
    }

    // ============================================================================
    // Main dispatch: getYofA
    // ============================================================================

    OPENSWMM_KERNEL_FN double getYofA(const XSectParams& xs, double a) const {
        if (a <= 0.0) return 0.0;
        double alpha = a / xs.a_full;

        switch (static_cast<XSectShape>(xs.type)) {
            case XSectShape::FORCE_MAIN:
            case XSectShape::CIRCULAR:        return circ_getYofA(xs, a);
            case XSectShape::FILLED_CIRCULAR: return filled_circ_getYofA(xs, a);
            case XSectShape::EGGSHAPED:
                return xs.y_full * lookup(alpha, tbl.Y_Egg, tbl.N_Y_Egg);
            case XSectShape::HORSESHOE:
                return xs.y_full * lookup(alpha, tbl.Y_Horseshoe, tbl.N_Y_Horseshoe);
            case XSectShape::GOTHIC:
                return xs.y_full * lookup(alpha, tbl.Y_Gothic, tbl.N_Y_Gothic);
            case XSectShape::CATENARY:
                return xs.y_full * lookup(alpha, tbl.Y_Catenary, tbl.N_Y_Catenary);
            case XSectShape::SEMIELLIPTICAL:
                return xs.y_full * lookup(alpha, tbl.Y_SemiEllip, tbl.N_Y_SemiEllip);
            case XSectShape::BASKETHANDLE:
                return xs.y_full * lookup(alpha, tbl.Y_BasketHandle, tbl.N_Y_BasketHandle);
            case XSectShape::SEMICIRCULAR:
                return xs.y_full * lookup(alpha, tbl.Y_SemiCirc, tbl.N_Y_SemiCirc);
            case XSectShape::HORIZ_ELLIPSE:
                return xs.y_full * invLookup(alpha, tbl.A_HorizEllipse, tbl.N_A_HorizEllipse);
            case XSectShape::VERT_ELLIPSE:
                return xs.y_full * invLookup(alpha, tbl.A_VertEllipse, tbl.N_A_VertEllipse);
            case XSectShape::ARCH:
                return xs.y_full * invLookup(alpha, tbl.A_Arch, tbl.N_A_Arch);
            case XSectShape::RECT_CLOSED:
            case XSectShape::RECT_OPEN:    return a / xs.w_max;
            case XSectShape::RECT_TRIANG:  return rect_triang_getYofA(xs, a);
            case XSectShape::RECT_ROUND:   return rect_round_getYofA(xs, a);
            case XSectShape::MOD_BASKET:   return mod_basket_getYofA(xs, a);
            case XSectShape::TRAPEZOIDAL:  return trapez_getYofA(xs, a);
            case XSectShape::TRIANGULAR:   return triang_getYofA(xs, a);
            case XSectShape::PARABOLIC:    return parab_getYofA(xs, a);
            case XSectShape::POWERFUNC:    return powerfunc_getYofA(xs, a);
            case XSectShape::IRREGULAR:
            case XSectShape::CUSTOM:
            case XSectShape::STREET_XSECT:
                // Invert the normalized area table (legacy xsect_getYofA IRREGULAR).
                if (xs.area_tbl)
                    return xs.y_full * invLookup(alpha, xs.area_tbl, xs.transect_tbl_size);
                return 0.0;
            default: return 0.0;
        }
    }

    // ============================================================================
    // Main dispatch: getSofA
    // ============================================================================

    OPENSWMM_KERNEL_FN double getSofA(const XSectParams& xs, double a) const {
        double alpha = a / xs.a_full;

        switch (static_cast<XSectShape>(xs.type)) {
            case XSectShape::FORCE_MAIN:
            case XSectShape::CIRCULAR:     return circ_getSofA(xs, a);
            case XSectShape::EGGSHAPED:
                return xs.s_full * lookup(alpha, tbl.S_Egg, tbl.N_S_Egg);
            case XSectShape::HORSESHOE:
                return xs.s_full * lookup(alpha, tbl.S_Horseshoe, tbl.N_S_Horseshoe);
            case XSectShape::GOTHIC:
                return xs.s_full * lookup(alpha, tbl.S_Gothic, tbl.N_S_Gothic);
            case XSectShape::CATENARY:
                return xs.s_full * lookup(alpha, tbl.S_Catenary, tbl.N_S_Catenary);
            case XSectShape::SEMIELLIPTICAL:
                return xs.s_full * lookup(alpha, tbl.S_SemiEllip, tbl.N_S_SemiEllip);
            case XSectShape::BASKETHANDLE:
                return xs.s_full * lookup(alpha, tbl.S_BasketHandle, tbl.N_S_BasketHandle);
            case XSectShape::SEMICIRCULAR:
                return xs.s_full * lookup(alpha, tbl.S_SemiCirc, tbl.N_S_SemiCirc);
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

    OPENSWMM_KERNEL_FN double getRofA(const XSectParams& xs, double a) const {
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
                return shape::rectOpenRofA(a, xs.w_max, xs.s_bot);
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

    OPENSWMM_KERNEL_FN double getdSdA(const XSectParams& xs, double a) const {
        switch (static_cast<XSectShape>(xs.type)) {
            case XSectShape::FORCE_MAIN:
            case XSectShape::CIRCULAR:     return circ_getdSdA(xs, a);
            case XSectShape::EGGSHAPED:
                return tabular_getdSdA(xs, a, tbl.S_Egg, tbl.N_S_Egg);
            case XSectShape::HORSESHOE:
                return tabular_getdSdA(xs, a, tbl.S_Horseshoe, tbl.N_S_Horseshoe);
            case XSectShape::GOTHIC:
                return tabular_getdSdA(xs, a, tbl.S_Gothic, tbl.N_S_Gothic);
            case XSectShape::CATENARY:
                return tabular_getdSdA(xs, a, tbl.S_Catenary, tbl.N_S_Catenary);
            case XSectShape::SEMIELLIPTICAL:
                return tabular_getdSdA(xs, a, tbl.S_SemiEllip, tbl.N_S_SemiEllip);
            case XSectShape::BASKETHANDLE:
                return tabular_getdSdA(xs, a, tbl.S_BasketHandle, tbl.N_S_BasketHandle);
            case XSectShape::SEMICIRCULAR:
                return tabular_getdSdA(xs, a, tbl.S_SemiCirc, tbl.N_S_SemiCirc);
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

    OPENSWMM_KERNEL_FN double getAofS(const XSectParams& xs, double s) const {
        double psi = s / xs.s_full;
        if (s <= 0.0) return 0.0;
        if (s > xs.s_max) s = xs.s_max;

        switch (static_cast<XSectShape>(xs.type)) {
            case XSectShape::DUMMY: return 0.0;
            case XSectShape::FORCE_MAIN:
            case XSectShape::CIRCULAR:     return circ_getAofS(xs, s);
            case XSectShape::EGGSHAPED:
                return xs.a_full * invLookup(psi, tbl.S_Egg, tbl.N_S_Egg);
            case XSectShape::HORSESHOE:
                return xs.a_full * invLookup(psi, tbl.S_Horseshoe, tbl.N_S_Horseshoe);
            case XSectShape::GOTHIC:
                return xs.a_full * invLookup(psi, tbl.S_Gothic, tbl.N_S_Gothic);
            case XSectShape::CATENARY:
                return xs.a_full * invLookup(psi, tbl.S_Catenary, tbl.N_S_Catenary);
            case XSectShape::SEMIELLIPTICAL:
                return xs.a_full * invLookup(psi, tbl.S_SemiEllip, tbl.N_S_SemiEllip);
            case XSectShape::BASKETHANDLE:
                return xs.a_full * invLookup(psi, tbl.S_BasketHandle, tbl.N_S_BasketHandle);
            case XSectShape::SEMICIRCULAR:
                return xs.a_full * invLookup(psi, tbl.S_SemiCirc, tbl.N_S_SemiCirc);
            default: {
                // Newton-Raphson on S(a) = s, bracketed in [a1, a2] (legacy generic_getAofS).
                // a2 = absolute area at max flow = xsect_getAmax.
                // PARITY: legacy xsect_getAmax (xsect.c:711-713) returns aBot for
                // BOTH IRREGULAR and CUSTOM (the physical area at the max section
                // factor, set from the transect/shape tables); every other shape
                // uses aFull * Amax-ratio.
                double a1, a2;
                const XSectShape sh = static_cast<XSectShape>(xs.type);
                double a_max = (sh == XSectShape::CUSTOM || sh == XSectShape::IRREGULAR)
                                   ? xs.a_bot
                                   : xs.a_full * getAmax(xs);
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

    OPENSWMM_KERNEL_FN double getAmax(const XSectParams& xs) const {
        if (xs.type >= 0 && xs.type <= 25)
            return tbl.Amax[xs.type];
        return 1.0;
    }

    // ============================================================================
    // getYcrit — critical depth for a given flow rate (legacy xsect_getYcrit)
    // ============================================================================

    OPENSWMM_KERNEL_FN double getYcrit(const XSectParams& xs, double q) const {
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

    OPENSWMM_KERNEL_FN bool isOpen(int type) const {
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

};

/// The host-bound table set and evaluator: `hostTables()` binds every pointer
/// straight back to the `xsect_tables::` arrays, so `hostEval()` reproduces the
/// pre-extraction code exactly. A device consumer builds its own pair over
/// device copies of the same arrays.
const XsectTables& hostTables();
const XsectEval&   hostEval();

} // namespace openswmm::xsect

#endif // OPENSWMM_XSECT_KERNELS_HPP
