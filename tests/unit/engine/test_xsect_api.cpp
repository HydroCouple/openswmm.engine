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

/*!
 * \file   test_xsect_api.cpp
 * \author Caleb Buahin <caleb.buahin@gmail.com>
 * \date   2026
 * \license Apache-2.0
 * \brief  Standalone cross-section geometry C API (openswmm_xsect.h).
 *
 * Covers:
 *   1. ShapeEnumParity  — SWMM_XSectShape codes select the shape they name.
 *   2. Analytic         — closed-form shapes match textbook formulas exactly.
 *   3. Tabulated        — handle results == direct xsect:: calls (guards the
 *                         ABI→batch enum translation for every shape).
 *   4. Inverse          — depth_of_area / area_of_sectfactor round-trip.
 *   5. Units            — US vs SI agree after conversion.
 *   6. Irregular/Custom/Street — tabulated constructors.
 *   7. LinkBound        — swmm_link_create_xsect matches standalone, and the
 *                         handle survives the engine that produced it.
 *   8. Errors           — bad shape / negative / NULL / NaN rejected.
 *   9. Arrays           — array forms == scalar loop.
 */
#include <gtest/gtest.h>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_links.h>
#include <openswmm/engine/openswmm_model.h>
#include <openswmm/engine/openswmm_nodes.h>
#include <openswmm/engine/openswmm_xsect.h>

#include "data/LinkData.hpp"
#include "hydraulics/Link.hpp"
#include "hydraulics/XSectBatch.hpp"

#include <cmath>
#include <vector>

namespace {

// The engine deliberately uses legacy SWMM's truncated PI literal (consts.h)
// rather than a full-precision one, because every derived quantity is pinned to
// legacy bit-parity. Analytic expectations must use the same literal or they
// disagree in the 10th significant digit.
constexpr double kPi = 3.141592654;

/// RAII wrapper so a failed assertion cannot leak a handle.
class Xs {
public:
    explicit Xs(SWMM_XSect h = nullptr) : h_(h) {}
    ~Xs() { swmm_xsect_free(h_); }
    Xs(const Xs&) = delete;
    Xs& operator=(const Xs&) = delete;
    SWMM_XSect get() const { return h_; }
    SWMM_XSect* out() { return &h_; }
private:
    SWMM_XSect h_;
};

double areaAt(SWMM_XSect h, double y) {
    double a = -1.0;
    EXPECT_EQ(swmm_xsect_area_of_depth(h, y, &a), SWMM_OK);
    return a;
}
double widthAt(SWMM_XSect h, double y) {
    double w = -1.0;
    EXPECT_EQ(swmm_xsect_width_of_depth(h, y, &w), SWMM_OK);
    return w;
}
double hydradAt(SWMM_XSect h, double y) {
    double r = -1.0;
    EXPECT_EQ(swmm_xsect_hydrad_of_depth(h, y, &r), SWMM_OK);
    return r;
}
double yFull(SWMM_XSect h) {
    double y = 0;
    EXPECT_EQ(swmm_xsect_full_properties(h, &y, nullptr, nullptr, nullptr,
                                         nullptr, nullptr), SWMM_OK);
    return y;
}

} // namespace

// ===========================================================================
// 1. Shape-enum parity
// ===========================================================================
//
// The regression guard for the pre-6.0 defect where SWMM_XSectShape and the
// engine's storage codes disagreed for every code >= 8, so e.g.
// SWMM_XSECT_IRREGULAR silently produced a vertical ellipse. The static_asserts
// in openswmm_links_impl.cpp make that unbuildable; this proves it end-to-end
// through the public API.

TEST(XSectShapeEnumParity, PublishedCodeSelectsTheShapeItNames)
{
    struct Case { int abi; openswmm::XsectShape stored; };
    const Case cases[] = {
        {SWMM_XSECT_CIRCULAR,        openswmm::XsectShape::CIRCULAR},
        {SWMM_XSECT_FILLED_CIRCULAR, openswmm::XsectShape::FILLED_CIRCULAR},
        {SWMM_XSECT_RECT_CLOSED,     openswmm::XsectShape::RECT_CLOSED},
        {SWMM_XSECT_RECT_OPEN,       openswmm::XsectShape::RECT_OPEN},
        {SWMM_XSECT_TRAPEZOIDAL,     openswmm::XsectShape::TRAPEZOIDAL},
        {SWMM_XSECT_TRIANGULAR,      openswmm::XsectShape::TRIANGULAR},
        {SWMM_XSECT_PARABOLIC,       openswmm::XsectShape::PARABOLIC},
        {SWMM_XSECT_POWER,           openswmm::XsectShape::POWER},
        {SWMM_XSECT_MOD_BASKET,      openswmm::XsectShape::MODBASKETHANDLE},
        {SWMM_XSECT_EGGSHAPED,       openswmm::XsectShape::EGGSHAPED},
        {SWMM_XSECT_HORSESHOE,       openswmm::XsectShape::HORSESHOE},
        {SWMM_XSECT_GOTHIC,          openswmm::XsectShape::GOTHIC},
        {SWMM_XSECT_CATENARY,        openswmm::XsectShape::CATENARY},
        {SWMM_XSECT_SEMIELLIPTICAL,  openswmm::XsectShape::SEMIELLIPTICAL},
        {SWMM_XSECT_BASKETHANDLE,    openswmm::XsectShape::BASKETHANDLE},
        {SWMM_XSECT_SEMICIRCULAR,    openswmm::XsectShape::SEMICIRCULAR},
        {SWMM_XSECT_RECT_TRIANG,     openswmm::XsectShape::RECT_TRIANG},
        {SWMM_XSECT_RECT_ROUND,      openswmm::XsectShape::RECT_ROUND},
        {SWMM_XSECT_HORIZ_ELLIPSE,   openswmm::XsectShape::HORIZ_ELLIPSE},
        {SWMM_XSECT_VERT_ELLIPSE,    openswmm::XsectShape::VERT_ELLIPSE},
        {SWMM_XSECT_ARCH,            openswmm::XsectShape::ARCH},
        {SWMM_XSECT_IRREGULAR,       openswmm::XsectShape::IRREGULAR},
        {SWMM_XSECT_CUSTOM,          openswmm::XsectShape::CUSTOM},
        {SWMM_XSECT_FORCE_MAIN,      openswmm::XsectShape::FORCE_MAIN},
        {SWMM_XSECT_STREET,          openswmm::XsectShape::STREET_XSECT},
        {SWMM_XSECT_DUMMY,           openswmm::XsectShape::DUMMY},
    };
    ASSERT_EQ(std::size(cases), 26u) << "every XsectShape must be pinned here";

    for (const auto& c : cases) {
        EXPECT_EQ(c.abi, static_cast<int>(c.stored))
            << "ABI code " << c.abi << " does not match its storage code — a "
               "model built through the C API would get the wrong geometry";
        // The name lookup must agree with the same code.
        const char* nm = swmm_xsect_shape_name(c.abi);
        ASSERT_NE(nm, nullptr) << "no name for code " << c.abi;
    }
}

