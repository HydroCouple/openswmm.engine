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
 * @file test_water_age_lid.cpp
 * @brief Phase A4 gates — water age through the LID layer stack.
 *
 * @details What each deck must PRODUCE for its gate to mean anything, since
 *          a branch that is never reached cannot be tested (lesson 59):
 *
 *          - `bio_deck`  — a bioretention cell with surface, soil, storage
 *                          AND an underdrain, on a subcatchment that keeps
 *                          feeding it. Reaches: the three-layer chain, the
 *                          drain, and the drain-to-node loader.
 *          - `swale_deck`— a vegetative swale: ONE layer, no drain. Reaches
 *                          the single-tank case where the mean age has a
 *                          closed form, V/Q.
 *          - `garden_deck`— a rain garden: storage ABSENT (`stor_thick` 0).
 *                          Reaches the absent-layer branch.
 *
 *          Every deck rains for its whole run. An `INTENSITY` gage reads one
 *          value per interval, so the series carries one entry per interval —
 *          A3's round shipped a deck that rained for five of its sixty
 *          minutes, and because the mixing term only acts while water is
 *          ARRIVING, every gate in it watched a draining surface and the
 *          phase's central defect was invisible (lesson 65).
 *
 * @note The `[LID_CONTROLS]` blocks below are in the user units the section
 *       normally carries — inches, in/hr, %, void RATIO — since PR #103
 *       (`5f6a2ba5`, merged 2026-08-29) made `LIDSolver::init` convert them.
 *       Until then the solver read them raw, and this file's decks were
 *       written in feet and ft/s to be sensible in the units it actually
 *       applied (issue #131); the LID fix round (2026-08-30) re-expressed the
 *       same physical column — 0.6 in berm, 3 in soil at 0.864 in/hr, 12 in
 *       storage at void ratio 3 (fraction 0.75), drain 12.4708 in/hr·in^-0.5
 *       (= 1e-3 ft/s·ft^-0.5 exactly: 43 200/√12) — so the age arithmetic
 *       these gates measure is unchanged. The one unit that could not be
 *       re-expressed was the storage void ratio, until the parameter
 *       validator stopped capping a RATIO at 1.0.
 *
 * @see plans/transport/WATER_AGE_TRACKING_PLAN.md §7 A4
 * @see plans/transport/A4_IMPLEMENTATION_BRIEF_2026-08-17.md
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include <openswmm/engine/openswmm_engine.h>

#include "core/SWMMEngine.hpp"
#include "hydrology/LID.hpp"

namespace {

openswmm::SWMMEngine& as_cpp_engine(SWMM_Engine engine) {
    return *static_cast<openswmm::SWMMEngine*>(engine);
}

using openswmm::LidLayer;
using openswmm::LidSpecies;

constexpr int kAge  = static_cast<int>(LidSpecies::AGE);
constexpr int kNL   = openswmm::LidLayerSpeciesState::kLayerCount;
constexpr double kRainInHr = 2.0;
/// A distinctive, non-zero source age: every residence time below is then a
/// DIFFERENCE against it, so an implementation that reported an absolute
/// elapsed time could not land on the right answer by coincidence.
constexpr double kRainH = 4.0;

void write_file(const char* path, const std::string& body) {
    std::ofstream f(path);
    f << body;
}

/// One entry per gage interval, for the whole run — see the file comment.
std::string rain_series(int minutes, int interval_min) {
    std::string s;
    char buf[80];
    for (int m = 0; m <= minutes + interval_min; m += interval_min) {
        std::snprintf(buf, sizeof(buf), "STORM 01/01/2026 %02d:%02d %.2f\n",
                      m / 60, m % 60, kRainInHr);
        s += buf;
    }
    return s;
}

std::string age_cfg(double rain_h, double init_h) {
    return "[WATER_AGE_SOURCES]\nRAINFALL GLOBAL " + std::to_string(rain_h) +
           "\nINITIAL_STATE GLOBAL " + std::to_string(init_h) + "\n";
}

/// `lid_block` supplies [LID_CONTROLS] + [LID_USAGE]; the rest is shared.
void write_deck(const char* path, const std::string& lid_block,
                const char* cfg) {
    std::ofstream f(path);
    f << "[TITLE]\nA4 LID layer age gate deck\n\n[OPTIONS]\n"
      << "FLOW_UNITS CFS\nINFILTRATION HORTON\nFLOW_ROUTING DYNWAVE\n"
      << "WATER_AGE ON\n"
      << "START_DATE 01/01/2026\nSTART_TIME 00:00:00\n"
      << "END_DATE 01/01/2026\nEND_TIME 03:00:00\n"
      << "REPORT_STEP 00:05:00\nWET_STEP 00:01:00\nDRY_STEP 00:05:00\n"
      << "ROUTING_STEP 10\n\n"
      << "[RAINGAGES]\nRG INTENSITY 0:05 1.0 TIMESERIES STORM\n\n"
      << "[TIMESERIES]\n" << rain_series(180, 5) << "\n"
      << "[SUBCATCHMENTS]\nS1 RG J1 5 0 500 0.5 0\n\n"
      << "[SUBAREAS]\nS1 0.01 0.1 0.0 0.0 100 OUTLET\n\n"
      << "[INFILTRATION]\nS1 0.0 0.0 4.0 7.0 0\n\n"
      << lid_block
      << "[JUNCTIONS]\nJ1 10.0 10 0 0 0\n\n"
      << "[OUTFALLS]\nOUT 9.0 FREE  NO\n\n"
      << "[CONDUITS]\nC1 J1 OUT 400 0.013 0 0 0\n\n"
      << "[XSECTIONS]\nC1 CIRCULAR 3.0 0 0 0\n\n"
      << "[PROCESS_COMPONENTS]\n"
      << "org.hydrocouple.openswmm.waterage config=\"" << cfg << "\"\n\n"
      << "[REPORT]\nINPUT NO\n";
}

/// Bioretention: surface + soil + storage + underdrain. 1 acre of the 5.
/// Every layer is sized to turn over INSIDE the run — a small surface store,
/// a thin half-saturated soil, and a drain fast enough that storage stays
/// shallow. A layer still filling at the final step has a mean age below its
/// V/Q, so a chain of slow layers cannot be checked against one.
/// See the file warning on units.
std::string bio_block(double drain_coeff = 12.4708) {
    std::ostringstream o;
    o << "[LID_CONTROLS]\n"
         "BC1 BC\n"
         "BC1 SURFACE  0.6    0.0  0.1  100  5\n"
         "BC1 SOIL     3.0    0.5  0.2  0.1  0.864  10.0 3.6\n"
         "BC1 STORAGE  12.0   3.0  0.0  0\n"
         "BC1 DRAIN    " << drain_coeff << " 0.5  0    0\n\n";
    return o.str() +
           "[LID_USAGE]\n"
           "S1 BC1 1 43560 500 50 100 0\n\n";
}

/// Vegetative swale: a single surface layer, no drain. A small storage
/// height and a real flow width let it fill and then shed at the rate it
/// receives, so it reaches steady state inside the run — which is what makes
/// V/Q the mean age rather than an asymptote it is still climbing towards.
std::string swale_block() {
    return "[LID_CONTROLS]\n"
           "SW1 VS\n"
           "SW1 SURFACE  0.6  0.0  0.2  2     5\n\n"
           "[LID_USAGE]\n"
           "S1 SW1 1 43560 500 0 100 0\n\n";
}

/// Rain garden: surface + soil, storage ABSENT (thickness 0).
std::string garden_block() {
    return "[LID_CONTROLS]\n"
           "RG1 RG\n"
           "RG1 SURFACE  6.0  0.0  0.1  100  5\n"
           "RG1 SOIL     3.0  0.5  0.2  0.1  0.864  10.0 3.6\n\n"
           "[LID_USAGE]\n"
           "S1 RG1 1 43560 0 50 100 0\n\n";
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
    } while (elapsed > 0.0 && ++guard < 40000);
    swmm_engine_end(e);
    return e;
}

/// The single LID unit of a one-unit deck, as (group, unit-in-group, flat).
struct Unit { int t = -1, u = -1, flat = -1; };

Unit only_unit(openswmm::SWMMEngine& eng) {
    const auto& st = eng.context().lid_layer_state;
    for (int t = 0; t < eng.lid().numGroups(); ++t)
        if (eng.lid().group(t).count > 0)
            return {t, 0, st.group_offset[static_cast<std::size_t>(t)]};
    return {};
}

/// The age of the water ARRIVING at a unit, which every residence time
/// below is measured as a difference from.
double st_inflow_age(const openswmm::SimulationContext& ctx, int flat) {
    const auto& st = ctx.lid_layer_state;
    return st.inflow_value[static_cast<std::size_t>(flat) *
                               static_cast<std::size_t>(st.n_species) +
                           static_cast<std::size_t>(kAge)];
}

double layer_age(const openswmm::SimulationContext& ctx, int flat,
                 LidLayer k) {
    return ctx.lid_layer_state.value[
        ctx.lid_layer_state.layer_index(flat, k, kAge)];
}

}  // namespace

