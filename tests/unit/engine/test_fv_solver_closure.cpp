/**
 * @file test_fv_solver_closure.cpp
 * @brief Cross-section closure gates for the explicit FV 1D solver.
 *
 * @details Covers the properties every downstream gate silently relies on
 *          (plan §3.3):
 *            - A(h) monotone and continuous through the crown
 *            - dA/dh == T(h) to the accuracy of a centred difference (the
 *              tapered slot mouth is what keeps this true AT the crown)
 *            - A ↔ h round-trip, including inside the taper band and deep in
 *              the slot
 *            - I₁ is the antiderivative of A, and its above-crown extension is
 *              analytic rather than extrapolated
 *            - the design celerity FV_SLOT_CELERITY is actually delivered
 *
 * @ingroup engine_fv
 */

#include <gtest/gtest.h>

#include <cmath>

#include "fv_test_support.hpp"

using namespace fvtest;
namespace k = openswmm::fv::kernels;

namespace {

FvGeometry makeCircular(double d, double celerity = 100.0) {
    FvGeometry g;
    buildGeometry(circular(d), false, celerity, g);
    return g;
}

FvGeometry makeRect(double w, double y, double celerity = 100.0) {
    FvGeometry g;
    buildGeometry(rectOpen(w, y), true, celerity, g);
    return g;
}

} // namespace

// ---------------------------------------------------------------------------
// Monotonicity + continuity
// ---------------------------------------------------------------------------

TEST(FvClosure, AreaIsStrictlyMonotoneThroughTheCrown) {
    const FvGeometry g = makeCircular(3.0);
    double prev = -1.0;
    for (int i = 0; i <= 4000; ++i) {
        const double h = 3.0 * static_cast<double>(i) / 2000.0;   // to 2×y_full
        const double a = k::areaOfDepth(g, h);
        EXPECT_GT(a, prev) << "A(h) not increasing at h=" << h;
        prev = a;
    }
}

TEST(FvClosure, AreaIsContinuousAtTheCrown) {
    const FvGeometry g = makeCircular(3.0);
    const double below = k::areaOfDepth(g, g.y_full - 1.0e-9);
    const double at    = k::areaOfDepth(g, g.y_full);
    const double above = k::areaOfDepth(g, g.y_full + 1.0e-9);
    EXPECT_NEAR(below, at, 1.0e-7);
    EXPECT_NEAR(above, at, 1.0e-7);
}

TEST(FvClosure, WidthIsTheDerivativeOfAreaForAnalyticSections) {
    // For a section whose area is computed from a formula rather than a table,
    // T(h) IS dA/dh and the identity must hold tightly.
    FvGeometry g;
    buildGeometry(rectOpen(10.0, 4.0), true, 100.0, g);
    for (double h : {0.3, 1.0, 2.0, 3.9, 4.0, 6.0}) {
        const double e = 1.0e-6;
        const double num =
            (k::areaOfDepth(g, h + e) - k::areaOfDepth(g, h - e)) / (2.0 * e);
        EXPECT_NEAR(num, k::widthOfDepth(g, h), 1.0e-7) << "at h=" << h;
    }
}

TEST(FvClosure, WidthTracksAreaSlopeToTableResolutionForTabulatedSections) {
    // The legacy geometry tables are INDEPENDENT tabulations: A_Circ and W_Circ
    // are each sampled from the true circle, so W is NOT the finite difference
    // of A — near the crown, where A_Circ flattens onto a 1/50-wide panel and
    // the true width collapses to zero, they differ by a large factor. That is
    // a property of SWMM's geometry, not of this scheme, and it is why:
    //   - the depth inversion bisects instead of using T as a Newton derivative
    //   - the flux uses only A and I₁, which ARE mutually consistent
    //   - T is used for celerity (where the physical width is the right thing)
    // Away from the crown the two agree to table resolution, which is what this
    // pins so a future table change cannot silently drift.
    const FvGeometry g = makeCircular(3.0);
    for (double h : {0.6, 1.2, 1.8, 2.4}) {
        const double e = 1.0e-4;
        const double num =
            (k::areaOfDepth(g, h + e) - k::areaOfDepth(g, h - e)) / (2.0 * e);
        EXPECT_NEAR(num, k::widthOfDepth(g, h), 0.10 * k::widthOfDepth(g, h))
            << "at h=" << h;
    }
}

TEST(FvClosure, SlotContributionIsExactlyItsOwnDerivative) {
    // The part this plan OWNS — the tapered slot — must satisfy T_slot = dA_slot/dh
    // exactly, including at the crown. A discontinuous dA/dh there is what would
    // produce spurious reflections and corrupt the wave-speed estimates.
    const FvGeometry g = makeCircular(3.0);
    const double band = g.y_full - g.y_crown;
    auto slotArea = [&](double h) {
        if (h >= g.y_full) return g.t_slot * band * 0.5 + g.t_slot * (h - g.y_full);
        return g.t_slot * band * k::slotRampIntegral((h - g.y_crown) / band);
    };
    auto slotWidth = [&](double h) {
        if (h >= g.y_full) return g.t_slot;
        return g.t_slot * k::slotRamp((h - g.y_crown) / band);
    };
    for (int i = 0; i <= 200; ++i) {
        const double h = g.y_crown + 2.0 * band * static_cast<double>(i) / 200.0;
        const double e = 1.0e-9;
        const double num = (slotArea(h + e) - slotArea(h - e)) / (2.0 * e);
        EXPECT_NEAR(num, slotWidth(h), 1.0e-4 * g.t_slot) << "at h=" << h;
    }
}

