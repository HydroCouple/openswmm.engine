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
 * @file test_kinwave_storage_outlets.cpp
 * @brief Issue #148 — a storage unit draining through non-conduit outlets
 *        under KINWAVE routing.
 *
 * @details The defect: KW handed every non-conduit link its upstream node's
 *          entire inflow instead of the link's own head-discharge flow
 *          (legacy flowrout.c:517 getLinkInflow). Two consequences, both
 *          checked here:
 *
 *          1. Manufactured water. N outlets on one node each carried the full
 *             node inflow, so the downstream node received N x the water. The
 *             reported model showed -100.463% routing continuity with an
 *             orifice and a weir sharing a pond.
 *          2. A pond that does not pond. Because the outlet flow tracked
 *             inflow rather than depth, the unit released whatever arrived,
 *             held nothing, and attenuated nothing. With a SINGLE outlet this
 *             one is SILENT — continuity closes while the pond is inert — so
 *             a continuity gate alone would not have caught it.
 *
 *          The decisive invariant is the inert weir: WR1's crest sits at 8 ft
 *          and the pond peaks near 3.6 ft, so the weir can never flow. Adding
 *          it must change NOTHING. Under the defect it doubled the outfall
 *          volume.
 *
 *          Scratch fixtures use the `_kws_` prefix (collision-checked).
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

#include <openswmm/engine/openswmm_engine.h>

#include "core/SWMMEngine.hpp"

namespace {

openswmm::SWMMEngine& as_cpp_engine(SWMM_Engine engine) {
    return *static_cast<openswmm::SWMMEngine*>(engine);
}

// --- Pond geometry, held in one place so the assertions can quote it --------
constexpr double kPondArea   = 20000.0;  ///< ft2, constant (FUNCTIONAL 0 0 A)
constexpr double kOrifDiam   = 0.83;     ///< ft
constexpr double kOrifCd     = 0.65;
constexpr double kWeirCrest  = 8.0;      ///< ft above the pond invert
constexpr double kInflowCfs  = 10.0;
constexpr double kGravity    = 32.2;     ///< ft/s2

/// @param with_weir  add the inert TRANSVERSE weir alongside the orifice.
void write_deck(const std::string& path, bool with_weir) {
    std::ofstream f(path);
    f << "[TITLE]\nKW storage outlet regression (issue #148)\n\n"
      << "[OPTIONS]\n"
      << "FLOW_UNITS CFS\n"
      << "FLOW_ROUTING KINWAVE\n"
      << "START_DATE 01/01/2007\nSTART_TIME 00:00:00\n"
      << "END_DATE 01/01/2007\nEND_TIME 06:00:00\n"
      << "REPORT_STEP 00:05:00\nROUTING_STEP 0:00:15\n"
      << "ALLOW_PONDING NO\n\n"
      << "[JUNCTIONS]\nJ_out 90 4 0 0 0\n\n"
      << "[OUTFALLS]\nOUT 88 FREE NO\n\n"
      // Area = 0*d^0 + 20000 -> a constant 20000 ft2 pond.
      << "[STORAGE]\nSU1 100 10 0 FUNCTIONAL 0 0 " << kPondArea << " 0 0\n\n"
      << "[CONDUITS]\nCOUT J_out OUT 100 0.01 0 0 0 0\n\n"
      << "[ORIFICES]\nOR1 SU1 J_out SIDE 0 " << kOrifCd << " NO 0\n\n";
    if (with_weir)
        f << "[WEIRS]\nWR1 SU1 J_out TRANSVERSE " << kWeirCrest
          << " 3.33 NO 0 0 NO\n\n";
    f << "[XSECTIONS]\n"
      << "COUT CIRCULAR 3 0 0 0\n"
      << "OR1 CIRCULAR " << kOrifDiam << " 0 0 0\n";
    if (with_weir) f << "WR1 RECT_OPEN 2 5 0 0\n";
    f << "\n[INFLOWS]\nSU1 FLOW Qin FLOW 1.0\n\n"
      << "[TIMESERIES]\n"
      << "Qin 0:00 " << kInflowCfs << "\n"
      << "Qin 2:00 " << kInflowCfs << "\n"
      << "Qin 2:01 0\n"
      << "Qin 6:00 0\n";
}

struct PondRun {
    bool   ok = false;
    double routing_error = 0.0;  ///< fraction
    double max_volume = 0.0;     ///< ft3
    double max_depth = 0.0;      ///< ft
    double peak_orifice = 0.0;   ///< cfs
    double peak_weir = 0.0;      ///< cfs
    double peak_inflow = 0.0;    ///< cfs into the pond
    double outfall_volume = 0.0; ///< ft3 delivered to OUT
};


PondRun run_deck(const std::string& tag, bool with_weir) {
    PondRun r;
    const std::string inp = tag + ".inp";
    write_deck(inp, with_weir);

    SWMM_Engine e = swmm_engine_create();
    if (e == nullptr) { ADD_FAILURE() << "engine create"; return r; }
    if (swmm_engine_open(e, inp.c_str(), (tag + ".rpt").c_str(),
                         (tag + ".out").c_str(), nullptr) != SWMM_OK ||
        swmm_engine_initialize(e) != SWMM_OK ||
        swmm_engine_start(e, 1) != SWMM_OK) {
        ADD_FAILURE() << "open/init/start failed for " << inp;
        swmm_engine_destroy(e);
        return r;
    }

    auto& ctx = as_cpp_engine(e).context();
    const int su1 = ctx.node_names.find("SU1");
    const int out = ctx.node_names.find("OUT");
    const int or1 = ctx.link_names.find("OR1");
    const int wr1 = with_weir ? ctx.link_names.find("WR1") : -1;
    if (su1 < 0 || out < 0 || or1 < 0 || (with_weir && wr1 < 0)) {
        ADD_FAILURE() << "element lookup failed for " << inp;
        swmm_engine_destroy(e);
        return r;
    }
    const auto usu = static_cast<std::size_t>(su1);
    const auto uout = static_cast<std::size_t>(out);

    double elapsed = 0.0, prev_elapsed = 0.0;
    int guard = 0;
    do {
        if (swmm_engine_step(e, &elapsed) != SWMM_OK) {
            ADD_FAILURE() << "step failed for " << inp;
            swmm_engine_destroy(e);
            return r;
        }
        const double dt = (elapsed - prev_elapsed) * 86400.0;  // days -> sec
        prev_elapsed = elapsed;

        r.max_volume = std::max(r.max_volume, ctx.nodes.volume[usu]);
        r.max_depth  = std::max(r.max_depth,  ctx.nodes.depth[usu]);
        r.peak_inflow = std::max(r.peak_inflow, ctx.nodes.inflow[usu]);
        r.peak_orifice = std::max(
            r.peak_orifice, std::fabs(ctx.links.flow[static_cast<std::size_t>(or1)]));
        if (wr1 >= 0)
            r.peak_weir = std::max(
                r.peak_weir, std::fabs(ctx.links.flow[static_cast<std::size_t>(wr1)]));
        if (dt > 0.0) r.outfall_volume += ctx.nodes.inflow[uout] * dt;
    } while (elapsed > 0.0 && ++guard < 200000);

    swmm_engine_end(e);
    r.routing_error = ctx.mass_balance.routing_error();
    r.ok = true;
    swmm_engine_destroy(e);
    return r;
}

}  // namespace

