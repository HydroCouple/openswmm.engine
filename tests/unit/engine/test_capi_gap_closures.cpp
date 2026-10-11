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
 * @file test_capi_gap_closures.cpp
 * @brief Input features that the C API could not reach before 2026-10-11
 *        (python/scripts/engine_feature_gaps.py), now readable and writable.
 *
 * @details Each closure is held to the same three claims:
 *          1. the value reads back what the parser stored (or the default);
 *          2. set → get round-trips, and the setter reaches the ENGINE field,
 *             not just a string table;
 *          3. a value set through the API survives save (swmm_model_write)
 *             and reopen.
 *
 *          Artifacts: tests/unit/engine/data/test_capi_gap_closures_artifacts/.
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_transport.h>

#include "core/SWMMEngine.hpp"

namespace fs = std::filesystem;

namespace {

openswmm::SWMMEngine& cpp(SWMM_Engine e) { return *static_cast<openswmm::SWMMEngine*>(e); }

fs::path artifactDir() {
    fs::path d = fs::current_path() / "test_capi_gap_closures_artifacts";
    fs::create_directories(d);
    return d;
}

std::string getopt(SWMM_Engine e, const char* key) {
    char buf[512] = {};
    if (swmm_options_get(e, key, buf, static_cast<int>(sizeof(buf))) != SWMM_OK) return "<ERR>";
    return buf;
}

std::string getext(SWMM_Engine e, const char* key) {
    char buf[512] = {};
    if (swmm_options_get_ext(e, key, buf, static_cast<int>(sizeof(buf))) != SWMM_OK)
        return "<ERR>";
    return buf;
}

/// A small 1D deck: two junctions, an outfall, two conduits.
const char* kDeck1D = R"([TITLE]
C-API gap closures

[OPTIONS]
FLOW_UNITS           CFS
FLOW_ROUTING         DYNWAVE
START_DATE           01/01/2024
START_TIME           00:00:00
END_DATE             01/01/2024
END_TIME             01:00:00
REPORT_STEP          00:15:00
ROUTING_STEP         00:00:30
COMPATIBILITY        5
WATER_DENSITY        999.5

[JUNCTIONS]
;;Name  Elev  MaxDepth
J1      10    5
J2      9     5

[OUTFALLS]
;;Name  Elev  Type
O1      8     FREE

[CONDUITS]
;;Name  From  To  Length  N      InOff  OutOff  InitQ
C1      J1    J2  100     0.013  0      0       0
C2      J2    O1  100     0.013  0      0       0

[XSECTIONS]
;;Link  Shape     G1  G2  G3  G4  Barrels
C1      CIRCULAR  1   0   0   0   1
C2      CIRCULAR  1   0   0   0   1
)";

/// Opens a deck written to the artifact directory; closes and destroys on scope exit.
struct OpenedDeck {
    SWMM_Engine e = nullptr;
    OpenedDeck(const fs::path& inp) {
        e = swmm_engine_create();
        const fs::path rpt = fs::path(inp).replace_extension(".rpt");
        const fs::path out = fs::path(inp).replace_extension(".out");
        rc = swmm_engine_open(e, inp.string().c_str(), rpt.string().c_str(),
                              out.string().c_str(), nullptr);
    }
    ~OpenedDeck() {
        if (e) {
            swmm_engine_close(e);
            swmm_engine_destroy(e);
        }
    }
    int rc = -1;
};

fs::path writeDeck(const char* name, const char* text) {
    const fs::path p = artifactDir() / name;
    std::ofstream(p) << text;
    return p;
}

}  // namespace

// ---------------------------------------------------------------------------
// [OPTIONS] keys that had no swmm_options_get/set branch.
// ---------------------------------------------------------------------------

