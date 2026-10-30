/**
 * @file test_conduit_losses.cpp
 * @brief Regression gates for OpenSWMM #2, USEPA #216 and PR #234.
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license Apache-2.0
 */
#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <tuple>
#include "core/SWMMEngine.hpp"
#include <openswmm/legacy/engine/openswmm_solver.h>

namespace {
const std::filesystem::path out = std::filesystem::path(ISSUE2_DATA_DIR) / "runs";
constexpr double m3ToFt3 = 35.31466672148859;

std::string capDeck(int barrels, double dt, double seep, double evap,
                    const std::string& routing = "DYNWAVE") {
    std::ostringstream f;
    f << "[OPTIONS]\nFLOW_UNITS LPS\nFLOW_ROUTING " << routing << "\n"
      << "START_DATE 01/01/2025\nEND_DATE 01/01/2025\nEND_TIME 01:00:00\n"
      << "REPORT_STEP 00:05:00\nROUTING_STEP " << dt << "\nVARIABLE_STEP 0\n"
      << "LENGTHENING_STEP 0\nTHREADS 1\n"
      // Large storage areas hold a constant shallow stage. The NODE budget
      // cannot mask a broken conduit-volume cap in this fixture.
      << "[STORAGE]\nJ1 0 1 0.001 FUNCTIONAL 0 0 1000000000\n"
      << "J2 0 1 0.001 FUNCTIONAL 0 0 1000000000\n"
      << "[OUTFALLS]\nO0 0 FREE NO\n"
      << "[CONDUITS]\nC1 J1 J2 400 0.01 0 0 0 0\n"
      << "[XSECTIONS]\nC1 RECT_OPEN 1 5 0 0 " << barrels << "\n"
      << "[LOSSES]\nC1 0 0 0 NO " << seep << "\n"
      << "[EVAPORATION]\nCONSTANT " << evap << "\n";
    return f.str();
}

std::string writeDeck(const std::string& name, const std::string& contents) {
    std::filesystem::create_directories(out);
    const auto path = out / (name + ".inp");
    std::ofstream(path) << contents;
    return path.string();
}

struct Totals {
    double loss = 0, volume = 0, evaporation = 0, seepage = 0, error = 0;
};

Totals capRun(const std::string& path, bool legacy) {
    Totals t;
    double elapsed = 0;
    if (legacy) {
        EXPECT_EQ(swmm_open(path.c_str(), (path + ".legacy.rpt").c_str(), ""), 0);
        EXPECT_EQ(swmm_start(0), 0);
        EXPECT_EQ(swmm_step(&elapsed), 0);
        t.volume = swmm_getValue(swmm_LINK_VOLUME, 0) * m3ToFt3;
        EXPECT_EQ(swmm_end(), 0);
        swmm_RoutingTotals totals{};
        EXPECT_EQ(swmm_getSystemRoutingTotals(&totals), 0);
        t.evaporation = totals.evapLoss;
        t.seepage = totals.seepLoss;
        EXPECT_EQ(swmm_close(), 0);
    } else {
        openswmm::SWMMEngine engine;
        EXPECT_EQ(engine.open(path.c_str(), (path + ".v6.rpt").c_str(), ""), 0);
        EXPECT_EQ(engine.initialize(), 0);
        EXPECT_EQ(engine.start(0), 0);
        EXPECT_EQ(engine.step(&elapsed), 0);
        t.volume = engine.context().links.volume[0];
        EXPECT_EQ(engine.end(), 0);
        EXPECT_EQ(engine.report(), 0);
        t.evaporation = engine.context().mass_balance.routing_evap_loss;
        t.seepage = engine.context().mass_balance.routing_seep_loss;
    }
    // Legacy's first routing-total sample covers half a step (old rate zero).
    if (legacy) {
        t.evaporation *= 2.0;
        t.seepage *= 2.0;
    }
    t.loss = t.evaporation + t.seepage;
    return t;
}

using CapCase = std::tuple<int, double, double, double>;
class ConduitVolumeCap : public ::testing::TestWithParam<CapCase> {};
TEST_P(ConduitVolumeCap, BothEnginesRespectPerBarrelInventory) {
    const auto [barrels, dt, seep, evap] = GetParam();
    const auto path = writeDeck("cap_" + std::to_string(barrels) + "_" + std::to_string(dt) + "_" +
                                    std::to_string(seep) + "_" + std::to_string(evap),
                                capDeck(barrels, dt, seep, evap));
    const auto a = capRun(path, true), b = capRun(path, false);
    // Analytic rectangle: 2000 m2 per barrel; seep in mm/hr, evap in mm/day.
    const double potential = 2000.0 * (seep / 3.6e6 + evap / 8.64e7) * barrels * dt * m3ToFt3;
    const double inventory = 2.0 * barrels * m3ToFt3;
    for (const auto& t : {a, b}) {
        EXPECT_NEAR(t.volume, inventory, 1e-3 * inventory);
        EXPECT_NEAR(t.loss, std::min(potential, inventory), 1e-6 * inventory);
        EXPECT_LE(t.loss, inventory * (1.0 + 1e-12));
        if (seep > 0 && evap > 0)
            EXPECT_NEAR(t.evaporation / t.seepage, (evap / 24.0) / seep, 1e-9);
    }
    EXPECT_NEAR(a.loss, b.loss, 1e-8);
}
INSTANTIATE_TEST_SUITE_P(SeepageAndEvaporation, ConduitVolumeCap,
                         ::testing::Values(CapCase{1, 100, 60, 0}, CapCase{2, 100, 60, 0},
                                           CapCase{3, 100, 60, 0}, CapCase{2, 100, 0, 1440},
                                           CapCase{2, 100, 60, 1440}, CapCase{2, 0.01, 60, 0},
                                           CapCase{2, 100, 0, 0}));

TEST(ConduitVolumeCapFV, AggregateInventoryIsNotMultipliedAgain) {
    for (int barrels : {1, 2, 3}) {
        const auto path =
            writeDeck("fv_cap_" + std::to_string(barrels), capDeck(barrels, 100, 60, 0, "FV"));
        const auto t = capRun(path, false);
        const double inventory = 2.0 * barrels * m3ToFt3;
        EXPECT_GT(t.loss, 0.0);
        EXPECT_LE(t.loss, inventory * (1.0 + 1e-6));
    }
}

class ConduitLossContinuity : public ::testing::TestWithParam<int> {};
TEST_P(ConduitLossContinuity, LowInflowBothEngines) {
    std::ifstream source(std::filesystem::path(ISSUE2_DATA_DIR) / "reported.inp");
    ASSERT_TRUE(source.is_open());
    std::string deck((std::istreambuf_iterator<char>(source)), {});
    auto replace = [&](const std::string& from, const std::string& to) {
        std::size_t pos = 0;
        while ((pos = deck.find(from, pos)) != std::string::npos) {
            deck.replace(pos, from.size(), to);
            pos += to.size();
        }
    };
    switch (GetParam()) {
    case 1:
        replace("ROUTING_STEP 1\n", "ROUTING_STEP 0.1\n");
        break;
    case 2: {
        const auto a = deck.find("[TIMESERIES]"), b = deck.find("[REPORT]");
        deck.replace(a, b - a, "[TIMESERIES]\nChicago 00:00 1.400468\nChicago 24:00 1.400468\n");
        break;
    }
    case 3:
        replace("RECT_OPEN 1 5 0 0 1", "RECT_OPEN 1 5 0 0 2");
        break;
    case 4:
        replace("C2 0 0 0 NO 60", "C2 0 0 0 NO 60\nC3 0 0 0 NO 60");
        break;
    case 5:
        replace("THREADS 1", "THREADS 2");
        break;
    case 6:
        replace("NO 60", "NO 0");
        break;
    }
    const auto name = "low_inflow_" + std::to_string(GetParam());
    const auto path = writeDeck(name, deck);
    const auto report = (out / (name + "-legacy.rpt")).string();
    ASSERT_EQ(swmm_open(path.c_str(), report.c_str(), ""), 0);
    ASSERT_EQ(swmm_start(0), 0);
    double elapsed;
    do {
        ASSERT_EQ(swmm_step(&elapsed), 0);
    } while (elapsed > 0);
    ASSERT_EQ(swmm_end(), 0);
    swmm_RoutingTotals a{};
    ASSERT_EQ(swmm_getSystemRoutingTotals(&a), 0);
    ASSERT_EQ(swmm_close(), 0);
    // Dry/wet transitions retain DW discretization error; bound it explicitly.
    // Do not treat a successful fix of the loss budget as exact mass closure.
    const double tolerance = (GetParam() >= 2 && GetParam() <= 4) ? 3.0 : 1.5;
    EXPECT_LT(std::abs(a.pctError), tolerance); // reported case previously -304.589%
    EXPECT_LE(a.seepLoss, a.exInflow + a.initStorage);

    openswmm::SWMMEngine engine;
    ASSERT_EQ(engine.open(path.c_str(), (out / (name + "-v6.rpt")).string().c_str(), ""), 0);
    ASSERT_EQ(engine.initialize(), 0);
    ASSERT_EQ(engine.start(0), 0);
    do {
        ASSERT_EQ(engine.step(&elapsed), 0);
    } while (elapsed > 0);
    ASSERT_EQ(engine.end(), 0);
    ASSERT_EQ(engine.report(), 0);
    const auto& b = engine.context().mass_balance;
    EXPECT_LT(std::abs(b.routing_error() * 100.0), tolerance);
    EXPECT_NEAR(a.seepLoss, b.routing_seep_loss, 1e-5 * std::max(1.0, a.seepLoss));
    EXPECT_NEAR(a.evapLoss, b.routing_evap_loss, 1e-5 * std::max(1.0, a.evapLoss));
    EXPECT_NEAR(a.outflow, b.routing_outflow, 1e-5 * std::max(1.0, a.outflow));
}
INSTANTIATE_TEST_SUITE_P(ReportedAndEdgeCases, ConduitLossContinuity, ::testing::Range(0, 7));
} // namespace
