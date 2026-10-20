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
 * @file test_2d_coupling_ponds.cpp
 * @brief Coupled-junction ponds under dynamic wave (COUPLING_POND_MODE,
 *        COUPLING_RIM_BAND) and the head-equalisation cap (COUPLING_EQUALIZE):
 *        conservation in every mode, TRACKING moving the pond area between
 *        the chamber and the wet footprint, and fewer drain/spill reversals
 *        on a fine mesh.
 *
 * @details plans/1D2D_FINE_SCALE_COUPLING_PLAN_2026-10-19.md §4. A 1 m
 *          triangle mesh over a 1.2 m manhole (the fine-mesh case the plan
 *          targets): a pulsed sewer inflow surcharges J1, which spills onto
 *          the patch and recaptures it. Inputs, reports and a per-run summary
 *          land in ./coupling_ponds_out/ for review.
 *
 * @ingroup engine_2d
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <openswmm/engine/openswmm_2d.h>
#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_massbalance.h>
#include <openswmm/engine/openswmm_model.h>
#include <openswmm/engine/openswmm_nodes.h>

#include "core/SWMMEngine.hpp"
#include "core/SimulationContext.hpp"
#include "platform_test_support.hpp"

namespace fs = std::filesystem;

namespace {

const fs::path kOut = "coupling_ponds_out";

// 10 × 10 m patch of kN × kN squares (two triangles each), J1 at the centre
// vertex: kN = 10 is the fine 1 m mesh, kN = 2 a coarse one whose wet
// footprint (6 × 12.5 m² / 3 = 25 m²) dwarfs the 1.13 m² manhole.
std::string deck(const std::string& opts2d, int kN = 10) {
    const double h = 10.0 / kN;
    std::ostringstream m;
    m << "[OPTIONS]\nFLOW_UNITS CMS\nFLOW_ROUTING DYNWAVE\nSTART_DATE 01/01/2026\n"
         "START_TIME 00:00:00\nEND_DATE 01/01/2026\nEND_TIME 00:40:00\nREPORT_STEP 00:01:00\n"
         "ROUTING_STEP 2\nALLOW_PONDING NO\n\n"
         "[JUNCTIONS]\nJ1 0.0 1.0 0 0 0\n\n[OUTFALLS]\nO1 -0.5 FREE NO\n\n"
         "[CONDUITS]\nC1 J1 O1 30.0 0.013 0 0 0\n\n[XSECTIONS]\nC1 CIRCULAR 0.2 0 0 0 1\n\n"
         "[INFLOWS]\nJ1 FLOW IN_TS FLOW 1.0 1.0\n\n"
         "[TIMESERIES]\n"
         "IN_TS 0:00 0.0\nIN_TS 0:02 0.09\nIN_TS 0:08 0.09\nIN_TS 0:09 0.0\n"
         "IN_TS 0:14 0.0\nIN_TS 0:15 0.09\nIN_TS 0:20 0.09\nIN_TS 0:21 0.0\nIN_TS 0:40 0.0\n\n"
         "[2D_OPTIONS]\nMAX_TIMESTEP 1\nDRY_DEPTH 0.002\nREPORT_2D NO\n" << opts2d << "\n"
         "[2D_VERTICES]\n";
    for (int j = 0; j <= kN; ++j)
        for (int i = 0; i <= kN; ++i) m << i * h << " " << j * h << " 1.0\n";
    m << "\n[2D_TRIANGLES]\n";
    auto v = [kN](int i, int j) { return j * (kN + 1) + i; };
    for (int j = 0; j < kN; ++j)
        for (int i = 0; i < kN; ++i) {
            m << v(i, j) << " " << v(i + 1, j) << " " << v(i + 1, j + 1) << " 0.03\n";
            m << v(i, j) << " " << v(i + 1, j + 1) << " " << v(i, j + 1) << " 0.03\n";
        }
    m << "\n[2D_VERTEX_NODE_MAP]\n" << v(kN / 2, kN / 2) << " J1\n";
    return m.str();
}

struct PondRun {
    bool ok = false;
    int reversals = 0;            ///< sign changes of J1's coupling inflow
    double peak_depth = 0.0;      ///< J1, m
    double max_pond_area = 0.0;   ///< J1 coupled pond area A_eq, m² (pond modes)
    double first_pond_area = 0.0;
    double spilled = 0.0, drained = 0.0, cont_2d = 0.0, cont_routing = 0.0;
    std::string backend;
};

PondRun run(const std::string& tag, const std::string& opts2d, int kN = 10) {
    PondRun r;
    fs::create_directories(kOut);
    const fs::path inp = kOut / (tag + ".inp");
    { std::ofstream f(inp); f << deck(opts2d, kN); }
    SWMM_Engine e = swmm_engine_create();
    if (swmm_engine_open(e, inp.string().c_str(), (kOut / (tag + ".rpt")).string().c_str(),
                         "", nullptr) != SWMM_OK ||
        swmm_engine_initialize(e) != SWMM_OK || swmm_engine_start(e, 0) != SWMM_OK) {
        swmm_engine_destroy(e);
        return r;
    }
    const auto& ctx = static_cast<openswmm::SWMMEngine*>(e)->context();
    const int j1 = swmm_node_index(e, "J1");
    SWMM_2DRunStats stats{};
    if (swmm_2d_get_run_stats(e, &stats) == SWMM_OK) r.backend = stats.backend;
    constexpr double kFt2ToM2 = 0.3048 * 0.3048;
    if (static_cast<std::size_t>(j1) < ctx.coupled_pond_area.size())
        r.first_pond_area = ctx.coupled_pond_area[j1] * kFt2ToM2;
    double elapsed = 0.0;
    int last_sign = 0;
    for (;;) {
        if (swmm_engine_step(e, &elapsed) != SWMM_OK || elapsed <= 0.0) break;
        double depth = 0.0;
        swmm_node_get_depth(e, j1, &depth);
        r.peak_depth = std::max(r.peak_depth, depth);
        const double q = ctx.nodes.coupling_inflow[static_cast<std::size_t>(j1)];
        const int sign = q > 1e-6 ? 1 : (q < -1e-6 ? -1 : 0);
        if (sign != 0) {
            if (last_sign != 0 && sign != last_sign) ++r.reversals;
            last_sign = sign;
        }
        if (static_cast<std::size_t>(j1) < ctx.coupled_pond_area.size())
            r.max_pond_area = std::max(r.max_pond_area, ctx.coupled_pond_area[j1] * kFt2ToM2);
    }
    swmm_engine_end(e);
    swmm_2d_get_mass_balance(e, nullptr, nullptr, nullptr, &r.spilled, &r.drained,
                             nullptr, nullptr, nullptr, nullptr, nullptr);
    swmm_2d_get_continuity_error(e, &r.cont_2d);
    swmm_get_routing_continuity_error(e, &r.cont_routing);
    swmm_engine_report(e);
    swmm_engine_close(e);
    swmm_engine_destroy(e);
    std::ofstream s(kOut / (tag + "_pond_summary.csv"));
    s << "metric,value\nreversals," << r.reversals << "\npeak_depth_m," << r.peak_depth
      << "\nfirst_pond_area_m2," << r.first_pond_area << "\nmax_pond_area_m2," << r.max_pond_area
      << "\nspilled_1d_to_2d_m3," << r.spilled << "\ndrained_2d_to_1d_m3," << r.drained
      << "\ncontinuity_2d," << r.cont_2d << "\ncontinuity_routing," << r.cont_routing << "\n";
    r.ok = true;
    return r;
}

} // namespace

