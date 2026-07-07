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
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>

#include "../../src/engine/hydraulics/XSectBatch.hpp"
#include "../../src/engine/hydraulics/xsect_tables.hpp"
#include "../../src/engine/hydraulics/XSectLookup.hpp"

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
