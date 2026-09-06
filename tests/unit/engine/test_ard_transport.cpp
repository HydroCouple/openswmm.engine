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
 * @file test_ard_transport.cpp
 * @brief Phase E1 gates for QUALITY_SOLVER EULERIAN_ARD
 *        (plans/transport/EULERIAN_ARD_TRANSPORT_PLAN.md §6, E1).
 *
 * @details Three engine-level gates on a 3-conduit line (J0→C1→J1→C2→J2→C3→OUT)
 *          under DYNWAVE hydraulics:
 *            1. Uniform-field preservation: constant inflow at concentration
 *               c0 with initial concentration c0 everywhere stays c0.
 *            2. Discrete maximum principle + front ordering: a clean system
 *               fed at c0 rises monotonically toward c0, never exceeds it,
 *               never goes negative, and downstream lags upstream.
 *            3. LEGACY selection regression: the same deck without
 *               QUALITY_SOLVER runs the legacy path and both engines agree
 *               at steady state (loose tolerance — different transport
 *               models, same steady limit).
 *
 *          Test artifacts (generated .inp/.rpt/.out) are written to the test
 *          working directory with the same underscore-prefix convention the
 *          other engine tests use (CLAUDE.md §4.1).
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include <gtest/gtest.h>

#include <cstdio>
#include <cmath>
#include <fstream>
#include <string>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_nodes.h>
#include <openswmm/engine/openswmm_massbalance.h>

#include "core/SWMMEngine.hpp"