TEST(XSectShapeEnumParity, ShapeNameRejectsOutOfRange)
{
    EXPECT_STREQ(swmm_xsect_shape_name(SWMM_XSECT_CIRCULAR), "CIRCULAR");
    EXPECT_STREQ(swmm_xsect_shape_name(SWMM_XSECT_IRREGULAR), "IRREGULAR");
    // POLYGON=26 (Phase 5) extended the valid range by one.
    EXPECT_STREQ(swmm_xsect_shape_name(SWMM_XSECT_POLYGON), "POLYGON");
    EXPECT_EQ(swmm_xsect_shape_name(-1), nullptr);
    EXPECT_EQ(swmm_xsect_shape_name(27), nullptr);
}

// ===========================================================================
// 2. Analytic shapes
// ===========================================================================

TEST(XSectAnalytic, CircularMatchesClosedForm)
{
    const double d = 2.0;
    Xs xs;
    ASSERT_EQ(swmm_xsect_create(SWMM_XSECT_CIRCULAR, d, 0, 0, 0,
                                SWMM_UNITS_US, xs.out()), SWMM_OK);

    double y_full = 0, a_full = 0, r_full = 0, w_max = 0;
    ASSERT_EQ(swmm_xsect_full_properties(xs.get(), &y_full, &a_full, &r_full,
                                         &w_max, nullptr, nullptr), SWMM_OK);
    EXPECT_DOUBLE_EQ(y_full, d);
    EXPECT_DOUBLE_EQ(a_full, kPi / 4.0 * d * d);
    EXPECT_DOUBLE_EQ(r_full, d / 4.0);
    EXPECT_DOUBLE_EQ(w_max, d);

    // Widest point is the springline; a half-full pipe is half the full area.
    EXPECT_NEAR(widthAt(xs.get(), d / 2.0), d, 1e-9);
    EXPECT_NEAR(areaAt(xs.get(), d / 2.0), a_full / 2.0, 1e-4 * a_full);

    // Closed shape: not open at the top.
    int open = -1;
    ASSERT_EQ(swmm_xsect_is_open(xs.get(), &open), SWMM_OK);
    EXPECT_EQ(open, 0);
}

TEST(XSectAnalytic, RectangularIsExact)
{
    const double h = 3.0, w = 5.0;
    Xs closed, open;
    ASSERT_EQ(swmm_xsect_create(SWMM_XSECT_RECT_CLOSED, h, w, 0, 0,
                                SWMM_UNITS_US, closed.out()), SWMM_OK);
    ASSERT_EQ(swmm_xsect_create(SWMM_XSECT_RECT_OPEN, h, w, 0, 0,
                                SWMM_UNITS_US, open.out()), SWMM_OK);

    for (double y : {0.5, 1.5, 3.0}) {
        EXPECT_DOUBLE_EQ(areaAt(closed.get(), y), y * w) << "y=" << y;
    }
    // Top width is constant below the crown...
    for (double y : {0.5, 1.5, 2.99}) {
        EXPECT_DOUBLE_EQ(widthAt(closed.get(), y), w) << "y=" << y;
    }
    // ...but a CLOSED conduit has no free surface exactly at full depth, so its
    // top width there is zero, not w. The routing solvers depend on this to
    // detect surcharge, so it is contract, not an edge-case artifact.
    EXPECT_DOUBLE_EQ(widthAt(closed.get(), h), 0.0);
    EXPECT_DOUBLE_EQ(widthAt(open.get(), h), w) << "an open channel keeps its width";

    // R = A / P, P = 2y + w for an open rectangle at depth y.
    EXPECT_DOUBLE_EQ(hydradAt(open.get(), 1.5), (1.5 * w) / (2.0 * 1.5 + w));

    int o = -1;
    ASSERT_EQ(swmm_xsect_is_open(open.get(), &o), SWMM_OK);
    EXPECT_EQ(o, 1);
    ASSERT_EQ(swmm_xsect_is_open(closed.get(), &o), SWMM_OK);
    EXPECT_EQ(o, 0);
}

TEST(XSectAnalytic, TrapezoidUsesBothSideSlopes)
{
    // geom: height, bottom width, left slope, right slope.
    const double h = 4.0, bw = 2.0, m1 = 1.0, m2 = 3.0;
    Xs xs;
    ASSERT_EQ(swmm_xsect_create(SWMM_XSECT_TRAPEZOIDAL, h, bw, m1, m2,
                                SWMM_UNITS_US, xs.out()), SWMM_OK);

    // A(y) = (bw + m_avg*y)*y with m_avg = (m1+m2)/2; T(y) = bw + (m1+m2)*y.
    const double y = 2.5, m_avg = (m1 + m2) / 2.0;
    EXPECT_DOUBLE_EQ(areaAt(xs.get(), y), (bw + m_avg * y) * y);
    EXPECT_DOUBLE_EQ(widthAt(xs.get(), y), bw + (m1 + m2) * y);

    // A rectangle-equivalent check: with both slopes zero it degenerates.
    Xs rectish;
    ASSERT_EQ(swmm_xsect_create(SWMM_XSECT_TRAPEZOIDAL, h, bw, 0.0, 0.0,
                                SWMM_UNITS_US, rectish.out()), SWMM_OK);
    EXPECT_DOUBLE_EQ(areaAt(rectish.get(), y), bw * y);
}

