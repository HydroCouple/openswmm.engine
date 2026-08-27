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
#include "hydraulics/ChebSection.hpp"
#include "hydraulics/LegacyShapeBoundary.hpp"
#include "hydraulics/XSectBoundary.hpp"

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

// ---------------------------------------------------------------------------
// Phase 1 — fused closure (test F1)
// ---------------------------------------------------------------------------
//
// closureAll() must be a pure fusion of areaOfDepth/widthOfDepth/
// hydRadOfDepth/i1OfDepth: same inputs in, bit-identical outputs, for every
// depth regime (dry, below crown, inside the taper band, at the crown exactly,
// and deep in the slot) and for both tabulated (CIRCULAR) and analytic
// (RECT_OPEN) sections.

namespace {

void expectClosureAllMatchesUnfused(const FvGeometry& g, double h) {
    const double a_ref  = k::areaOfDepth(g, h);
    const double w_ref  = k::widthOfDepth(g, h);
    const double r_ref  = k::hydRadOfDepth(g, h);
    const double i1_ref = k::i1OfDepth(g, h, a_ref);

    double a, w, r, i1;
    k::closureAll(g, h, &a, &w, &r, &i1);

    EXPECT_EQ(a, a_ref)   << "A mismatch at h=" << h;
    EXPECT_EQ(w, w_ref)   << "W mismatch at h=" << h;
    EXPECT_EQ(r, r_ref)   << "R mismatch at h=" << h;
    EXPECT_EQ(i1, i1_ref) << "I1 mismatch at h=" << h;
}

} // namespace

TEST(FvClosure, F1_ClosureAllIsBitIdenticalToUnfusedPath_Circular) {
    const FvGeometry g = makeCircular(3.0);
    expectClosureAllMatchesUnfused(g, 0.0);
    expectClosureAllMatchesUnfused(g, -1.0);       // dry, negative depth
    for (int i = 0; i <= 4000; ++i) {
        const double h = g.y_full * 2.0 * static_cast<double>(i) / 4000.0;
        expectClosureAllMatchesUnfused(g, h);
    }
    expectClosureAllMatchesUnfused(g, g.y_crown);   // exactly at the taper start
    expectClosureAllMatchesUnfused(g, g.y_full);    // exactly at the crown
}

TEST(FvClosure, F1_ClosureAllIsBitIdenticalToUnfusedPath_OpenRect) {
    const FvGeometry g = makeRect(10.0, 4.0);
    expectClosureAllMatchesUnfused(g, 0.0);
    for (int i = 0; i <= 4000; ++i) {
        const double h = g.y_full * 3.0 * static_cast<double>(i) / 4000.0;
        expectClosureAllMatchesUnfused(g, h);
    }
    expectClosureAllMatchesUnfused(g, g.y_full);
}

// ---------------------------------------------------------------------------
// Compiled (XSECT_GEOMETRY EXACT) sections — Phase 6 / promptperf.md Phase E
// ---------------------------------------------------------------------------
//
// Two things changed for a section carrying a compiled Chebyshev boundary:
// closureAll reaches A/W/R through one fused evaluation, and depthOfArea
// seeds Newton from the compiled inverse instead of running Brent. The
// second is the risky one — depthOfArea must be a TRUE inverse of
// areaOfDepth, not merely an accurate one, or lake-at-rest fails on every
// partly-full pipe (see that function's own header). These pin the
// round-trip at the same tolerance the legacy path is held to, plus the two
// places the compiled path could diverge from it: inside the slot taper
// band, where the seed is deliberately an over-estimate Newton must walk
// back, and against the bracketed reference solver that remains the
// definition of the answer.

namespace {

/// Circular section of diameter @p d compiled to an exact arc boundary, the
/// way PostParseResolver does under XSECT_GEOMETRY EXACT. The ChebSection is
/// returned by reference-parameter because FvGeometry::xs.cheb points at it.
FvGeometry makeCompiledCircular(double d, openswmm::chebsec::ChebSection& cs,
                                double celerity = 100.0) {
    XSectParams xs = circular(d);
    std::vector<openswmm::xsboundary::BElem> elems;
    EXPECT_TRUE(openswmm::xsboundary::buildLegacyBoundary(
        openswmm::XSectShape::CIRCULAR, xs, elems));
    EXPECT_EQ(openswmm::chebsec::compile(cs, elems.data(),
                                         static_cast<int>(elems.size()), false), 0);
    xs.cheb = &cs;
    FvGeometry g;
    buildGeometry(xs, false, celerity, g);
    return g;
}

} // namespace