TEST(CapiGapOptions, DefaultsAndParsedValuesRead) {
    OpenedDeck d(writeDeck("options_defaults.inp", kDeck1D));
    ASSERT_EQ(d.rc, SWMM_OK);
    EXPECT_DOUBLE_EQ(std::stod(getopt(d.e, "WATER_DENSITY")), 999.5);  // parsed
    EXPECT_DOUBLE_EQ(std::stod(getopt(d.e, "WATER_SPECIFIC_HEAT_CAPACITY")), 4184.0);
    // Full precision: std::to_string would have printed 0.000000.
    EXPECT_DOUBLE_EQ(std::stod(getopt(d.e, "WIND_FUNC_COEFF_A")), 1.505e-8);
    EXPECT_DOUBLE_EQ(std::stod(getopt(d.e, "WIND_FUNC_COEFF_B")), 1.6e-8);
    EXPECT_DOUBLE_EQ(std::stod(getopt(d.e, "PRESSURE_RATIO")), 1.0);
    EXPECT_EQ(getopt(d.e, "FV_NODE_FEEDBACK_DT"), "NO");
    EXPECT_EQ(getopt(d.e, "COMPATIBILITY"), "5");
    EXPECT_EQ(getopt(d.e, "VIRTUAL_JUNCTION_MOMENTUM"), "NONE");
}

TEST(CapiGapOptions, SettersReachTheEngine) {
    OpenedDeck d(writeDeck("options_set.inp", kDeck1D));
    ASSERT_EQ(d.rc, SWMM_OK);
    ASSERT_EQ(swmm_options_set(d.e, "WATER_DENSITY", "998.2"), SWMM_OK);
    ASSERT_EQ(swmm_options_set(d.e, "WATER_SPECIFIC_HEAT_CAPACITY", "4180"), SWMM_OK);
    ASSERT_EQ(swmm_options_set(d.e, "WIND_FUNC_COEFF_A", "2.25e-8"), SWMM_OK);
    ASSERT_EQ(swmm_options_set(d.e, "WIND_FUNC_COEFF_B", "1.75e-8"), SWMM_OK);
    ASSERT_EQ(swmm_options_set(d.e, "PRESSURE_RATIO", "0.95"), SWMM_OK);
    ASSERT_EQ(swmm_options_set(d.e, "FV_NODE_FEEDBACK_DT", "YES"), SWMM_OK);
    ASSERT_EQ(swmm_options_set(d.e, "COMPATIBILITY", "4"), SWMM_OK);

    const auto& o = cpp(d.e).context().options;
    EXPECT_DOUBLE_EQ(o.water_density, 998.2);
    EXPECT_DOUBLE_EQ(o.water_specific_heat, 4180.0);
    EXPECT_DOUBLE_EQ(o.wind_func_coeff_a, 2.25e-8);
    EXPECT_DOUBLE_EQ(o.wind_func_coeff_b, 1.75e-8);
    EXPECT_DOUBLE_EQ(o.pressure_ratio, 0.95);
    EXPECT_TRUE(o.fv.node_feedback_dt);
    EXPECT_EQ(getopt(d.e, "COMPATIBILITY"), "4");

    // get -> set(echo) is a no-op.
    const std::string echoed = getopt(d.e, "WIND_FUNC_COEFF_A");
    ASSERT_EQ(swmm_options_set(d.e, "WIND_FUNC_COEFF_A", echoed.c_str()), SWMM_OK);
    EXPECT_DOUBLE_EQ(o.wind_func_coeff_a, 2.25e-8);

    // Malformed numerics are refused and leave the value alone.
    EXPECT_EQ(swmm_options_set(d.e, "PRESSURE_RATIO", "abc"), SWMM_ERR_BADPARAM);
    EXPECT_DOUBLE_EQ(o.pressure_ratio, 0.95);
}

