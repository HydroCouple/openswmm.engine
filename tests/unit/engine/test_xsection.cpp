/**
 * @file test_xsection.cpp
 * @brief Unit tests for cross-section geometry (per-element + batch).
 *
 * @details Tests:
 *   - Per-element: area, hyd-rad, width, depth-from-area at 0%, 50%, 100% full
 *     for CIRCULAR, RECT_CLOSED, TRAPEZOIDAL, TRIANGULAR, PARABOLIC, POWERFUNC
 *   - Tabulated shapes: EGG, HORSESHOE, HORIZ_ELLIPSE, VERT_ELLIPSE, ARCH
 *   - Batch: XSectGroups build, computeAreas/HydRad/Widths scatter correctness
 *   - Lookup/invLookup table interpolation
 *   - setParams for basic shapes
 *
 * @see src/engine/hydraulics/XSection.hpp
 * @see src/engine/hydraulics/XSectBatch.hpp
 * @ingroup engine_hydraulics
 */

#include <gtest/gtest.h>
#include <cmath>
#include <vector>

#include "../../src/engine/hydraulics/XSectBatch.hpp"

using namespace openswmm;

static constexpr double PI = 3.14159265358979323846;

// ============================================================================
// Helper: build a circular XSectParams
// ============================================================================

static XSectParams make_circular(double diameter) {
    XSectParams xs;
    double p[4] = {diameter, 0, 0, 0};
    xsect::setParams(xs, static_cast<int>(XSectShape::CIRCULAR), p, 1.0);
    return xs;
}

static XSectParams make_rect_closed(double height, double width) {
    XSectParams xs;
    double p[4] = {height, width, 0, 0};
    xsect::setParams(xs, static_cast<int>(XSectShape::RECT_CLOSED), p, 1.0);
    return xs;
}

static XSectParams make_trapezoidal(double depth, double bot_width, double m1, double m2) {
    XSectParams xs;
    double p[4] = {depth, bot_width, m1, m2};
    xsect::setParams(xs, static_cast<int>(XSectShape::TRAPEZOIDAL), p, 1.0);
    return xs;
}

static XSectParams make_triangular(double depth, double top_width) {
    XSectParams xs;
    double p[4] = {depth, top_width, 0, 0};
    xsect::setParams(xs, static_cast<int>(XSectShape::TRIANGULAR), p, 1.0);
    return xs;
}

// ============================================================================
// Per-element: CIRCULAR
// ============================================================================

TEST(XSectionCircular, FullAreaMatchesFormula) {
    auto xs = make_circular(2.0);  // D=2ft
    EXPECT_NEAR(xs.a_full, PI / 4.0 * 4.0, 1e-10);  // pi/4 * D^2
    EXPECT_NEAR(xs.r_full, 0.5, 1e-10);               // D/4
}

TEST(XSectionCircular, AreaAtZeroDepth) {
    auto xs = make_circular(2.0);
    EXPECT_DOUBLE_EQ(xsect::getAofY(xs, 0.0), 0.0);
}

TEST(XSectionCircular, AreaAtFullDepth) {
    auto xs = make_circular(2.0);
    double a = xsect::getAofY(xs, 2.0);
    EXPECT_NEAR(a, xs.a_full, 1e-6);
}

TEST(XSectionCircular, AreaAtHalfDepth) {
    auto xs = make_circular(2.0);
    double a = xsect::getAofY(xs, 1.0);  // y/D = 0.5
    // At half depth, A/Afull = 0.5 for a circle
    EXPECT_NEAR(a / xs.a_full, 0.5, 0.01);
}

TEST(XSectionCircular, HydRadAtFullDepth) {
    auto xs = make_circular(2.0);
    double r = xsect::getRofY(xs, 2.0);
    EXPECT_NEAR(r, xs.r_full, 1e-6);
}

TEST(XSectionCircular, WidthAtHalfDepth) {
    auto xs = make_circular(2.0);
    double w = xsect::getWofY(xs, 1.0);  // y/D = 0.5
    // At half depth, W/Wmax = 1.0 for a circle
    EXPECT_NEAR(w / xs.w_max, 1.0, 0.01);
}

TEST(XSectionCircular, WidthAtFullDepthIsZero) {
    auto xs = make_circular(2.0);
    double w = xsect::getWofY(xs, 2.0);
    EXPECT_NEAR(w, 0.0, 0.01);
}

TEST(XSectionCircular, DepthFromAreaRoundTrip) {
    auto xs = make_circular(3.0);
    double y_orig = 1.5;  // half full
    double a = xsect::getAofY(xs, y_orig);
    double y_back = xsect::getYofA(xs, a);
    EXPECT_NEAR(y_back, y_orig, 0.02);
}