TEST(FvClosureCompiled, DepthAreaRoundTripsOnACompiledSection) {
    for (double d : {3.0, 0.5, 8.0}) {
        openswmm::chebsec::ChebSection cs{};
        const FvGeometry g = makeCompiledCircular(d, cs);
        ASSERT_NE(g.xs.cheb, nullptr) << "compiled boundary not attached";
        for (int i = 1; i <= 3000; ++i) {
            const double h = g.y_full * 2.0 * static_cast<double>(i) / 1500.0;
            const double a = k::areaOfDepth(g, h);
            EXPECT_NEAR(k::depthOfArea(g, a), h, 1.0e-8 * std::max(1.0, h))
                << "D=" << d << " round-trip failed at h=" << h;
        }
    }
}

TEST(FvClosureCompiled, DepthAreaRoundTripsInsideTheTaperBand) {
    // The band is where the compiled-inverse seed is knowingly wrong (part of
    // the area is slot, which chebYofA knows nothing about), so Newton has to
    // do real work here rather than confirming an already-exact guess.
    openswmm::chebsec::ChebSection cs{};
    const FvGeometry g = makeCompiledCircular(3.0, cs);
    for (int i = 0; i <= 2000; ++i) {
        const double h = g.y_crown +
                         (g.y_full - g.y_crown) * static_cast<double>(i) / 2000.0;
        const double a = k::areaOfDepth(g, h);
        EXPECT_NEAR(k::depthOfArea(g, a), h, 1.0e-9);
    }
}

TEST(FvClosureCompiled, NewtonAgreesWithTheBracketedReference) {
    // depthOfAreaBracketed is untouched by this work and remains the
    // definition of the root. The fast path must land on the same one.
    openswmm::chebsec::ChebSection cs{};
    const FvGeometry g = makeCompiledCircular(3.0, cs);
    for (int i = 1; i <= 2000; ++i) {
        const double a = g.a_crown * static_cast<double>(i) / 2000.0;
        const double h_fast = k::depthOfArea(g, a);
        const double h_ref  = k::depthOfAreaBracketed(g, a);
        EXPECT_NEAR(h_fast, h_ref, 1.0e-10 * g.y_full)
            << "diverged from the bracketed reference at a=" << a;
    }
}

TEST(FvClosureCompiled, FusedClosureAllMatchesTheIndividualAccessors) {
    // closureAll's compiled branch must equal areaOfDepth/widthOfDepth/
    // hydRadOfDepth/i1OfDepth at the same depth — the fusion is an
    // optimization, not a reformulation.
    openswmm::chebsec::ChebSection cs{};
    const FvGeometry g = makeCompiledCircular(3.0, cs);
    for (int i = 0; i <= 1200; ++i) {
        const double h = g.y_full * 1.4 * static_cast<double>(i) / 1000.0;
        double A = 0.0, W = 0.0, R = 0.0, I1 = 0.0;
        k::closureAll(g, h, &A, &W, &R, &I1);
        EXPECT_EQ(A, k::areaOfDepth(g, h))   << "at h=" << h;
        EXPECT_EQ(W, k::widthOfDepth(g, h))  << "at h=" << h;
        EXPECT_EQ(R, k::hydRadOfDepth(g, h)) << "at h=" << h;
        EXPECT_EQ(I1, k::i1OfDepth(g, h, A)) << "at h=" << h;
    }
}