// ---------------------------------------------------------------------------
// Gate 1 — the block exists, is per-LAYER, and the unit is RECEIVING water.
//
// The setup leg is the point. "wb_inflow > 0" would be true of a unit that
// took one drop an hour ago and has been draining ever since, which is the
// regime where a mixing term does nothing at all (lesson 65). So this asserts
// a magnitude AND that water is still arriving at the final step.
// ---------------------------------------------------------------------------
TEST(WaterAgeLidTest, LayerBlockIsSizedAndTheUnitIsReceiving) {
    write_file("_a4.age", age_cfg(kRainH, kRainH));
    write_deck("_a4.inp", bio_block(), "_a4.age");
    SWMM_Engine e = run_and_hold("_a4.inp", "_a4.rpt", "_a4.out");
    ASSERT_NE(e, nullptr);
    auto& eng = as_cpp_engine(e);
    const auto& ctx = eng.context();
    const auto& st  = ctx.lid_layer_state;

    ASSERT_TRUE(st.active()) << "the per-layer block was never sized — no LID "
                                "units were built, so nothing below is a test";
    ASSERT_EQ(st.n_units, 1);
    ASSERT_EQ(static_cast<int>(st.value.size()), st.n_units * kNL *
                                                 st.n_species);
    ASSERT_EQ(st.n_species, static_cast<int>(LidSpecies::COUNT_));

    const Unit un = only_unit(eng);
    ASSERT_GE(un.flat, 0);
    const auto& g  = eng.lid().group(un.t);
    const auto  ui = static_cast<std::size_t>(un.u);

    // SETUP, magnitude: 2 in/h over an acre for two hours is ~0.33 ft of
    // water on the unit. Anything far below that means the storm did not
    // reach it.
    EXPECT_GT(g.wb_inflow[ui], 0.2)
        << "the unit received only " << g.wb_inflow[ui] << " ft of water over "
           "the run; the deck is meant to deliver about 0.33 ft";
    // SETUP, regime: still arriving at the last step.
    EXPECT_GT(g.in_surf[ui], 0.0)
        << "no inflow at the final step — the storm ended early and every "
           "gate here would be watching a draining unit";

    EXPECT_GT(layer_age(ctx, un.flat, LidLayer::SURFACE), 0.0);
    EXPECT_GT(layer_age(ctx, un.flat, LidLayer::SOIL), 0.0)
        << "the soil layer never aged — nothing percolated into it";
    swmm_engine_destroy(e);
}

