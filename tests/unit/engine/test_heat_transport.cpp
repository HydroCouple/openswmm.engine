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
 * @file test_heat_transport.cpp
 * @brief Phase H1 gates — `__TEMPERATURE__` transport under LEGACY.
 *
 * @details H1 carries temperature as a conservative tracer: no fluxes, no
 *          decay, no growth. The gates therefore have to separate "the
 *          temperature moved and mixed correctly" from "a number appeared".
 *
 *          Observation paths:
 *          - TwoSourcesMixConservativelyAtANode: the plan's own verify
 *            criterion (heat plan §6 H1). TWO loader pathways deliver to
 *            one node at different temperatures and unequal flows; the
 *            answer is the FLOW-WEIGHTED mean. Chosen so that the correct
 *            value collides with nothing: 24 degC is not the unweighted
 *            mean (18), not the default source temperature (20), not the
 *            INITIAL_STATE (3) and not either inlet (30, 6). Every wrong
 *            arithmetic this gate is meant to catch produces a DIFFERENT
 *            number, which is the lesson-26 discipline about identical
 *            carriers.
 *          - SubZeroTemperaturesSurvive: the trap in copying the age
 *            mirror. Age clamps at 0 because negative age is meaningless;
 *            temperature must not, or every cold-weather model silently
 *            warms to freezing.
 *          - TemperatureReportsAsATrailingColumnByName: the column reaches
 *            the .out AND is identified by NAME, not by index (lesson 40 —
 *            A2b's stride razor was blind precisely because it trusted an
 *            index).
 *          - PollutantsAndAgeUnchangedWithHeatOn: the mirror writes only
 *            heat_state.
 *          - HeatOffChangesNothing / HeatUnderArdWarns: the bypass legs.
 *          - HeatSourcesConfigParsesAndDefers: the config surface.
 *
 * @see plans/transport/HEAT_TRANSPORT_PLAN.md §1, §3, §6 H1
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <string>
#include <vector>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_output.h>

#include "core/InpWriter.hpp"
#include "core/SWMMEngine.hpp"

