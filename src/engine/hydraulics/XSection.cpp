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
#include "XSectLookup.hpp"
#include "XSectKernels.hpp"

// Belt-and-suspenders against FMA contraction (global -ffp-contract=off /
// /fp:precise already enforce this; see XSectLookup.hpp §3). Keeps the scalar
// bit-exact lookup path from fusing mul+add and diverging across platforms.
#if defined(__clang__) || defined(__GNUC__)
#  pragma STDC FP_CONTRACT OFF
#endif

#include <cmath>
#include <algorithm>

namespace openswmm {
namespace xsect {


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
// Geometry layer — MOVED to XSectKernels.hpp (plan §5.1 gate 1).
//
// The bodies below used to live here. They now live in the portable header so
// the identical source compiles for the host dynamic-wave solver, the host
// finite-volume solver and the finite-volume device backend. Nothing was
// rewritten: the only edit was replacing `xsect_tables::X` with `tbl.X`, where
// `tbl` binds straight back to `xsect_tables::X` on the host. The public
// entry points are one-line forwarders so every existing caller is unchanged.
// ============================================================================

const XsectTables& hostTables() {
    static const XsectTables t = [] {
        XsectTables t;
    t.A_Arch = xsect_tables::A_Arch;
    t.A_Baskethandle = xsect_tables::A_Baskethandle;
    t.A_Circ = xsect_tables::A_Circ;
    t.A_Egg = xsect_tables::A_Egg;
    t.A_HorizEllipse = xsect_tables::A_HorizEllipse;
    t.A_Horseshoe = xsect_tables::A_Horseshoe;
    t.A_VertEllipse = xsect_tables::A_VertEllipse;
    t.R_Arch = xsect_tables::R_Arch;
    t.R_Baskethandle = xsect_tables::R_Baskethandle;
    t.R_Circ = xsect_tables::R_Circ;
    t.R_Egg = xsect_tables::R_Egg;
    t.R_HorizEllipse = xsect_tables::R_HorizEllipse;
    t.R_Horseshoe = xsect_tables::R_Horseshoe;
    t.R_VertEllipse = xsect_tables::R_VertEllipse;
    t.S_BasketHandle = xsect_tables::S_BasketHandle;
    t.S_Catenary = xsect_tables::S_Catenary;
    t.S_Circ = xsect_tables::S_Circ;
    t.S_Egg = xsect_tables::S_Egg;
    t.S_Gothic = xsect_tables::S_Gothic;
    t.S_Horseshoe = xsect_tables::S_Horseshoe;
    t.S_SemiCirc = xsect_tables::S_SemiCirc;
    t.S_SemiEllip = xsect_tables::S_SemiEllip;
    t.W_Arch = xsect_tables::W_Arch;
    t.W_BasketHandle = xsect_tables::W_BasketHandle;
    t.W_Catenary = xsect_tables::W_Catenary;
    t.W_Circ = xsect_tables::W_Circ;
    t.W_Egg = xsect_tables::W_Egg;
    t.W_Gothic = xsect_tables::W_Gothic;
    t.W_HorizEllipse = xsect_tables::W_HorizEllipse;
    t.W_Horseshoe = xsect_tables::W_Horseshoe;
    t.W_SemiCirc = xsect_tables::W_SemiCirc;
    t.W_SemiEllip = xsect_tables::W_SemiEllip;
    t.W_VertEllipse = xsect_tables::W_VertEllipse;
    t.Y_BasketHandle = xsect_tables::Y_BasketHandle;
    t.Y_Catenary = xsect_tables::Y_Catenary;
    t.Y_Circ = xsect_tables::Y_Circ;
    t.Y_Egg = xsect_tables::Y_Egg;
    t.Y_Gothic = xsect_tables::Y_Gothic;
    t.Y_Horseshoe = xsect_tables::Y_Horseshoe;
    t.Y_SemiCirc = xsect_tables::Y_SemiCirc;
    t.Y_SemiEllip = xsect_tables::Y_SemiEllip;
    t.Amax = xsect_tables::Amax;
    t.N_A_Arch = xsect_tables::N_A_Arch;
    t.N_A_Baskethandle = xsect_tables::N_A_Baskethandle;
    t.N_A_Circ = xsect_tables::N_A_Circ;
    t.N_A_Egg = xsect_tables::N_A_Egg;
    t.N_A_HorizEllipse = xsect_tables::N_A_HorizEllipse;
    t.N_A_Horseshoe = xsect_tables::N_A_Horseshoe;
    t.N_A_VertEllipse = xsect_tables::N_A_VertEllipse;
    t.N_R_Arch = xsect_tables::N_R_Arch;
    t.N_R_Baskethandle = xsect_tables::N_R_Baskethandle;
    t.N_R_Circ = xsect_tables::N_R_Circ;
    t.N_R_Egg = xsect_tables::N_R_Egg;
    t.N_R_HorizEllipse = xsect_tables::N_R_HorizEllipse;
    t.N_R_Horseshoe = xsect_tables::N_R_Horseshoe;
    t.N_R_VertEllipse = xsect_tables::N_R_VertEllipse;
    t.N_S_BasketHandle = xsect_tables::N_S_BasketHandle;
    t.N_S_Catenary = xsect_tables::N_S_Catenary;
    t.N_S_Circ = xsect_tables::N_S_Circ;
    t.N_S_Egg = xsect_tables::N_S_Egg;
    t.N_S_Gothic = xsect_tables::N_S_Gothic;
    t.N_S_Horseshoe = xsect_tables::N_S_Horseshoe;
    t.N_S_SemiCirc = xsect_tables::N_S_SemiCirc;
    t.N_S_SemiEllip = xsect_tables::N_S_SemiEllip;
    t.N_W_Arch = xsect_tables::N_W_Arch;
    t.N_W_BasketHandle = xsect_tables::N_W_BasketHandle;
    t.N_W_Catenary = xsect_tables::N_W_Catenary;
    t.N_W_Circ = xsect_tables::N_W_Circ;
    t.N_W_Egg = xsect_tables::N_W_Egg;
    t.N_W_Gothic = xsect_tables::N_W_Gothic;
    t.N_W_HorizEllipse = xsect_tables::N_W_HorizEllipse;
    t.N_W_Horseshoe = xsect_tables::N_W_Horseshoe;
    t.N_W_SemiCirc = xsect_tables::N_W_SemiCirc;
    t.N_W_SemiEllip = xsect_tables::N_W_SemiEllip;
    t.N_W_VertEllipse = xsect_tables::N_W_VertEllipse;
    t.N_Y_BasketHandle = xsect_tables::N_Y_BasketHandle;
    t.N_Y_Catenary = xsect_tables::N_Y_Catenary;
    t.N_Y_Circ = xsect_tables::N_Y_Circ;
    t.N_Y_Egg = xsect_tables::N_Y_Egg;
    t.N_Y_Gothic = xsect_tables::N_Y_Gothic;
    t.N_Y_Horseshoe = xsect_tables::N_Y_Horseshoe;
    t.N_Y_SemiCirc = xsect_tables::N_Y_SemiCirc;
    t.N_Y_SemiEllip = xsect_tables::N_Y_SemiEllip;
        return t;
    }();
    return t;
}

/// One host-bound evaluator, constructed once. Holding it in a function-local
/// static keeps the forwarders free of any per-call setup, and lets the
/// finite-volume geometry hold a pointer to it instead of a copy.
const XsectEval& hostEval() {
    static const XsectEval e{hostTables()};
    return e;
}

int    locate(double y, const double* table, int jLast)  { return hostEval().locate(y, table, jLast); }
double lookup(double x, const double* table, int n)      { return hostEval().lookup(x, table, n); }
double invLookup(double y, const double* table, int n)   { return hostEval().invLookup(y, table, n); }
double getYcircular(double alpha)                        { return hostEval().getYcircular(alpha); }
double getScircular(double alpha)                        { return hostEval().getScircular(alpha); }

double getAofY(const XSectParams& xs, double y)  { return hostEval().getAofY(xs, y); }
double getWofY(const XSectParams& xs, double y)  { return hostEval().getWofY(xs, y); }
double getRofY(const XSectParams& xs, double y)  { return hostEval().getRofY(xs, y); }
double getYofA(const XSectParams& xs, double a)  { return hostEval().getYofA(xs, a); }
double getSofA(const XSectParams& xs, double a)  { return hostEval().getSofA(xs, a); }
double getRofA(const XSectParams& xs, double a)  { return hostEval().getRofA(xs, a); }
double getdSdA(const XSectParams& xs, double a)  { return hostEval().getdSdA(xs, a); }
double getAofS(const XSectParams& xs, double s)  { return hostEval().getAofS(xs, s); }
double getAmax(const XSectParams& xs)            { return hostEval().getAmax(xs); }
double getYcrit(const XSectParams& xs, double q) { return hostEval().getYcrit(xs, q); }
bool   isOpen(int type)                          { return hostEval().isOpen(type); }


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
            xs.s_max = a_max * std::pow(hostEval().rect_closed_getRofA(xs, a_max), 2.0 / 3.0);
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
            xs.r_full = hostEval().parab_getRofY(xs, y);
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
            xs.r_full = hostEval().powerfunc_getRofY(xs, y);
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
            xs.a_bot  = hostEval().circ_getAofY(xs, xs.y_bot);
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
            xs.s_max = a_max * std::pow(hostEval().rect_triang_getRofA(xs, a_max), 2.0 / 3.0);
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
            xs.s_max = a_max * std::pow(hostEval().rect_round_getRofA(xs, a_max), 2.0 / 3.0);
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