// ---------------------------------------------------------------------------
// Gate 2 — a ONE-LAYER unit sheds at its residence time V/Q.
//
// The magnitude gate (lesson 61). A swale is a single complete-mix tank: at
// steady state inflow equals outflow, so the mean age of the water it holds
// is exactly V/Q, with V the stored depth and Q the inflow rate — both
// published, so the reference is computed from the run rather than fitted.
//
// This is the gate that separates a correct mixing volume from A3's: mixing
// on the NET change in storage admits nothing while a unit passes water
// straight through, and the age becomes the time since the unit first wetted
// instead of the time water spends in it.
// ---------------------------------------------------------------------------
TEST(WaterAgeLidTest, SingleLayerUnitHoldsWaterForItsResidenceTime) {
    write_file("_a4s.age", age_cfg(kRainH, kRainH));
    write_deck("_a4s.inp", swale_block(), "_a4s.age");
    SWMM_Engine e = run_and_hold("_a4s.inp", "_a4s.rpt", "_a4s.out");
    ASSERT_NE(e, nullptr);
    auto& eng = as_cpp_engine(e);
    const auto& ctx = eng.context();
    ASSERT_TRUE(ctx.lid_layer_state.active());

    const Unit un = only_unit(eng);
    ASSERT_GE(un.flat, 0);
    const auto& g  = eng.lid().group(un.t);
    const auto  ui = static_cast<std::size_t>(un.u);

    // SETUP: steady state — receiving, and holding water.
    ASSERT_GT(g.in_surf[ui], 0.0) << "not receiving at the final step";
    const double v = g.surf_depth[ui] * g.surf_void_frac[ui];
    ASSERT_GT(v, 0.0) << "the swale holds nothing at the final step";

    // SETUP: steady state. A tank still filling has a mean age BELOW its
    // V/Q, so the closed form only applies once outflow matches inflow.
    const double q_out = g.surface_runoff[ui] + g.infil_loss[ui] / 60.0;
    ASSERT_NEAR(q_out / g.in_surf[ui], 1.0, 0.10)
        << "the swale is not at steady state: in " << g.in_surf[ui]
        << " ft/s against out " << q_out << " ft/s, so V/Q is an asymptote "
           "it is still climbing towards rather than its mean age";

    const double residence_s = v / g.in_surf[ui];
    const double arrived_s   = st_inflow_age(ctx, un.flat);
    const double reported_s  = layer_age(ctx, un.flat, LidLayer::SURFACE);
    const double held_s      = reported_s - arrived_s;
    EXPECT_NEAR(held_s / residence_s, 1.0, 0.05)
        << "the swale surface reads " << reported_s << " s on water that "
           "arrived at " << arrived_s << " s, so it held it for " << held_s
        << " s against a residence time V/Q of " << residence_s << " s (V = "
        << v << " ft, Q = " << g.in_surf[ui] << " ft/s) — a ratio of "
        << (held_s / residence_s)
        << ". A ratio far above 1 means the mixing volume is not admitting "
           "the water flowing through, so the age is measuring time since "
           "the unit first wetted.";
    swmm_engine_destroy(e);
}

