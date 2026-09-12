/**
 * @file test_fv_section_geometry.cpp
 * @brief Gates for the exact-geometry FV closure (plan
 *        FV1D_CLOSURE_KERNEL_PERF_PLAN_2026-09-11 §2c).
 *
 * @details Three layers, each verified against geometry rather than legacy:
 *          - SectionGeometry's closed forms are self-consistent (A = ∫W) and
 *            agree with the legacy full-depth properties;
 *          - the tabulated closure reproduces the exact section (with the slot
 *            folded in) to interpolation accuracy, is monotone, inverts to the
 *            closure tolerance and integrates to its own I₁; the polynomial
 *            class is exact;
 *          - the closure is a plain buffer: a byte copy at another address
 *            evaluates bit-identically (the device-capture contract).
 *          The legacy 51-row table's error against the exact section is
 *          recorded per shape, not asserted.
 *
 * @ingroup engine_fv
 */

#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#include "fv_test_support.hpp"
#include "hydraulics/fv/FvClosureKernels.hpp"
#include "hydraulics/fv/SectionGeometry.hpp"

using namespace fvtest;
namespace k = openswmm::fv::kernels;
namespace sg = openswmm::fv::secgeom;

namespace {

struct Case {
    const char* name;
    XSectShape  shape;
    double      p[4];
};

// Closed-form shapes (round, rectangular, open), then table/composite shapes.
// SWMM's elliptical pipes are the standard elliptical concrete-pipe profiles
// (table-defined), not mathematical ellipses: a true 3×4 ellipse has 17.5 %
// less area than the legacy HORIZ_ELLIPSE table at mid-depth and 21 % less
// when full — so they are table-defined here, deliberately.
const Case kClosedForm[] = {
    {"CIRCULAR",        XSectShape::CIRCULAR,        {3.0, 0.0, 0.0, 0.0}},
    {"FORCE_MAIN",      XSectShape::FORCE_MAIN,      {3.0, 130.0, 0.0, 0.0}},
    {"FILLED_CIRCULAR", XSectShape::FILLED_CIRCULAR, {3.0, 0.5, 0.0, 0.0}},
    {"RECT_CLOSED",     XSectShape::RECT_CLOSED,     {3.0, 2.0, 0.0, 0.0}},
    {"RECT_OPEN",       XSectShape::RECT_OPEN,       {5.0, 10.0, 0.0, 0.0}},
    {"TRAPEZOIDAL",     XSectShape::TRAPEZOIDAL,     {5.0, 4.0, 2.0, 2.0}},
    {"TRIANGULAR",      XSectShape::TRIANGULAR,      {5.0, 10.0, 0.0, 0.0}},
    {"PARABOLIC",       XSectShape::PARABOLIC,       {5.0, 10.0, 0.0, 0.0}},
    {"POWERFUNC",       XSectShape::POWERFUNC,       {5.0, 10.0, 2.0, 0.0}},
};
const Case kTableDefined[] = {
    {"HORIZ_ELLIPSE",   XSectShape::HORIZ_ELLIPSE,   {3.0, 4.0, 0.0, 0.0}},
    {"VERT_ELLIPSE",    XSectShape::VERT_ELLIPSE,    {3.0, 2.0, 0.0, 0.0}},
    {"EGGSHAPED",       XSectShape::EGGSHAPED,       {3.0, 0.0, 0.0, 0.0}},
    {"ARCH",            XSectShape::ARCH,            {3.0, 4.0, 0.0, 0.0}},
    {"RECT_TRIANG",     XSectShape::RECT_TRIANG,     {3.0, 2.0, 1.0, 0.0}},
    {"RECT_ROUND",      XSectShape::RECT_ROUND,      {3.0, 2.0, 1.5, 0.0}},
    {"MOD_BASKET",      XSectShape::MOD_BASKET,      {3.0, 2.0, 1.0, 0.0}},
    {"HORSESHOE",       XSectShape::HORSESHOE,       {3.0, 0.0, 0.0, 0.0}},
};
// Indices into kClosedForm used below.
constexpr int kIdxRectOpen = 4, kIdxTrapezoid = 5, kIdxTriangular = 6,
              kIdxParabolic = 7, kIdxPowerfunc = 8;

XSectParams params(const Case& c) {
    XSectParams xs;
    xs.type = static_cast<int>(c.shape);
    const int rc = xsect::setParams(xs, xs.type, c.p, 1.0);
    EXPECT_EQ(rc, 0) << c.name;
    return xs;
}

FvGeometry build(const Case& c, double celerity = 100.0, int barrels = 1) {
    const XSectParams xs = params(c);
    FvGeometry g;
    buildGeometry(xs, xsect::isOpen(xs.type), celerity, g, barrels);
    return g;
}

/// The exact section with the closure's slot taper folded in — what the
/// tabulated closure is supposed to reproduce (per cell, i.e. × barrels).
double expectedArea(const FvGeometry& g, double h) {
    double a = g.barrel_scale * sg::areaOfDepth(g.xs, h);
    const double band = g.y_full - g.y_crown;
    if (band > 0.0 && h > g.y_crown)
        a += g.t_slot * band * k::slotRampIntegral((h - g.y_crown) / band);
    return a;
}
double expectedWidth(const FvGeometry& g, double h) {
    double w = g.barrel_scale * sg::widthOfDepth(g.xs, h);
    const double band = g.y_full - g.y_crown;
    if (band > 0.0 && h > g.y_crown)
        w += g.t_slot * k::slotRamp((h - g.y_crown) / band);
    return w;
}

std::vector<Case> allCases() {
    std::vector<Case> v(std::begin(kClosedForm), std::end(kClosedForm));
    v.insert(v.end(), std::begin(kTableDefined), std::end(kTableDefined));
    return v;
}

} // namespace

