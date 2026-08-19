/**
 * @file test_xsect_boundary.cpp
 * @brief Exact arc/line boundary primitives — Green's theorem area/moment,
 *        height queries, evalExact, and construction (Phase 2 of the
 *        cross-section geometry compiler).
 *
 * @details Every numeric target here was verified independently (200 001-pt
 *          quadrature for the Green's-theorem formulas, hand-derived closed
 *          forms for the rectangle and circle) before this file was written,
 *          per the spec these primitives transcribe.
 *
 * @ingroup engine_hydraulics
 */

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <vector>

#include "hydraulics/XSectBoundary.hpp"

using namespace openswmm::xsboundary;

namespace {

constexpr double kPi = 3.14159265358979323846;

BElem makeArc(double cx, double cy, double r, double a0, double a1) {
    BElem e{};
    e.cx = cx; e.cy = cy; e.radius = r; e.a0 = a0; e.a1 = a1;
    e.x0 = cx + r * std::cos(a0); e.y0 = cy + r * std::sin(a0);
    e.x1 = cx + r * std::cos(a1); e.y1 = cy + r * std::sin(a1);
    return e;
}

BElem makeSeg(double x0, double y0, double x1, double y1) {
    BElem e{};
    e.x0 = x0; e.y0 = y0; e.x1 = x1; e.y1 = y1;
    return e;
}

/// Rectangle w x h, invert at y=0, CCW from (0,0).
std::vector<BElem> rectangle(double w, double h) {
    return {
        makeSeg(0, 0, w, 0),
        makeSeg(w, 0, w, h),
        makeSeg(w, h, 0, h),
        makeSeg(0, h, 0, 0),
    };
}

} // namespace

// ---------------------------------------------------------------------------
// Green's theorem — area
// ---------------------------------------------------------------------------

TEST(XSectBoundary, GreenAreaFullCircle) {
    // Full circle radius 2 centred (1,3): area = 4*pi, to 1e-14.
    const BElem e = makeArc(1.0, 3.0, 2.0, 0.0, 2.0 * kPi);
    EXPECT_NEAR(greenArea(e), 4.0 * kPi, 1.0e-14);
}

TEST(XSectBoundary, GreenAreaLowerHalfDisc) {
    // Lower half-disc radius 2, centred at origin: area = 2*pi.
    const BElem e = makeArc(0.0, 0.0, 2.0, kPi, 2.0 * kPi);
    EXPECT_NEAR(greenArea(e), 2.0 * kPi, 1.0e-14);
}

TEST(XSectBoundary, GreenAreaSegmentMatchesShoelace) {
    const BElem e = makeSeg(2.0, -1.0, 5.0, 3.0);
    EXPECT_DOUBLE_EQ(greenArea(e), 0.5 * (2.0 * 3.0 - 5.0 * -1.0));
}

// ---------------------------------------------------------------------------
// Green's theorem — first moment
// ---------------------------------------------------------------------------

TEST(XSectBoundary, GreenMomentYHalfDiscCentroid) {
    // Lower half-disc radius r centred at origin: ybar = -4r/(3*pi).
    const double r = 2.0;
    const BElem e = makeArc(0.0, 0.0, r, kPi, 2.0 * kPi);
    const double area = greenArea(e);
    const double ybar = greenMomentY(e) / area;
    EXPECT_NEAR(ybar, -4.0 * r / (3.0 * kPi), 1.0e-12);
}

TEST(XSectBoundary, GreenMomentYSegmentMatchesTrapezoid) {
    const BElem e = makeSeg(0.0, 1.0, 4.0, 5.0);
    const int m = 200001;
    double ref = 0.0;
    for (int i = 0; i < m; ++i) {
        const double t0 = static_cast<double>(i) / m;
        const double t1 = static_cast<double>(i + 1) / m;
        const double y0 = e.y0 + t0 * (e.y1 - e.y0);
        const double y1 = e.y0 + t1 * (e.y1 - e.y0);
        // -0.5 * integral of y^2 dx, dx = (x1-x0)/m per step.
        const double dx = (e.x1 - e.x0) / m;
        ref += -0.5 * dx * 0.5 * (y0 * y0 + y1 * y1);
    }
    EXPECT_NEAR(greenMomentY(e), ref, 1.0e-9 * std::fabs(ref));
}

// ---------------------------------------------------------------------------
// Rectangle — exact at several depths
// ---------------------------------------------------------------------------

