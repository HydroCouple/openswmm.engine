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
 * @file test_2d_infil_integration.cpp
 * @brief Track-I end-to-end gates for per-cell 2D infiltration (plan §5.5).
 *
 * @details Drives real rain-on-grid models through the public C API. Covers
 *          the gates that only a running solver can settle:
 *
 *          - **G1**  no `[2D_INFILTRATION*]` section leaves the run unchanged.
 *          - **G3**  the ledger closes and `sum(infil_cum * area) == infil_out`.
 *          - **G4**  the per-cell continuity residual stays clean on an
 *                    infiltrating cell (guards the silent call site 4).
 *          - **G6**  a cell that sits inactive across many `INFIL_STEP`s ends
 *                    in the same infiltration state as a forced-active twin.
 *          - **G7**  section round-trip through the writer.
 *          - **G11** validation rejects what §5.5.4 says it must.
 *
 *          Artifacts are written under `tests/unit/engine/data/infil2d_out/`
 *          per CLAUDE.md §4.1 — reviewable, never a temp directory.
 *
 * @see plans/TWO_ZONE_GROUNDWATER_EXPLICIT_LTS_PLAN_2026-08-15.md §5.5
 * @ingroup engine_2d
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

#include "platform_test_support.hpp"

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_2d.h>
#include <openswmm/engine/openswmm_infil2d.h>
#include <openswmm/engine/openswmm_model.h>

namespace fs = std::filesystem;

namespace {

using plattest::closeFd;
using plattest::dup2Fd;
using plattest::dupFd;
using plattest::fileNo;
using plattest::setEnvVar;
using plattest::unsetEnvVar;

constexpr double kMetrePerInch = 0.0254;
constexpr double kSecPerHour   = 3600.0;
constexpr double inhrToMs(double v) { return v * kMetrePerInch / kSecPerHour; }

// ---------------------------------------------------------------------------
// A closed rain-on-grid basin: a 20 x 20 m flat pan with NO 1D network, no
// outfall, no boundary. Every drop that falls either stands on the mesh,
// evaporates, or infiltrates — which is what makes the budget in G3 closed.
// Four triangles so tags and per-cell overrides have somewhere to land.
// ---------------------------------------------------------------------------
std::string buildModel(const std::string& infil_sections,
                       const std::string& flow_units = "CMS",
                       const std::string& end_time   = "02:00:00") {
    std::ostringstream s;
    s << "[OPTIONS]\n"
         "FLOW_UNITS           " << flow_units << "\n"
         "FLOW_ROUTING         DYNWAVE\n"
         "START_DATE           01/01/2026\n"
         "START_TIME           00:00:00\n"
         "END_DATE             01/01/2026\n"
         "END_TIME             " << end_time << "\n"
         "REPORT_STEP          00:05:00\n"
         "WET_STEP             00:05:00\n"
         "DRY_STEP             00:05:00\n"
         "ROUTING_STEP         10\n"
         "ALLOW_PONDING        NO\n"
         "\n"
         // A minimal 1D network. It carries no water and is NOT coupled to the
         // mesh (there is no [2D_VERTEX_NODE_MAP] / [2D_TRIANGLE_NODE_MAP]),
         // but without it FLOW_ROUTING is reported as NO and the engine has no
         // routing loop for the 2D surface to co-advance against.
         "[JUNCTIONS]\n"
         ";;Name  Elev  MaxDepth  InitDepth  SurDepth  Aponded\n"
         "J1      0.0   1.0       0          0         0\n"
         "\n"
         "[OUTFALLS]\n"
         ";;Name  Elev   Type  Gated\n"
         "O1     -0.5    FREE  NO\n"
         "\n"
         "[CONDUITS]\n"
         ";;Name  From  To  Length  Roughness  InOffset  OutOffset  InitFlow\n"
         "C1      J1    O1  30.0    0.013      0         0          0\n"
         "\n"
         "[XSECTIONS]\n"
         ";;Link  Shape     Geom1  Geom2  Geom3  Geom4  Barrels\n"
         "C1      CIRCULAR  0.3    0      0      0      1\n"
         "\n"
         "[2D_OPTIONS]\n"
         "MAX_TIMESTEP     2\n"
         "DRY_DEPTH        0.001\n"
         "REPORT_2D        NO\n"
         "\n"
         "[2D_VERTICES]\n"
         ";;X      Y      Z\n"
         " 0.0    0.0   10.0\n"
         "20.0    0.0   10.0\n"
         "20.0   20.0   10.0\n"
         " 0.0   20.0   10.0\n"
         "10.0   10.0   10.0\n"
         "\n"
         "[2D_TRIANGLES]\n"
         ";;V1  V2  V3  MANNINGS_N  INIT_DEPTH  TAG\n"
         "0     1   4   0.03        0.0         LAWN\n"
         "1     2   4   0.03        0.0         WOODS\n"
         "2     3   4   0.03        0.0         LAWN\n"
         "3     0   4   0.03        0.0         PAVED\n"
      << infil_sections;
    return s.str();
}

struct RunResult {
    bool   ok            = false;
    double rainfall_in   = 0.0;
    double evap_out      = 0.0;
    double infil_out     = 0.0;   // ledger, via swmm_infil2d_get_total_volume
    double infil_out_mb  = 0.0;   // same row, via swmm_2d_get_mass_balance? (n/a)
    double init_storage  = 0.0;
    double final_storage = 0.0;
    double cont_2d       = 0.0;
    int    n_tri         = 0;
    std::vector<double> depth;     // m
    std::vector<double> infil_cum; // m
    std::vector<double> infil_rate;// m/s
    std::vector<double> area;      // m2
    std::vector<double> max_cont_err;  // m3/s
};

/// Run the model under an arbitrary rain schedule (m/s as a function of
/// elapsed seconds). Returns the ledger and per-cell series.
RunResult runWith(const fs::path& dir, const std::string& name,
                  const std::string& model,
                  const std::function<double(double)>& rain_at,
                  const std::function<void(double, SWMM_Engine)>& on_step = {}) {
    RunResult r;
    fs::create_directories(dir);
    const fs::path inp = dir / (name + ".inp");
    const fs::path rpt = dir / (name + ".rpt");
    const fs::path out = dir / (name + ".out");
    { std::ofstream f(inp); f << model; }

    SWMM_Engine eng = swmm_engine_create();
    if (swmm_engine_open(eng, inp.string().c_str(), rpt.string().c_str(),
                         out.string().c_str(), nullptr) != SWMM_OK) {
        swmm_engine_destroy(eng); return r;
    }
    if (swmm_engine_initialize(eng) != SWMM_OK) {
        swmm_engine_close(eng); swmm_engine_destroy(eng); return r;
    }
    int active = 0;
    swmm_2d_is_active(eng, &active);
    if (!active) { swmm_engine_close(eng); swmm_engine_destroy(eng); return r; }
    swmm_2d_triangle_count(eng, &r.n_tri);
    if (swmm_engine_start(eng, 1) != SWMM_OK) {
        swmm_engine_close(eng); swmm_engine_destroy(eng); return r;
    }

    double elapsed = 0.0;
    while (true) {
        const double t_s = elapsed * 86400.0;
        swmm_2d_force_rainfall_uniform(eng, rain_at(t_s), SWMM_FORCING_OVERRIDE,
                                       SWMM_FORCING_PERSIST);
        if (swmm_engine_step(eng, &elapsed) != SWMM_OK || elapsed <= 0.0) break;
        if (on_step) on_step(elapsed * 86400.0, eng);
    }

    r.depth.assign(r.n_tri, 0.0);
    r.infil_cum.assign(r.n_tri, 0.0);
    r.infil_rate.assign(r.n_tri, 0.0);
    r.area.assign(r.n_tri, 0.0);
    r.max_cont_err.assign(r.n_tri, 0.0);
    swmm_2d_get_depths_bulk(eng, r.depth.data());
    swmm_infil2d_get_cum_bulk(eng, r.infil_cum.data(), r.n_tri);
    swmm_infil2d_get_rate_bulk(eng, r.infil_rate.data(), r.n_tri);
    swmm_2d_get_stat_max_continuity_err(eng, r.max_cont_err.data());
    for (int i = 0; i < r.n_tri; ++i)
        swmm_2d_triangle_get_area(eng, i, &r.area[i]);

    swmm_engine_end(eng);

    swmm_2d_get_mass_balance(eng, &r.init_storage, &r.final_storage,
                             &r.rainfall_in, nullptr, nullptr, nullptr,
                             nullptr, nullptr, nullptr, &r.evap_out);
    swmm_infil2d_get_total_volume(eng, &r.infil_out);
    swmm_2d_get_continuity_error(eng, &r.cont_2d);

    swmm_engine_report(eng);
    swmm_engine_close(eng);
    swmm_engine_destroy(eng);
    r.ok = true;
    return r;
}

/// Rain @p rain_ms on every cell for the first @p rain_s seconds, then stop.
RunResult run(const fs::path& dir, const std::string& name,
              const std::string& model, double rain_ms, double rain_s) {
    return runWith(dir, name, model,
                   [=](double t_s) { return (t_s < rain_s) ? rain_ms : 0.0; });
}

class Infil2DIntegrationTest : public ::testing::Test {
protected:
    fs::path dir_;
    void SetUp() override {
        dir_ = fs::path("infil2d_out");
        fs::create_directories(dir_);
    }
};

} // namespace

