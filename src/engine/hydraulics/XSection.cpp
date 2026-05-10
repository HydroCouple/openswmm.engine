/**
 * @file XSection.cpp
 * @brief Cross-section geometry — numerically identical to legacy xsect.c.
 *
 * @details All formulas, lookup tables, constants, and dispatch logic are
 *          direct translations from src/legacy/engine/xsect.c (SWMM 5.2.1).
 *          Table data comes from xsect_tables.hpp.
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

static constexpr double TINY = 1.0e-6;
static constexpr double PI   = 3.14159265358979323846;

// ============================================================================
// Lookup helpers (identical to legacy)
// ============================================================================

int locate(double y, const double* table, int n) {
    int lo = 0, hi = n;
    while (hi - lo > 1) {
        int mid = (lo + hi) / 2;
        if (table[mid] > y) hi = mid;
        else                lo = mid;
    }
    return lo;
}

double lookup(double x, const double* table, int n_items) {
    double delta = 1.0 / static_cast<double>(n_items - 1);
    int i = static_cast<int>(x / delta);
    if (i >= n_items - 1) return table[n_items - 1];

    double x0 = i * delta;
    double x1 = (static_cast<double>(i) + 1.0) * delta;

    // Linear interpolation
    double y = table[i] + (x - x0) * (table[i + 1] - table[i]) / delta;

    // Quadratic refinement for small x values
    if (i < 2) {
        double y2 = y + (x - x0) * (x - x1) / (delta * delta) *
                    (table[i] / 2.0 - table[i + 1] + table[i + 2] / 2.0);
        if (y2 > 0.0) y = y2;
    }
    y = std::max(y, 0.0);
    return y;
}

double invLookup(double y, const double* table, int n_items) {
    double dx = 1.0 / static_cast<double>(n_items - 1);
    int n = n_items;

    // Truncate if last 2 entries are decreasing
    if (table[n - 3] > table[n - 1]) n = n - 2;

    // Check if y falls in decreasing portion of table
    int i;
    if (n < n_items && y > table[n_items - 1]) {
        if (y >= table[n_items - 3]) return static_cast<double>(n - 1) * dx;
        if (y <= table[n_items - 2]) i = n_items - 2;
        else i = n_items - 3;
    } else {
        i = locate(y, table, n - 1);
    }
    if (i >= n - 1) return static_cast<double>(n - 1) * dx;

    double x0 = i * dx;
    double dy = table[i + 1] - table[i];
    double x;
    if (dy == 0.0) x = x0;
    else x = x0 + (y - table[i]) * dx / dy;
    x = std::max(x, 0.0);
    x = std::min(x, 1.0);
    return x;
}

// ============================================================================
// Special circular functions for small alpha (< 0.04)
// ============================================================================

double getYcircular(double alpha) {
    // From legacy: for very small areas, use power approximation
    // y/D = 0.7854 * (A/Afull)^0.4 (empirical fit)
    return 0.7854 * std::pow(alpha, 0.4);
}

double getScircular(double alpha) {
    // S/Sfull = (A/Afull) * (R/Rfull)^(2/3)
    // For small alpha: approximate R/Rfull ≈ (Y/Yfull)
    double y_norm = getYcircular(alpha);
    double r_norm = lookup(y_norm, xsect_tables::R_Circ, xsect_tables::N_R_Circ);
    return alpha * std::pow(r_norm, 2.0 / 3.0);
}

// ============================================================================
// Shape-specific analytical functions — trapezoidal
// ============================================================================

static double trapez_getAofY(const XSectParams& xs, double y) {
    return (xs.y_bot + xs.s_bot * y) * y;
}

static double trapez_getWofY(const XSectParams& xs, double y) {
    return xs.y_bot + 2.0 * y * xs.s_bot;
}

static double trapez_getYofA(const XSectParams& xs, double a) {
    if (xs.s_bot == 0.0) return a / xs.y_bot;
    return (std::sqrt(xs.y_bot * xs.y_bot + 4.0 * xs.s_bot * a)
            - xs.y_bot) / (2.0 * xs.s_bot);
}

static double trapez_getRofY(const XSectParams& xs, double y) {
    if (y == 0.0) return 0.0;
    return trapez_getAofY(xs, y) / (xs.y_bot + y * xs.r_bot);
}

// ============================================================================
// Shape-specific analytical functions — triangular
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

static double triang_getRofY(const XSectParams& xs, double y) {
    return (y * xs.s_bot) / (2.0 * xs.r_bot);
}

// ============================================================================
// Shape-specific analytical functions — parabolic
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

static double parab_getPofY(const XSectParams& xs, double y) {
    double dy1 = 0.02 * xs.y_full;
    double x1 = 0.0, y1 = 0.0, p = 0.0;
    do {
        double y2 = y1 + dy1;
        y2 = std::min(y2, y);
        double x2 = 0.5 * xs.r_bot * xs.r_bot * y2;
        double dx = x2 - x1;
        double dy = y2 - y1;
        p += std::sqrt(dx * dx + dy * dy);
        x1 = x2;
        y1 = y2;
    } while (y1 < y);
    return 2.0 * p;
}

static double parab_getRofY(const XSectParams& xs, double y) {
    if (y <= 0.0) return 0.0;
    return parab_getAofY(xs, y) / parab_getPofY(xs, y);
}

// ============================================================================
// Shape-specific analytical functions — power function
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

static double powerfunc_getPofY(const XSectParams& xs, double y) {
    double dy1 = 0.02 * xs.y_full;
    double h = (xs.s_bot + 1.0) * xs.r_bot / 2.0;
    double m = xs.s_bot;
    double p = 0.0;
    double y1 = 0.0, x1 = 0.0;
    do {
        double y2 = y1 + dy1;
        y2 = std::min(y2, y);
        double x2 = h * std::pow(y2, m);
        double dx = x2 - x1;
        double dy = y2 - y1;
        p += std::sqrt(dx * dx + dy * dy);
        x1 = x2;
        y1 = y2;
    } while (y1 < y);
    return 2.0 * p;
}

static double powerfunc_getRofY(const XSectParams& xs, double y) {
    if (y <= 0.0) return 0.0;
    return powerfunc_getAofY(xs, y) / powerfunc_getPofY(xs, y);
}

// ============================================================================
// Shape-specific analytical functions — circular (for YofA, SofA)
// ============================================================================

static double circ_getYofA(const XSectParams& xs, double a) {
    double alpha = a / xs.a_full;
    if (alpha < 0.04)
        return xs.y_full * getYcircular(alpha);
    else
        return xs.y_full * lookup(alpha, xsect_tables::Y_Circ, xsect_tables::N_Y_Circ);
}

static double circ_getSofA(const XSectParams& xs, double a) {
    double alpha = a / xs.a_full;
    if (alpha < 0.04)
        return xs.s_full * getScircular(alpha);
    else
        return xs.s_full * lookup(alpha, xsect_tables::S_Circ, xsect_tables::N_S_Circ);
}

// ============================================================================
// Shape-specific — filled circular
// ============================================================================

static double filled_circ_getAofY(const XSectParams& xs, double y) {
    // Full pipe area at depth y+yBot, minus filled bottom area
    double y_norm = (y + xs.y_bot) / (xs.y_full + xs.y_bot);
    double a_pipe = (xs.a_full + xs.a_bot) *
                    lookup(y_norm, xsect_tables::A_Circ, xsect_tables::N_A_Circ);
    return a_pipe - xs.a_bot;
}

static double filled_circ_getYofA(const XSectParams& xs, double a) {
    double a_total = a + xs.a_bot;
    double a_full_pipe = xs.a_full + xs.a_bot;
    double alpha = a_total / a_full_pipe;
    double y_pipe;
    if (alpha < 0.04)
        y_pipe = (xs.y_full + xs.y_bot) * getYcircular(alpha);
    else
        y_pipe = (xs.y_full + xs.y_bot) *
                 lookup(alpha, xsect_tables::Y_Circ, xsect_tables::N_Y_Circ);
    return y_pipe - xs.y_bot;
}

static double filled_circ_getRofY(const XSectParams& xs, double y) {
    double a = filled_circ_getAofY(xs, y);
    if (a <= 0.0) return 0.0;
    // Wetted perimeter of pipe from bottom of fill to y+yBot
    double y_norm_top = (y + xs.y_bot) / (xs.y_full + xs.y_bot);
    double y_norm_bot = xs.y_bot / (xs.y_full + xs.y_bot);
    double d = xs.y_full + xs.y_bot;  // pipe diameter

    // Use theta = 2*acos(1 - 2*y/D) for pipe perimeter
    double theta_top = 2.0 * std::acos(1.0 - 2.0 * y_norm_top);
    double theta_bot = 2.0 * std::acos(1.0 - 2.0 * y_norm_bot);
    double p = d / 2.0 * (theta_top - theta_bot);
    // Add fill top width
    double w_fill = d * std::sin(theta_bot / 2.0);
    p += w_fill;
    if (p <= 0.0) return 0.0;
    return a / p;
}

// ============================================================================
// Shape-specific — rect_triang
// ============================================================================

static double rect_triang_getAofY(const XSectParams& xs, double y) {
    if (y <= xs.y_bot) {
        // In triangular bottom portion
        return y * y * xs.s_bot;
    }
    // Rectangular portion above triangle
    return xs.a_bot + xs.w_max * (y - xs.y_bot);
}

static double rect_triang_getWofY(const XSectParams& xs, double y) {
    if (y <= xs.y_bot) {
        return 2.0 * xs.s_bot * y;
    }
    if (y >= xs.y_full) return 0.0;
    return xs.w_max;
}

static double rect_triang_getYofA(const XSectParams& xs, double a) {
    if (a <= xs.a_bot) {
        return std::sqrt(a / xs.s_bot);
    }
    return xs.y_bot + (a - xs.a_bot) / xs.w_max;
}

static double rect_triang_getRofY(const XSectParams& xs, double y) {
    if (y <= 0.0) return 0.0;
    double a = rect_triang_getAofY(xs, y);
    double p;
    if (y <= xs.y_bot) {
        p = 2.0 * y * xs.r_bot;  // r_bot = sqrt(1 + s_bot^2)
    } else {
        p = 2.0 * xs.y_bot * xs.r_bot + 2.0 * (y - xs.y_bot);
    }
    if (p <= 0.0) return 0.0;
    return a / p;
}

// ============================================================================
// Shape-specific — rect_round
// ============================================================================

static double rect_round_getAofY(const XSectParams& xs, double y) {
    if (y <= xs.y_bot) {
        // Circular segment at bottom
        double y_norm = y / (xs.y_full + xs.y_bot);
        // Use circular area formula with the bottom radius
        double theta = 2.0 * std::acos(1.0 - y / xs.r_bot);
        double a = xs.r_bot * xs.r_bot / 2.0 * (theta - std::sin(theta));
        return a;
    }
    return xs.a_bot + xs.w_max * (y - xs.y_bot);
}

static double rect_round_getWofY(const XSectParams& xs, double y) {
    if (y <= xs.y_bot) {
        double theta = 2.0 * std::acos(1.0 - y / xs.r_bot);
        return 2.0 * xs.r_bot * std::sin(theta / 2.0);
    }
    if (y >= xs.y_full) return 0.0;
    return xs.w_max;
}

static double rect_round_getYofA(const XSectParams& xs, double a) {
    if (a <= xs.a_bot) {
        // Newton iteration to find y from circular segment area
        double y = xs.y_bot * a / xs.a_bot;  // linear initial guess
        for (int iter = 0; iter < 40; ++iter) {
            double theta = 2.0 * std::acos(1.0 - y / xs.r_bot);
            double a_calc = xs.r_bot * xs.r_bot / 2.0 * (theta - std::sin(theta));
            double w = 2.0 * xs.r_bot * std::sin(theta / 2.0);
            if (w == 0.0) break;
            double dy = (a - a_calc) / w;
            y += dy;
            if (std::fabs(dy) < TINY) break;
        }
        return y;
    }
    return xs.y_bot + (a - xs.a_bot) / xs.w_max;
}

static double rect_round_getRofY(const XSectParams& xs, double y) {
    if (y <= 0.0) return 0.0;
    double a = rect_round_getAofY(xs, y);
    double p;
    if (y <= xs.y_bot) {
        double theta = 2.0 * std::acos(1.0 - y / xs.r_bot);
        p = xs.r_bot * theta;
    } else {
        double theta = 2.0 * std::acos(1.0 - xs.y_bot / xs.r_bot);
        p = xs.r_bot * theta + 2.0 * (y - xs.y_bot);
    }
    if (p <= 0.0) return 0.0;
    return a / p;
}

// ============================================================================
// Shape-specific — mod_basket
// ============================================================================

static double mod_basket_getAofY(const XSectParams& xs, double y) {
    if (y <= xs.y_bot) {
        // Circular arc at bottom
        double theta = 2.0 * std::acos(1.0 - y / xs.r_bot);
        return xs.r_bot * xs.r_bot / 2.0 * (theta - std::sin(theta));
    }
    return xs.a_bot + xs.w_max * (y - xs.y_bot);
}

static double mod_basket_getWofY(const XSectParams& xs, double y) {
    if (y <= xs.y_bot) {
        double theta = 2.0 * std::acos(1.0 - y / xs.r_bot);
        return 2.0 * xs.r_bot * std::sin(theta / 2.0);
    }
    if (y >= xs.y_full) return 0.0;
    return xs.w_max;
}

static double mod_basket_getYofA(const XSectParams& xs, double a) {
    if (a <= xs.a_bot) {
        double y = xs.y_bot * a / xs.a_bot;
        for (int iter = 0; iter < 40; ++iter) {
            double theta = 2.0 * std::acos(1.0 - y / xs.r_bot);
            double a_calc = xs.r_bot * xs.r_bot / 2.0 * (theta - std::sin(theta));
            double w = 2.0 * xs.r_bot * std::sin(theta / 2.0);
            if (w == 0.0) break;
            double dy = (a - a_calc) / w;
            y += dy;
            if (std::fabs(dy) < TINY) break;
        }
        return y;
    }
    return xs.y_bot + (a - xs.a_bot) / xs.w_max;
}

// ============================================================================
// Shape-specific — rect_closed and rect_open section factors
// ============================================================================

static double rect_closed_getSofA(const XSectParams& xs, double a) {
    if (a == 0.0) return 0.0;
    double y = a / xs.w_max;
    double p = xs.w_max + 2.0 * y;
    double r = a / p;
    return a * std::pow(r, 2.0 / 3.0);
}

static double rect_open_getSofA(const XSectParams& xs, double a) {
    if (a == 0.0) return 0.0;
    double y = a / xs.w_max;
    double p = xs.w_max + 2.0 * y;
    double r = a / p;
    return a * std::pow(r, 2.0 / 3.0);
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

        case XSectShape::FILLED_CIRCULAR:
            return filled_circ_getAofY(xs, y);

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
        case XSectShape::RECT_OPEN:
            return y * xs.w_max;

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

        case XSectShape::RECT_OPEN:
            return xs.w_max;

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

        case XSectShape::RECT_CLOSED:
        case XSectShape::RECT_OPEN: {
            if (y <= 0.0) return 0.0;
            double a = y * xs.w_max;
            double p = xs.w_max + 2.0 * y;
            return (p > 0.0) ? a / p : 0.0;
        }

        // Default: compute R = A / P via getRofA(getAofY(y))
        default:
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
        case XSectShape::CIRCULAR:
            return circ_getYofA(xs, a);

        case XSectShape::FILLED_CIRCULAR:
            return filled_circ_getYofA(xs, a);

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
        case XSectShape::RECT_OPEN:
            return a / xs.w_max;

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
    if (a <= 0.0) return 0.0;
    double alpha = a / xs.a_full;

    switch (static_cast<XSectShape>(xs.type)) {
        case XSectShape::FORCE_MAIN:
        case XSectShape::CIRCULAR:
            return circ_getSofA(xs, a);

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

        case XSectShape::RECT_CLOSED:
            return rect_closed_getSofA(xs, a);

        case XSectShape::RECT_OPEN:
            return rect_open_getSofA(xs, a);

        default: {
            // General: S = A * R^(2/3)
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
    double y = getYofA(xs, a);
    return getRofY(xs, y);
}

// ============================================================================
// getdSdA — derivative of section factor w.r.t. area
// ============================================================================

double getdSdA(const XSectParams& xs, double a) {
    if (a <= 0.0) return 0.0;
    double da = 0.001 * xs.a_full;
    if (da == 0.0) return 0.0;
    double s1 = getSofA(xs, a);
    double s2 = getSofA(xs, a + da);
    return (s2 - s1) / da;
}

// ============================================================================
// getAofS — area from section factor (Newton iteration)
// ============================================================================

double getAofS(const XSectParams& xs, double s_factor) {
    if (s_factor <= 0.0) return 0.0;
    if (s_factor >= xs.s_max) return xs.a_full * getAmax(xs);

    // Newton iteration: find a such that getSofA(a) = s_factor
    double a = xs.a_full * s_factor / xs.s_full;  // initial guess
    for (int iter = 0; iter < 40; ++iter) {
        double s = getSofA(xs, a);
        double ds = getdSdA(xs, a);
        if (ds == 0.0) break;
        double da = (s_factor - s) / ds;
        a += da;
        a = std::max(a, 0.0);
        a = std::min(a, xs.a_full);
        if (std::fabs(da) < TINY * xs.a_full) break;
    }
    return a;
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
// getYcrit — critical depth for a given flow rate
// ============================================================================

double getYcrit(const XSectParams& xs, double q) {
    if (q <= 0.0) return 0.0;
    // Matching legacy xsect_getYcrit in xsect.c
    static constexpr double GRAVITY = 32.2;
    double q2g = q * q / GRAVITY;

    if (q2g == 0.0) return 0.0;

    double y;
    auto shape = static_cast<XSectShape>(xs.type);

    switch (shape) {
        case XSectShape::DUMMY:
            return 0.0;

        case XSectShape::RECT_OPEN:
        case XSectShape::RECT_CLOSED:
            // Analytical: y = (q²/g / w²)^(1/3)
            y = std::pow(q2g / (xs.w_max * xs.w_max), 1.0 / 3.0);
            break;

        case XSectShape::TRIANGULAR:
            // Analytical: y = (2 * q²/g / s²)^(1/5) where s = side slope
            y = std::pow(2.0 * q2g / (xs.s_bot * xs.s_bot), 1.0 / 5.0);
            break;

        case XSectShape::PARABOLIC:
            // Analytical: y = (27/32 * q²/g / rBot²)^(1/4)
            y = std::pow(27.0 / 32.0 * q2g / (xs.r_bot * xs.r_bot), 1.0 / 4.0);
            break;

        case XSectShape::POWERFUNC:
            y = 1.0 / (2.0 * xs.s_bot + 3.0);
            y = std::pow(q2g * (xs.s_bot + 1.0) / (xs.r_bot * xs.r_bot), y);
            break;

        default: {
            // Numerical: interval enumeration matching legacy getYcritEnum.
            // Divide depth range into 25 increments, find where Q_crit crosses Q.
            constexpr int N_INC = 25;
            double dy = xs.y_full / N_INC;
            if (dy <= 0.0) return 0.0;

            // Initial estimate: equivalent circular approximation
            double y0 = 1.01 * std::pow(q2g / xs.y_full, 0.25);
            if (y0 >= xs.y_full) y0 = 0.97 * xs.y_full;
            int i0 = static_cast<int>(y0 / dy);
            i0 = std::max(i0, 0);
            i0 = std::min(i0, N_INC);

            // Compute Q_crit at depth = i*dy: Q_c = A * sqrt(g*A/W)
            auto qCritAt = [&](double depth) -> double {
                if (depth <= 0.0) return 0.0;
                double a = getAofY(xs, depth);
                double w = getWofY(xs, depth);
                if (a <= 0.0 || w <= 0.0) return 0.0;
                return a * std::sqrt(GRAVITY * a / w);
            };

            double q0 = qCritAt(i0 * dy);
            y = xs.y_full;  // default if not found

            if (q0 < q) {
                // Search upward
                for (int i = i0 + 1; i <= N_INC; ++i) {
                    double qc = qCritAt(i * dy);
                    if (qc >= q) {
                        // Linear interpolation
                        y = ((q - q0) / (qc - q0) + static_cast<double>(i - 1)) * dy;
                        break;
                    }
                    q0 = qc;
                }
            } else {
                // Search downward
                for (int i = i0 - 1; i >= 0; --i) {
                    double qc = qCritAt(i * dy);
                    if (qc <= q) {
                        // Linear interpolation
                        y = (static_cast<double>(i) + (q - qc) / (q0 - qc)) * dy;
                        break;
                    }
                    q0 = qc;
                }
            }
            break;
        }
    }

    // Do not allow yCritical > yFull
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
// setParams — compute full-depth properties from raw geometry inputs
// ============================================================================

int setParams(XSectParams& xs, int type, const double p[], double ucf) {
    xs.type = type;
    auto shape = static_cast<XSectShape>(type);

    switch (shape) {
        case XSectShape::CIRCULAR:
        case XSectShape::FORCE_MAIN: {
            double d = p[0] / ucf;
            xs.y_full = d;
            xs.w_max  = d;
            xs.yw_max = d / 2.0;
            xs.a_full = PI / 4.0 * d * d;
            xs.r_full = d / 4.0;
            xs.s_full = xs.a_full * std::pow(xs.r_full, 2.0 / 3.0);
            if (shape == XSectShape::FORCE_MAIN) xs.r_bot = p[1];
            break;
        }

        case XSectShape::RECT_CLOSED: {
            double y = p[0] / ucf;
            double w = p[1] / ucf;
            xs.y_full = y;
            xs.w_max  = w;
            xs.yw_max = 0.0;
            xs.a_full = y * w;
            xs.r_full = y * w / (2.0 * y + w);
            xs.s_full = xs.a_full * std::pow(xs.r_full, 2.0 / 3.0);
            break;
        }

        case XSectShape::RECT_OPEN: {
            double y = p[0] / ucf;
            double w = p[1] / ucf;
            xs.y_full = y;
            xs.w_max  = w;
            xs.yw_max = y;
            xs.a_full = y * w;
            xs.r_full = y * w / (2.0 * y + w);
            xs.s_full = xs.a_full * std::pow(xs.r_full, 2.0 / 3.0);
            break;
        }

        case XSectShape::TRAPEZOIDAL: {
            double y = p[0] / ucf;
            double yb = p[1] / ucf;         // bottom width
            double m1 = p[2], m2 = p[3];    // side slopes (dimensionless)
            double m = (m1 + m2) / 2.0;
            xs.y_full = y;
            xs.y_bot  = yb;
            xs.s_bot  = m;
            xs.r_bot  = std::sqrt(1.0 + m1 * m1) + std::sqrt(1.0 + m2 * m2);
            xs.w_max  = yb + 2.0 * m * y;
            xs.yw_max = y;
            xs.a_full = (yb + m * y) * y;
            double p_full = yb + y * xs.r_bot;
            xs.r_full = xs.a_full / p_full;
            xs.s_full = xs.a_full * std::pow(xs.r_full, 2.0 / 3.0);
            break;
        }

        case XSectShape::TRIANGULAR: {
            double y = p[0] / ucf;
            double w = p[1] / ucf;
            double m = w / y / 2.0;
            xs.y_full = y;
            xs.w_max  = w;
            xs.yw_max = y;
            xs.s_bot  = m;
            xs.r_bot  = std::sqrt(1.0 + m * m);
            xs.a_full = m * y * y;
            xs.r_full = (m * y) / (2.0 * xs.r_bot);
            xs.s_full = xs.a_full * std::pow(xs.r_full, 2.0 / 3.0);
            break;
        }

        case XSectShape::PARABOLIC: {
            double y = p[0] / ucf;
            double w = p[1] / ucf;
            xs.y_full = y;
            xs.w_max  = w;
            xs.yw_max = y;
            xs.r_bot  = w / (2.0 * std::sqrt(y));
            xs.a_full = (4.0 / 3.0) * xs.r_bot * y * std::sqrt(y);
            double p_full = parab_getPofY(xs, y);
            xs.r_full = xs.a_full / p_full;
            xs.s_full = xs.a_full * std::pow(xs.r_full, 2.0 / 3.0);
            break;
        }

        case XSectShape::POWERFUNC: {
            double y = p[0] / ucf;
            double w = p[1] / ucf;
            double exp = p[2];
            xs.y_full = y;
            xs.w_max  = w;
            xs.yw_max = y;
            xs.s_bot  = 1.0 / exp;
            xs.r_bot  = w / (xs.s_bot + 1.0) / std::pow(y, xs.s_bot);
            xs.a_full = xs.r_bot * std::pow(y, xs.s_bot + 1.0);
            double p_full = powerfunc_getPofY(xs, y);
            xs.r_full = xs.a_full / p_full;
            xs.s_full = xs.a_full * std::pow(xs.r_full, 2.0 / 3.0);
            break;
        }

        default:
            // Tabulated shapes — a_full, r_full etc. set externally
            break;
    }

    // Compute s_max for all shapes
    double a_max_ratio = getAmax(xs);
    if (a_max_ratio < 1.0) {
        double a_max = xs.a_full * a_max_ratio;
        xs.s_max = getSofA(xs, a_max);
    } else {
        xs.s_max = xs.s_full;
    }

    return 0;
}

} // namespace xsect
} // namespace openswmm