// ---------------------------------------------------------------------------
// SectionGeometry — the closed forms against geometry
// ---------------------------------------------------------------------------

TEST(FvSectionGeometry, ClosedFormAreaIsTheIntegralOfWidth) {
    for (const Case& c : kClosedForm) {
        const XSectParams xs = params(c);
        ASSERT_TRUE(sg::hasClosedForm(xs.type)) << c.name;
        for (int i = 1; i <= 16; ++i) {
            const double h = xs.y_full * static_cast<double>(i) / 16.0;
            const double a = sg::areaOfDepth(xs, h);
            const double ia = sg::detail::integrate(
                [&](double y) { return sg::widthOfDepth(xs, y); }, 0.0, h,
                1.0e-13 * xs.a_full);
            EXPECT_NEAR(a, ia, 2.0e-9 * xs.a_full) << c.name << " h=" << h;
        }
    }
}

TEST(FvSectionGeometry, ClosedFormFullDepthMatchesTheLegacyFullProperties) {
    for (const Case& c : kClosedForm) {
        const XSectParams xs = params(c);
        const double a = sg::areaOfDepth(xs, xs.y_full);
        const double r = sg::hydRadOfDepth(xs, xs.y_full);
        // Legacy FILLED_CIRCULAR subtracts a TABLE-derived sediment area and
        // builds its full perimeter from the R table (measured: +1.1e-4 in
        // area, −5 % in R against the exact circle); POWERFUNC's legacy
        // perimeter is a 50-step polyline. Everything else agrees to legacy's
        // own π = 3.141592654.
        const bool filled = c.shape == XSectShape::FILLED_CIRCULAR;
        const bool power  = c.shape == XSectShape::POWERFUNC;
        EXPECT_NEAR(a / xs.a_full, 1.0, filled ? 1.0e-3 : 1.0e-9) << c.name;
        EXPECT_NEAR(r / xs.r_full, 1.0, filled ? 1.0e-1 : (power ? 2.0e-2 : 1.0e-6))
            << c.name << " R_full exact " << r << " legacy " << xs.r_full;
    }
}

// ---------------------------------------------------------------------------
// The closure — against the exact section
// ---------------------------------------------------------------------------