// ============================================================================
// G1 — a project with no [2D_INFILTRATION*] section is unchanged.
// ============================================================================

// The in-process half of the bitwise-regression gate: with no section present
// the ledger row is exactly zero, the per-cell series are empty-or-zero, and
// the surface result is what it always was. (The cross-binary half — this
// build vs. a pristine HEAD build over the benchmark suite — is run by
// tests/scripts/trackI_bitwise_regression.sh.)
TEST_F(Infil2DIntegrationTest, NoSectionMeansNoInfiltrationAndNoLedgerRow) {
    RunResult r = run(dir_, "g1_no_section", buildModel(""),
                      inhrToMs(2.0), 1800.0);
    ASSERT_TRUE(r.ok);
    ASSERT_GT(r.rainfall_in, 0.0) << "no rain fell — gate is vacuous";

    EXPECT_DOUBLE_EQ(r.infil_out, 0.0)
        << "an unconfigured project booked " << r.infil_out << " m3 of infiltration";
    for (int i = 0; i < r.n_tri; ++i) {
        EXPECT_DOUBLE_EQ(r.infil_rate[i], 0.0) << "cell " << i;
        EXPECT_DOUBLE_EQ(r.infil_cum[i], 0.0)  << "cell " << i;
    }

    // Every drop must still be standing on the mesh (nothing else can take it).
    const double stored = r.final_storage - r.init_storage;
    EXPECT_NEAR(stored, r.rainfall_in - r.evap_out,
                std::max(1e-9, r.rainfall_in * 1e-9))
        << "surface-only budget did not close without infiltration";
}

// ============================================================================
// G3 — the ledger closes, and the per-cell series is consistent with it.
// ============================================================================

