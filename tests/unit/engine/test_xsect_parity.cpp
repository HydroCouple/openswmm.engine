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
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <limits>
#include <utility>
#include <string>
#include <vector>

#include "../../src/engine/hydraulics/XSectBatch.hpp"
#include "../../src/engine/hydraulics/xsect_tables.hpp"
#include "../../src/engine/hydraulics/XSectLookup.hpp"
#include "../../src/engine/hydraulics/XSectKernels.hpp"
#include "../../src/engine/hydraulics/Transect.hpp"
#include "../../src/engine/core/SimulationContext.hpp"

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

// ============================================================================
// Bit-exact parity — ULP == 0 vs legacy (the default-build mandate).
//
// The tolerance harness above proves the port is algorithmically identical; this
// section proves the circular / force-main / shared-lookup path reproduces legacy
// xsect.c to the LAST BIT. Because the legacy engine is compiled into this same
// executable on every CI OS, a green run here == bit-exact on
// Windows(MSVC) + Linux + macOS(arm64 + x86). See
// docs/plans/xsect_bitexact_vectorization.md §7.1.
//
// Legacy lookup()/the tables are `static` in xsect.c (not linkable), so we drive
// legacy lookup() through the exported getters with unit scale factors
// (aFull/rFull/wMax = 1, yFull = 1): for the lookup-based shapes
// xsect_getAofY(y) == 1.0 * lookup(y/1.0, table) == lookup(y, table) for y > 0.
// ============================================================================

namespace {

// Bit-exact comparison (ULP == 0). +0.0 and -0.0 count as equal (0 ULP);
// value-equal doubles are 0 ULP apart, everything else reports its ULP gap.
long long ulpGap(double a, double b) {
    if (a == b) return 0;                       // includes +0.0 == -0.0
    std::int64_t ia, ib;
    std::memcpy(&ia, &a, sizeof(ia));
    std::memcpy(&ib, &b, sizeof(ib));
    if (ia < 0) ia = static_cast<std::int64_t>(0x8000000000000000ULL) - ia;
    if (ib < 0) ib = static_cast<std::int64_t>(0x8000000000000000ULL) - ib;
    std::int64_t d = ia - ib;
    return d < 0 ? -d : d;
}

::testing::AssertionResult BitEq(double legacy, double neu) {
    if (legacy == neu) return ::testing::AssertionSuccess();   // ULP == 0
    return ::testing::AssertionFailure()
        << std::hexfloat << "legacy=" << legacy << " new=" << neu
        << std::defaultfloat << " ulp=" << ulpGap(legacy, neu);
}

// A unit-scale legacy TXsect so xsect_get{A,R,W}ofY act as legacy lookup() on the
// raw table (scale == 1, yFull == 1). Only the fields the circular/tabulated
// getters read are set.
LegacyTXsect unitLegacy(int type) {
    LegacyTXsect L{};
    L.type  = type;
    L.yFull = 1.0;
    L.aFull = 1.0;
    L.rFull = 1.0;
    L.wMax  = 1.0;
    return L;
}

// Shapes whose getAofY/getRofY/getWofY route entirely through lookup() — the
// set in scope for the ULP==0 mandate (circular + shared-table shapes).
const std::vector<ShapeCase>& lookupBasedShapes() {
    static const std::vector<ShapeCase> cases = {
        {"CIRCULAR",      static_cast<int>(XSectShape::CIRCULAR),      {3.0, 0,     0, 0}},
        {"FORCE_MAIN",    static_cast<int>(XSectShape::FORCE_MAIN),    {3.0, 130.0, 0, 0}},
        {"EGGSHAPED",     static_cast<int>(XSectShape::EGGSHAPED),     {3.0, 0,     0, 0}},
        {"HORSESHOE",     static_cast<int>(XSectShape::HORSESHOE),     {3.0, 0,     0, 0}},
        {"BASKETHANDLE",  static_cast<int>(XSectShape::BASKETHANDLE),  {3.0, 0,     0, 0}},
        {"HORIZ_ELLIPSE", static_cast<int>(XSectShape::HORIZ_ELLIPSE), {3.0, 4.5,   0, 0}},
        {"VERT_ELLIPSE",  static_cast<int>(XSectShape::VERT_ELLIPSE),  {4.5, 3.0,   0, 0}},
        {"ARCH",          static_cast<int>(XSectShape::ARCH),          {3.0, 4.0,   0, 0}},
    };
    return cases;
}

// Adversarial normalized-x set in [0, 1]: every table node k/50, x = 1.0, the
// nextafter neighbors of each node, and tiny x exercising the i<2 quadratic path.
std::vector<double> adversarialX(int n_items) {
    std::vector<double> xs;
    for (int k = 0; k <= n_items - 1; ++k) {
        double x = static_cast<double>(k) / static_cast<double>(n_items - 1);
        xs.push_back(x);
        xs.push_back(std::nextafter(x, 0.0));
        xs.push_back(std::nextafter(x, 1.0));
    }
    for (double t : {1e-9, 1e-8, 1e-7, 1e-6, 1e-5, 1e-4, 1e-3}) xs.push_back(t);
    xs.push_back(1.0);
    std::vector<double> out;
    for (double x : xs) if (x > 0.0 && x <= 1.0) out.push_back(x);  // lookup domain
    return out;
}

} // namespace

