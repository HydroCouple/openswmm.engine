/**
 * @file test_xsect_parity.cpp
 * @brief Golden numerical-parity harness: new xsect:: vs legacy xsect.c.
 *
 * @details Links the legacy SWMM cross-section code (compiled into
 *          openswmm_legacy_engine) and diffs it against the new C++
 *          implementation (openswmm::xsect::) shape-by-shape across the full
 *          depth/area range. This is the authoritative parity gate referenced
 *          by docs/XSECT_PARITY_AND_PERF_REVIEW.md.
 *
 *          Two families of checks:
 *            1. **Getter parity** — seed BOTH sides from legacy xsect_setParams,
 *               then compare getAofY/getWofY/getRofY/getYofA/getSofA/getRofA/
 *               getAofS/getdSdA/getYcrit over a dense sweep. Isolates the getter
 *               math from any setup differences.
 *            2. **setParams parity** — compare the field output of new
 *               xsect::setParams against legacy xsect_setParams for the analytic
 *               shapes the new helper handles directly.
 *
 *          IRREGULAR/CUSTOM/STREET are intentionally NOT covered here: their
 *          legacy setters reach into global Transect[]/Street[]/Curve[]/Shape[]
 *          arrays. They are verified end-to-end in tests/regression instead.
 *
 * @ingroup engine_hydraulics
 */

#include <gtest/gtest.h>
#include <cmath>
#include <string>
#include <vector>

#include "../../src/engine/hydraulics/XSectBatch.hpp"

using namespace openswmm;

// ============================================================================
// Legacy ABI: replica of TXsect (objects.h) + C prototypes from xsect.c.
//
// Field order/types match src/legacy/engine/objects.h:592-610 exactly, so a
// pointer to this struct is layout-compatible with the legacy library.
// ============================================================================

extern "C" {

struct LegacyTXsect {
    int    type;
    int    culvertCode;
    int    transect;
    double yFull;
    double wMax;
    double ywMax;
    double aFull;
    double rFull;
    double sFull;
    double sMax;
    double yBot;
    double aBot;
    double sBot;
    double rBot;
};

// Legacy functions have C linkage (compiled as C). Declared with our replica
// struct, which is ABI-identical to the library's TXsect.
int    xsect_setParams(LegacyTXsect* xsect, int type, double p[], double ucf);
int    xsect_isOpen(int type);
double xsect_getAmax(LegacyTXsect* xsect);
double xsect_getSofA(LegacyTXsect* xsect, double area);
double xsect_getYofA(LegacyTXsect* xsect, double area);
double xsect_getRofA(LegacyTXsect* xsect, double area);
double xsect_getAofS(LegacyTXsect* xsect, double sFactor);
double xsect_getdSdA(LegacyTXsect* xsect, double area);
double xsect_getAofY(LegacyTXsect* xsect, double y);
double xsect_getRofY(LegacyTXsect* xsect, double y);
double xsect_getWofY(LegacyTXsect* xsect, double y);
double xsect_getYcrit(LegacyTXsect* xsect, double q);

} // extern "C"

// ============================================================================
// Helpers
// ============================================================================

