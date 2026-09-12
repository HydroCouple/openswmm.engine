/**
 * @file test_exfiltration.cpp
 * @brief Unit and benchmark tests for storage-node exfiltration.
 *
 * @details Exercises the dedicated storage exfiltration solver, which couples
 *          storage geometry to bottom and bank Green-Ampt seepage.
 *
 * @see src/engine/hydraulics/Exfiltration.hpp
 * @see src/engine/hydraulics/Node.hpp
 */

#include <gtest/gtest.h>

#include <cmath>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "core/SimulationContext.hpp"
#include "hydraulics/Exfiltration.hpp"
#include "hydraulics/Node.hpp"

using namespace openswmm;

#ifndef BENCHMARK_DATA_DIR
#  define BENCHMARK_DATA_DIR ""
#endif

namespace {

struct ExfilBenchmarkRow {
    double t_s;
    double exfil_rate_cfs;
    double exfil_cumul_ft3;
};

static std::vector<ExfilBenchmarkRow> load_exfil_benchmark(const std::string& path) {
    std::vector<ExfilBenchmarkRow> rows;
    std::ifstream in(path);
    if (!in.is_open()) return rows;

    std::string line;
    bool header_seen = false;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        if (!header_seen) {
            header_seen = true;
            continue;
        }

        std::istringstream ss(line);
        std::string tok;
        double values[3] = {};
        int col = 0;
        while (std::getline(ss, tok, ',') && col < 3) {
            values[col++] = std::stod(tok);
        }
        if (col >= 3) {
            rows.push_back({values[0], values[1], values[2]});
        }
    }
    return rows;
}

static SimulationContext make_fixed_stage_storage_context() {
    SimulationContext ctx;
    auto& nodes = ctx.nodes;
    nodes.resize(1);

    nodes.full_depth[0] = 5.0;
    // Relational side-table: create the storage row and populate its config.
    const int sr = ctx.node_subtypes.set_node_type(nodes, 0, NodeType::STORAGE);
    auto& S = ctx.node_subtypes.storages;
    const auto ur = static_cast<std::size_t>(sr);
    S.curve[ur] = -1;
    S.a[ur] = 100.0;  // A(d) = 50 + 100 * d
    S.b[ur] = 1.0;
    S.c[ur] = 50.0;
    nodes.full_volume[0] = openswmm::node::getVolume(nodes, 0, nodes.full_depth[0],
                                                     nullptr, 0, &ctx.node_subtypes);

    nodes.depth[0] = 2.0;
    nodes.volume[0] = 1.0e9;  // effectively fixed-stage for the benchmark horizon

    S.exfil_suction[ur] = 6.0;  // 0.5 ft in project rain-depth units
    S.exfil_ksat[ur] = 4.32;    // in/hr -> internal Ks = 1.0e-4 ft/s
    S.exfil_imd[ur] = 0.2;
    return ctx;
}

}  // namespace

TEST(StorageExfilGeometry, FunctionalLinearInitUsesBottomAndBankPartition) {
    SimulationContext ctx = make_fixed_stage_storage_context();

    exfil::ExfilSolver solver;
    solver.init(ctx);

    auto& state = solver.state();
    ASSERT_EQ(state.count, 1);
    EXPECT_NEAR(state.btm_area[0], 50.0, 1e-12);
    EXPECT_NEAR(state.bank_min_depth[0], 0.0, 1e-12);
    // For functional (analytical) storage curves the bank extends to the SWMM
    // sentinel BIG = 1.0e10, meaning "no upper limit on bank depth/area".
    EXPECT_GT(state.bank_max_depth[0], 1.0e9);  // BIG = 1.0e10
    EXPECT_GT(state.bank_max_area[0], 1.0e9);   // BIG = 1.0e10
}