// ---------------------------------------------------------------------------
// Gate 3 — the HRT CHAIN down the column, which is plan §7 A4's own verify
// criterion ("drain age ≈ layer HRT chain under constant inflow").
//
// Each layer is a tank in series, so at steady state the age leaving layer k
// is the age arriving plus that layer's own V/Q. Summing down the column
// gives a reference for the storage layer built entirely from published
// state — not a direction, and not a number this test chose.
// ---------------------------------------------------------------------------
TEST(WaterAgeLidTest, StorageAgeIsTheSumOfTheLayerResidenceTimes) {
    write_file("_a4c.age", age_cfg(kRainH, kRainH));
    write_deck("_a4c.inp", bio_block(), "_a4c.age");
    SWMM_Engine e = run_and_hold("_a4c.inp", "_a4c.rpt", "_a4c.out");
    ASSERT_NE(e, nullptr);
    auto& eng = as_cpp_engine(e);
    const auto& ctx = eng.context();
    ASSERT_TRUE(ctx.lid_layer_state.active());

    const Unit un = only_unit(eng);
    ASSERT_GE(un.flat, 0);
    const auto& g  = eng.lid().group(un.t);
    const auto  ui = static_cast<std::size_t>(un.u);

    const double v_surf = g.surf_depth[ui] * g.surf_void_frac[ui];
    const double v_soil = g.soil_moist[ui] * g.soil_thick[ui];
    const double v_stor = g.stor_depth[ui] * g.stor_void[ui];
    const double q_surf = g.in_surf[ui];
    const double q_soil = g.in_soil[ui];
    const double q_stor = g.in_stor[ui];

    // SETUP: every link of the chain must be carrying water, or the sum
    // below is over tanks that are not actually in series.
    ASSERT_GT(q_surf, 0.0) << "no inflow onto the surface";
    ASSERT_GT(q_soil, 0.0) << "nothing infiltrating into the soil";
    ASSERT_GT(q_stor, 0.0) << "nothing percolating into the storage layer";
    ASSERT_GT(v_stor, 0.0) << "the storage layer holds nothing";

    const double chain_s = v_surf / q_surf + v_soil / q_soil + v_stor / q_stor;
    const double arrived_s  = st_inflow_age(ctx, un.flat);
    const double reported_s = layer_age(ctx, un.flat, LidLayer::STORAGE);
    const double held_s     = reported_s - arrived_s;

    // Monotone first: each layer must be older than the one above it.
    EXPECT_GT(layer_age(ctx, un.flat, LidLayer::SOIL),
              layer_age(ctx, un.flat, LidLayer::SURFACE))
        << "soil water is not older than surface water — the layers are not "
           "in series, or the donor chain is wired the wrong way up";
    EXPECT_GT(reported_s, layer_age(ctx, un.flat, LidLayer::SOIL))
        << "storage water is not older than soil water";

    // Then the magnitude. The band is wide because the column is not at
    // perfect steady state at the final step — but a factor-level error in
    // any layer's mixing moves this far outside it.
    // 15 %: the storage layer's own turnover is short enough that a
    // one-minute runoff step cannot resolve it exactly, and the column is
    // not perfectly steady at the final step. A factor-level error in any
    // layer's mixing moves this far outside the band.
    EXPECT_NEAR(held_s / chain_s, 1.0, 0.15)
        << "water reaching storage was held " << held_s << " s against a "
           "chained residence time of " << chain_s << " s (surface "
        << (v_surf/q_surf) << " + soil " << (v_soil/q_soil) << " + storage "
        << (v_stor/q_stor) << ") — a ratio of " << (held_s / chain_s) << ".";
    swmm_engine_destroy(e);
}

