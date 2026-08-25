/**
 * @file test_fv_closure_shapes.cpp
 * @brief FV slot program R0 — the closure identity gates of
 *        test_fv_solver_closure.cpp, swept across the FULL closed
 *        cross-section catalog instead of just the circle.
 *
 * Every FV unit gate before this file ran on a circle or an open rectangle,
 * while xsect::setParams supports 17 standalone closed shapes whose
 * A_full/W_max ratios differ by 2-3x — which moves both the slot width the
 * 5%-of-W_max cap delivers and the width DISCONTINUITY at the crown
 * (a circle's own top width vanishes at the crown; a closed rectangle's is
 * the full width). This harness gates the invariants every shape must hold
 * and MEASURES the per-shape properties the program's later phases act on:
 * the crown width jump (R3's work order) and the cap-implied celerity.
 *
 * CUSTOM and STREET_XSECT are closed shapes too, but their geometry comes
 * from curve/street tables that need a parse context — they are exercised
 * at the network level, not here.
 */

#include <gtest/gtest.h>

#include <cmath>
#include <string>

#include "fv_test_support.hpp"

using namespace fvtest;
namespace k = openswmm::fv::kernels;

namespace {

struct ShapeCase {
    const char* name;
    XSectShape  shape;
    double      p[4];   // [XSECTIONS] geom1..geom4, US units (ucf = 1)
};

// One representative of every closed shape setParams can build standalone.
// Sizes in the 3-ft class so the numbers are comparable across shapes.
const ShapeCase kClosedShapes[] = {
    {"CIRCULAR",        XSectShape::CIRCULAR,        {3.0, 0.0, 0.0, 0.0}},
    {"FORCE_MAIN",      XSectShape::FORCE_MAIN,      {3.0, 130.0, 0.0, 0.0}},
    {"FILLED_CIRCULAR", XSectShape::FILLED_CIRCULAR, {3.0, 0.5, 0.0, 0.0}},
    {"RECT_CLOSED",     XSectShape::RECT_CLOSED,     {3.0, 2.0, 0.0, 0.0}},
    {"RECT_TRIANG",     XSectShape::RECT_TRIANG,     {3.0, 2.0, 1.0, 0.0}},
    {"RECT_ROUND",      XSectShape::RECT_ROUND,      {3.0, 2.0, 1.5, 0.0}},
    {"MOD_BASKET",      XSectShape::MOD_BASKET,      {3.0, 2.0, 1.0, 0.0}},
    {"HORIZ_ELLIPSE",   XSectShape::HORIZ_ELLIPSE,   {3.0, 4.0, 0.0, 0.0}},
    {"VERT_ELLIPSE",    XSectShape::VERT_ELLIPSE,    {3.0, 2.0, 0.0, 0.0}},
    {"ARCH",            XSectShape::ARCH,            {3.0, 4.0, 0.0, 0.0}},
    {"EGGSHAPED",       XSectShape::EGGSHAPED,       {3.0, 0.0, 0.0, 0.0}},
    {"HORSESHOE",       XSectShape::HORSESHOE,       {3.0, 0.0, 0.0, 0.0}},
    {"GOTHIC",          XSectShape::GOTHIC,          {3.0, 0.0, 0.0, 0.0}},
    {"CATENARY",        XSectShape::CATENARY,        {3.0, 0.0, 0.0, 0.0}},
    {"SEMIELLIPTICAL",  XSectShape::SEMIELLIPTICAL,  {3.0, 0.0, 0.0, 0.0}},
    {"BASKETHANDLE",    XSectShape::BASKETHANDLE,    {3.0, 0.0, 0.0, 0.0}},
    {"SEMICIRCULAR",    XSectShape::SEMICIRCULAR,    {3.0, 0.0, 0.0, 0.0}},
};

FvGeometry build(const ShapeCase& sc, double celerity, int barrels = 1) {
    XSectParams xs;
    xs.type = static_cast<int>(sc.shape);
    const int rc = xsect::setParams(xs, xs.type, sc.p, 1.0);
    EXPECT_EQ(rc, 0) << sc.name;
    EXPECT_GT(xs.a_full, 0.0) << sc.name;
    EXPECT_FALSE(xsect::isOpen(xs.type)) << sc.name;
    FvGeometry g;
    buildGeometry(xs, /*is_open=*/false, celerity, g, barrels);
    return g;
}

}  // namespace