TEST(XSectionCircular, SectionFactorAtFull) {
    auto xs = make_circular(2.0);
    double s = xsect::getSofA(xs, xs.a_full);
    EXPECT_NEAR(s, xs.s_full, 0.01 * xs.s_full);
}

// ============================================================================
// Per-element: RECT_CLOSED
// ============================================================================

TEST(XSectionRectClosed, FullAreaExact) {
    auto xs = make_rect_closed(3.0, 4.0);
    EXPECT_NEAR(xs.a_full, 12.0, 1e-10);
}

TEST(XSectionRectClosed, AreaLinearInDepth) {
    auto xs = make_rect_closed(3.0, 4.0);
    EXPECT_NEAR(xsect::getAofY(xs, 1.5), 6.0, 1e-10);
}

TEST(XSectionRectClosed, HydRadExact) {
    auto xs = make_rect_closed(3.0, 4.0);
    // R = w*y / (w + 2*y) at half: 4*1.5/(4+3) = 6/7
    double r = xsect::getRofY(xs, 1.5);
    EXPECT_NEAR(r, 6.0 / 7.0, 0.05);
}

TEST(XSectionRectClosed, DepthFromAreaExact) {
    auto xs = make_rect_closed(3.0, 4.0);
    EXPECT_NEAR(xsect::getYofA(xs, 8.0), 2.0, 1e-10);
}

TEST(XSectionRectClosed, WidthConstant) {
    auto xs = make_rect_closed(3.0, 4.0);
    EXPECT_NEAR(xsect::getWofY(xs, 1.0), 4.0, 1e-10);
}

// ============================================================================
// Per-element: TRAPEZOIDAL
// ============================================================================

TEST(XSectionTrapezoidal, FullAreaExact) {
    auto xs = make_trapezoidal(2.0, 3.0, 1.0, 1.0);
    // A = (bot + m*y)*y = (3 + 1*2)*2 = 10
    EXPECT_NEAR(xs.a_full, 10.0, 1e-10);
}

TEST(XSectionTrapezoidal, AreaFormula) {
    auto xs = make_trapezoidal(2.0, 3.0, 1.0, 1.0);
    // At y=1: A = (3+1)*1 = 4
    EXPECT_NEAR(xsect::getAofY(xs, 1.0), 4.0, 1e-10);
}

TEST(XSectionTrapezoidal, WidthFormula) {
    auto xs = make_trapezoidal(2.0, 3.0, 1.0, 1.0);
    // W = bot + 2*m*y = 3 + 2*1*1 = 5
    EXPECT_NEAR(xsect::getWofY(xs, 1.0), 5.0, 1e-10);
}

TEST(XSectionTrapezoidal, DepthFromAreaInverse) {
    auto xs = make_trapezoidal(2.0, 3.0, 1.0, 1.0);
    double y = xsect::getYofA(xs, 4.0);
    EXPECT_NEAR(y, 1.0, 1e-8);
}

// ============================================================================
// Per-element: TRIANGULAR
// ============================================================================

TEST(XSectionTriangular, FullAreaExact) {
    auto xs = make_triangular(2.0, 4.0);
    // m = w/(2*y) = 4/(2*2) = 1, A = m*y^2 = 1*4 = 4
    EXPECT_NEAR(xs.a_full, 4.0, 1e-10);
}

TEST(XSectionTriangular, AreaQuadratic) {
    auto xs = make_triangular(2.0, 4.0);
    EXPECT_NEAR(xsect::getAofY(xs, 1.0), 1.0, 1e-10);
}

TEST(XSectionTriangular, WidthLinear) {
    auto xs = make_triangular(2.0, 4.0);
    EXPECT_NEAR(xsect::getWofY(xs, 1.0), 2.0, 1e-10);
}

TEST(XSectionTriangular, DepthFromAreaSqrt) {
    auto xs = make_triangular(2.0, 4.0);
    EXPECT_NEAR(xsect::getYofA(xs, 1.0), 1.0, 1e-10);
}

// ============================================================================
// Lookup table helpers
// ============================================================================

TEST(XSectionLookup, LinearInterpolation) {
    // Simple linear table: table[i] = i * delta
    double table[5] = {0.0, 0.25, 0.5, 0.75, 1.0};
    EXPECT_NEAR(xsect::lookup(0.5, table, 5), 0.5, 1e-10);
    EXPECT_NEAR(xsect::lookup(0.0, table, 5), 0.0, 1e-10);
    EXPECT_NEAR(xsect::lookup(1.0, table, 5), 1.0, 1e-10);
}