namespace {

// Copy a fully-populated legacy TXsect into the new XSectParams (1:1 fields).
XSectParams fromLegacy(const LegacyTXsect& L) {
    XSectParams xs;
    xs.type         = L.type;
    xs.culvert_code = L.culvertCode;
    xs.transect     = L.transect;
    xs.y_full = L.yFull;
    xs.w_max  = L.wMax;
    xs.yw_max = L.ywMax;
    xs.a_full = L.aFull;
    xs.r_full = L.rFull;
    xs.s_full = L.sFull;
    xs.s_max  = L.sMax;
    xs.y_bot  = L.yBot;
    xs.a_bot  = L.aBot;
    xs.s_bot  = L.sBot;
    xs.r_bot  = L.rBot;
    return xs;
}

// Combined absolute+relative closeness, mirroring the regression-suite
// Tight tolerance (abs 1e-8, rel 1e-7) — the port is algorithmically identical
constexpr double kAbsTol = 1e-8;
constexpr double kRelTol = 1e-7;

::testing::AssertionResult Close(double legacy, double neu) {
    double diff = std::fabs(legacy - neu);
    double tol  = kAbsTol + kRelTol * std::fabs(legacy);
    if (diff <= tol) return ::testing::AssertionSuccess();
    return ::testing::AssertionFailure()
        << "legacy=" << legacy << " new=" << neu
        << " |diff|=" << diff << " tol=" << tol;
}

// One self-contained shape case (no global-array dependencies).
struct ShapeCase {
    const char* name;
    int         type;
    double      p[4];
};

// Shapes whose legacy setParams is self-contained (baked tables / formulas).
// Excludes IRREGULAR/CUSTOM/STREET (need globals).
const std::vector<ShapeCase>& selfContainedShapes() {
    static const std::vector<ShapeCase> cases = {
        {"CIRCULAR",        static_cast<int>(XSectShape::CIRCULAR),        {3.0, 0, 0, 0}},
        {"FORCE_MAIN",      static_cast<int>(XSectShape::FORCE_MAIN),      {3.0, 130.0, 0, 0}},
        {"FILLED_CIRCULAR", static_cast<int>(XSectShape::FILLED_CIRCULAR), {3.0, 0.75, 0, 0}},
        {"RECT_CLOSED",     static_cast<int>(XSectShape::RECT_CLOSED),     {3.0, 4.0, 0, 0}},
        {"RECT_OPEN_s0",    static_cast<int>(XSectShape::RECT_OPEN),       {3.0, 4.0, 0.0, 0}},
        {"RECT_OPEN_s1",    static_cast<int>(XSectShape::RECT_OPEN),       {3.0, 4.0, 1.0, 0}},
        {"RECT_OPEN_s2",    static_cast<int>(XSectShape::RECT_OPEN),       {3.0, 4.0, 2.0, 0}},
        {"RECT_TRIANG",     static_cast<int>(XSectShape::RECT_TRIANG),     {3.0, 4.0, 1.0, 0}},
        {"RECT_ROUND",      static_cast<int>(XSectShape::RECT_ROUND),      {3.0, 4.0, 2.5, 0}},
        {"MOD_BASKET",      static_cast<int>(XSectShape::MOD_BASKET),      {3.0, 4.0, 2.5, 0}},
        {"TRAPEZOIDAL",     static_cast<int>(XSectShape::TRAPEZOIDAL),     {2.0, 3.0, 1.0, 1.5}},
        {"TRIANGULAR",      static_cast<int>(XSectShape::TRIANGULAR),      {2.0, 4.0, 0, 0}},
        {"PARABOLIC",       static_cast<int>(XSectShape::PARABOLIC),       {2.0, 6.0, 0, 0}},
        {"POWERFUNC",       static_cast<int>(XSectShape::POWERFUNC),       {2.0, 5.0, 2.0, 0}},
        {"EGGSHAPED",       static_cast<int>(XSectShape::EGGSHAPED),       {3.0, 0, 0, 0}},
        {"HORSESHOE",       static_cast<int>(XSectShape::HORSESHOE),       {3.0, 0, 0, 0}},
        {"GOTHIC",          static_cast<int>(XSectShape::GOTHIC),          {3.0, 0, 0, 0}},
        {"CATENARY",        static_cast<int>(XSectShape::CATENARY),        {3.0, 0, 0, 0}},
        {"SEMIELLIPTICAL",  static_cast<int>(XSectShape::SEMIELLIPTICAL),  {3.0, 0, 0, 0}},
        {"BASKETHANDLE",    static_cast<int>(XSectShape::BASKETHANDLE),    {3.0, 0, 0, 0}},
        {"SEMICIRCULAR",    static_cast<int>(XSectShape::SEMICIRCULAR),    {3.0, 0, 0, 0}},
        {"HORIZ_ELLIPSE",   static_cast<int>(XSectShape::HORIZ_ELLIPSE),   {3.0, 4.5, 0, 0}},
        {"VERT_ELLIPSE",    static_cast<int>(XSectShape::VERT_ELLIPSE),    {4.5, 3.0, 0, 0}},
        {"ARCH",            static_cast<int>(XSectShape::ARCH),            {3.0, 4.0, 0, 0}},
    };
    return cases;
}

// Dense depth-fraction sweep — dense near 0 (small-area branches) and near
// the crown (near-full branches).
const std::vector<double>& depthFractions() {
    static const std::vector<double> f = {
        1e-4, 1e-3, 0.005, 0.01, 0.02, 0.05, 0.1, 0.2, 0.3, 0.4, 0.5,
        0.6, 0.7, 0.8, 0.9, 0.95, 0.98, 0.99, 0.999, 1.0
    };
    return f;
}

const std::vector<double>& areaFractions() {
    static const std::vector<double> f = {
        1e-4, 1e-3, 0.01, 0.05, 0.1, 0.25, 0.5, 0.75, 0.9, 0.95, 0.99, 1.0
    };
    return f;
}

} // namespace