namespace {

openswmm::SWMMEngine& as_cpp_engine(SWMM_Engine engine) {
    return *static_cast<openswmm::SWMMEngine*>(engine);
}

// The two-source arithmetic, chosen so no wrong answer lands on the right
// number (see the file comment).
constexpr double kQExt   = 6.0;   ///< cfs, [INFLOWS]  → EXTERNAL_INFLOW
constexpr double kQDwf   = 2.0;   ///< cfs, [DWF]      → DWF
constexpr double kTExt   = 30.0;  ///< degC
constexpr double kTDwf   = 6.0;   ///< degC
constexpr double kTInit  = 3.0;   ///< degC, INITIAL_STATE
/// (6*30 + 2*6) / 8 = 24. Unweighted would be 18; the default is 20.
constexpr double kTMixed = 24.0;

void write_file(const char* path, const std::string& body) {
    std::ofstream f(path);
    f << body;
}

/// Two-inflow chain: J0 takes both an [INFLOWS] flow and a [DWF] baseline,
/// then drains through a conduit to an outfall.
void write_deck(const char* path, const std::string& extra_options,
                const std::string& pc_lines, bool pollutants = false,
                double q_ext = kQExt, double q_dwf = kQDwf) {
    std::ofstream f(path);
    f << "[TITLE]\nH1 heat transport gate deck\n\n[OPTIONS]\n"
      << "FLOW_UNITS CFS\nFLOW_ROUTING DYNWAVE\n"
      << "START_DATE 01/01/2026\nSTART_TIME 00:00:00\n"
      << "END_DATE 01/01/2026\nEND_TIME 02:00:00\n"
      << "ROUTING_STEP 5\nREPORT_STEP 00:01:00\n"
      << extra_options << "\n"
      << "[JUNCTIONS]\n"
      << "J0 10.0 10 1.5 0 0\nJ1 9.0 10 1.5 0 0\n\n"
      << "[OUTFALLS]\nOUT 8.0 FREE  NO\n\n"
      << "[CONDUITS]\n"
      << "C1 J0 J1  500 0.013 0 0 0\nC2 J1 OUT 500 0.013 0 0 0\n\n"
      << "[XSECTIONS]\n"
      << "C1 CIRCULAR 2.0 0 0 0\nC2 CIRCULAR 2.0 0 0 0\n\n";
    if (q_ext > 0.0)
        f << "[INFLOWS]\nJ0 FLOW \"\" FLOW 1.0 1.0 " << q_ext << "\n\n";
    if (q_dwf > 0.0)
        f << "[DWF]\nJ0 FLOW " << q_dwf << "\n\n";
    if (pollutants)
        f << "[POLLUTANTS]\n"
          << ";;Name Units Crain Cgw Crdii Kdecay SnowOnly CoPollut CoFrac "
             "Cdwf Cinit\n"
          << "TSS    MG/L  0     0   0     0      NO       *        0      "
             "0    42\n\n";
    if (!pc_lines.empty())
        f << "[PROCESS_COMPONENTS]\n" << pc_lines << "\n\n";
    f << "[REPORT]\nINPUT NO\n";
}

/// The standard H1 source table: the two inlet temperatures plus a
/// distinguishable initial state.
std::string heat_cfg(double t_ext = kTExt, double t_dwf = kTDwf,
                     double t_init = kTInit) {
    return "[HEAT_SOURCES]\n"
           "EXTERNAL_INFLOW GLOBAL " + std::to_string(t_ext) + "\n"
           "DWF             GLOBAL " + std::to_string(t_dwf) + "\n"
           "INITIAL_STATE   GLOBAL " + std::to_string(t_init) + "\n";
}

bool run_deck(const char* inp, const char* rpt, const char* out) {
    SWMM_Engine e = swmm_engine_create();
    if (e == nullptr) return false;
    bool ok = swmm_engine_open(e, inp, rpt, out, nullptr) == SWMM_OK &&
              swmm_engine_initialize(e) == SWMM_OK &&
              swmm_engine_start(e, 1) == SWMM_OK;
    if (ok) {
        double elapsed = 0.0;
        int guard = 0;
        do {
            if (swmm_engine_step(e, &elapsed) != SWMM_OK) { ok = false; break; }
        } while (elapsed > 0.0 && ++guard < 20000);
        if (ok) ok = swmm_engine_end(e) == SWMM_OK;
    }
    swmm_engine_destroy(e);
    return ok;
}

/// Run to completion and hand back the live engine so a gate can read
/// internal state (the published temperatures, the node inflow that proves
/// the deck delivered). Caller destroys.
SWMM_Engine run_and_hold(const char* inp, const char* rpt, const char* out) {
    SWMM_Engine e = swmm_engine_create();
    if (e == nullptr) return nullptr;
    if (swmm_engine_open(e, inp, rpt, out, nullptr) != SWMM_OK ||
        swmm_engine_initialize(e) != SWMM_OK ||
        swmm_engine_start(e, 1) != SWMM_OK) {
        swmm_engine_destroy(e);
        return nullptr;
    }
    double elapsed = 0.0;
    int guard = 0;
    do {
        if (swmm_engine_step(e, &elapsed) != SWMM_OK) break;
    } while (elapsed > 0.0 && ++guard < 20000);
    swmm_engine_end(e);
    return e;
}

}  // namespace