TEST(FvClosureCompiled, ExactI1TableAgreesWithFineQuadrature) {
    // The closed form replacing Simpson (NetworkMeshBuilder::exactI1) must
    // integrate the SAME areaOfDepth the solver uses — slot and barrels
    // included, which is exactly what the phase spec's literal "fill from
    // chebI1ofY" would have dropped. Checked against a fine composite
    // Simpson of areaOfDepth itself, so the reference is the real integrand
    // rather than the compiled series.
    openswmm::chebsec::ChebSection cs{};
    const FvGeometry g = makeCompiledCircular(3.0, cs);

    auto refI1 = [&](double h) {
        const int m = 20000;                 // even -> composite Simpson
        const double dh = h / static_cast<double>(m);
        double s = k::areaOfDepth(g, 0.0) + k::areaOfDepth(g, h);
        for (int i = 1; i < m; ++i)
            s += ((i & 1) ? 4.0 : 2.0) * k::areaOfDepth(g, static_cast<double>(i) * dh);
        return s * dh / 3.0;
    };

    // Sample at EXACT table-node depths. At a node, i1OfDepth's trapezoid
    // refinement term is (h - h_i) = 0, so this reads the stored value and
    // isolates the closed form from the between-node interpolation (which is
    // second-order and unchanged by this work).
    const int n = static_cast<int>(openswmm::fv::kI1Samples);
    const double dh = g.y_full / static_cast<double>(n - 1);
    for (int i : {1, 2, 4, 8, 16, 32, 64, 96, 128}) {
        if (i > n - 1) continue;
        const double h = static_cast<double>(i) * dh;
        const double got = k::i1OfDepth(g, h, k::areaOfDepth(g, h));
        const double ref = refI1(h);
        // Tolerance keys off chebsec::kFitTol (1e-9), the accuracy the
        // compiled series is deliberately chopped to (ChebSection.hpp) — the
        // closed form cannot be more exact than the I1 series it reads, and
        // measured it lands right at that bound. Scaled by the field's FULL
        // range (i1_crown), not by the local value: a Chebyshev fit's error
        // is uniform across the piece, so near the invert — where I1 is five
        // orders below its crown value — the same absolute error is a large
        // RELATIVE one. Judging it locally would be holding the fit to a
        // tolerance it never claimed.
        EXPECT_NEAR(got, ref, 5.0e-9 * g.i1_crown)
            << "I1 node mismatch at h=" << h << " (I1=" << got
            << " ref=" << ref << ")";
    }

    // Is the closed form actually BETTER than the Simpson quadrature it
    // replaces? Compare both against the same fine reference, on the same
    // integrand, using the identical kSub=8 composite scheme buildI1Table
    // applies to a non-compiled section. This is the check that justifies
    // the substitution rather than assuming it: near the invert A ~ h^1.5
    // has an unbounded fourth derivative, which is exactly where Simpson's
    // error term blows up and a closed form does not care.
    auto simpsonNode = [&](int node) {
        constexpr int kSub = 8;
        const double hs = dh / static_cast<double>(2 * kSub);
        double acc = 0.0;
        for (int i = 1; i <= node; ++i) {
            const double h0 = static_cast<double>(i - 1) * dh;
            double sum = k::areaOfDepth(g, h0) + k::areaOfDepth(g, h0 + dh);
            for (int q = 1; q < 2 * kSub; ++q)
                sum += ((q & 1) ? 4.0 : 2.0) *
                       k::areaOfDepth(g, h0 + static_cast<double>(q) * hs);
            acc += sum * hs / 3.0;
        }
        return acc;
    };
    for (int node : {1, 2, 3}) {
        const double h = static_cast<double>(node) * dh;
        const double ref = refI1(h);
        const double e_exact = std::fabs(g.i1_tbl[static_cast<std::size_t>(node)] - ref);
        const double e_simp  = std::fabs(simpsonNode(node) - ref);
        EXPECT_LT(e_exact, e_simp)
            << "near the invert the closed form should beat Simpson: node "
            << node << " h=" << h << " exact_err=" << e_exact
            << " simpson_err=" << e_simp;
    }

    // I1 must stay monotone and start at zero — the properties the
    // well-balanced construction actually depends on.
    double prev = 0.0;
    for (int i = 0; i <= 400; ++i) {
        const double h = g.y_full * 1.2 * static_cast<double>(i) / 400.0;
        const double v = k::i1OfDepth(g, h, k::areaOfDepth(g, h));
        EXPECT_GE(v, prev - 1.0e-14) << "I1 decreased at h=" << h;
        prev = v;
    }
    EXPECT_EQ(k::i1OfDepth(g, 0.0, 0.0), 0.0);
}

// ---------------------------------------------------------------------------
// Test 19 (FV half) — a top width that collapses to zero at the crown
// ---------------------------------------------------------------------------