// Fixed-stage analytical storage exfiltration benchmark.
//
// Storage shape: A(d) = 50 + 100 d, evaluated at fixed depth d=2 ft.
// Bottom area = 50 ft^2, bank area = 250 ft^2, bank depth = 1 ft.
// Both Green-Ampt states are forced onto the saturated branch so the exact
// cumulative infiltration for each component is given by the implicit
// Green-Ampt relation t(F) = [F - c1 ln(1 + F/c1)] / Ks.
TEST(StorageExfilGeometry, FixedStageGreenAmptGeometryBenchmark) {
    const std::string path = std::string(BENCHMARK_DATA_DIR)
        + "/manufactured/exfil-storage-geometry-greenampt/reference.csv";

    auto rows = load_exfil_benchmark(path);
    if (rows.empty()) {
        GTEST_SKIP() << "Benchmark data not found: " << path;
    }

    SimulationContext ctx = make_fixed_stage_storage_context();
    exfil::ExfilSolver solver;
    solver.init(ctx);
    solver.state().btm_ga[0].saturated = true;
    solver.state().bank_ga[0].saturated = true;

    // Legacy grnampt_getF2 cannot start its Newton solve from F = 0 (the
    // derivative term 1 - c1/(f2 + c1) vanishes there) and falls back to the
    // Ks-only floor F1 + Ks*dt for that step — a state legacy itself never
    // reaches, since it enters the saturated branch with F >= Fs > 0. Seed
    // both components with the exact F at the benchmark's first row (the
    // implicit relation inverted by bisection) and check from the second.
    auto exact_F = [](double c1, double ks, double t) {
        double lo = 0.0, hi = 1.0;                 // g(hi) > 0 for these c1
        for (int it = 0; it < 200; ++it) {
            double mid = 0.5 * (lo + hi);
            double g = mid - c1 * std::log(1.0 + mid / c1) - ks * t;
            if (g < 0.0) lo = mid; else hi = mid;
        }
        return 0.5 * (lo + hi);
    };
    {
        auto& btm  = solver.state().btm_ga[0];
        auto& bank = solver.state().bank_ga[0];
        const double c1_btm  = (btm.S  + 2.0) * btm.IMD;    // bottom under 2 ft
        const double c1_bank = (bank.S + 1.0) * bank.IMD;   // bank under d/2
        btm.F  = exact_F(c1_btm,  btm.Ks,  rows[1].t_s);  btm.Fu  = btm.F;
        bank.F = exact_F(c1_bank, bank.Ks, rows[1].t_s);  bank.Fu = bank.F;
    }

    double cumulative_loss = rows[1].exfil_cumul_ft3;
    double max_rate_err = 0.0;
    double max_cumulative_err = 0.0;
    double prev_t = rows[1].t_s;

    for (size_t i = 2; i < rows.size(); ++i) {
        double dt = rows[i].t_s - prev_t;
        solver.computeAll(ctx, dt);

        // Guard: computeAll must not update depth (fixed-stage benchmark relies
        // on depth remaining 2.0 ft so that bank_area = 250 ft² throughout).
        // Total exfil over 600 s is ~72 ft³; the 1e9 ft³ volume makes drift
        // negligible, but this assertion catches any accidental depth update.
        EXPECT_NEAR(ctx.nodes.depth[0], 2.0, 1e-6)
            << "Storage depth drifted from 2.0 ft at step " << i;

        double step_loss = ctx.node_subtypes.storages.exfil_loss[
            static_cast<std::size_t>(ctx.node_subtypes.storage_row(0))];
        cumulative_loss += step_loss;
        max_rate_err = std::max(max_rate_err,
            std::abs(step_loss / dt - rows[i].exfil_rate_cfs));
        max_cumulative_err = std::max(max_cumulative_err,
            std::abs(cumulative_loss - rows[i].exfil_cumul_ft3));
        prev_t = rows[i].t_s;
    }

    EXPECT_LT(max_rate_err, 5e-5)
        << "Exfiltration-rate max error " << max_rate_err
        << " cfs exceeds 5e-5 cfs (benchmark: " << path << ")";
    EXPECT_LT(max_cumulative_err, 3e-3)
        << "Cumulative exfil max error " << max_cumulative_err
        << " ft^3 exceeds 3e-3 ft^3";
}