// Batch/fused kernels are bit-exact (ULP==0) in the default build, but the
// engine may be compiled with the fast reciprocal-multiply lookup on by default
// (-DOPENSWMM_FAST_XSECT_LOOKUP=ON → SWMM_XSECT_FAST_LOOKUP). In that build the
// batch kernels are tolerance-parity (abs 1e-8 / rel 1e-7), not bit-identical,
// so the batch/fused checks relax to the tolerance gate. The scalar lookup_exact
// checks below stay ULP==0 in BOTH builds (the scalar accessor always uses the
// exact core). Configure with -DOPENSWMM_FAST_XSECT_LOOKUP=OFF to hold the batch
// path to ULP==0.
#ifdef SWMM_XSECT_FAST_LOOKUP
#  define BATCH_EQ(legacy, got) Close((legacy), (got))
#else
#  define BATCH_EQ(legacy, got) BitEq((legacy), (got))
#endif

// (§7.1 bullet 1) lookup_exact vs legacy lookup() for A_Circ / R_Circ / W_Circ.
TEST(XSectBitExact, LookupCircularTablesVsLegacy) {
    using namespace xsect_tables;
    LegacyTXsect La = unitLegacy(static_cast<int>(XSectShape::CIRCULAR));  // area  -> A_Circ
    LegacyTXsect Lr = La;                                                  // hydrad-> R_Circ
    LegacyTXsect Lw = La;                                                  // width -> W_Circ

    for (double x : adversarialX(N_A_Circ)) {
        SCOPED_TRACE("x=" + std::to_string(x));
        EXPECT_TRUE(BitEq(xsect_getAofY(&La, x), xsect::lookup_exact(x, A_Circ, N_A_Circ))) << "A_Circ";
        EXPECT_TRUE(BitEq(xsect_getRofY(&Lr, x), xsect::lookup_exact(x, R_Circ, N_R_Circ))) << "R_Circ";
        EXPECT_TRUE(BitEq(xsect_getWofY(&Lw, x), xsect::lookup_exact(x, W_Circ, N_W_Circ))) << "W_Circ";
    }
}

// (§7.1 bullet 2) Per-element getters: new xsect:: vs legacy, ULP==0, across a
// depth sweep including y=0, y=yFull, y>yFull, nextafter(yFull), and tiny y.
TEST(XSectBitExact, ScalarGettersVsLegacy) {
    for (const auto& c : lookupBasedShapes()) {
        LegacyTXsect L{};
        double p[4] = {c.p[0], c.p[1], c.p[2], c.p[3]};
        ASSERT_TRUE(xsect_setParams(&L, c.type, p, 1.0)) << c.name;
        XSectParams N = fromLegacy(L);

        std::vector<double> ys;
        for (int k = 0; k <= 50; ++k) ys.push_back(L.yFull * (static_cast<double>(k) / 50.0));
        ys.push_back(0.0);
        ys.push_back(L.yFull);
        ys.push_back(std::nextafter(L.yFull, 0.0));
        ys.push_back(std::nextafter(L.yFull, L.yFull * 2.0));   // surcharge (y > yFull)
        ys.push_back(L.yFull * 1.5);
        for (double t : {1e-9, 1e-7, 1e-5, 1e-3}) ys.push_back(L.yFull * t);

        for (double y : ys) {
            SCOPED_TRACE(std::string(c.name) + " y=" + std::to_string(y));
            EXPECT_TRUE(BitEq(xsect_getAofY(&L, y), xsect::getAofY(N, y))) << "getAofY";
            EXPECT_TRUE(BitEq(xsect_getRofY(&L, y), xsect::getRofY(N, y))) << "getRofY";
            EXPECT_TRUE(BitEq(xsect_getWofY(&L, y), xsect::getWofY(N, y))) << "getWofY";
        }
    }
}

// (§7.1 bullet 3) Batch (SoA) kernels: computeAreas/HydRad/Widths AND the fused
// computeAreaAndHydRad vs legacy per-element, ULP==0. Catches the normalization
// deviation (depth*inv_y_full) and any SIMD-vs-scalar divergence.
TEST(XSectBitExact, BatchKernelsVsLegacy) {
    const auto& shapes = lookupBasedShapes();
    int n = static_cast<int>(shapes.size());

    std::vector<LegacyTXsect> L(static_cast<std::size_t>(n));
    std::vector<XSectParams>  N(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        auto ui = static_cast<std::size_t>(i);
        double p[4] = {shapes[i].p[0], shapes[i].p[1], shapes[i].p[2], shapes[i].p[3]};
        ASSERT_TRUE(xsect_setParams(&L[ui], shapes[i].type, p, 1.0)) << shapes[i].name;
        N[ui] = fromLegacy(L[ui]);
    }

    XSectGroups groups;
    groups.build(N.data(), n);

    // Depth fractions spanning dry, quadratic-refinement, mid, exactly-full, and
    // surcharge — each link uses its own yFull so a single frac hits every shape.
    std::vector<double> fracs = {0.0, 1e-9, 1e-5, 1e-3, 0.02, 0.04, 0.1, 0.5,
                                 0.9, 0.98, 0.999, 1.0, 1.25};

    std::vector<double> depths(static_cast<std::size_t>(n));
    std::vector<double> areas(static_cast<std::size_t>(n)), hyd(static_cast<std::size_t>(n));
    std::vector<double> wid(static_cast<std::size_t>(n));
    std::vector<double> fa(static_cast<std::size_t>(n)), fr(static_cast<std::size_t>(n));

    for (double frac : fracs) {
        for (int i = 0; i < n; ++i)
            depths[static_cast<std::size_t>(i)] = L[static_cast<std::size_t>(i)].yFull * frac;

        groups.computeAreas(depths.data(), areas.data(), n);
        groups.computeHydRad(depths.data(), hyd.data(), n);
        groups.computeWidths(depths.data(), wid.data(), n);
        groups.computeAreaAndHydRad(depths.data(), fa.data(), fr.data(), n);  // fused kernel

        for (int i = 0; i < n; ++i) {
            auto ui = static_cast<std::size_t>(i);
            double y = depths[ui];
            SCOPED_TRACE(std::string(shapes[i].name) + " frac=" + std::to_string(frac));
            EXPECT_TRUE(BATCH_EQ(xsect_getAofY(&L[ui], y), areas[ui])) << "batch area";
            EXPECT_TRUE(BATCH_EQ(xsect_getRofY(&L[ui], y), hyd[ui]))   << "batch hydrad";
            EXPECT_TRUE(BATCH_EQ(xsect_getWofY(&L[ui], y), wid[ui]))   << "batch width";
            EXPECT_TRUE(BATCH_EQ(xsect_getAofY(&L[ui], y), fa[ui]))    << "fused area";
            EXPECT_TRUE(BATCH_EQ(xsect_getRofY(&L[ui], y), fr[ui]))    << "fused hydrad";
        }
    }
}