TEST(XSectAnalytic, TriangularAndParabolic)
{
    const double h = 2.0, tw = 6.0;
    Xs tri;
    ASSERT_EQ(swmm_xsect_create(SWMM_XSECT_TRIANGULAR, h, tw, 0, 0,
                                SWMM_UNITS_US, tri.out()), SWMM_OK);
    // Full triangle: A = 0.5 * base * height.
    EXPECT_DOUBLE_EQ(areaAt(tri.get(), h), 0.5 * tw * h);
    // Width scales linearly with depth.
    EXPECT_DOUBLE_EQ(widthAt(tri.get(), h / 2.0), tw / 2.0);

    Xs par;
    ASSERT_EQ(swmm_xsect_create(SWMM_XSECT_PARABOLIC, h, tw, 0, 0,
                                SWMM_UNITS_US, par.out()), SWMM_OK);
    // Parabola: A = (2/3) * T * y at full depth.
    EXPECT_NEAR(areaAt(par.get(), h), 2.0 / 3.0 * tw * h, 1e-9);
}

TEST(XSectAnalytic, ForceMainIsGeometricallyCircular)
{
    const double d = 1.5;
    Xs circ, fm;
    ASSERT_EQ(swmm_xsect_create(SWMM_XSECT_CIRCULAR, d, 0, 0, 0,
                                SWMM_UNITS_US, circ.out()), SWMM_OK);
    // geom2 is the Hazen-Williams C factor, not a length.
    ASSERT_EQ(swmm_xsect_create(SWMM_XSECT_FORCE_MAIN, d, 130.0, 0, 0,
                                SWMM_UNITS_US, fm.out()), SWMM_OK);

    for (double y : {0.2, 0.75, 1.5}) {
        EXPECT_DOUBLE_EQ(areaAt(fm.get(), y), areaAt(circ.get(), y)) << "y=" << y;
        EXPECT_DOUBLE_EQ(widthAt(fm.get(), y), widthAt(circ.get(), y)) << "y=" << y;
    }
}

TEST(XSectAnalytic, DummyIsAllZeros)
{
    Xs xs;
    ASSERT_EQ(swmm_xsect_create(SWMM_XSECT_DUMMY, 0, 0, 0, 0,
                                SWMM_UNITS_US, xs.out()), SWMM_OK);
    EXPECT_DOUBLE_EQ(areaAt(xs.get(), 1.0), 0.0);
    EXPECT_DOUBLE_EQ(widthAt(xs.get(), 1.0), 0.0);
    EXPECT_DOUBLE_EQ(hydradAt(xs.get(), 1.0), 0.0);
    double yc = -1;
    ASSERT_EQ(swmm_xsect_critical_depth(xs.get(), 5.0, &yc), SWMM_OK);
    EXPECT_DOUBLE_EQ(yc, 0.0);
}

// ===========================================================================
// 3. Tabulated shapes vs the kernels directly
// ===========================================================================
//
// Every closed shape whose geometry comes from a static lookup table. Building
// the reference through xsect::setParams with link::translateShape is what
// catches a wrong ABI→batch mapping: if the handle used the wrong table the
// numbers diverge immediately.

TEST(XSectTabulated, EveryStaticTableShapeMatchesTheKernel)
{
    const int shapes[] = {
        SWMM_XSECT_FILLED_CIRCULAR, SWMM_XSECT_MOD_BASKET, SWMM_XSECT_EGGSHAPED,
        SWMM_XSECT_HORSESHOE, SWMM_XSECT_GOTHIC, SWMM_XSECT_CATENARY,
        SWMM_XSECT_SEMIELLIPTICAL, SWMM_XSECT_BASKETHANDLE,
        SWMM_XSECT_SEMICIRCULAR, SWMM_XSECT_RECT_TRIANG, SWMM_XSECT_RECT_ROUND,
        SWMM_XSECT_HORIZ_ELLIPSE, SWMM_XSECT_VERT_ELLIPSE, SWMM_XSECT_ARCH,
    };

    for (int shape : shapes) {
        // geom2/geom3 are only meaningful for some shapes; these values are
        // valid for all of them (fill/width below the crown, positive radius).
        const double g1 = 4.0, g2 = 2.0, g3 = 1.0;
        Xs xs;
        ASSERT_EQ(swmm_xsect_create(shape, g1, g2, g3, 0.0,
                                    SWMM_UNITS_US, xs.out()), SWMM_OK)
            << "shape " << swmm_xsect_shape_name(shape);

        // Reference: the kernel path the routing solvers use.
        openswmm::XSectParams ref{};
        const double p[4] = {g1, g2, g3, 0.0};
        const int rc = openswmm::xsect::setParams(
            ref, openswmm::link::translateShape(
                     static_cast<openswmm::XsectShape>(shape)),
            p, /*ucf=*/1.0);
        ASSERT_EQ(rc, 0) << "shape " << swmm_xsect_shape_name(shape);

        const double yf = yFull(xs.get());
        ASSERT_GT(yf, 0.0) << "shape " << swmm_xsect_shape_name(shape);

        for (double frac : {0.05, 0.25, 0.5, 0.75, 0.99, 1.0}) {
            const double y = frac * yf;
            EXPECT_DOUBLE_EQ(areaAt(xs.get(), y), openswmm::xsect::getAofY(ref, y))
                << swmm_xsect_shape_name(shape) << " area @ y/yf=" << frac;
            EXPECT_DOUBLE_EQ(widthAt(xs.get(), y), openswmm::xsect::getWofY(ref, y))
                << swmm_xsect_shape_name(shape) << " width @ y/yf=" << frac;
            EXPECT_DOUBLE_EQ(hydradAt(xs.get(), y), openswmm::xsect::getRofY(ref, y))
                << swmm_xsect_shape_name(shape) << " hydrad @ y/yf=" << frac;
        }
    }
}

// ===========================================================================
// 4. Inverses
// ===========================================================================