// ---------------------------------------------------------------------------
// Gate 4 — the underdrain leaves at the STORAGE age, and the NODE gets it.
//
// Two claims, and the second is the one nothing else here can see: the
// wet-weather loader used to hand LID drain water the configured RAINFALL
// age with a comment naming phase A4 as the fix. Retiring a deferral means
// flipping the assertion that recorded it in the same changeset (lesson 21),
// so the rain age is set to a distinctive value that the drain age cannot be
// mistaken for.
// ---------------------------------------------------------------------------
TEST(WaterAgeLidTest, DrainLeavesAtStorageAgeAndReachesTheNode) {
    constexpr double kRainH = 4.0;
    write_file("_a4d.age", age_cfg(kRainH, kRainH));
    // A slower underdrain than the chain gate's: the column has to hold its
    // water long enough for the drain to be DISTINCTLY older than the
    // subcatchment's own runoff, or the bracket below has no width.
    write_deck("_a4d.inp", bio_block(1.0), "_a4d.age");
    SWMM_Engine e = run_and_hold("_a4d.inp", "_a4d.rpt", "_a4d.out");
    ASSERT_NE(e, nullptr);
    auto& eng = as_cpp_engine(e);
    const auto& ctx = eng.context();
    const auto& st  = ctx.lid_layer_state;
    ASSERT_TRUE(st.active());

    const Unit un = only_unit(eng);
    ASSERT_GE(un.flat, 0);
    const auto& g  = eng.lid().group(un.t);
    const auto  ui = static_cast<std::size_t>(un.u);

    ASSERT_GT(g.drain_flow[ui], 0.0)
        << "the underdrain never flowed — the drain branch was not reached, "
           "so neither claim below is under test";

    const double drain_s = st.drain_value[
        static_cast<std::size_t>(un.flat) *
            static_cast<std::size_t>(st.n_species) +
        static_cast<std::size_t>(kAge)];
    EXPECT_DOUBLE_EQ(drain_s, layer_age(ctx, un.flat, LidLayer::STORAGE))
        << "the drain is not leaving at the storage layer's age";

    // And the node. J1 receives TWO things on this deck — the underdrain,
    // and the surface runoff of the four acres the LID does not cover — so
    // its age is a mixture and must be bracketed rather than matched.
    ASSERT_FALSE(ctx.water_age_state.node_age.empty());
    ASSERT_FALSE(ctx.water_age_state.subcatch_runoff_age.empty());
    const double j1_h    = ctx.water_age_state.node_age[0] / 3600.0;
    const double drain_h = drain_s / 3600.0;
    const double surf_h  = ctx.water_age_state.subcatch_runoff_age[0] / 3600.0;

    // SETUP: the drain must be distinctly older than everything else
    // arriving, or the bracket below has no width to detect anything in.
    ASSERT_GT(drain_h, surf_h + 0.2)
        << "the underdrain (" << drain_h << " h) is not meaningfully older "
           "than the subcatchment's own runoff (" << surf_h << " h), so no "
           "assertion about J1 can tell which age it was given";

    EXPECT_LT(j1_h, drain_h)
        << "J1 (" << j1_h << " h) is older than the oldest thing entering it, "
           "the underdrain at " << drain_h << " h";
    EXPECT_GT(j1_h, surf_h + 0.03)
        << "J1 holds water at " << j1_h << " h while the subcatchment's own "
           "runoff is " << surf_h << " h and the underdrain delivered "
        << drain_h << " h. The drain is the only thing that can pull the node "
           "above its surface runoff, so a J1 at or below " << surf_h
        << " h means the loader is still handing drain water the "
        << kRainH << " h RAINFALL age — the stand-in that named phase A4 as "
           "its fix.";
    swmm_engine_destroy(e);
}

