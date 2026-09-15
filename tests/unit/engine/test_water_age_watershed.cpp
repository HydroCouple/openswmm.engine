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
 * @file test_water_age_watershed.cpp
 * @brief Phase A3 gates — water age on subcatchment surfaces.
 *
 * @details Every gate here has a reachability problem to solve first
 *          (lesson 59): the code under test only runs on a deck that
 *          actually produces runoff, and the run-on path only runs on a
 *          deck where one subcatchment drains into another. So each gate
 *          asserts its SETUP before its result, and the file comment names
 *          what each deck must produce.
 *
 *          The gate that fails on today's `main` is
 *          `RunonCarriesTheDonorsAge`. Before A3 the FLOW path added
 *          `q_runon` while the age path added nothing, so cascade water
 *          reached the node with no upstream age attached.
 *
 * @see plans/transport/WATER_AGE_TRACKING_PLAN.md §3, §7 A3
 * @see plans/transport/A3_SCOPING_2026-08-17.md
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
#include <vector>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_output.h>

#include "core/SWMMEngine.hpp"

namespace {

openswmm::SWMMEngine& as_cpp_engine(SWMM_Engine engine) {
    return *static_cast<openswmm::SWMMEngine*>(engine);
}

constexpr int kNSub = static_cast<int>(openswmm::SubArea::COUNT_);

void write_file(const char* path, const std::string& body) {
    std::ofstream f(path);
    f << body;
}

/// An INTENSITY gage on a 5-minute interval reads one value per interval, so
/// a series with entries only at 00:00 and 01:00 rains for FIVE MINUTES and
/// then recedes for fifty-five. That matters more than it looks: the mixing
/// term is exercised only while water is ARRIVING, so a five-minute storm
/// leaves every gate watching a draining surface. One entry per interval.
std::string rain_series() {
    std::string s;
    char buf[64];
    for (int m = 0; m <= 65; m += 5) {
        std::snprintf(buf, sizeof(buf), "rain_ts 01/01/2026 %02d:%02d 2.0\n",
                      m / 60, m % 60);
        s += buf;
    }
    return s + "\n";
}

/// `cascade` makes S1 drain into S2 (run-on) instead of straight to the node
/// — the only configuration in which the run-on age path executes at all.
void write_deck(const char* path, const std::string& pc_lines,
                bool cascade = false) {
    std::ofstream f(path);
    f << "[TITLE]\nA3 watershed age gate deck\n\n[OPTIONS]\n"
      << "FLOW_UNITS CFS\nFLOW_ROUTING DYNWAVE\nWATER_AGE ON\n"
      << "INFILTRATION HORTON\n"
      << "START_DATE 01/01/2026\nSTART_TIME 00:00:00\n"
      << "END_DATE 01/01/2026\nEND_TIME 01:00:00\n"
      << "WET_STEP 00:01:00\nDRY_STEP 00:05:00\n"
      << "ROUTING_STEP 10\nREPORT_STEP 00:05:00\n\n"
      << "[RAINGAGES]\nRG1 INTENSITY 0:05 1.0 TIMESERIES rain_ts\n\n"
      << "[TIMESERIES]\n" << rain_series()
      // Two subcatchments. S1 drains to S2 when cascading, else to J1.
      << "[SUBCATCHMENTS]\n"
      << "S1 RG1 " << (cascade ? "S2" : "J1")
      << " 5 50 500 0.5 0\nS2 RG1 J1 5 50 500 0.5 0\n\n"
      << "[SUBAREAS]\n"
      // N-Imperv N-Perv S-Imperv S-Perv PctZero RouteTo
      << "S1 0.01 0.1 0.05 0.10 25 OUTLET\n"
      << "S2 0.01 0.1 0.05 0.10 25 OUTLET\n\n"
      << "[INFILTRATION]\n"
      << "S1 3.0 0.5 4 7 0\nS2 3.0 0.5 4 7 0\n\n"
      << "[JUNCTIONS]\nJ1 10.0 10 0 0 0\n\n"
      << "[OUTFALLS]\nOUT 9.0 FREE  NO\n\n"
      << "[CONDUITS]\nC1 J1 OUT 400 0.013 0 0 0\n\n"
      << "[XSECTIONS]\nC1 CIRCULAR 3.0 0 0 0\n\n";
    if (!pc_lines.empty())
        f << "[PROCESS_COMPONENTS]\n" << pc_lines << "\n\n";
    f << "[REPORT]\nINPUT NO\n";
}

std::string age_cfg(double rain_h) {
    return "[WATER_AGE_SOURCES]\nRAINFALL GLOBAL " +
           std::to_string(rain_h) + "\n";
}

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
// Gate 1 — the state exists, is per-SUBAREA, and ages.
// ---------------------------------------------------------------------------
TEST(WaterAgeWatershedTest, SubareaAgeStateIsSizedAndAdvances) {
    write_file("_a3.age", age_cfg(0.0));
    write_deck("_a3.inp", "org.hydrocouple.openswmm.waterage "
                          "config=\"_a3.age\"");
    SWMM_Engine e = run_and_hold("_a3.inp", "_a3.rpt", "_a3.out");
    ASSERT_NE(e, nullptr);
    const auto& ws = as_cpp_engine(e).context().water_age_state;
    const int nsc = as_cpp_engine(e).context().n_subcatches();

    ASSERT_EQ(nsc, 2) << "deck did not build two subcatchments";
    // Per-SUBAREA, not per-subcatchment: three rows each (user decision).
    ASSERT_EQ(static_cast<int>(ws.subarea_age.size()), nsc * kNSub)
        << "subarea_age is sized " << ws.subarea_age.size() << ", expected "
        << (nsc * kNSub) << " — per-subarea state was not allocated";
    ASSERT_EQ(static_cast<int>(ws.subcatch_runoff_age.size()), nsc);

    // SETUP (lesson 59/36): the deck must actually have produced runoff, or
    // every age below is the untouched zero of a surface nothing landed on.
    const auto& sc = as_cpp_engine(e).context().subcatches;
    bool any_runoff = false;
    for (int i = 0; i < nsc; ++i)
        if (sc.stat_runoff_vol[static_cast<std::size_t>(i)] > 0.0)
            any_runoff = true;
    ASSERT_TRUE(any_runoff)
        << "no subcatchment produced runoff — the rainfall deck is not "
           "exercising the code under test";

    bool any_aged = false;
    for (double a : ws.subarea_age)
        if (a > 0.0) any_aged = true;
    EXPECT_TRUE(any_aged)
        << "every subarea age is still 0 after an hour of rain — the "
           "watershed update never ran";
    swmm_engine_destroy(e);
}

// ---------------------------------------------------------------------------
// Gate 2 — run-on carries the donor's age. THIS IS THE GATE THAT FAILS
// BEFORE A3.
// ---------------------------------------------------------------------------
TEST(WaterAgeWatershedTest, RunonCarriesTheDonorsAge) {
    // Rainfall enters at a distinctive non-zero age so "carried" and
    // "invented locally" produce different numbers (lesson 26).
    constexpr double kRainH = 3.0;
    write_file("_a3c.age", age_cfg(kRainH));
    write_deck("_a3c.inp", "org.hydrocouple.openswmm.waterage "
                           "config=\"_a3c.age\"", /*cascade=*/true);
    SWMM_Engine e = run_and_hold("_a3c.inp", "_a3c.rpt", "_a3c.out");
    ASSERT_NE(e, nullptr);
    const auto& ctx = as_cpp_engine(e).context();
    const auto& ws  = ctx.water_age_state;

    // SETUP: S1 must actually be draining into S2, or the run-on path never
    // executes and this gate proves nothing (lesson 59 — the deck must
    // reach the branch).
    ASSERT_GE(ctx.n_subcatches(), 2);
    ASSERT_EQ(ctx.subcatches.outlet_subcatch[0], 1)
        << "S1 does not drain to S2 — the cascade deck did not build, so the "
           "run-on age path was never entered";
    ASSERT_GT(ctx.subcatches.stat_runoff_vol[0], 0.0)
        << "S1 produced no runoff, so there was no run-on to carry age";

    // The receiving subcatchment's runoff must be at least as old as the
    // rain, because it is a mix of rain and water that already aged on S1.
    const double a2_h = ws.subcatch_runoff_age[1] / 3600.0;
    EXPECT_GT(a2_h, kRainH)
        << "S2 sheds water at " << a2_h << " h against " << kRainH
        << " h of rain. Equal to the rainfall age means run-on arrived with "
           "NO age attached — the defect A3 exists to fix, where the flow "
           "path adds q_runon and the age path adds nothing.";

    // And the NODE must receive that age. Everything above reads the
    // watershed array directly, so it is satisfied whether or not the
    // wet-weather loader was ever changed — reverting the loader to the
    // configured RAINFALL source age leaves every assertion so far intact
    // and simply hands the network 3 h water. §4.2 is only tested here.
    // S2 is the only subcatchment discharging to J1 (S1 drains into S2), so
    // the node should be holding what S2 shed, not what fell from the sky.
    // Comparing the two directly beats an offset from kRainH: the gap this
    // has to resolve is a quarter of an hour, and the agreement is four
    // ten-thousandths of one.
    ASSERT_FALSE(ws.node_age.empty());
    const double j1_h = ws.node_age[0] / 3600.0;
    EXPECT_NEAR(j1_h, a2_h, 0.05)
        << "J1 receives water at " << j1_h << " h while S2 sheds at " << a2_h
        << " h. A node sitting at the " << kRainH << " h RAINFALL source age "
           "means the wet-weather loader is still delivering the age water "
           "arrives from the SKY with, not the age it leaves the surface "
           "with — which every assertion above is blind to, because they all "
           "read the watershed array rather than the network.";
    swmm_engine_destroy(e);
}

// ---------------------------------------------------------------------------
// Gate 3 — the published runoff age is VOLUME-weighted across subareas.
// ---------------------------------------------------------------------------
TEST(WaterAgeWatershedTest, RunoffAgeIsVolumeWeightedNotAreaWeighted) {
    write_file("_a3w.age", age_cfg(1.0));
    write_deck("_a3w.inp", "org.hydrocouple.openswmm.waterage "
                           "config=\"_a3w.age\"");
    SWMM_Engine e = run_and_hold("_a3w.inp", "_a3w.rpt", "_a3w.out");
    ASSERT_NE(e, nullptr);
    const auto& ws = as_cpp_engine(e).context().water_age_state;

    // Recompute the volume-weighted mean from the state itself and compare.
    // A dry subarea holds no water and must contribute NOTHING; an
    // area-weighted or unweighted mean would let it drag the result.
    const auto base = static_cast<std::size_t>(0);
    double num = 0.0, den = 0.0;
    for (int k = 0; k < kNSub; ++k) {
        const auto idx = base * kNSub + static_cast<std::size_t>(k);
        const double v = ws.subarea_vol_prev[idx];
        if (v > 1.0e-12) { num += ws.subarea_age[idx] * v; den += v; }
    }
    ASSERT_GT(den, 0.0)
        << "no subarea on S1 holds water at the end of the run, so the "
           "weighting cannot be observed — lengthen the storm";

    EXPECT_NEAR(ws.subcatch_runoff_age[0], num / den, 1.0e-6)
        << "published runoff age " << ws.subcatch_runoff_age[0]
        << " s differs from the volume-weighted mean " << (num / den)
        << " s of the subareas that actually hold water.";
    swmm_engine_destroy(e);
}

// ---------------------------------------------------------------------------
// Gate 4 — the watershed rows are sized and stay sized.
//
// `WaterAgeState::resize` gained a third parameter, which is deliberately
// NOT defaulted. It was, and this gate was written to watch for a call site
// left on the two-argument form — but measurement showed that observer
// cannot work: every one of the five sites is guarded by a size mismatch,
// so a wipe can only land before any age has accumulated, and
// routeSubcatchmentAge re-sizes at the top of its next call. Reverting each
// site in turn (including ArdEngine::init, the only one that runs after a
// runoff step) left the published ages BIT-IDENTICAL.
//
// So the trap was removed instead of watched: without a default, a
// two-argument call is a compile error, which is the only observer that can
// see a defect that repairs itself at runtime. What remains here is a cheap
// invariant, not a falsifier — recorded as such rather than counted as
// coverage.
// ---------------------------------------------------------------------------
TEST(WaterAgeWatershedTest, ResizeKeepsTheWatershedRows) {
    write_file("_a3r.age", age_cfg(0.5));
    write_deck("_a3r.inp", "org.hydrocouple.openswmm.waterage "
                           "config=\"_a3r.age\"");
    SWMM_Engine e = run_and_hold("_a3r.inp", "_a3r.rpt", "_a3r.out");
    ASSERT_NE(e, nullptr);
    const auto& ctx = as_cpp_engine(e).context();
    const auto& ws  = ctx.water_age_state;

    // The run has stepped the loaders (which re-size on mismatch), the
    // legacy mirror and possibly the ARD init. If any of them used the
    // 2-argument form, these arrays are now empty.
    EXPECT_EQ(static_cast<int>(ws.subarea_age.size()),
              ctx.n_subcatches() * kNSub)
        << "subarea_age is not sized to n_subcatch * 3 at the end of the "
           "run — some path re-sized WaterAgeState without its subcatchment "
           "count";
    EXPECT_EQ(static_cast<int>(ws.subcatch_runoff_age.size()),
              ctx.n_subcatches());
    swmm_engine_destroy(e);
}

// ---------------------------------------------------------------------------
// Gate 5 — the age of shed water is its RESIDENCE TIME, against an
// analytic reference.
//
// Every gate above is a comparison against the implementation's own state:
// gate 3 recomputes the weighted mean from the same two arrays the code
// stored, and gates 1 and 2 are directional. None of them constrains the
// MAGNITUDE, so none can tell a residence time from an elapsed time — and
// those are the two things the mixing term chooses between.
//
// A 100 % impervious subarea with zero depression storage under steady rain
// is a complete-mix tank at steady state: inflow = outflow = Q, storage = V,
// and the mean age of the water leaving is exactly V/Q. Both V and Q are
// published, so the reference is computed from the run rather than hard
// coded, and the assertion is on the ratio.
//
// This is what distinguishes the two candidate mixing volumes. Measured on
// this deck: V/Q = 0.14062 h; mixing on the GROSS inflow gives 0.14022 h,
// mixing on the net gain (v_new - v_old, which is ~0 while a surface sheds
// as fast as it fills) gives 0.88592 h — 6.3x too old, because with no net
// gain no rain is ever admitted and the age degenerates into the time since
// the surface first wetted.
// ---------------------------------------------------------------------------
TEST(WaterAgeWatershedTest, ShedAgeIsTheResidenceTimeUnderSteadyRain) {
    write_file("_a3t.age", age_cfg(0.0));
    // One subcatchment, wholly impervious, PctZero 100 so every impervious
    // square foot is IMPERV0 (dStore = 0) — a through-flowing sheet with no
    // depression storage to hold water back.
    {
        std::ofstream f("_a3t.inp");
        f << "[TITLE]\nA3 residence-time gate deck\n\n[OPTIONS]\n"
          << "FLOW_UNITS CFS\nFLOW_ROUTING DYNWAVE\nWATER_AGE ON\n"
          << "INFILTRATION HORTON\n"
          << "START_DATE 01/01/2026\nSTART_TIME 00:00:00\n"
          << "END_DATE 01/01/2026\nEND_TIME 01:00:00\n"
          << "WET_STEP 00:01:00\nDRY_STEP 00:05:00\n"
          << "ROUTING_STEP 10\nREPORT_STEP 00:05:00\n\n"
          << "[RAINGAGES]\nRG1 INTENSITY 0:05 1.0 TIMESERIES rain_ts\n\n"
          << "[TIMESERIES]\n" << rain_series()
          // PctZero 50, not 100: both impervious subareas get dStore 0, so
          // they are identical thin sheets and V/Q is still the mean
          // residence — but each carries frac 0.5, so a mixing volume that
          // forgets the subarea fraction is no longer inert here. At
          // PctZero 100 f0 is exactly 1 and that mistake is invisible.
          << "[SUBCATCHMENTS]\nS1 RG1 J1 5 100 500 0.5 0\n\n"
          << "[SUBAREAS]\nS1 0.01 0.1 0.0 0.0 50 OUTLET\n\n"
          << "[INFILTRATION]\nS1 3.0 0.5 4 7 0\n\n"
          << "[JUNCTIONS]\nJ1 10.0 10 0 0 0\n\n"
          << "[OUTFALLS]\nOUT 9.0 FREE  NO\n\n"
          << "[CONDUITS]\nC1 J1 OUT 400 0.013 0 0 0\n\n"
          << "[XSECTIONS]\nC1 CIRCULAR 3.0 0 0 0\n\n"
          << "[PROCESS_COMPONENTS]\n"
          << "org.hydrocouple.openswmm.waterage config=\"_a3t.age\"\n\n"
          << "[REPORT]\nINPUT NO\n";
    }
    SWMM_Engine e = run_and_hold("_a3t.inp", "_a3t.rpt", "_a3t.out");
    ASSERT_NE(e, nullptr);
    const auto& ctx = as_cpp_engine(e).context();
    const auto& ws  = ctx.water_age_state;

    ASSERT_GE(ctx.n_subcatches(), 1);
    const double q = ctx.subcatches.runoff[0];              // cfs
    double v = 0.0;                                        // ft3, all subareas
    for (int k = 0; k < kNSub; ++k)
        v += ws.subarea_vol_prev[static_cast<std::size_t>(k)];

    // SETUP: the surface must be at steady state, shedding what it receives,
    // or V/Q is not the mean age of anything. Rain must still be falling at
    // the final step (a recession has no inflow, and the net-gain and
    // gross-inflow rules agree there — which is exactly why a deck that
    // rains for five of its sixty minutes cannot tell them apart).
    ASSERT_GT(ctx.subcatches.rainfall[0], 0.0)
        << "the storm had already ended at the final step — this gate needs "
           "the surface RECEIVING water, not draining";
    ASSERT_GT(q, 0.0) << "no runoff at the final step";
    ASSERT_GT(v, 0.0) << "no water stored on the surface at the final step";

    const double residence_h = (v / q) / 3600.0;
    const double reported_h  = ws.subcatch_runoff_age[0] / 3600.0;
    EXPECT_NEAR(reported_h / residence_h, 1.0, 0.05)
        << "S1 sheds water at " << reported_h << " h against a residence "
           "time V/Q of " << residence_h << " h (V = " << v << " ft3, Q = "
        << q << " cfs) — a ratio of " << (reported_h / residence_h)
        << ". A ratio far above 1 means the mixing volume is not admitting "
           "the rain falling through the surface, so the age is measuring "
           "time since the storm began rather than time on the surface.";
    swmm_engine_destroy(e);
}

// ---------------------------------------------------------------------------
// Gate 6 — the cascade's analytical mixing, which is the plan's own A3
// verify criterion (WATER_AGE_TRACKING_PLAN.md §7: "two-subcatchment cascade
// analytical mixing").
//
// Gate 2 only establishes a DIRECTION — that S2 sheds older than the rain —
// which is satisfied by any donor age at all, including S2's own. So a
// donor/receiver mix-up in addRunonAge survives it. This gate brackets the
// magnitude instead, with both bounds computed from published state:
//
//   lower = the RAINFALL source age. S2's inflow is rain at that age plus
//           run-on at S1's shed age, and complete-mix cannot produce water
//           younger than either.
//   upper = S1's shed age plus ONE full turnover of S2's surface, V/Q.
//           S1 sheds older than the rain (it has no run-on of its own), so
//           the flow-weighted arrival at S2 is at most S1's age; a
//           complete-mix tank's mean residence is exactly V/Q, so the
//           departing age cannot exceed arrival plus a turnover.
//
// The bracket is what makes this a magnitude claim without inventing a
// tolerance: both ends are physical limits, not fitted numbers. Run-on
// carrying the RECEIVER's age instead of the donor's feeds S2 its own
// output and walks it past the upper bound.
// ---------------------------------------------------------------------------
TEST(WaterAgeWatershedTest, CascadeShedAgeSitsInsideItsAnalyticBracket) {
    constexpr double kRainH = 3.0;
    write_file("_a3b.age", age_cfg(kRainH));
    write_deck("_a3b.inp", "org.hydrocouple.openswmm.waterage "
                           "config=\"_a3b.age\"", /*cascade=*/true);
    SWMM_Engine e = run_and_hold("_a3b.inp", "_a3b.rpt", "_a3b.out");
    ASSERT_NE(e, nullptr);
    const auto& ctx = as_cpp_engine(e).context();
    const auto& ws  = ctx.water_age_state;
    const auto& sc  = ctx.subcatches;

    ASSERT_GE(ctx.n_subcatches(), 2);
    ASSERT_EQ(sc.outlet_subcatch[0], 1) << "the cascade deck did not build";
    ASSERT_GT(sc.rainfall[1], 0.0)
        << "the storm had ended by the final step — with no arrival there is "
           "no mixing to bracket";
    ASSERT_GT(sc.runon_inflow[1], 0.0) << "no run-on reached S2";
    ASSERT_GT(sc.runoff[1], 0.0);

    const double a1_h = ws.subcatch_runoff_age[0] / 3600.0;
    const double a2_h = ws.subcatch_runoff_age[1] / 3600.0;

    double v2 = 0.0;
    for (int k = 0; k < kNSub; ++k)
        v2 += ws.subarea_vol_prev[1 * kNSub + static_cast<std::size_t>(k)];
    const double turnover_h = (v2 / sc.runoff[1]) / 3600.0;

    // SETUP: the donor must be distinguishable from the rain, or "carried
    // the donor's age" and "carried the source age" are the same number.
    ASSERT_GT(a1_h, kRainH + 0.05)
        << "S1 sheds at " << a1_h << " h against " << kRainH
        << " h of rain — too close to tell a carried age from a source age";

    EXPECT_GT(a2_h, kRainH)
        << "S2 sheds at " << a2_h << " h, younger than the " << kRainH
        << " h rain that is the youngest thing entering it.";
    EXPECT_LT(a2_h, a1_h + turnover_h)
        << "S2 sheds at " << a2_h << " h, older than S1's " << a1_h
        << " h plus one full turnover of S2's own surface (" << turnover_h
        << " h, V = " << v2 << " ft3 / Q = " << sc.runoff[1] << " cfs). "
           "Nothing entering S2 is older than S1's runoff, so exceeding this "
           "means the run-on is carrying an age it did not get from the "
           "donor.";
    swmm_engine_destroy(e);
}

// ---------------------------------------------------------------------------
// Gate 7 — the retired placeholder actually reaches the .out, in the column
// the header NAMES as the age column.
//
// A2b shipped the subcatchment age column pinned at 0 and A3 retires it, so
// this is a lesson-21 pairing: the phase that removes a placeholder owns the
// gate that proves it is gone. Nothing else here can see it — every other
// gate reads ctx.water_age_state directly, and the 14-deck bit-identity
// corpus has no deck that turns WATER_AGE on, so re-pinning the column to 0
// leaves the whole suite and the whole corpus green.
//
// Read BY NAME, not by index (lesson 40): A2b's own stride razor was blind
// precisely because it trusted a column position. With one pollutant and age
// enabled the age column is species index 1, but it is the NAME that says so.
// ---------------------------------------------------------------------------
TEST(WaterAgeWatershedTest, SubcatchmentAgeReachesTheOutByName) {
    write_file("_a3o.age", age_cfg(2.0));
    {
        std::ofstream f("_a3o.inp");
        f << "[TITLE]\nA3 .out subcatchment age column\n\n[OPTIONS]\n"
          << "FLOW_UNITS CFS\nFLOW_ROUTING DYNWAVE\nWATER_AGE ON\n"
          << "INFILTRATION HORTON\n"
          << "START_DATE 01/01/2026\nSTART_TIME 00:00:00\n"
          << "END_DATE 01/01/2026\nEND_TIME 01:00:00\n"
          << "WET_STEP 00:01:00\nDRY_STEP 00:05:00\n"
          << "ROUTING_STEP 10\nREPORT_STEP 00:05:00\n\n"
          << "[RAINGAGES]\nRG1 INTENSITY 0:05 1.0 TIMESERIES rain_ts\n\n"
          << "[TIMESERIES]\n" << rain_series()
          << "[SUBCATCHMENTS]\nS1 RG1 J1 5 50 500 0.5 0\n\n"
          << "[SUBAREAS]\nS1 0.01 0.1 0.05 0.10 25 OUTLET\n\n"
          << "[INFILTRATION]\nS1 3.0 0.5 4 7 0\n\n"
          << "[POLLUTANTS]\n"
          << "TSS MG/L 0 0 0 0 NO * 0 0 0\n\n"
          << "[JUNCTIONS]\nJ1 10.0 10 0 0 0\n\n"
          << "[OUTFALLS]\nOUT 9.0 FREE  NO\n\n"
          << "[CONDUITS]\nC1 J1 OUT 400 0.013 0 0 0\n\n"
          << "[XSECTIONS]\nC1 CIRCULAR 3.0 0 0 0\n\n"
          << "[PROCESS_COMPONENTS]\n"
          << "org.hydrocouple.openswmm.waterage config=\"_a3o.age\"\n\n"
          << "[REPORT]\nINPUT NO\n";
    }
    SWMM_Engine e = run_and_hold("_a3o.inp", "_a3o.rpt", "_a3o.out");
    ASSERT_NE(e, nullptr);
    const double shed_h =
        as_cpp_engine(e).context().water_age_state.subcatch_runoff_age[0]
        / 3600.0;
    swmm_engine_destroy(e);

    SWMM_Output h = swmm_output_open("_a3o.out");
    ASSERT_NE(h, nullptr);
    ASSERT_EQ(swmm_output_get_pollut_count(h), 2);
    const char* s0 = swmm_output_get_pollut_id(h, 0);
    const char* s1 = swmm_output_get_pollut_id(h, 1);
    ASSERT_NE(s0, nullptr);
    ASSERT_NE(s1, nullptr);
    EXPECT_STREQ(s0, "TSS");
    ASSERT_STREQ(s1, "__WATER_AGE__")
        << "species 1 is '" << s1 << "' — the age column is not where this "
           "gate is about to read, so find it by name before trusting an "
           "index";

    const int periods = swmm_output_get_period_count(h);
    ASSERT_GT(periods, 1);
    const int age_attr = SWMM_OUT_SUBCATCH_POLLUT_BASE + 1;  // species index 1

    // The column must carry a real age somewhere, not the retired 0. Scan
    // every period: the last one may fall in a gap between washoff steps,
    // and legacy's runoff gate reports 0 for a subcatchment shedding nothing.
    const int nsub = swmm_output_get_subcatch_count(h);
    ASSERT_GE(nsub, 1);
    std::vector<float> vals(static_cast<std::size_t>(nsub), 0.0f);
    double best = 0.0;
    for (int t = 0; t < periods; ++t) {
        ASSERT_EQ(swmm_output_get_subcatch_result(h, t, age_attr, vals.data()),
                  0);
        best = std::max(best, static_cast<double>(vals[0]));
    }
    swmm_output_close(h);

    EXPECT_GT(best, 0.0)
        << "the subcatchment age column is 0 in every reporting period. A2b "
           "pinned it there as a placeholder and A3 retires it — a column "
           "still reading 0 means the retirement never reached the writer.";
    // And it is the age the engine computed, not some other quantity that
    // happens to be non-zero.
    EXPECT_GT(best, 1.9)
        << "the column peaks at " << best << " h against a 2.0 h RAINFALL "
           "source age and a computed shed age of " << shed_h
        << " h — non-zero, but not the age.";
}

// ---------------------------------------------------------------------------
// Gate 8 — every subcatchment age stays at or above the youngest thing
// entering the model, on a deck whose run-on comes from a LID underdrain and
// a returning outfall.
//
// This gate exists because of a defect it would have caught. `runon_inflow`
// has THREE contributors — the subcatchment cascade, LID underdrain return
// (`lid_drain_runon_cfs`) and outfall return (`outfall_runon_vol`) — and A3
// filled the run-on age accumulator from the cascade alone, then divided it
// by all three. Every A3 deck had only a cascade, so the arithmetic was
// exercised in the one configuration where its numerator was complete
// (lesson 59, at the level of a whole contributor rather than a branch).
//
// The signature is what makes this checkable without a reference value:
// nothing in the model is younger than the RAINFALL source age, so an
// arriving age below it is IMPOSSIBLE, not merely inaccurate. Measured on
// the defect: 3.834 h of shed water under a 4 h rain, with subarea ages of
// 3.644 / 3.653 / 3.883 h. A missing contributor announces itself.
// ---------------------------------------------------------------------------
TEST(WaterAgeWatershedTest, RunonFromEveryContributorKeepsAgesAboveTheSource) {
    constexpr double kRainH = 4.0;
    write_file("_a3l.age", age_cfg(kRainH));
    {
        // S1 drains to J1 and hosts a bioretention cell whose underdrain
        // returns to S1 as run-on; OUT routes its discharge back to S1 too.
        // LID layer parameters are in FEET and FEET/SECOND — see the warning
        // in test_water_age_lid.cpp: the engine does not unit-convert them.
        std::ofstream f("_a3l.inp");
        f << "[TITLE]\nA3 run-on contributors deck\n\n[OPTIONS]\n"
          << "FLOW_UNITS CFS\nFLOW_ROUTING DYNWAVE\nWATER_AGE ON\n"
          << "INFILTRATION HORTON\n"
          << "START_DATE 01/01/2026\nSTART_TIME 00:00:00\n"
          << "END_DATE 01/01/2026\nEND_TIME 03:00:00\n"
          << "WET_STEP 00:01:00\nDRY_STEP 00:05:00\n"
          << "ROUTING_STEP 10\nREPORT_STEP 00:05:00\n\n"
          << "[RAINGAGES]\nRG1 INTENSITY 0:05 1.0 TIMESERIES rain_ts\n\n"
          << "[TIMESERIES]\n";
        for (int m = 0; m <= 185; m += 5) {
            char buf[64];
            std::snprintf(buf, sizeof(buf),
                          "rain_ts 01/01/2026 %02d:%02d 2.0\n", m / 60, m % 60);
            f << buf;
        }
        f << "\n[SUBCATCHMENTS]\nS1 RG1 J1 5 50 500 0.5 0\n\n"
          << "[SUBAREAS]\nS1 0.01 0.1 0.02 0.02 25 OUTLET\n\n"
          << "[INFILTRATION]\nS1 3.0 0.5 4 7 0\n\n"
          << "[LID_CONTROLS]\n"
          << "BC1 BC\n"
          << "BC1 SURFACE  0.05   0.0  0.1  1.0  5\n"
          << "BC1 SOIL     0.25   0.5  0.2  0.1  2.0e-5 10.0 0.3\n"
          << "BC1 STORAGE  1.0    0.75 0.0  0\n"
          << "BC1 DRAIN    1.0e-3 0.5  0    0\n\n"
          << "[LID_USAGE]\nS1 BC1 1 43560 500 50 100 0\n\n"
          << "[JUNCTIONS]\nJ1 10.0 10 0 0 0\n\n"
          // The trailing S1 is the RouteTo column: this outfall's discharge
          // re-enters the runoff system as run-on.
          << "[OUTFALLS]\nOUT 9.0 FREE  NO  S1\n\n"
          << "[CONDUITS]\nC1 J1 OUT 400 0.013 0 0 0\n\n"
          << "[XSECTIONS]\nC1 CIRCULAR 3.0 0 0 0\n\n"
          << "[PROCESS_COMPONENTS]\n"
          << "org.hydrocouple.openswmm.waterage config=\"_a3l.age\"\n\n"
          << "[REPORT]\nINPUT NO\n";
    }
    SWMM_Engine e = run_and_hold("_a3l.inp", "_a3l.rpt", "_a3l.out");
    ASSERT_NE(e, nullptr);
    const auto& ctx = as_cpp_engine(e).context();
    const auto& ws  = ctx.water_age_state;

    // SETUP: both extra contributors must actually be returning water, or
    // this is just the cascade deck again under another name.
    ASSERT_GE(ctx.n_subcatches(), 1);
    ASSERT_GT(ctx.subcatches.runon_inflow[0], 0.0)
        << "no run-on reached S1 at the final step — neither the underdrain "
           "nor the outfall returned anything, so the arithmetic this gate "
           "guards was never exercised";
    ASSERT_TRUE(ctx.lid_layer_state.active())
        << "no LID unit was built, so the underdrain contributor is absent";
    ASSERT_EQ(ctx.node_subtypes.outfalls.route_to[0], 0)
        << "the outfall does not route back to S1 — the RouteTo column did "
           "not parse, so the outfall contributor is absent";

    const double floor_s = kRainH * 3600.0;
    for (std::size_t i = 0; i < ws.subarea_age.size(); ++i) {
        if (ws.subarea_vol_prev[i] <= 0.0) continue;  // holds no water
        EXPECT_GE(ws.subarea_age[i], floor_s * 0.999)
            << "subarea " << i << " holds water aged " << (ws.subarea_age[i]
               / 3600.0) << " h, younger than the " << kRainH
            << " h RAINFALL source — and nothing in this model is younger "
               "than that. An age below the floor means run-on volume is "
               "being divided into an age accumulator that some contributor "
               "never wrote to.";
    }
    ASSERT_FALSE(ws.subcatch_runoff_age.empty());
    EXPECT_GE(ws.subcatch_runoff_age[0], floor_s * 0.999)
        << "S1 sheds water at " << (ws.subcatch_runoff_age[0] / 3600.0)
        << " h against a " << kRainH << " h floor.";
    swmm_engine_destroy(e);
}