namespace {

/// Box `w` wide and `h_wall` tall closed by a triangular roof rising to a
/// POINT: the top width falls to zero LINEARLY at the crown. Compiled and
/// wrapped as an FvGeometry the way PostParseResolver + buildGeometry would.
FvGeometry makePointedRoof(openswmm::chebsec::ChebSection& cs,
                           double w, double h_wall, double h_apex,
                           double celerity = 100.0) {
    const double hw = 0.5 * w;
    const double px[5] = {-hw, hw, hw, 0.0, -hw};
    const double py[5] = {0.0, 0.0, h_wall, h_apex, h_wall};
    std::vector<openswmm::xsboundary::BElem> elems;
    EXPECT_EQ(openswmm::xsboundary::fromPolyline(px, py, 5, elems), 0);
    EXPECT_EQ(openswmm::chebsec::compile(cs, elems.data(),
                                         static_cast<int>(elems.size()), false), 0);
    XSectParams xs{};
    xs.type   = static_cast<int>(openswmm::XSectShape::POLYGON);
    xs.y_full = cs.y_full; xs.a_full = cs.a_full; xs.r_full = cs.r_full;
    xs.w_max  = cs.w_max;  xs.yw_max = cs.yw_max;
    xs.s_full = cs.s_full; xs.s_max  = cs.s_max;
    xs.cheb   = &cs;
    FvGeometry g;
    buildGeometry(xs, false, celerity, g);
    return g;
}

} // namespace

TEST(FvClosureCompiled, VanishingTopWidthStaysFiniteThroughTheClosure) {
    // Test 19. A section whose width goes to ZERO at the crown is the case
    // that turns a small fitting overshoot into a solver-wide NaN: celerity
    // is sqrt(g*A/B), so B <= 0 is either a division by zero or the square
    // root of a negative. The Preissmann slot is what makes this safe — the
    // ramp floors the width at t_slot before the geometric width reaches
    // zero — and this pins that the floor actually holds for a compiled
    // boundary, not just for the tabulated shapes the slot was written
    // against.
    //
    // A POINTED roof is used rather than a round one deliberately: it closes
    // LINEARLY, so it reaches small widths over a much wider band of depth
    // than a round crown's sqrt does, giving the slot ramp the most
    // opportunity to be caught mis-blending.
    openswmm::chebsec::ChebSection cs{};
    const FvGeometry g = makePointedRoof(cs, 4.0, 2.0, 3.0);
    ASSERT_NE(g.xs.cheb, nullptr);
    ASSERT_GT(g.t_slot, 0.0) << "a closed section must have a slot";

    double min_w = 1.0e300, max_cel = 0.0;
    for (int i = 0; i <= 40000; ++i) {
        const double h = 1.5 * g.y_full * static_cast<double>(i) / 40000.0;
        double A = 0.0, W = 0.0, R = 0.0, I1 = 0.0;
        k::closureAll(g, h, &A, &W, &R, &I1);
        ASSERT_TRUE(std::isfinite(A) && std::isfinite(W) &&
                    std::isfinite(R) && std::isfinite(I1))
            << "non-finite closure at h=" << h << " A=" << A << " W=" << W
            << " R=" << R << " I1=" << I1;
        ASSERT_GE(A, 0.0) << "negative area at h=" << h;
        if (h <= 1.0e-9) continue;         // the invert itself is legitimately dry
        ASSERT_GT(W, 0.0) << "top width reached zero at h=" << h
                          << " — celerity would be infinite";
        min_w = std::min(min_w, W);
        const double cel = std::sqrt(32.2 * A / W);
        ASSERT_TRUE(std::isfinite(cel)) << "non-finite celerity at h=" << h;
        max_cel = std::max(max_cel, cel);
    }

    // The floor is the slot width itself, not some arbitrary epsilon — that
    // is the mechanism, so assert the mechanism.
    EXPECT_GE(min_w, g.t_slot * (1.0 - 1.0e-12))
        << "width fell below the slot floor (min=" << min_w
        << " t_slot=" << g.t_slot << ")";
    // And the celerity the floor buys is the DESIGN celerity, so a future
    // change to the ramp that technically keeps W > 0 but lets it collapse
    // by orders would still be caught here.
    EXPECT_LT(max_cel, 1.05 * 100.0)
        << "slot celerity overshot its design value: " << max_cel;
}

// ---------------------------------------------------------------------------
// Test 20 — POLYGON circle vs. legacy CIRCULAR through the FV closure
// ---------------------------------------------------------------------------

