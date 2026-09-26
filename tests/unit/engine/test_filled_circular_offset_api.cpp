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

/*!
 * \file   test_filled_circular_offset_api.cpp
 * \author Caleb Buahin <caleb.buahin@gmail.com>
 * \date   2026
 * \license Apache-2.0
 * \brief  FILLED_CIRCULAR conduit offsets cross the C API as AUTHORED values.
 *
 * resolve_cross_references() raises both stored offsets of a partly filled
 * circular conduit by the sediment depth (legacy link.c:1072-1077) and the
 * .inp / GeoPackage writers subtract it again. The C API is the GUI's model
 * store, so its offset getters / setters, swmm_link_set_xsect and the conduit
 * split must keep that same authored contract — otherwise a save drifts by
 * yBot (0.5 ft in the fixture) every time a filled conduit is touched.
 *
 * Fixture: inp_roundtrip/filled_circular_offsets_api.inp — C1 authored
 * 0.3 / 0.7, FILLED_CIRCULAR 1.5 with 0.5 of sediment. Written files land
 * beside it as inp_roundtrip/_filled_circular_offsets_api_<case>.inp.
 */
#include <gtest/gtest.h>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_edit.h>
#include <openswmm/engine/openswmm_links.h>
#include <openswmm/engine/openswmm_model.h>
#include <openswmm/engine/openswmm_nodes.h>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr const char* kFixture = "inp_roundtrip/filled_circular_offsets_api.inp";
constexpr const char* kReport  = "inp_roundtrip/_filled_circular_offsets_api.rpt";

std::string outPath(const char* tag) {
    return std::string("inp_roundtrip/_filled_circular_offsets_api_") + tag + ".inp";
}

/// Whitespace-split columns of the row in [sec] whose first column is `id`
/// (empty when the section or the row is absent).
std::vector<std::string> sectionRow(const std::string& path, const std::string& sec,
                                    const std::string& id) {
    std::ifstream in(path);
    std::string line;
    bool inside = false;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::istringstream row(line);
        std::string tok;
        if (!(row >> tok)) continue;
        if (tok.front() == '[') { inside = (tok == "[" + sec + "]"); continue; }
        if (!inside || tok.rfind(";", 0) == 0 || tok != id) continue;
        std::vector<std::string> cols{tok};
        while (row >> tok) cols.push_back(tok);
        return cols;
    }
    return {};
}

double up(SWMM_Engine e, int idx) {
    double v = -1.0;
    EXPECT_EQ(swmm_link_get_offset_up(e, idx, &v), SWMM_OK);
    return v;
}

double dn(SWMM_Engine e, int idx) {
    double v = -1.0;
    EXPECT_EQ(swmm_link_get_offset_dn(e, idx, &v), SWMM_OK);
    return v;
}

/// [CONDUITS] columns: Name From To Length N InOffset OutOffset InitFlow MaxFlow.
void expectConduitRow(const std::string& path, const char* id, double in, double out) {
    const auto r = sectionRow(path, "CONDUITS", id);
    ASSERT_GE(r.size(), 7u) << id << " missing from " << path;
    EXPECT_NEAR(std::stod(r[5]), in,  1e-9) << id << " InOffset in " << path;
    EXPECT_NEAR(std::stod(r[6]), out, 1e-9) << id << " OutOffset in " << path;
}

struct Opened {
    SWMM_Engine e = swmm_engine_create();
    int c1 = -1;
    Opened() {
        const int rc = swmm_engine_open(e, kFixture, kReport, nullptr, nullptr);
        EXPECT_EQ(rc, SWMM_OK) << (swmm_get_last_error_msg(e) ? swmm_get_last_error_msg(e) : "");
        c1 = swmm_link_index(e, "C1");
    }
    ~Opened() { swmm_engine_close(e); swmm_engine_destroy(e); }
};

}  // namespace