// (§7.2) Cross-platform drift guard: hash the circular-kernel output bytes over
// a fixed synthetic input and compare against a committed golden. The kernels
// use only IEEE division + non-FMA mul/add, so they are byte-identical on every
// little-endian CI OS (Windows/MSVC-x64, Linux-x64, macOS-arm64, macOS-x86). A
// mismatch means one platform diverged — most likely an accidental FMA/NEON
// fusion sneaking past -ffp-contract=off (see plan §3). This is insurance on top
// of the legacy-linked ULP==0 tests above. If the kernel math legitimately
// changes, recompute the golden (the failure message prints the actual hash).
TEST(XSectBitExact, GoldenHashCircularKernels) {
    constexpr int M = 256;
    std::vector<double> depth(M), yfull(M), afull(M), rfull(M), wmax(M);
    for (int k = 0; k < M; ++k) {
        auto uk = static_cast<std::size_t>(k);
        double yf = 1.0 + 0.01 * k;                 // varied full depth
        yfull[uk] = yf;
        afull[uk] = 0.7853981633974483 * yf * yf;   // pi/4 * D^2
        rfull[uk] = 0.25 * yf;
        wmax[uk]  = yf;
        depth[uk] = yf * (static_cast<double>(k) / (M - 1)) * 1.2;  // 0 .. 1.2*yf (surcharge)
    }
    std::vector<double> area(M), hyd(M), wid(M);
    xsect_batch::area_hydrad_circular(depth.data(), yfull.data(), afull.data(),
                                      rfull.data(), area.data(), hyd.data(), M);
    xsect_batch::width_circular(depth.data(), yfull.data(), wmax.data(), wid.data(), M);

    auto fnv1a = [](const std::vector<double>& v, std::uint64_t h) {
        const auto* p = reinterpret_cast<const unsigned char*>(v.data());
        std::size_t nbytes = v.size() * sizeof(double);
        for (std::size_t i = 0; i < nbytes; ++i) { h ^= p[i]; h *= 1099511628211ULL; }
        return h;
    };
    std::uint64_t h = 1469598103934665603ULL;
    h = fnv1a(area, h);
    h = fnv1a(hyd, h);
    h = fnv1a(wid, h);

    // The golden depends on the lookup variant the engine was built with.
#ifdef SWMM_XSECT_FAST_LOOKUP
    constexpr std::uint64_t kGolden = 0x4bb6291e0b8b9381ULL;  // fast reciprocal-multiply lookup
#else
    constexpr std::uint64_t kGolden = 0xfcd310e47838c666ULL;  // bit-exact divide form
#endif
    EXPECT_EQ(h, kGolden) << "circular-kernel golden hash drift — actual=0x"
                          << std::hex << h << std::dec
                          << " (recompute golden if the kernel math changed; see plan §7.2)";
}

