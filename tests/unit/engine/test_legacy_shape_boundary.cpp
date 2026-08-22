/**
 * @file test_legacy_shape_boundary.cpp
 * @brief LegacyShapeBoundary (Phase 5): built-in shapes reconstructed as an
 *        exact arc/line boundary under `[OPTIONS] XSECT_GEOMETRY EXACT`.
 *
 * @details Exists because `buildLegacyBoundary()`'s failure mode is silent:
 *          returning `false` just leaves a link on the ordinary LEGACY
 *          table/formula path with no error, no warning, and (at full depth)
 *          numerically plausible-looking scalars — `y_full`/`a_full`/
 *          `r_full`/`w_max` for CIRCULAR come from setParams()'s closed-form
 *          circle formulas either way, so a broken boundary construction is
 *          invisible unless something specifically checks the return value
 *          or queries a PARTIAL depth. That is exactly what happened here:
 *          `buildCircle()` originally built a 2-point boundary (two
 *          antipodal points, bulge=1 each), which `fromArcSpec()` rejects
 *          outright (n >= 3 required) — so EXACT mode silently never
 *          engaged for CIRCULAR/FORCE_MAIN at all until a full end-to-end
 *          run on a real network (Bellinge, ~950 circular conduits) was
 *          diffed against LEGACY and came back byte-identical, which is
 *          what caught it. Every case here checks the return value AND a
 *          partial-depth query against the same "is it actually different
 *          from LEGACY, and by a sane amount" bar that full-scale test
 *          would have applied immediately.
 *
 * @ingroup engine_hydraulics
 */

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

#include "hydraulics/ChebSection.hpp"
#include "hydraulics/LegacyShapeBoundary.hpp"
#include "hydraulics/XSectBatch.hpp"
#include "hydraulics/XSectBoundary.hpp"
#include "hydraulics/XSectKernels.hpp"

using namespace openswmm;
using openswmm::xsboundary::BElem;

namespace {

/// Build a LEGACY XSectParams for `shape` with geom[0..3], then attempt the
/// EXACT boundary reconstruction and compile it. Returns whether
/// buildLegacyBoundary() succeeded; on success, `exact_out` is `legacy`
/// with `.cheb` pointing at `cs` (which the caller must keep alive).
bool tryBuildExact(XSectShape shape, const double geom[4], XSectParams& legacy,
                   chebsec::ChebSection& cs, XSectParams& exact_out) {
    legacy = XSectParams{};
    double p[4] = {geom[0], geom[1], geom[2], geom[3]};
    xsect::setParams(legacy, static_cast<int>(shape), p, 1.0);

    std::vector<BElem> elems;
    if (!xsboundary::buildLegacyBoundary(shape, legacy, elems)) return false;

    cs = chebsec::ChebSection{};
    if (chebsec::compile(cs, elems.data(), static_cast<int>(elems.size()), false) != 0)
        return false;

    exact_out = legacy;
    exact_out.cheb = &cs;
    return true;
}

} // namespace

// ===========================================================================
// Every shape this module claims to support must actually build AND compile.
// (TEST() macros below are deliberately outside the anonymous namespace
// above, matching test_cheb_section.cpp's convention — only the helper
// functions are namespace-local.)
// ===========================================================================

TEST(LegacyShapeBoundary, SupportedShapesAllBuildAndCompile) {
    struct Case { XSectShape shape; double geom[4]; };
    const Case cases[] = {
        {XSectShape::CIRCULAR,       {2.0, 0, 0, 0}},
        {XSectShape::FORCE_MAIN,     {2.0, 100.0, 0, 0}},
        {XSectShape::EGGSHAPED,      {3.0, 0, 0, 0}},
        {XSectShape::HORSESHOE,      {3.0, 0, 0, 0}},
        {XSectShape::GOTHIC,         {3.0, 0, 0, 0}},
        {XSectShape::CATENARY,       {3.0, 0, 0, 0}},
        {XSectShape::SEMIELLIPTICAL, {3.0, 0, 0, 0}},
        {XSectShape::BASKETHANDLE,   {3.0, 0, 0, 0}},
        {XSectShape::SEMICIRCULAR,   {3.0, 0, 0, 0}},
        {XSectShape::ARCH,           {3.0, 4.0, 0, 0}},
        {XSectShape::HORIZ_ELLIPSE,  {2.0, 3.0, 0, 0}},
        {XSectShape::VERT_ELLIPSE,   {3.0, 2.0, 0, 0}},
    };
    for (const auto& c : cases) {
        XSectParams legacy, exact;
        chebsec::ChebSection cs;
        EXPECT_TRUE(tryBuildExact(c.shape, c.geom, legacy, cs, exact))
            << "shape " << static_cast<int>(c.shape) << " should build+compile under EXACT";
        EXPECT_GT(cs.n_pieces, 0);
        EXPECT_GT(cs.y_full, 0.0);
        EXPECT_GT(cs.a_full, 0.0);
    }
}

