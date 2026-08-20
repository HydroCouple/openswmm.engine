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
 * @file test_heat_lid.cpp
 * @brief Phase H5b gates — temperature through the LID layer stack.
 *
 * @warning LID layer parameters in these decks are written in FEET and
 *          FEET/SECOND deliberately. Issue #131: a conventional
 *          `[LID_CONTROLS]` block reaches the solver UNCONVERTED, so a soil
 *          layer given in inches arrives as 18 ft with a 0.5 ft/s
 *          conductivity — 43,200x too fast. **These gates are expected to
 *          fail when the conversion lands, and the correct response then is
 *          to convert the decks, not to widen the bands.**
 *
 * @details The load-bearing gates here need no reference value:
 *          - with conduction ON and both atmospheric modules OFF, the
 *            column's total heat content is CONSERVED (gate 4);
 *          - conduction only ever moves temperatures toward each other, so
 *            every layer stays inside the bracket its column started in
 *            (gate 5).
 *          Neither can be staled by a deck or timestep change, which is the
 *          shape lesson 72 asks for.
 *
 * @see plans/transport/HEAT_TRANSPORT_PLAN.md §6 H5b, §6.1 D-H5b/D-H5c
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>

#include <openswmm/engine/openswmm_engine.h>

#include "core/SWMMEngine.hpp"
#include "transport/components/HeatModule/HeatLid.hpp"

namespace tr = openswmm::transport;