TEST(FvClosure, WidthNeverVanishesAboveTheCrownCutoff) {
    // A vanishing top width would send the celerity to infinity and collapse
    // the CFL step. The slot ramp is what floors it.
    const FvGeometry g = makeCircular(3.0);
    for (int i = 0; i <= 500; ++i) {
        const double h = g.y_crown + (g.y_full - g.y_crown) *
                                          static_cast<double>(i) / 500.0;
        EXPECT_GT(k::widthOfDepth(g, h), 0.0) << "T vanished at h=" << h;
    }
}

// ---------------------------------------------------------------------------
// Inversion
// ---------------------------------------------------------------------------

TEST(FvClosure, DepthAreaRoundTripsEverywhere) {
    for (const FvGeometry& g : {makeCircular(3.0), makeCircular(0.5),
                                makeRect(10.0, 4.0)}) {
        for (int i = 1; i <= 3000; ++i) {
            const double h = g.y_full * 2.0 * static_cast<double>(i) / 1500.0;
            const double a = k::areaOfDepth(g, h);
            const double h2 = k::depthOfArea(g, a);
            EXPECT_NEAR(h, h2, 1.0e-8 * std::max(1.0, h))
                << "round-trip failed at h=" << h;
        }
    }
}

TEST(FvClosure, DepthAreaRoundTripsInsideTheTaperBand) {
    const FvGeometry g = makeCircular(3.0);
    for (int i = 0; i <= 2000; ++i) {
        const double h = g.y_crown +
                         (g.y_full - g.y_crown) * static_cast<double>(i) / 2000.0;
        const double a = k::areaOfDepth(g, h);
        EXPECT_NEAR(k::depthOfArea(g, a), h, 1.0e-9);
    }
}

// ---------------------------------------------------------------------------
// First moment
// ---------------------------------------------------------------------------

TEST(FvClosure, I1IsTheAntiderivativeOfArea) {
    const FvGeometry g = makeCircular(3.0);
    // Compare the tabulated I₁ against an independent fine quadrature.
    for (double h : {0.25, 0.9, 1.7, 2.5, 2.99}) {
        const int m = 20000;
        double ref = 0.0;
        const double step = h / static_cast<double>(m);
        for (int i = 0; i < m; ++i) {
            const double x0 = static_cast<double>(i) * step;
            ref += 0.5 * (k::areaOfDepth(g, x0) + k::areaOfDepth(g, x0 + step)) * step;
        }
        const double got = k::i1OfDepth(g, h, k::areaOfDepth(g, h));
        EXPECT_NEAR(got, ref, 1.0e-5 * std::max(1.0, ref)) << "I1 at h=" << h;
    }
}

TEST(FvClosure, I1AboveTheCrownIsAnalytic) {
    const FvGeometry g = makeCircular(3.0);
    // A(h) is exactly linear above the crown, so I₁ must be exactly quadratic.
    // This is what keeps deep surcharge accurate with a small table.
    for (double d : {0.0, 1.0, 25.0, 200.0}) {
        const double h = g.y_full + d;
        const double expect = g.i1_crown + g.a_crown * d + 0.5 * g.t_slot * d * d;
        EXPECT_NEAR(k::i1OfDepth(g, h, k::areaOfDepth(g, h)), expect,
                    1.0e-9 * std::max(1.0, expect));
    }
}

// ---------------------------------------------------------------------------
// Slot celerity
// ---------------------------------------------------------------------------

TEST(FvClosure, SlotCelerityMatchesTheDesignValue) {
    for (double c_design : {50.0, 100.0, 300.0}) {
        const FvGeometry g = makeCircular(3.0, c_design);
        const double h = g.y_full + 5.0;                 // well into the slot
        const double a = k::areaOfDepth(g, h);
        const double t = k::widthOfDepth(g, h);
        const double c = k::celerity(a, t);
        // The pressurized celerity grows slowly with head because the stored
        // slot area adds to A; check it at the crown where the design value is
        // defined, and confirm it stays the right order deeper in.
        const double c_at_crown = k::celerity(k::areaOfDepth(g, g.y_full),
                                              k::widthOfDepth(g, g.y_full));
        EXPECT_NEAR(c_at_crown, c_design, 0.03 * c_design);
        EXPECT_GT(c, 0.5 * c_design);
        EXPECT_LT(c, 4.0 * c_design);
    }
}

TEST(FvClosure, OpenSectionsGetNoSlotAndExtendVertically) {
    const FvGeometry g = makeRect(10.0, 4.0);
    EXPECT_EQ(g.is_open, 1);
    EXPECT_DOUBLE_EQ(g.y_crown, g.y_full);
    EXPECT_DOUBLE_EQ(g.t_slot, 10.0);
    // Above full depth an open rectangle is just taller: A = w·h throughout.
    for (double h : {1.0, 3.9, 4.0, 4.5, 12.0})
        EXPECT_NEAR(k::areaOfDepth(g, h), 10.0 * h, 1.0e-9);
}