// Every pond mode spills the surcharge onto the patch and recaptures it with
// the 2D ledger closed and the 1D ledger no worse than the legacy step.
TEST(CouplingPonds, EveryModeConserves) {
    const PondRun legacy = run("legacy", "");
    ASSERT_TRUE(legacy.ok);
    ASSERT_GT(legacy.spilled, 1.0) << "fixture never spilled";
    for (const char* mode : {"FOOTPRINT\nCOUPLING_RIM_BAND 0.05", "CHAMBER", "TRACKING"}) {
        const std::string tag = std::string("mode_") + (mode[0] == 'F' ? "footprint_band" : mode);
        const PondRun r = run(tag, std::string("COUPLING_LAW WEIR_ORIFICE\nCOUPLING_POND_MODE ") + mode + "\n");
        ASSERT_TRUE(r.ok) << tag;
        EXPECT_GT(r.spilled, 1.0) << tag;
        EXPECT_GT(r.drained, 0.5 * r.spilled) << tag << ": the patch was not recaptured";
        EXPECT_LT(std::fabs(r.cont_2d), 0.005) << tag;
        EXPECT_LT(std::fabs(r.cont_routing), std::max(0.02, 1.5 * std::fabs(legacy.cont_routing))) << tag;
    }
}

// TRACKING starts the pond at the chamber (the 1.2 m manhole, 1.13 m²) and,
// once the coarse patch around J1 is wet, relaxes it toward the wet footprint
// (25 m²); CHAMBER stays at the chamber. On the fine mesh the footprint
// (1 m²) is below the chamber, so TRACKING holds at its floor there.
TEST(CouplingPonds, TrackingPondFollowsTheWetFootprintAboveTheChamber) {
    const double chamber_m2 = 3.14159265358979 * 1.44 / 4.0;
    const PondRun r = run("tracking_coarse", "COUPLING_LAW WEIR_ORIFICE\nCOUPLING_POND_MODE TRACKING\n"
                                             "COUPLING_POND_RELAX 10\n", 2);
    ASSERT_TRUE(r.ok);
    EXPECT_NEAR(r.first_pond_area, chamber_m2, 1e-9) << "starts at the chamber";
    EXPECT_GT(r.max_pond_area, 0.5 * 25.0) << "never followed the wet footprint";
    EXPECT_LE(r.max_pond_area, 25.0 + 1e-9);
    EXPECT_LT(std::fabs(r.cont_2d), 0.005);
    const PondRun chamber = run("chamber_coarse", "COUPLING_LAW WEIR_ORIFICE\nCOUPLING_POND_MODE CHAMBER\n", 2);
    ASSERT_TRUE(chamber.ok);
    EXPECT_EQ(chamber.max_pond_area, chamber.first_pond_area) << "CHAMBER is fixed";
    const PondRun fine = run("tracking_fine", "COUPLING_LAW WEIR_ORIFICE\nCOUPLING_POND_MODE TRACKING\n");
    ASSERT_TRUE(fine.ok);
    EXPECT_NEAR(fine.max_pond_area, chamber_m2, 1e-9) << "floored at the chamber";
}

