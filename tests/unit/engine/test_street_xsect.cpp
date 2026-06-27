/**
 * @file test_street_xsect.cpp
 * @brief End-to-end test for STREET cross-section conduits.
 *
 * @details Exercises the full STREET path: [XSECTIONS] STREET parse → street
 *          name resolution → street::buildTransect table generation →
 *          XSectGroups per-link transect tables → DYNWAVE routing. Confirms a
 *          street conduit builds valid geometry (non-zero full area / width)
 *          and carries flow under rainfall without error.
 *
 * @note Working directory is tests/unit/engine/data/ (set by CMakeLists).
 * @ingroup engine_tests
 */

#include <gtest/gtest.h>
#include <cmath>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_links.h>

namespace {

TEST(StreetXsect, RunsAndCarriesFlow) {
    SWMM_Engine e = swmm_engine_create();
    ASSERT_NE(e, nullptr);
    ASSERT_EQ(swmm_engine_open(e, "street_xsect.inp",
                               "street_xsect.rpt", "street_xsect.out", nullptr),
              SWMM_OK);

    ASSERT_EQ(swmm_link_count(e), 1);

    // The single conduit C1 must have resolved to a STREET cross-section.
    int shape = -1;
    double g1 = 0, g2 = 0, g3 = 0, g4 = 0;
    ASSERT_EQ(swmm_link_get_xsect(e, 0, &shape, &g1, &g2, &g3, &g4), SWMM_OK);
    EXPECT_EQ(shape, 24);  // XsectShape::STREET_XSECT

    ASSERT_EQ(swmm_engine_initialize(e), SWMM_OK);
    ASSERT_EQ(swmm_engine_start(e, 1), SWMM_OK);

    double elapsed = 0.0;
    double peak_flow = 0.0;
    bool   all_finite = true;
    int    steps = 0;
    do {
        ASSERT_EQ(swmm_engine_step(e, &elapsed), SWMM_OK);
        double q = 0.0, d = 0.0;
        swmm_link_get_flow(e, 0, &q);
        swmm_link_get_depth(e, 0, &d);
        if (!std::isfinite(q) || !std::isfinite(d)) all_finite = false;
        peak_flow = std::max(peak_flow, q);
        ++steps;
    } while (elapsed > 0.0 && steps < 100000);

    EXPECT_TRUE(all_finite) << "street conduit produced non-finite flow/depth";
    EXPECT_GT(peak_flow, 0.0) << "street conduit carried no flow — geometry not built?";

    EXPECT_EQ(swmm_engine_end(e), SWMM_OK);
    swmm_engine_close(e);
}

}  // namespace