TEST(CapiGapOptions, RetiredVirtualJunctionMomentumRefusesFull) {
    OpenedDeck d(writeDeck("options_vjm.inp", kDeck1D));
    ASSERT_EQ(d.rc, SWMM_OK);
    EXPECT_EQ(swmm_options_set(d.e, "VIRTUAL_JUNCTION_MOMENTUM", "NONE"), SWMM_OK);
    EXPECT_EQ(swmm_options_set(d.e, "VIRTUAL_JUNCTION_MOMENTUM", "FULL"), SWMM_ERR_BADPARAM);
    EXPECT_EQ(getopt(d.e, "VIRTUAL_JUNCTION_MOMENTUM"), "NONE");
}

TEST(CapiGapOptions, ValuesSurviveSaveAndReopen) {
    const fs::path saved = artifactDir() / "options_saved.inp";
    {
        OpenedDeck d(writeDeck("options_roundtrip.inp", kDeck1D));
        ASSERT_EQ(d.rc, SWMM_OK);
        ASSERT_EQ(swmm_options_set(d.e, "WATER_DENSITY", "998.2"), SWMM_OK);
        ASSERT_EQ(swmm_options_set(d.e, "WATER_SPECIFIC_HEAT_CAPACITY", "4180"), SWMM_OK);
        ASSERT_EQ(swmm_options_set(d.e, "WIND_FUNC_COEFF_A", "2.25e-8"), SWMM_OK);
        ASSERT_EQ(swmm_options_set(d.e, "WIND_FUNC_COEFF_B", "1.75e-8"), SWMM_OK);
        ASSERT_EQ(swmm_options_set(d.e, "PRESSURE_RATIO", "0.95"), SWMM_OK);
        // FV knobs are written only under FLOW_ROUTING FV (InpWriter keeps a
        // DW deck's [OPTIONS] legacy-clean), so select FV for that key.
        ASSERT_EQ(swmm_options_set(d.e, "FLOW_ROUTING", "FV"), SWMM_OK);
        ASSERT_EQ(swmm_options_set(d.e, "FV_NODE_FEEDBACK_DT", "YES"), SWMM_OK);
        ASSERT_EQ(swmm_options_set(d.e, "COMPATIBILITY", "4"), SWMM_OK);
        ASSERT_EQ(swmm_model_write(d.e, saved.string().c_str()), SWMM_OK);
    }
    OpenedDeck r(saved);
    ASSERT_EQ(r.rc, SWMM_OK);
    EXPECT_DOUBLE_EQ(std::stod(getopt(r.e, "WATER_DENSITY")), 998.2);
    EXPECT_DOUBLE_EQ(std::stod(getopt(r.e, "WATER_SPECIFIC_HEAT_CAPACITY")), 4180.0);
    EXPECT_DOUBLE_EQ(std::stod(getopt(r.e, "WIND_FUNC_COEFF_A")), 2.25e-8);
    EXPECT_DOUBLE_EQ(std::stod(getopt(r.e, "WIND_FUNC_COEFF_B")), 1.75e-8);
    EXPECT_DOUBLE_EQ(std::stod(getopt(r.e, "PRESSURE_RATIO")), 0.95);
    EXPECT_EQ(getopt(r.e, "FV_NODE_FEEDBACK_DT"), "YES");
    EXPECT_EQ(getopt(r.e, "COMPATIBILITY"), "4");
}

// ---------------------------------------------------------------------------
// [2D_OPTIONS] DISPERSION: was missing from is2DOptionKey, so set_ext wrote
// the generic ext map instead of SolverOptions2D::dispersion.
// ---------------------------------------------------------------------------