class FvClosureShapes : public ::testing::TestWithParam<ShapeCase> {};

INSTANTIATE_TEST_SUITE_P(
    ClosedCatalog, FvClosureShapes, ::testing::ValuesIn(kClosedShapes),
    [](const ::testing::TestParamInfo<ShapeCase>& i) {
        return std::string(i.param.name);
    });

// Area strictly monotone from near-dry through the crown band into the slot,
// and continuous at the crown (the width may jump; the area may not).
TEST_P(FvClosureShapes, areaIsMonotoneAndContinuousThroughTheCrown) {
    const FvGeometry g = build(GetParam(), 100.0);
    double prev = -1.0;
    for (int i = 1; i <= 400; ++i) {
        const double h = g.y_full * 2.0 * (static_cast<double>(i) / 400.0);
        const double a = k::areaOfDepth(g, h);
        ASSERT_GT(a, prev) << GetParam().name << " at h=" << h;
        prev = a;
    }
    const double below = k::areaOfDepth(g, g.y_full * (1.0 - 1.0e-9));
    const double above = k::areaOfDepth(g, g.y_full * (1.0 + 1.0e-9));
    EXPECT_NEAR(above, below, 1.0e-6 * g.a_full) << GetParam().name;
}

// depthOfArea ∘ areaOfDepth == id everywhere, including inside the taper
// band and deep in the slot — the inverse-well-posedness gate that would
// bite first if a shape's tabulated geometry broke the bracket tables.
TEST_P(FvClosureShapes, depthAreaRoundTripsEverywhere) {
    const FvGeometry g = build(GetParam(), 100.0);
    for (int i = 1; i <= 200; ++i) {
        const double h = g.y_full * 2.0 * (static_cast<double>(i) / 200.0);
        const double a = k::areaOfDepth(g, h);
        const double h2 = k::depthOfArea(g, a);
        ASSERT_NEAR(h2, h, 1.0e-8) << GetParam().name << " at h=" << h;
    }
}

// I1 is the antiderivative of A for every shape: compare the closure's
// i1OfDepth against composite-trapezoid quadrature of its own areaOfDepth.
TEST_P(FvClosureShapes, i1IsTheAntiderivativeOfArea) {
    const FvGeometry g = build(GetParam(), 100.0);
    const int    n  = 20000;
    const double hi = 1.5 * g.y_full;
    const double dh = hi / n;
    double acc = 0.0, prev_a = 0.0;
    for (int i = 1; i <= n; ++i) {
        const double h = dh * i;
        const double a = k::areaOfDepth(g, h);
        acc += 0.5 * (a + prev_a) * dh;
        prev_a = a;
        if (i % 4000 == 0) {
            const double i1 = k::i1OfDepth(g, h, a);
            ASSERT_NEAR(i1, acc, 5.0e-4 * std::max(acc, 1.0))
                << GetParam().name << " at h=" << h;
        }
    }
}