namespace {

/// The same physical circle a user would write as a `[CURVES] XPOLYGON`:
/// four quarter-arc bulge points, the construction
/// test_xsect_boundary.cpp's FourQuarterArcBulgesFormAFullCircle proves
/// traces an exact circle.
FvGeometry makePolygonCircle(openswmm::chebsec::ChebSection& cs, double d,
                             double celerity = 100.0) {
    const double r = 0.5 * d;
    const double b = std::tan(0.25 * 3.14159265358979323846 / 2.0);
    const double px[4] = {r, 0.0, -r, 0.0};
    const double py[4] = {0.0, r, 0.0, -r};
    const double pb[4] = {b, b, b, b};
    std::vector<openswmm::xsboundary::BElem> elems;
    EXPECT_EQ(openswmm::xsboundary::fromArcSpec(px, py, pb, 4, elems), 0);
    EXPECT_EQ(openswmm::chebsec::compile(cs, elems.data(),
                                         static_cast<int>(elems.size()), false), 0);
    XSectParams xs = circular(d);       // identical scalars, per PostParseResolver
    xs.cheb = &cs;
    FvGeometry g;
    buildGeometry(xs, false, celerity, g);
    return g;
}

/// Exact circular-segment area — the outside ground truth neither backend
/// is built from.
double trueCircleArea(double h, double d) {
    if (h <= 0.0) return 0.0;
    if (h >= d) return 0.25 * 3.14159265358979323846 * d * d;
    const double th = 2.0 * std::acos(1.0 - 2.0 * h / d);
    return (d * d / 8.0) * (th - std::sin(th));
}

} // namespace

TEST(FvClosureCompiled, PolygonCircleAndLegacyCircularShareTheSlotClosureExactly) {
    // Test 20, first half. The plan asks for POLYGON-circle and CIRCULAR to
    // "agree to 1e-9 over [0, 1.5 y_full], slot region included". Measured,
    // that is FALSE below the crown and cannot be made true: the two are
    // different GEOMETRIES there (an exact arc vs. a 51-point interpolated
    // table) and disagree by up to ~1.2% around y/D = 0.02-0.05. Phase 6's
    // own spec text already corrects the plan on exactly this point — "the
    // two geometry sources are EXPECTED to disagree" — and the whole project
    // exists because the table is the wrong one.
    //
    // ABOVE the crown they agree BIT-EXACTLY, and the mechanism is worth
    // stating precisely rather than being read as "the two geometries
    // agree there", which is not what it shows. For h >= y_full,
    // FvKernels::areaOfDepth returns `a_crown + t_slot*(h - y_full)` and
    // widthOfDepth returns `t_slot` — no call into the section at all — and
    // buildGeometry derives all three of y_crown, t_slot and a_crown from
    // the XSectParams scalars y_full/a_full/w_max, which both modes take
    // from setParams()'s closed-form circle. So what is pinned here is that
    // the slot closure is a pure function of those shared scalars and is
    // INDEPENDENT of the geometry backend — which is the useful content of
    // "slot region included", and is exactly the property that would break
    // if someone routed the above-crown branch back through the section.
    //
    // Established by mutation, not assumed: perturbing the compiled circle's
    // radius by 1 part in 1e9 leaves every assertion below passing (the
    // compiled boundary is genuinely not consulted up there), which is why
    // the sub-crown disagreement is asserted too — without it this test
    // could be satisfied by two backends that were secretly the same object.
    const double d = 3.0;
    openswmm::chebsec::ChebSection cs{};
    const FvGeometry g_poly = makePolygonCircle(cs, d);
    const FvGeometry g_leg  = makeCircular(d);

    // The slot is parametrized off scalars both modes share, so these must
    // match before the sweep means anything.
    ASSERT_EQ(g_poly.y_full,  g_leg.y_full);
    ASSERT_EQ(g_poly.y_crown, g_leg.y_crown);
    ASSERT_EQ(g_poly.t_slot,  g_leg.t_slot);
    ASSERT_EQ(g_poly.a_crown, g_leg.a_crown);

    for (int i = 0; i <= 1500; ++i) {
        const double h = g_leg.y_full +
                         0.5 * g_leg.y_full * static_cast<double>(i) / 1500.0;
        EXPECT_EQ(k::areaOfDepth(g_poly, h),  k::areaOfDepth(g_leg, h))
            << "area diverged above the crown at h=" << h;
        EXPECT_EQ(k::widthOfDepth(g_poly, h), k::widthOfDepth(g_leg, h))
            << "width diverged above the crown at h=" << h;
    }

    // NON-VACUITY: below the crown these really are two different sections.
    // Without this the bit-exact block above would also pass if `cheb` had
    // silently failed to attach and both FvGeometry objects were evaluating
    // the identical legacy table.
    double worst_rel = 0.0;
    for (int i = 1; i < 1000; ++i) {
        const double h = 0.999 * d * static_cast<double>(i) / 1000.0;
        const double a_leg = k::areaOfDepth(g_leg, h);
        if (a_leg <= 0.0) continue;
        worst_rel = std::max(worst_rel,
                             std::fabs(k::areaOfDepth(g_poly, h) - a_leg) / a_leg);
    }
    EXPECT_GT(worst_rel, 1.0e-3)
        << "the two backends are indistinguishable below the crown ("
        << worst_rel << ") — the compiled boundary is probably not attached";
}