// ============================================================================
// §6 fast-mode tolerance gate. The reciprocal-multiply lookup (opt-in
// -DSWMM_XSECT_FAST_LOOKUP) is NOT bit-exact; the plan requires it to pass the
// existing abs 1e-8 / rel 1e-7 gate AND have its bound asserted. lookup_fast is
// always compiled, so we can verify its deviation from legacy here regardless of
// the default build mode.
// ============================================================================
TEST(XSectFastMode, LookupFastWithinTolerance) {
    using namespace xsect_tables;
    LegacyTXsect La = unitLegacy(static_cast<int>(XSectShape::CIRCULAR));  // -> A_Circ
    LegacyTXsect Lr = La;                                                  // -> R_Circ
    LegacyTXsect Lw = La;                                                  // -> W_Circ

    double max_rel = 0.0, max_abs = 0.0;
    auto check = [&](double legacy, double fast) {
        EXPECT_TRUE(Close(legacy, fast));                    // the §6 gate (abs 1e-8/rel 1e-7)
        double d = std::fabs(legacy - fast);
        max_abs = std::max(max_abs, d);
        if (std::fabs(legacy) > 0.0) max_rel = std::max(max_rel, d / std::fabs(legacy));
    };
    for (double x : adversarialX(N_A_Circ)) {
        SCOPED_TRACE("x=" + std::to_string(x));
        check(xsect_getAofY(&La, x), xsect::lookup_fast(x, A_Circ, N_A_Circ));
        check(xsect_getRofY(&Lr, x), xsect::lookup_fast(x, R_Circ, N_R_Circ));
        check(xsect_getWofY(&Lw, x), xsect::lookup_fast(x, W_Circ, N_W_Circ));
    }
    // Documented (asserted) bound — the measured worst case is ~1e-7 rel at the
    // exactly-full node where fast pre-clamps to table[n-1] instead of legacy's
    // last-segment interpolation. Well inside the tolerance gate.
    RecordProperty("fast_lookup_max_rel", std::to_string(max_rel));
    RecordProperty("fast_lookup_max_abs", std::to_string(max_abs));
    EXPECT_LT(max_rel, 1e-6) << "fast-lookup rel deviation regressed; max_rel=" << max_rel;
}

// ============================================================================
// Lookup-acceleration identity gate — plans/XSECT_LOOKUP_ACCEL_PLAN.md Phase 1/2.
//
// A1 (bucket LUT), A2 (fused tabulated pair) and A3 (transect-grouped element
// order) are the plan's BIT-EXACT set: each is a pure restatement of how the
// existing arithmetic is reached, never of the arithmetic itself. That claim is
// only worth anything if it is pinned, so these tests keep the pre-change forms
// compiled — plain bisection, two separate lookup passes, the unsorted element
// order — and assert memcmp-level equality against them, not a tolerance.
//
// Note this holds in the SWMM_XSECT_FAST_LOOKUP build too (unlike the
// legacy-parity checks above, which relax there): both sides of every
// comparison here are the SAME lookup variant, so the fast mode's ULP
// deviation from legacy cancels out and the identity is exact either way.
// ============================================================================

namespace {

using openswmm::xsect::LocateLut;
using openswmm::xsect::LutId;

// Values that stress the bracket logic: every table node, both nextafter
// neighbours of each node, cell midpoints, and points outside the value range
// (which locate() resolves through its own clamps).
std::vector<double> adversarialTableValues(const double* t, int n) {
    std::vector<double> v;
    for (int k = 0; k < n; ++k) {
        v.push_back(t[k]);
        v.push_back(std::nextafter(t[k], -1e300));
        v.push_back(std::nextafter(t[k], 1e300));
        if (k + 1 < n) v.push_back(0.5 * (t[k] + t[k + 1]));
    }
    for (double x : {-1.0, -1e-300, 0.0, 1e-300, 1.0, 1.5, 2.0, 1e6}) v.push_back(x);
    return v;
}

// A handful of real transect tables, built by the shipped builder from
// station/elevation data (a V notch, a wide flood plain with a deep channel,
// a near-trapezoid, and an asymmetric section) — NOT hand-written tables, so
// the flat spans and clustered rows a real transect produces are exercised.
struct TestTransect {
    openswmm::transect::TransectData td;
};

const std::vector<TestTransect>& testTransects() {
    static const std::vector<TestTransect> tts = [] {
        std::vector<TestTransect> out;
        const std::vector<std::pair<std::vector<double>, std::vector<double>>> sections = {
            {{0, 5, 10},                    {10, 0, 10}},                    // V notch
            {{0, 40, 45, 50, 55, 60, 100},  {12, 10, 2, 0, 2, 10, 12}},      // flood plain
            {{0, 2, 12, 14},                {6, 0, 0, 6}},                   // trapezoid
            {{0, 3, 9, 30, 33},             {8, 1.5, 0, 4, 9}},              // asymmetric
        };
        for (std::size_t i = 0; i < sections.size(); ++i) {
            TestTransect tt;
            tt.td.name       = "T" + std::to_string(i);
            tt.td.stations   = sections[i].first;
            tt.td.elevations = sections[i].second;
            tt.td.n_channel  = 0.03;
            tt.td.n_left     = 0.05;
            tt.td.n_right    = 0.05;
            tt.td.x_left_bank  = sections[i].first.front();
            tt.td.x_right_bank = sections[i].first.back();
            openswmm::transect::buildTables(tt.td);
            out.push_back(std::move(tt));
        }
        return out;
    }();
    return tts;
}

}  // namespace

