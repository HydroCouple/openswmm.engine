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
 * @file test_water_age_api.cpp
 * @brief X5: the water-age source-table C API (A6-min), and X6's owed
 *        API-flux extraction gate.
 *
 * @details Subplan X5 (= water-age plan A6's GUI-facing subset; GUI plan
 *          §6 prereq 5). The claims:
 *
 *          1. CRUD round-trips: globals and per-node overrides set, read
 *             back, update in place (not duplicate), remove, and count —
 *             in HOURS across the boundary, SECONDS inside.
 *          2. The API's scope rule is the PARSER's rule: only DWF and
 *             EXTERNAL_INFLOW take NODE overrides, and bad handles /
 *             indices / null pointers return the house error codes. An
 *             editor must not be able to author a table the file parser
 *             would refuse.
 *          3. `swmm_water_age_save` writes a file the waterage COMPONENT
 *             parses back to the same values — the editor's save path,
 *             verified through the parser rather than by string matching.
 *          4. **Edits reach the engine**: a global EXTERNAL_INFLOW age set
 *             through the API before `start()` shifts the outfall age by
 *             exactly that amount (the A1a-gate-2 claim, now via the API).
 *          5. **X6.vi's owed gate**: a NEGATIVE `swmm_node_set_quality_mass_flux`
 *             extracts mass at runtime, clamps, warns, and never drives a
 *             concentration below zero — the observer the X6 round could
 *             not build without this API surface in hand.
 *
 *          Scratch fixtures use the `_wa_` prefix (collision-checked).
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_nodes.h>
#include <openswmm/engine/openswmm_water_age.h>

#include "core/SWMMEngine.hpp"