TEST_F(Infil2DIntegrationTest, RainOnGridBudgetClosesWithInfiltration) {
    const std::string sections =
        "\n[2D_INFILTRATION_DEFAULTS]\n"
        ";;tag   method     p1    p2    p3    p4   p5   dest\n"
        "*       CONSTANT   5.0   -     -     -    -    LOST\n";

    RunResult r = run(dir_, "g3_budget",
                      buildModel(sections, "CFS"),   // 5.0 in/hr
                      inhrToMs(4.0), 3600.0);
    ASSERT_TRUE(r.ok);
    ASSERT_GT(r.rainfall_in, 0.0)  << "no rain fell — gate is vacuous";
    ASSERT_GT(r.infil_out, 0.0)    << "nothing infiltrated — gate is vacuous";

    // Closed basin: in - out = change in storage.
    const double residual = r.rainfall_in - r.infil_out - r.evap_out
                          - (r.final_storage - r.init_storage);
    const double scale = std::max(r.rainfall_in, 1e-12);
    EXPECT_LT(std::abs(residual) / scale, 1e-9)
        << "budget did not close: rainfall " << r.rainfall_in
        << " - infil " << r.infil_out << " - evap " << r.evap_out
        << " - dStorage " << (r.final_storage - r.init_storage)
        << " = " << residual << " m3";

    // The plan's direct probe of call site 5: the per-cell APPLIED cumulative
    // depth must reconstruct the ledger row exactly.
    double sum = 0.0;
    for (int i = 0; i < r.n_tri; ++i) sum += r.infil_cum[i] * r.area[i];
    EXPECT_LT(std::abs(sum - r.infil_out) / std::max(r.infil_out, 1e-12), 1e-9)
        << "sum(infil_cum * area) = " << sum
        << " m3 but the ledger row reads " << r.infil_out << " m3";

    // Infiltration must be a MEANINGFUL share of the rain, or the gate cannot
    // tell a working sink from a rounding error.
    EXPECT_GT(r.infil_out, 0.05 * r.rainfall_in)
        << "infiltration took only " << (r.infil_out / r.rainfall_in * 100.0)
        << "% of the rain — too small to discriminate";
}

// Diagnostic companion to the gate above, and a gate in its own right: with
// the surface ponded far above DRY_DEPTH for the whole run, infilSink() is the
// raw held rate at every substep AND at the end-of-step depth the ledger reads,
// so the two cannot disagree. If this closes and the drying run above does not,
// the ledger's re-derivation at the accepted end-of-step depth is the cause,
// not the sink itself.
TEST_F(Infil2DIntegrationTest, LedgerClosesExactlyWhileTheSurfaceStaysPonded) {
    const std::string sections =
        "\n[2D_INFILTRATION_DEFAULTS]\n"
        "*       CONSTANT   0.5   -     -     -    -    LOST\n";

    // Rain 4 in/hr for the WHOLE run against a 0.5 in/hr sink: the pan fills
    // and never dries, so no cell ever enters the wet/dry ramp band.
    RunResult r = run(dir_, "g3_ponded", buildModel(sections, "CFS"),
                      inhrToMs(4.0), 1.0e9);
    ASSERT_TRUE(r.ok);
    ASSERT_GT(r.infil_out, 0.0) << "gate is vacuous";
    for (int i = 0; i < r.n_tri; ++i)
        ASSERT_GT(r.depth[i], 0.01)
            << "cell " << i << " ended at depth " << r.depth[i]
            << " m — it is not ponded, so this is not the intended regime";

    const double residual = r.rainfall_in - r.infil_out - r.evap_out
                          - (r.final_storage - r.init_storage);
    EXPECT_LT(std::abs(residual) / std::max(r.rainfall_in, 1e-12), 1e-9)
        << "the ledger does not close even with every cell ponded: rainfall "
        << r.rainfall_in << " - infil " << r.infil_out << " - evap "
        << r.evap_out << " - dStorage " << (r.final_storage - r.init_storage)
        << " = " << residual << " m3";
}

// A configured project must actually route LESS water into storage than the
// same project without infiltration. Without this, a model that publishes
// rates and books infil_out while never removing water (the §3.4 failure
// shape) would pass every other gate here.
TEST_F(Infil2DIntegrationTest, InfiltrationActuallyRemovesWaterFromTheSurface) {
    const std::string sections =
        "\n[2D_INFILTRATION_DEFAULTS]\n"
        "*       CONSTANT   5.0   -     -     -    -    LOST\n";

    RunResult off = run(dir_, "g3_off", buildModel("", "CFS"),
                        inhrToMs(4.0), 3600.0);
    RunResult on  = run(dir_, "g3_on",  buildModel(sections, "CFS"),
                        inhrToMs(4.0), 3600.0);
    ASSERT_TRUE(off.ok);
    ASSERT_TRUE(on.ok);
    ASSERT_GT(on.infil_out, 0.0) << "gate is vacuous";

    const double stored_off = off.final_storage - off.init_storage;
    const double stored_on  = on.final_storage  - on.init_storage;
    EXPECT_LT(stored_on, stored_off)
        << "infiltration booked " << on.infil_out
        << " m3 to its ledger but the surface still holds as much water as the "
           "un-infiltrated run (" << stored_on << " vs " << stored_off
        << " m3) — the sink is published but not applied";

    // The water that left is the water that was booked.
    EXPECT_NEAR(stored_off - stored_on, on.infil_out,
                std::max(1e-6, on.infil_out * 1e-3))
        << "storage fell by " << (stored_off - stored_on)
        << " m3 but the ledger booked " << on.infil_out << " m3";
}

// ============================================================================
// G4 — the per-cell continuity residual (guards the SILENT call site 4).
// ============================================================================