// A1 — the bucket map returns locate()'s index, not merely a nearby one. This
// is the whole basis of the bit-exactness claim: same index in, same
// interpolation arithmetic out.
TEST(XSectAccel, BucketLutReturnsIdenticalIndex) {
    struct Case { const char* name; const double* t; int n; };
    using namespace xsect_tables;
    const std::vector<Case> tables = {
        {"Y_Gothic", Y_Gothic, N_Y_Gothic},
        {"Y_Catenary", Y_Catenary, N_Y_Catenary},
        {"Y_SemiEllip", Y_SemiEllip, N_Y_SemiEllip},
        {"Y_SemiCirc", Y_SemiCirc, N_Y_SemiCirc},
        {"A_HorizEllipse", A_HorizEllipse, N_A_HorizEllipse},
        {"A_VertEllipse", A_VertEllipse, N_A_VertEllipse},
        {"A_Arch", A_Arch, N_A_Arch},
        {"S_Circ", S_Circ, N_S_Circ},
        {"S_Egg", S_Egg, N_S_Egg},
        {"S_Horseshoe", S_Horseshoe, N_S_Horseshoe},
        {"S_Gothic", S_Gothic, N_S_Gothic},
        {"S_Catenary", S_Catenary, N_S_Catenary},
        {"S_SemiEllip", S_SemiEllip, N_S_SemiEllip},
        {"S_BasketHandle", S_BasketHandle, N_S_BasketHandle},
        {"S_SemiCirc", S_SemiCirc, N_S_SemiCirc},
    };

    int checked = 0;
    for (const auto& c : tables) {
        SCOPED_TRACE(c.name);
        LocateLut lut{};
        openswmm::xsect::build_invlookup_lut(lut, c.t, c.n);
        ASSERT_GT(lut.scale, 0.0) << "no map built for " << c.name;
        const int jLast = lut.j_last;
        for (double y : adversarialTableValues(c.t, jLast + 1)) {
            EXPECT_EQ(openswmm::xsect::locate_lut(y, c.t, jLast, lut),
                      openswmm::xsect::locate_bisect(y, c.t, jLast))
                << "y=" << y;
            ++checked;
        }
    }
    // NaN resolves to index 0 in plain bisection (every `y >= t[j]` is false);
    // the map must not diverge there either.
    for (const auto& c : tables) {
        LocateLut lut{};
        openswmm::xsect::build_invlookup_lut(lut, c.t, c.n);
        const double nan = std::numeric_limits<double>::quiet_NaN();
        EXPECT_EQ(openswmm::xsect::locate_lut(nan, c.t, lut.j_last, lut),
                  openswmm::xsect::locate_bisect(nan, c.t, lut.j_last)) << c.name;
    }
    EXPECT_GT(checked, 2000);
}

// A1 — same, on real transect area tables (the getYofA IRREGULAR path).
TEST(XSectAccel, BucketLutIdenticalOnTransectTables) {
    for (const auto& tt : testTransects()) {
        SCOPED_TRACE(tt.td.name);
        ASSERT_GT(tt.td.a_full, 0.0);
        ASSERT_GT(tt.td.area_lut.scale, 0.0) << "no map built for this transect";
        const int jLast = tt.td.area_lut.j_last;
        for (double y : adversarialTableValues(tt.td.area_tbl, jLast + 1)) {
            EXPECT_EQ(openswmm::xsect::locate_lut(y, tt.td.area_tbl, jLast, tt.td.area_lut),
                      openswmm::xsect::locate_bisect(y, tt.td.area_tbl, jLast))
                << "y=" << y;
        }
    }
}

// A1 — the interpolated value, not just the index: invLookup with and without
// the map, ULP == 0. Covers the S-tables' two-row top truncation, which is
// resolved outside locate() and so must be unaffected by the map.
TEST(XSectAccel, InvLookupWithMapIsBitIdentical) {
    const xsect::XsectEval& ev = xsect::hostEval();
    struct Case { const char* name; const double* t; int n; LutId id; };
    using namespace xsect_tables;
    const std::vector<Case> tables = {
        {"Y_Gothic", Y_Gothic, N_Y_Gothic, LutId::Y_Gothic},
        {"Y_Catenary", Y_Catenary, N_Y_Catenary, LutId::Y_Catenary},
        {"Y_SemiEllip", Y_SemiEllip, N_Y_SemiEllip, LutId::Y_SemiEllip},
        {"Y_SemiCirc", Y_SemiCirc, N_Y_SemiCirc, LutId::Y_SemiCirc},
        {"A_HorizEllipse", A_HorizEllipse, N_A_HorizEllipse, LutId::A_HorizEllipse},
        {"A_VertEllipse", A_VertEllipse, N_A_VertEllipse, LutId::A_VertEllipse},
        {"A_Arch", A_Arch, N_A_Arch, LutId::A_Arch},
        {"S_Circ", S_Circ, N_S_Circ, LutId::S_Circ},
        {"S_Egg", S_Egg, N_S_Egg, LutId::S_Egg},
        {"S_Horseshoe", S_Horseshoe, N_S_Horseshoe, LutId::S_Horseshoe},
        {"S_Gothic", S_Gothic, N_S_Gothic, LutId::S_Gothic},
        {"S_Catenary", S_Catenary, N_S_Catenary, LutId::S_Catenary},
        {"S_SemiEllip", S_SemiEllip, N_S_SemiEllip, LutId::S_SemiEllip},
        {"S_BasketHandle", S_BasketHandle, N_S_BasketHandle, LutId::S_BasketHandle},
        {"S_SemiCirc", S_SemiCirc, N_S_SemiCirc, LutId::S_SemiCirc},
    };
    for (const auto& c : tables) {
        SCOPED_TRACE(c.name);
        const LocateLut* lut = xsect::hostTables().lut(c.id);
        ASSERT_NE(lut, nullptr);
        for (double y : adversarialTableValues(c.t, c.n)) {
            EXPECT_TRUE(BitEq(ev.invLookup(y, c.t, c.n, nullptr),
                              ev.invLookup(y, c.t, c.n, lut))) << "y=" << y;
        }
    }
    for (const auto& tt : testTransects()) {
        SCOPED_TRACE(tt.td.name);
        const int n = openswmm::transect::N_TRANSECT_TBL;
        for (double y : adversarialTableValues(tt.td.area_tbl, n)) {
            EXPECT_TRUE(BitEq(ev.invLookup(y, tt.td.area_tbl, n, nullptr),
                              ev.invLookup(y, tt.td.area_tbl, n, &tt.td.area_lut)))
                << "y=" << y;
        }
    }
}