// ============================================================================
// Getter parity — seed both sides from legacy setParams.
// ============================================================================

TEST(XSectParity, AreaOfDepth) {
    for (const auto& c : selfContainedShapes()) {
        LegacyTXsect L{};
        double p[4] = {c.p[0], c.p[1], c.p[2], c.p[3]};
        ASSERT_TRUE(xsect_setParams(&L, c.type, p, 1.0)) << c.name;
        XSectParams N = fromLegacy(L);

        for (double frac : depthFractions()) {
            double y = L.yFull * frac;
            SCOPED_TRACE(std::string(c.name) + " getAofY frac=" + std::to_string(frac));
            EXPECT_TRUE(Close(xsect_getAofY(&L, y), xsect::getAofY(N, y)));
        }
    }
}

TEST(XSectParity, WidthOfDepth) {
    for (const auto& c : selfContainedShapes()) {
        LegacyTXsect L{};
        double p[4] = {c.p[0], c.p[1], c.p[2], c.p[3]};
        ASSERT_TRUE(xsect_setParams(&L, c.type, p, 1.0)) << c.name;
        XSectParams N = fromLegacy(L);

        for (double frac : depthFractions()) {
            double y = L.yFull * frac;
            SCOPED_TRACE(std::string(c.name) + " getWofY frac=" + std::to_string(frac));
            EXPECT_TRUE(Close(xsect_getWofY(&L, y), xsect::getWofY(N, y)));
        }
    }
}

TEST(XSectParity, HydRadOfDepth) {
    for (const auto& c : selfContainedShapes()) {
        LegacyTXsect L{};
        double p[4] = {c.p[0], c.p[1], c.p[2], c.p[3]};
        ASSERT_TRUE(xsect_setParams(&L, c.type, p, 1.0)) << c.name;
        XSectParams N = fromLegacy(L);

        for (double frac : depthFractions()) {
            double y = L.yFull * frac;
            SCOPED_TRACE(std::string(c.name) + " getRofY frac=" + std::to_string(frac));
            EXPECT_TRUE(Close(xsect_getRofY(&L, y), xsect::getRofY(N, y)));
        }
    }
}

TEST(XSectParity, DepthOfArea) {
    for (const auto& c : selfContainedShapes()) {
        LegacyTXsect L{};
        double p[4] = {c.p[0], c.p[1], c.p[2], c.p[3]};
        ASSERT_TRUE(xsect_setParams(&L, c.type, p, 1.0)) << c.name;
        XSectParams N = fromLegacy(L);

        for (double frac : areaFractions()) {
            double a = L.aFull * frac;
            SCOPED_TRACE(std::string(c.name) + " getYofA frac=" + std::to_string(frac));
            EXPECT_TRUE(Close(xsect_getYofA(&L, a), xsect::getYofA(N, a)));
        }
    }
}

TEST(XSectParity, SectionFactorOfArea) {
    for (const auto& c : selfContainedShapes()) {
        LegacyTXsect L{};
        double p[4] = {c.p[0], c.p[1], c.p[2], c.p[3]};
        ASSERT_TRUE(xsect_setParams(&L, c.type, p, 1.0)) << c.name;
        XSectParams N = fromLegacy(L);

        for (double frac : areaFractions()) {
            double a = L.aFull * frac;
            SCOPED_TRACE(std::string(c.name) + " getSofA frac=" + std::to_string(frac));
            EXPECT_TRUE(Close(xsect_getSofA(&L, a), xsect::getSofA(N, a)));
        }
    }
}

