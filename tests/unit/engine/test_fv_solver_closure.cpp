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
