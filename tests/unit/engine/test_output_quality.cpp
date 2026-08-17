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
void write_deck(const char* path, const std::string& extra_options = "") {
    std::ofstream f(path);
    f << "[TITLE]\nsnapshot quality gate deck\n\n[OPTIONS]\n"
      << "FLOW_UNITS CFS\nFLOW_ROUTING DYNWAVE\n"
      << "START_DATE 01/01/2026\nSTART_TIME 00:00:00\n"
      << "END_DATE 01/01/2026\nEND_TIME 00:10:00\n"
      << "ROUTING_STEP 5\nREPORT_STEP 00:01:00\n"
      << extra_options << "\n"
      << "[JUNCTIONS]\n"
      << "J0 10.0 10 1.5 0 0\nJ1 10.0 10 1.5 0 0\nJ2 10.0 10 1.5 0 0\n\n"
      << "[OUTFALLS]\nOUT 10.0 FIXED 11.5 NO\n\n"
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
      << kCinit << "\n\n"
      << "[REPORT]\nINPUT NO\n";
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

}  // namespace