// MEASUREMENT, not a pass/fail on magnitude: the width discontinuity at the
// crown. In the band W = w_section(h) + t_slot*ramp; at y_full W = t_slot —
// so the jump equals the section's own top width at the crown. A circle's
// vanishes; a closed rectangle's is the full barrel width. The recorded
// table is R3's work order for the mouth-smoothness rule.
TEST_P(FvClosureShapes, crownWidthJumpIsMeasuredAndRecorded) {
    const FvGeometry g = build(GetParam(), 100.0);
    const double w_below = k::widthOfDepth(g, g.y_full * (1.0 - 1.0e-9));
    const double w_above = k::widthOfDepth(g, g.y_full * (1.0 + 1.0e-9));
    EXPECT_NEAR(w_above, g.t_slot, 1.0e-12) << GetParam().name;
    const double jump = w_below - w_above;
    EXPECT_GE(jump, -1.0e-9) << GetParam().name;   // never NEGATIVE at crown
    ::testing::Test::RecordProperty(
        std::string(GetParam().name) + "_crown_width_jump_ft",
        std::to_string(jump));
    ::testing::Test::RecordProperty(
        std::string(GetParam().name) + "_jump_over_wmax",
        std::to_string((g.w_max > 0.0) ? jump / g.w_max : 0.0));
}

// The 5%-of-W_max cap binds at a shape-dependent celerity because
// A_full/W_max varies ~2-3x across the catalog. Recompute the builder's own
// arithmetic and record the cap-implied celerity per shape — the honesty
// table behind WARNING 108 and the R1 sweep.
TEST_P(FvClosureShapes, slotWidthMatchesTheDesignArithmetic) {
    for (double c : {50.0, 100.0, 300.0}) {
        const FvGeometry g = build(GetParam(), c);
        const double t_req = 32.2 * g.a_full / (c * c);
        const double cap   = 0.05 * g.w_max;
        const double expect = std::max(std::min(t_req, cap), 1.0e-6);
        ASSERT_NEAR(g.t_slot, expect, 1.0e-12 * std::max(expect, 1.0))
            << GetParam().name << " at c=" << c;
    }
    const FvGeometry g = build(GetParam(), 100.0);
    ::testing::Test::RecordProperty(
        std::string(GetParam().name) + "_cap_implied_celerity_ftps",
        std::to_string(std::sqrt(32.2 * g.a_full / (0.05 * g.w_max))));
}

// Multi-barrel slots are sized from the AGGREGATE section, so both the
// delivered and the cap-implied celerity are barrel-invariant.
TEST(FvClosureShapesBarrels, slotCelerityIsBarrelInvariant) {
    const ShapeCase circ = kClosedShapes[0];
    const FvGeometry g1 = build(circ, 100.0, 1);
    const FvGeometry g3 = build(circ, 100.0, 3);
    EXPECT_NEAR(g3.a_full, 3.0 * g1.a_full, 1.0e-12);
    EXPECT_NEAR(g3.t_slot, 3.0 * g1.t_slot, 1.0e-12);
    // celerity = sqrt(g*A/T) — the barrel factor cancels
    EXPECT_NEAR(std::sqrt(32.2 * g3.a_full / g3.t_slot),
                std::sqrt(32.2 * g1.a_full / g1.t_slot), 1.0e-9);
}

// KNOWN-ISSUE pin (slot program R0): FV_SLOT_CELERITY silently saturates
// once the cap binds — for a 3 ft circle the cap-implied celerity is
// sqrt(g*A_full/(0.05*W_max)) ≈ 39 ft/s, and every request below it
// produces the identical geometry. This test EXPECTS the silence so the
// day R3 makes the knob honest, the change is deliberate and this gate
// flips.
TEST(FvClosureShapesKnownIssue, celerityKnobSaturatesSilentlyBelowTheCap) {
    const ShapeCase circ = kClosedShapes[0];
    const FvGeometry g30 = build(circ, 30.0);
    const FvGeometry g15 = build(circ, 15.0);
    EXPECT_DOUBLE_EQ(g30.t_slot, g15.t_slot);
    EXPECT_DOUBLE_EQ(g30.a_crown, g15.a_crown);
    // both really are on the cap, not coincidentally equal
    EXPECT_DOUBLE_EQ(g30.t_slot, 0.05 * g30.w_max);
}