// ---------------------------------------------------------------------------
// Gate 5 — an ABSENT layer stays absent.
//
// A rain garden has no storage layer (`stor_thick` 0). Its storage row must
// never mix, and — because the drain decision reads the storage row — a unit
// with no storage and no drain must not publish a drain age drawn from a
// layer that does not exist.
// ---------------------------------------------------------------------------
TEST(WaterAgeLidTest, AbsentStorageLayerNeverMixes) {
    write_file("_a4g.age", age_cfg(kRainH, kRainH));
    write_deck("_a4g.inp", garden_block(), "_a4g.age");
    SWMM_Engine e = run_and_hold("_a4g.inp", "_a4g.rpt", "_a4g.out");
    ASSERT_NE(e, nullptr);
    auto& eng = as_cpp_engine(e);
    const auto& ctx = eng.context();
    ASSERT_TRUE(ctx.lid_layer_state.active());

    const Unit un = only_unit(eng);
    ASSERT_GE(un.flat, 0);
    const auto& g  = eng.lid().group(un.t);
    const auto  ui = static_cast<std::size_t>(un.u);

    // SETUP: the rest of the stack must be working, or "storage is empty" is
    // just "nothing ran".
    ASSERT_EQ(g.stor_thick[ui], 0.0) << "the deck built a storage layer";
    ASSERT_GT(layer_age(ctx, un.flat, LidLayer::SOIL), 0.0)
        << "the soil layer never aged, so this deck is not exercising the "
           "unit at all";

    EXPECT_EQ(g.in_stor[ui], 0.0)
        << "water is being published as entering a storage layer the deck "
           "does not have";
    EXPECT_EQ(g.drain_flow[ui], 0.0)
        << "a rain garden with no storage layer reported drain flow";
    // And the row itself must stay empty. Without this the layer simply
    // advances by dt every step and ends the run holding the elapsed time —
    // which is then what the underdrain would report, since the drain reads
    // the storage row.
    EXPECT_EQ(layer_age(ctx, un.flat, LidLayer::STORAGE), 0.0)
        << "the absent storage layer accumulated an age of "
        << layer_age(ctx, un.flat, LidLayer::STORAGE)
        << " s; a layer holding no water has no age to report";
    swmm_engine_destroy(e);
}