TEST(FvSectionGeometry, ClosureReproducesTheExactSection) {
    // Hermite with the exact width as node slope is O(dh⁴) on smooth panels,
    // but a round invert/crown behaves as h^{3/2}, which a cubic follows only
    // to O(dh^{3/2}) inside its first and last panel: measured 5e-5 of a_full
    // for the 3 ft circle at 128 panels (the legacy table: 7e-4), with the
    // width and R errors likewise confined to those two panels (W ∝ √h).
    // Flat-bottomed sections are exact. The thresholds below are those
    // measured envelopes; a graded first/last panel is the remediation if a
    // downstream gate ever needs more.
    for (const Case& c : allCases()) {
        const FvGeometry g = build(c);
        const bool closed_form = sg::hasClosedForm(g.xs.type);
        double max_a = 0.0, max_w = 0.0, max_r = 0.0;
        for (int i = 1; i < 4000; ++i) {
            const double h = g.y_full * static_cast<double>(i) / 4000.0;
            const double ea = expectedArea(g, h);
            max_a = std::max(max_a, std::fabs(k::areaOfDepth(g, h) - ea) / g.a_full);
            if (closed_form) {
                // The legacy width table is NOT dA/dh, so only closed-form
                // shapes have an exact width to compare against.
                const double ew = expectedWidth(g, h);
                const double er = sg::hydRadOfDepth(g.xs, h);
                max_w = std::max(max_w, std::fabs(k::widthOfDepth(g, h) - ew) / g.w_max);
                max_r = std::max(max_r, std::fabs(k::hydRadOfDepth(g, h) - er) / g.r_full);
            }
        }
        EXPECT_LT(max_a, closed_form ? 1.0e-4 : 5.0e-3) << c.name;
        if (closed_form) {
            EXPECT_LT(max_w, 5.0e-2) << c.name;
            // R is linear between nodes; the filled circle's R falls steeply
            // through its crown panel (measured 5.9 % there, friction-only).
            EXPECT_LT(max_r, c.shape == XSectShape::FILLED_CIRCULAR ? 1.0e-1 : 5.0e-2) << c.name;
        }
        std::printf("  closure-vs-exact %-16s max|dA|/a_full %.2e  max|dW|/w_max %.2e  max|dR|/r_full %.2e\n",
                    c.name, max_a, max_w, max_r);
    }
}

TEST(FvSectionGeometry, WidthIsTheDerivativeOfTheClosureArea) {
    // The property the legacy tables lacked: T ≡ dA/dh of the closure itself,
    // for every shape class, checked against a centred difference of the
    // closure's own A (O(δ²) on a cubic, so this is tight).
    for (const Case& c : allCases()) {
        const FvGeometry g = build(c);
        const double d = 1.0e-6 * g.y_full;
        const double dh = g.y_full / openswmm::fv::kClosurePanels;
        for (int i = 1; i < 2000; ++i) {
            const double h = 1.4 * g.y_full * static_cast<double>(i) / 2000.0;
            if (std::fabs(h - g.y_full) < 2.0 * d) continue;   // the crown kink
            // The table is C¹ at its panel nodes, not C²: a centred difference
            // straddling a node carries (δ/4)·[A''] and is not the derivative.
            const double frac = h / dh - std::floor(h / dh);
            if (h < g.y_full && (frac * dh < 2.0 * d || (1.0 - frac) * dh < 2.0 * d)) continue;
            const double fd = (k::areaOfDepth(g, h + d) - k::areaOfDepth(g, h - d)) / (2.0 * d);
            EXPECT_NEAR(k::widthOfDepth(g, h), fd, 1.0e-6 * g.w_max + 1.0e-9 * std::fabs(fd))
                << c.name << " h=" << h;
        }
    }
}

TEST(FvSectionGeometry, ClosureIsMonotoneWithNonNegativeWidth) {
    for (const Case& c : allCases()) {
        const FvGeometry g = build(c);
        double prev = -1.0;
        for (int i = 0; i <= 8000; ++i) {
            const double h = 1.5 * g.y_full * static_cast<double>(i) / 8000.0;
            const double a = k::areaOfDepth(g, h);
            ASSERT_GE(a, prev) << c.name << " at h=" << h;
            ASSERT_GE(k::widthOfDepth(g, h), 0.0) << c.name << " at h=" << h;
            if (h > 0.0) ASSERT_GT(a, prev) << c.name << " flat at h=" << h;
            prev = a;
        }
    }
}