// ---------------------------------------------------------------------------
// Gate 1 — the plan's verify criterion: conservative mixing of two sources.
// ---------------------------------------------------------------------------
TEST(HeatTransportTest, TwoSourcesMixConservativelyAtANode) {
    write_file("_ht_mix.heat", heat_cfg());
    write_deck("_ht_mix.inp", "HEAT_TRANSPORT ON\n",
               "org.hydrocouple.openswmm.heat config=\"_ht_mix.heat\"");
    SWMM_Engine e = run_and_hold("_ht_mix.inp", "_ht_mix.rpt", "_ht_mix.out");
    ASSERT_NE(e, nullptr);
    const auto& ctx = as_cpp_engine(e).context();

    // LIVENESS FIRST (lesson 36): a gate that reads 24 degC on a deck where
    // only one pathway ever fired would be measuring nothing. Both inflows
    // must be present, or the "mix" is a single source.
    ASSERT_GT(ctx.n_nodes(), 0);
    const double q_total = ctx.nodes.lat_flow.empty() ? 0.0
                                                      : ctx.nodes.lat_flow[0];
    EXPECT_NEAR(q_total, kQExt + kQDwf, 0.5)
        << "J0 receives " << q_total << " cfs, expected "
        << (kQExt + kQDwf)
        << " — one of the two pathways did not deliver, so this deck cannot "
           "observe mixing at all.";

    ASSERT_FALSE(ctx.heat_state.node_temp.empty())
        << "heat state never sized — HEAT_TRANSPORT did not reach the engine";

    const double t_node = ctx.heat_state.node_temp[0];
    EXPECT_NEAR(t_node, kTMixed, 0.5)
        << "J0 settles at " << t_node << " degC, expected the FLOW-WEIGHTED "
           "mean " << kTMixed << ". 18 would mean the weighting was dropped "
           "(plain average), 20 the default source temperature (the table "
           "never reached the loaders), 3 the initial state (no inflow was "
           "mixed in), 30 or 6 a single pathway winning outright.";

    // The downstream link and node carry it too — transport, not just a
    // node-local calculation.
    ASSERT_FALSE(ctx.heat_state.link_temp.empty());
    EXPECT_NEAR(ctx.heat_state.link_temp[0], kTMixed, 1.0)
        << "C1 reads " << ctx.heat_state.link_temp[0]
        << " degC — the mixed temperature did not propagate into the link.";
    swmm_engine_destroy(e);
}

// ---------------------------------------------------------------------------
// Gate 2 — sub-zero temperatures survive (the age-mirror copy trap).
// ---------------------------------------------------------------------------
TEST(HeatTransportTest, SubZeroTemperaturesSurvive) {
    // Single cold source; the network starts warm so reaching a negative
    // value also proves the inflow actually displaced the initial state.
    write_file("_ht_cold.heat",
               "[HEAT_SOURCES]\n"
               "EXTERNAL_INFLOW GLOBAL -5.0\n"
               "INITIAL_STATE   GLOBAL 10.0\n");
    write_deck("_ht_cold.inp", "HEAT_TRANSPORT ON\n",
               "org.hydrocouple.openswmm.heat config=\"_ht_cold.heat\"",
               false, kQExt, 0.0);
    SWMM_Engine e =
        run_and_hold("_ht_cold.inp", "_ht_cold.rpt", "_ht_cold.out");
    ASSERT_NE(e, nullptr);
    const auto& ctx = as_cpp_engine(e).context();
    ASSERT_FALSE(ctx.heat_state.node_temp.empty());

    const double t = ctx.heat_state.node_temp[0];
    EXPECT_LT(t, 0.0)
        << "J0 reads " << t << " degC on a -5 degC inflow. Exactly 0.0 means "
           "a max(value, 0) clamp was carried over from the water-age mirror, "
           "where it is correct because negative age is meaningless — here it "
           "silently warms every below-freezing model to the freezing point.";
    EXPECT_NEAR(t, -5.0, 1.0) << "cold inflow did not reach steady state";
    swmm_engine_destroy(e);
}