TEST(XSectParity, HydRadOfArea) {
    for (const auto& c : selfContainedShapes()) {
        LegacyTXsect L{};
        double p[4] = {c.p[0], c.p[1], c.p[2], c.p[3]};
        ASSERT_TRUE(xsect_setParams(&L, c.type, p, 1.0)) << c.name;
        XSectParams N = fromLegacy(L);

        for (double frac : areaFractions()) {
            double a = L.aFull * frac;
            SCOPED_TRACE(std::string(c.name) + " getRofA frac=" + std::to_string(frac));
            EXPECT_TRUE(Close(xsect_getRofA(&L, a), xsect::getRofA(N, a)));
        }
    }
}

TEST(XSectParity, AreaOfSectionFactor) {
    for (const auto& c : selfContainedShapes()) {
        LegacyTXsect L{};
        double p[4] = {c.p[0], c.p[1], c.p[2], c.p[3]};
        ASSERT_TRUE(xsect_setParams(&L, c.type, p, 1.0)) << c.name;
        XSectParams N = fromLegacy(L);

        for (double frac : areaFractions()) {
            double a = L.aFull * frac;
            // Feed the SAME section factor (from legacy) into both inverses.
            double s = xsect_getSofA(&L, a);
            SCOPED_TRACE(std::string(c.name) + " getAofS frac=" + std::to_string(frac));
            EXPECT_TRUE(Close(xsect_getAofS(&L, s), xsect::getAofS(N, s)));
        }
    }
}

TEST(XSectParity, dSdAOfArea) {
    for (const auto& c : selfContainedShapes()) {
        LegacyTXsect L{};
        double p[4] = {c.p[0], c.p[1], c.p[2], c.p[3]};
        ASSERT_TRUE(xsect_setParams(&L, c.type, p, 1.0)) << c.name;
        XSectParams N = fromLegacy(L);

        for (double frac : areaFractions()) {
            double a = L.aFull * frac;
            SCOPED_TRACE(std::string(c.name) + " getdSdA frac=" + std::to_string(frac));
            EXPECT_TRUE(Close(xsect_getdSdA(&L, a), xsect::getdSdA(N, a)));
        }
    }
}

TEST(XSectParity, CriticalDepthOfFlow) {
    for (const auto& c : selfContainedShapes()) {
        LegacyTXsect L{};
        double p[4] = {c.p[0], c.p[1], c.p[2], c.p[3]};
        ASSERT_TRUE(xsect_setParams(&L, c.type, p, 1.0)) << c.name;
        XSectParams N = fromLegacy(L);

        // Sweep a few flows spanning sub- to super-critical for this geometry.
        for (double q : {0.05, 0.5, 2.0, 10.0, 50.0}) {
            SCOPED_TRACE(std::string(c.name) + " getYcrit q=" + std::to_string(q));
            EXPECT_TRUE(Close(xsect_getYcrit(&L, q), xsect::getYcrit(N, q)));
        }
    }
}

TEST(XSectParity, AmaxAndIsOpen) {
    for (const auto& c : selfContainedShapes()) {
        LegacyTXsect L{};
        double p[4] = {c.p[0], c.p[1], c.p[2], c.p[3]};
        ASSERT_TRUE(xsect_setParams(&L, c.type, p, 1.0)) << c.name;
        XSectParams N = fromLegacy(L);

        SCOPED_TRACE(std::string(c.name));
        // NOTE: legacy xsect_getAmax returns the ABSOLUTE area at max flow
        // (Amax[type] * aFull); the new xsect::getAmax returns the RATIO
        // Amax[type] (internal callers multiply by a_full). Compare with that
        // conversion applied.
        EXPECT_TRUE(Close(xsect_getAmax(&L), N.a_full * xsect::getAmax(N)));
        EXPECT_EQ(xsect_isOpen(c.type) != 0, xsect::isOpen(c.type));
    }
}