// If infilSink were missing from computeCellContinuity, the diagnostic residual
// on an infiltrating cell would carry the whole infiltration flux. Compare
// against the same model with the sink switched off: the residual must not
// grow by anything like the infiltration flux.
TEST_F(Infil2DIntegrationTest, CellContinuityResidualStaysCleanWhileInfiltrating) {
    const std::string sections =
        "\n[2D_INFILTRATION_DEFAULTS]\n"
        "*       CONSTANT   5.0   -     -     -    -    LOST\n";

    RunResult off = run(dir_, "g4_off", buildModel("", "CFS"),
                        inhrToMs(4.0), 3600.0);
    RunResult on  = run(dir_, "g4_on",  buildModel(sections, "CFS"),
                        inhrToMs(4.0), 3600.0);
    ASSERT_TRUE(off.ok);
    ASSERT_TRUE(on.ok);
    ASSERT_GT(on.infil_out, 0.0) << "gate is vacuous";

    for (int i = 0; i < on.n_tri; ++i) {
        // The infiltration volumetric flux this cell carried at its peak.
        const double infil_flux = inhrToMs(5.0) * on.area[i];   // m3/s
        ASSERT_GT(infil_flux, 0.0);

        // Allow the residual to grow a little (it is a first-order diagnostic
        // and the run is not identical), but nothing approaching the flux the
        // omitted term would have injected — a missing infilSink would put
        // the whole flux in the residual, i.e. a ratio of ~1.
        //
        // The bar was 5% while computeCellContinuity divided the one-batch
        // storage delta by the whole OUTPUT-REFRESH span (30 s here against
        // a 10 s ROUTING_STEP), which reported exactly a third of the real
        // residual. With the denominator corrected to the batch span the
        // measured ratio on this deck is 0.106, and it falls with the routing
        // step (under 0.05 at ROUTING_STEP <= 5 s) — first-order, as the
        // diagnostic is documented to be: infilSink is evaluated at the
        // accepted end-of-step depth while the marcher integrated it against
        // the depths it saw, and this deck (5 in/hr capacity against 4 in/hr
        // rain) sits exactly on the depth-limited boundary where those differ
        // most.
        const double budget = std::max(off.max_cont_err[i] * 10.0,
                                       infil_flux * 0.25);
        EXPECT_LT(on.max_cont_err[i], budget)
            << "cell " << i << ": max |continuity residual| rose to "
            << on.max_cont_err[i] << " m3/s with infiltration on (was "
            << off.max_cont_err[i] << "); the infiltration flux is "
            << infil_flux << " m3/s — computeCellContinuity is missing the "
               "infilSink term (plan §5.5.2 site 4)";
    }
}

// ============================================================================
// G6 — lazy-path equivalence (a dry cell must still advance its state).
// ============================================================================

// §5.5.2's dry-cell rule, end-to-end: "the router advances state for every
// model-carrying cell each INFIL_STEP regardless of active-set membership".
//
// Two halves of one mesh carry the identical Horton row. Cells 0-1 are rained
// on from t=0 and stay in the marcher's active set the whole run. Cells 2-3
// get nothing until the last twenty minutes, so they sit dry and OUT of the
// active set for two hours while the router keeps ticking them.
//
// When the late rain arrives the long-inactive pair must present the FULL
// initial Horton capacity, hand-computed in SI from f0 = 3.0 in/hr — its state
// must be neither corrupted nor spuriously advanced by the ticks it received
// while inactive — while the continuously-wet pair must by then have decayed
// toward fmin. Same row, two genuinely separate state trajectories.
//
// (The complementary half of this gate — that a DRY cell's Horton recovery
// still runs while it is inactive — is Infil2DDryCellTest in test_2d_infil.cpp.
// It cannot be reached from a closed rain-on-grid pan, because the wet/dry
// sink ramp leaves a residual film that keeps the kernel on its wet path.)
TEST_F(Infil2DIntegrationTest, AnInactiveCellStillAdvancesItsInfiltrationState) {
    const std::string sections =
        "\n[2D_INFILTRATION_DEFAULTS]\n"
        "*       HORTON   3.0   0.5   4.14   0   0   LOST\n";

    fs::create_directories(dir_);
    const fs::path inp = dir_ / "g6_twin.inp";
    { std::ofstream f(inp); f << buildModel(sections, "CFS", "02:20:00"); }

    SWMM_Engine eng = swmm_engine_create();
    ASSERT_EQ(swmm_engine_open(eng, inp.string().c_str(),
                               (dir_ / "g6_twin.rpt").string().c_str(),
                               (dir_ / "g6_twin.out").string().c_str(),
                               nullptr), SWMM_OK);
    ASSERT_EQ(swmm_engine_initialize(eng), SWMM_OK);
    int nt = 0;
    swmm_2d_triangle_count(eng, &nt);
    ASSERT_EQ(nt, 4);
    ASSERT_EQ(swmm_engine_start(eng, 1), SWMM_OK);

    constexpr double kLateRainAt = 7200.0;   // the dry pair's rain starts here
    double f_late_at_arrival = -1.0;
    double f_wet_at_arrival  = -1.0;
    double cum_late_before   = -1.0;

    double elapsed = 0.0;
    while (true) {
        const double t_s = elapsed * 86400.0;
        for (int i = 0; i < 2; ++i)
            swmm_2d_force_rainfall(eng, i, inhrToMs(4.0),
                                   SWMM_FORCING_OVERRIDE, SWMM_FORCING_PERSIST);
        const double late = (t_s >= kLateRainAt) ? inhrToMs(4.0) : 0.0;
        for (int i = 2; i < 4; ++i)
            swmm_2d_force_rainfall(eng, i, late,
                                   SWMM_FORCING_OVERRIDE, SWMM_FORCING_PERSIST);

        if (t_s < kLateRainAt && cum_late_before < 0.0 && t_s > 6000.0) {
            double F[8] = {0};
            if (swmm_infil2d_get_cum_bulk(eng, F, 8) == SWMM_OK)
                cum_late_before = F[2];
        }

        if (swmm_engine_step(eng, &elapsed) != SWMM_OK || elapsed <= 0.0) break;

        const double now = elapsed * 86400.0;
        if (f_late_at_arrival < 0.0 && now >= kLateRainAt + 300.0) {
            double f[8] = {0};
            if (swmm_infil2d_get_rate_bulk(eng, f, 8) == SWMM_OK) {
                f_late_at_arrival = f[2];
                f_wet_at_arrival  = f[0];
            }
        }
    }
    swmm_engine_end(eng);
    double total = 0.0;
    swmm_infil2d_get_total_volume(eng, &total);
    swmm_engine_report(eng);
    swmm_engine_close(eng);
    swmm_engine_destroy(eng);

    ASSERT_GT(total, 0.0)              << "gate is vacuous — nothing infiltrated";
    ASSERT_GE(cum_late_before, 0.0)    << "never sampled the dry pair";
    ASSERT_GT(f_late_at_arrival, 0.0)  << "never sampled the late rain";
    ASSERT_GT(f_wet_at_arrival, 0.0)   << "never sampled the wet pair";

    // A cell that never received water must have infiltrated nothing — a stale
    // held rate on an inactive cell would show up here as a phantom loss.
    EXPECT_DOUBLE_EQ(cum_late_before, 0.0)
        << "cell 2 accumulated " << cum_late_before
        << " m of infiltration before any rain reached it";

    // Its capacity must still be the FULL initial f0. Hand-computed in SI:
    // the first step-average of the Horton curve from tp = 0.
    const double f0   = inhrToMs(3.0);
    const double fmin = inhrToMs(0.5);
    const double k    = 4.14 / kSecPerHour;
    auto F = [&](double t) {
        return fmin * t + (f0 - fmin) / k * (1.0 - std::exp(-k * t));
    };
    const double expect_fresh = (F(300.0) - F(0.0)) / 300.0;
    EXPECT_NEAR(f_late_at_arrival, expect_fresh, expect_fresh * 0.05)
        << "when its rain finally arrived the long-inactive cell published "
        << f_late_at_arrival << " m/s; a cell starting from tp = 0 gives "
        << expect_fresh << " m/s. Its Horton state was corrupted or advanced "
           "while it sat inactive (plan §5.5.2).";

    // Meanwhile the pair that was wet the whole time must have decayed. If the
    // per-cell state were shared or the router ticked one array for all cells,
    // these two would agree.
    EXPECT_LT(f_wet_at_arrival, f_late_at_arrival * 0.6)
        << "after two hours of rain the continuously-wet cell still publishes "
        << f_wet_at_arrival << " m/s against the fresh cell's "
        << f_late_at_arrival << " m/s — the two cells are not on separate "
           "Horton state trajectories";
    EXPECT_GT(f_wet_at_arrival, fmin * 0.99)
        << "the wet cell fell below its own fmin";
}