// ---------------------------------------------------------------------------
// Gate 3 — the column reaches the .out and is identified BY NAME.
// ---------------------------------------------------------------------------
TEST(HeatTransportTest, TemperatureReportsAsATrailingColumnByName) {
    write_file("_ht_rpt.heat", heat_cfg());
    write_file("_ht_rpt.age",
               "[WATER_AGE_SOURCES]\nINITIAL_STATE GLOBAL 0.0\n");
    write_deck("_ht_rpt.inp",
               "HEAT_TRANSPORT ON\nWATER_AGE ON\n",
               "org.hydrocouple.openswmm.heat config=\"_ht_rpt.heat\"\n"
               "org.hydrocouple.openswmm.waterage config=\"_ht_rpt.age\"",
               /*pollutants=*/true);
    ASSERT_TRUE(run_deck("_ht_rpt.inp", "_ht_rpt.rpt", "_ht_rpt.out"));

    SWMM_Output h = swmm_output_open("_ht_rpt.out");
    ASSERT_NE(h, nullptr);

    // Three species: TSS, then age, then temperature — asserted BY NAME.
    // A2b's razor failed because it read v[6]/v[7] by index and never
    // checked a name, so reordering the header alone left it green.
    ASSERT_EQ(swmm_output_get_pollut_count(h), 3);
    const char* s0 = swmm_output_get_pollut_id(h, 0);
    const char* s1 = swmm_output_get_pollut_id(h, 1);
    const char* s2 = swmm_output_get_pollut_id(h, 2);
    ASSERT_NE(s0, nullptr);
    ASSERT_NE(s1, nullptr);
    ASSERT_NE(s2, nullptr);
    EXPECT_STREQ(s0, "TSS");
    EXPECT_STREQ(s1, "__WATER_AGE__");
    EXPECT_STREQ(s2, "__TEMPERATURE__")
        << "species 2 is '" << s2
        << "' — temperature must be the TRAILING column so adding heat to an "
           "existing water-age model does not move the age column.";

    const int n_periods = swmm_output_get_period_count(h);
    ASSERT_GT(n_periods, 1);
    const int period = n_periods - 1;

    // Node layout: 6 fixed columns, then one per species in header order.
    std::vector<float> v(32, -1.0f);
    int count = 0;
    ASSERT_EQ(swmm_output_get_node_attribute(h, 0, period, v.data(), &count), 0);
    ASSERT_GE(count, 9) << "node record is missing a species column";

    // "The TSS column did not move" is a claim about THIS deck with and
    // without heat, not about any particular concentration. Pinning a literal
    // 42 here would be importing the level-pool value from A2b's deck into a
    // FLOWING one: both inflow pathways carry zero TSS, so Cinit is diluted
    // (to 32.465 as it happens) and the literal fails while nothing is wrong.
    // Read the reference from the same deck with heat off instead.
    write_deck("_ht_rpt_off.inp", "WATER_AGE ON\n",
               "org.hydrocouple.openswmm.waterage config=\"_ht_rpt.age\"",
               /*pollutants=*/true);
    ASSERT_TRUE(run_deck("_ht_rpt_off.inp", "_ht_rpt_off.rpt",
                         "_ht_rpt_off.out"));
    SWMM_Output h_off = swmm_output_open("_ht_rpt_off.out");
    ASSERT_NE(h_off, nullptr);
    ASSERT_EQ(swmm_output_get_pollut_count(h_off), 2);
    std::vector<float> vo(32, -1.0f);
    int count_off = 0;
    ASSERT_EQ(swmm_output_get_node_attribute(h_off, 0, period, vo.data(),
                                             &count_off), 0);
    ASSERT_GE(count_off, 8);
    EXPECT_FLOAT_EQ(v[6], vo[6])
        << "the TSS column moved when temperature was added (stride slip): "
        << v[6] << " with heat, " << vo[6] << " without";
    EXPECT_FLOAT_EQ(v[7], vo[7])
        << "the age column moved when temperature was added: " << v[7]
        << " with heat, " << vo[7] << " without";
    swmm_output_close(h_off);
    EXPECT_NEAR(v[8], static_cast<float>(kTMixed), 0.5f)
        << "temperature column reads " << v[8] << ", expected " << kTMixed;
    swmm_output_close(h);
}

