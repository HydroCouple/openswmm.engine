// SPDX-License-Identifier: Apache-2.0
//
// Copyright 2026 Caleb Buahin
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/**
 * @file test_crs_api_roundtrip.cpp
 * @brief The CRS is held in two engine stores — the spatial frame and the
 *        [OPTIONS] table. Every C-API write path must update both, and the
 *        .inp writer must emit whichever is set, or a caller that reprojects
 *        coordinates and saves writes the NEW coordinates under the OLD CRS.
 *
 * Measured 2026-09-12 before the fix: swmm_spatial_set_crs("EPSG:32618") on
 * a model opened with CRS EPSG:26985, then swmm_model_write, produced a file
 * still declaring EPSG:26985 — and swmm_get_crs still returned the old code.
 *
 * Not covered here: the frame-only fallback in swmm_get_crs and the writer
 * (the GeoPackage-open case). No C-API route reaches a frame-only state
 * without the GeoPackage input plugin, so that leg is reviewed, not tested.
 */

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_model.h>
#include <openswmm/engine/openswmm_nodes.h>
#include <openswmm/engine/openswmm_links.h>
#include <openswmm/engine/openswmm_spatial.h>

namespace fs = std::filesystem;

namespace {

constexpr const char* kOldCrs = "EPSG:26985";
constexpr const char* kNewCrs = "EPSG:32618";

std::string spatialCrs(SWMM_Engine e) {
    char buf[512] = {};
    EXPECT_EQ(swmm_spatial_get_crs(e, buf, sizeof buf), SWMM_OK);
    return buf;
}

// swmm_get_crs is what the GUI reads on open (SWMMModelLayer). It returns
// SWMM_ERR_CRS when neither store carries a value.
std::string optionCrs(SWMM_Engine e) {
    char buf[512] = {};
    return swmm_get_crs(e, buf, sizeof buf) == SWMM_OK ? std::string(buf) : std::string();
}

// The CRS line inside [OPTIONS] only — [TITLE] text may itself start with "CRS".
std::string crsLineInOptions(const fs::path& inp) {
    std::ifstream f(inp);
    std::string line;
    bool inOptions = false;
    while (std::getline(f, line)) {
        if (!line.empty() && line[0] == '[') {
            inOptions = line.rfind("[OPTIONS]", 0) == 0;
            continue;
        }
        if (inOptions && line.rfind("CRS", 0) == 0 && line.size() > 3
            && (line[3] == ' ' || line[3] == '\t'))
            return line;
    }
    return {};
}

} // namespace

class CrsApiRoundTripTest : public ::testing::Test {
protected:
    SWMM_Engine engine   = nullptr;
    SWMM_Engine reopened = nullptr;

    void SetUp() override {
        engine = swmm_engine_new();
        ASSERT_NE(engine, nullptr);
        // Minimal valid topology so swmm_model_write / reopen succeed.
        ASSERT_EQ(swmm_node_add(engine, "J1", SWMM_NODE_JUNCTION), SWMM_OK);
        ASSERT_EQ(swmm_node_add(engine, "O1", SWMM_NODE_OUTFALL),  SWMM_OK);
        ASSERT_EQ(swmm_link_add(engine, "L1", SWMM_LINK_CONDUIT),  SWMM_OK);
        const int l1 = swmm_link_index(engine, "L1");
        ASSERT_EQ(swmm_link_set_nodes(engine, l1,
                      swmm_node_index(engine, "J1"),
                      swmm_node_index(engine, "O1")), SWMM_OK);
        ASSERT_EQ(swmm_link_set_length(engine, l1, 100.0), SWMM_OK);
        ASSERT_EQ(swmm_link_set_roughness(engine, l1, 0.013), SWMM_OK);
        ASSERT_EQ(swmm_link_set_xsect(engine, l1, SWMM_XSECT_CIRCULAR,
                                      1.0, 0.0, 0.0, 0.0), SWMM_OK);
    }
    void TearDown() override {
        if (reopened) { swmm_engine_close(reopened); swmm_engine_destroy(reopened); }
        if (engine) swmm_engine_destroy(engine);
    }