TEST(XSectBoundary, RectangleExactPropertiesAtSeveralDepths) {
    // i stops short of 20 (y == h exactly) on purpose: at the flat rim,
    // crossingsAt's half-open convention reports no crossing on a HORIZONTAL
    // top edge (only a transversal wall crossing counts), so width would
    // read 0 instead of w there. That degenerate exact-rim height is exactly
    // what the critical-height analysis (Phase 3) exists to enumerate and
    // special-case; evalExact() is only claimed exact at a generic depth.
    const double w = 3.0, h = 5.0;
    const auto rect = rectangle(w, h);
    for (int i = 1; i <= 19; ++i) {
        const double y = h * static_cast<double>(i) / 20.0;
        const ExactProps p = evalExact(rect.data(), static_cast<int>(rect.size()), y);
        EXPECT_NEAR(p.area, w * y, 1.0e-13) << "at y=" << y;
        EXPECT_NEAR(p.width, w, 1.0e-13) << "at y=" << y;
        EXPECT_NEAR(p.perim, 2.0 * y + w, 1.0e-13) << "at y=" << y;
        EXPECT_NEAR(p.i1, w * y * y / 2.0, 1.0e-12) << "at y=" << y;
        EXPECT_EQ(p.ncomp, 1);
    }
}

// ---------------------------------------------------------------------------
// Arc vs polyline — a circle built one way should crush a circle built the
// coarse way.
// ---------------------------------------------------------------------------

TEST(XSectBoundary, ArcCircleMatchesAnalyticExactlyPolylineDoesNot) {
    const double r = 2.0, d = 2.0 * r;
    const BElem arc = makeArc(0.0, r, r, -0.5 * kPi, 1.5 * kPi);
    std::vector<BElem> chain = {arc};

    const double y = 0.5 * d;   // half-full: exact A = pi*r^2/2
    const ExactProps pa = evalExact(chain.data(), 1, y);
    const double analytic = 0.5 * kPi * r * r;
    EXPECT_NEAR(pa.area, analytic, 1.0e-13);

    // A 512-vertex polyline approximation of the same circle should show a
    // measurable (not 1e-13-level) discretization error.
    constexpr int nseg = 512;
    std::vector<double> px(nseg), py(nseg);
    for (int i = 0; i < nseg; ++i) {
        const double t = 2.0 * kPi * static_cast<double>(i) / nseg;
        px[i] = r * std::cos(t);
        py[i] = r + r * std::sin(t);
    }
    std::vector<BElem> poly;
    ASSERT_EQ(fromPolyline(px.data(), py.data(), nseg, poly), 0);
    const ExactProps pp = evalExact(poly.data(), static_cast<int>(poly.size()), y);
    const double poly_err = std::fabs(pp.area - analytic) / analytic;
    EXPECT_GT(poly_err, 1.0e-7);
    EXPECT_LT(poly_err, 1.0e-3);
}

// ---------------------------------------------------------------------------
// fromArcSpec — bulge construction
// ---------------------------------------------------------------------------

TEST(XSectBoundary, FourQuarterArcBulgesFormAFullCircle) {
    // Four points on a unit circle at 0, 90, 180, 270 degrees, each segment
    // bulging by tan(pi/8) (a 90-degree arc) traces the full circle CCW.
    const double b = std::tan(0.25 * kPi / 2.0);   // tan(theta/4), theta = pi/2
    const double x[4] = {1.0, 0.0, -1.0, 0.0};
    const double y[4] = {0.0, 1.0, 0.0, -1.0};
    const double bulge[4] = {b, b, b, b};

    std::vector<BElem> chain;
    ASSERT_EQ(fromArcSpec(x, y, bulge, 4, chain), 0);
    ASSERT_EQ(chain.size(), 4u);

    // Invert shifted to y=0: original min y was -1, so full depth is 2.
    const double y_full = 2.0;
    const ExactProps full = evalExact(chain.data(), 4, y_full);
    EXPECT_NEAR(full.area, kPi, 1.0e-10);
    EXPECT_NEAR(full.width, 0.0, 1.0e-9);   // top of circle: width -> 0

    // A GENERIC depth (not the y=1.0 equator, which after the shift lands
    // exactly on two of the four vertex heights at once — a critical-height
    // case that crossingsAt's half-open convention does not claim to handle;
    // that degeneracy is Phase 3's job). At y=1.3 the circle (centre (0,1),
    // r=1) has local height d=0.3 above its own centre; width and area below
    // it follow the standard circular-segment closed forms.
    const double d = 0.3;
    const ExactProps generic = evalExact(chain.data(), 4, 1.0 + d);
    const double width_ref = 2.0 * std::sqrt(1.0 - d * d);
    const double area_ref  = d * std::sqrt(1.0 - d * d) + std::asin(d) + 0.5 * kPi;
    EXPECT_NEAR(generic.width, width_ref, 1.0e-10);
    EXPECT_NEAR(generic.area, area_ref, 1.0e-10);
}