// A1 — end to end through the dispatchers the solvers actually call, against an
// evaluator with the maps unbound (`luts == nullptr`), which is exactly the
// pre-change code path.
TEST(XSectAccel, DispatchersBitIdenticalWithAndWithoutMaps) {
    xsect::XsectTables no_luts = xsect::hostTables();
    no_luts.luts = nullptr;
    const xsect::XsectEval bisecting{no_luts};
    const xsect::XsectEval& mapped = xsect::hostEval();

    for (const auto& c : selfContainedShapes()) {
        SCOPED_TRACE(c.name);
        LegacyTXsect L{};
        xsect_setParams(&L, c.type, const_cast<double*>(c.p), 1.0);
        const XSectParams xs = fromLegacy(L);
        for (double f : areaFractions()) {
            const double a = f * xs.a_full;
            EXPECT_TRUE(BitEq(bisecting.getYofA(xs, a), mapped.getYofA(xs, a))) << "f=" << f;
            EXPECT_TRUE(BitEq(bisecting.getRofA(xs, a), mapped.getRofA(xs, a))) << "f=" << f;
        }
        for (double f : areaFractions()) {
            const double s = f * xs.s_full;
            EXPECT_TRUE(BitEq(bisecting.getAofS(xs, s), mapped.getAofS(xs, s))) << "f=" << f;
        }
        for (double f : depthFractions()) {
            const double y = f * xs.y_full;
            EXPECT_TRUE(BitEq(bisecting.getAofY(xs, y), mapped.getAofY(xs, y))) << "f=" << f;
        }
    }

    // The transect path: same comparison, with the per-link map bound or not.
    for (const auto& tt : testTransects()) {
        SCOPED_TRACE(tt.td.name);
        XSectParams xs;
        xs.type   = static_cast<int>(XSectShape::IRREGULAR);
        xs.y_full = tt.td.y_full;
        xs.a_full = tt.td.a_full;
        xs.r_full = tt.td.r_full;
        xs.w_max  = tt.td.w_max;
        xs.area_tbl  = tt.td.area_tbl;
        xs.hrad_tbl  = tt.td.hrad_tbl;
        xs.width_tbl = tt.td.width_tbl;
        xs.transect_tbl_size = openswmm::transect::N_TRANSECT_TBL;
        XSectParams xs_mapped = xs;
        xs_mapped.area_lut = &tt.td.area_lut;
        for (double f : areaFractions()) {
            const double a = f * xs.a_full;
            EXPECT_TRUE(BitEq(mapped.getYofA(xs, a), mapped.getYofA(xs_mapped, a)))
                << "f=" << f;
        }
    }
}

// A2 — the fused area+hyd-radius pass against the two separate passes it
// replaces, over a batch of links deliberately chasing DIFFERENT transects.
TEST(XSectAccel, FusedTabulatedPairIsBitIdentical) {
    const auto& tts = testTransects();
    ASSERT_FALSE(tts.empty());
    constexpr int kLinks = 97;                 // prime: no alignment with 4 transects
    constexpr int n = openswmm::transect::N_TRANSECT_TBL;

    std::vector<double> depth(kLinks), nrm(kLinks), afull(kLinks), rfull(kLinks);
    std::vector<const double*> ta(kLinks), tb(kLinks);
    for (int k = 0; k < kLinks; ++k) {
        const auto& td = tts[static_cast<std::size_t>(k) % tts.size()].td;
        const auto uk = static_cast<std::size_t>(k);
        // Sweep depth from below zero (the y<=0 guard) through surcharge.
        depth[uk] = td.y_full * (-0.05 + 1.15 * static_cast<double>(k) / (kLinks - 1));
        nrm[uk]   = td.y_full;
        afull[uk] = td.a_full;
        rfull[uk] = td.r_full;
        ta[uk]    = td.area_tbl;
        tb[uk]    = td.hrad_tbl;
    }
#ifdef SWMM_XSECT_FAST_LOOKUP
    for (int k = 0; k < kLinks; ++k) {         // the kernels take 1/y_full here
        auto uk = static_cast<std::size_t>(k);
        nrm[uk] = (nrm[uk] > 0.0) ? 1.0 / nrm[uk] : 0.0;
    }
#endif

    std::vector<double> ref_a(kLinks), ref_b(kLinks), got_a(kLinks), got_b(kLinks);
    xsect_batch::perlink_tabulated(depth.data(), nrm.data(), afull.data(),
                                   ta.data(), n, ref_a.data(), kLinks);
    xsect_batch::perlink_tabulated(depth.data(), nrm.data(), rfull.data(),
                                   tb.data(), n, ref_b.data(), kLinks);
    xsect_batch::perlink_tabulated_pair(depth.data(), nrm.data(),
                                        afull.data(), rfull.data(),
                                        ta.data(), tb.data(), n,
                                        got_a.data(), got_b.data(), kLinks);
    EXPECT_EQ(0, std::memcmp(ref_a.data(), got_a.data(), ref_a.size() * sizeof(double)));
    EXPECT_EQ(0, std::memcmp(ref_b.data(), got_b.data(), ref_b.size() * sizeof(double)));
    for (int k = 0; k < kLinks; ++k) {         // per-element message on failure
        auto uk = static_cast<std::size_t>(k);
        EXPECT_TRUE(BitEq(ref_a[uk], got_a[uk])) << "area k=" << k;
        EXPECT_TRUE(BitEq(ref_b[uk], got_b[uk])) << "hydrad k=" << k;
    }

    // A missing table on one side must still behave like the unfused pair.
    ta[3] = nullptr;
    tb[7] = nullptr;
    xsect_batch::perlink_tabulated(depth.data(), nrm.data(), afull.data(),
                                   ta.data(), n, ref_a.data(), kLinks);
    xsect_batch::perlink_tabulated(depth.data(), nrm.data(), rfull.data(),
                                   tb.data(), n, ref_b.data(), kLinks);
    xsect_batch::perlink_tabulated_pair(depth.data(), nrm.data(),
                                        afull.data(), rfull.data(),
                                        ta.data(), tb.data(), n,
                                        got_a.data(), got_b.data(), kLinks);
    EXPECT_EQ(0, std::memcmp(ref_a.data(), got_a.data(), ref_a.size() * sizeof(double)));
    EXPECT_EQ(0, std::memcmp(ref_b.data(), got_b.data(), ref_b.size() * sizeof(double)));
}

