// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_xsect_geometry_option.cpp
 * @brief XSECT_GEOMETRY through the generic swmm_options_get / swmm_options_set
 *        key API.
 *
 * The key is parsed from [OPTIONS] and printed in the report, but until it was
 * added to the two key chains no program could read or set it — the
 * engine_feature_gaps audit reported it as G2 (parsed, unreachable). These pin
 * the round trip and the one way this key differs from its siblings: the
 * compiled sections it selects are built once, at open, so a change after
 * open is refused rather than left disagreeing with what the hot loop uses.
 */

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include <openswmm/engine/openswmm_engine.h>

namespace fs = std::filesystem;

namespace {

std::string geometry(SWMM_Engine e) {
    char buf[32] = {};
    EXPECT_EQ(swmm_options_get(e, "XSECT_GEOMETRY", buf, static_cast<int>(sizeof buf)), SWMM_OK);
    return buf;
}

/// The smallest model that opens: one conduit between a junction and an outfall.
std::string writeModel() {
    const std::string d = std::string(OPENSWMM_FV_TEST_OUT_DIR) + "/options_api";
    std::error_code ec;
    fs::create_directories(d, ec);
    const std::string path = d + "/xsect_geometry.inp";
    std::ofstream os(path);
    os << "[OPTIONS]\nFLOW_UNITS           CFS\nFLOW_ROUTING         DYNWAVE\n"
          "START_DATE           01/01/2026\nSTART_TIME           00:00:00\n"
          "END_DATE             01/01/2026\nEND_TIME             00:10:00\n"
          "REPORT_STEP          00:05:00\nROUTING_STEP         5\n\n"
          "[JUNCTIONS]\nJ0  40.0  12.0  0  0  0\n\n"
          "[OUTFALLS]\nO1  39.6  FREE  NO\n\n"
          "[CONDUITS]\nC0  J0  O1  200.0  0.013  0  0  0\n\n"
          "[XSECTIONS]\nC0  CIRCULAR  4.0  0  0  0  1\n\n"
          "[REPORT]\nINPUT  NO\nCONTROLS  NO\n";
    return path;
}

}  // namespace

TEST(XsectGeometryOption, DefaultsToLegacyAndRoundTripsCaseInsensitively) {
    SWMM_Engine e = swmm_engine_create();
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(geometry(e), "LEGACY");

    ASSERT_EQ(swmm_options_set(e, "XSECT_GEOMETRY", "EXACT"), SWMM_OK);
    EXPECT_EQ(geometry(e), "EXACT");
    // Both the key and the value are case-insensitive, like every sibling key.
    ASSERT_EQ(swmm_options_set(e, "xsect_geometry", "legacy"), SWMM_OK);
    EXPECT_EQ(geometry(e), "LEGACY");
    ASSERT_EQ(swmm_options_set(e, "XSECT_GEOMETRY", "exact"), SWMM_OK);
    EXPECT_EQ(geometry(e), "EXACT");

    swmm_engine_destroy(e);
}

TEST(XsectGeometryOption, RejectsAnUnknownValueAndKeepsThePreviousOne) {
    SWMM_Engine e = swmm_engine_create();
    ASSERT_NE(e, nullptr);
    ASSERT_EQ(swmm_options_set(e, "XSECT_GEOMETRY", "EXACT"), SWMM_OK);

    EXPECT_EQ(swmm_options_set(e, "XSECT_GEOMETRY", "APPROX"), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_options_set(e, "XSECT_GEOMETRY", ""), SWMM_ERR_BADPARAM);
    EXPECT_EQ(geometry(e), "EXACT") << "a refused set must not disturb the stored value";

    swmm_engine_destroy(e);
}

TEST(XsectGeometryOption, RefusesToChangeOnceTheModelIsOpen) {
    const std::string inp = writeModel();
    const std::string rpt = inp.substr(0, inp.size() - 4) + ".rpt";
    const std::string out = inp.substr(0, inp.size() - 4) + ".out";

    SWMM_Engine e = swmm_engine_create();
    ASSERT_NE(e, nullptr);
    ASSERT_EQ(swmm_engine_open(e, inp.c_str(), rpt.c_str(), out.c_str(), nullptr), SWMM_OK);
    EXPECT_EQ(geometry(e), "LEGACY");

    // The resolver has already decided, at open, which sections to compile.
    // Accepting a change here would update the report line and nothing else.
    EXPECT_EQ(swmm_options_set(e, "XSECT_GEOMETRY", "EXACT"), SWMM_ERR_LIFECYCLE);
    EXPECT_EQ(geometry(e), "LEGACY") << "the refused change must leave the option untouched";

    swmm_engine_close(e);
    swmm_engine_destroy(e);
}
