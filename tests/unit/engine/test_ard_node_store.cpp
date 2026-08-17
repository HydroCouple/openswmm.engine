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
 * @file test_ard_node_store.cpp
 * @brief The ARD node store mixes before it discharges, at any ROUTING_STEP.
 *
 * @details The store used to apply the external load and the face inflows
 *          AFTER reading the donor concentration the outflow faces carry.
 *          That is a forward-Euler CSTR, and a junction's own volume is far
 *          smaller than what passes through it in a routing step, so it ran
 *          well past its stability bound and failed in both directions:
 *
 *          - The face debit could exceed the store, and `max(0.0, ...)` on
 *            volume swallowed the excess while the matching mass debit
 *            stood. A steady 5 cfs / 100 mg/L inflow published 70.6 mg/L at
 *            ROUTING_STEP 5, and 53.9 at 20.
 *          - Above ROUTING_STEP ~10 the oscillation diverged and the floor
 *            at zero clipped its negative half, MANUFACTURING mass: 7730
 *            mg/L out of a 100 mg/L inflow, compounding link by link.
 *
 *          Observation paths, each chosen so it cannot pass for the wrong
 *          reason:
 *          - SteadyInflowIsTransportedExactlyAtEveryRoutingStep: the deck
 *            has ONE steady source and NO decay, so the analytic answer is
 *            "every element equals the inflow concentration" independent of
 *            step size, mesh, and hydraulics. It is also checked against
 *            LEGACY on the same deck, so a shared misreading of the deck
 *            cannot hide behind it (the ARD-vs-LEGACY split is what
 *            identified the defect in the first place).
 *          - NoElementEverExceedsTheInflowConcentration: the maximum
 *            principle, sampled at EVERY step rather than at the end. Mass
 *            manufacture shows up here even when the final state has
 *            relaxed back, and a single element over the bound fails it.
 *          - StoreStaysBoundedWhenThroughputDwarfsStoreVolume: drives
 *            dt*q/V to ~40 deliberately. This is the regime the old code
 *            diverged in; the mixed donor is a weighted average, so it
 *            cannot.
 *
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

#include "core/SWMMEngine.hpp"