TEST(XSectInverse, DepthOfAreaRoundTripsForClosedFormShapes)
{
    // Shapes with an analytic inverse — these round-trip to solver precision.
    const int shapes[] = {SWMM_XSECT_CIRCULAR, SWMM_XSECT_RECT_CLOSED,
                          SWMM_XSECT_TRAPEZOIDAL, SWMM_XSECT_TRIANGULAR};
    for (int shape : shapes) {
        Xs xs;
        ASSERT_EQ(swmm_xsect_create(shape, 3.0, 2.0, 1.0, 1.0,
                                    SWMM_UNITS_US, xs.out()), SWMM_OK);
        const double yf = yFull(xs.get());
        for (double frac : {0.1, 0.3, 0.5, 0.8, 0.95}) {
            const double y = frac * yf;
            const double a = areaAt(xs.get(), y);
            double back = -1;
            ASSERT_EQ(swmm_xsect_depth_of_area(xs.get(), a, &back), SWMM_OK);
            EXPECT_NEAR(back, y, 1e-3 * yf)
                << swmm_xsect_shape_name(shape) << " @ y/yf=" << frac;
        }
    }
}

TEST(XSectInverse, TabulatedInverseIsBitIdenticalToTheKernel)
{
    // A tabulated shape's inverse goes through invLookup on a coarse normalized
    // table, so getYofA(getAofY(y)) drifts from y by ~1.5% of y_full. That is
    // legacy SWMM's behaviour, inherited deliberately — this API's contract is
    // to REPRODUCE the engine exactly, not to be more accurate than it. So the
    // assertion is exact agreement with the kernel, not closeness to y.
    const double g1 = 3.0;
    Xs xs;
    ASSERT_EQ(swmm_xsect_create(SWMM_XSECT_EGGSHAPED, g1, 0, 0, 0,
                                SWMM_UNITS_US, xs.out()), SWMM_OK);

    openswmm::XSectParams ref{};
    const double p[4] = {g1, 0, 0, 0};
    ASSERT_EQ(openswmm::xsect::setParams(
                  ref, openswmm::link::translateShape(
                           openswmm::XsectShape::EGGSHAPED), p, 1.0), 0);

    for (double frac : {0.1, 0.3, 0.5, 0.8, 0.95}) {
        const double y = frac * g1;
        const double a = areaAt(xs.get(), y);
        double back = -1;
        ASSERT_EQ(swmm_xsect_depth_of_area(xs.get(), a, &back), SWMM_OK);
        EXPECT_DOUBLE_EQ(back, openswmm::xsect::getYofA(ref, a))
            << "@ y/yf=" << frac;
    }
}

TEST(XSectInverse, AreaOfSectionFactorRoundTrips)
{
    Xs xs;
    ASSERT_EQ(swmm_xsect_create(SWMM_XSECT_CIRCULAR, 2.0, 0, 0, 0,
                                SWMM_UNITS_US, xs.out()), SWMM_OK);
    const double a_in = areaAt(xs.get(), 0.8);
    double sf = -1, a_back = -1;
    ASSERT_EQ(swmm_xsect_sectfactor_of_area(xs.get(), a_in, &sf), SWMM_OK);
    ASSERT_GT(sf, 0.0);
    ASSERT_EQ(swmm_xsect_area_of_sectfactor(xs.get(), sf, &a_back), SWMM_OK);
    EXPECT_NEAR(a_back, a_in, 1e-3 * a_in);
}

TEST(XSectInverse, CriticalDepthRisesWithFlow)
{
    Xs xs;
    ASSERT_EQ(swmm_xsect_create(SWMM_XSECT_CIRCULAR, 3.0, 0, 0, 0,
                                SWMM_UNITS_US, xs.out()), SWMM_OK);
    double prev = -1.0;
    for (double q : {0.5, 2.0, 8.0, 20.0}) {
        double yc = -1;
        ASSERT_EQ(swmm_xsect_critical_depth(xs.get(), q, &yc), SWMM_OK);
        EXPECT_GT(yc, prev) << "critical depth must increase with flow, q=" << q;
        prev = yc;
    }
    // Zero flow → zero critical depth.
    double yc0 = -1;
    ASSERT_EQ(swmm_xsect_critical_depth(xs.get(), 0.0, &yc0), SWMM_OK);
    EXPECT_DOUBLE_EQ(yc0, 0.0);
}

TEST(XSectInverse, RectangularCriticalDepthMatchesClosedForm)
{
    // yc = (q^2 / (g * b^2))^(1/3) for a rectangular channel.
    const double b = 4.0, q = 30.0, g = 32.2;
    Xs xs;
    ASSERT_EQ(swmm_xsect_create(SWMM_XSECT_RECT_OPEN, 10.0, b, 0, 0,
                                SWMM_UNITS_US, xs.out()), SWMM_OK);
    double yc = -1;
    ASSERT_EQ(swmm_xsect_critical_depth(xs.get(), q, &yc), SWMM_OK);
    EXPECT_NEAR(yc, std::cbrt(q * q / (g * b * b)), 1e-3);
}

// ===========================================================================
// 5. Units
// ===========================================================================

TEST(XSectUnits, UsAndSiAgreeAfterConversion)
{
    const double d_m = 1.0;
    const double m_per_ft = 0.3048;
    Xs si, us;
    ASSERT_EQ(swmm_xsect_create(SWMM_XSECT_CIRCULAR, d_m, 0, 0, 0,
                                SWMM_UNITS_SI, si.out()), SWMM_OK);
    ASSERT_EQ(swmm_xsect_create(SWMM_XSECT_CIRCULAR, d_m / m_per_ft, 0, 0, 0,
                                SWMM_UNITS_US, us.out()), SWMM_OK);

    // Same physical pipe: SI area (m²) == US area (ft²) × 0.3048².
    const double a_si = areaAt(si.get(), 0.5);
    const double a_us = areaAt(us.get(), 0.5 / m_per_ft);
    EXPECT_NEAR(a_si, a_us * m_per_ft * m_per_ft, 1e-9);

    // And the SI answer is the textbook one for a half-full 1 m pipe.
    EXPECT_NEAR(a_si, kPi / 8.0 * d_m * d_m, 1e-4);

    int us_sys = -1, fu = -1;
    ASSERT_EQ(swmm_xsect_get_units(si.get(), &us_sys, &fu), SWMM_OK);
    EXPECT_EQ(us_sys, SWMM_UNITS_SI);
    EXPECT_EQ(fu, 3) << "a standalone SI handle reports flows in CMS";
    ASSERT_EQ(swmm_xsect_get_units(us.get(), &us_sys, &fu), SWMM_OK);
    EXPECT_EQ(us_sys, SWMM_UNITS_US);
    EXPECT_EQ(fu, 0) << "a standalone US handle reports flows in CFS";
}