// ---------------------------------------------------------------------------
// Gate 1 — the pond conserves mass and actually ponds.
// ---------------------------------------------------------------------------
TEST(KinwaveStorageOutlets, PondStoresAndConservesMass) {
    const PondRun r = run_deck("_kws_orif_weir", /*with_weir=*/true);
    ASSERT_TRUE(r.ok);

    EXPECT_LT(std::fabs(r.routing_error), 0.01)
        << "flow routing continuity error = " << (r.routing_error * 100.0)
        << "% (issue #148 reported -100.463%)";

    // A pond fed 10 cfs for 2 h over 20000 ft2 must hold water. Under the
    // defect this stayed exactly 0 for the whole run.
    EXPECT_GT(r.max_volume, 0.0) << "storage unit never accumulated volume";
    EXPECT_GT(r.max_depth, 1.0)
        << "pond peaked at " << r.max_depth << " ft; expected several feet";
}

// ---------------------------------------------------------------------------
// Gate 2 — the outlet cannot pass more than its geometry allows.
// ---------------------------------------------------------------------------
TEST(KinwaveStorageOutlets, OrificeDischargeIsPhysicallyAchievable) {
    const PondRun r = run_deck("_kws_orif_weir", /*with_weir=*/true);
    ASSERT_TRUE(r.ok);

    // Q = Cd*A*sqrt(2*g*h), evaluated at the deepest the pond ever got. This
    // is the ceiling the issue computed by hand: 34.99 cfs would have needed
    // ~154 ft of head on a 0.83 ft orifice.
    const double area = M_PI * kOrifDiam * kOrifDiam / 4.0;
    const double q_ceiling = kOrifCd * area * std::sqrt(2.0 * kGravity * r.max_depth);

    EXPECT_LE(r.peak_orifice, q_ceiling * 1.05)
        << "orifice passed " << r.peak_orifice << " cfs at a peak head of "
        << r.max_depth << " ft; the orifice equation caps it at " << q_ceiling;

    // Attenuation is the whole point of a detention pond.
    EXPECT_LT(r.peak_orifice, r.peak_inflow)
        << "peak release " << r.peak_orifice << " cfs vs peak inflow "
        << r.peak_inflow << " cfs — the pond attenuated nothing";
}

// ---------------------------------------------------------------------------
// Gate 3 — an outlet that cannot flow contributes nothing.
//
// This is the doubling bug stated as an invariant, and it is the one that
// separates "outlet flow comes from head" from "outlet flow comes from node
// inflow". WR1's crest is at 8 ft; the pond peaks near 3.6 ft.
// ---------------------------------------------------------------------------
TEST(KinwaveStorageOutlets, InertWeirDoesNotManufactureFlow) {
    const PondRun with = run_deck("_kws_orif_weir", /*with_weir=*/true);
    const PondRun without = run_deck("_kws_orif_only", /*with_weir=*/false);
    ASSERT_TRUE(with.ok);
    ASSERT_TRUE(without.ok);

    ASSERT_LT(with.max_depth, kWeirCrest)
        << "fixture invalid: pond reached the weir crest, so the weir is not inert";

    EXPECT_DOUBLE_EQ(with.peak_weir, 0.0)
        << "weir crest is " << kWeirCrest << " ft above a pond that peaked at "
        << with.max_depth << " ft, yet it carried " << with.peak_weir << " cfs";

    // Identical outfall delivery with and without the inert weir. The defect
    // put the pond's full inflow through BOTH outlets, roughly doubling this.
    EXPECT_NEAR(with.outfall_volume, without.outfall_volume,
                without.outfall_volume * 1e-6)
        << "adding an inert weir changed the delivered volume from "
        << without.outfall_volume << " to " << with.outfall_volume << " ft3";
}
