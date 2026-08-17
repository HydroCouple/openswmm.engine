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
 * @file test_output_quality.cpp
 * @brief Pollutant concentrations reach the binary .out file.
 *
 * @details `SimulationSnapshot::{node,link,subcatch}_quality` are what the
 *          output plugins write into their pollutant columns — and NOTHING
 *          populated them. Both readers guard with `qi < size()` and fall
 *          back to 0.0, so every pollutant column in every .out was written
 *          as ZERO while the header advertised the column count and unit
 *          codes. The engine state was correct throughout (nodes.conc /
 *          links.conc carry the routed values), which is exactly why every
 *          transport gate in the suite — all of which read those arrays
 *          DIRECTLY — passed over it. Found while scoping A2b, whose age
 *          column would have ridden the same dead pipeline.
 *
 *          These gates read back through the PUBLIC output reader, the
 *          surface a user, the GUI, and `analysis_*` actually see. That is
 *          the point: a gate that reads ctx arrays cannot observe this
 *          defect at all.
 *
 *          Observation paths:
 *          - NodeAndLinkConcentrationsReachTheOutFile: a deck whose TSS is
 *            pinned at a known value by Cinit on a stagnant network must
 *            read back that value, not 0. Falsified by removing either
 *            population block.
 *          - IgnoreQualityWritesNoPollutantColumns: the writer sets
 *            n_polluts_ = 0 under IGNORE_QUALITY; the vectors stay empty
 *            and the file must report zero pollutants (the bypass leg).
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include <gtest/gtest.h>

#include <cmath>
#include <fstream>
#include <string>
#include <vector>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_output.h>