TEST(FvSectionGeometry, DepthAreaRoundTripsToTheClosureTolerance) {
    // Dense (one sample per ~1/300 panel) plus a coherent random walk of the
    // kind the solver presents, because a Newton failure can be confined to a
    // sliver of one panel that a coarse sweep steps over.
    for (const Case& c : allCases()) {
        const FvGeometry g = build(c);
        double worst = 0.0, worst_h = 0.0;
        auto probe = [&](double h) {
            const double back = k::depthOfArea(g, k::areaOfDepth(g, h));
            const double e = std::fabs(back - h) / g.y_full;
            if (e > worst) { worst = e; worst_h = h; }
        };
        for (int i = 0; i <= 40000; ++i)
            probe(1.5 * g.y_full * static_cast<double>(i) / 40000.0);
        unsigned long long s = 0x9E3779B97F4A7C15ULL;
        double h = 0.4 * g.y_full;
        for (int i = 0; i < 20000; ++i) {
            s = s * 6364136223846793005ULL + 1442695040888963407ULL;
            const double u = static_cast<double>(s >> 11) * (1.0 / 9007199254740992.0);
            h += 0.02 * g.y_full * (2.0 * u - 1.0);
            if (h < 0.0) h = -h;
            if (h > 1.5 * g.y_full) h = 3.0 * g.y_full - h;
            probe(h);
        }
        EXPECT_LT(worst, 1.0e-12) << c.name << " worst at h=" << worst_h
                                  << " (panel " << worst_h / (g.y_full / openswmm::fv::kClosurePanels) << ")";
    }
}

TEST(FvSectionGeometry, I1IsTheAntiderivativeOfTheClosureArea) {
    // The reference integrates the closure PANEL BY PANEL: the table is a
    // piecewise cubic with a second-derivative kink at every node, and one
    // adaptive Simpson over [0, h] aliases those kinks in its error estimate
    // (measured: a constant 3e-6 offset on VERT_ELLIPSE while the per-panel
    // integrals agreed with the recursion to 4e-16).
    for (const Case& c : allCases()) {
        const FvGeometry g = build(c);
        const double dh = g.y_full / openswmm::fv::kClosurePanels;
        auto area = [&](double y) { return k::areaOfDepth(g, y); };
        for (int i = 1; i <= 12; ++i) {
            const double h = 1.3 * g.y_full * static_cast<double>(i) / 12.0;
            const double i1 = k::i1OfDepth(g, h, k::areaOfDepth(g, h));
            double ref = 0.0, lo = 0.0;
            while (lo < std::min(h, g.y_full) - 1.0e-15 * g.y_full) {
                const double hi = std::min(lo + dh, std::min(h, g.y_full));
                ref += sg::detail::integrate(area, lo, hi, 1.0e-15 * std::max(1.0, i1));
                lo = hi;
            }
            if (h > g.y_full)
                ref += sg::detail::integrate(area, g.y_full, h, 1.0e-15 * std::max(1.0, i1));
            EXPECT_NEAR(i1, ref, 1.0e-9 * std::max(1.0, ref)) << c.name << " h=" << h;
        }
    }
}

TEST(FvSectionGeometry, PolynomialClassIsExactAndInvertsInClosedForm) {
    const Case polys[] = {kClosedForm[kIdxRectOpen], kClosedForm[kIdxTrapezoid],
                          kClosedForm[kIdxTriangular]};
    for (const Case& c : polys) {
        const FvGeometry g = build(c);
        ASSERT_EQ(int(g.closure_tbl.kind), int(openswmm::fv::kClosurePolynomial)) << c.name;
        // From h > 0: at exactly zero depth the closure reports a dry width of
        // 0 (every class does), while the exact section's invert width is w.
        for (int i = 1; i <= 2000; ++i) {
            const double h = 1.5 * g.y_full * static_cast<double>(i) / 2000.0;
            const double a = k::areaOfDepth(g, h);
            if (h <= g.y_full) {
                EXPECT_DOUBLE_EQ(a, g.barrel_scale * sg::areaOfDepth(g.xs, h)) << c.name;
                EXPECT_DOUBLE_EQ(k::widthOfDepth(g, h), g.barrel_scale * sg::widthOfDepth(g.xs, h))
                    << c.name;
            }
            EXPECT_NEAR(k::depthOfArea(g, a), h, 1.0e-14 * g.y_full) << c.name << " h=" << h;
        }
    }
    // Parabolic and power sections are open but not polynomial: tabulated.
    EXPECT_EQ(build(kClosedForm[kIdxParabolic]).closure_tbl.kind, openswmm::fv::kClosureTabulated);
    EXPECT_EQ(build(kClosedForm[kIdxPowerfunc]).closure_tbl.kind, openswmm::fv::kClosureTabulated);
}