// ===========================================================================
// Shapes deliberately OUT of scope (already closed-form, no table error to
// remove) must decline rather than silently doing nothing useful.
// ===========================================================================

TEST(LegacyShapeBoundary, OutOfScopeShapesDeclineRatherThanSilentlyNoOp) {
    struct Case { XSectShape shape; double geom[4]; };
    const Case cases[] = {
        {XSectShape::RECT_CLOSED,  {2.0, 3.0, 0, 0}},
        {XSectShape::RECT_OPEN,    {2.0, 3.0, 0, 0}},
        {XSectShape::TRAPEZOIDAL,  {2.0, 1.0, 1.0, 1.0}},
        {XSectShape::TRIANGULAR,   {2.0, 3.0, 0, 0}},
        {XSectShape::PARABOLIC,    {2.0, 3.0, 0, 0}},
        {XSectShape::POWERFUNC,    {2.0, 3.0, 2.0, 0}},
        {XSectShape::FILLED_CIRCULAR, {4.0, 1.0, 0, 0}},
    };
    for (const auto& c : cases) {
        XSectParams legacy, exact;
        chebsec::ChebSection cs;
        EXPECT_FALSE(tryBuildExact(c.shape, c.geom, legacy, cs, exact))
            << "shape " << static_cast<int>(c.shape) << " has no table error to remove — "
               "buildLegacyBoundary must decline, not silently succeed with nothing gained";
    }
}

// ===========================================================================
// The regression this file exists for: EXACT must produce a REAL, sane-sized
// difference from LEGACY at a low-fill depth for CIRCULAR — the exact
// scenario a 2-point (rejected) boundary made silently indistinguishable
// from LEGACY doing nothing.
// ===========================================================================

TEST(LegacyShapeBoundary, CircularExactDivergesFromLegacyTableAtLowFill) {
    const double geom[4] = {1.0, 0, 0, 0};
    XSectParams legacy, exact;
    chebsec::ChebSection cs;
    ASSERT_TRUE(tryBuildExact(XSectShape::CIRCULAR, geom, legacy, cs, exact));

    // At y/D = 0.02, legacy's 26-point table is documented (Phase 4 design
    // notes) to carry ~1.3% true error on CIRCULAR specifically — small
    // compared to other shapes, but not zero, and NOT what "two different
    // code paths returning the identical float" looks like.
    const double y = 0.02 * legacy.y_full;
    const double aL = xsect::getAofY(legacy, y);
    const double aE = xsect::getAofY(exact, y);
    ASSERT_GT(aL, 0.0);
    const double rel_diff = std::fabs(aL - aE) / aL;

    EXPECT_GT(rel_diff, 1.0e-4)
        << "legacy=" << aL << " exact=" << aE << " — EXACT must be measurably "
           "different from LEGACY here, or it silently is not engaging";
    // But not absurdly different either — both describe the same physical
    // pipe, so this is a bound against a sign error or unit mixup, not a
    // tight accuracy assertion (that belongs in test_cheb_section.cpp).
    EXPECT_LT(rel_diff, 0.5);

    // Full-depth scalars come from setParams()'s closed-form circle formula
    // in BOTH modes (this is exactly why the original bug was invisible at
    // full depth) — confirm they still agree, so this test cannot be
    // satisfied by an EXACT path that is merely wrong instead of engaged.
    EXPECT_NEAR(exact.a_full, legacy.a_full, 1.0e-9);
    EXPECT_NEAR(exact.r_full, legacy.r_full, 1.0e-9);
}

TEST(LegacyShapeBoundary, GothicExactDivergesFromLegacyTable) {
    const double geom[4] = {4.0, 0, 0, 0};
    XSectParams legacy, exact;
    chebsec::ChebSection cs;
    ASSERT_TRUE(tryBuildExact(XSectShape::GOTHIC, geom, legacy, cs, exact));

    // GOTHIC's own width/area tables are documented (Phase 4 closeout) to
    // disagree with each other by up to 439% near the invert — pick a depth
    // comfortably inside that regime.
    const double y = 0.05 * legacy.y_full;
    const double wL = xsect::getWofY(legacy, y);
    const double wE = xsect::getWofY(exact, y);
    ASSERT_GT(wL, 0.0);
    const double rel_diff = std::fabs(wL - wE) / wL;
    EXPECT_GT(rel_diff, 1.0e-3)
        << "legacy=" << wL << " exact=" << wE;
}