// ============================================================================
// G5 — the Kokkos marcher must agree with the CPU marcher.
// ============================================================================

// §3.4 of the handoff: ExplicitKokkosSurfaceSolver carries its OWN verbatim
// copies of the CPU wet/dry ramps in devInfilSink, and uploads d_infil_ in
// pushForcings. The failure this guards against is a GPU run that publishes
// rates and books infil_out while never removing water — every ledger reading
// looks right and the surface stays wet. So the assertion is on the DEPTH
// FIELD as well as the ledger.
//
// OPENSWMM_2D_BACKEND=omp bypasses the mesh-size gate, so a small mesh still
// exercises the plugin. The test refuses to pass silently if the plugin did
// not load — a CPU-vs-CPU comparison would be vacuous.
TEST_F(Infil2DIntegrationTest, KokkosMarcherAgreesWithTheCpuMarcher) {
    const std::string sections =
        "\n[2D_INFILTRATION_DEFAULTS]\n"
        "*       CONSTANT   5.0   -     -     -    -    LOST\n";
    const std::string model = buildModel(sections, "CFS");

    const char* prev = std::getenv("OPENSWMM_2D_BACKEND");
    const std::string saved = prev ? prev : "";

    setEnvVar("OPENSWMM_2D_BACKEND", "cpu");
    RunResult cpu = run(dir_, "g5_cpu", model, inhrToMs(4.0), 3600.0);

    // Capture stderr so a silent fallback to the CPU marcher is detectable.
    const fs::path errlog = dir_ / "g5_omp_stderr.log";
    fflush(stderr);
    const int saved_fd = dupFd(fileNo(stderr));
    FILE* redirected = std::freopen(errlog.string().c_str(), "w", stderr);
    setEnvVar("OPENSWMM_2D_BACKEND", "omp");
    RunResult omp = run(dir_, "g5_omp", model, inhrToMs(4.0), 3600.0);
    fflush(stderr);
    if (redirected) { dup2Fd(saved_fd, fileNo(stderr)); }
    closeFd(saved_fd);

    if (saved.empty()) unsetEnvVar("OPENSWMM_2D_BACKEND");
    else               setEnvVar("OPENSWMM_2D_BACKEND", saved.c_str());

    ASSERT_TRUE(cpu.ok);
    ASSERT_TRUE(omp.ok);
    ASSERT_GT(cpu.infil_out, 0.0) << "the CPU run infiltrated nothing — gate is vacuous";

    std::string err_text;
    { std::ifstream f(errlog); std::ostringstream ss; ss << f.rdbuf(); err_text = ss.str(); }
    if (err_text.find("falling back to the CPU marcher") != std::string::npos) {
        GTEST_SKIP() << "the Kokkos omp plugin was not loadable, so this would "
                        "be a CPU-vs-CPU comparison: " << err_text;
    }

    // Ledger agreement.
    EXPECT_NEAR(omp.infil_out, cpu.infil_out, std::abs(cpu.infil_out) * 1e-6)
        << "Kokkos booked " << omp.infil_out << " m3 of infiltration against "
           "the CPU marcher's " << cpu.infil_out << " m3";
    EXPECT_NEAR(omp.rainfall_in, cpu.rainfall_in,
                std::abs(cpu.rainfall_in) * 1e-6);

    // Depth-field agreement — the half that catches "booked but never removed".
    ASSERT_EQ(omp.n_tri, cpu.n_tri);
    for (int i = 0; i < cpu.n_tri; ++i) {
        const double tol = std::max(1e-9, std::abs(cpu.depth[i]) * 1e-5);
        EXPECT_NEAR(omp.depth[i], cpu.depth[i], tol)
            << "cell " << i << ": Kokkos ended at depth " << omp.depth[i]
            << " m, the CPU marcher at " << cpu.depth[i]
            << " m — the device sink is not removing the same water";
    }

    // And the per-cell cumulative series the sidecar publishes.
    for (int i = 0; i < cpu.n_tri; ++i) {
        const double tol = std::max(1e-12, std::abs(cpu.infil_cum[i]) * 1e-5);
        EXPECT_NEAR(omp.infil_cum[i], cpu.infil_cum[i], tol) << "cell " << i;
    }
}