#ifdef OPENSWMM_HAS_2D
TEST(CapiGap2DOptions, DispersionReachesTheSolverOptions) {
    // Working directory is tests/unit/engine/data.
    const fs::path deck = fs::path("../../../../examples/2d_complete_example.inp");
    ASSERT_TRUE(fs::exists(deck)) << fs::absolute(deck);
    const fs::path inp = artifactDir() / "dispersion_2d.inp";
    fs::copy_file(deck, inp, fs::copy_options::overwrite_existing);
    {
        OpenedDeck d(inp);
        ASSERT_EQ(d.rc, SWMM_OK);
        EXPECT_DOUBLE_EQ(std::stod(getext(d.e, "DISPERSION")), 0.0);
        ASSERT_EQ(swmm_options_set_ext(d.e, "DISPERSION", "0.25"), SWMM_OK);
        EXPECT_DOUBLE_EQ(std::stod(getext(d.e, "DISPERSION")), 0.25);
        ASSERT_NE(cpp(d.e).context().twod_io.options, nullptr);
        EXPECT_DOUBLE_EQ(cpp(d.e).context().twod_io.options->dispersion, 0.25);
        EXPECT_EQ(cpp(d.e).context().options.ext_options.count("DISPERSION"), 0u);
        EXPECT_EQ(swmm_options_set_ext(d.e, "DISPERSION", "-1"), SWMM_ERR_BADPARAM);
        ASSERT_EQ(swmm_model_write(d.e, (artifactDir() / "dispersion_2d_saved.inp")
                                           .string().c_str()), SWMM_OK);
    }
    OpenedDeck r(artifactDir() / "dispersion_2d_saved.inp");
    ASSERT_EQ(r.rc, SWMM_OK);
    EXPECT_DOUBLE_EQ(std::stod(getext(r.e, "DISPERSION")), 0.25);
}
#endif

// ---------------------------------------------------------------------------
// transport.ard authoring: DETAILED_OUTPUT, [CONDUIT_DISPERSION],
// [TRANSPORT_BOUNDARIES] and [TRANSPORT_SOURCES] were read-only.
// ---------------------------------------------------------------------------

namespace {

void writeText(const fs::path& p, const std::string& text) { std::ofstream(p) << text; }

std::string slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

/// Deck with an inert reactions component (species X) and a transport.ard
/// component; files live in the artifact directory under @p stem.
fs::path writeArdDeck(const std::string& stem, const std::string& ard) {
    const fs::path dir = artifactDir();
    writeText(dir / (stem + ".rxn"),
              "[REACTION_OPTIONS]\nRATE_UNITS SEC\n[REACTION_SPECIES]\nBULK X MG\n"
              "[REACTION_PIPES]\nRATE X 0\n[REACTION_TANKS]\nRATE X 0\n");
    writeText(dir / (stem + ".ard"), ard);
    std::string deck = kDeck1D;
    deck += "\n[TIMESERIES]\nsrc_ts 0 1.0\nsrc_ts 1 1.0\n\n[PROCESS_COMPONENTS]\n"
            "org.hydrocouple.openswmm.reactions config=\"" + stem + ".rxn\"\n"
            "org.hydrocouple.openswmm.transport.ard config=\"" + stem + ".ard\"\n";
    const fs::path inp = dir / (stem + ".inp");
    writeText(inp, deck);
    return inp;
}

int boundaryCount(SWMM_Engine e) { int n = -1; swmm_transport_boundary_count(e, &n); return n; }
int sourceCount(SWMM_Engine e) { int n = -1; swmm_transport_source_count(e, &n); return n; }

}  // namespace

TEST(CapiGapTransport, DetailedOutputReadsAndWrites) {
    OpenedDeck d(writeArdDeck("ard_detail", "[TRANSPORT_OPTIONS]\nDETAILED_OUTPUT cells.csv\n"));
    ASSERT_EQ(d.rc, SWMM_OK) << swmm_get_last_error_msg(d.e);
    char buf[1024] = {};
    ASSERT_EQ(swmm_transport_get_detailed_output(d.e, buf, sizeof(buf)), SWMM_OK);
    EXPECT_EQ(fs::path(buf).filename(), "cells.csv");  // resolved against the .ard dir
    ASSERT_EQ(swmm_transport_set_detailed_output(d.e, "other.csv"), SWMM_OK);
    ASSERT_EQ(swmm_transport_get_detailed_output(d.e, buf, sizeof(buf)), SWMM_OK);
    EXPECT_STREQ(buf, "other.csv");
    EXPECT_EQ(cpp(d.e).context().ard_config.detailed_output_path, "other.csv");
    ASSERT_EQ(swmm_transport_set_detailed_output(d.e, ""), SWMM_OK);
    EXPECT_TRUE(cpp(d.e).context().ard_config.detailed_output_path.empty());
}