// The resolver stores 0.8 / 1.2 (authored + 0.5 of sediment); the API reports
// what the file says.
TEST(FilledCircularOffsetApi, GettersReturnTheAuthoredOffsets) {
    Opened m;
    ASSERT_GE(m.c1, 0);
    EXPECT_NEAR(up(m.e, m.c1), 0.3, 1e-9);
    EXPECT_NEAR(dn(m.e, m.c1), 0.7, 1e-9);
}

// A get→set round trip (what every GUI editor does on Apply) is a no-op, and
// a newly typed value lands in the file as typed.
TEST(FilledCircularOffsetApi, SetThenSaveKeepsTheAuthoredValue) {
    Opened m;
    ASSERT_GE(m.c1, 0);
    ASSERT_EQ(swmm_link_set_offset_up(m.e, m.c1, up(m.e, m.c1)), SWMM_OK);
    ASSERT_EQ(swmm_link_set_offset_dn(m.e, m.c1, dn(m.e, m.c1)), SWMM_OK);
    EXPECT_NEAR(up(m.e, m.c1), 0.3, 1e-9);
    EXPECT_NEAR(dn(m.e, m.c1), 0.7, 1e-9);
    const std::string f1 = outPath("roundtrip");
    ASSERT_EQ(swmm_model_write(m.e, f1.c_str()), SWMM_OK);
    expectConduitRow(f1, "C1", 0.3, 0.7);

    ASSERT_EQ(swmm_link_set_offset_up(m.e, m.c1, 1.0), SWMM_OK);
    EXPECT_NEAR(up(m.e, m.c1), 1.0, 1e-9);
    const std::string f2 = outPath("edited");
    ASSERT_EQ(swmm_model_write(m.e, f2.c_str()), SWMM_OK);
    expectConduitRow(f2, "C1", 1.0, 0.7);
}

// Editing the section changes the stored bump, never the authored offsets:
// thinner sediment, no sediment (plain CIRCULAR), and back again.
TEST(FilledCircularOffsetApi, ShapeEditsLeaveTheAuthoredOffsetsAlone) {
    Opened m;
    ASSERT_GE(m.c1, 0);

    ASSERT_EQ(swmm_link_set_xsect(m.e, m.c1, SWMM_XSECT_FILLED_CIRCULAR, 1.5, 0.2, 0.0, 0.0), SWMM_OK);
    EXPECT_NEAR(up(m.e, m.c1), 0.3, 1e-9);
    EXPECT_NEAR(dn(m.e, m.c1), 0.7, 1e-9);
    const std::string f1 = outPath("thinner");
    ASSERT_EQ(swmm_model_write(m.e, f1.c_str()), SWMM_OK);
    expectConduitRow(f1, "C1", 0.3, 0.7);
    const auto xs = sectionRow(f1, "XSECTIONS", "C1");
    ASSERT_GE(xs.size(), 4u);
    EXPECT_NEAR(std::stod(xs[3]), 0.2, 1e-9) << "sediment depth was not saved";

    ASSERT_EQ(swmm_link_set_xsect(m.e, m.c1, SWMM_XSECT_CIRCULAR, 1.5, 0.0, 0.0, 0.0), SWMM_OK);
    EXPECT_NEAR(up(m.e, m.c1), 0.3, 1e-9);
    EXPECT_NEAR(dn(m.e, m.c1), 0.7, 1e-9);
    const std::string f2 = outPath("circular");
    ASSERT_EQ(swmm_model_write(m.e, f2.c_str()), SWMM_OK);
    expectConduitRow(f2, "C1", 0.3, 0.7);

    ASSERT_EQ(swmm_link_set_xsect(m.e, m.c1, SWMM_XSECT_FILLED_CIRCULAR, 1.5, 0.5, 0.0, 0.0), SWMM_OK);
    EXPECT_NEAR(up(m.e, m.c1), 0.3, 1e-9);
    EXPECT_NEAR(dn(m.e, m.c1), 0.7, 1e-9);
    const std::string f3 = outPath("refilled");
    ASSERT_EQ(swmm_model_write(m.e, f3.c_str()), SWMM_OK);
    expectConduitRow(f3, "C1", 0.3, 0.7);
}