// ============================================================================
// G7 — resolution order end-to-end, and the writer round-trip.
// ============================================================================

TEST_F(Infil2DIntegrationTest, ResolutionOrderReachesTheSolverAndTheCellAPI) {
    const std::string sections =
        "\n[2D_INFILTRATION_OPTIONS]\n"
        "INFIL_STEP   00:02:00\n"
        "\n[2D_INFILTRATION_DEFAULTS]\n"
        "*        CONSTANT   1.0   -     -     -    -    LOST\n"
        "LAWN     HORTON     3.0   0.5   4.14  7.0  0    LOST\n"
        "PAVED    NONE\n"
        "\n[2D_INFILTRATION]\n"
        ";; cell (1-based)  method        p1    p2   p3\n"
        "3                  CURVE_NUMBER  85    -    7.0   LOST\n";

    fs::create_directories(dir_);
    const fs::path inp = dir_ / "g7_resolution.inp";
    { std::ofstream f(inp); f << buildModel(sections, "CFS"); }

    SWMM_Engine eng = swmm_engine_create();
    ASSERT_EQ(swmm_engine_open(eng, inp.string().c_str(),
                               (dir_ / "g7_resolution.rpt").string().c_str(),
                               (dir_ / "g7_resolution.out").string().c_str(),
                               nullptr), SWMM_OK);
    ASSERT_EQ(swmm_engine_initialize(eng), SWMM_OK);

    // Cell 1 (index 0) = LAWN -> tag row; cell 2 (index 1) = WOODS -> '*';
    // cell 3 (index 2) = LAWN but OVERRIDDEN; cell 4 (index 3) = PAVED -> NONE.
    struct Expect { int tri; int method; int has; int is_override; const char* why; };
    const Expect want[] = {
        {0, SWMM_INFIL2D_HORTON,       1, 0, "LAWN tag row"},
        {1, SWMM_INFIL2D_CONSTANT,     1, 0, "WOODS falls through to '*'"},
        {2, SWMM_INFIL2D_CURVE_NUMBER, 1, 1, "per-cell override beats the LAWN tag"},
        {3, 0,                         0, 0, "PAVED spells NONE, clearing '*'"},
    };
    for (const Expect& w : want) {
        SWMM_Infil2DRow row{};
        int is_override = -1;
        ASSERT_EQ(swmm_infil2d_get_cell(eng, w.tri, &row, &is_override), SWMM_OK)
            << "tri " << w.tri;
        EXPECT_EQ(row.has_method, w.has) << "tri " << w.tri << ": " << w.why;
        if (w.has) EXPECT_EQ(row.method, w.method) << "tri " << w.tri << ": " << w.why;
        EXPECT_EQ(is_override, w.is_override) << "tri " << w.tri << ": " << w.why;
    }

    SWMM_Infil2DOptions opt{};
    ASSERT_EQ(swmm_infil2d_get_options(eng, &opt), SWMM_OK);
    EXPECT_DOUBLE_EQ(opt.infil_step, 120.0) << "INFIL_STEP did not survive parsing";

    int n_defaults = 0;
    ASSERT_EQ(swmm_infil2d_defaults_count(eng, &n_defaults), SWMM_OK);
    EXPECT_EQ(n_defaults, 3) << "expected the '*', LAWN and PAVED default rows";

    swmm_engine_close(eng);
    swmm_engine_destroy(eng);
}