TEST(XSectUnits, SectionFactorConvertsAsLengthToTheEightThirds)
{
    const double m_per_ft = 0.3048;
    Xs si, us;
    ASSERT_EQ(swmm_xsect_create(SWMM_XSECT_RECT_OPEN, 2.0, 3.0, 0, 0,
                                SWMM_UNITS_SI, si.out()), SWMM_OK);
    ASSERT_EQ(swmm_xsect_create(SWMM_XSECT_RECT_OPEN, 2.0 / m_per_ft,
                                3.0 / m_per_ft, 0, 0,
                                SWMM_UNITS_US, us.out()), SWMM_OK);
    double sf_si = 0, sf_us = 0;
    ASSERT_EQ(swmm_xsect_full_properties(si.get(), nullptr, nullptr, nullptr,
                                         nullptr, &sf_si, nullptr), SWMM_OK);
    ASSERT_EQ(swmm_xsect_full_properties(us.get(), nullptr, nullptr, nullptr,
                                         nullptr, &sf_us, nullptr), SWMM_OK);
    const double f = std::pow(m_per_ft, 8.0 / 3.0);
    EXPECT_NEAR(sf_si, sf_us * f, 1e-9 * std::max(1.0, sf_si));
}

// ===========================================================================
// 6. Irregular / custom / street
// ===========================================================================

TEST(XSectIrregular, TrapezoidTransectApproximatesTheAnalyticTrapezoid)
{
    // A symmetric trapezoid: 2 ft bottom at elev 0, 1:1 banks rising 4 ft.
    const double st[] = {0.0, 4.0, 6.0, 10.0};
    const double el[] = {4.0, 0.0, 0.0,  4.0};
    Xs irr;
    ASSERT_EQ(swmm_xsect_create_irregular(st, el, 4,
                                          /*x_left_bank=*/4.0,
                                          /*x_right_bank=*/6.0,
                                          /*n_left=*/0.03, /*n_channel=*/0.03,
                                          /*n_right=*/0.03, /*length_factor=*/1.0,
                                          SWMM_UNITS_US, irr.out()), SWMM_OK);

    double y_full = 0, w_max = 0;
    ASSERT_EQ(swmm_xsect_full_properties(irr.get(), &y_full, nullptr, nullptr,
                                         &w_max, nullptr, nullptr), SWMM_OK);
    EXPECT_NEAR(y_full, 4.0, 1e-6);
    EXPECT_NEAR(w_max, 10.0, 1e-6);

    // Analytic trapezoid, same geometry: bottom 2, both slopes 1:1, height 4.
    Xs tz;
    ASSERT_EQ(swmm_xsect_create(SWMM_XSECT_TRAPEZOIDAL, 4.0, 2.0, 1.0, 1.0,
                                SWMM_UNITS_US, tz.out()), SWMM_OK);

    // The transect is a 51-entry table, so agreement is to interpolation
    // tolerance rather than exact.
    for (double y : {1.0, 2.0, 3.0, 4.0}) {
        EXPECT_NEAR(areaAt(irr.get(), y), areaAt(tz.get(), y), 0.05 * areaAt(tz.get(), y))
            << "area @ y=" << y;
        EXPECT_NEAR(widthAt(irr.get(), y), widthAt(tz.get(), y), 0.05 * widthAt(tz.get(), y))
            << "width @ y=" << y;
    }

    int open = -1;
    ASSERT_EQ(swmm_xsect_is_open(irr.get(), &open), SWMM_OK);
    EXPECT_EQ(open, 1) << "a natural channel is open at the top";
}

TEST(XSectIrregular, RoughnessChangesTheHydraulicRadiusTable)
{
    // Same geometry, different overbank roughness → the conveyance-weighted
    // hydraulic radius must differ. This is why the constructor takes n at all.
    const double st[] = {0.0, 4.0, 6.0, 10.0};
    const double el[] = {4.0, 0.0, 0.0,  4.0};
    Xs uniform, rough;
    ASSERT_EQ(swmm_xsect_create_irregular(st, el, 4, 4.0, 6.0,
                                          0.03, 0.03, 0.03, 1.0,
                                          SWMM_UNITS_US, uniform.out()), SWMM_OK);
    ASSERT_EQ(swmm_xsect_create_irregular(st, el, 4, 4.0, 6.0,
                                          0.15, 0.03, 0.15, 1.0,
                                          SWMM_UNITS_US, rough.out()), SWMM_OK);

    // Areas are pure geometry — identical.
    EXPECT_NEAR(areaAt(uniform.get(), 3.0), areaAt(rough.get(), 3.0), 1e-9);
    // Hydraulic radius is roughness-weighted — different.
    EXPECT_NE(hydradAt(uniform.get(), 3.0), hydradAt(rough.get(), 3.0));
}

TEST(XSectCustom, ShapeCurveScalesToFullDepth)
{
    // A "rectangular" shape curve: constant width at every depth.
    const double cx[] = {0.0, 0.5, 1.0};
    const double cy[] = {1.0, 1.0, 1.0};
    const double y_full = 5.0;
    Xs cs;
    ASSERT_EQ(swmm_xsect_create_custom(y_full, cx, cy, 3,
                                       SWMM_UNITS_US, cs.out()), SWMM_OK);
    double yf = 0, w_max = 0;
    ASSERT_EQ(swmm_xsect_full_properties(cs.get(), &yf, nullptr, nullptr,
                                         &w_max, nullptr, nullptr), SWMM_OK);
    EXPECT_NEAR(yf, y_full, 1e-9);
    // Width is constant with depth for this curve.
    EXPECT_NEAR(widthAt(cs.get(), 1.0), widthAt(cs.get(), 4.0), 1e-6);

    int shape = -1;
    ASSERT_EQ(swmm_xsect_get_shape(cs.get(), &shape), SWMM_OK);
    EXPECT_EQ(shape, SWMM_XSECT_CUSTOM);
}