TEST(FvSectionGeometry, BarrelsScaleAreaAndWidthNotHydraulicRadius) {
    const FvGeometry g1 = build(kClosedForm[0], 100.0, 1);
    const FvGeometry g3 = build(kClosedForm[0], 100.0, 3);
    for (int i = 1; i < 200; ++i) {
        const double h = g1.y_full * static_cast<double>(i) / 200.0;
        EXPECT_NEAR(k::areaOfDepth(g3, h), 3.0 * k::areaOfDepth(g1, h), 1.0e-12 * g3.a_full);
        EXPECT_NEAR(k::hydRadOfDepth(g3, h), k::hydRadOfDepth(g1, h), 1.0e-12);
    }
}

// ---------------------------------------------------------------------------
// POD contract — a byte copy at another address evaluates bit-identically
// ---------------------------------------------------------------------------

TEST(FvSectionGeometry, ClosureIsAPlainBufferAnyBackendCanCapture) {
    static_assert(std::is_trivially_copyable_v<openswmm::fv::FvClosure>);
    static_assert(std::is_standard_layout_v<openswmm::fv::FvClosure>);
    for (const Case& c : allCases()) {
        const FvGeometry g = build(c);
        std::vector<unsigned char> bytes(sizeof(openswmm::fv::FvClosure));
        std::memcpy(bytes.data(), &g.closure_tbl, bytes.size());
        auto copy = std::make_unique<openswmm::fv::FvClosure>();
        std::memcpy(copy.get(), bytes.data(), bytes.size());
        for (int i = 0; i <= 1000; ++i) {
            const double h = 1.5 * g.y_full * static_cast<double>(i) / 1000.0;
            const auto e0 = k::closureEval(g.closure_tbl, h);
            const auto e1 = k::closureEval(*copy, h);
            EXPECT_EQ(e0.a, e1.a) << c.name;
            EXPECT_EQ(e0.t, e1.t) << c.name;
            EXPECT_EQ(e0.i1, e1.i1) << c.name;
            EXPECT_EQ(k::closureDepthOfArea(g.closure_tbl, e0.a),
                      k::closureDepthOfArea(*copy, e1.a)) << c.name;
            EXPECT_EQ(k::closureHydRad(g.closure_tbl, h), k::closureHydRad(*copy, h)) << c.name;
        }
    }
}

// ---------------------------------------------------------------------------
// Recorded, not asserted: how far the legacy table sat from the exact section
// ---------------------------------------------------------------------------

TEST(FvSectionGeometry, LegacyTableErrorAgainstTheExactSectionIsRecorded) {
    for (const Case& c : kClosedForm) {
        const XSectParams xs = params(c);
        double max_a = 0.0, max_w = 0.0, max_r = 0.0;
        for (int i = 1; i < 2000; ++i) {
            const double h = xs.y_full * static_cast<double>(i) / 2000.0;
            max_a = std::max(max_a, std::fabs(xsect::getAofY(xs, h) - sg::areaOfDepth(xs, h)) / xs.a_full);
            max_w = std::max(max_w, std::fabs(xsect::getWofY(xs, h) - sg::widthOfDepth(xs, h)) / xs.w_max);
            max_r = std::max(max_r, std::fabs(xsect::getRofY(xs, h) - sg::hydRadOfDepth(xs, h)) / xs.r_full);
        }
        std::printf("  legacy-vs-exact %-16s max|dA|/a_full %.2e  max|dW|/w_max %.2e  max|dR|/r_full %.2e\n",
                    c.name, max_a, max_w, max_r);
        EXPECT_TRUE(std::isfinite(max_a) && std::isfinite(max_w) && std::isfinite(max_r)) << c.name;
    }
}

