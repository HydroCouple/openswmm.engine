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
 * @file test_2d_node_coupling_section.cpp
 * @brief [2D_NODE_COUPLING]: per-node coupling type — parse, class defaults,
 *        validation, C API, node delete, .inp / GeoPackage round trip, and the
 *        router dropping map rows of non-coupleable nodes.
 *
 * @details plans/1D2D_FINE_SCALE_COUPLING_PLAN_2026-10-19.md §5. Inputs and
 *          outputs land in ./node_coupling_section_out/ (working directory
 *          tests/unit/engine/data) for review.
 *
 * @ingroup engine_2d
 */

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <openswmm/engine/openswmm_2d.h>
#include <openswmm/engine/openswmm_edit.h>
#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_model.h>
#include <openswmm/engine/openswmm_nodes.h>

namespace fs = std::filesystem;

namespace {

const fs::path kOut = "node_coupling_section_out";

// Chain J1 → V1 (virtual) → J2 → O1, with J3, J4 and storage ST1 draining to
// J2. `section` is spliced in BEFORE the node sections so every row exercises
// the deferred-name replay.
std::string deck(const std::string& section) {
    return section +
        "\n[OPTIONS]\n"
        "FLOW_UNITS           CMS\n"
        "FLOW_ROUTING         DYNWAVE\n"
        "START_DATE           01/01/2026\n"
        "START_TIME           00:00:00\n"
        "END_DATE             01/01/2026\n"
        "END_TIME             00:10:00\n"
        "ROUTING_STEP         5\n"
        "\n[JUNCTIONS]\n"
        "J1   3.0  1.0  0  0  0\n"
        "J2   1.0  1.0  0  0  0\n"
        "J3   2.0  1.0  0  0  0\n"
        "J4   2.0  1.0  0  0  0\n"
        "\n[VIRTUAL_JUNCTIONS]\n"
        "V1   2.0\n"
        "\n[STORAGE]\n"
        "ST1  2.0  2.0  0  FUNCTIONAL  0  0  50  0  0\n"
        "\n[OUTFALLS]\n"
        "O1   0.0  FREE  NO\n"
        "\n[CONDUITS]\n"
        "C1   J1   V1  20  0.013  0  0  0\n"
        "C2   V1   J2  20  0.013  0  0  0\n"
        "C3   J2   O1  20  0.013  0  0  0\n"
        "C4   J3   J2  20  0.013  0  0  0\n"
        "C5   J4   J2  20  0.013  0  0  0\n"
        "C6   ST1  J2  20  0.013  0  0  0\n"
        "\n[XSECTIONS]\n"
        "C1   CIRCULAR  0.3  0  0  0  1\n"
        "C2   CIRCULAR  0.3  0  0  0  1\n"
        "C3   CIRCULAR  0.3  0  0  0  1\n"
        "C4   CIRCULAR  0.3  0  0  0  1\n"
        "C5   CIRCULAR  0.3  0  0  0  1\n"
        "C6   CIRCULAR  0.3  0  0  0  1\n";
}

const char* kSection =
    "[2D_NODE_COUPLING]\n"
    ";;Node  Type     KEY=VALUE ...\n"
    "J2      NONE\n"
    "J3      grate    PERIMETER=1.8 AREA=0.18\n"
    "J4      MANHOLE  D=1.2 CREST=MESH_BED COEFFS=RUBINATO2017 CW=0.55\n"
    "O1      OUTFALL  SPREAD=3 TAILWATER_RELAX=10\n"
    "ST1     OPEN\n";

SWMM_Engine open_text(const std::string& tag, const std::string& text, int* rc = nullptr) {
    fs::create_directories(kOut);
    const fs::path inp = kOut / (tag + ".inp");
    { std::ofstream f(inp); f << text; }
    SWMM_Engine e = swmm_engine_create();
    const int r = swmm_engine_open(e, inp.string().c_str(),
                                   (kOut / (tag + ".rpt")).string().c_str(), "", nullptr);
    if (rc) *rc = r;
    if (r != SWMM_OK) { swmm_engine_destroy(e); return nullptr; }
    return e;
}

void close(SWMM_Engine e) {
    if (!e) return;
    swmm_engine_close(e);
    swmm_engine_destroy(e);
}

int type_of(SWMM_Engine e, const char* node, int* authored = nullptr) {
    int t = -1;
    EXPECT_EQ(swmm_node_get_2d_coupling_type(e, swmm_node_index(e, node), &t, authored), SWMM_OK);
    return t;
}

std::string param(SWMM_Engine e, const char* node, const char* key) {
    char buf[64] = {};
    EXPECT_EQ(swmm_node_get_2d_coupling_param(e, swmm_node_index(e, node), key, buf, 64), SWMM_OK);
    return buf;
}

std::string read_text(const fs::path& p) {
    std::ifstream f(p);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

void expect_section_rows(SWMM_Engine e) {
    int authored = 0;
    EXPECT_EQ(type_of(e, "J1", &authored), SWMM_2D_COUPLING_MANHOLE);
    EXPECT_EQ(authored, 0);
    EXPECT_EQ(type_of(e, "J2", &authored), SWMM_2D_COUPLING_NONE);
    EXPECT_EQ(authored, 1);
    EXPECT_EQ(type_of(e, "J3"), SWMM_2D_COUPLING_GRATE);
    EXPECT_EQ(param(e, "J3", "PERIMETER"), "1.8");
    EXPECT_EQ(param(e, "J3", "AREA"), "0.18");
    EXPECT_EQ(param(e, "J3", "D"), "");
    EXPECT_EQ(type_of(e, "J4"), SWMM_2D_COUPLING_MANHOLE);
    EXPECT_EQ(param(e, "J4", "D"), "1.2");
    EXPECT_EQ(param(e, "J4", "CREST"), "MESH_BED");
    EXPECT_EQ(param(e, "J4", "COEFFS"), "RUBINATO2017");
    EXPECT_EQ(param(e, "J4", "CW"), "0.55");
    EXPECT_EQ(type_of(e, "O1"), SWMM_2D_COUPLING_OUTFALL);
    EXPECT_EQ(param(e, "O1", "SPREAD"), "3");
    EXPECT_EQ(param(e, "O1", "TAILWATER_RELAX"), "10");
    EXPECT_EQ(type_of(e, "ST1", &authored), SWMM_2D_COUPLING_OPEN);
    EXPECT_EQ(authored, 1);
}

} // namespace

// D6: a node without a row takes its class default.
TEST(NodeCoupling2DSection, DefaultsFollowNodeClass) {
    SWMM_Engine e = open_text("defaults", deck(""));
    ASSERT_NE(e, nullptr);
    int authored = -1;
    EXPECT_EQ(type_of(e, "J1", &authored), SWMM_2D_COUPLING_MANHOLE);
    EXPECT_EQ(authored, 0);
    EXPECT_EQ(type_of(e, "V1"), SWMM_2D_COUPLING_NONE) << "virtual/inlet junction default";
    EXPECT_EQ(type_of(e, "ST1"), SWMM_2D_COUPLING_OPEN);
    EXPECT_EQ(type_of(e, "O1"), SWMM_2D_COUPLING_OUTFALL);
    int dflt = -1;
    ASSERT_EQ(swmm_node_get_2d_coupling_default_type(e, swmm_node_index(e, "J1"), &dflt), SWMM_OK);
    EXPECT_EQ(dflt, SWMM_2D_COUPLING_MANHOLE);
    close(e);
}

// Parse (ahead of the node sections), write, reopen: same rows, and the
// writer emits the section only when rows exist.
TEST(NodeCoupling2DSection, ParsesAndRoundTripsThroughInp) {
    SWMM_Engine e = open_text("parse", deck(kSection));
    ASSERT_NE(e, nullptr);
    expect_section_rows(e);

    const fs::path out = kOut / "parse_written.inp";
    ASSERT_EQ(swmm_model_write(e, out.string().c_str()), SWMM_OK);
    close(e);
    const std::string text = read_text(out);
    EXPECT_NE(text.find("[2D_NODE_COUPLING]"), std::string::npos) << text;
    EXPECT_NE(text.find("J3               GRATE    PERIMETER=1.8 AREA=0.18"), std::string::npos);

    SWMM_Engine e2 = open_text("parse_reopened", text);
    ASSERT_NE(e2, nullptr);
    expect_section_rows(e2);
    close(e2);

    SWMM_Engine bare = open_text("no_rows", deck(""));
    ASSERT_NE(bare, nullptr);
    const fs::path bare_out = kOut / "no_rows_written.inp";
    ASSERT_EQ(swmm_model_write(bare, bare_out.string().c_str()), SWMM_OK);
    close(bare);
    EXPECT_EQ(read_text(bare_out).find("[2D_NODE_COUPLING]"), std::string::npos);
}

// Rows illegal for the node class, unknown keys/types/nodes, keys that do not
// apply to the type, bad values and duplicates refuse the model.
TEST(NodeCoupling2DSection, IllegalRowsRefuseTheModel) {
    const char* bad[] = {
        "J1 OUTFALL",                  // OUTFALL only on outfalls
        "O1 MANHOLE",                  // an outfall is OUTFALL or NONE
        "J1 OPEN",                     // OPEN only on storage
        "J1 PIPE",                     // unknown type
        "NOPE MANHOLE",                // unknown node
        "J1 MANHOLE SPREAD=2",         // outfall key on a structure
        "O1 OUTFALL D=1",              // structure key on an outfall
        "J2 NONE D=1",                 // NONE takes no keys
        "J1 MANHOLE FOO=1",            // unknown key
        "J1 MANHOLE D",                // not KEY=VALUE
        "J1 MANHOLE LID_OPEN_FRACTION=2",
        "J1 MANHOLE D=-1",
        "J1 MANHOLE CREST=TOP",
        "J1 MANHOLE D=1 D=2",          // duplicate key
        "J1 MANHOLE\nJ1 GRATE",        // duplicate row
    };
    int i = 0;
    for (const char* row : bad) {
        int rc = SWMM_OK;
        SWMM_Engine e = open_text("bad_" + std::to_string(i++),
                                  deck(std::string("[2D_NODE_COUPLING]\n") + row + "\n"), &rc);
        EXPECT_EQ(e, nullptr) << "accepted: " << row;
        EXPECT_NE(rc, SWMM_OK) << row;
        close(e);
    }
}

TEST(NodeCoupling2DSection, ApiEditsValidateAndDropInapplicableKeys) {
    SWMM_Engine e = open_text("api", deck(kSection));
    ASSERT_NE(e, nullptr);
    const int j1 = swmm_node_index(e, "J1"), j4 = swmm_node_index(e, "J4"),
              o1 = swmm_node_index(e, "O1");

    EXPECT_EQ(swmm_node_set_2d_coupling_type(e, o1, SWMM_2D_COUPLING_MANHOLE), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_node_set_2d_coupling_type(e, j1, SWMM_2D_COUPLING_OUTFALL), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_node_set_2d_coupling_type(e, j1, 9), SWMM_ERR_BADPARAM);

    // Structure keys survive a switch between structure types and are dropped
    // by NONE (they do not apply).
    ASSERT_EQ(swmm_node_set_2d_coupling_type(e, j4, SWMM_2D_COUPLING_GRATE), SWMM_OK);
    EXPECT_EQ(param(e, "J4", "D"), "1.2");
    ASSERT_EQ(swmm_node_set_2d_coupling_type(e, j4, SWMM_2D_COUPLING_NONE), SWMM_OK);
    EXPECT_EQ(param(e, "J4", "D"), "");

    // A parameter on a node without a row authors one at the class default.
    int authored = -1;
    EXPECT_EQ(type_of(e, "J1", &authored), SWMM_2D_COUPLING_MANHOLE);
    EXPECT_EQ(authored, 0);
    ASSERT_EQ(swmm_node_set_2d_coupling_param(e, j1, "d", "1.5"), SWMM_OK);
    EXPECT_EQ(type_of(e, "J1", &authored), SWMM_2D_COUPLING_MANHOLE);
    EXPECT_EQ(authored, 1);
    EXPECT_EQ(param(e, "J1", "D"), "1.5");
    EXPECT_EQ(swmm_node_set_2d_coupling_param(e, j1, "SPREAD", "2"), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_node_set_2d_coupling_param(e, j1, "LAW", "WEIR"), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_node_set_2d_coupling_param(e, j1, "NOPE", "1"), SWMM_ERR_BADPARAM);
    ASSERT_EQ(swmm_node_set_2d_coupling_param(e, j1, "LAW", "weir_orifice"), SWMM_OK);
    EXPECT_EQ(param(e, "J1", "LAW"), "WEIR_ORIFICE");
    ASSERT_EQ(swmm_node_set_2d_coupling_param(e, j1, "D", ""), SWMM_OK);
    EXPECT_EQ(param(e, "J1", "D"), "");

    ASSERT_EQ(swmm_node_reset_2d_coupling(e, j1), SWMM_OK);
    EXPECT_EQ(type_of(e, "J1", &authored), SWMM_2D_COUPLING_MANHOLE);
    EXPECT_EQ(authored, 0);
    char buf[8];
    EXPECT_EQ(swmm_node_get_2d_coupling_param(e, j1, "NOPE", buf, 8), SWMM_ERR_BADPARAM);
    close(e);
}

// Rows key on the node index: deleting a node drops its row and renumbers the
// rest, so every row still names the node it was authored for.
TEST(NodeCoupling2DSection, NodeDeleteDropsAndRenumbersRows) {
    SWMM_Engine e = open_text("delete", deck(kSection));
    ASSERT_NE(e, nullptr);
    ASSERT_EQ(swmm_node_delete(e, swmm_node_index(e, "J1"), nullptr), SWMM_OK);
    EXPECT_EQ(type_of(e, "J3"), SWMM_2D_COUPLING_GRATE);
    EXPECT_EQ(param(e, "J3", "PERIMETER"), "1.8");
    EXPECT_EQ(type_of(e, "O1"), SWMM_2D_COUPLING_OUTFALL);
    ASSERT_EQ(swmm_node_delete(e, swmm_node_index(e, "J3"), nullptr), SWMM_OK);
    EXPECT_EQ(type_of(e, "J4"), SWMM_2D_COUPLING_MANHOLE);
    EXPECT_EQ(param(e, "J4", "D"), "1.2");

    const fs::path out = kOut / "delete_written.inp";
    ASSERT_EQ(swmm_model_write(e, out.string().c_str()), SWMM_OK);
    close(e);
    const std::string text = read_text(out);
    const auto at = text.find("[2D_NODE_COUPLING]");
    ASSERT_NE(at, std::string::npos) << text;
    const std::string section = text.substr(at, text.find("\n[", at + 1) - at);
    EXPECT_EQ(section.find("J3"), std::string::npos) << "deleted node's row survived:\n" << section;
    EXPECT_NE(section.find("J4               MANHOLE  D=1.2"), std::string::npos) << section;
}

TEST(NodeCoupling2DSection, GeoPackageRoundTrip) {
    SWMM_Engine e = open_text("gpkg", deck(kSection));
    ASSERT_NE(e, nullptr);
    const fs::path gpkg = kOut / "gpkg.gpkg";
    std::error_code ec;
    for (const char* sfx : {"", "-wal", "-shm"}) fs::remove(gpkg.string() + sfx, ec);
    const char* kGpkgId = "org.hydrocouple.openswmm.plugins.geopackage";
    const int rc = swmm_model_write_with_plugin(e, gpkg.string().c_str(), kGpkgId);
    close(e);
    if (rc != SWMM_OK) GTEST_SKIP() << "geopackage plugin unavailable (rc=" << rc << ")";

    SWMM_Engine e2 = swmm_engine_create();
    ASSERT_EQ(swmm_engine_open(e2, gpkg.string().c_str(), "", "", kGpkgId), SWMM_OK);
    expect_section_rows(e2);
    close(e2);
}

// ---------------------------------------------------------------------------
// Router: a map row naming a NONE node does not couple.
// ---------------------------------------------------------------------------

namespace {

// J1 drains a rained-on 10×10 m patch through vertex 0. `j1_row` is an
// optional [2D_NODE_COUPLING] row; `j2_row` likewise for an unmapped node.
std::string coupled_deck(const std::string& rows) {
    return deck(rows.empty() ? "" : "[2D_NODE_COUPLING]\n" + rows) +
        "\n[2D_OPTIONS]\n"
        "MAX_TIMESTEP     5\n"
        "DRY_DEPTH        0.002\n"
        "REPORT_2D        NO\n"
        "\n[2D_VERTICES]\n"
        " 0.0    0.0   4.0\n"
        "10.0    0.0   4.0\n"
        "10.0   10.0   4.0\n"
        " 0.0   10.0   4.0\n"
        "\n[2D_TRIANGLES]\n"
        "0  1  2  0.03\n"
        "0  2  3  0.03\n"
        "\n[2D_VERTEX_NODE_MAP]\n"
        "0  J1  0.7  0.1\n";
}

struct CoupledRun { bool ok = false; double drained = 0.0; std::string rpt; };

CoupledRun run_coupled(const std::string& tag, const std::string& rows) {
    CoupledRun r;
    fs::create_directories(kOut);
    const fs::path inp = kOut / (tag + ".inp"), rpt = kOut / (tag + ".rpt");
    { std::ofstream f(inp); f << coupled_deck(rows); }
    SWMM_Engine e = swmm_engine_create();
    if (swmm_engine_open(e, inp.string().c_str(), rpt.string().c_str(), "", nullptr) != SWMM_OK ||
        swmm_engine_initialize(e) != SWMM_OK || swmm_engine_start(e, 0) != SWMM_OK) {
        swmm_engine_destroy(e);
        return r;
    }
    double elapsed = 0.0;
    for (;;) {
        swmm_2d_force_rainfall_uniform(e, 0.0005, SWMM_FORCING_OVERRIDE, SWMM_FORCING_PERSIST);
        if (swmm_engine_step(e, &elapsed) != SWMM_OK || elapsed <= 0.0) break;
    }
    swmm_engine_end(e);
    swmm_2d_get_mass_balance(e, nullptr, nullptr, nullptr, nullptr, &r.drained,
                             nullptr, nullptr, nullptr, nullptr, nullptr);
    swmm_engine_report(e);
    close(e);
    r.rpt = read_text(rpt);
    r.ok = true;
    return r;
}

} // namespace

TEST(NodeCoupling2DSection, MapRowsOfNoneNodesAreIgnored) {
    CoupledRun coupled = run_coupled("router_default", "");
    CoupledRun none    = run_coupled("router_none", "J1 NONE\n");
    ASSERT_TRUE(coupled.ok && none.ok);
    EXPECT_GT(coupled.drained, 1.0) << "J1 (MANHOLE by default) should drain the patch";
    EXPECT_EQ(none.drained, 0.0) << "a NONE node's map row must not couple";
    EXPECT_NE(none.rpt.find("2D map rows ignored for 1 node(s)"), std::string::npos) << none.rpt;
    EXPECT_EQ(coupled.rpt.find("2D map rows ignored"), std::string::npos);
}

TEST(NodeCoupling2DSection, AuthoredCoupleableNodeWithoutMapRowIsReported) {
    CoupledRun r = run_coupled("router_unmapped", "J3 GRATE PERIMETER=1.8\n");
    ASSERT_TRUE(r.ok);
    EXPECT_NE(r.rpt.find("1 node(s) authored as coupleable in [2D_NODE_COUPLING] have no"),
              std::string::npos) << r.rpt;
    EXPECT_NE(r.rpt.find(": J3"), std::string::npos);
}