namespace {

openswmm::SWMMEngine& as_cpp_engine(SWMM_Engine engine) {
    return *static_cast<openswmm::SWMMEngine*>(engine);
}

constexpr double kQ = 5.0;
constexpr double kCin = 100.0;

/// Five-conduit chain, WATER_AGE ON, optional waterage component file.
void write_deck(const std::string& path, const char* component_line) {
    std::ofstream f(path);
    f << "[TITLE]\nX5 age API\n\n[OPTIONS]\n"
      << "FLOW_UNITS CFS\nFLOW_ROUTING DYNWAVE\n"
      << "QUALITY_SOLVER LAGRANGIAN\nWATER_AGE ON\n"
      << "START_DATE 01/01/2026\nSTART_TIME 00:00:00\n"
      << "END_DATE 01/01/2026\nEND_TIME 04:00:00\n"
      << "ROUTING_STEP 5\nREPORT_STEP 00:05:00\n\n"
      << "[JUNCTIONS]\n"
      << "J0 10.0 10 1.5 0 0\nJ1 9.4  10 1.5 0 0\nJ2 8.8  10 1.5 0 0\n"
      << "J3 8.2  10 1.5 0 0\nJ4 7.6  10 1.5 0 0\n\n"
      << "[OUTFALLS]\nOUT 7.0 FREE  NO\n\n"
      << "[CONDUITS]\n"
      << "C1 J0 J1 500 0.013 0 0 0\nC2 J1 J2 500 0.013 0 0 0\n"
      << "C3 J2 J3 500 0.013 0 0 0\nC4 J3 J4 500 0.013 0 0 0\n"
      << "C5 J4 OUT 500 0.013 0 0 0\n\n"
      << "[XSECTIONS]\n"
      << "C1 CIRCULAR 2.0 0 0 0\nC2 CIRCULAR 2.0 0 0 0\n"
      << "C3 CIRCULAR 2.0 0 0 0\nC4 CIRCULAR 2.0 0 0 0\n"
      << "C5 CIRCULAR 2.0 0 0 0\n\n"
      << "[POLLUTANTS]\nTSS MG/L 0 0 0 0 NO * 0 0 0\n\n";
    if (component_line != nullptr)
        f << "[PROCESS_COMPONENTS]\n" << component_line << "\n\n";
    f << "[INFLOWS]\n"
      << "J0 FLOW \"\" FLOW 1.0 1.0 " << kQ << "\n"
      << "J0 TSS  \"\" CONCEN 1.0 1.0 " << kCin << "\n\n"
      << "[REPORT]\nINPUT NO\n";
}

/// Open an engine on a fresh deck (OPENED state — where an editor sits).
SWMM_Engine open_deck(const std::string& tag,
                      const char* component_line = nullptr) {
    write_deck(tag + ".inp", component_line);
    SWMM_Engine e = swmm_engine_create();
    if (e == nullptr) return nullptr;
    if (swmm_engine_open(e, (tag + ".inp").c_str(), (tag + ".rpt").c_str(),
                         (tag + ".out").c_str(), nullptr) != SWMM_OK) {
        swmm_engine_destroy(e);
        return nullptr;
    }
    return e;
}

// ---------------------------------------------------------------------------
// Gate 1 — CRUD round-trips, hours across the boundary.
// ---------------------------------------------------------------------------
TEST(WaterAgeApiTest, CrudRoundTripsInHours) {
    SWMM_Engine e = open_deck("_wa_crud");
    ASSERT_NE(e, nullptr);

    int enabled = -1;
    ASSERT_EQ(swmm_water_age_get_enabled(e, &enabled), SWMM_OK);
    EXPECT_EQ(enabled, 1) << "the deck says WATER_AGE ON";

    // Globals: default 0, set, read back, and the engine holds SECONDS.
    double h = -1.0;
    ASSERT_EQ(swmm_water_age_get_global_source(e, SWMM_AGE_SRC_GW, &h),
              SWMM_OK);
    EXPECT_DOUBLE_EQ(h, 0.0);
    ASSERT_EQ(swmm_water_age_set_global_source(e, SWMM_AGE_SRC_GW, 6.0),
              SWMM_OK);
    ASSERT_EQ(swmm_water_age_get_global_source(e, SWMM_AGE_SRC_GW, &h),
              SWMM_OK);
    EXPECT_DOUBLE_EQ(h, 6.0);
    EXPECT_DOUBLE_EQ(
        as_cpp_engine(e).context().water_age_config.global_age[
            static_cast<int>(openswmm::WaterAgeSource::GW)],
        21600.0)
        << "the boundary is HOURS and the engine stores SECONDS";

    // D-NS1: negatives pass through unfloored.
    ASSERT_EQ(swmm_water_age_set_global_source(e, SWMM_AGE_SRC_RDII, -2.0),
              SWMM_OK);
    ASSERT_EQ(swmm_water_age_get_global_source(e, SWMM_AGE_SRC_RDII, &h),
              SWMM_OK);
    EXPECT_DOUBLE_EQ(h, -2.0) << "a negative source age was floored — "
                                 "extraction must survive the boundary";

    // Overrides: add two, update one in place, remove one.
    int count = -1;
    ASSERT_EQ(swmm_water_age_override_count(e, &count), SWMM_OK);
    EXPECT_EQ(count, 0);
    ASSERT_EQ(swmm_water_age_set_override(e, SWMM_AGE_SRC_DWF, 0, 3.0),
              SWMM_OK);
    ASSERT_EQ(
        swmm_water_age_set_override(e, SWMM_AGE_SRC_EXTERNAL_INFLOW, 1, 4.0),
        SWMM_OK);
    ASSERT_EQ(swmm_water_age_override_count(e, &count), SWMM_OK);
    EXPECT_EQ(count, 2);

    ASSERT_EQ(swmm_water_age_set_override(e, SWMM_AGE_SRC_DWF, 0, 9.0),
              SWMM_OK);
    ASSERT_EQ(swmm_water_age_override_count(e, &count), SWMM_OK);
    EXPECT_EQ(count, 2) << "an update duplicated the row instead of "
                           "replacing it";

    int src = -1, nd = -1;
    double hv = 0.0;
    ASSERT_EQ(swmm_water_age_get_override(e, 0, &src, &nd, &hv), SWMM_OK);
    EXPECT_EQ(src, SWMM_AGE_SRC_DWF);
    EXPECT_EQ(nd, 0);
    EXPECT_DOUBLE_EQ(hv, 9.0);

    ASSERT_EQ(swmm_water_age_remove_override(e, SWMM_AGE_SRC_DWF, 0),
              SWMM_OK);
    ASSERT_EQ(swmm_water_age_override_count(e, &count), SWMM_OK);
    EXPECT_EQ(count, 1);
    // The survivor is the one we did not remove.
    ASSERT_EQ(swmm_water_age_get_override(e, 0, &src, &nd, &hv), SWMM_OK);
    EXPECT_EQ(src, SWMM_AGE_SRC_EXTERNAL_INFLOW);
    EXPECT_DOUBLE_EQ(hv, 4.0);

    swmm_engine_destroy(e);
}

// ---------------------------------------------------------------------------
// Gate 2 — the API refuses exactly what the parser refuses.
// ---------------------------------------------------------------------------
TEST(WaterAgeApiTest, RejectsBadArgumentsAndOutOfScopeOverrides) {
    double h = 0.0;
    int n = 0;
    // Bad handle on every entry point.
    EXPECT_EQ(swmm_water_age_get_enabled(nullptr, &n), SWMM_ERR_BADHANDLE);
    EXPECT_EQ(swmm_water_age_get_global_source(nullptr, 0, &h),
              SWMM_ERR_BADHANDLE);
    EXPECT_EQ(swmm_water_age_set_global_source(nullptr, 0, 1.0),
              SWMM_ERR_BADHANDLE);
    EXPECT_EQ(swmm_water_age_override_count(nullptr, &n),
              SWMM_ERR_BADHANDLE);
    EXPECT_EQ(swmm_water_age_set_override(nullptr, 1, 0, 1.0),
              SWMM_ERR_BADHANDLE);
    EXPECT_EQ(swmm_water_age_remove_override(nullptr, 1, 0),
              SWMM_ERR_BADHANDLE);
    EXPECT_EQ(swmm_water_age_save(nullptr, "_wa_x.age"), SWMM_ERR_BADHANDLE);

    SWMM_Engine e = open_deck("_wa_bad");
    ASSERT_NE(e, nullptr);

    // Null out-pointers and bad source codes.
    EXPECT_EQ(swmm_water_age_get_enabled(e, nullptr), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_water_age_get_global_source(e, 0, nullptr),
              SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_water_age_get_global_source(e, -1, &h), SWMM_ERR_BADINDEX);
    EXPECT_EQ(swmm_water_age_get_global_source(e, SWMM_AGE_SRC_COUNT, &h),
              SWMM_ERR_BADINDEX);
    EXPECT_EQ(swmm_water_age_set_global_source(e, 99, 1.0), SWMM_ERR_BADINDEX);
    EXPECT_EQ(swmm_water_age_save(e, nullptr), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_water_age_save(e, ""), SWMM_ERR_BADPARAM);

    // The A1a scope rule: GW/RAINFALL/etc. take GLOBAL only.
    EXPECT_EQ(swmm_water_age_set_override(e, SWMM_AGE_SRC_GW, 0, 1.0),
              SWMM_ERR_BADPARAM)
        << "the API allowed a NODE override the file parser refuses";
    EXPECT_EQ(swmm_water_age_set_override(e, SWMM_AGE_SRC_INITIAL_STATE, 0,
                                          1.0),
              SWMM_ERR_BADPARAM);
    // Bad node index, and removing a row that does not exist.
    EXPECT_EQ(swmm_water_age_set_override(e, SWMM_AGE_SRC_DWF, 999, 1.0),
              SWMM_ERR_BADINDEX);
    EXPECT_EQ(swmm_water_age_remove_override(e, SWMM_AGE_SRC_DWF, 0),
              SWMM_ERR_BADINDEX);
    // Reading past the end.
    EXPECT_EQ(swmm_water_age_get_override(e, 0, nullptr, nullptr, nullptr),
              SWMM_ERR_BADINDEX);

    swmm_engine_destroy(e);
}

// ---------------------------------------------------------------------------
// Gate 3 — save writes what the COMPONENT parses back (round-trip through
// the parser, not through string matching).
// ---------------------------------------------------------------------------
TEST(WaterAgeApiTest, SavedFileParsesBackToTheSameTable) {
    SWMM_Engine a = open_deck("_wa_save");
    ASSERT_NE(a, nullptr);
    ASSERT_EQ(swmm_water_age_set_global_source(a, SWMM_AGE_SRC_INITIAL_STATE,
                                               1.5),
              SWMM_OK);
    ASSERT_EQ(swmm_water_age_set_global_source(a, SWMM_AGE_SRC_RDII, -2.0),
              SWMM_OK);
    ASSERT_EQ(swmm_water_age_set_override(a, SWMM_AGE_SRC_EXTERNAL_INFLOW, 0,
                                          6.0),
              SWMM_OK);
    ASSERT_EQ(swmm_water_age_save(a, "_wa_saved.age"), SWMM_OK);
    swmm_engine_destroy(a);

    // Reopen a deck that REGISTERS the saved file as its component config.
    SWMM_Engine b = open_deck(
        "_wa_reopen",
        "org.hydrocouple.openswmm.waterage config=\"_wa_saved.age\"");
    ASSERT_NE(b, nullptr) << "the saved component file failed to parse";

    double h = 0.0;
    ASSERT_EQ(swmm_water_age_get_global_source(b, SWMM_AGE_SRC_INITIAL_STATE,
                                               &h),
              SWMM_OK);
    EXPECT_DOUBLE_EQ(h, 1.5);
    ASSERT_EQ(swmm_water_age_get_global_source(b, SWMM_AGE_SRC_RDII, &h),
              SWMM_OK);
    EXPECT_DOUBLE_EQ(h, -2.0)
        << "the negative (extraction) row did not survive the round-trip";

    int count = 0;
    ASSERT_EQ(swmm_water_age_override_count(b, &count), SWMM_OK);
    ASSERT_EQ(count, 1);
    int src = -1, nd = -1;
    ASSERT_EQ(swmm_water_age_get_override(b, 0, &src, &nd, &h), SWMM_OK);
    EXPECT_EQ(src, SWMM_AGE_SRC_EXTERNAL_INFLOW);
    EXPECT_EQ(nd, 0) << "the node NAME did not resolve back to its index";
    EXPECT_DOUBLE_EQ(h, 6.0);

    // Untouched sources stay absent (a save-as of an unedited model must
    // not look configured).
    ASSERT_EQ(swmm_water_age_get_global_source(b, SWMM_AGE_SRC_GW, &h),
              SWMM_OK);
    EXPECT_DOUBLE_EQ(h, 0.0);
    // The value check above cannot SEE a written zero row — "GW GLOBAL 0"
    // parses back to the same 0.0 as absence (measured: X5 falsifier vi
    // was invisible to it). Absence is a property of the FILE, so this
    // one leg reads the file: no untouched source may appear at all.
    {
        std::ifstream fin("_wa_saved.age");
        const std::string body((std::istreambuf_iterator<char>(fin)),
                               std::istreambuf_iterator<char>());
        EXPECT_EQ(body.find("GW"), std::string::npos)
            << "an untouched source wrote a (zero) row:\n" << body;
        EXPECT_EQ(body.find("RAINFALL"), std::string::npos);
    }
    swmm_engine_destroy(b);
}

// ---------------------------------------------------------------------------
// Gate 4 — an API edit reaches the engine: the outfall age shifts by it.
// ---------------------------------------------------------------------------
TEST(WaterAgeApiTest, ApiEditShiftsTheSimulatedAge) {
    auto run = [](const std::string& tag, double hours) {
        SWMM_Engine e = open_deck(tag);
        EXPECT_NE(e, nullptr);
        if (e == nullptr) return 0.0;
        if (hours != 0.0)
            EXPECT_EQ(swmm_water_age_set_global_source(
                          e, SWMM_AGE_SRC_EXTERNAL_INFLOW, hours),
                      SWMM_OK);
        EXPECT_EQ(swmm_engine_initialize(e), SWMM_OK);
        EXPECT_EQ(swmm_engine_start(e, 1), SWMM_OK);
        double elapsed = 0.0;
        int guard = 0;
        do {
            if (swmm_engine_step(e, &elapsed) != SWMM_OK) break;
        } while (elapsed > 0.0 && ++guard < 200000);
        swmm_engine_end(e);
        auto& ctx = as_cpp_engine(e).context();
        double age = 0.0;
        for (int j = 0; j < ctx.n_nodes(); ++j)
            if (ctx.nodes.type[static_cast<std::size_t>(j)] ==
                openswmm::NodeType::OUTFALL)
                age = ctx.water_age_state.node_age[
                    static_cast<std::size_t>(j)];
        swmm_engine_destroy(e);
        return age;
    };

    const double base = run("_wa_shift_base", 0.0);
    const double src = run("_wa_shift_src", 6.0);
    ASSERT_GT(base, 600.0) << "no age developed (deck premise)";
    // Steady state: every parcel entered 6 h older (the A1a gate-2 claim,
    // reached through the API instead of a config file).
    EXPECT_NEAR(src - base, 21600.0, 60.0)
        << "an API-set source age did not reach the engine (base " << base
        << " s, src " << src << " s)";
}

// ---------------------------------------------------------------------------
// Gate 5 — X6.vi's OWED GATE: a negative API mass flux extracts, clamps,
// warns, and never goes negative.
// ---------------------------------------------------------------------------
TEST(WaterAgeApiTest, NegativeApiMassFluxExtractsAndClamps) {
    auto run = [](const std::string& tag, double flux, double* min_conc,
                  long* clamps, bool* warned) {
        SWMM_Engine e = open_deck(tag);
        EXPECT_NE(e, nullptr);
        if (e == nullptr) return 0.0;
        EXPECT_EQ(swmm_engine_initialize(e), SWMM_OK);
        EXPECT_EQ(swmm_engine_start(e, 1), SWMM_OK);
        auto& ctx = as_cpp_engine(e).context();
        const int np = ctx.n_pollutants();
        double elapsed = 0.0;
        int guard = 0;
        *min_conc = 1.0e300;
        do {
            // The flux is PERSISTENT (the engine never clears it — see
            // SWMMEngine.cpp's "Persistent user quality mass flux" note),
            // so re-applying each step is redundant. Done anyway so the
            // gate reads as a forcing loop and would still hold if the
            // convention ever changed to per-step.
            if (flux != 0.0)
                EXPECT_EQ(swmm_node_set_quality_mass_flux(e, 2, 0, flux),
                          SWMM_OK);
            if (swmm_engine_step(e, &elapsed) != SWMM_OK) break;
            for (int j = 0; j < ctx.n_nodes(); ++j)
                *min_conc = std::min(
                    *min_conc,
                    ctx.nodes.conc[static_cast<std::size_t>(j * np)]);
        } while (elapsed > 0.0 && ++guard < 200000);
        swmm_engine_end(e);
        double outfall = 0.0;
        for (int j = 0; j < ctx.n_nodes(); ++j)
            if (ctx.nodes.type[static_cast<std::size_t>(j)] ==
                openswmm::NodeType::OUTFALL)
                outfall = ctx.nodes.conc[static_cast<std::size_t>(j * np)];
        *clamps = ctx.negsrc.clamp_events;
        *warned = false;
        for (const auto& w : ctx.warnings)
            if (w.find("negative quality mass flux") != std::string::npos)
                *warned = true;
        swmm_engine_destroy(e);
        return outfall;
    };

    double min_a = 0.0, min_b = 0.0, min_c = 0.0;
    long cl_a = 0, cl_b = 0, cl_c = 0;
    bool warn_a = false, warn_b = false, warn_c = false;

    const double base = run("_wa_flux_base", 0.0, &min_a, &cl_a, &warn_a);
    ASSERT_GT(base, 0.9 * kCin) << "control never reached steady state";
    EXPECT_FALSE(warn_a) << "the API warning fired with no negative flux";

    // Moderate extraction: measurably below the control, still positive.
    const double mod = run("_wa_flux_mod", -50.0, &min_b, &cl_b, &warn_b);
    EXPECT_LT(mod, base * 0.999)
        << "a negative API mass flux had NO effect — this is exactly the "
           "`w <= 0` skip X6 removed (falsifier X6.vi's observer)";
    EXPECT_TRUE(warn_b) << "the negative-API-flux warning did not fire";
    EXPECT_GE(min_b, -1.0e-12);

    // Extreme extraction: clamps, warns, never negative.
    const double big = run("_wa_flux_big", -1.0e6, &min_c, &cl_c, &warn_c);
    EXPECT_GE(min_c, -1.0e-12)
        << "extreme API extraction drove a concentration negative";
    EXPECT_GT(cl_c, 0) << "extreme API extraction never clamped";
    EXPECT_LT(big, mod) << "more extraction did not remove more mass";
}

}  // namespace