// ============================================================================
// setParams parity — compare new xsect::setParams field output vs legacy.
//
// Only the analytic shapes the new helper handles directly are checked here;
// tabulated shapes are populated by PostParseResolver in production, not by
// xsect::setParams, so they would spuriously fail.
// ============================================================================

TEST(XSectSetParamsParity, AnalyticShapeFields) {
    // All self-contained shapes (same set the harness diffs the getters on):
    // xsect::setParams must now match legacy xsect_setParams field-for-field, so
    // PostParseResolver can delegate to it as the single source of truth.
    const auto& handled = selfContainedShapes();

    for (const auto& c : handled) {
        LegacyTXsect L{};
        double pl[4] = {c.p[0], c.p[1], c.p[2], c.p[3]};
        ASSERT_TRUE(xsect_setParams(&L, c.type, pl, 1.0)) << c.name;

        XSectParams N;
        double pn[4] = {c.p[0], c.p[1], c.p[2], c.p[3]};
        xsect::setParams(N, c.type, pn, 1.0);

        SCOPED_TRACE(c.name);
        EXPECT_TRUE(Close(L.yFull, N.y_full)) << "y_full";
        EXPECT_TRUE(Close(L.wMax,  N.w_max))  << "w_max";
        EXPECT_TRUE(Close(L.ywMax, N.yw_max)) << "yw_max";
        EXPECT_TRUE(Close(L.aFull, N.a_full)) << "a_full";
        EXPECT_TRUE(Close(L.rFull, N.r_full)) << "r_full";
        EXPECT_TRUE(Close(L.sFull, N.s_full)) << "s_full";
        EXPECT_TRUE(Close(L.sMax,  N.s_max))  << "s_max";
        EXPECT_TRUE(Close(L.yBot,  N.y_bot))  << "y_bot";
        EXPECT_TRUE(Close(L.aBot,  N.a_bot))  << "a_bot";
        EXPECT_TRUE(Close(L.sBot,  N.s_bot))  << "s_bot";
        EXPECT_TRUE(Close(L.rBot,  N.r_bot))  << "r_bot";
    }
}

// ============================================================================
// Batch (SoA) parity — the production hot-loop path. Build XSectGroups from
// legacy-seeded params and diff its batch kernels against legacy per-element.
// ============================================================================

TEST(XSectBatchParity, AreasHydRadWidths) {
    const auto& shapes = selfContainedShapes();
    int n = static_cast<int>(shapes.size());

    std::vector<LegacyTXsect> L(static_cast<std::size_t>(n));
    std::vector<XSectParams>  N(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        double p[4] = {shapes[i].p[0], shapes[i].p[1], shapes[i].p[2], shapes[i].p[3]};
        ASSERT_TRUE(xsect_setParams(&L[static_cast<std::size_t>(i)], shapes[i].type, p, 1.0))
            << shapes[i].name;
        N[static_cast<std::size_t>(i)] = fromLegacy(L[static_cast<std::size_t>(i)]);
    }

    XSectGroups groups;
    groups.build(N.data(), n);

    std::vector<double> depths(n), areas(n), hyd(n), wid(n);
    for (double frac : depthFractions()) {
        for (int i = 0; i < n; ++i)
            depths[static_cast<std::size_t>(i)] = L[static_cast<std::size_t>(i)].yFull * frac;
        groups.computeAreas(depths.data(), areas.data(), n);
        groups.computeHydRad(depths.data(), hyd.data(), n);
        groups.computeWidths(depths.data(), wid.data(), n);

        for (int i = 0; i < n; ++i) {
            auto ui = static_cast<std::size_t>(i);
            double y = depths[ui];
            SCOPED_TRACE(std::string(shapes[i].name) + " frac=" + std::to_string(frac));
            EXPECT_TRUE(Close(xsect_getAofY(&L[ui], y), areas[ui])) << "batch area";
            EXPECT_TRUE(Close(xsect_getRofY(&L[ui], y), hyd[ui]))   << "batch hydrad";
            EXPECT_TRUE(Close(xsect_getWofY(&L[ui], y), wid[ui]))   << "batch width";
        }
    }
}