// ---------------------------------------------------------------------------
// Gate 4 — HEAT_TRANSPORT ON leaves pollutants and age untouched.
// ---------------------------------------------------------------------------
TEST(HeatTransportTest, PollutantsAndAgeUnchangedWithHeatOn) {
    write_file("_ht_ref.age",
               "[WATER_AGE_SOURCES]\nINITIAL_STATE GLOBAL 2.0\n");
    write_file("_ht_ref.heat", heat_cfg());

    write_deck("_ht_ref.inp", "WATER_AGE ON\n",
               "org.hydrocouple.openswmm.waterage config=\"_ht_ref.age\"",
               true);
    SWMM_Engine a = run_and_hold("_ht_ref.inp", "_ht_ref.rpt", "_ht_ref.out");
    ASSERT_NE(a, nullptr);
    const auto age_ref  = as_cpp_engine(a).context().water_age_state.node_age;
    const auto conc_ref = as_cpp_engine(a).context().nodes.conc;
    swmm_engine_destroy(a);

    write_deck("_ht_both.inp", "WATER_AGE ON\nHEAT_TRANSPORT ON\n",
               "org.hydrocouple.openswmm.waterage config=\"_ht_ref.age\"\n"
               "org.hydrocouple.openswmm.heat config=\"_ht_ref.heat\"",
               true);
    SWMM_Engine b = run_and_hold("_ht_both.inp", "_ht_both.rpt", "_ht_both.out");
    ASSERT_NE(b, nullptr);
    const auto& ctxb = as_cpp_engine(b).context();

    ASSERT_EQ(age_ref.size(), ctxb.water_age_state.node_age.size());
    for (std::size_t i = 0; i < age_ref.size(); ++i)
        EXPECT_DOUBLE_EQ(age_ref[i], ctxb.water_age_state.node_age[i])
            << "node " << i << " age moved when HEAT_TRANSPORT was enabled — "
               "the heat mirror must write only heat_state";
    ASSERT_EQ(conc_ref.size(), ctxb.nodes.conc.size());
    for (std::size_t i = 0; i < conc_ref.size(); ++i)
        EXPECT_DOUBLE_EQ(conc_ref[i], ctxb.nodes.conc[i])
            << "pollutant slot " << i << " moved when heat was enabled";
    swmm_engine_destroy(b);
}

// ---------------------------------------------------------------------------
// Gate 5 — HEAT_TRANSPORT off: no species column, no state.
// ---------------------------------------------------------------------------
TEST(HeatTransportTest, HeatOffAddsNoColumnAndNoState) {
    write_deck("_ht_off.inp", "", "", /*pollutants=*/true);
    ASSERT_TRUE(run_deck("_ht_off.inp", "_ht_off.rpt", "_ht_off.out"));
    SWMM_Output h = swmm_output_open("_ht_off.out");
    ASSERT_NE(h, nullptr);
    EXPECT_EQ(swmm_output_get_pollut_count(h), 1)
        << "a heat column appeared with HEAT_TRANSPORT off";
    const char* s0 = swmm_output_get_pollut_id(h, 0);
    ASSERT_NE(s0, nullptr);
    EXPECT_STREQ(s0, "TSS");
    swmm_output_close(h);
}