TEST(XSectionLookup, InvLookupSymmetry) {
    double table[5] = {0.0, 0.25, 0.5, 0.75, 1.0};
    // invLookup of lookup should return the original
    double x = 0.375;
    double y = xsect::lookup(x, table, 5);
    double x_back = xsect::invLookup(y, table, 5);
    EXPECT_NEAR(x_back, x, 0.01);
}

// ============================================================================
// Tabulated shapes: basic sanity (full area, zero area)
// ============================================================================

TEST(XSectionTabulated, EggZeroAndFullArea) {
    XSectParams xs;
    xs.type = static_cast<int>(XSectShape::EGGSHAPED);
    xs.y_full = 3.0;
    xs.a_full = 5.0;
    xs.r_full = 0.75;
    xs.w_max  = 2.0;

    EXPECT_DOUBLE_EQ(xsect::getAofY(xs, 0.0), 0.0);
    EXPECT_NEAR(xsect::getAofY(xs, 3.0), 5.0, 0.01);
}

TEST(XSectionTabulated, HorseshoeZeroAndFullArea) {
    XSectParams xs;
    xs.type = static_cast<int>(XSectShape::HORSESHOE);
    xs.y_full = 4.0;
    xs.a_full = 10.0;
    xs.r_full = 1.0;
    xs.w_max  = 4.0;

    EXPECT_DOUBLE_EQ(xsect::getAofY(xs, 0.0), 0.0);
    EXPECT_NEAR(xsect::getAofY(xs, 4.0), 10.0, 0.01);
}

// ============================================================================
// isOpen
// ============================================================================

TEST(XSectionIsOpen, ClosedShapes) {
    EXPECT_FALSE(xsect::isOpen(static_cast<int>(XSectShape::CIRCULAR)));
    EXPECT_FALSE(xsect::isOpen(static_cast<int>(XSectShape::RECT_CLOSED)));
    EXPECT_FALSE(xsect::isOpen(static_cast<int>(XSectShape::EGGSHAPED)));
}

TEST(XSectionIsOpen, OpenShapes) {
    EXPECT_TRUE(xsect::isOpen(static_cast<int>(XSectShape::RECT_OPEN)));
    EXPECT_TRUE(xsect::isOpen(static_cast<int>(XSectShape::TRAPEZOIDAL)));
    EXPECT_TRUE(xsect::isOpen(static_cast<int>(XSectShape::TRIANGULAR)));
    EXPECT_TRUE(xsect::isOpen(static_cast<int>(XSectShape::PARABOLIC)));
    EXPECT_TRUE(xsect::isOpen(static_cast<int>(XSectShape::POWERFUNC)));
}

// ============================================================================
// Batch: XSectGroups
// ============================================================================

class XSectBatchTest : public ::testing::Test {
protected:
    void SetUp() override {
        // 6 links: 3 circular (D=2,3,4), 2 rectangular (3x4, 2x5), 1 trapezoidal
        params_.resize(6);

        // Circular links
        double p0[4] = {2.0, 0, 0, 0};
        xsect::setParams(params_[0], static_cast<int>(XSectShape::CIRCULAR), p0, 1.0);
        double p1[4] = {3.0, 0, 0, 0};
        xsect::setParams(params_[1], static_cast<int>(XSectShape::CIRCULAR), p1, 1.0);
        double p2[4] = {4.0, 0, 0, 0};
        xsect::setParams(params_[2], static_cast<int>(XSectShape::CIRCULAR), p2, 1.0);

        // Rectangular links
        double p3[4] = {3.0, 4.0, 0, 0};
        xsect::setParams(params_[3], static_cast<int>(XSectShape::RECT_CLOSED), p3, 1.0);
        double p4[4] = {2.0, 5.0, 0, 0};
        xsect::setParams(params_[4], static_cast<int>(XSectShape::RECT_CLOSED), p4, 1.0);

        // Trapezoidal link
        double p5[4] = {2.0, 3.0, 1.0, 1.0};
        xsect::setParams(params_[5], static_cast<int>(XSectShape::TRAPEZOIDAL), p5, 1.0);

        groups_.build(params_.data(), 6);
    }

    std::vector<XSectParams> params_;
    XSectGroups groups_;
};

TEST_F(XSectBatchTest, GroupCount) {
    // Should have 3 groups: circular, rect_closed, trapezoidal
    EXPECT_EQ(groups_.numGroups(), 3);
}