TEST(XSectStreet, FullDepthIsTheHigherOfCurbAndCrown)
{
    // A street's transect runs curb-top → gutter → crown → gutter → curb-top,
    // so its full depth is whichever of the curb or the crown stands higher
    // above the gutter. Crown rise = slope × (width - gutter_width).
    const double width = 20.0, curb = 0.5;

    // Curb-dominated: 2% over 20 ft rises 0.4 ft, below the 0.5 ft curb.
    Xs curbed;
    ASSERT_EQ(swmm_xsect_create_street(width, curb, /*slope=*/2.0, 0.016,
                                       0.0, 0.0, /*sides=*/2, 0.0, 0.0, 0.0,
                                       SWMM_UNITS_US, curbed.out()), SWMM_OK);
    double y_full = 0, a_full = 0, w_max = 0;
    ASSERT_EQ(swmm_xsect_full_properties(curbed.get(), &y_full, &a_full,
                                         nullptr, &w_max, nullptr, nullptr),
              SWMM_OK);
    EXPECT_NEAR(y_full, curb, 1e-6) << "curb is higher than the 0.4 ft crown";
    EXPECT_GT(a_full, 0.0);
    EXPECT_NEAR(w_max, 2.0 * width, 1e-6) << "full street = two halves";

    // Crown-dominated: 4% over 20 ft rises 0.8 ft, above the 0.5 ft curb.
    Xs crowned;
    ASSERT_EQ(swmm_xsect_create_street(width, curb, /*slope=*/4.0, 0.016,
                                       0.0, 0.0, /*sides=*/2, 0.0, 0.0, 0.0,
                                       SWMM_UNITS_US, crowned.out()), SWMM_OK);
    ASSERT_EQ(swmm_xsect_full_properties(crowned.get(), &y_full, nullptr,
                                         nullptr, nullptr, nullptr, nullptr),
              SWMM_OK);
    EXPECT_NEAR(y_full, 0.04 * width, 1e-6) << "crown now sets the full depth";

    int shape = -1;
    ASSERT_EQ(swmm_xsect_get_shape(crowned.get(), &shape), SWMM_OK);
    EXPECT_EQ(shape, SWMM_XSECT_STREET);
}

TEST(XSectStreet, HalfStreetIsHalfAsWide)
{
    Xs full, half;
    ASSERT_EQ(swmm_xsect_create_street(20.0, 0.5, 2.0, 0.016, 0, 0, /*sides=*/2,
                                       0, 0, 0, SWMM_UNITS_US, full.out()),
              SWMM_OK);
    ASSERT_EQ(swmm_xsect_create_street(20.0, 0.5, 2.0, 0.016, 0, 0, /*sides=*/1,
                                       0, 0, 0, SWMM_UNITS_US, half.out()),
              SWMM_OK);
    double w_full = 0, w_half = 0;
    ASSERT_EQ(swmm_xsect_full_properties(full.get(), nullptr, nullptr, nullptr,
                                         &w_full, nullptr, nullptr), SWMM_OK);
    ASSERT_EQ(swmm_xsect_full_properties(half.get(), nullptr, nullptr, nullptr,
                                         &w_half, nullptr, nullptr), SWMM_OK);
    EXPECT_NEAR(w_half, w_full / 2.0, 1e-6);
}

// ===========================================================================
// 7. Link-bound handles
// ===========================================================================

namespace {

/// A BUILDING-state model with one conduit J1→OUT1 carrying the given xsect.
/// The outfall is not decoration — swmm_validate_model (and hence
/// swmm_finalize_model) rejects a model without one.
SWMM_Engine makeConduitModel(int shape, double g1, double g2)
{
    SWMM_Engine e = swmm_engine_new();
    EXPECT_EQ(swmm_node_add(e, "J1", SWMM_NODE_JUNCTION), SWMM_OK);
    EXPECT_EQ(swmm_node_add(e, "J2", SWMM_NODE_OUTFALL), SWMM_OK);
    EXPECT_EQ(swmm_link_add(e, "C1", SWMM_LINK_CONDUIT), SWMM_OK);
    const int li = swmm_link_index(e, "C1");
    EXPECT_EQ(swmm_link_set_nodes(e, li, swmm_node_index(e, "J1"),
                                  swmm_node_index(e, "J2")), SWMM_OK);
    EXPECT_EQ(swmm_link_set_length(e, li, 100.0), SWMM_OK);
    EXPECT_EQ(swmm_link_set_roughness(e, li, 0.016), SWMM_OK);
    EXPECT_EQ(swmm_link_set_xsect(e, li, shape, g1, g2, 0.0, 0.0), SWMM_OK);
    return e;
}

/// Same, resolved — the derived full-flow geometry a link handle needs only
/// exists after finalize.
SWMM_Engine makeResolvedConduitModel(int shape, double g1, double g2)
{
    SWMM_Engine e = makeConduitModel(shape, g1, g2);
    EXPECT_EQ(swmm_finalize_model(e), SWMM_OK);
    return e;
}

} // namespace

TEST(XSectLinkBound, MatchesStandaloneWithTheSameGeometry)
{
    SWMM_Engine e = makeResolvedConduitModel(SWMM_XSECT_CIRCULAR, 2.5, 0.0);
    const int li = swmm_link_index(e, "C1");

    Xs bound, standalone;
    ASSERT_EQ(swmm_link_create_xsect(e, li, bound.out()), SWMM_OK);
    ASSERT_EQ(swmm_xsect_create(SWMM_XSECT_CIRCULAR, 2.5, 0, 0, 0,
                                SWMM_UNITS_US, standalone.out()), SWMM_OK);

    int shape = -1;
    ASSERT_EQ(swmm_xsect_get_shape(bound.get(), &shape), SWMM_OK);
    EXPECT_EQ(shape, SWMM_XSECT_CIRCULAR);

    for (double y : {0.3, 1.25, 2.5}) {
        EXPECT_DOUBLE_EQ(areaAt(bound.get(), y), areaAt(standalone.get(), y));
        EXPECT_DOUBLE_EQ(hydradAt(bound.get(), y), hydradAt(standalone.get(), y));
    }

    swmm_engine_close(e);
    swmm_engine_destroy(e);
}