// ---------------------------------------------------------------------------
// Gate 6 — the unit's inflow carries the SUBCATCHMENT's age, not the rain's.
//
// The other half of the A3/A4 composition. Gate 4 covers what leaves the unit
// for the network; this covers what enters it from the surface around it.
// Water captured from a subcatchment's impervious subareas already aged there
// (A3), so a LID fed by runoff must not treat its inflow as fresh rain.
//
// The deck this needs is different from every other one here: it has an
// IMPERVIOUS fraction and a `FromImp` capture, so `q_imperv` is non-zero.
// With %Imperv 0 — as the other decks have it — the unit's only inflow is
// direct rainfall, the subarea terms are all multiplied by zero, and
// replacing the whole weighted mean with the raw rainfall age changes
// nothing at all.
// ---------------------------------------------------------------------------
TEST(WaterAgeLidTest, UnitInflowCarriesTheSubcatchmentAge) {
    write_file("_a4i.age", age_cfg(kRainH, kRainH));
    {
        // 50 % impervious, and the LID takes all of that runoff.
        std::ofstream f("_a4i.inp");
        f << "[TITLE]\nA4 LID inflow composition deck\n\n[OPTIONS]\n"
          << "FLOW_UNITS CFS\nINFILTRATION HORTON\nFLOW_ROUTING DYNWAVE\n"
          << "WATER_AGE ON\n"
          << "START_DATE 01/01/2026\nSTART_TIME 00:00:00\n"
          << "END_DATE 01/01/2026\nEND_TIME 03:00:00\n"
          << "REPORT_STEP 00:05:00\nWET_STEP 00:01:00\nDRY_STEP 00:05:00\n"
          << "ROUTING_STEP 10\n\n"
          << "[RAINGAGES]\nRG INTENSITY 0:05 1.0 TIMESERIES STORM\n\n"
          << "[TIMESERIES]\n" << rain_series(180, 5) << "\n"
          << "[SUBCATCHMENTS]\nS1 RG J1 5 50 500 0.5 0\n\n"
          << "[SUBAREAS]\nS1 0.01 0.1 0.02 0.02 25 OUTLET\n\n"
          << "[INFILTRATION]\nS1 0.0 0.0 4.0 7.0 0\n\n"
          << bio_block()
          << "[JUNCTIONS]\nJ1 10.0 10 0 0 0\n\n"
          << "[OUTFALLS]\nOUT 9.0 FREE  NO\n\n"
          << "[CONDUITS]\nC1 J1 OUT 400 0.013 0 0 0\n\n"
          << "[XSECTIONS]\nC1 CIRCULAR 3.0 0 0 0\n\n"
          << "[PROCESS_COMPONENTS]\n"
          << "org.hydrocouple.openswmm.waterage config=\"_a4i.age\"\n\n"
          << "[REPORT]\nINPUT NO\n";
    }
    SWMM_Engine e = run_and_hold("_a4i.inp", "_a4i.rpt", "_a4i.out");
    ASSERT_NE(e, nullptr);
    auto& eng = as_cpp_engine(e);
    const auto& ctx = eng.context();
    ASSERT_TRUE(ctx.lid_layer_state.active());

    const Unit un = only_unit(eng);
    ASSERT_GE(un.flat, 0);
    const auto& g  = eng.lid().group(un.t);
    const auto  ui = static_cast<std::size_t>(un.u);

    // SETUP: the unit must actually be capturing subarea runoff, not just
    // catching rain — otherwise every subarea term is multiplied by zero.
    ASSERT_GT(g.from_imperv[ui], 0.0) << "the deck captures no impervious "
                                         "runoff, so the composition this "
                                         "gate tests is not exercised";
    ASSERT_GT(g.inflow[ui], 0.0);
    const double rain_ft_s = ctx.subcatches.rainfall[0];
    ASSERT_GT(g.inflow[ui], rain_ft_s * 1.05)
        << "the unit's inflow (" << g.inflow[ui] << " ft/s) is no more than "
           "the rain falling on it (" << rain_ft_s << " ft/s) — no subarea "
           "runoff is being captured";

    // The captured runoff aged on the surface before it arrived, so the mix
    // must be older than the rain that fell straight in.
    const double arrived_h = st_inflow_age(ctx, un.flat) / 3600.0;
    EXPECT_GT(arrived_h, kRainH + 0.01)
        << "water arrives at the unit at " << arrived_h << " h against "
        << kRainH << " h of rain, while most of it came off an impervious "
           "surface that had already aged it. An inflow age equal to the "
           "source age means the subarea ages A3 computed are not reaching "
           "the LID.";
    swmm_engine_destroy(e);
}