namespace {

constexpr double kCinit = 42.0;  ///< distinctive: not 0, not 1, not 10

/// Level pool (zero flow, wet junctions): every element holds Cinit for the
/// whole run, so the EXPECTED reported value is exactly kCinit — no
/// transport physics enters the assertion. `extra_options` appends to
/// [OPTIONS].
void write_file(const char* path, const std::string& body) {
    std::ofstream c(path);
    c << body;
}

void write_deck(const char* path, const std::string& extra_options = "",
                const std::string& pc_lines = "", bool dry = false) {
    std::ofstream f(path);
    f << "[TITLE]\nsnapshot quality gate deck\n\n[OPTIONS]\n"
      << "FLOW_UNITS CFS\nFLOW_ROUTING DYNWAVE\n"
      << "START_DATE 01/01/2026\nSTART_TIME 00:00:00\n"
      << "END_DATE 01/01/2026\nEND_TIME 00:10:00\n"
      << "ROUTING_STEP 5\nREPORT_STEP 00:01:00\n"
      << extra_options << "\n"
      << "[JUNCTIONS]\n"
      // `dry` = InitDepth 0 and a FREE outfall: nothing anywhere holds
      // water, which is the configuration that exposed dry-element age
      // reporting (links published 6.000000 h on water that never existed).
      << (dry ? "J0 10.0 10 0 0 0\nJ1 10.0 10 0 0 0\nJ2 10.0 10 0 0 0\n\n"
              : "J0 10.0 10 1.5 0 0\nJ1 10.0 10 1.5 0 0\nJ2 10.0 10 1.5 0 0\n\n")
      << (dry ? "[OUTFALLS]\nOUT 10.0 FREE  NO\n\n"
              : "[OUTFALLS]\nOUT 10.0 FIXED 11.5 NO\n\n")
      << "[CONDUITS]\n"
      << "C1 J0 J1 500 0.013 0 0 0\nC2 J1 J2 500 0.013 0 0 0\n"
      << "C3 J2 OUT 500 0.013 0 0 0\n\n"
      << "[XSECTIONS]\n"
      << "C1 CIRCULAR 2.0 0 0 0\nC2 CIRCULAR 2.0 0 0 0\n"
      << "C3 CIRCULAR 2.0 0 0 0\n\n"
      << "[POLLUTANTS]\n"
      << ";;Name Units Crain Cgw Crdii Kdecay SnowOnly CoPollut CoFrac "
         "Cdwf Cinit\n"
      << "TSS    MG/L  0     0   0     0      NO       *        0      0    "
      << kCinit << "\n\n";
    if (!pc_lines.empty())
        f << "[PROCESS_COMPONENTS]\n" << pc_lines << "\n\n";
    f << "[REPORT]\nINPUT NO\n";
}

/// The species IDs the .out header carries, read from the bytes.
///
/// Not gratuitous: `swmm_output_*` and `OutputReader` expose pollut_count()
/// but NO pollutant-ID accessor (only subcatch/node/link IDs are parsed), so
/// the header NAMES — the thing an age column must be told apart by, since
/// it shares MG_PER_L's unit code — are unreachable through the reader. A
/// gate that checks values by index alone passes happily while the header
/// names them in the opposite order.
///
/// Layout (DefaultOutputPlugin::writeHeader): 7 ints, then one
/// (int len, chars) ID per subcatch/node/link/species, then the unit codes.
std::vector<std::string> read_species_ids(const char* path) {
    std::vector<std::string> ids;
    std::ifstream f(path, std::ios::binary);
    int hdr[7] = {0};
    f.read(reinterpret_cast<char*>(hdr), sizeof(hdr));
    if (!f) return ids;
    const int n_ids = hdr[3] + hdr[4] + hdr[5] + hdr[6];
    for (int i = 0; i < n_ids && f; ++i) {
        int len = 0;
        f.read(reinterpret_cast<char*>(&len), sizeof(len));
        if (!f || len < 0) return ids;
        std::string s(static_cast<std::size_t>(len), '\0');
        if (len > 0) f.read(s.data(), len);
        if (i >= n_ids - hdr[6]) ids.push_back(s);
    }
    return ids;
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

// ---------------------------------------------------------------------------
// Gate 1 — node and link concentrations reach the .out file.
// ---------------------------------------------------------------------------
TEST(OutputQualityTest, NodeAndLinkConcentrationsReachTheOutFile) {
    write_deck("_oq.inp");
    ASSERT_TRUE(run_deck("_oq.inp", "_oq.rpt", "_oq.out"));

    SWMM_Output h = swmm_output_open("_oq.out");
    ASSERT_NE(h, nullptr);

    const int n_periods = swmm_output_get_period_count(h);
    ASSERT_GT(n_periods, 1) << "no reporting periods — deck defect";
    const int period = n_periods - 1;   // last period: fully settled

    // Node variable layout: depth, head, volume, latflow, inflow, overflow,
    // then one column per pollutant (DefaultOutputPlugin::writeHeader).
    {
        std::vector<float> v(32, -1.0f);
        int count = 0;
        ASSERT_EQ(swmm_output_get_node_attribute(h, 0, period, v.data(),
                                                 &count),
                  0);
        ASSERT_GE(count, 7) << "node record has no pollutant column — the "
                               "header advertised none";
        EXPECT_NEAR(v[6], static_cast<float>(kCinit), 1.0e-3f)
            << "node pollutant column reads " << v[6] << ", expected "
            << kCinit
            << " — the snapshot's node_quality never reaches the writer "
               "(every .out pollutant column was zero).";
    }

    // Link layout: flow, depth, velocity, volume, capacity, then pollutants.
    {
        std::vector<float> v(32, -1.0f);
        int count = 0;
        ASSERT_EQ(swmm_output_get_link_attribute(h, 0, period, v.data(),
                                                 &count),
                  0);
        ASSERT_GE(count, 6) << "link record has no pollutant column";
        EXPECT_NEAR(v[5], static_cast<float>(kCinit), 1.0e-3f)
            << "link pollutant column reads " << v[5] << ", expected "
            << kCinit << " — link_quality never reaches the writer.";
    }

    swmm_output_close(h);
}

// ---------------------------------------------------------------------------
// Gate 2 — IGNORE_QUALITY writes no pollutant columns (the bypass leg).
// ---------------------------------------------------------------------------
TEST(OutputQualityTest, IgnoreQualityWritesNoPollutantColumns) {
    write_deck("_oq_ig.inp", "IGNORE_QUALITY YES\n");
    ASSERT_TRUE(run_deck("_oq_ig.inp", "_oq_ig.rpt", "_oq_ig.out"));

    SWMM_Output h = swmm_output_open("_oq_ig.out");
    ASSERT_NE(h, nullptr);
    const int n_polluts = swmm_output_get_pollut_count(h);
    EXPECT_EQ(n_polluts, 0)
        << "IGNORE_QUALITY still advertised pollutant columns — the "
           "population block must stay gated on it (an ungated fill would "
           "write into columns the header says do not exist).";
    swmm_output_close(h);
}

// ---------------------------------------------------------------------------
// A2b — water age reports as a trailing pseudo-pollutant column, in HOURS.
// ---------------------------------------------------------------------------
TEST(OutputQualityTest, WaterAgeReportsAsATrailingColumnInHours) {
    // Level pool + WATER_AGE ON + INITIAL_STATE 2 h. Zero flow, so at the
    // last report the age is exactly 2 h + elapsed — an ANALYTIC target in
    // the reported unit, which is also the units razor: a seconds/hours slip
    // is a 3600x miss, and reading the pollutant column instead of the age
    // column returns 42.
    write_file("_oq_age.age", "[WATER_AGE_SOURCES]\nINITIAL_STATE GLOBAL 2.0\n");
    write_deck("_oq_age.inp",
               "WATER_AGE ON\nQUALITY_SOLVER EULERIAN_ARD\n",
               "org.hydrocouple.openswmm.waterage config=\"_oq_age.age\"");
    ASSERT_TRUE(run_deck("_oq_age.inp", "_oq_age.rpt", "_oq_age.out"));

    SWMM_Output h = swmm_output_open("_oq_age.out");
    ASSERT_NE(h, nullptr);

    // The header must advertise TWO species columns: TSS then __WATER_AGE__.
    ASSERT_EQ(swmm_output_get_pollut_count(h), 2)
        << "the age pseudo-column is missing from the header (the writer "
           "strided by n_pollutants() instead of the reported count)";

    // ...and must NAME them in the order the data is written. Checking the
    // values by index cannot see a names/data swap: reordering only the name
    // list leaves every value assertion below satisfied while every consumer
    // that keys on the name (the .out's only way to tell HOURS from a
    // concentration — both carry unit code 0) reads the wrong column.
    const auto ids = read_species_ids("_oq_age.out");
    ASSERT_EQ(ids.size(), 2u) << "header species ID block is malformed";
    EXPECT_EQ(ids[0], "TSS");
    EXPECT_EQ(ids[1], "__WATER_AGE__")
        << "the age column must be named LAST, matching where the snapshot "
           "writes it";

    const int n_periods = swmm_output_get_period_count(h);
    ASSERT_GT(n_periods, 1);
    const int period = n_periods - 1;

    std::vector<float> v(32, -1.0f);
    int count = 0;
    ASSERT_EQ(swmm_output_get_node_attribute(h, 0, period, v.data(), &count),
              0);
    ASSERT_GE(count, 8) << "node record lacks the second species column";

    // [6] = TSS (unchanged by age's presence — the stride razor), [7] = age.
    EXPECT_NEAR(v[6], static_cast<float>(kCinit), 1.0e-3f)
        << "the pollutant column moved or was overwritten when the age "
           "column was added (reported-vs-transport stride slip)";

    // END_TIME 00:10:00; the last report is at 10 min = 0.1667 h, so the
    // expected age is 2 h + ~0.1667 h. Band covers report-instant placement.
    EXPECT_GT(v[7], 2.0f)
        << "age column reads " << v[7]
        << " h — below the 2 h INITIAL_STATE, so it is not the age column "
           "(or the seeding never reached the report)";
    EXPECT_LT(v[7], 2.5f)
        << "age column reads " << v[7]
        << " h — a seconds-for-hours slip would read ~7210";
    swmm_output_close(h);
}

// ---------------------------------------------------------------------------
// The species-ID reader: the API that makes the age column identifiable.
//
// A2b gave the age column a CONCENTRATION unit code (the .out unit enum has
// no HOURS slot) and declared the NAME the only way to tell hours from
// mg/L — then no public entry point could read a species name. The writer
// had always emitted the list; the reader stopped after subcatch/node/link
// IDs. So the field the format decision rested on was unreachable, and
// A2b's own gate read columns by fixed INDEX and asserted no name, which is
// why reordering just the name list passed the whole suite.
//
// These two gates close both halves: the reader returns the names, and the
// ORDER is asserted so a header/data mismatch cannot pass.
// ---------------------------------------------------------------------------
TEST(OutputQualityTest, SpeciesIdsAreReadableAndOrdered) {
    write_file("_oq_id.age", "[WATER_AGE_SOURCES]\nINITIAL_STATE GLOBAL 2.0\n");
    write_deck("_oq_id.inp",
               "WATER_AGE ON\nQUALITY_SOLVER EULERIAN_ARD\n",
               "org.hydrocouple.openswmm.waterage config=\"_oq_id.age\"");
    ASSERT_TRUE(run_deck("_oq_id.inp", "_oq_id.rpt", "_oq_id.out"));

    SWMM_Output h = swmm_output_open("_oq_id.out");
    ASSERT_NE(h, nullptr);
    ASSERT_EQ(swmm_output_get_pollut_count(h), 2);

    // Names must be READABLE at all — nullptr here means the reader never
    // parsed the list the writer emits.
    const char* s0 = swmm_output_get_pollut_id(h, 0);
    const char* s1 = swmm_output_get_pollut_id(h, 1);
    ASSERT_NE(s0, nullptr) << "species 0 name unreadable";
    ASSERT_NE(s1, nullptr) << "species 1 name unreadable";

    // And in the ORDER the data columns use. This is the assertion A2b
    // lacked: swapping only the name list must fail here even though every
    // VALUE still reads correctly at its index.
    EXPECT_STREQ(s0, "TSS")
        << "species 0 is '" << s0
        << "' — the header's name order disagrees with the data column "
           "order, so a consumer keying on the name reads the wrong series.";
    EXPECT_STREQ(s1, "__WATER_AGE__")
        << "species 1 is '" << s1 << "', expected __WATER_AGE__";

    // Out-of-range indices return nullptr rather than reading past the end.
    EXPECT_EQ(swmm_output_get_pollut_id(h, 2), nullptr);
    EXPECT_EQ(swmm_output_get_pollut_id(h, -1), nullptr);
    swmm_output_close(h);
}

TEST(OutputQualityTest, SpeciesIdsReadableWithoutWaterAge) {
    // The reader change must not depend on age being on: a plain pollutant
    // deck must also return its name (this list was never parsed before, so
    // the no-age path is new coverage too, not a regression check).
    write_deck("_oq_id2.inp");
    ASSERT_TRUE(run_deck("_oq_id2.inp", "_oq_id2.rpt", "_oq_id2.out"));
    SWMM_Output h = swmm_output_open("_oq_id2.out");
    ASSERT_NE(h, nullptr);
    ASSERT_EQ(swmm_output_get_pollut_count(h), 1);
    const char* s0 = swmm_output_get_pollut_id(h, 0);
    ASSERT_NE(s0, nullptr);
    EXPECT_STREQ(s0, "TSS");
    swmm_output_close(h);
}

// ---------------------------------------------------------------------------
// A dry element reports NO age (A2b carry c).
//
// The state must keep aging — a refilling pipe would otherwise jump, and a
// hotstart would lose the age it exists to restore — so the mask lives at
// the report boundary, keyed on the element's own reported depth. Validation
// of A2b observed links publishing exactly 6.000000 h of age on water that
// never existed; this is that observation as an assertion.
// ---------------------------------------------------------------------------
TEST(OutputQualityTest, DryElementsReportNoAge) {
    // INITIAL_STATE 6 h is the discriminator: unmasked, a dry link would
    // report ~6.167 h after the 10-minute run (6 h seed + elapsed). Masked,
    // it reports exactly 0.
    write_file("_oq_dry.age", "[WATER_AGE_SOURCES]\nINITIAL_STATE GLOBAL 6.0\n");
    write_deck("_oq_dry.inp",
               "WATER_AGE ON\nQUALITY_SOLVER EULERIAN_ARD\n",
               "org.hydrocouple.openswmm.waterage config=\"_oq_dry.age\"",
               /*dry=*/true);
    ASSERT_TRUE(run_deck("_oq_dry.inp", "_oq_dry.rpt", "_oq_dry.out"));

    SWMM_Output h = swmm_output_open("_oq_dry.out");
    ASSERT_NE(h, nullptr);
    ASSERT_EQ(swmm_output_get_pollut_count(h), 2);
    const int period = swmm_output_get_period_count(h) - 1;
    ASSERT_GE(period, 0);

    // Every link on a bone-dry network: depth ~0, so age must read 0.
    for (int l = 0; l < 3; ++l) {
        std::vector<float> v(32, -1.0f);
        int count = 0;
        ASSERT_EQ(swmm_output_get_link_attribute(h, l, period, v.data(),
                                                 &count),
                  0);
        ASSERT_GE(count, 7);
        // Liveness: the deck must actually BE dry, or the gate proves
        // nothing about the mask.
        //
        // Asserted on VOLUME, not depth. A dry conduit does not report
        // depth 0 — the dynamic-wave router floors it at FUDGE (1e-4 ft) —
        // so a depth-based liveness test is unsatisfiable and a depth-based
        // MASK never fires. Volume is the field that actually goes to ~0:
        // 0.0107 ft3 here against 1263.5 ft3 on the wet deck. The bound is
        // quality::ZERO_VOLUME (1 litre), the same constant the mask uses.
        EXPECT_NEAR(v[1], 1.0e-4f, 1.0e-5f)
            << "link " << l << " reports depth " << v[1]
            << " — expected the router's dry floor; if this moved, the "
               "mask's premise needs rechecking";
        ASSERT_LT(v[3], 0.0353147f)
            << "link " << l << " holds " << v[3]
            << " ft3 — the deck is not dry, so this gate cannot observe "
               "the mask";
        EXPECT_FLOAT_EQ(v[6], 0.0f)
            << "link " << l << " reports age " << v[6]
            << " h on an element holding no water (unmasked would be ~6.167)";
    }

    // ...and every NODE. The node mask is a separate branch from the link
    // mask and needs its own observation: with only the loop above, removing
    // the node guard entirely leaves this gate green.
    for (int n = 0; n < 4; ++n) {
        std::vector<float> v(32, -1.0f);
        int count = 0;
        ASSERT_EQ(swmm_output_get_node_attribute(h, n, period, v.data(),
                                                 &count),
                  0);
        ASSERT_GE(count, 8);
        ASSERT_FLOAT_EQ(v[0], 0.0f)
            << "node " << n << " reports depth " << v[0]
            << " — not dry, so this leg cannot observe the mask";
        EXPECT_FLOAT_EQ(v[7], 0.0f)
            << "node " << n << " reports age " << v[7]
            << " h while holding no water (unmasked would be ~6.167)";
    }
    swmm_output_close(h);
}

TEST(OutputQualityTest, WetElementsStillReportAgeAfterTheMask) {
    // The mask must not over-apply: the wet level pool still reports its age
    // (this is WaterAgeReportsAsATrailingColumnInHours' deck, asserted here
    // as the mask's negative control).
    write_file("_oq_wet.age", "[WATER_AGE_SOURCES]\nINITIAL_STATE GLOBAL 2.0\n");
    write_deck("_oq_wet.inp",
               "WATER_AGE ON\nQUALITY_SOLVER EULERIAN_ARD\n",
               "org.hydrocouple.openswmm.waterage config=\"_oq_wet.age\"");
    ASSERT_TRUE(run_deck("_oq_wet.inp", "_oq_wet.rpt", "_oq_wet.out"));
    SWMM_Output h = swmm_output_open("_oq_wet.out");
    ASSERT_NE(h, nullptr);
    const int period = swmm_output_get_period_count(h) - 1;
    std::vector<float> v(32, -1.0f);
    int count = 0;
    ASSERT_EQ(swmm_output_get_node_attribute(h, 0, period, v.data(), &count), 0);
    ASSERT_GE(count, 8);
    EXPECT_GT(v[7], 2.0f)
        << "the wet deck's node age was masked to " << v[7]
        << " — the mask is over-applying (junction reported VOLUME is 0 by "
           "legacy convention, which is why the mask keys on DEPTH)";

    // A LINK full of standing water, too. This deck's conduits hold 1263 ft3
    // at zero flow, so they are the only case that exercises the link
    // predicate's HOLDS leg on its own — without this, a flow-only predicate
    // blanks the age of every stagnant pipe in the model and no gate notices.
    ASSERT_EQ(swmm_output_get_link_attribute(h, 0, period, v.data(), &count), 0);
    ASSERT_GE(count, 7);
    ASSERT_GT(v[3], 1.0f) << "link 0 holds " << v[3]
                          << " ft3 — expected standing water";
    ASSERT_NEAR(v[0], 0.0f, 1.0e-6f)
        << "link 0 carries flow " << v[0]
        << " — this leg must test HOLDS with no help from CONVEYS";
    EXPECT_GT(v[6], 2.0f)
        << "a full but motionless conduit reported age " << v[6]
        << " — standing water has an age";
    swmm_output_close(h);
}

// ---------------------------------------------------------------------------
// A link that STORES nothing but CONVEYS water keeps its age.
//
// The second half of the mask's trap, with the fields exchanged. Nodes had
// to key on depth because a junction's reported volume is 0 by convention;
// links have to key on volume because the router floors a dry conduit's
// depth at FUDGE. But a pump stores nothing at all — volume 0 AND depth 0 —
// while carrying full flow, and its link_age is its upstream node's age: a
// real number. A volume-only test (or any depth-only test) blanks the age of
// every pump, orifice and weir in the model.
//
// So the predicate is holds-water OR conveys-water, and both legs are
// load-bearing: this deck's pump has no volume, and the wet level-pool deck
// above has no flow.
// ---------------------------------------------------------------------------
TEST(OutputQualityTest, FlowingRegulatorsKeepTheirAge) {
    write_file("_oq_reg.age",
               "[WATER_AGE_SOURCES]\nINITIAL_STATE GLOBAL 0.0\n");
    {
        std::ofstream f("_oq_reg.inp");
        f << "[TITLE]\nregulator age gate\n\n[OPTIONS]\n"
          << "FLOW_UNITS CFS\nFLOW_ROUTING DYNWAVE\n"
          << "START_DATE 01/01/2026\nSTART_TIME 00:00:00\n"
          << "END_DATE 01/01/2026\nEND_TIME 02:00:00\n"
          << "ROUTING_STEP 5\nREPORT_STEP 00:05:00\n"
          << "WATER_AGE ON\nQUALITY_SOLVER EULERIAN_ARD\n\n"
          << "[JUNCTIONS]\nJ0 10.0 10 3.0 0 0\nJ1 9.0 10 3.0 0 0\n\n"
          << "[OUTFALLS]\nOUT 6.0 FREE NO\n\n"
          << "[CONDUITS]\nC1 J0 J1 300 0.013 0 0 0\n\n"
          << "[PUMPS]\nP1 J1 OUT PC1 ON 0 0\n\n"
          << "[XSECTIONS]\nC1 CIRCULAR 3.0 0 0 0\n\n"
          << "[CURVES]\nPC1 PUMP4 0 5\nPC1 10 5\n\n"
          << "[INFLOWS]\nJ0 FLOW \"\" FLOW 1.0 1.0 5.0\n\n"
          << "[PROCESS_COMPONENTS]\n"
          << "org.hydrocouple.openswmm.waterage config=\"_oq_reg.age\"\n\n"
          << "[REPORT]\nINPUT NO\n";
    }
    ASSERT_TRUE(run_deck("_oq_reg.inp", "_oq_reg.rpt", "_oq_reg.out"));

    SWMM_Output h = swmm_output_open("_oq_reg.out");
    ASSERT_NE(h, nullptr);
    ASSERT_EQ(swmm_output_get_pollut_count(h), 1);
    const int period = swmm_output_get_period_count(h) - 1;
    ASSERT_GE(period, 0);

    // Link 1 is the pump: flow, depth, velocity, volume, capacity, then age.
    std::vector<float> v(32, -1.0f);
    int count = 0;
    ASSERT_EQ(swmm_output_get_link_attribute(h, 1, period, v.data(), &count), 0);
    ASSERT_GE(count, 6);

    // Liveness, both halves: the pump really does store nothing, and it
    // really is running. Without these the gate could pass on a deck where
    // the pump happens to look like a conduit.
    ASSERT_LT(v[3], 1.0e-9f) << "pump reports volume " << v[3]
                             << " — expected a storage-free element";
    ASSERT_GT(v[0], 1.0f) << "pump is not running (flow " << v[0]
                          << "), so this gate cannot observe the mask";

    EXPECT_GT(v[5], 0.0f)
        << "the running pump's age was masked to 0 — a storage-free link that "
           "is conveying water has a real age (its upstream node's)";
    swmm_output_close(h);
}

}  // namespace