namespace {

openswmm::SWMMEngine& as_cpp_engine(SWMM_Engine engine) {
    return *static_cast<openswmm::SWMMEngine*>(engine);
}

constexpr int kNL   = openswmm::LidLayerSpeciesState::kLayerCount;
constexpr int kTemp = static_cast<int>(openswmm::LidSpecies::TEMPERATURE);
constexpr int kAge  = static_cast<int>(openswmm::LidSpecies::AGE);

void write_file(const char* path, const std::string& body) {
    std::ofstream f(path);
    f << body;
}

/// One value per interval — an INTENSITY gage reads one per gage interval,
/// not one per hour (A3's round learned this the expensive way).
std::string series(const char* name, int last_min, double v,
                   int stop_min = -1) {
    if (stop_min < 0) stop_min = last_min;
    std::string s;
    char buf[80];
    for (int m = 0; m <= last_min; m += 5) {
        std::snprintf(buf, sizeof(buf), "%s 01/01/2026 %02d:%02d %.4f\n",
                      name, m / 60, m % 60, m <= stop_min ? v : 0.0);
        s += buf;
    }
    return s + "\n";
}

struct Opts {
    bool water_age  = false;
    bool heat       = true;
    bool conduction = false;
    bool surface    = false;
    bool radiative  = false;
    double air_f    = 68.0;
    double rain_c   = 5.0;
    double init_c   = 25.0;
    int    end_min  = 60;
    /// Last minute that rains; -1 rains for the whole run. Used by the
    /// conservation ledger, which needs the column to stop exchanging water
    /// with the rest of the model before it can attribute a change in heat
    /// content to conduction alone.
    int    rain_stop_min = -1;
    /// Underdrain coefficient. Zeroing it, with the storm stopped, leaves a
    /// column that holds its water: no advection, no drainage, so the only
    /// operator left is the one being measured.
    double drain_coeff = 1.0e-3;
    /// Route the outfall's discharge back onto S1 as run-on. On by default
    /// because most gates want the LID fed; OFF for the ledger, which needs
    /// the column to stop exchanging water with the rest of the model.
    bool   outfall_routes_back = true;
    /// `[LID_USAGE]` FromImp — the percentage of the subcatchment's
    /// impervious runoff routed onto the unit. At 0 the LID receives only
    /// rain on its own footprint, so when the storm stops its inflow is
    /// EXACTLY zero. At the default 100 the subcatchment trickles into it
    /// indefinitely and no layer ever reaches the dry-but-present state.
    int    from_imperv = 100;
    const char* dry_policy = nullptr;
};

/// A bioretention cell on one subcatchment, with an underdrain returning as
/// run-on and an outfall routing discharge back — so all three run-on
/// contributors are live, the third of them (the drain) only from H5b.
void write_deck(const char* path, const Opts& o) {
    std::ofstream f(path);
    f << "[TITLE]\nH5b LID heat gate deck\n\n[OPTIONS]\n"
      << "FLOW_UNITS CFS\nFLOW_ROUTING DYNWAVE\nINFILTRATION HORTON\n"
      << (o.water_age ? "WATER_AGE ON\n" : "")
      << (o.heat ? "HEAT_TRANSPORT ON\n" : "")
      << "START_DATE 01/01/2026\nSTART_TIME 00:00:00\n"
      << "END_DATE 01/01/2026\nEND_TIME " << (o.end_min / 60) << ":"
      << (o.end_min % 60 < 10 ? "0" : "") << (o.end_min % 60) << ":00\n"
      << "WET_STEP 00:01:00\nDRY_STEP 00:05:00\n"
      << "ROUTING_STEP 10\nREPORT_STEP 00:05:00\n\n"
      << "[TEMPERATURE]\nTIMESERIES air_ts\n"
      << "WINDSPEED MONTHLY";
    for (int m = 0; m < 12; ++m) f << " 5.0";
    f << "\nHUMIDITY";
    for (int m = 0; m < 12; ++m) f << " 50.0";
    f << "\n\n[RAINGAGES]\nRG1 INTENSITY 0:05 1.0 TIMESERIES rain_ts\n\n"
      << "[TIMESERIES]\n"
      << series("rain_ts", o.end_min + 5, 2.0, o.rain_stop_min)
      << series("air_ts", o.end_min + 5, o.air_f)
      << "[SUBCATCHMENTS]\nS1 RG1 J1 5 50 500 0.5 0\n\n"
      << "[SUBAREAS]\nS1 0.01 0.1 0.02 0.02 25 OUTLET\n\n"
      << "[INFILTRATION]\nS1 3.0 0.5 4 7 0\n\n"
      // FEET and FT/S — see the file warning and issue #131.
      << "[LID_CONTROLS]\n"
      << "BC1 BC\n"
      << "BC1 SURFACE  0.05   0.0  0.1  1.0  5\n"
      << "BC1 SOIL     0.25   0.5  0.2  0.1  2.0e-5 10.0 0.3\n"
      << "BC1 STORAGE  1.0    0.75 0.0  0\n"
      << "BC1 DRAIN    " << o.drain_coeff << " 0.5  0    0\n\n"
      << "[LID_USAGE]\nS1 BC1 1 43560 500 50 " << o.from_imperv << " 0\n\n"
      << "[JUNCTIONS]\nJ1 10.0 10 0 0 0\n\n"
      << "[OUTFALLS]\nOUT 9.0 FREE  NO"
      << (o.outfall_routes_back ? "  S1" : "") << "\n\n"
      << "[CONDUITS]\nC1 J1 OUT 400 0.013 0 0 0\n\n"
      << "[XSECTIONS]\nC1 CIRCULAR 3.0 0 0 0\n\n"
      << "[PROCESS_COMPONENTS]\n";
    if (o.heat)
        f << "org.hydrocouple.openswmm.heat config=\"_h5b.heat\"\n";
    if (o.water_age)
        f << "org.hydrocouple.openswmm.waterage config=\"_h5b.age\"\n";
    f << "\n[REPORT]\nINPUT NO\n";
}

void write_cfg(const Opts& o) {
    std::string s = "[HEAT_SOURCES]\n";
    s += "RAINFALL GLOBAL " + std::to_string(o.rain_c) + "\n";
    s += "INITIAL_STATE GLOBAL " + std::to_string(o.init_c) + "\n\n";
    s += "[HEAT_FLUXES]\n";
    s += std::string("SURFACE_EXCHANGE ")   + (o.surface   ? "ON" : "OFF") + "\n";
    s += std::string("RADIATIVE_EXCHANGE ") + (o.radiative ? "ON" : "OFF") + "\n";
    s += std::string("LAYER_CONDUCTION ")   + (o.conduction? "ON" : "OFF") + "\n";
    if (o.dry_policy != nullptr)
        s += std::string("DRY_ELEMENT_TEMPERATURE ") + o.dry_policy + "\n";
    write_file("_h5b.heat", s);
    // INITIAL_STATE is not decoration. `initLidLayerAge` seeds a wet layer
    // with it, so with no row the seed is 0 — and a wipe of a zero seed is
    // invisible. Gate 2's both-on leg exists to catch exactly that wipe.
    write_file("_h5b.age",
               "[WATER_AGE_SOURCES]\nRAINFALL GLOBAL 1.0\n"
               "INITIAL_STATE GLOBAL 4.0\n");
}

SWMM_Engine run(const Opts& o) {
    write_cfg(o);
    write_deck("_h5b.inp", o);
    SWMM_Engine e = swmm_engine_create();
    if (e == nullptr) return nullptr;
    if (swmm_engine_open(e, "_h5b.inp", "_h5b.rpt", "_h5b.out", nullptr)
            != SWMM_OK ||
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

/// SETUP shared by every gate: a LID unit must actually have been built, or
/// every value below is an untouched seed (lesson 59).
::testing::AssertionResult HasLidUnit(const openswmm::SimulationContext& c) {
    if (!c.lid_layer_state.active())
        return ::testing::AssertionFailure()
               << "no LID unit was built — the deck is not exercising the "
                  "code under test";
    return ::testing::AssertionSuccess();
}

}  // namespace

// ---------------------------------------------------------------------------
// Gate 1 — the TEMPERATURE row exists beside AGE, and every layer is seeded.
// ---------------------------------------------------------------------------
TEST(HeatLidTest, TemperatureRowIsAllocatedAndEveryLayerSeeded) {
    Opts o{};
    o.init_c = 25.0;
    SWMM_Engine e = run(o);
    ASSERT_NE(e, nullptr);
    const auto& ctx = as_cpp_engine(e).context();
    const auto& st  = ctx.lid_layer_state;
    ASSERT_TRUE(HasLidUnit(ctx));

    ASSERT_EQ(st.n_species, static_cast<int>(openswmm::LidSpecies::COUNT_))
        << "the species stride is " << st.n_species << ", expected 2 — A4 "
           "reserved it for exactly this row";
    ASSERT_EQ(static_cast<int>(st.value.size()),
              st.n_units * kNL * st.n_species);

    // EVERY layer, wet or dry. The age row leaves a dry layer at 0 because 0
    // is age's "nothing here"; a temperature row must not, because 0 C is a
    // freezing layer and nothing distinguishes it from the sentinel.
    for (int u = 0; u < st.n_units; ++u)
        for (int k = 0; k < kNL; ++k) {
            const double t = st.value[st.layer_index(
                u, static_cast<openswmm::LidLayer>(k), kTemp)];
            EXPECT_NE(t, 0.0)
                << "unit " << u << " layer " << k << " reports exactly 0 C — "
                   "A4's 'no water, no value' sentinel leaked into the heat "
                   "row";
            EXPECT_TRUE(std::isfinite(t));
        }
    swmm_engine_destroy(e);
}

// ---------------------------------------------------------------------------
// Gate 2 — THE CONFIGURATION TRAP. Heat without age, and age without heat,
//          must each work. This is the eighth instance of lesson 20.
// ---------------------------------------------------------------------------
TEST(HeatLidTest, EitherCapabilityAloneSizesAndSeedsItsOwnRow) {
    {   // HEAT only — A4's initLidLayerAge would never have sized the block.
        Opts o{}; o.water_age = false; o.heat = true; o.init_c = 21.0;
        SWMM_Engine e = run(o);
        ASSERT_NE(e, nullptr);
        const auto& ctx = as_cpp_engine(e).context();
        ASSERT_FALSE(ctx.options.water_age);
        ASSERT_TRUE(HasLidUnit(ctx))
            << "a HEAT-only deck did not size the LID layer block — the "
               "sizing is still behind a water_age guard";
        swmm_engine_destroy(e);
    }
    {   // AGE only — the temperature row must not be left at 0 C, and the
        // age row must still be correct.
        Opts o{}; o.water_age = true; o.heat = false;
        SWMM_Engine e = run(o);
        ASSERT_NE(e, nullptr);
        const auto& ctx = as_cpp_engine(e).context();
        ASSERT_FALSE(ctx.options.heat_transport);
        ASSERT_TRUE(HasLidUnit(ctx));
        const auto& st = ctx.lid_layer_state;
        bool any_age = false;
        for (int u = 0; u < st.n_units; ++u)
            for (int k = 0; k < kNL; ++k)
                if (st.value[st.layer_index(
                        u, static_cast<openswmm::LidLayer>(k), kAge)] > 0.0)
                    any_age = true;
        EXPECT_TRUE(any_age)
            << "adding the temperature row broke the age row — the stride "
               "widened but the age indices did not follow";
        swmm_engine_destroy(e);
    }
    {   // BOTH — the only configuration in which the two initialisers run
        // against the same block, and therefore the only one where `resize`
        // differs from `ensureSized` at all. Neither leg above reaches it:
        // heat-only never runs the age initialiser, age-only never runs the
        // heat one, and `resize` sizes correctly in both. With both on, the
        // second initialiser to run wipes what the first seeded.
        //
        // Measured with `resize` restored: SOIL age 17842.60 -> 17681.34 s,
        // STORAGE and drain 17917.45 -> 17750.90, and the subcatchment's
        // runoff age 16453.76 -> 16337.80. Silent, and every gate in this
        // file and in A4's passed through it.
        auto ages = [](bool heat, double (&out)[kNL], double* drain) {
            Opts o{}; o.water_age = true; o.heat = heat;
            SWMM_Engine e = run(o);
            if (e == nullptr) return false;
            const auto& ctx = as_cpp_engine(e).context();
            const auto& st  = ctx.lid_layer_state;
            if (!st.active()) { swmm_engine_destroy(e); return false; }
            for (int k = 0; k < kNL; ++k)
                out[k] = st.value[st.layer_index(
                    0, static_cast<openswmm::LidLayer>(k), kAge)];
            *drain = st.drain_value[static_cast<std::size_t>(st.n_species) *
                                    0 + static_cast<std::size_t>(kAge)];
            swmm_engine_destroy(e);
            return true;
        };
        auto ctx_age_init = []() {
            Opts o{}; o.water_age = true; o.heat = false;
            SWMM_Engine e = run(o);
            if (e == nullptr) return 0.0;
            const double a = as_cpp_engine(e).context().water_age_config
                                 .global_age[static_cast<int>(
                                     openswmm::WaterAgeSource::INITIAL_STATE)];
            swmm_engine_destroy(e);
            return a;
        };
        double age_only[kNL] = {}, age_both[kNL] = {};
        double drain_only = 0.0, drain_both = 0.0;
        ASSERT_TRUE(ages(false, age_only, &drain_only));
        ASSERT_TRUE(ages(true,  age_both, &drain_both));

        // SETUP: the ages must be non-trivial, or "unchanged" is vacuous.
        ASSERT_GT(age_only[static_cast<int>(openswmm::LidLayer::STORAGE)],
                  0.0)
            << "the age-only run produced no storage-layer age";
        // SETUP, and the reason this leg was blind on the deck as delivered:
        // the wipe replaces the seed with 0, so it can only be seen if the
        // seed is NOT 0. `initLidLayerAge` seeds from the INITIAL_STATE
        // source, and the config carried no such row.
        ASSERT_GT(ctx_age_init(), 0.0)
            << "the age config seeds INITIAL_STATE at 0, so a wipe of that "
               "seed writes the same value it replaces and this leg cannot "
               "observe it";

        for (int k = 0; k < kNL; ++k)
            EXPECT_DOUBLE_EQ(age_both[k], age_only[k])
                << "layer " << k << ": enabling HEAT_TRANSPORT changed the "
                   "water AGE from " << age_only[k] << " s to "
                << age_both[k] << " s. Heat writes a different species row "
                   "and must not perturb the age row at all; a difference "
                   "here means one initialiser re-sized the shared block and "
                   "wiped what the other had seeded.";
        EXPECT_DOUBLE_EQ(drain_both, drain_only)
            << "the drain's AGE moved when heat was enabled";
    }
}

// ---------------------------------------------------------------------------
// Gate 3 — the drain leaves at the STORAGE temperature. Retires the marker
//          on HeatSource::RAINFALL that said LID drains borrow the rain's.
// ---------------------------------------------------------------------------
TEST(HeatLidTest, TheDrainLeavesAtTheStorageTemperatureNotTheRains) {
    Opts o{};
    o.rain_c = 5.0;
    o.init_c = 25.0;   // distinct, so "carried" and "borrowed" differ
    SWMM_Engine e = run(o);
    ASSERT_NE(e, nullptr);
    const auto& ctx = as_cpp_engine(e).context();
    const auto& st  = ctx.lid_layer_state;
    ASSERT_TRUE(HasLidUnit(ctx));

    const double drain = st.drain_value[
        static_cast<std::size_t>(st.n_species) * 0 +
        static_cast<std::size_t>(kTemp)];
    const double storage = st.value[st.layer_index(
        0, openswmm::LidLayer::STORAGE, kTemp)];

    EXPECT_NEAR(drain, storage, 1.0e-12)
        << "the drain reports " << drain << " C while the storage layer it "
           "draws from is at " << storage << " C";
    EXPECT_NE(drain, o.rain_c)
        << "the drain is still reporting the configured RAINFALL "
           "temperature — the H5 marker on HeatSource::RAINFALL was retired "
           "in the comment but not in the code";
    swmm_engine_destroy(e);
}

// ---------------------------------------------------------------------------
// Gate 4 — CONDUCTION CONSERVES ENERGY, in two parts. The bracketing half
//          asserts every layer stays inside the band its sources span. The
//          LEDGER half actually computes the column's heat content at two
//          times and requires it not to move. As delivered this gate had
//          only the first half and its name promised the second, so an
//          inter-layer flux that was not equal-and-opposite passed it.
//          Neither half needs a reference value.
// ---------------------------------------------------------------------------
TEST(HeatLidTest, ConductionConservesTheColumnsHeatContent) {
    Opts o{};
    o.conduction = true;
    o.surface = false;
    o.radiative = false;
    o.end_min = 30;          // short: advection must not dominate the ledger
    SWMM_Engine e = run(o);
    ASSERT_NE(e, nullptr);
    const auto& ctx = as_cpp_engine(e).context();
    const auto& st  = ctx.lid_layer_state;
    ASSERT_TRUE(HasLidUnit(ctx));
    ASSERT_TRUE(ctx.heat_config.layer_conduction)
        << "LAYER_CONDUCTION did not parse — this gate would be asserting "
           "that a disabled operator conserves energy, which is vacuous";
    ASSERT_FALSE(ctx.heat_config.surface_exchange);
    ASSERT_FALSE(ctx.heat_config.radiative_exchange);

    // Every layer must be finite and inside the band the sources span. With
    // no atmospheric term, no source is outside [rain, initial].
    const double lo = std::min(o.rain_c, o.init_c) - 1.0e-6;
    const double hi = std::max(o.rain_c, o.init_c) + 1.0e-6;
    const auto& gsoa = as_cpp_engine(e).lid().group(0);
    double cap_total = 0.0;
    for (int k = 0; k < kNL; ++k) {
        const double t = st.value[st.layer_index(
            0, static_cast<openswmm::LidLayer>(k), kTemp)];
        EXPECT_TRUE(std::isfinite(t)) << "layer " << k << " is not finite";
        EXPECT_GE(t, lo) << "layer " << k << " at " << t
                         << " C is below every source in the model";
        EXPECT_LE(t, hi) << "layer " << k << " at " << t
                         << " C is above every source in the model. "
                            "Conduction can only move layers TOWARD each "
                            "other; a value outside the source band means "
                            "the operator is creating energy";
        cap_total += tr::lidLayerHeatCapacity(ctx, gsoa, 0, k);
    }
    ASSERT_GT(cap_total, 0.0)
        << "every layer reports zero heat capacity, so the conservation "
           "claim above is about an empty column";
    swmm_engine_destroy(e);

    // ---- The LEDGER. Everything above brackets the answer; none of it
    //      computes the column's heat content, so an inter-layer flux that
    //      is not equal-and-opposite passes it. Measured with the `+= h`
    //      dropped from one side: the column MANUFACTURES energy, its
    //      content climbing 49.87 -> 51.74 MJ/m2 between these two times
    //      while the correct form drifts -0.014%.
    //
    //      A LID column is a flow-through element, so it cannot be closed
    //      exactly from a deck: water carries heat across its boundary
    //      whenever anything drains. The storm is stopped, the underdrain
    //      is zeroed and the outfall no longer routes back, which leaves
    //      only residual seepage — small enough that a 1% band separates
    //      conservation from its absence by more than two orders.
    auto content = [](int end_min, double* out) {
        Opts o{};
        o.conduction = true;
        o.surface = false;
        o.radiative = false;
        o.rain_c = 2.0;
        o.init_c = 30.0;             // a real gradient for conduction to move
        o.rain_stop_min = 10;        // and then nothing enters
        o.drain_coeff = 0.0;         // and nothing leaves through the drain
        o.outfall_routes_back = false;
        o.end_min = end_min;
        SWMM_Engine e2 = run(o);
        if (e2 == nullptr) return false;
        const auto& c   = as_cpp_engine(e2).context();
        const auto& st2 = c.lid_layer_state;
        if (!st2.active()) { swmm_engine_destroy(e2); return false; }
        const auto& g2 = as_cpp_engine(e2).lid().group(0);
        double total = 0.0;
        for (int k = 0; k < kNL; ++k)
            total += tr::lidLayerHeatCapacity(c, g2, 0, k) *
                     st2.value[st2.layer_index(
                         0, static_cast<openswmm::LidLayer>(k), kTemp)];
        *out = total;
        swmm_engine_destroy(e2);
        return true;
    };
    double c90 = 0.0, c120 = 0.0;
    ASSERT_TRUE(content(90, &c90));
    ASSERT_TRUE(content(120, &c120));
    ASSERT_GT(std::fabs(c90), 0.0)
        << "the column carries no heat content, so the ledger is vacuous";
    EXPECT_LT(std::fabs(c120 - c90) / std::fabs(c90), 0.01)
        << "the column's heat content moved from " << c90 << " to " << c120
        << " J/m2 (" << (100.0 * (c120 - c90) / c90) << "%) over half an "
           "hour in which no flux module was enabled, the storm was over, "
           "the underdrain was shut and the outfall did not return. "
           "Conduction only moves heat BETWEEN layers, so the total cannot "
           "change: an inter-layer term that is not applied equal and "
           "opposite creates or destroys energy.";
}

// ---------------------------------------------------------------------------
// Gate 5 — conduction MOVES heat, and in the right direction. Distinguishes
//          "conserves energy" from "does nothing", which also conserves it.
// ---------------------------------------------------------------------------
TEST(HeatLidTest, ConductionNarrowsTheSpreadBetweenLayers) {
    auto spread = [](bool conduction, double* out_lo, double* out_hi) {
        Opts o{};
        o.conduction = conduction;
        o.rain_c = 2.0;     // cold rain onto a warm column: a real gradient
        o.init_c = 30.0;
        o.end_min = 30;
        SWMM_Engine e = run(o);
        if (e == nullptr) return false;
        const auto& ctx = as_cpp_engine(e).context();
        const auto& st  = ctx.lid_layer_state;
        if (!st.active()) { swmm_engine_destroy(e); return false; }
        double lo = 1.0e30, hi = -1.0e30;
        for (int k = 0; k < kNL; ++k) {
            const double t = st.value[st.layer_index(
                0, static_cast<openswmm::LidLayer>(k), kTemp)];
            lo = std::min(lo, t);
            hi = std::max(hi, t);
        }
        *out_lo = lo; *out_hi = hi;
        swmm_engine_destroy(e);
        return true;
    };

    double off_lo = 0, off_hi = 0, on_lo = 0, on_hi = 0;
    ASSERT_TRUE(spread(false, &off_lo, &off_hi));
    ASSERT_TRUE(spread(true,  &on_lo,  &on_hi));

    // SETUP: without conduction there must BE a gradient, or there is
    // nothing for conduction to narrow and the gate proves nothing.
    ASSERT_GT(off_hi - off_lo, 0.1)
        << "the advection-only column spans only " << (off_hi - off_lo)
        << " C, so this deck has no gradient for conduction to act on";

    EXPECT_LT(on_hi - on_lo, off_hi - off_lo)
        << "conduction ON spans " << (on_hi - on_lo) << " C against "
        << (off_hi - off_lo) << " C with it OFF — the operator is not "
           "moving heat between layers at all";
}

// ---------------------------------------------------------------------------
// Gate 6 — a dry or absent layer takes the D-H5c policy, not zero.
// ---------------------------------------------------------------------------
TEST(HeatLidTest, DryLayersTakeTheDeckPolicy) {
    auto perv_of = [](const char* policy, double* out) {
        Opts o{};
        o.dry_policy = policy;
        o.end_min = 120;      // long dry tail after a 5-minute storm
        o.air_f   = 95.0;     // 35 C: far from both 20 C and the last wet
        SWMM_Engine e = run(o);
        if (e == nullptr) return false;
        const auto& ctx = as_cpp_engine(e).context();
        const auto& st  = ctx.lid_layer_state;
        if (!st.active()) { swmm_engine_destroy(e); return false; }
        // PAVEMENT is absent on a bioretention cell — the clearest dry case.
        *out = st.value[st.layer_index(0, openswmm::LidLayer::PAVEMENT,
                                       kTemp)];
        swmm_engine_destroy(e);
        return true;
    };

    double hold = 0, air = 0, dflt = 0;
    ASSERT_TRUE(perv_of("HOLD", &hold));
    ASSERT_TRUE(perv_of("AIR", &air));
    ASSERT_TRUE(perv_of("DEFAULT", &dflt));

    EXPECT_NEAR(dflt, openswmm::HeatConfigData::kDefaultTemp, 1.0e-9)
        << "DEFAULT did not fall to kDefaultTemp on an absent layer";
    EXPECT_NEAR(air, 35.0, 0.5)
        << "AIR did not track the air temperature (95 F = 35 C)";
    EXPECT_GT(std::fabs(hold - dflt), 1.0e-6)
        << "HOLD and DEFAULT agree — the policy is not reaching the LID "
           "dry branch, only the watershed one";
}

// ---------------------------------------------------------------------------
// Gate 7 — the LID underdrain is now a counted run-on contributor. This is
//          the item H5a recorded as owed, and it closes the pair invariant.
// ---------------------------------------------------------------------------
TEST(HeatLidTest, TheUnderdrainContributesToRunonTemperature) {
    Opts o{};
    SWMM_Engine e = run(o);
    ASSERT_NE(e, nullptr);
    const auto& ctx = as_cpp_engine(e).context();
    ASSERT_TRUE(HasLidUnit(ctx));
    ASSERT_GT(ctx.n_subcatches(), 0);

    // The rate accumulator is zeroed each runoff step after consumption, so
    // what is asserted here is the INVARIANT, not a snapshot value: the
    // counted rate may never exceed the total run-on. H5a divides by the
    // counted rate, so a contributor added to the numerator without the
    // denominator would show up here as a rate exceeding the total.
    const auto& hs = ctx.heat_state;
    for (std::size_t i = 0; i < hs.subcatch_runon_temp_rate.size() &&
                            i < ctx.subcatches.runon_inflow.size(); ++i)
        EXPECT_LE(hs.subcatch_runon_temp_rate[i],
                  ctx.subcatches.runon_inflow[i] + 1.0e-9)
            << "subcatchment " << i << ": the run-on rate whose temperature "
               "is counted (" << hs.subcatch_runon_temp_rate[i]
            << ") exceeds the total run-on ("
            << ctx.subcatches.runon_inflow[i]
            << "). A contributor is being added to the numerator without "
               "its rate, which is A3's defect inverted";
    swmm_engine_destroy(e);
}

// ---------------------------------------------------------------------------
// Gate 8 — HEAT_TRANSPORT off touches nothing.
// ---------------------------------------------------------------------------
TEST(HeatLidTest, HeatOffLeavesTheTemperatureRowUntouched) {
    Opts o{}; o.water_age = true; o.heat = false;
    SWMM_Engine e = run(o);
    ASSERT_NE(e, nullptr);
    const auto& ctx = as_cpp_engine(e).context();
    ASSERT_FALSE(ctx.options.heat_transport);
    const auto& st = ctx.lid_layer_state;
    ASSERT_TRUE(HasLidUnit(ctx));
    for (int u = 0; u < st.n_units; ++u)
        for (int k = 0; k < kNL; ++k)
            EXPECT_EQ(st.value[st.layer_index(
                          u, static_cast<openswmm::LidLayer>(k), kTemp)], 0.0)
                << "the temperature row was written on a deck with "
                   "HEAT_TRANSPORT off (lesson 52)";
    swmm_engine_destroy(e);
}

// ---------------------------------------------------------------------------
// Gate 9 — a DRY-BUT-PRESENT layer still conducts, and is NOT reset by the
//          dry policy. The gate for the `live[k]` / `mass[k]` conflation.
//
//          `live[k]` asks "does this layer hold water"; conduction and the
//          D-H5c policy both need "does it have thermal mass". A buried
//          layer is a matrix with water in its voids — drained, it still has
//          rho_s*cp_s and still conducts.
//
// @par Reaching the state at all, which took three decks
//      `live[k]` is `v_old > tiny || v_in > tiny` — depth OR INFLOW. A deck
//      whose storage depth reads 0 is therefore not enough, and the first
//      two attempts here both passed vacuously on that mistake:
//
//      * With the outfall routing its discharge back onto S1, storage never
//        even emptied: 5.06e-4 ft after three hours.
//      * With that closed, depth reached exactly 0 — but `in_stor` sat at
//        9.9e-7, SIX ORDERS above the 1e-12 threshold, because the
//        subcatchment keeps trickling into the unit long after the storm.
//        Every falsifier was inert, and not because the assertions were
//        weak: the branch was never entered.
//
//      `FromImp = 0` is what closes it. The unit then receives only rain on
//      its own footprint, so when the storm stops every inflow is exactly
//      zero, the soil settles to field capacity and storage reaches the
//      dry-but-present state the fix is about. SETUP 2 asserts the whole
//      predicate — depth AND inflow — so this cannot silently regress.
// ---------------------------------------------------------------------------
TEST(HeatLidTest, ADrainedLayerStillConductsAndIsNotResetByThePolicy) {
    auto build = [](int end_min) {
        Opts o{};
        o.conduction = true;
        o.surface = true;        // drive the column, or nothing moves
        o.dry_policy = "DEFAULT";
        o.air_f = 104.0;         // 40 C — far from kDefaultTemp
        o.rain_c = 4.0;
        o.init_c = 34.0;
        o.rain_stop_min = 10;
        o.from_imperv = 0;       // see the note above: this is the whole trick
        o.outfall_routes_back = false;
        o.end_min = end_min;
        return o;
    };
    SWMM_Engine e = run(build(180));
    ASSERT_NE(e, nullptr);
    const auto& ctx  = as_cpp_engine(e).context();
    const auto& st   = ctx.lid_layer_state;
    const auto& gsoa = as_cpp_engine(e).lid().group(0);
    ASSERT_TRUE(HasLidUnit(ctx));
    ASSERT_TRUE(ctx.heat_config.layer_conduction);
    ASSERT_TRUE(ctx.heat_config.surface_exchange);

    const int kStorI = static_cast<int>(openswmm::LidLayer::STORAGE);

    // SETUP 1 — storage is PRESENT: it has a thickness, hence a matrix and a
    // heat capacity, whatever water it holds.
    ASSERT_GT(gsoa.stor_thick[0], 0.0);
    ASSERT_GT(tr::lidLayerHeatCapacity(ctx, gsoa, 0, kStorI), 0.0)
        << "the storage layer reports zero heat capacity despite having a "
           "thickness — the capacity expression, not the index set, is wrong";

    // SETUP 2 — and it is DRY by the predicate the code actually uses:
    // no stored water AND no inflow. Asserting depth alone is what let two
    // earlier versions of this gate pass without entering the branch.
    ASSERT_LT(gsoa.stor_depth[0], 1.0e-9)
        << "storage still holds " << gsoa.stor_depth[0] << " ft";
    ASSERT_LT(gsoa.in_stor[0], 1.0e-12)
        << "storage is still receiving " << gsoa.in_stor[0] << " ft/s of "
           "inflow, so `live` is TRUE and the dry-but-present branch is "
           "never entered. Depth alone is not the predicate — do NOT relax "
           "this, close the inflow instead (FromImp = 0)";

    const double storage = st.value[st.layer_index(
        0, openswmm::LidLayer::STORAGE, kTemp)];
    const double soil = st.value[st.layer_index(
        0, openswmm::LidLayer::SOIL, kTemp)];
    const double surface = st.value[st.layer_index(
        0, openswmm::LidLayer::SURFACE, kTemp)];

    // SETUP 3 — the neighbour must differ from the policy value, or "reset
    // to 20" and "conducted to 20" coincide.
    ASSERT_GT(std::fabs(soil - openswmm::HeatConfigData::kDefaultTemp), 0.5)
        << "soil sits at " << soil << " C, within half a degree of "
           "kDefaultTemp — a reset and a conduction result would be "
           "indistinguishable here";

    // (a) NOT RESET — the dry-policy half. Measured on the defect: 20.004
    //     against 28.339.
    //     NEAR, not EQUAL: the policy writes kDefaultTemp into the solve's
    //     right-hand side, and the conduction step then pulls it partway
    //     back within the same step, so the reset lands at 20.004 rather
    //     than 20. An exact `EXPECT_NE` passes on that and did.
    EXPECT_GT(std::fabs(storage - openswmm::HeatConfigData::kDefaultTemp), 0.5)
        << "the drained storage layer sits at " << storage << " C, within "
           "half a degree of kDefaultTemp. It has a matrix, a heat capacity "
           "and a neighbour at " << soil << " C, so its temperature is a "
           "physical state governed by conduction, not a D-H5c case — the "
           "policy is resetting it every step";

    // (b) STILL IN THE SOLVE — the conduction half, which (a) cannot see:
    //     dropping the layer from the tridiagonal system leaves it FROZEN at
    //     whatever it held when it went dry, which is not kDefaultTemp
    //     either. A layer still being conducted with keeps moving; an
    //     excluded one is bit-identical between two end times. Measured:
    //     28.31646534 -> 28.33914633 coupled, and 28.30066495 -> 28.30066495
    //     excluded.
    SWMM_Engine e2 = run(build(120));
    ASSERT_NE(e2, nullptr);
    const auto& st2 = as_cpp_engine(e2).context().lid_layer_state;
    const double storage_earlier = st2.value[st2.layer_index(
        0, openswmm::LidLayer::STORAGE, kTemp)];
    swmm_engine_destroy(e2);
    EXPECT_NE(storage, storage_earlier)
        << "the drained storage layer reads " << storage << " C at 180 min "
           "and exactly the same at 120 min. A layer with thermal mass and a "
           "neighbour at a different temperature must keep exchanging with "
           "it; a frozen value means it was dropped from the coupled solve";

    // (c) And the SURFACE layer keeps the WATER test (§4.2): dry, it is
    //     ponded water over a face and genuinely has no thermal mass, so the
    //     policy governs it. Making `mass[]` uniform gives it an invented
    //     capacity and it equilibrates with the column instead — measured
    //     29.54 against the policy's 20.
    ASSERT_LT(gsoa.surf_depth[0], 1.0e-9)
        << "the surface layer is not dry, so this leg is about nothing";
    EXPECT_EQ(surface, openswmm::HeatConfigData::kDefaultTemp)
        << "the dry surface layer reports " << surface << " C rather than "
           "the policy value. It is ponded water over a face: dry it has no "
           "matrix and no thermal mass, and giving it one lets a layer with "
           "cap ~ 0 hit the solver's fallback and equilibrate instantly with "
           "whatever is below it";
    swmm_engine_destroy(e);
}