TEST(CapiGapTransport, ConduitDispersionUpsertAndRemove) {
    OpenedDeck d(writeArdDeck("ard_disp", "[TRANSPORT_OPTIONS]\nDISPERSION 0\n"));
    ASSERT_EQ(d.rc, SWMM_OK) << swmm_get_last_error_msg(d.e);
    const int c2 = swmm_link_index(d.e, "C2");
    ASSERT_EQ(swmm_transport_set_conduit_disp(d.e, c2, 0.5), SWMM_OK);
    ASSERT_EQ(swmm_transport_set_conduit_disp(d.e, c2, 0.75), SWMM_OK);  // replaces
    int n = 0, link = -1;
    double v = 0;
    ASSERT_EQ(swmm_transport_conduit_disp_count(d.e, &n), SWMM_OK);
    EXPECT_EQ(n, 1);
    ASSERT_EQ(swmm_transport_get_conduit_disp(d.e, 0, &link, &v), SWMM_OK);
    EXPECT_EQ(link, c2);
    EXPECT_DOUBLE_EQ(v, 0.75);
    EXPECT_EQ(swmm_transport_set_conduit_disp(d.e, c2, -1.0), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_transport_set_conduit_disp(d.e, 99, 1.0), SWMM_ERR_BADINDEX);
    ASSERT_EQ(swmm_transport_remove_conduit_disp(d.e, 0), SWMM_OK);
    ASSERT_EQ(swmm_transport_conduit_disp_count(d.e, &n), SWMM_OK);
    EXPECT_EQ(n, 0);
    EXPECT_EQ(swmm_transport_remove_conduit_disp(d.e, 0), SWMM_ERR_BADINDEX);
}

TEST(CapiGapTransport, BoundaryAndSourceRowsResolveOrAreRefused) {
    OpenedDeck d(writeArdDeck("ard_rows", "[TRANSPORT_OPTIONS]\nDISPERSION 0\n"));
    ASSERT_EQ(d.rc, SWMM_OK) << swmm_get_last_error_msg(d.e);
    const auto& cfg = cpp(d.e).context().ard_config;

    ASSERT_EQ(swmm_transport_set_boundary(d.e, "J1", "X", 0, 8.25, nullptr), SWMM_OK);
    EXPECT_EQ(boundaryCount(d.e), 1);
    ASSERT_EQ(cfg.bc_node.size(), 1u);  // resolved, so the run sees it
    EXPECT_EQ(cfg.bc_node[0], swmm_node_index(d.e, "J1"));
    EXPECT_DOUBLE_EQ(cfg.bc_value[0], 8.25);
    ASSERT_EQ(swmm_transport_set_boundary(d.e, "J1", "X", 0, 4.0, nullptr), SWMM_OK);
    EXPECT_EQ(boundaryCount(d.e), 1);  // same node + species replaces
    EXPECT_DOUBLE_EQ(cfg.bc_value[0], 4.0);

    // Refusals leave the configuration as it was.
    EXPECT_EQ(swmm_transport_set_boundary(d.e, "NOPE", "X", 0, 1.0, nullptr), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_transport_set_boundary(d.e, "J2", "Y", 0, 1.0, nullptr), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_transport_set_boundary(d.e, "J2", "X", 0, -1.0, nullptr), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_transport_set_boundary(d.e, "J2", "X", 1, 0.0, "no_ts"), SWMM_ERR_BADPARAM);
    EXPECT_EQ(boundaryCount(d.e), 1);
    EXPECT_EQ(cfg.bc_node.size(), 1u);

    ASSERT_EQ(swmm_transport_set_source(d.e, "C1", "X", 1, 0.0, "src_ts"), SWMM_OK);
    ASSERT_EQ(swmm_transport_set_source(d.e, "C2", "X", 0, -0.5, nullptr), SWMM_OK);  // extraction
    EXPECT_EQ(sourceCount(d.e), 2);
    EXPECT_EQ(cfg.src_link.size(), 2u);
    EXPECT_EQ(swmm_transport_set_source(d.e, "J1", "X", 0, 1.0, nullptr), SWMM_ERR_BADPARAM);

    ASSERT_EQ(swmm_transport_remove_source(d.e, 0), SWMM_OK);
    EXPECT_EQ(sourceCount(d.e), 1);
    EXPECT_EQ(cfg.src_link.size(), 1u);
    ASSERT_EQ(swmm_transport_remove_boundary(d.e, 0), SWMM_OK);
    EXPECT_EQ(boundaryCount(d.e), 0);
    EXPECT_TRUE(cfg.bc_node.empty());
    EXPECT_EQ(swmm_transport_remove_boundary(d.e, 0), SWMM_ERR_BADINDEX);
}