namespace {

constexpr double kC0 = 12.5;  ///< inflow / initial concentration (mg/L)

openswmm::SWMMEngine& as_cpp_engine(SWMM_Engine engine) {
    return *static_cast<openswmm::SWMMEngine*>(engine);
}

/// Three conduits in series, constant 1 cfs inflow at J0 with pollutant TSS
/// at concentration `c_in`; initial TSS everywhere = `c_init`.
/// `extra_options` appends raw [OPTIONS] lines (E2: FV_MIN_CELLS etc.).
void write_deck(const char* path, const char* quality_solver_line,
                double c_in, double c_init,
                const char* extra_options = "") {
    std::ofstream f(path);
    f << "[TITLE]\nE1 ARD transport gate deck\n\n"
      << "[OPTIONS]\n"
      << "FLOW_UNITS           CFS\n"
      << "FLOW_ROUTING         DYNWAVE\n"
      << quality_solver_line
      << extra_options
      << "START_DATE           01/01/2026\nSTART_TIME           00:00:00\n"
      << "END_DATE             01/01/2026\nEND_TIME             06:00:00\n"
      << "REPORT_START_DATE    01/01/2026\nREPORT_START_TIME    00:00:00\n"
      << "ROUTING_STEP         5\nREPORT_STEP          00:05:00\n"
      << "WET_STEP             00:05:00\nDRY_STEP             00:05:00\n\n"
      << "[JUNCTIONS]\n"
      << ";;Name  Elev  MaxDepth  InitDepth  SurDepth  Aponded\n"
      << "J0      10.0  10        0.5        0         0\n"
      << "J1      9.0   10        0.5        0         0\n"
      << "J2      8.0   10        0.5        0         0\n\n"
      << "[OUTFALLS]\n;;Name  Elev  Type  StageData  Gated\n"
      << "OUT     7.0   FREE              NO\n\n"
      << "[CONDUITS]\n"
      << ";;Name  From  To   Length  N      Zin  Zout  Q0\n"
      << "C1      J0    J1   400     0.013  0    0     0\n"
      << "C2      J1    J2   400     0.013  0    0     0\n"
      << "C3      J2    OUT  400     0.013  0    0     0\n\n"
      << "[XSECTIONS]\n"
      << ";;Link  Shape       G1   G2 G3 G4\n"
      << "C1      CIRCULAR    1.5  0  0  0\n"
      << "C2      CIRCULAR    1.5  0  0  0\n"
      << "C3      CIRCULAR    1.5  0  0  0\n\n"
      << "[POLLUTANTS]\n"
      << ";;Name  Units  Crain  Cgw  Crdii  Kdecay  SnowOnly  CoPollut  "
         "CoFrac  Cdwf  Cinit\n"
      << "TSS     MG/L   0.0    0.0  0.0    0.0     NO        *         "
         "0.0     0.0   " << c_init << "\n\n"
      << "[INFLOWS]\n"
      << ";;Node  Constituent  Timeseries  Type    Mfactor  Sfactor  "
         "Baseline  Pattern\n"
      << "J0      FLOW         \"\"          FLOW    1.0      1.0      "
         "1.0\n"
      << "J0      TSS          \"\"          CONCEN  1.0      1.0      "
      << c_in << "\n\n"
      << "[REPORT]\nINPUT NO\nCONTINUITY YES\nFLOWSTATS NO\n";
}

class ArdTransportTest : public ::testing::Test {
protected:
    void SetUp() override {
        engine_ = swmm_engine_create();
        ASSERT_NE(engine_, nullptr);
    }
    void TearDown() override {
        if (engine_) { swmm_engine_destroy(engine_); engine_ = nullptr; }
    }
    /// @param forced_mass_rate  if > 0, applied once via
    ///        swmm_node_set_quality_mass_flux at node 0 / pollutant 0 after
    ///        start() — the persistent runtime-API forcing path.
    void run(const char* inp, const char* rpt, const char* out,
             double forced_mass_rate = 0.0) {
        ASSERT_EQ(swmm_engine_open(engine_, inp, rpt, out, nullptr), SWMM_OK);
        ASSERT_EQ(swmm_engine_initialize(engine_), SWMM_OK);
        ASSERT_EQ(swmm_engine_start(engine_, 1), SWMM_OK);
        if (forced_mass_rate > 0.0)
            ASSERT_EQ(swmm_node_set_quality_mass_flux(engine_, 0, 0,
                                                      forced_mass_rate),
                      SWMM_OK);
        double elapsed_days = 0.0;
        int guard = 0;
        do {
            ASSERT_EQ(swmm_engine_step(engine_, &elapsed_days), SWMM_OK);
        } while (elapsed_days > 0.0 && ++guard < 200000);
        ASSERT_EQ(swmm_engine_end(engine_), SWMM_OK);
    }
    /// Fresh engine handle, so one TEST_F can run several simulations.
    void restart() {
        swmm_engine_destroy(engine_);
        engine_ = swmm_engine_create();
        ASSERT_NE(engine_, nullptr);
    }
    SWMM_Engine engine_ = nullptr;
};

// ---------------------------------------------------------------------------
// Gate 1 — uniform field stays uniform under EULERIAN_ARD (E1 / G-UT2 seed).
// ---------------------------------------------------------------------------
TEST_F(ArdTransportTest, UniformFieldStaysUniform) {
    write_deck("_ard_uniform.inp", "QUALITY_SOLVER       EULERIAN_ARD\n",
               kC0, kC0);
    run("_ard_uniform.inp", "_ard_uniform.rpt", "_ard_uniform.out");

    const auto& ctx = as_cpp_engine(engine_).context();
    ASSERT_EQ(ctx.n_pollutants(), 1);
    // ARD engine must actually be active — a silent LEGACY fallback would
    // pass the value checks and mask a broken mesh path.
    bool fell_back = false;
    for (const auto& w : ctx.warnings)
        if (w.find("falling back to LEGACY") != std::string::npos)
            fell_back = true;
    ASSERT_FALSE(fell_back);

    for (int l = 0; l < ctx.n_links(); ++l)
        EXPECT_NEAR(ctx.links.conc[static_cast<std::size_t>(l)], kC0,
                    1.0e-6 * kC0)
            << "link " << l;
    for (int n = 0; n < ctx.n_nodes(); ++n) {
        // Outfalls hold no volume; only check nodes that carry water.
        if (ctx.nodes.volume[static_cast<std::size_t>(n)] > 1.0e-9)
            EXPECT_NEAR(ctx.nodes.conc[static_cast<std::size_t>(n)], kC0,
                        1.0e-6 * kC0)
                << "node " << n;
    }
    std::remove("_ard_uniform.inp");
}

// ---------------------------------------------------------------------------
// Gate 2 — max principle + monotone front on a clean system fed at c0.
// ---------------------------------------------------------------------------
TEST_F(ArdTransportTest, FrontIsBoundedMonotoneAndOrdered) {
    write_deck("_ard_front.inp", "QUALITY_SOLVER       EULERIAN_ARD\n",
               kC0, 0.0);
    run("_ard_front.inp", "_ard_front.rpt", "_ard_front.out");

    const auto& ctx = as_cpp_engine(engine_).context();
    const double tol = 1.0e-9 * kC0;
    double upstream = kC0 + tol;
    for (int l = 0; l < ctx.n_links(); ++l) {
        const double c = ctx.links.conc[static_cast<std::size_t>(l)];
        EXPECT_GE(c, -tol) << "negative concentration, link " << l;
        EXPECT_LE(c, kC0 + tol) << "overshoot above feed, link " << l;
        // Series line fed from upstream: concentration is non-increasing
        // downstream while the front is still filling.
        EXPECT_LE(c, upstream + tol) << "front ordering violated, link " << l;
        upstream = c;
    }
    // After 6 h at ~1 cfs through 1200 ft the front must have arrived: the
    // first link is essentially at feed concentration.
    EXPECT_GT(ctx.links.conc[0], 0.9 * kC0);
    std::remove("_ard_front.inp");
}

// ---------------------------------------------------------------------------
// Gate 3 — LEGACY still the default; both engines agree at steady state.
// ---------------------------------------------------------------------------
TEST_F(ArdTransportTest, LegacyDefaultAndSteadyStateAgreement) {
    write_deck("_ard_legacy.inp", "", kC0, 0.0);   // no QUALITY_SOLVER line
    run("_ard_legacy.inp", "_ard_legacy.rpt", "_ard_legacy.out");
    std::vector<double> legacy_conc;
    {
        const auto& ctx = as_cpp_engine(engine_).context();
        ASSERT_EQ(static_cast<int>(ctx.options.quality_solver),
                  static_cast<int>(openswmm::QualitySolverKind::LEGACY));
        legacy_conc = ctx.links.conc;
    }
    swmm_engine_destroy(engine_);
    engine_ = swmm_engine_create();
    ASSERT_NE(engine_, nullptr);

    write_deck("_ard_steady.inp", "QUALITY_SOLVER       EULERIAN_ARD\n",
               kC0, 0.0);
    run("_ard_steady.inp", "_ard_steady.rpt", "_ard_steady.out");
    const auto& ctx = as_cpp_engine(engine_).context();
    ASSERT_EQ(legacy_conc.size(), ctx.links.conc.size());
    // Steady limit: both transport models must deliver the feed
    // concentration through the line (5% band — models differ in transient,
    // not in the steady limit).
    for (std::size_t l = 0; l < legacy_conc.size(); ++l) {
        EXPECT_NEAR(ctx.links.conc[l], legacy_conc[l], 0.05 * kC0)
            << "steady-state disagreement, link " << l;
    }
    std::remove("_ard_legacy.inp");
    std::remove("_ard_steady.inp");
}

// ---------------------------------------------------------------------------
// Gate 4 (E2) — constituents cross a structure (orifice) as zero-volume
// passthrough of the donor node store.
// ---------------------------------------------------------------------------

/// J0 →C1→ J1 →O1(orifice)→ J2 →C2→ OUT, constant inflow at concentration c0.
void write_orifice_deck(const char* path, double c_in) {
    std::ofstream f(path);
    f << "[TITLE]\nE2 structure passthrough gate deck\n\n"
      << "[OPTIONS]\n"
      << "FLOW_UNITS           CFS\nFLOW_ROUTING         DYNWAVE\n"
      << "QUALITY_SOLVER       EULERIAN_ARD\n"
      << "START_DATE           01/01/2026\nSTART_TIME           00:00:00\n"
      << "END_DATE             01/01/2026\nEND_TIME             06:00:00\n"
      << "REPORT_START_DATE    01/01/2026\nREPORT_START_TIME    00:00:00\n"
      << "ROUTING_STEP         5\nREPORT_STEP          00:05:00\n"
      << "WET_STEP             00:05:00\nDRY_STEP             00:05:00\n\n"
      << "[JUNCTIONS]\n"
      << ";;Name  Elev  MaxDepth  InitDepth  SurDepth  Aponded\n"
      << "J0      10.0  10        0.5        0         0\n"
      << "J1      9.0   10        0.5        0         0\n"
      << "J2      8.5   10        0.5        0         0\n\n"
      << "[OUTFALLS]\n;;Name  Elev  Type  StageData  Gated\n"
      << "OUT     7.0   FREE              NO\n\n"
      << "[CONDUITS]\n"
      << ";;Name  From  To   Length  N      Zin  Zout  Q0\n"
      << "C1      J0    J1   400     0.013  0    0     0\n"
      << "C2      J2    OUT  400     0.013  0    0     0\n\n"
      << "[ORIFICES]\n"
      << ";;Name  From  To   Type  Offset  Cd    Gated  CloseTime\n"
      << "O1      J1    J2   SIDE  0.0     0.65  NO     0\n\n"
      << "[XSECTIONS]\n"
      << ";;Link  Shape       G1   G2 G3 G4\n"
      << "C1      CIRCULAR    1.5  0  0  0\n"
      << "C2      CIRCULAR    1.5  0  0  0\n"
      << "O1      CIRCULAR    1.0  0  0  0\n\n"
      << "[POLLUTANTS]\n"
      << ";;Name  Units  Crain  Cgw  Crdii  Kdecay  SnowOnly  CoPollut  "
         "CoFrac  Cdwf  Cinit\n"
      << "TSS     MG/L   0.0    0.0  0.0    0.0     NO        *         "
         "0.0     0.0   0.0\n\n"
      << "[INFLOWS]\n"
      << ";;Node  Constituent  Timeseries  Type    Mfactor  Sfactor  "
         "Baseline  Pattern\n"
      << "J0      FLOW         \"\"          FLOW    1.0      1.0      1.0\n"
      << "J0      TSS          \"\"          CONCEN  1.0      1.0      "
      << c_in << "\n\n"
      << "[REPORT]\nINPUT NO\nCONTINUITY YES\nFLOWSTATS NO\n";
}

TEST_F(ArdTransportTest, StructurePassthroughCarriesMass) {
    write_orifice_deck("_ard_orifice.inp", kC0);
    run("_ard_orifice.inp", "_ard_orifice.rpt", "_ard_orifice.out");

    const auto& ctx = as_cpp_engine(engine_).context();
    bool fell_back = false;
    for (const auto& w : ctx.warnings)
        if (w.find("falling back to LEGACY") != std::string::npos)
            fell_back = true;
    ASSERT_FALSE(fell_back);

    const double tol = 1.0e-9 * kC0;
    // Downstream of the orifice (link index order: C1=0, C2=1, O1=structure):
    // by 6 h the feed must have crossed the structure — the E1 behavior this
    // gate exists to kill was C2 pinned at exactly zero forever.
    double c2 = -1.0;
    for (int l = 0; l < ctx.n_links(); ++l) {
        const double c = ctx.links.conc[static_cast<std::size_t>(l)];
        EXPECT_GE(c, -tol) << "link " << l;
        EXPECT_LE(c, kC0 + tol) << "link " << l;   // max principle across the structure
    }
    c2 = ctx.links.conc[1];
    EXPECT_GT(c2, 0.5 * kC0)
        << "constituent did not cross the orifice (E2 passthrough)";
    std::remove("_ard_orifice.inp");
}

// ---------------------------------------------------------------------------
// Gate 5 (E2) — CSTR limit: 1 cell/conduit + UPWIND vs the LEGACY engine.
// Loose band by design: the two models share the steady limit and the same
// storage topology at this resolution but differ in mixing order.
// ---------------------------------------------------------------------------
TEST_F(ArdTransportTest, CstrLimitTracksLegacy) {
    const char* kCstrOpts =
        "FV_MIN_CELLS         1\nFV_SCALAR_SCHEME     UPWIND\n";
    write_deck("_ard_cstr_legacy.inp", "", kC0, 0.0);
    run("_ard_cstr_legacy.inp", "_ard_cstr_legacy.rpt", "_ard_cstr_legacy.out");
    std::vector<double> legacy_conc;
    {
        const auto& ctx = as_cpp_engine(engine_).context();
        legacy_conc = ctx.links.conc;
    }
    swmm_engine_destroy(engine_);
    engine_ = swmm_engine_create();
    ASSERT_NE(engine_, nullptr);

    write_deck("_ard_cstr.inp", "QUALITY_SOLVER       EULERIAN_ARD\n",
               kC0, 0.0, kCstrOpts);
    run("_ard_cstr.inp", "_ard_cstr.rpt", "_ard_cstr.out");
    const auto& ctx = as_cpp_engine(engine_).context();
    ASSERT_EQ(legacy_conc.size(), ctx.links.conc.size());
    for (std::size_t l = 0; l < legacy_conc.size(); ++l)
        EXPECT_NEAR(ctx.links.conc[l], legacy_conc[l], 0.20 * kC0)
            << "CSTR-limit divergence beyond the documented band, link " << l;
    std::remove("_ard_cstr_legacy.inp");
    std::remove("_ard_cstr.inp");
}

// ---------------------------------------------------------------------------
// Gate 6 — persistent runtime-API quality mass flux must actually ROUTE, under
// BOTH quality engines, and must close continuity.
//
// This gate exists because the failure it guards is silent in both directions.
// The forcing used to be applied as a post-quality `conc +=` bump, which the
// next step's mixing (LEGACY) or the next ARD publish overwrote: the mass was
// booked in the ledger and never entered the system, so a forced run produced
// byte-identical loads to an unforced one while the report looked healthy.
// Delivering it without booking it is the mirror failure — a large negative
// continuity error. Assert both halves: the mass arrives downstream AND the
// ledger still closes. Legacy reference: routing.c addExternalInflows(),
// apiExtQualMassFlux → Node[j].newQual[p] + massbal EXTERNAL_INFLOW.
// ---------------------------------------------------------------------------
TEST_F(ArdTransportTest, ForcedQualityMassFluxRoutesUnderBothEngines) {
    struct Case { const char* name; const char* solver_line; };
    const Case cases[] = {
        {"LEGACY",       ""},
        {"EULERIAN_ARD", "QUALITY_SOLVER       EULERIAN_ARD\n"},
    };

    for (const auto& c : cases) {
        SCOPED_TRACE(c.name);

        // Baseline: same deck, no forcing.
        write_deck("_ard_force_base.inp", c.solver_line, kC0, 0.0);
        restart();
        run("_ard_force_base.inp", "_ard_force_base.rpt", "_ard_force_base.out");
        const double base_c0 = as_cpp_engine(engine_).context().links.conc[0];

        // Same deck with a persistent forced mass rate equal to the deck's own
        // feed rate (1 cfs x kC0), so a working path roughly doubles the
        // in-system concentration and a broken one changes nothing at all.
        write_deck("_ard_force.inp", c.solver_line, kC0, 0.0);
        restart();
        run("_ard_force.inp", "_ard_force.rpt", "_ard_force.out", kC0);
        const auto& ctx = as_cpp_engine(engine_).context();
        const double forced_c0 = ctx.links.conc[0];

        EXPECT_GT(forced_c0, 1.5 * base_c0)
            << "forced quality mass never reached the network (base "
            << base_c0 << " -> forced " << forced_c0 << ")";

        // ...and it must be booked, or the mass shows up as a continuity hole.
        double err = 0.0;
        ASSERT_EQ(swmm_get_quality_continuity_error(engine_, 0, &err), SWMM_OK);
        EXPECT_LT(std::fabs(err), 0.05)
            << "forced mass routed but is missing from the ledger, error " << err;
    }
    std::remove("_ard_force_base.inp");
    std::remove("_ard_force.inp");
}

}  // namespace
