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

#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_links.h>
#include <openswmm/engine/openswmm_xsect.h>

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
    // Writing back the value in force changes nothing, so it is accepted (the
    // Python options mapping round-trips every key this way).
    EXPECT_EQ(swmm_options_set(e, "XSECT_GEOMETRY", "LEGACY"), SWMM_OK);
    EXPECT_EQ(geometry(e), "LEGACY");

    swmm_engine_close(e);
    swmm_engine_destroy(e);
}

namespace {

/// One CIRCULAR, one FORCE_MAIN and one EGG conduit of the same 1 ft size, each
/// to its own outfall, under the given XSECT_GEOMETRY.
std::string writeShapes(const std::string& mode) {
    const std::string d = std::string(OPENSWMM_FV_TEST_OUT_DIR) + "/options_api";
    std::error_code ec;
    fs::create_directories(d, ec);
    const std::string path = d + "/xsect_geometry_shapes_" + mode + ".inp";
    std::ofstream os(path);
    os << "[OPTIONS]\nFLOW_UNITS CFS\nFLOW_ROUTING DYNWAVE\nXSECT_GEOMETRY " << mode << "\n"
          "START_DATE 01/01/2026\nSTART_TIME 00:00:00\nEND_DATE 01/01/2026\nEND_TIME 00:10:00\n"
          "REPORT_STEP 00:05:00\nROUTING_STEP 5\n\n"
          "[JUNCTIONS]\nJ0 41.0 12.0 0 0 0\n\n"
          "[OUTFALLS]\nOC 40.0 FREE NO\nOF 40.0 FREE NO\nOE 40.0 FREE NO\n\n"
          "[CONDUITS]\nCIRC J0 OC 1000 0.013 0 0 0 0\nFMAIN J0 OF 1000 0.013 0 0 0 0\n"
          "EGG J0 OE 1000 0.013 0 0 0 0\n\n"
          "[XSECTIONS]\nCIRC CIRCULAR 1 0 0 0 1\nFMAIN FORCE_MAIN 1 130 0 0 1\n"
          "EGG EGG 1 0 0 0 1\n\n"
          "[REPORT]\nINPUT NO\nCONTROLS NO\n";
    return path;
}

struct Full {
    double a_full = 0, r_full = 0, s_full = 0;  // what the engine stores (full flow = s_full * beta)
    double a_at_full = 0, s_at_full = 0;         // the routed geometry, read at full depth
    double a_half = 0;                           // the routed geometry at half depth
};

Full fullOf(SWMM_Engine e, const char* id) {
    Full f;
    SWMM_XSect xs = nullptr;
    EXPECT_EQ(swmm_link_create_xsect(e, swmm_link_index(e, id), &xs), SWMM_OK) << id;
    if (!xs) return f;
    double y_full = 0, w_max = 0, a_max = 0;
    swmm_xsect_full_properties(xs, &y_full, &f.a_full, &f.r_full, &w_max, &f.s_full, &a_max);
    swmm_xsect_area_of_depth(xs, y_full, &f.a_at_full);
    swmm_xsect_sectfactor_of_area(xs, f.a_at_full, &f.s_at_full);
    swmm_xsect_area_of_depth(xs, 0.5 * y_full, &f.a_half);
    swmm_xsect_free(xs);
    return f;
}

/// Opens the shape deck under @p mode and reads all three conduits.
void readShapes(const std::string& mode, Full& circ, Full& fmain, Full& egg) {
    const std::string inp = writeShapes(mode);
    const std::string base = inp.substr(0, inp.size() - 4);
    SWMM_Engine e = swmm_engine_create();
    ASSERT_NE(e, nullptr);
    ASSERT_EQ(swmm_engine_open(e, inp.c_str(), (base + ".rpt").c_str(), (base + ".out").c_str(),
                               nullptr),
              SWMM_OK);
    circ = fullOf(e, "CIRC");
    fmain = fullOf(e, "FMAIN");
    egg = fullOf(e, "EGG");
    swmm_engine_close(e);
    swmm_engine_destroy(e);
}

}  // namespace

// Under EXACT a circle's full values must describe the circle the solver routes
// through — and a FORCE_MAIN is that same circle, so its section factor is the
// Manning A*R^(2/3), not LEGACY's Hazen-Williams A*R^0.63 (issue #166), and its
// full flow equals a CIRCULAR pipe's of the same size.
TEST(XsectGeometryOption, ExactCirclesTakeTheirFullValuesFromTheCompiledCircle) {
    Full circ, fmain, egg;
    readShapes("EXACT", circ, fmain, egg);
    for (const Full* f : {&circ, &fmain}) {
        EXPECT_NEAR(f->a_full, f->a_at_full, 1e-9 * f->a_at_full);
        EXPECT_NEAR(f->s_full, f->s_at_full, 1e-9 * f->s_at_full);
    }
    EXPECT_NEAR(fmain.s_full, circ.s_full, 1e-12 * circ.s_full);
    // pi/4 * D^2 * (D/4)^(2/3) for D = 1 ft
    EXPECT_NEAR(circ.s_full, 0.25 * 3.14159265358979 * std::pow(0.25, 2.0 / 3.0), 1e-6);
}

// The tabulated shapes stay on their LEGACY tables under EXACT: their width-table
// reconstruction is 2-8% smaller than the catalogue, so EXACT must leave them
// exactly as LEGACY evaluates them.
TEST(XsectGeometryOption, ExactLeavesTabulatedShapesOnTheirLegacyTables) {
    Full circL, fmainL, eggL, circE, fmainE, eggE;
    readShapes("LEGACY", circL, fmainL, eggL);
    readShapes("EXACT", circE, fmainE, eggE);
    EXPECT_EQ(eggE.a_full, eggL.a_full);
    EXPECT_EQ(eggE.s_full, eggL.s_full);
    EXPECT_EQ(eggE.a_half, eggL.a_half);
    EXPECT_EQ(eggE.a_at_full, eggL.a_at_full);
    // and under LEGACY nothing changes: the force main keeps its LEGACY factor
    EXPECT_GT(fmainL.s_full, circL.s_full * 1.04)
        << "LEGACY must keep EPA's A*R^0.63 for FORCE_MAIN (5.2% high at 1 ft)";
}