TEST(XSectLinkBound, HandleOutlivesTheEngine)
{
    SWMM_Engine e = makeResolvedConduitModel(SWMM_XSECT_CIRCULAR, 2.0, 0.0);
    const int li = swmm_link_index(e, "C1");

    Xs bound;
    ASSERT_EQ(swmm_link_create_xsect(e, li, bound.out()), SWMM_OK);
    const double before = areaAt(bound.get(), 1.0);

    swmm_engine_close(e);
    swmm_engine_destroy(e);

    // The handle deep-copied its geometry, so it still answers correctly.
    EXPECT_DOUBLE_EQ(areaAt(bound.get(), 1.0), before);
    EXPECT_NEAR(before, kPi / 8.0 * 2.0 * 2.0, 1e-4);
}

TEST(XSectLinkBound, RejectsOutOfRangeIndex)
{
    SWMM_Engine e = makeResolvedConduitModel(SWMM_XSECT_CIRCULAR, 1.0, 0.0);
    SWMM_XSect h = nullptr;
    EXPECT_EQ(swmm_link_create_xsect(e, -1, &h), SWMM_ERR_BADINDEX);
    EXPECT_EQ(swmm_link_create_xsect(e, 999, &h), SWMM_ERR_BADINDEX);
    EXPECT_EQ(swmm_link_create_xsect(nullptr, 0, &h), SWMM_ERR_BADHANDLE);
    EXPECT_EQ(swmm_link_create_xsect(e, 0, nullptr), SWMM_ERR_BADPARAM);
    swmm_engine_close(e);
    swmm_engine_destroy(e);
}

TEST(XSectLinkBound, DoesNotDependOnTheInitTimeBatchShapeCache)
{
    // Regression guard. link::buildXSectParams takes its shape from
    // links.xsect_batch_shape — a cache SWMMEngine::initialize() fills, and
    // only for conduits. A model that is merely OPENED still has 0 there, which
    // decodes as DUMMY, so a handle that trusted the cache would report the
    // right full_depth (read straight from the array) while every query
    // returned 0. Trapezoids make this obvious: their area needs y_bot/s_bot.
    SWMM_Engine e = makeResolvedConduitModel(SWMM_XSECT_TRAPEZOIDAL, 3.0, 2.0);
    const int li = swmm_link_index(e, "C1");

    Xs bound;
    ASSERT_EQ(swmm_link_create_xsect(e, li, bound.out()), SWMM_OK);
    EXPECT_NEAR(yFull(bound.get()), 3.0, 1e-9);
    EXPECT_GT(areaAt(bound.get(), 1.5), 0.0)
        << "a trapezoid with a 2 ft bottom cannot have zero area at mid-depth";

    int shape = -1;
    ASSERT_EQ(swmm_xsect_get_shape(bound.get(), &shape), SWMM_OK);
    EXPECT_EQ(shape, SWMM_XSECT_TRAPEZOIDAL);

    swmm_engine_close(e);
    swmm_engine_destroy(e);
}

TEST(XSectLinkBound, RefusesAnUnresolvedModelRatherThanReturningZeros)
{
    // Regression guard. In BUILDING state the link's derived geometry does not
    // exist yet and xsect_batch_shape is still 0 — which decodes as DUMMY, so
    // an unguarded handle would answer 0 to every query and look plausible.
    // Failing loudly is the whole point.
    SWMM_Engine e = makeConduitModel(SWMM_XSECT_CIRCULAR, 2.0, 0.0);
    const int li = swmm_link_index(e, "C1");

    SWMM_XSect h = nullptr;
    EXPECT_EQ(swmm_link_create_xsect(e, li, &h), SWMM_ERR_LIFECYCLE);
    EXPECT_EQ(h, nullptr);

    // ...and once resolved, the same call succeeds and answers correctly.
    ASSERT_EQ(swmm_finalize_model(e), SWMM_OK);
    Xs ok;
    ASSERT_EQ(swmm_link_create_xsect(e, li, ok.out()), SWMM_OK);
    EXPECT_NEAR(areaAt(ok.get(), 1.0), kPi / 8.0 * 2.0 * 2.0, 1e-4);

    swmm_engine_close(e);
    swmm_engine_destroy(e);
}

// ===========================================================================
// 8. Error paths
// ===========================================================================

TEST(XSectErrors, ConstructorRejectsBadInput)
{
    SWMM_XSect h = nullptr;
    EXPECT_EQ(swmm_xsect_create(SWMM_XSECT_CIRCULAR, 1.0, 0, 0, 0,
                                SWMM_UNITS_US, nullptr), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_xsect_create(-1, 1.0, 0, 0, 0, SWMM_UNITS_US, &h),
              SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_xsect_create(26, 1.0, 0, 0, 0, SWMM_UNITS_US, &h),
              SWMM_ERR_BADPARAM);
    // Unit system out of range.
    EXPECT_EQ(swmm_xsect_create(SWMM_XSECT_CIRCULAR, 1.0, 0, 0, 0, 2, &h),
              SWMM_ERR_BADPARAM);
    // Degenerate geometry.
    EXPECT_EQ(swmm_xsect_create(SWMM_XSECT_CIRCULAR, 0.0, 0, 0, 0,
                                SWMM_UNITS_US, &h), SWMM_ERR_BADPARAM);
    // Filled circular with fill above the crown.
    EXPECT_EQ(swmm_xsect_create(SWMM_XSECT_FILLED_CIRCULAR, 2.0, 3.0, 0, 0,
                                SWMM_UNITS_US, &h), SWMM_ERR_BADPARAM);
    // NaN.
    EXPECT_EQ(swmm_xsect_create(SWMM_XSECT_CIRCULAR, std::nan(""), 0, 0, 0,
                                SWMM_UNITS_US, &h), SWMM_ERR_BADPARAM);
    // Tabulated shapes need their own constructors.
    EXPECT_EQ(swmm_xsect_create(SWMM_XSECT_IRREGULAR, 1.0, 0, 0, 0,
                                SWMM_UNITS_US, &h), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_xsect_create(SWMM_XSECT_CUSTOM, 1.0, 0, 0, 0,
                                SWMM_UNITS_US, &h), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_xsect_create(SWMM_XSECT_STREET, 1.0, 0, 0, 0,
                                SWMM_UNITS_US, &h), SWMM_ERR_BADPARAM);
}