TEST(FvClosureCompiled, DepthAreaRoundTripsInBothGeometryBackends) {
    // Test 20, second half. The property the FV solver actually depends on
    // is not that the two backends agree with each other — it is that EACH
    // is self-consistent, because depthOfArea o areaOfDepth == identity is
    // what makes lake-at-rest hold (see depthOfArea's own header). Asserted
    // over the full [0, 1.5 y_full] the plan names, slot band included.
    const double d = 3.0;
    openswmm::chebsec::ChebSection cs{};
    const FvGeometry g_poly = makePolygonCircle(cs, d);
    const FvGeometry g_leg  = makeCircular(d);

    for (int i = 1; i <= 3000; ++i) {
        const double h = 1.5 * d * static_cast<double>(i) / 3000.0;
        SCOPED_TRACE(::testing::Message() << "h=" << h);
        EXPECT_NEAR(k::depthOfArea(g_poly, k::areaOfDepth(g_poly, h)), h, 1.0e-8);
        EXPECT_NEAR(k::depthOfArea(g_leg,  k::areaOfDepth(g_leg,  h)), h, 1.0e-8);
    }
}

TEST(FvClosureCompiled, WhereTheBackendsDisagreeTheCompiledOneIsRight) {
    // Test 20, third half — the one that turns the previous test's admitted
    // disagreement from a caveat into the project's actual claim. Both
    // backends are compared against the analytic circular-segment area,
    // which is outside ground truth for each of them.
    //
    // Measured over 999 depths: the compiled boundary is closer at 99.9% of
    // them and ~317x better on mean absolute error. Below y/D = 0.10 — the
    // dry-weather-flow regime, where self-cleansing velocity and sediment
    // initiation are decided — the legacy table's worst relative error is
    // 4.7 (i.e. 470%) against the compiled path's 4.5e-7. The bounds below
    // sit well inside those measurements so ordinary table or fit changes
    // do not trip them, while a regression that silently put the legacy
    // table back in front of the compiled boundary would.
    const double d = 3.0;
    openswmm::chebsec::ChebSection cs{};
    const FvGeometry g_poly = makePolygonCircle(cs, d);
    const FvGeometry g_leg  = makeCircular(d);

    double sum_leg = 0.0, sum_poly = 0.0;
    int poly_closer = 0, n = 0;
    for (int i = 1; i < 1000; ++i) {
        const double h = d * static_cast<double>(i) / 1000.0;
        const double a_true = trueCircleArea(h, d);
        const double e_leg  = std::fabs(k::areaOfDepth(g_leg,  h) - a_true);
        const double e_poly = std::fabs(k::areaOfDepth(g_poly, h) - a_true);
        sum_leg += e_leg;
        sum_poly += e_poly;
        if (e_poly < e_leg) ++poly_closer;
        ++n;
    }
    EXPECT_GT(poly_closer, static_cast<int>(0.95 * n))
        << "compiled boundary was closer to the true circle at only "
        << poly_closer << " of " << n << " depths";
    EXPECT_LT(sum_poly * 50.0, sum_leg)
        << "compiled mean error " << (sum_poly / n)
        << " vs legacy " << (sum_leg / n) << " — expected a large margin";

    // The low-fill band the project targets, stated on its own.
    double worst_leg = 0.0, worst_poly = 0.0;
    for (int i = 1; i <= 500; ++i) {
        const double h = 0.10 * d * static_cast<double>(i) / 500.0;
        const double a_true = trueCircleArea(h, d);
        ASSERT_GT(a_true, 0.0);
        worst_leg  = std::max(worst_leg,
                              std::fabs(k::areaOfDepth(g_leg,  h) - a_true) / a_true);
        worst_poly = std::max(worst_poly,
                              std::fabs(k::areaOfDepth(g_poly, h) - a_true) / a_true);
    }
    EXPECT_LT(worst_poly, 1.0e-4) << "compiled low-fill error " << worst_poly;
    EXPECT_GT(worst_leg,  0.5)    << "legacy low-fill error " << worst_leg
                                  << " — the table got better, re-check the premise";
    std::printf("[fv] low-fill (y/D<=0.10) worst rel err: legacy=%.3e compiled=%.3e\n",
                worst_leg, worst_poly);
}