// On the fine mesh the legacy orifice (AREA 1 m², footprint pond, no cap)
// flips between draining and spilling; the regime-aware law with the
// equalisation cap and a tracking pond flips less.
TEST(CouplingPonds, EqualisationReducesDrainSpillReversals) {
    const PondRun legacy = run("churn_legacy", "");
    const PondRun law = run("churn_weir_only", "COUPLING_LAW WEIR_ORIFICE\nCOUPLING_EQUALIZE NO\n");
    const PondRun fine = run("churn_weir_tracking_equalize",
                             "COUPLING_LAW WEIR_ORIFICE\nCOUPLING_POND_MODE TRACKING\nCOUPLING_EQUALIZE YES\n");
    // Attribution: each half on its own (recorded, not gated).
    const PondRun pond_only = run("churn_weir_tracking", "COUPLING_LAW WEIR_ORIFICE\n"
                                  "COUPLING_POND_MODE TRACKING\nCOUPLING_EQUALIZE NO\n");
    const PondRun cap_only = run("churn_weir_equalize", "COUPLING_LAW WEIR_ORIFICE\nCOUPLING_EQUALIZE YES\n");
    const PondRun legacy_cap = run("churn_orifice_equalize", "COUPLING_EQUALIZE YES\n");
    ASSERT_TRUE(legacy.ok && law.ok && fine.ok && pond_only.ok && cap_only.ok && legacy_cap.ok);
    std::printf("reversals: legacy %d, legacy+EQUALIZE %d, WEIR_ORIFICE alone %d, +TRACKING %d, "
                "+EQUALIZE %d, +TRACKING+EQUALIZE %d\n", legacy.reversals, legacy_cap.reversals,
                law.reversals, pond_only.reversals, cap_only.reversals, fine.reversals);
    EXPECT_LT(fine.reversals, legacy.reversals / 10) << "the fine-mesh churn did not settle";
    EXPECT_LE(fine.reversals, law.reversals);
}

// The Kokkos marcher applies the same equalisation cap on the fine mesh,
// where it binds on nearly every exchange: the OpenMP plugin agrees with the
// CPU marcher. Skipped where the plugin is not built or not found (set
// OPENSWMM_GPU_PLUGIN_PATH to the plugin directory).
TEST(CouplingPonds, KokkosCapMatchesCpu) {
    const std::string opts = "COUPLING_LAW WEIR_ORIFICE\nCOUPLING_POND_MODE TRACKING\nCOUPLING_EQUALIZE YES\n";
    plattest::setEnvVar("OPENSWMM_2D_BACKEND", "cpu");
    const PondRun cpu = run("backend_cpu", opts);
    plattest::setEnvVar("OPENSWMM_2D_BACKEND", "omp");
    const PondRun omp = run("backend_omp", opts);
    plattest::setEnvVar("OPENSWMM_2D_BACKEND", "cpu");
    ASSERT_TRUE(cpu.ok && omp.ok);
    if (omp.backend.rfind("omp", 0) != 0)
        GTEST_SKIP() << "omp plugin not loaded (backend: " << omp.backend << ")";
    EXPECT_EQ(omp.reversals, cpu.reversals);
    EXPECT_NEAR(omp.spilled, cpu.spilled, 1e-6 * cpu.spilled);
    EXPECT_NEAR(omp.drained, cpu.drained, 1e-6 * cpu.drained);
    EXPECT_NEAR(omp.peak_depth, cpu.peak_depth, 1e-9);
}