TEST(XSectBoundary, NegativeBulgeConcaveNotchStillGivesCorrectArea) {
    // A square with one edge replaced by a small CONCAVE (inward, negative
    // bulge) notch. The enclosed area must be the square's area minus the
    // notch's bulge area — this is the case that requires a1 < a0.
    const double s = 4.0;
    // Points CCW: (0,0) -> (4,0) [concave notch] -> (4,4) -> (0,4) -> close.
    const double x[4] = {0.0, 4.0, 4.0, 0.0};
    const double y[4] = {0.0, 0.0, 4.0, 4.0};
    const double b = -0.4142135623730951;   // tan(45deg/4)*(-1)-ish, |b|<1
    const double bulge[4] = {b, 0.0, 0.0, 0.0};

    std::vector<BElem> chain;
    ASSERT_EQ(fromArcSpec(x, y, bulge, 4, chain), 0);
    ASSERT_EQ(chain[0].radius > 0.0, true);
    EXPECT_LT(chain[0].a1, chain[0].a0) << "concave (negative-bulge) arc must have a1 < a0";

    const double theta = 4.0 * std::atan(b);   // negative
    const double c = 4.0;                      // chord length
    const double r = c / (2.0 * std::fabs(std::sin(0.5 * theta)));
    // Circular-segment area cut OUT by the notch (theta magnitude, minor segment).
    const double notch_area = 0.5 * r * r * (std::fabs(theta) - std::sin(std::fabs(theta)));

    const ExactProps full = evalExact(chain.data(), 4, s);
    EXPECT_NEAR(full.area, s * s - notch_area, 1.0e-9);
}

// ---------------------------------------------------------------------------
// Validation / error codes
// ---------------------------------------------------------------------------

TEST(XSectBoundary, RejectsTooFewPoints) {
    const double x[2] = {0, 1};
    const double y[2] = {0, 1};
    std::vector<BElem> out;
    EXPECT_EQ(fromPolyline(x, y, 2, out), 1);
}

TEST(XSectBoundary, RejectsSelfIntersectingBowtie) {
    // Bow-tie: (0,0) -> (4,4) -> (4,0) -> (0,4) -> close. Chords (0,0)-(4,4)
    // and (4,0)-(0,4) cross.
    const double x[4] = {0, 4, 4, 0};
    const double y[4] = {0, 4, 0, 4};
    std::vector<BElem> out;
    EXPECT_EQ(fromPolyline(x, y, 4, out), 2);
}

TEST(XSectBoundary, RejectsZeroArea) {
    // All points collinear.
    const double x[3] = {0, 1, 2};
    const double y[3] = {0, 0, 0};
    std::vector<BElem> out;
    EXPECT_EQ(fromPolyline(x, y, 3, out), 3);
}

TEST(XSectBoundary, RejectsNonFinite) {
    const double x[3] = {0, 1, std::numeric_limits<double>::quiet_NaN()};
    const double y[3] = {0, 1, 2};
    std::vector<BElem> out;
    EXPECT_EQ(fromPolyline(x, y, 3, out), 4);
}

TEST(XSectBoundary, RejectsBulgeMagnitudeAboveOne) {
    const double x[3] = {0, 1, 2};
    const double y[3] = {0, 1, 0};
    const double bulge[3] = {1.5, 0, 0};
    std::vector<BElem> out;
    EXPECT_EQ(fromArcSpec(x, y, bulge, 3, out), 7);
}

TEST(XSectBoundary, ClockwiseInputIsReorientedCCW) {
    // Same rectangle as `rectangle()` but wound CW.
    const double x[4] = {0, 0, 3, 3};
    const double y[4] = {0, 5, 5, 0};
    std::vector<BElem> out;
    ASSERT_EQ(fromPolyline(x, y, 4, out), 0);
    double area = 0.0;
    for (const BElem& e : out) area += greenArea(e);
    EXPECT_GT(area, 0.0);
    EXPECT_NEAR(area, 15.0, 1.0e-12);
}

TEST(XSectBoundary, ShiftsMinYToZeroIncludingArcInterior) {
    // A small triangle up high, (-1,5)-(1,5)-(0,6), with the base replaced by
    // a semicircle bulge (b=1) that dips DOWN to y=4 at its midpoint — below
    // every one of the three VERTEX y-values (5, 5, 6). The boundary's true
    // min y must come from that arc interior, not from min(y0,y1,y2).
    const double x[3] = {-1.0, 1.0, 0.0};
    const double y[3] = {5.0, 5.0, 6.0};
    const double bulge[3] = {1.0, 0.0, 0.0};
    std::vector<BElem> out;
    ASSERT_EQ(fromArcSpec(x, y, bulge, 3, out), 0);
    double y_min = std::numeric_limits<double>::infinity();
    for (const BElem& e : out) {
        y_min = std::min({y_min, e.y0, e.y1});
        if (e.radius != 0.0) y_min = std::min(y_min, e.cy - e.radius);
    }
    EXPECT_NEAR(y_min, 0.0, 1.0e-12);
}