// The programmatic builder (Python / MCP): in the BUILDING state the stores
// hold authored values and swmm_finalize_model resolves — and bumps — them
// exactly once. A second bump would read back here as 2.7, not 2.2.
TEST(FilledCircularOffsetApi, BuiltModelIsBumpedExactlyOnceAtFinalize) {
    SWMM_Engine e = swmm_engine_new();
    ASSERT_NE(e, nullptr);
    ASSERT_EQ(swmm_node_add(e, "J0",  SWMM_NODE_JUNCTION), SWMM_OK);
    ASSERT_EQ(swmm_node_add(e, "OUT", SWMM_NODE_OUTFALL),  SWMM_OK);
    ASSERT_EQ(swmm_link_add(e, "C1",  SWMM_LINK_CONDUIT),  SWMM_OK);
    ASSERT_EQ(swmm_link_set_nodes(e, 0, 0, 1), SWMM_OK);
    ASSERT_EQ(swmm_node_set_invert_elev(e, 0, 10.0), SWMM_OK);
    ASSERT_EQ(swmm_node_set_invert_elev(e, 1,  7.0), SWMM_OK);
    ASSERT_EQ(swmm_link_set_length(e, 0, 400.0), SWMM_OK);
    ASSERT_EQ(swmm_link_set_xsect(e, 0, SWMM_XSECT_FILLED_CIRCULAR, 1.5, 0.5, 0.0, 0.0), SWMM_OK);
    ASSERT_EQ(swmm_link_set_offset_up(e, 0, 2.2), SWMM_OK);
    ASSERT_EQ(swmm_link_set_offset_dn(e, 0, 0.7), SWMM_OK);
    EXPECT_NEAR(up(e, 0), 2.2, 1e-9);
    EXPECT_NEAR(dn(e, 0), 0.7, 1e-9);

    ASSERT_EQ(swmm_finalize_model(e), SWMM_OK)
        << (swmm_get_last_error_msg(e) ? swmm_get_last_error_msg(e) : "");
    EXPECT_NEAR(up(e, 0), 2.2, 1e-9);
    EXPECT_NEAR(dn(e, 0), 0.7, 1e-9);
    const std::string f = outPath("built");
    ASSERT_EQ(swmm_model_write(e, f.c_str()), SWMM_OK);
    expectConduitRow(f, "C1", 2.2, 0.7);

    swmm_engine_close(e);
    swmm_engine_destroy(e);
}

// Splitting a filled conduit (the GUI's add-node-on-conduit tools) writes an
// authored 0 at the new junction — stored as yBot, not 0, or the file would
// carry -0.5 for both halves.
TEST(FilledCircularOffsetApi, SplitWritesAnAuthoredZeroAtTheNewJunction) {
    Opened m;
    ASSERT_GE(m.c1, 0);
    int jn = -1, c1b = -1;
    ASSERT_EQ(swmm_conduit_split(m.e, m.c1, 0.5, "JN", "C1B", 0, &jn, &c1b), SWMM_OK)
        << (swmm_get_last_error_msg(m.e) ? swmm_get_last_error_msg(m.e) : "");
    ASSERT_GE(c1b, 0);
    EXPECT_NEAR(up(m.e, m.c1), 0.3, 1e-9);
    EXPECT_NEAR(dn(m.e, m.c1), 0.0, 1e-9);
    EXPECT_NEAR(up(m.e, c1b),  0.0, 1e-9);
    EXPECT_NEAR(dn(m.e, c1b),  0.7, 1e-9);

    const std::string f = outPath("split");
    ASSERT_EQ(swmm_model_write(m.e, f.c_str()), SWMM_OK);
    expectConduitRow(f, "C1",  0.3, 0.0);
    expectConduitRow(f, "C1B", 0.0, 0.7);
}