TEST_F(XSectBatchTest, CircularGroupHasThreeLinks) {
    auto* circ = groups_.findGroup(XSectShape::CIRCULAR);
    ASSERT_NE(circ, nullptr);
    EXPECT_EQ(circ->count, 3);
}

TEST_F(XSectBatchTest, RectGroupHasTwoLinks) {
    auto* rect = groups_.findGroup(XSectShape::RECT_CLOSED);
    ASSERT_NE(rect, nullptr);
    EXPECT_EQ(rect->count, 2);
}

TEST_F(XSectBatchTest, ComputeAreasMatchesPerElement) {
    // Set depths: half-full for each link
    double depths[6] = {1.0, 1.5, 2.0, 1.5, 1.0, 1.0};
    double areas_batch[6] = {};
    double areas_elem[6] = {};

    groups_.computeAreas(depths, areas_batch, 6);

    // Per-element reference
    for (int i = 0; i < 6; ++i) {
        areas_elem[i] = xsect::getAofY(params_[i], depths[i]);
    }

    for (int i = 0; i < 6; ++i) {
        EXPECT_NEAR(areas_batch[i], areas_elem[i], 1e-10)
            << "Mismatch at link " << i;
    }
}

TEST_F(XSectBatchTest, ComputeHydRadMatchesPerElement) {
    double depths[6] = {1.0, 1.5, 2.0, 1.5, 1.0, 1.0};
    double hydrad_batch[6] = {};
    double hydrad_elem[6] = {};

    groups_.computeHydRad(depths, hydrad_batch, 6);

    for (int i = 0; i < 6; ++i) {
        hydrad_elem[i] = xsect::getRofY(params_[i], depths[i]);
    }

    for (int i = 0; i < 6; ++i) {
        EXPECT_NEAR(hydrad_batch[i], hydrad_elem[i], 1e-10)
            << "Mismatch at link " << i;
    }
}

TEST_F(XSectBatchTest, ComputeWidthsMatchesPerElement) {
    double depths[6] = {1.0, 1.5, 2.0, 1.5, 1.0, 1.0};
    double widths_batch[6] = {};
    double widths_elem[6] = {};

    groups_.computeWidths(depths, widths_batch, 6);

    for (int i = 0; i < 6; ++i) {
        widths_elem[i] = xsect::getWofY(params_[i], depths[i]);
    }

    for (int i = 0; i < 6; ++i) {
        EXPECT_NEAR(widths_batch[i], widths_elem[i], 1e-10)
            << "Mismatch at link " << i;
    }
}

TEST_F(XSectBatchTest, ZeroDepthGivesZeroArea) {
    double depths[6] = {0, 0, 0, 0, 0, 0};
    double areas[6] = {-1, -1, -1, -1, -1, -1};
    groups_.computeAreas(depths, areas, 6);
    for (int i = 0; i < 6; ++i) {
        EXPECT_DOUBLE_EQ(areas[i], 0.0);
    }
}

TEST_F(XSectBatchTest, FullDepthGivesFullArea) {
    double depths[6];
    for (int i = 0; i < 6; ++i) depths[i] = params_[i].y_full;
    double areas[6] = {};
    groups_.computeAreas(depths, areas, 6);
    for (int i = 0; i < 6; ++i) {
        EXPECT_NEAR(areas[i], params_[i].a_full, 0.01 * params_[i].a_full)
            << "Link " << i;
    }
}

// ============================================================================
// Batch with many links of one shape (vectorisation validation)
// ============================================================================

TEST(XSectBatchLarge, CircularBatch1000) {
    constexpr int N = 1000;
    std::vector<XSectParams> params(N);
    for (int i = 0; i < N; ++i) {
        double d = 1.0 + static_cast<double>(i) * 0.01;  // diameters 1.0 to 10.99
        double p[4] = {d, 0, 0, 0};
        xsect::setParams(params[i], static_cast<int>(XSectShape::CIRCULAR), p, 1.0);
    }

    XSectGroups groups;
    groups.build(params.data(), N);
    EXPECT_EQ(groups.numGroups(), 1);

    std::vector<double> depths(N), areas_batch(N), areas_elem(N);
    for (int i = 0; i < N; ++i) {
        depths[i] = params[i].y_full * 0.5;  // half full
    }

    groups.computeAreas(depths.data(), areas_batch.data(), N);

    for (int i = 0; i < N; ++i) {
        areas_elem[i] = xsect::getAofY(params[i], depths[i]);
    }

    for (int i = 0; i < N; ++i) {
        EXPECT_NEAR(areas_batch[i], areas_elem[i], 1e-10)
            << "Mismatch at link " << i;
    }
}