TEST(CapiGapTransport, EditsSurviveSaveAndReopen) {
    const fs::path inp = writeArdDeck("ard_save", "[TRANSPORT_OPTIONS]\nDISPERSION 0\n");
    const fs::path saved = artifactDir() / "ard_save_out.inp";
    {
        OpenedDeck d(inp);
        ASSERT_EQ(d.rc, SWMM_OK) << swmm_get_last_error_msg(d.e);
        ASSERT_EQ(swmm_transport_set_detailed_output(d.e, "cells.csv"), SWMM_OK);
        ASSERT_EQ(swmm_transport_set_conduit_disp(d.e, swmm_link_index(d.e, "C2"), 0.75), SWMM_OK);
        ASSERT_EQ(swmm_transport_set_boundary(d.e, "J1", "X", 0, 8.25, nullptr), SWMM_OK);
        ASSERT_EQ(swmm_transport_set_source(d.e, "C1", "X", 1, 0.0, "src_ts"), SWMM_OK);
        ASSERT_EQ(swmm_model_write(d.e, saved.string().c_str()), SWMM_OK)
            << swmm_get_last_error_msg(d.e);
    }
    const std::string ard = slurp(artifactDir() / "ard_save.ard");
    EXPECT_NE(ard.find("DETAILED_OUTPUT"), std::string::npos) << ard;
    EXPECT_NE(ard.find("[CONDUIT_DISPERSION]"), std::string::npos) << ard;

    OpenedDeck r(saved);
    ASSERT_EQ(r.rc, SWMM_OK) << swmm_get_last_error_msg(r.e);
    char buf[1024] = {};
    ASSERT_EQ(swmm_transport_get_detailed_output(r.e, buf, sizeof(buf)), SWMM_OK);
    EXPECT_EQ(fs::path(buf).filename(), "cells.csv");
    int n = 0;
    ASSERT_EQ(swmm_transport_conduit_disp_count(r.e, &n), SWMM_OK);
    EXPECT_EQ(n, 1);
    EXPECT_EQ(boundaryCount(r.e), 1);
    EXPECT_EQ(sourceCount(r.e), 1);
}

TEST(CapiGapTransport, EditsAreRefusedOnceTheRunStarts) {
    OpenedDeck d(writeArdDeck("ard_lifecycle", "[TRANSPORT_OPTIONS]\nDISPERSION 0\n"));
    ASSERT_EQ(d.rc, SWMM_OK) << swmm_get_last_error_msg(d.e);
    ASSERT_EQ(swmm_engine_initialize(d.e), SWMM_OK);
    EXPECT_EQ(swmm_transport_set_boundary(d.e, "J1", "X", 0, 1.0, nullptr), SWMM_ERR_LIFECYCLE);
    EXPECT_EQ(swmm_transport_set_detailed_output(d.e, "x.csv"), SWMM_ERR_LIFECYCLE);
}