// A3 — the reordering is only sound because the tabulated kernels are
// elementwise. Feed the same links in two different orders and require each
// link's result to be identical, which is precisely what the init-time sort
// relies on.
TEST(XSectAccel, TabulatedKernelsArePositionIndependent) {
    const auto& tts = testTransects();
    constexpr int kLinks = 64;
    constexpr int n = openswmm::transect::N_TRANSECT_TBL;

    std::vector<double> depth(kLinks), nrm(kLinks), afull(kLinks), rfull(kLinks);
    std::vector<const double*> ta(kLinks), tb(kLinks);
    for (int k = 0; k < kLinks; ++k) {
        // Interleave transects, the ordering the sort exists to undo.
        const auto& td = tts[static_cast<std::size_t>(k) % tts.size()].td;
        const auto uk = static_cast<std::size_t>(k);
        depth[uk] = td.y_full * (0.02 + 0.96 * static_cast<double>(k) / (kLinks - 1));
        nrm[uk]   = td.y_full;
        afull[uk] = td.a_full;
        rfull[uk] = td.r_full;
        ta[uk]    = td.area_tbl;
        tb[uk]    = td.hrad_tbl;
    }
#ifdef SWMM_XSECT_FAST_LOOKUP
    for (int k = 0; k < kLinks; ++k) {
        auto uk = static_cast<std::size_t>(k);
        nrm[uk] = (nrm[uk] > 0.0) ? 1.0 / nrm[uk] : 0.0;
    }
#endif

    std::vector<double> a0(kLinks), b0(kLinks);
    xsect_batch::perlink_tabulated_pair(depth.data(), nrm.data(),
                                        afull.data(), rfull.data(),
                                        ta.data(), tb.data(), n,
                                        a0.data(), b0.data(), kLinks);

    // Group by transect — exactly the permutation sortGroupByTransect applies.
    std::vector<int> order(kLinks);
    for (int k = 0; k < kLinks; ++k) order[static_cast<std::size_t>(k)] = k;
    std::stable_sort(order.begin(), order.end(), [&](int l, int r) {
        return (l % static_cast<int>(tts.size())) < (r % static_cast<int>(tts.size()));
    });
    std::vector<double> pd(kLinks), pn(kLinks), pa(kLinks), pr(kLinks);
    std::vector<const double*> pta(kLinks), ptb(kLinks);
    for (int k = 0; k < kLinks; ++k) {
        const auto uk = static_cast<std::size_t>(k);
        const auto us = static_cast<std::size_t>(order[uk]);
        pd[uk] = depth[us]; pn[uk] = nrm[us];
        pa[uk] = afull[us]; pr[uk] = rfull[us];
        pta[uk] = ta[us];   ptb[uk] = tb[us];
    }
    std::vector<double> a1(kLinks), b1(kLinks);
    xsect_batch::perlink_tabulated_pair(pd.data(), pn.data(), pa.data(), pr.data(),
                                        pta.data(), ptb.data(), n,
                                        a1.data(), b1.data(), kLinks);
    for (int k = 0; k < kLinks; ++k) {
        const auto uk = static_cast<std::size_t>(k);
        const auto us = static_cast<std::size_t>(order[uk]);
        EXPECT_TRUE(BitEq(a0[us], a1[uk])) << "area, permuted position " << k;
        EXPECT_TRUE(BitEq(b0[us], b1[uk])) << "hydrad, permuted position " << k;
    }
}

