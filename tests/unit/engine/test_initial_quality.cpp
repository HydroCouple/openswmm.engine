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

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_initial_quality.h>
#include <openswmm/engine/openswmm_model.h>

#include "core/SWMMEngine.hpp"
#include "transport/InitialQualitySeeds.hpp"
#include "data/InitialQualityData.hpp"

namespace {

openswmm::SWMMEngine& as_cpp_engine(SWMM_Engine engine) {
    return *static_cast<openswmm::SWMMEngine*>(engine);
}

/// Minimal wet two-junction deck with one pollutant. `iq_lines` lands in an
/// [INITIAL_QUALITY] section when non-empty; `options_extra` is appended to
/// [OPTIONS] verbatim; `extra_sections` lands before [REPORT] (sidecar
/// registration for the E-A3 reserved-species gates).
void write_deck(const char* path, const std::string& iq_lines,
                const std::string& options_extra = "",
                const std::string& extra_sections = "") {
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
      << "J1     9.0  10 0.5 0 0\n"
      << "J2     9.5  10 0   0 0\n"     // dry (InitDepth 0)
      << "J3     9.8  10 0   0 0\n\n"   // dry (InitDepth 0)
      << "[OUTFALLS]\n;;Name Elev Type StageData Gated\nOUT 7.0 FREE  NO\n\n"
      << "[STORAGE]\n"
      << ";;Name Elev MaxDepth InitDepth Shape     Coeff Expon Const\n"
      << "ST1    8.5  10       0.5       FUNCTIONAL 0    0     1000\n\n"
      << "[CONDUITS]\n;;Name From To Length N Zin Zout Q0\n"
      << "C1 J0 J1  400 0.013 0 0 0\n"
      << "C2 J1 OUT 400 0.013 0 0 0\n"
      << "C3 J2 J1  400 0.013 0 0 0\n"
      << "C4 J3 J2  400 0.013 0 0 0\n"   // both ends dry -> dry link
      << "C5 ST1 J1 400 0.013 0 0 0\n\n"
      << "[XSECTIONS]\n;;Link Shape G1 G2 G3 G4\n"
      << "C1 CIRCULAR 1.5 0 0 0\nC2 CIRCULAR 1.5 0 0 0\n"
      << "C3 CIRCULAR 1.5 0 0 0\nC4 CIRCULAR 1.5 0 0 0\n"
      << "C5 CIRCULAR 1.5 0 0 0\n\n"
      << "[POLLUTANTS]\n"
      << ";;Name Units Crain Cgw Crdii Kdecay SnowOnly CoPollut CoFrac "
         "Cdwf Cinit\n"
      << "TSS    MG/L  0.0   0.0 0.0   0.0    NO       *        0.0    "
         "0.0  5.0\n\n";
    if (!iq_lines.empty())
        f << "[INITIAL_QUALITY]\n" << iq_lines << "\n";
    f << extra_sections;
    f << "[REPORT]\nINPUT NO\nNODES ALL\nLINKS ALL\n";
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
// E-A2 — the quartet under LEGACY: wet override, dry set, untouched wet
// (Cinit), untouched dry (0). initQuality() runs inside initialize(), so no
// stepping is needed. Falsifier: with the override loop removed, J1 reads
// Cinit (5), J2/C4 read 0.
TEST_F(InitialQualityTest, OverridesApplyWetAndDry) {
    write_deck("_iq_ovr.inp",
               "NODE J1 TSS 12.5\n"      // wet node override
               "NODE J2 TSS 7.0\n"       // dry node set (D-IQ4: applies)
               "LINK C4 TSS 3.0\n");     // dry link set (both ends dry)
    ASSERT_EQ(open_deck("_iq_ovr.inp", "_iq_ovr.rpt", nullptr), SWMM_OK);
    ASSERT_EQ(swmm_engine_initialize(engine_), SWMM_OK);

    const auto& ctx = as_cpp_engine(engine_).context();
    const auto ni = [&](const char* n) {
        return static_cast<std::size_t>(ctx.node_names.find(n));
    };
    const auto li = [&](const char* n) {
        return static_cast<std::size_t>(ctx.link_names.find(n));
    };
    // np == 1, so conc[idx] == conc[element index].
    EXPECT_DOUBLE_EQ(ctx.nodes.conc[ni("J1")], 12.5);  // wet override
    EXPECT_DOUBLE_EQ(ctx.nodes.conc[ni("J2")], 7.0);   // dry set (D-IQ4)
    EXPECT_DOUBLE_EQ(ctx.nodes.conc[ni("J0")], 5.0);   // untouched wet: Cinit
    EXPECT_DOUBLE_EQ(ctx.nodes.conc[ni("J3")], 0.0);   // untouched dry: 0
    EXPECT_DOUBLE_EQ(ctx.links.conc[li("C4")], 3.0);   // dry link set
    EXPECT_DOUBLE_EQ(ctx.links.conc[li("C1")], 5.0);   // untouched wet link
    // conc_old mirrors conc (both seeds write the pair).
    EXPECT_DOUBLE_EQ(ctx.nodes.conc_old[ni("J1")], 12.5);
    EXPECT_DOUBLE_EQ(ctx.links.conc_old[li("C4")], 3.0);

    std::remove("_iq_ovr.inp");
}

// ---------------------------------------------------------------------------
// E-A2 — ARD and LARD both seed lazily FROM the initQuality()-written arrays
// (D-IQ6), so the override must be visible after the first step under each.
// Zero flow (Q0=0, no inflows) keeps concentrations put; tolerance is for
// one transport step of numerical noise.
TEST_F(InitialQualityTest, ArdAndLardPickUpOverrides) {
    struct Solver { const char* tag; const char* opt; };
    const Solver solvers[] = {
        {"ard",  "QUALITY_SOLVER       EULERIAN_ARD\n"},
        {"lard", "QUALITY_SOLVER       LAGRANGIAN\n"},
    };
    for (const auto& s : solvers) {
        std::string inp = std::string("_iq_") + s.tag + ".inp";
        std::string rpt = std::string("_iq_") + s.tag + ".rpt";
        std::string out = std::string("_iq_") + s.tag + ".out";
        write_deck(inp.c_str(),
                   "NODE ST1 TSS 12.5\n"   // storage node: holds mass
                   "LINK C1 TSS 9.0\n"     // wet conduit override
                   "LINK C4 TSS 3.0\n",    // dry conduit row: inert, no crash
                   s.opt);
        SWMM_Engine e = swmm_engine_create();
        ASSERT_NE(e, nullptr) << s.tag;
        ASSERT_EQ(swmm_engine_open(e, inp.c_str(), rpt.c_str(), out.c_str(),
                                   nullptr), SWMM_OK) << s.tag;
        ASSERT_EQ(swmm_engine_initialize(e), SWMM_OK) << s.tag;
        ASSERT_EQ(swmm_engine_start(e, 1), SWMM_OK) << s.tag;
        double elapsed = 1.0;
        ASSERT_EQ(swmm_engine_step(e, &elapsed), SWMM_OK) << s.tag;

        const auto& ctx = as_cpp_engine(e).context();
        const auto ust = static_cast<std::size_t>(ctx.node_names.find("ST1"));
        const auto uc1 = static_cast<std::size_t>(ctx.link_names.find("C1"));
        const auto uc4 = static_cast<std::size_t>(ctx.link_names.find("C4"));
        // One routing step of real exchange (the deck has a head gradient)
        // drifts conc by ~2e-4; the falsifier (override loop removed) reads
        // the Cinit 5.0 instead — 1e-2 separates them by 400x.
        EXPECT_NEAR(ctx.links.conc[uc1], 9.0, 1e-2)
            << s.tag << ": wet conduit override lost in first step";
        if (std::string(s.tag) == "ard") {
            // Junctions are zero-volume passthrough under ARD — only a node
            // WITH storage holds initial mass, so the node gate probes ST1.
            EXPECT_NEAR(ctx.nodes.conc[ust], 12.5, 1e-2)
                << "ard: storage-node override lost in first step";
        }
        // Dry conduit: LARD seeds no segment (v<=0 skip); ARD may hold the
        // seeded value in a zero-volume cell. Either way the value must be
        // finite and within [0, override] — no NaN, no garbage, no crash.
        EXPECT_TRUE(std::isfinite(ctx.links.conc[uc4])) << s.tag;
        EXPECT_GE(ctx.links.conc[uc4], 0.0) << s.tag;
        EXPECT_LE(ctx.links.conc[uc4], 3.0 + 1e-6) << s.tag;

        swmm_engine_end(e);
        swmm_engine_destroy(e);
        std::remove(inp.c_str());
    }
}

// ---------------------------------------------------------------------------
// E-A3 — __WATER_AGE__ rows override the sidecar INITIAL_STATE GLOBAL under
// every engine. Global 2 h, overridden elements 6 h; after one 5 s step the
// two populations sit ~4 h apart, so a 60 s tolerance separates them by
// 240x. Falsifier: with the seed-site wiring removed, every element reads
// the global 2 h.
TEST_F(InitialQualityTest, ReservedAgeRowsOverrideGlobalPerEngine) {
    {
        std::ofstream a("_iq_g2.age");
        a << "[WATER_AGE_SOURCES]\nINITIAL_STATE GLOBAL 2.0\n";
    }
    const std::string pc =
        "[PROCESS_COMPONENTS]\n"
        "org.hydrocouple.openswmm.waterage  config=\"_iq_g2.age\"\n\n";

    struct Solver { const char* tag; const char* opt; bool nodes; };
    const Solver solvers[] = {
        {"aleg",  "WATER_AGE            YES\n", true},
        {"aard",  "WATER_AGE            YES\n"
                  "QUALITY_SOLVER       EULERIAN_ARD\n", true},
        {"alard", "WATER_AGE            YES\n"
                  "QUALITY_SOLVER       LAGRANGIAN\n", false},
    };
    for (const auto& s : solvers) {
        std::string inp = std::string("_iq_") + s.tag + ".inp";
        std::string rpt = std::string("_iq_") + s.tag + ".rpt";
        write_deck(inp.c_str(),
                   "LINK C1  __WATER_AGE__ 6.0\n"
                   "NODE ST1 __WATER_AGE__ 6.0\n",
                   s.opt, pc);
        SWMM_Engine e = swmm_engine_create();
        ASSERT_NE(e, nullptr) << s.tag;
        ASSERT_EQ(swmm_engine_open(e, inp.c_str(), rpt.c_str(), nullptr,
                                   nullptr), SWMM_OK) << s.tag;
        ASSERT_EQ(swmm_engine_initialize(e), SWMM_OK) << s.tag;
        ASSERT_EQ(swmm_engine_start(e, 1), SWMM_OK) << s.tag;
        double elapsed = 1.0;
        ASSERT_EQ(swmm_engine_step(e, &elapsed), SWMM_OK) << s.tag;

        const auto& ctx = as_cpp_engine(e).context();
        const auto& ws  = ctx.water_age_state;
        const auto uc1 = static_cast<std::size_t>(ctx.link_names.find("C1"));
        const auto uc2 = static_cast<std::size_t>(ctx.link_names.find("C2"));
        EXPECT_NEAR(ws.link_age[uc1], 6.0 * 3600.0, 60.0)
            << s.tag << ": link age override lost";
        // Control: the untouched link must sit near the 2 h global, NOT the
        // 6 h override. ARD's first-step publish carries a ~10% volume-
        // bookkeeping sag on this near-static deck (same factor on every
        // species), so the control is a range, not a point.
        EXPECT_GT(ws.link_age[uc2], 1.0 * 3600.0)
            << s.tag << ": untouched link lost the global seed";
        EXPECT_LT(ws.link_age[uc2], 4.0 * 3600.0)
            << s.tag << ": untouched link picked up the override";
        if (s.nodes) {
            const auto ust =
                static_cast<std::size_t>(ctx.node_names.find("ST1"));
            EXPECT_NEAR(ws.node_age[ust], 6.0 * 3600.0, 60.0)
                << s.tag << ": storage-node age override lost";
        }
        swmm_engine_end(e);
        swmm_engine_destroy(e);
        std::remove(inp.c_str());
    }
    std::remove("_iq_g2.age");
}

// ---------------------------------------------------------------------------
// E-A3 — __TEMPERATURE__ rows override the sidecar INITIAL_STATE GLOBAL
// under LEGACY and ARD (LARD does not advance temperature; out of scope).
// All heat fluxes off, so one step leaves the fields essentially static.
TEST_F(InitialQualityTest, ReservedTempRowsOverrideGlobalPerEngine) {
    {
        std::ofstream h("_iq_h15.heat");
        h << "[HEAT_SOURCES]\nINITIAL_STATE GLOBAL 15.0\n\n"
          << "[HEAT_FLUXES]\nSURFACE_EXCHANGE OFF\nRADIATIVE_EXCHANGE OFF\n";
    }
    const std::string pc =
        "[PROCESS_COMPONENTS]\n"
        "org.hydrocouple.openswmm.heat  config=\"_iq_h15.heat\"\n\n";

    struct Solver { const char* tag; const char* opt; };
    const Solver solvers[] = {
        {"tleg", "HEAT_TRANSPORT       YES\n"},
        {"tard", "HEAT_TRANSPORT       YES\n"
                 "QUALITY_SOLVER       EULERIAN_ARD\n"},
    };
    for (const auto& s : solvers) {
        std::string inp = std::string("_iq_") + s.tag + ".inp";
        std::string rpt = std::string("_iq_") + s.tag + ".rpt";
        write_deck(inp.c_str(),
                   "LINK C1  __TEMPERATURE__ 18.5\n"
                   "NODE ST1 __TEMPERATURE__ 18.5\n",
                   s.opt, pc);
        SWMM_Engine e = swmm_engine_create();
        ASSERT_NE(e, nullptr) << s.tag;
        ASSERT_EQ(swmm_engine_open(e, inp.c_str(), rpt.c_str(), nullptr,
                                   nullptr), SWMM_OK) << s.tag;
        ASSERT_EQ(swmm_engine_initialize(e), SWMM_OK) << s.tag;
        ASSERT_EQ(swmm_engine_start(e, 1), SWMM_OK) << s.tag;
        double elapsed = 1.0;
        ASSERT_EQ(swmm_engine_step(e, &elapsed), SWMM_OK) << s.tag;

        const auto& ctx = as_cpp_engine(e).context();
        const auto& hs  = ctx.heat_state;
        const auto uc1 = static_cast<std::size_t>(ctx.link_names.find("C1"));
        const auto uc2 = static_cast<std::size_t>(ctx.link_names.find("C2"));
        const auto ust = static_cast<std::size_t>(ctx.node_names.find("ST1"));
        EXPECT_NEAR(hs.link_temp[uc1], 18.5, 0.5)
            << s.tag << ": link temperature override lost";
        // Range control — see the age gate's note on ARD's first-step
        // publish sag.
        EXPECT_GT(hs.link_temp[uc2], 12.0)
            << s.tag << ": untouched link lost the global seed";
        EXPECT_LT(hs.link_temp[uc2], 16.5)
            << s.tag << ": untouched link picked up the override";
        EXPECT_NEAR(hs.node_temp[ust], 18.5, 0.5)
            << s.tag << ": storage-node temperature override lost";
        swmm_engine_end(e);
        swmm_engine_destroy(e);
        std::remove(inp.c_str());
    }
    std::remove("_iq_h15.heat");
}

// ---------------------------------------------------------------------------
// E-A3 — D-IQ7 enforcement point: the age applier itself refuses to write
// over a hotstart-loaded state, so no call site can get the precedence
// wrong. Probed at the helper level.
TEST_F(InitialQualityTest, HotstartWinsInsideTheAgeApplier) {
    write_deck("_iq_hs.inp", "NODE J0 __WATER_AGE__ 6.0\n",
               "WATER_AGE            YES\n");
    ASSERT_EQ(open_deck("_iq_hs.inp", "_iq_hs.rpt", nullptr), SWMM_OK);
    auto& ctx = as_cpp_engine(engine_).context();
    auto& ws = ctx.water_age_state;
    ws.resize(ctx.n_nodes(), ctx.n_links(), ctx.n_subcatches());
    const auto uj0 = static_cast<std::size_t>(ctx.node_names.find("J0"));

    std::fill(ws.node_age.begin(), ws.node_age.end(), 999.0);
    ws.hotstart_loaded = true;
    openswmm::transport::applyInitialAgeOverrides(ctx);
    EXPECT_DOUBLE_EQ(ws.node_age[uj0], 999.0)
        << "hotstart must win over [INITIAL_QUALITY] (D-IQ7)";

    ws.hotstart_loaded = false;
    openswmm::transport::applyInitialAgeOverrides(ctx);
    EXPECT_DOUBLE_EQ(ws.node_age[uj0], 6.0 * 3600.0)
        << "without hotstart the row must apply";

    std::remove("_iq_hs.inp");
}

// ---------------------------------------------------------------------------
// E-A2 — mass-balance falsifier for D-IQ4: "Initial Stored Mass" moves by
// exactly ΔC·V of the overridden WET link (dry elements carry no volume, so
// their overrides add nothing).
TEST_F(InitialQualityTest, InitialStoredMassDelta) {
    write_deck("_iq_m0.inp", "");
    write_deck("_iq_m1.inp",
               "LINK C1 TSS 12.5\n"
               "NODE J2 TSS 7.0\n");     // dry: must contribute 0 mass

    auto init_stored = [](const char* inp, const char* rpt,
                          double* c1_vol) -> double {
        SWMM_Engine e = swmm_engine_create();
        if (!e) return -1.0;
        double m = -1.0;
        if (swmm_engine_open(e, inp, rpt, nullptr, nullptr) == SWMM_OK &&
            swmm_engine_initialize(e) == SWMM_OK &&
            swmm_engine_start(e, 1) == SWMM_OK) {
            const auto& ctx = as_cpp_engine(e).context();
            if (!ctx.mass_balance.qual_routing_init.empty())
                m = ctx.mass_balance.qual_routing_init[0];
            if (c1_vol) {
                const int c1 = ctx.link_names.find("C1");
                if (c1 >= 0) *c1_vol =
                    ctx.links.volume[static_cast<std::size_t>(c1)];
            }
        }
        swmm_engine_destroy(e);
        return m;
    };

    double vol_c1 = 0.0;
    const double m_base = init_stored("_iq_m0.inp", "_iq_m0.rpt", nullptr);
    const double m_ovr  = init_stored("_iq_m1.inp", "_iq_m1.rpt", &vol_c1);
    ASSERT_GE(m_base, 0.0);
    ASSERT_GE(m_ovr, 0.0);
    ASSERT_GT(vol_c1, 0.0) << "C1 must hold water for this gate to bite";
    EXPECT_NEAR(m_ovr - m_base, (12.5 - 5.0) * vol_c1,
                1e-9 * std::max(1.0, m_ovr))
        << "delta must be exactly the wet link's dC*V; the dry node row "
           "must contribute nothing";

    std::remove("_iq_m0.inp");
    std::remove("_iq_m1.inp");
}

// ---------------------------------------------------------------------------
// E-A4 — the row C API: set x3 / count / get / upsert-in-place / remove-
// shifts. Falsifier before the round: the symbols do not exist (link
// failure); after: each contract assertion.
TEST_F(InitialQualityTest, ApiRoundTripUpsertRemove) {
    write_deck("_iq_api.inp", "", "WATER_AGE            YES\n");
    ASSERT_EQ(open_deck("_iq_api.inp", "_iq_api.rpt", nullptr), SWMM_OK);
    const auto& ctx = as_cpp_engine(engine_).context();
    const int j0 = ctx.node_names.find("J0");
    const int j1 = ctx.node_names.find("J1");
    const int c1 = ctx.link_names.find("C1");

    EXPECT_EQ(swmm_init_quality_count(engine_), 0);
    ASSERT_EQ(swmm_init_quality_set(engine_, 0, j1, "TSS", 12.5), SWMM_OK);
    ASSERT_EQ(swmm_init_quality_set(engine_, 1, c1, "TSS", 9.0), SWMM_OK);
    // Signed age is legal (D-NS1).
    ASSERT_EQ(swmm_init_quality_set(engine_, 0, j0, "__WATER_AGE__", -2.0),
              SWMM_OK);
    EXPECT_EQ(swmm_init_quality_count(engine_), 3);

    int is_link = -1, ei = -1;
    char buf[64];
    double v = 0.0;
    ASSERT_EQ(swmm_init_quality_get(engine_, 0, &is_link, &ei, buf, 64, &v),
              SWMM_OK);
    EXPECT_EQ(is_link, 0);
    EXPECT_EQ(ei, j1);
    EXPECT_STREQ(buf, "TSS");
    EXPECT_DOUBLE_EQ(v, 12.5);

    // Upsert: the same key changes the value without growing the count.
    ASSERT_EQ(swmm_init_quality_set(engine_, 0, j1, "TSS", 6.25), SWMM_OK);
    EXPECT_EQ(swmm_init_quality_count(engine_), 3);
    ASSERT_EQ(swmm_init_quality_get(engine_, 0, &is_link, &ei, buf, 64, &v),
              SWMM_OK);
    EXPECT_DOUBLE_EQ(v, 6.25);

    // Rejections: unknown constituent, negative pollutant, bad element.
    EXPECT_EQ(swmm_init_quality_set(engine_, 0, j1, "NOPE", 1.0),
              SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_init_quality_set(engine_, 0, j1, "TSS", -1.0),
              SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_init_quality_set(engine_, 0, 9999, "TSS", 1.0),
              SWMM_ERR_BADINDEX);

    // Remove shifts subsequent entries down.
    ASSERT_EQ(swmm_init_quality_remove(engine_, 0), SWMM_OK);
    EXPECT_EQ(swmm_init_quality_count(engine_), 2);
    ASSERT_EQ(swmm_init_quality_get(engine_, 0, &is_link, &ei, buf, 64, &v),
              SWMM_OK);
    EXPECT_EQ(is_link, 1);
    EXPECT_EQ(ei, c1);

    std::remove("_iq_api.inp");
}

// ---------------------------------------------------------------------------
// E-A4 — the API and the parser feed the SAME store (anti-drift): a row set
// through the API survives swmm_model_write -> reopen and then actually
// seeds state. Mutation after start is refused (the set_init_conc
// lifecycle contract).
TEST_F(InitialQualityTest, ApiSaveRoundTripAndLifecycleGuard) {
    write_deck("_iq_api2.inp", "");
    ASSERT_EQ(open_deck("_iq_api2.inp", "_iq_api2.rpt", nullptr), SWMM_OK);
    const auto& ctx = as_cpp_engine(engine_).context();
    const int j1 = ctx.node_names.find("J1");
    ASSERT_EQ(swmm_init_quality_set(engine_, 0, j1, "TSS", 12.5), SWMM_OK);

    ASSERT_EQ(swmm_model_write(engine_, "_iq_api2_saved.inp"), SWMM_OK);
    SWMM_Engine e2 = swmm_engine_create();
    ASSERT_NE(e2, nullptr);
    ASSERT_EQ(swmm_engine_open(e2, "_iq_api2_saved.inp", "_iq_api3.rpt",
                               nullptr, nullptr),
              SWMM_OK);
    EXPECT_EQ(swmm_init_quality_count(e2), 1);
    ASSERT_EQ(swmm_engine_initialize(e2), SWMM_OK);
    const auto& ctx2 = as_cpp_engine(e2).context();
    EXPECT_DOUBLE_EQ(
        ctx2.nodes.conc[static_cast<std::size_t>(ctx2.node_names.find("J1"))],
        12.5)
        << "the API-set row must seed state after the save round-trip";
    swmm_engine_destroy(e2);

    ASSERT_EQ(swmm_engine_initialize(engine_), SWMM_OK);
    ASSERT_EQ(swmm_engine_start(engine_, 1), SWMM_OK);
    EXPECT_EQ(swmm_init_quality_set(engine_, 0, j1, "TSS", 1.0),
              SWMM_ERR_LIFECYCLE);
    EXPECT_EQ(swmm_init_quality_remove(engine_, 0), SWMM_ERR_LIFECYCLE);
    swmm_engine_end(engine_);

    std::remove("_iq_api2.inp");
    std::remove("_iq_api2_saved.inp");
}

}  // namespace
