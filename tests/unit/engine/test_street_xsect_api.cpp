/*!
 * \file   test_street_xsect_api.cpp
 * \author Caleb Buahin <caleb.buahin@gmail.com>
 * \date   2026
 * \license MIT
 * \brief  Programmatic STREET cross-section public-API round-trip.
 *
 * Complements test_street_xsect.cpp (which exercises the .inp parse + run
 * path) by covering the GUI-facing builder surface:
 *   1. swmm_street_get_params round-trips swmm_street_set_params.
 *   2. swmm_link_set_xsect(SWMM_XSECT_STREET, streetIndex) stores the shape
 *      and swmm_link_get_xsect reports the shape + street index back
 *      (mirrors the IRREGULAR/transect contract — geom1 is an index).
 */
#include <gtest/gtest.h>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_model.h>
#include <openswmm/engine/openswmm_nodes.h>
#include <openswmm/engine/openswmm_links.h>
#include <openswmm/engine/openswmm_infrastructure.h>

namespace {

// A fresh BUILDING-state engine with one conduit J1→J2 (no xsect yet).
SWMM_Engine makeConduitModel()
{
    SWMM_Engine e = swmm_engine_new();
    EXPECT_EQ(swmm_node_add(e, "J1", SWMM_NODE_JUNCTION), SWMM_OK);
    EXPECT_EQ(swmm_node_add(e, "J2", SWMM_NODE_JUNCTION), SWMM_OK);
    EXPECT_EQ(swmm_link_add(e, "C1", SWMM_LINK_CONDUIT), SWMM_OK);
    const int li = swmm_link_index(e, "C1");
    EXPECT_GE(li, 0);
    EXPECT_EQ(swmm_link_set_nodes(e, li, swmm_node_index(e, "J1"),
                                        swmm_node_index(e, "J2")), SWMM_OK);
    EXPECT_EQ(swmm_link_set_length(e, li, 100.0), SWMM_OK);
    EXPECT_EQ(swmm_link_set_roughness(e, li, 0.016), SWMM_OK);
    return e;
}

void destroy(SWMM_Engine e)
{
    swmm_engine_close(e);
    swmm_engine_destroy(e);
}

} // namespace

TEST(StreetXsectApi, GetParamsRoundTripsSetParams)
{
    SWMM_Engine e = swmm_engine_new();
    ASSERT_EQ(swmm_street_add(e, "HEC-12"), SWMM_OK);
    const int si = swmm_street_index(e, "HEC-12");
    ASSERT_GE(si, 0);

    ASSERT_EQ(swmm_street_set_params(e, si,
        /*t_crown*/30.0, /*h_curb*/0.5, /*sx*/4.0, /*n_road*/0.016,
        /*gutter_depres*/0.25, /*gutter_width*/2.0, /*sides*/2,
        /*back_width*/3.0, /*back_slope*/6.0, /*back_n*/0.02), SWMM_OK);

    double tCrown = 0, hCurb = 0, sx = 0, nRoad = 0, gDep = 0, gW = 0;
    int    sides = 0;
    double bW = 0, bSlope = 0, bN = 0;
    ASSERT_EQ(swmm_street_get_params(e, si, &tCrown, &hCurb, &sx, &nRoad,
                                     &gDep, &gW, &sides, &bW, &bSlope, &bN),
              SWMM_OK);

    EXPECT_DOUBLE_EQ(tCrown, 30.0);
    EXPECT_DOUBLE_EQ(hCurb, 0.5);
    EXPECT_DOUBLE_EQ(sx, 4.0);
    EXPECT_DOUBLE_EQ(nRoad, 0.016);
    EXPECT_DOUBLE_EQ(gDep, 0.25);
    EXPECT_DOUBLE_EQ(gW, 2.0);
    EXPECT_EQ(sides, 2);
    EXPECT_DOUBLE_EQ(bW, 3.0);
    EXPECT_DOUBLE_EQ(bSlope, 6.0);
    EXPECT_DOUBLE_EQ(bN, 0.02);

    // NULL out-pointers are tolerated.
    EXPECT_EQ(swmm_street_get_params(e, si, &tCrown, nullptr, nullptr, nullptr,
                                     nullptr, nullptr, nullptr, nullptr,
                                     nullptr, nullptr), SWMM_OK);

    destroy(e);
}

TEST(StreetXsectApi, LinkXsectStoresShapeAndStreetIndex)
{
    SWMM_Engine e = makeConduitModel();

    ASSERT_EQ(swmm_street_add(e, "HEC-12"), SWMM_OK);
    const int si = swmm_street_index(e, "HEC-12");
    ASSERT_GE(si, 0);
    ASSERT_EQ(swmm_street_set_params(e, si,
        30.0, 0.5, 4.0, 0.016, 0.0, 0.0, 2, 0.0, 0.0, 0.0), SWMM_OK);

    const int li = swmm_link_index(e, "C1");
    ASSERT_GE(li, 0);
    ASSERT_EQ(swmm_link_set_xsect(e, li, SWMM_XSECT_STREET,
                                  static_cast<double>(si), 0.0, 0.0, 0.0),
              SWMM_OK);

    int    shape = -1;
    double g1 = -1, g2 = -1, g3 = -1, g4 = -1;
    ASSERT_EQ(swmm_link_get_xsect(e, li, &shape, &g1, &g2, &g3, &g4), SWMM_OK);
    EXPECT_EQ(shape, SWMM_XSECT_STREET);
    EXPECT_EQ(static_cast<int>(g1), si);

    destroy(e);
}

TEST(StreetXsectApi, LinkXsectRejectsOutOfRangeStreet)
{
    SWMM_Engine e = makeConduitModel();
    const int li = swmm_link_index(e, "C1");
    // No streets defined → index 0 is out of range → not SWMM_OK.
    EXPECT_NE(swmm_link_set_xsect(e, li, SWMM_XSECT_STREET, 0.0, 0.0, 0.0, 0.0),
              SWMM_OK);
    destroy(e);
}