// Author -> save -> reload -> the same resolved model. The writer must emit a
// COMPACT file (provenance-preserving, D-I3): a mesh-wide '*' row must not be
// exploded into one row per cell.
TEST_F(Infil2DIntegrationTest, SectionsRoundTripThroughTheWriter) {
    const std::string sections =
        "\n[2D_INFILTRATION_OPTIONS]\n"
        "INFIL_STEP   00:02:00\n"
        "\n[2D_INFILTRATION_DEFAULTS]\n"
        "*        CONSTANT   1.0   -     -     -    -    LOST\n"
        "LAWN     HORTON     3.0   0.5   4.14  7.0  0    LOST\n"
        "PAVED    NONE\n"
        "\n[2D_INFILTRATION]\n"
        "3                  CURVE_NUMBER  85    -    7.0   LOST\n";

    fs::create_directories(dir_);
    const fs::path inp  = dir_ / "g7_rt_in.inp";
    const fs::path save = dir_ / "g7_rt_saved.inp";
    { std::ofstream f(inp); f << buildModel(sections, "CFS"); }

    SWMM_Engine eng = swmm_engine_create();
    ASSERT_EQ(swmm_engine_open(eng, inp.string().c_str(),
                               (dir_ / "g7_rt.rpt").string().c_str(),
                               (dir_ / "g7_rt.out").string().c_str(),
                               nullptr), SWMM_OK);
    ASSERT_EQ(swmm_model_write(eng, save.string().c_str()), SWMM_OK);
    swmm_engine_close(eng);
    swmm_engine_destroy(eng);

    ASSERT_TRUE(fs::exists(save));
    std::string text;
    { std::ifstream f(save); std::ostringstream ss; ss << f.rdbuf(); text = ss.str(); }

    EXPECT_NE(text.find("[2D_INFILTRATION_OPTIONS]"), std::string::npos)
        << "the writer dropped [2D_INFILTRATION_OPTIONS]";
    EXPECT_NE(text.find("[2D_INFILTRATION_DEFAULTS]"), std::string::npos)
        << "the writer dropped [2D_INFILTRATION_DEFAULTS]";
    EXPECT_NE(text.find("[2D_INFILTRATION]"), std::string::npos)
        << "the writer dropped [2D_INFILTRATION]";

    // Compactness: a 4-cell mesh whose '*' row covers 3 cells must NOT emit
    // three per-cell rows.
    {
        std::istringstream in(text);
        std::string line;
        bool in_cells = false;
        int  cell_rows = 0;
        while (std::getline(in, line)) {
            if (!line.empty() && line[0] == '[') {
                in_cells = (line.rfind("[2D_INFILTRATION]", 0) == 0);
                continue;
            }
            if (!in_cells) continue;
            const auto first = line.find_first_not_of(" \t\r\n");
            if (first == std::string::npos || line[first] == ';') continue;
            ++cell_rows;
        }
        EXPECT_EQ(cell_rows, 1)
            << "[2D_INFILTRATION] emitted " << cell_rows
            << " rows; provenance should keep it to the single override (D-I3)";
    }

    // Reload the saved file and confirm the resolved model is identical.
    SWMM_Engine eng2 = swmm_engine_create();
    ASSERT_EQ(swmm_engine_open(eng2, save.string().c_str(),
                               (dir_ / "g7_rt2.rpt").string().c_str(),
                               (dir_ / "g7_rt2.out").string().c_str(),
                               nullptr), SWMM_OK);
    ASSERT_EQ(swmm_engine_initialize(eng2), SWMM_OK);

    const int want_method[4] = {SWMM_INFIL2D_HORTON, SWMM_INFIL2D_CONSTANT,
                                SWMM_INFIL2D_CURVE_NUMBER, -1};
    for (int t = 0; t < 4; ++t) {
        SWMM_Infil2DRow row{};
        int is_override = -1;
        ASSERT_EQ(swmm_infil2d_get_cell(eng2, t, &row, &is_override), SWMM_OK);
        if (want_method[t] < 0) {
            EXPECT_EQ(row.has_method, 0) << "tri " << t << " should be NONE after reload";
        } else {
            ASSERT_EQ(row.has_method, 1) << "tri " << t << " lost its model on reload";
            EXPECT_EQ(row.method, want_method[t]) << "tri " << t;
        }
    }
    SWMM_Infil2DOptions opt{};
    ASSERT_EQ(swmm_infil2d_get_options(eng2, &opt), SWMM_OK);
    EXPECT_DOUBLE_EQ(opt.infil_step, 120.0) << "INFIL_STEP lost on round-trip";

    swmm_engine_close(eng2);
    swmm_engine_destroy(eng2);
}