TEST(XSectErrors, TabulatedConstructorsRejectBadInput)
{
    SWMM_XSect h = nullptr;
    const double st[] = {0.0, 1.0, 2.0};
    const double el[] = {1.0, 0.0, 1.0};

    EXPECT_EQ(swmm_xsect_create_irregular(nullptr, el, 3, 0, 2, 0, 0.03, 0, 1,
                                          SWMM_UNITS_US, &h), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_xsect_create_irregular(st, el, 1, 0, 2, 0, 0.03, 0, 1,
                                          SWMM_UNITS_US, &h), SWMM_ERR_BADPARAM);
    // n_channel must be positive — buildTables divides by it.
    EXPECT_EQ(swmm_xsect_create_irregular(st, el, 3, 0, 2, 0, 0.0, 0, 1,
                                          SWMM_UNITS_US, &h), SWMM_ERR_BADPARAM);

    const double cx[] = {0.0, 1.0};
    const double cy[] = {1.0, 1.0};
    EXPECT_EQ(swmm_xsect_create_custom(0.0, cx, cy, 2, SWMM_UNITS_US, &h),
              SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_xsect_create_custom(1.0, cx, cy, 1, SWMM_UNITS_US, &h),
              SWMM_ERR_BADPARAM);

    EXPECT_EQ(swmm_xsect_create_street(20, 0.5, 4, 0.016, 0, 0, /*sides=*/3,
                                       0, 0, 0, SWMM_UNITS_US, &h),
              SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_xsect_create_street(20, 0.5, 4, /*roughness=*/0.0, 0, 0, 2,
                                       0, 0, 0, SWMM_UNITS_US, &h),
              SWMM_ERR_BADPARAM);
}

TEST(XSectErrors, QueriesRejectBadInput)
{
    Xs xs;
    ASSERT_EQ(swmm_xsect_create(SWMM_XSECT_CIRCULAR, 2.0, 0, 0, 0,
                                SWMM_UNITS_US, xs.out()), SWMM_OK);
    double v = 0;
    EXPECT_EQ(swmm_xsect_area_of_depth(nullptr, 1.0, &v), SWMM_ERR_BADHANDLE);
    EXPECT_EQ(swmm_xsect_area_of_depth(xs.get(), 1.0, nullptr), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_xsect_area_of_depth(xs.get(), -1.0, &v), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_xsect_area_of_depth(xs.get(), std::nan(""), &v), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_xsect_depth_of_area(xs.get(), -1.0, &v), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_xsect_critical_depth(xs.get(), -1.0, &v), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_xsect_is_open(xs.get(), nullptr), SWMM_ERR_BADPARAM);
}

TEST(XSectErrors, FreeAcceptsNull)
{
    EXPECT_EQ(swmm_xsect_free(nullptr), SWMM_OK);
}

TEST(XSectErrors, DepthAboveFullIsClampedNotRejected)
{
    // Closed shapes clamp, matching the routing solvers — a surcharged conduit
    // must not error.
    Xs xs;
    ASSERT_EQ(swmm_xsect_create(SWMM_XSECT_CIRCULAR, 2.0, 0, 0, 0,
                                SWMM_UNITS_US, xs.out()), SWMM_OK);
    double a_full = 0;
    ASSERT_EQ(swmm_xsect_full_properties(xs.get(), nullptr, &a_full, nullptr,
                                         nullptr, nullptr, nullptr), SWMM_OK);
    EXPECT_DOUBLE_EQ(areaAt(xs.get(), 100.0), a_full);
}

// ===========================================================================
// 9. Array forms
// ===========================================================================

TEST(XSectArray, MatchesScalarLoop)
{
    Xs xs;
    ASSERT_EQ(swmm_xsect_create(SWMM_XSECT_CIRCULAR, 2.0, 0, 0, 0,
                                SWMM_UNITS_US, xs.out()), SWMM_OK);
    const double depths[] = {0.0, 0.25, 0.5, 1.0, 1.5, 2.0};
    const int n = static_cast<int>(std::size(depths));

    double got[6] = {};
    ASSERT_EQ(swmm_xsect_area_of_depth_array(xs.get(), depths, n, got), SWMM_OK);
    for (int i = 0; i < n; ++i) {
        EXPECT_DOUBLE_EQ(got[i], areaAt(xs.get(), depths[i])) << "i=" << i;
    }

    ASSERT_EQ(swmm_xsect_width_of_depth_array(xs.get(), depths, n, got), SWMM_OK);
    for (int i = 0; i < n; ++i) {
        EXPECT_DOUBLE_EQ(got[i], widthAt(xs.get(), depths[i])) << "i=" << i;
    }
}

TEST(XSectArray, EmptyIsANoOpAndBadInputIsRejected)
{
    Xs xs;
    ASSERT_EQ(swmm_xsect_create(SWMM_XSECT_CIRCULAR, 2.0, 0, 0, 0,
                                SWMM_UNITS_US, xs.out()), SWMM_OK);
    EXPECT_EQ(swmm_xsect_area_of_depth_array(xs.get(), nullptr, 0, nullptr),
              SWMM_OK);

    const double bad[] = {1.0, -1.0};
    double out[2] = {};
    EXPECT_EQ(swmm_xsect_area_of_depth_array(xs.get(), bad, 2, out),
              SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_xsect_area_of_depth_array(xs.get(), bad, -1, out),
              SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_xsect_area_of_depth_array(nullptr, bad, 2, out),
              SWMM_ERR_BADHANDLE);
}

TEST(XSectArray, InputAndOutputMayAlias)
{
    Xs xs;
    ASSERT_EQ(swmm_xsect_create(SWMM_XSECT_RECT_CLOSED, 4.0, 2.0, 0, 0,
                                SWMM_UNITS_US, xs.out()), SWMM_OK);
    double buf[3] = {1.0, 2.0, 3.0};
    ASSERT_EQ(swmm_xsect_area_of_depth_array(xs.get(), buf, 3, buf), SWMM_OK);
    // area = depth * width(2.0)
    EXPECT_DOUBLE_EQ(buf[0], 2.0);
    EXPECT_DOUBLE_EQ(buf[1], 4.0);
    EXPECT_DOUBLE_EQ(buf[2], 6.0);
}