    // Artifacts under the test working directory (tests/unit/engine/data) so
    // they stay reviewable after a run, per project testing conventions.
    static fs::path artifactDir() {
        fs::path d = fs::current_path() / "test_crs_api_roundtrip_artifacts";
        fs::create_directories(d);
        return d;
    }

    // Write `engine` to `stem.inp`, assert the [OPTIONS] CRS line carries
    // `expect`, reopen it, and assert both stores of the reopened engine agree.
    void writeAndReopenExpecting(const char* stem, const std::string& expect) {
        const fs::path inp = artifactDir() / (std::string(stem) + ".inp");
        const fs::path rpt = artifactDir() / (std::string(stem) + ".rpt");
        const fs::path out = artifactDir() / (std::string(stem) + ".out");
        ASSERT_EQ(swmm_model_write(engine, inp.string().c_str()), SWMM_OK)
            << swmm_get_last_error_msg(engine);

        const std::string line = crsLineInOptions(inp);
        EXPECT_NE(line.find(expect), std::string::npos)
            << "[OPTIONS] CRS line was: '" << line << "'";

        reopened = swmm_engine_create();
        ASSERT_NE(reopened, nullptr);
        ASSERT_EQ(swmm_engine_open(reopened, inp.string().c_str(),
                                   rpt.string().c_str(), out.string().c_str(),
                                   nullptr), SWMM_OK)
            << "reopen failed: " << swmm_get_last_error_msg(reopened);
        EXPECT_EQ(optionCrs(reopened),  expect);
        EXPECT_EQ(spatialCrs(reopened), expect);
    }
};

// A blank engine has no CRS in either store; swmm_get_crs says so.
TEST_F(CrsApiRoundTripTest, BlankEngineHasNoCrs) {
    char buf[512] = {};
    EXPECT_EQ(swmm_get_crs(engine, buf, sizeof buf), SWMM_ERR_CRS);
    EXPECT_EQ(spatialCrs(engine), "");
}

// Path 1 — the spatial API (what a coordinate reprojection uses). This was
// the broken one: it filled only the frame, so the writer and swmm_get_crs
// kept whatever [OPTIONS] had.
TEST_F(CrsApiRoundTripTest, SpatialSetReachesBothStoresAndTheWriter) {
    ASSERT_EQ(swmm_options_set(engine, "CRS", kOldCrs), SWMM_OK);
    ASSERT_EQ(swmm_spatial_set_crs(engine, kNewCrs), SWMM_OK);

    EXPECT_EQ(spatialCrs(engine), kNewCrs);
    EXPECT_EQ(optionCrs(engine),  kNewCrs) << "swmm_get_crs still returned the old code";

    writeAndReopenExpecting("spatial_set", kNewCrs);
}

// Path 2 — the [OPTIONS] key (what the Simulation Options page uses). Must
// also reach the frame, which the GeoPackage writer and 2D output read.
TEST_F(CrsApiRoundTripTest, OptionSetReachesBothStoresAndTheWriter) {
    ASSERT_EQ(swmm_spatial_set_crs(engine, kOldCrs), SWMM_OK);
    ASSERT_EQ(swmm_options_set(engine, "CRS", kNewCrs), SWMM_OK);

    EXPECT_EQ(optionCrs(engine),  kNewCrs);
    EXPECT_EQ(spatialCrs(engine), kNewCrs) << "spatial frame kept the old code";

    writeAndReopenExpecting("option_set", kNewCrs);
}

// No CRS assigned anywhere → no CRS line is written (files that never had one
// must stay byte-identical on a round-trip).
TEST_F(CrsApiRoundTripTest, NoCrsWritesNoLine) {
    const fs::path inp = artifactDir() / "no_crs.inp";
    ASSERT_EQ(swmm_model_write(engine, inp.string().c_str()), SWMM_OK)
        << swmm_get_last_error_msg(engine);
    EXPECT_EQ(crsLineInOptions(inp), "");
}