// The external-mesh half of G7 (§5.5.5): the sections are per-cell MESH
// attributes, so they follow the mesh into the `.2dm`. After a save they must
// still live in exactly ONE file — a copy left in the `.inp` is a second source
// of truth that silently wins or loses depending on parse order.
TEST_F(Infil2DIntegrationTest, ExternalMeshKeepsTheSectionsInExactlyOneFile) {
    fs::create_directories(dir_);
    const fs::path meshf = dir_ / "g7_ext.2dm";
    const fs::path inp   = dir_ / "g7_ext.inp";
    const fs::path saved = dir_ / "g7_ext_saved.inp";

    // The sidecar carries the mesh AND its infiltration.
    {
        std::ofstream f(meshf);
        f << "[2D_VERTICES]\n"
             " 0.0    0.0   10.0\n"
             "20.0    0.0   10.0\n"
             "20.0   20.0   10.0\n"
             " 0.0   20.0   10.0\n"
             "10.0   10.0   10.0\n"
             "\n[2D_TRIANGLES]\n"
             "0     1   4   0.03        0.0         LAWN\n"
             "1     2   4   0.03        0.0         WOODS\n"
             "2     3   4   0.03        0.0         LAWN\n"
             "3     0   4   0.03        0.0         PAVED\n"
             "\n[2D_INFILTRATION_DEFAULTS]\n"
             "*        CONSTANT   1.0   -     -     -    -    LOST\n"
             "LAWN     HORTON     3.0   0.5   4.14  7.0  0    LOST\n"
             "\n[2D_INFILTRATION]\n"
             "3        CURVE_NUMBER  85    -    7.0   LOST\n";
    }
    // The .inp references it and carries no infiltration of its own.
    {
        std::ofstream f(inp);
        f << "[OPTIONS]\n"
             "FLOW_UNITS           CFS\n"
             "FLOW_ROUTING         DYNWAVE\n"
             "START_DATE           01/01/2026\n"
             "START_TIME           00:00:00\n"
             "END_DATE             01/01/2026\n"
             "END_TIME             00:10:00\n"
             "REPORT_STEP          00:05:00\n"
             "ROUTING_STEP         10\n"
             "\n[JUNCTIONS]\nJ1      0.0   1.0       0          0         0\n"
             "\n[OUTFALLS]\nO1     -0.5    FREE  NO\n"
             "\n[CONDUITS]\nC1      J1    O1  30.0    0.013      0         0          0\n"
             "\n[XSECTIONS]\nC1      CIRCULAR  0.3    0      0      0      1\n"
             "\n[2D_OPTIONS]\nMAX_TIMESTEP 2\nDRY_DEPTH 0.001\nREPORT_2D NO\n"
             "\n[2D_MESH_FILE]\nFILE " << meshf.filename().string() << "\n";
    }

    SWMM_Engine eng = swmm_engine_create();
    ASSERT_EQ(swmm_engine_open(eng, inp.string().c_str(),
                               (dir_ / "g7_ext.rpt").string().c_str(),
                               (dir_ / "g7_ext.out").string().c_str(),
                               nullptr), SWMM_OK);
    ASSERT_EQ(swmm_engine_initialize(eng), SWMM_OK);

    // The sidecar's rows reached the resolver. Tags are LAWN / WOODS / LAWN /
    // PAVED and the override row names cell 3 — ONE-BASED in the file, so it
    // lands on triangle index 2. PAVED has no row of its own, so it falls
    // through to '*'.
    const int want[4] = {SWMM_INFIL2D_HORTON,       // LAWN tag row
                         SWMM_INFIL2D_CONSTANT,     // WOODS -> '*'
                         SWMM_INFIL2D_CURVE_NUMBER, // override (file cell 3)
                         SWMM_INFIL2D_CONSTANT};    // PAVED -> '*'
    for (int t = 0; t < 4; ++t) {
        SWMM_Infil2DRow row{};
        int is_override = -1;
        ASSERT_EQ(swmm_infil2d_get_cell(eng, t, &row, &is_override), SWMM_OK);
        ASSERT_EQ(row.has_method, 1) << "tri " << t
            << " got no model from the external .2dm";
        EXPECT_EQ(row.method, want[t]) << "tri " << t;
    }

    ASSERT_EQ(swmm_model_write(eng, saved.string().c_str()), SWMM_OK);
    swmm_engine_close(eng);
    swmm_engine_destroy(eng);

    std::string text;
    { std::ifstream f(saved); std::ostringstream ss; ss << f.rdbuf(); text = ss.str(); }
    EXPECT_EQ(text.find("[2D_INFILTRATION"), std::string::npos)
        << "the saved .inp carries a copy of the infiltration sections that the "
           "external .2dm also carries — two sources of truth (§5.5.5)";
    EXPECT_NE(text.find("[2D_MESH_FILE]"), std::string::npos)
        << "the saved .inp lost its external-mesh reference, so the sections "
           "are now in NO file the engine will read";
}

// ============================================================================
// G11 — validation, at the level a user meets it.
// ============================================================================

TEST_F(Infil2DIntegrationTest, UnsupportedDestinationFailsTheRun) {
    const std::string sections =
        "\n[2D_INFILTRATION_DEFAULTS]\n"
        "*   CONSTANT   1.0   -   -   -   -   AQUIFER_2D\n";
    fs::create_directories(dir_);
    const fs::path inp = dir_ / "g11_dest.inp";
    { std::ofstream f(inp); f << buildModel(sections, "CFS"); }

    SWMM_Engine eng = swmm_engine_create();
    const int oc = swmm_engine_open(eng, inp.string().c_str(),
                                    (dir_ / "g11_dest.rpt").string().c_str(),
                                    (dir_ / "g11_dest.out").string().c_str(),
                                    nullptr);
    int ic = SWMM_OK;
    if (oc == SWMM_OK) ic = swmm_engine_initialize(eng);
    EXPECT_TRUE(oc != SWMM_OK || ic != SWMM_OK)
        << "AQUIFER_2D was accepted; D-I4 says it must be rejected until "
           "G1 step 11b";
    swmm_engine_close(eng);
    swmm_engine_destroy(eng);
}

TEST_F(Infil2DIntegrationTest, OutOfRangeCellFailsTheRun) {
    const std::string sections =
        "\n[2D_INFILTRATION]\n"
        "99   CONSTANT   1.0   -   -   -   -   LOST\n";
    fs::create_directories(dir_);
    const fs::path inp = dir_ / "g11_cell.inp";
    { std::ofstream f(inp); f << buildModel(sections, "CFS"); }

    SWMM_Engine eng = swmm_engine_create();
    const int oc = swmm_engine_open(eng, inp.string().c_str(),
                                    (dir_ / "g11_cell.rpt").string().c_str(),
                                    (dir_ / "g11_cell.out").string().c_str(),
                                    nullptr);
    int ic = SWMM_OK;
    if (oc == SWMM_OK) ic = swmm_engine_initialize(eng);
    EXPECT_TRUE(oc != SWMM_OK || ic != SWMM_OK)
        << "cell 99 on a 4-triangle mesh was accepted";
    swmm_engine_close(eng);
    swmm_engine_destroy(eng);
}

TEST_F(Infil2DIntegrationTest, UnknownMethodTokenFailsTheParse) {
    const std::string sections =
        "\n[2D_INFILTRATION_DEFAULTS]\n"
        "*   SPONGE   1.0\n";
    fs::create_directories(dir_);
    const fs::path inp = dir_ / "g11_method.inp";
    { std::ofstream f(inp); f << buildModel(sections, "CFS"); }

    SWMM_Engine eng = swmm_engine_create();
    const int oc = swmm_engine_open(eng, inp.string().c_str(),
                                    (dir_ / "g11_method.rpt").string().c_str(),
                                    (dir_ / "g11_method.out").string().c_str(),
                                    nullptr);
    int ic = SWMM_OK;
    if (oc == SWMM_OK) ic = swmm_engine_initialize(eng);
    EXPECT_TRUE(oc != SWMM_OK || ic != SWMM_OK)
        << "an unknown METHOD token was accepted";
    swmm_engine_close(eng);
    swmm_engine_destroy(eng);
}