namespace {

openswmm::SWMMEngine& as_cpp_engine(SWMM_Engine engine) {
    return *static_cast<openswmm::SWMMEngine*>(engine);
}

constexpr double kQ = 5.0;      ///< steady inflow, cfs
constexpr double kCin = 100.0;  ///< steady inflow concentration, mg/L

/// Five 500 ft conduits in a chain, one steady flow + concentration source
/// at the head, no decay and no reactions: the exact answer everywhere is
/// kCin, at every ROUTING_STEP.
void write_deck(const char* path, int routing_step, bool ard) {
    std::ofstream f(path);
    f << "[TITLE]\nARD node store\n\n[OPTIONS]\n"
      << "FLOW_UNITS CFS\nFLOW_ROUTING DYNWAVE\n"
      << (ard ? "QUALITY_SOLVER EULERIAN_ARD\n" : "QUALITY_SOLVER LEGACY\n")
      << "START_DATE 01/01/2026\nSTART_TIME 00:00:00\n"
      << "END_DATE 01/01/2026\nEND_TIME 04:00:00\n"
      << "ROUTING_STEP " << routing_step << "\nREPORT_STEP 00:05:00\n\n"
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
      << "[POLLUTANTS]\nTSS MG/L 0 0 0 0 NO * 0 0 0\n\n"
      << "[INFLOWS]\n"
      << "J0 FLOW \"\" FLOW 1.0 1.0 " << kQ << "\n"
      << "J0 TSS  \"\" CONCEN 1.0 1.0 " << kCin << "\n\n"
      << "[REPORT]\nINPUT NO\n";
}

struct StoreRun {
    std::vector<double> link_final;  ///< [link] mg/L at the end
    double peak_link = 0.0;          ///< max over ALL links and ALL steps
    double peak_node = 0.0;
    bool ok = false;
};

StoreRun run_deck(const char* tag, int routing_step, bool ard) {
    StoreRun r;
    const std::string inp = std::string(tag) + ".inp";
    const std::string rpt = std::string(tag) + ".rpt";
    const std::string out = std::string(tag) + ".out";
    write_deck(inp.c_str(), routing_step, ard);

    SWMM_Engine e = swmm_engine_create();
    if (e == nullptr) { ADD_FAILURE() << "engine create"; return r; }
    if (swmm_engine_open(e, inp.c_str(), rpt.c_str(), out.c_str(), nullptr) !=
            SWMM_OK ||
        swmm_engine_initialize(e) != SWMM_OK ||
        swmm_engine_start(e, 1) != SWMM_OK) {
        ADD_FAILURE() << "open/init/start failed for " << inp;
        swmm_engine_destroy(e);
        return r;
    }
    auto& ctx = as_cpp_engine(e).context();
    const int np = ctx.n_pollutants();
    const int nl = ctx.n_links();
    const int nn = ctx.n_nodes();
    double elapsed = 0.0;
    int guard = 0;
    do {
        if (swmm_engine_step(e, &elapsed) != SWMM_OK) {
            ADD_FAILURE() << "step failed for " << inp;
            swmm_engine_destroy(e);
            return r;
        }
        // Sample the peak at EVERY step. Mass manufacture is a transient
        // that can relax away before the run ends.
        for (int l = 0; l < nl; ++l)
            r.peak_link = std::max(
                r.peak_link,
                ctx.links.conc[static_cast<std::size_t>(l * np)]);
        for (int j = 0; j < nn; ++j)
            r.peak_node = std::max(
                r.peak_node,
                ctx.nodes.conc[static_cast<std::size_t>(j * np)]);
    } while (elapsed > 0.0 && ++guard < 200000);
    swmm_engine_end(e);
    r.link_final.assign(static_cast<std::size_t>(nl), 0.0);
    for (int l = 0; l < nl; ++l)
        r.link_final[static_cast<std::size_t>(l)] =
            ctx.links.conc[static_cast<std::size_t>(l * np)];
    r.ok = true;
    swmm_engine_destroy(e);
    return r;
}

const int kSteps[] = {1, 2, 3, 4, 5, 10, 20, 30, 60, 120};

// ---------------------------------------------------------------------------
// Gate 1 — a steady source is transported exactly, at every ROUTING_STEP.
// ---------------------------------------------------------------------------
TEST(ArdNodeStoreTest, SteadyInflowIsTransportedExactlyAtEveryRoutingStep) {
    for (const int rs : kSteps) {
        const StoreRun ard = run_deck("_ns_ard", rs, /*ard=*/true);
        const StoreRun leg = run_deck("_ns_leg", rs, /*ard=*/false);
        ASSERT_TRUE(ard.ok) << "rs=" << rs;
        ASSERT_TRUE(leg.ok) << "rs=" << rs;
        ASSERT_EQ(ard.link_final.size(), 5u);

        for (std::size_t l = 0; l < ard.link_final.size(); ++l) {
            // The reference engine first: if LEGACY does not read kCin here
            // then the DECK is wrong, not the ARD store, and the assertion
            // below would be measuring the wrong thing.
            ASSERT_NEAR(leg.link_final[l], kCin, 1.0e-6)
                << "LEGACY link " << l << " at rs=" << rs
                << " — the deck's premise is broken, not the engine";
            EXPECT_NEAR(ard.link_final[l], kCin, 1.0e-6)
                << "ARD link " << l << " at ROUTING_STEP " << rs
                << ": a steady source with no decay must arrive intact. "
                   "Short means the store discharged before mixing in what "
                   "arrived (measured 70.594 at rs=5 before the fix).";
        }
    }
}

// ---------------------------------------------------------------------------
// Gate 2 — the maximum principle. Nothing may exceed the source.
// ---------------------------------------------------------------------------
TEST(ArdNodeStoreTest, NoElementEverExceedsTheInflowConcentration) {
    for (const int rs : kSteps) {
        const StoreRun ard = run_deck("_ns_max", rs, /*ard=*/true);
        ASSERT_TRUE(ard.ok) << "rs=" << rs;
        // A mixed store donates a weighted average of what it held and what
        // just arrived, so no element can exceed the single source feeding
        // the network. Exceeding it is mass created from nothing — measured
        // at 7730 mg/L (rs=20) and 502.95 (rs=10) before the fix.
        EXPECT_LE(ard.peak_link, kCin * (1.0 + 1.0e-9))
            << "a link exceeded the inflow concentration at ROUTING_STEP "
            << rs << " — mass was manufactured";
        EXPECT_LE(ard.peak_node, kCin * (1.0 + 1.0e-9))
            << "a node store exceeded the inflow concentration at "
               "ROUTING_STEP " << rs;
    }
}

// ---------------------------------------------------------------------------
// Gate 3 — the regime the old code diverged in, driven deliberately hard.
// ---------------------------------------------------------------------------
TEST(ArdNodeStoreTest, StoreStaysBoundedWhenThroughputDwarfsStoreVolume) {
    // A junction store on this deck holds ~16 ft3 and passes 5 cfs, so its
    // residence time is ~3.2 s. At ROUTING_STEP 120 the step is ~38 store
    // volumes: an explicit CSTR is unconditionally unstable here and the
    // old code diverged by orders of magnitude. Nothing about the mixed
    // donor cares how large the ratio gets.
    const StoreRun ard = run_deck("_ns_stiff", 120, /*ard=*/true);
    ASSERT_TRUE(ard.ok);
    for (std::size_t l = 0; l < ard.link_final.size(); ++l)
        EXPECT_NEAR(ard.link_final[l], kCin, 1.0e-6)
            << "link " << l << " at a throughput/store ratio near 40";
    EXPECT_LE(ard.peak_link, kCin * (1.0 + 1.0e-9));
}

}  // namespace