// ---------------------------------------------------------------------------
// Gate 6 — H4 RETIRED this deferral: EULERIAN_ARD now transports temperature.
//
// H1 shipped a warning saying the ARD binding was owed to phase H4, and this
// gate asserted it. H4 lands the binding, so the gate INVERTS in the same
// changeset (lesson 21): the warning must be gone AND the feature must
// actually work under ARD. Asserting only the warning's absence would pass
// for a deck where nothing ran at all.
// ---------------------------------------------------------------------------
TEST(HeatTransportTest, ArdTransportsTemperatureAndTheH4WarningIsRetired) {
    write_file("_ht_ard.heat", heat_cfg());
    write_deck("_ht_ard.inp",
               "HEAT_TRANSPORT ON\nQUALITY_SOLVER EULERIAN_ARD\n",
               "org.hydrocouple.openswmm.heat config=\"_ht_ard.heat\"");
    SWMM_Engine e = run_and_hold("_ht_ard.inp", "_ht_ard.rpt", "_ht_ard.out");
    ASSERT_NE(e, nullptr);
    const auto& ctx = as_cpp_engine(e).context();

    const auto& w = ctx.warnings;
    const bool still_deferred = std::any_of(
        w.begin(), w.end(), [](const std::string& s) {
            return s.find("EULERIAN_ARD") != std::string::npos &&
                   s.find("H4") != std::string::npos;
        });
    EXPECT_FALSE(still_deferred)
        << "the H1 warning that ARD tracks no temperature is still emitted, "
           "but H4 implements the mesh binding — a retired deferral must "
           "lose its message in the changeset that retires it.";

    // And the feature works: the ARD mesh must publish into heat_state, the
    // same place the snapshot reads regardless of engine.
    ASSERT_FALSE(ctx.heat_state.node_temp.empty())
        << "heat_state was never sized under ARD — the row is not on the mesh";
    EXPECT_NEAR(ctx.heat_state.node_temp[0], kTMixed, 1.0)
        << "under EULERIAN_ARD J0 settles at " << ctx.heat_state.node_temp[0]
        << " degC; the flow-weighted mean of the two loader pathways is "
        << kTMixed << ". Reading the 3 degC INITIAL_STATE means the mesh row "
           "exists but no loader reached it; reading 20 means the config "
           "table never arrived.";
    swmm_engine_destroy(e);
}

// ---------------------------------------------------------------------------
// Gate 7 — the config surface: NODE override, deferrals, range refusal.
// ---------------------------------------------------------------------------
TEST(HeatTransportTest, HeatSourcesConfigParsesAndDefers) {
    // A NODE override must beat the GLOBAL row for that node/source.
    write_file("_ht_node.heat",
               "[HEAT_SOURCES]\n"
               "EXTERNAL_INFLOW GLOBAL 30.0\n"
               "EXTERNAL_INFLOW NODE J0 -2.0\n"
               "INITIAL_STATE   GLOBAL 3.0\n");
    write_deck("_ht_node.inp", "HEAT_TRANSPORT ON\n",
               "org.hydrocouple.openswmm.heat config=\"_ht_node.heat\"",
               false, kQExt, 0.0);
    SWMM_Engine e =
        run_and_hold("_ht_node.inp", "_ht_node.rpt", "_ht_node.out");
    ASSERT_NE(e, nullptr);
    const double t = as_cpp_engine(e).context().heat_state.node_temp[0];
    EXPECT_NEAR(t, -2.0, 1.0)
        << "J0 reads " << t << " degC — the NODE override lost to the GLOBAL "
           "row (30) or to the default (20).";
    swmm_engine_destroy(e);

    // A TIMESERIES row must refuse by NAME of the phase, and must be
    // reachable in its documented (longer) spelling — the A1a defect was
    // that an arity check ran first and reported "malformed" instead.
    write_file("_ht_ts.heat",
               "[HEAT_SOURCES]\n"
               "EXTERNAL_INFLOW NODE J0 TIMESERIES temp_ts\n");
    write_deck("_ht_ts.inp", "HEAT_TRANSPORT ON\n",
               "org.hydrocouple.openswmm.heat config=\"_ht_ts.heat\"");
    SWMM_Engine e2 = swmm_engine_create();
    ASSERT_NE(e2, nullptr);
    const int rc = swmm_engine_open(e2, "_ht_ts.inp", "_ht_ts.rpt",
                                    "_ht_ts.out", nullptr);
    EXPECT_NE(rc, SWMM_OK) << "a deferred TIMESERIES row opened silently";
    swmm_engine_destroy(e2);

    // An out-of-range temperature (a Fahrenheit value pasted into a degC
    // field) must refuse rather than route 68-degree-Celsius water.
    write_file("_ht_bad.heat",
               "[HEAT_SOURCES]\nEXTERNAL_INFLOW GLOBAL 451.0\n");
    write_deck("_ht_bad.inp", "HEAT_TRANSPORT ON\n",
               "org.hydrocouple.openswmm.heat config=\"_ht_bad.heat\"");
    SWMM_Engine e3 = swmm_engine_create();
    ASSERT_NE(e3, nullptr);
    EXPECT_NE(swmm_engine_open(e3, "_ht_bad.inp", "_ht_bad.rpt", "_ht_bad.out",
                               nullptr),
              SWMM_OK)
        << "451 degC was accepted as a water temperature";
    swmm_engine_destroy(e3);
}

