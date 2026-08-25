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
 * @file test_initial_quality.cpp
 * @brief E-A1 gates: [INITIAL_QUALITY] parse, resolve, round-trip.
 *
 * @details Falsifiers (each gate states what would make it fail):
 *          - FourRowsParseAndResolve fails if the section handler is never
 *            registered (count()==0), if name resolution or constituent
 *            classification is wrong, or if the inert-row warning is dropped.
 *          - The error-matrix tests fail if any validation path is removed
 *            (each asserts open() FAILS with a message naming the section).
 *          - SaveRoundTrip fails while the writer emits nothing (second
 *            parse count()==0) or drops/permutes any column.
 *          - BehaviorNeutralThisRound pins E-A1's no-consumption contract:
 *            adding the section must not change the .out. E-A2 REWRITES this
 *            test when consumption lands.
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_model.h>

#include "core/SWMMEngine.hpp"
#include "data/InitialQualityData.hpp"

namespace {

openswmm::SWMMEngine& as_cpp_engine(SWMM_Engine engine) {
    return *static_cast<openswmm::SWMMEngine*>(engine);
}

/// Minimal wet two-junction deck with one pollutant. `iq_lines` lands in an
/// [INITIAL_QUALITY] section when non-empty; `options_extra` is appended to
/// [OPTIONS] verbatim.
void write_deck(const char* path, const std::string& iq_lines,
                const std::string& options_extra = "") {
    std::ofstream f(path);
    f << "[TITLE]\nE-A1 initial quality gate deck\n\n"
      << "[OPTIONS]\n"
      << "FLOW_UNITS           CFS\nFLOW_ROUTING         DYNWAVE\n"
      << "START_DATE           01/01/2026\nSTART_TIME           00:00:00\n"
      << "END_DATE             01/01/2026\nEND_TIME             00:30:00\n"
      << "ROUTING_STEP         5\nREPORT_STEP          00:05:00\n"
      << options_extra << "\n"
      << "[JUNCTIONS]\n;;Name Elev MaxDepth InitDepth SurDepth Aponded\n"
      << "J0     10.0 10 0.5 0 0\n"
      << "J1     9.0  10 0.5 0 0\n\n"
      << "[OUTFALLS]\n;;Name Elev Type StageData Gated\nOUT 7.0 FREE  NO\n\n"
      << "[CONDUITS]\n;;Name From To Length N Zin Zout Q0\n"
      << "C1 J0 J1  400 0.013 0 0 0\n"
      << "C2 J1 OUT 400 0.013 0 0 0\n\n"
      << "[XSECTIONS]\n;;Link Shape G1 G2 G3 G4\n"
      << "C1 CIRCULAR 1.5 0 0 0\nC2 CIRCULAR 1.5 0 0 0\n\n"
      << "[POLLUTANTS]\n"
      << ";;Name Units Crain Cgw Crdii Kdecay SnowOnly CoPollut CoFrac "
         "Cdwf Cinit\n"
      << "TSS    MG/L  0.0   0.0 0.0   0.0    NO       *        0.0    "
         "0.0  5.0\n\n";
    if (!iq_lines.empty())
        f << "[INITIAL_QUALITY]\n" << iq_lines << "\n";
    f << "[REPORT]\nINPUT NO\nNODES ALL\nLINKS ALL\n";
}

std::string slurp(const char* path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

class InitialQualityTest : public ::testing::Test {
protected:
    void SetUp() override {
        engine_ = swmm_engine_create();
        ASSERT_NE(engine_, nullptr);
    }
    void TearDown() override {
        if (engine_) { swmm_engine_destroy(engine_); engine_ = nullptr; }
    }
    int open_deck(const char* inp, const char* rpt, const char* out) {
        return swmm_engine_open(engine_, inp, rpt, out, nullptr);
    }
    static bool contains(const std::vector<std::string>& v,
                         const std::string& needle) {
        for (const auto& e : v)
            if (e.find(needle) != std::string::npos) return true;
        return false;
    }
    SWMM_Engine engine_ = nullptr;
};

// ---------------------------------------------------------------------------
TEST_F(InitialQualityTest, FourRowsParseAndResolve) {
    // WATER_AGE ON so the age row is live; heat left OFF so the temperature
    // row exercises warn-and-keep (stored, classified, warned, inert).
    write_deck("_iq_ok.inp",
               "NODE J1 TSS 12.5\n"
               "NODE J0 __WATER_AGE__ 6.0\n"
               "LINK C2 __TEMPERATURE__ 18.5\n"
               "LINK C2 TSS 0.05\n",
               "WATER_AGE            YES\n");
    ASSERT_EQ(open_deck("_iq_ok.inp", "_iq_ok.rpt", "_iq_ok.out"), SWMM_OK);

    const auto& ctx = as_cpp_engine(engine_).context();
    const auto& iq  = ctx.initial_quality;
    ASSERT_EQ(iq.count(), 4);

    EXPECT_EQ(iq.is_link[0], 0);
    EXPECT_EQ(iq.elem_idx[0], ctx.node_names.find("J1"));
    EXPECT_EQ(iq.kind[0], 0);                      // TSS = pollutant 0
    EXPECT_DOUBLE_EQ(iq.value[0], 12.5);

    EXPECT_EQ(iq.is_link[1], 0);
    EXPECT_EQ(iq.elem_idx[1], ctx.node_names.find("J0"));
    EXPECT_EQ(iq.kind[1], openswmm::InitialQualityData::kKindWaterAge);
    EXPECT_DOUBLE_EQ(iq.value[1], 6.0);

    EXPECT_EQ(iq.is_link[2], 1);
    EXPECT_EQ(iq.elem_idx[2], ctx.link_names.find("C2"));
    EXPECT_EQ(iq.kind[2], openswmm::InitialQualityData::kKindTemperature);
    EXPECT_DOUBLE_EQ(iq.value[2], 18.5);

    EXPECT_EQ(iq.is_link[3], 1);
    EXPECT_EQ(iq.elem_idx[3], ctx.link_names.find("C2"));
    EXPECT_EQ(iq.kind[3], 0);
    EXPECT_DOUBLE_EQ(iq.value[3], 0.05);

    // Temperature row with HEAT_TRANSPORT off: kept, but warned inert.
    EXPECT_TRUE(contains(ctx.warnings, "HEAT_TRANSPORT is OFF"));
    // Age row is live (WATER_AGE YES) — no inert-age warning.
    EXPECT_FALSE(contains(ctx.warnings, "WATER_AGE is OFF"));

    std::remove("_iq_ok.inp");
}

// ---------------------------------------------------------------------------
TEST_F(InitialQualityTest, InertAgeRowWarnsWhenAgeOff) {
    write_deck("_iq_ageoff.inp", "NODE J0 __WATER_AGE__ 6.0\n");
    ASSERT_EQ(open_deck("_iq_ageoff.inp", "_iq_ageoff.rpt", "_iq_ageoff.out"),
              SWMM_OK);
    const auto& ctx = as_cpp_engine(engine_).context();
    ASSERT_EQ(ctx.initial_quality.count(), 1);   // warn-and-keep, not dropped
    EXPECT_TRUE(contains(ctx.warnings, "WATER_AGE is OFF"));
    std::remove("_iq_ageoff.inp");
}

// ---------------------------------------------------------------------------
// Error matrix: each invalid deck must FAIL open with a message naming the
// section. A negative age is legal (D-NS1), pinned at the end.
TEST_F(InitialQualityTest, ErrorMatrix) {
    struct Case { const char* tag; const char* row; const char* needle; };
    const Case cases[] = {
        {"unode", "NODE NOPE TSS 1.0\n",       "unknown node 'NOPE'"},
        {"ulink", "LINK NOPE TSS 1.0\n",       "unknown link 'NOPE'"},
        {"ucons", "NODE J1 HOCL 1.0\n",        "unknown constituent 'HOCL'"},
        {"umsx",  "NODE J1 HOCL 1.0\n",        "[REACTION_QUALITY] NODE|LINK"},
        {"dup",   "NODE J1 TSS 1.0\nNODE J1 TSS 2.0\n", "duplicate row"},
        {"scope", "CELL J1 TSS 1.0\n",         "scope must be NODE or LINK"},
        {"badv",  "NODE J1 TSS abc\n",         "bad value 'abc'"},
        {"negp",  "NODE J1 TSS -1.0\n",        "negative value for pollutant"},
    };
    for (const auto& c : cases) {
        SWMM_Engine e = swmm_engine_create();
        ASSERT_NE(e, nullptr) << c.tag;
        std::string inp = std::string("_iq_err_") + c.tag + ".inp";
        std::string rpt = std::string("_iq_err_") + c.tag + ".rpt";
        write_deck(inp.c_str(), c.row);
        EXPECT_NE(swmm_engine_open(e, inp.c_str(), rpt.c_str(), nullptr,
                                   nullptr),
                  SWMM_OK) << c.tag;
        const auto& ctx = as_cpp_engine(e).context();
        EXPECT_TRUE(contains(ctx.errors, "[INITIAL_QUALITY]")) << c.tag;
        EXPECT_TRUE(contains(ctx.errors, c.needle)) << c.tag;
        swmm_engine_destroy(e);
        std::remove(inp.c_str());
    }

    // Negative AGE is legal (signed age, D-NS1) — open must succeed.
    write_deck("_iq_negage.inp", "NODE J0 __WATER_AGE__ -2.0\n",
               "WATER_AGE            YES\n");
    ASSERT_EQ(open_deck("_iq_negage.inp", "_iq_negage.rpt", nullptr), SWMM_OK);
    EXPECT_EQ(as_cpp_engine(engine_).context().initial_quality.count(), 1);
    std::remove("_iq_negage.inp");
}

// ---------------------------------------------------------------------------
TEST_F(InitialQualityTest, SaveRoundTrip) {
    write_deck("_iq_rt.inp",
               "NODE J1 TSS 12.5\n"
               "NODE J0 __WATER_AGE__ 6.0\n"
               "LINK C2 __TEMPERATURE__ 18.5\n"
               "LINK C2 TSS 0.05\n",
               "WATER_AGE            YES\n");
    ASSERT_EQ(open_deck("_iq_rt.inp", "_iq_rt.rpt", nullptr), SWMM_OK);
    ASSERT_EQ(swmm_model_write(engine_, "_iq_rt_saved.inp"), SWMM_OK);

    SWMM_Engine e2 = swmm_engine_create();
    ASSERT_NE(e2, nullptr);
    ASSERT_EQ(swmm_engine_open(e2, "_iq_rt_saved.inp", "_iq_rt2.rpt", nullptr,
                               nullptr),
              SWMM_OK);

    const auto& a = as_cpp_engine(engine_).context().initial_quality;
    const auto& b = as_cpp_engine(e2).context().initial_quality;
    ASSERT_EQ(b.count(), a.count());
    const auto& ctx_a = as_cpp_engine(engine_).context();
    const auto& ctx_b = as_cpp_engine(e2).context();
    for (int i = 0; i < a.count(); ++i) {
        auto u = static_cast<std::size_t>(i);
        EXPECT_EQ(b.is_link[u], a.is_link[u]) << i;
        EXPECT_EQ(b.kind[u], a.kind[u]) << i;
        EXPECT_DOUBLE_EQ(b.value[u], a.value[u]) << i;
        // Compare via resolved names so index permutation cannot hide drift.
        if (a.is_link[u]) {
            EXPECT_EQ(ctx_b.link_names.name_of(b.elem_idx[u]),
                      ctx_a.link_names.name_of(a.elem_idx[u])) << i;
        } else {
            EXPECT_EQ(ctx_b.node_names.name_of(b.elem_idx[u]),
                      ctx_a.node_names.name_of(a.elem_idx[u])) << i;
        }
        EXPECT_EQ(b.constituent[u], a.constituent[u]) << i;
    }
    swmm_engine_destroy(e2);
    std::remove("_iq_rt.inp");
    std::remove("_iq_rt_saved.inp");
}

// ---------------------------------------------------------------------------
// E-A1 contract: the section is stored but NOT consumed — identical results
// with and without it. E-A2 rewrites this gate into an override assertion.
TEST_F(InitialQualityTest, BehaviorNeutralThisRound) {
    write_deck("_iq_base.inp", "");
    write_deck("_iq_with.inp", "NODE J1 TSS 12.5\nLINK C2 TSS 0.05\n");

    auto run = [](const char* inp, const char* rpt, const char* out) {
        SWMM_Engine e = swmm_engine_create();
        if (!e) return false;
        bool ok = swmm_engine_open(e, inp, rpt, out, nullptr) == SWMM_OK &&
                  swmm_engine_initialize(e) == SWMM_OK &&
                  swmm_engine_start(e, 1) == SWMM_OK;
        if (!ok) {
            ADD_FAILURE() << "open/init/start failed for " << inp;
            for (const auto& msg : as_cpp_engine(e).context().errors)
                ADD_FAILURE() << "engine error: " << msg;
        }
        double elapsed = 1.0;
        while (ok && elapsed > 0.0) {
            if (swmm_engine_step(e, &elapsed) != SWMM_OK) {
                ADD_FAILURE() << "step failed at elapsed=" << elapsed;
                ok = false;
                break;
            }
        }
        if (ok) swmm_engine_end(e);
        swmm_engine_destroy(e);
        return ok;
    };
    ASSERT_TRUE(run("_iq_base.inp", "_iq_base.rpt", "_iq_base.out"));
    ASSERT_TRUE(run("_iq_with.inp", "_iq_with.rpt", "_iq_with.out"));

    const std::string base = slurp("_iq_base.out");
    const std::string with = slurp("_iq_with.out");
    ASSERT_FALSE(base.empty());
    EXPECT_EQ(base, with)
        << "E-A1 must not consume [INITIAL_QUALITY]; if E-A2 landed, "
           "rewrite this gate into the override assertion.";

    std::remove("_iq_base.inp");
    std::remove("_iq_with.inp");
}

}  // namespace