// A3 — the reordering itself, not just the property it leans on.
//
// `sortGroupByTransect` permutes FOURTEEN parallel arrays. Permuting the table
// pointers while leaving (say) `a_full` behind would silently scale each link's
// area by another link's full area — every kernel would still run, every
// per-element result would still be "some valid number", and
// TabulatedKernelsArePositionIndependent above would still pass, because it
// tests the kernels rather than the permutation. So this test checks the two
// things that can actually break: that the group really is grouped by transect
// afterwards, and that each element's parameters still belong to ITS link.
TEST(XSectAccel, TransectSortKeepsEveryParallelArrayWithItsLink) {
    const auto& tts = testTransects();
    ASSERT_GE(tts.size(), 3u);

    // Links deal round-robin from the transect deck, so the pre-sort order
    // interleaves — the arrangement the sort exists to undo. A prime link count
    // keeps the cycle from lining up with the group boundaries.
    constexpr int kLinks = 23;
    SimulationContext ctx;
    ctx.links.resize(kLinks);
    ctx.transect_tables.resize(3);
    for (std::size_t t = 0; t < ctx.transect_tables.size(); ++t)
        ctx.transect_tables[t] = tts[t].td;

    std::vector<XSectParams> params(kLinks);
    std::vector<int> curve_of_link(kLinks);
    for (int k = 0; k < kLinks; ++k) {
        const int ti = k % static_cast<int>(ctx.transect_tables.size());
        curve_of_link[static_cast<std::size_t>(k)] = ti;
        ctx.links.xsect_curve[static_cast<std::size_t>(k)] = ti;
        params[static_cast<std::size_t>(k)].type =
            static_cast<int>(XSectShape::IRREGULAR);
        // Deliberately WRONG-but-distinct seed values: attachTransectTables
        // overwrites them from the transect, so if the sort moved an element's
        // tables without moving its scalars the mismatch shows up below.
        params[static_cast<std::size_t>(k)].y_full = 1.0 + k;
        params[static_cast<std::size_t>(k)].a_full = 100.0 + k;
    }

    XSectGroups groups;
    groups.build(params.data(), kLinks);
    groups.attachTransectTables(ctx);

    const ShapeGroup* g = groups.findGroup(XSectShape::IRREGULAR);
    ASSERT_NE(g, nullptr);
    ASSERT_EQ(g->count, kLinks);

    // 1. The group is actually grouped: transect indices are non-decreasing.
    for (int k = 1; k < g->count; ++k) {
        const int prev = curve_of_link[static_cast<std::size_t>(g->link_idx[static_cast<std::size_t>(k - 1)])];
        const int cur  = curve_of_link[static_cast<std::size_t>(g->link_idx[static_cast<std::size_t>(k)])];
        EXPECT_LE(prev, cur) << "group not sorted by transect at position " << k;
    }
    // And it really was a reorder, not a no-op on an already-sorted input.
    bool reordered = false;
    for (int k = 0; k < g->count; ++k)
        if (g->link_idx[static_cast<std::size_t>(k)] != k) { reordered = true; break; }
    EXPECT_TRUE(reordered) << "input was already grouped — this test proves nothing";

    // 2. Every element still carries ITS OWN link's data. link_idx is the only
    //    thing tying a slot back to a link, so each array is checked against
    //    the transect that link references.
    for (int k = 0; k < g->count; ++k) {
        const auto uk = static_cast<std::size_t>(k);
        const int link = g->link_idx[uk];
        const auto& td = ctx.transect_tables[
            static_cast<std::size_t>(curve_of_link[static_cast<std::size_t>(link)])];
        SCOPED_TRACE("slot " + std::to_string(k) + " -> link " + std::to_string(link));
        EXPECT_EQ(g->area_tables[uk],  td.area_tbl);
        EXPECT_EQ(g->hrad_tables[uk],  td.hrad_tbl);
        EXPECT_EQ(g->width_tables[uk], td.width_tbl);
        EXPECT_DOUBLE_EQ(g->y_full[uk], td.y_full);
        EXPECT_DOUBLE_EQ(g->a_full[uk], td.a_full);
        EXPECT_DOUBLE_EQ(g->r_full[uk], td.r_full);
        EXPECT_DOUBLE_EQ(g->w_max[uk],  td.w_max);
        EXPECT_DOUBLE_EQ(g->inv_y_full[uk], 1.0 / td.y_full);
    }

    // 3. End to end: the batch area a link gets must equal what the per-element
    //    accessor computes for that link's own transect. This is the assertion
    //    that fails if the permutation ever pairs a link with another's table.
    std::vector<double> depths(kLinks), areas(kLinks, -1.0);
    for (int k = 0; k < kLinks; ++k) {
        const auto& td = ctx.transect_tables[
            static_cast<std::size_t>(curve_of_link[static_cast<std::size_t>(k)])];
        depths[static_cast<std::size_t>(k)] = 0.37 * td.y_full;
    }
    groups.computeAreas(depths.data(), areas.data(), kLinks);

    for (int k = 0; k < kLinks; ++k) {
        const auto uk = static_cast<std::size_t>(k);
        const auto& td = ctx.transect_tables[
            static_cast<std::size_t>(curve_of_link[uk])];
        XSectParams ref{};
        ref.type   = static_cast<int>(XSectShape::IRREGULAR);
        ref.y_full = td.y_full;
        ref.a_full = td.a_full;
        ref.area_tbl = td.area_tbl;
        ref.area_lut = &td.area_lut;
        ref.transect_tbl_size = openswmm::transect::N_TRANSECT_TBL;
        EXPECT_TRUE(BATCH_EQ(xsect::getAofY(ref, depths[uk]), areas[uk]))
            << "link " << k << " got another link's geometry";
    }
}