// ---------------------------------------------------------------------------
// Gate 8 — the default inlet temperature, with no component at all.
//
// Without this the default is UNOBSERVED: every other gate configures the
// table explicitly, so changing 20 degC to 0 would break nothing (lesson 39
// — distinguish "green" from "not looked at"). It also pins the claim that
// [OPTIONS] HEAT_TRANSPORT ON alone is a working configuration.
// ---------------------------------------------------------------------------
TEST(HeatTransportTest, DefaultsApplyWithoutAComponent) {
    write_deck("_ht_def.inp", "HEAT_TRANSPORT ON\n", "");
    SWMM_Engine e = run_and_hold("_ht_def.inp", "_ht_def.rpt", "_ht_def.out");
    ASSERT_NE(e, nullptr);
    const auto& ctx = as_cpp_engine(e).context();
    ASSERT_FALSE(ctx.heat_state.node_temp.empty())
        << "HEAT_TRANSPORT ON without a component tracked nothing — the "
           "option alone must be a working configuration";
    EXPECT_NEAR(ctx.heat_state.node_temp[0], 20.0, 1.0e-6)
        << "J0 reads " << ctx.heat_state.node_temp[0]
        << " degC with no [HEAT_SOURCES] — every source defaults to 20 degC, "
           "so an unconfigured model is isothermal at the default. 0.0 here "
           "means the table defaulted to zero, which is indistinguishable "
           "from a deck that means freezing.";
    swmm_engine_destroy(e);
}

// ---------------------------------------------------------------------------
// Gate 9 — saving the model keeps the key that selects this feature.
//
// A1a's defect verbatim: InpWriter dropped WATER_AGE, so a save-as reopened
// with the feature silently off. The observation path is the REOPENED
// options, not the text — a key spelled in a way the parser rejects would
// still pass a text check.
// ---------------------------------------------------------------------------
TEST(HeatTransportTest, SaveKeepsHeatTransport) {
    write_deck("_ht_rt.inp", "HEAT_TRANSPORT ON\n", "");
    SWMM_Engine e = swmm_engine_create();
    ASSERT_NE(e, nullptr);
    ASSERT_EQ(swmm_engine_open(e, "_ht_rt.inp", "_ht_rt.rpt", "_ht_rt.out",
                               nullptr),
              SWMM_OK);
    ASSERT_TRUE(as_cpp_engine(e).context().options.heat_transport)
        << "the source deck did not even parse HEAT_TRANSPORT ON";
    ASSERT_EQ(openswmm::inp_writer::writeInpFile(as_cpp_engine(e).context(),
                                                 "_ht_rt_saved.inp", nullptr),
              0);
    swmm_engine_destroy(e);

    SWMM_Engine e2 = swmm_engine_create();
    ASSERT_NE(e2, nullptr);
    ASSERT_EQ(swmm_engine_open(e2, "_ht_rt_saved.inp", "_ht_rt_saved.rpt",
                               "_ht_rt_saved.out", nullptr),
              SWMM_OK);
    EXPECT_TRUE(as_cpp_engine(e2).context().options.heat_transport)
        << "the saved model reopened with HEAT_TRANSPORT off — a save-as "
           "silently disabled the feature (the A1a defect).";
    swmm_engine_destroy(e2);
}
