/**
 * @file test_regression_suite.cpp
 * @brief Regression tests: new engine vs legacy SWMM.
 *
 * @details Runs the same input model through both the legacy engine
 *          (openswmm_legacy_engine) and the new engine (openswmm_engine),
 *          then compares output time series for all nodes and links.
 *
 * @section regression_tolerance Tolerance
 *
 * - Absolute: +/-0.001 project length/flow units
 * - Relative: +/-0.1% of the reference value
 *
 * Per-model tolerances live in the table in LoadRegressionCases(); the SLOT
 * model carries a measured, wider band (see the comment there).
 *
 * @section regression_workdir Working directory
 *
 * ctest runs this binary from tests/regression/data (CMakeLists.txt), and the
 * models are resolved relative to that. If no model resolves, the parameterized
 * suite is left uninstantiated and gtest's
 * GoogleTestVerification.UninstantiatedParameterizedTestSuite check fails the
 * run -- do NOT add a --gtest_filter that excludes that check, which is how
 * this suite previously reported "Passed" while running zero test cases.
 *
 * @ingroup engine_regression
 */

#include <gtest/gtest.h>
#include <string>
#include <vector>
#include <cmath>
#include <algorithm>
#include <filesystem>
#include <cstdio>
#include <cstdlib>

#include "../../include/openswmm/engine/openswmm_engine.h"
#include "../../include/openswmm/legacy/engine/openswmm_solver.h"

namespace fs = std::filesystem;

namespace {

struct RegressionParams {
    std::string model_name;
    std::string inp_path;
    double      abs_tol;
    double      rel_tol;
};

static std::vector<RegressionParams> LoadRegressionCases(const std::string& data_dir) {
    std::vector<RegressionParams> cases;
    const struct { const char* name; const char* file; double abs_tol; double rel_tol; } known[] = {
        {"Example1", "Example1.inp", 0.001, 0.001},
        {"Example2", "Example2.inp", 0.001, 0.001},
        {"Example3", "Example3.inp", 0.001, 0.001},
        // SURCHARGE_METHOD SLOT — the only coverage of the Preissmann-slot
        // conveyance-area momentum path (issue #144). The looser tolerance is
        // measured, not guessed: the two engines run this model to 0.0073 cfs
        // of link flow and 0.0195 ft of node depth, an order above the EXTRAN
        // spread, because the slot geometry amplifies small head differences.
        {"SlotSurcharge", "slot_surcharge.inp", 0.05, 0.01},
    };
    for (const auto& c : known) {
        std::string path = data_dir + c.file;
        if (fs::exists(path)) {
            cases.push_back({c.name, path, c.abs_tol, c.rel_tol});
        }
    }
    return cases;
}

// ============================================================================
// Time series storage for comparison
// ============================================================================

struct TimeStep {
    double time;
    std::vector<double> node_depths;
    std::vector<double> link_flows;
};

// ============================================================================
// Regression test fixture
// ============================================================================

class RegressionTest : public ::testing::TestWithParam<RegressionParams> {
protected:
    void RunLegacy(const std::string& inp) {
        const auto tmp = fs::temp_directory_path();
        std::string rpt = (tmp / "regression_legacy.rpt").string();
        std::string out = (tmp / "regression_legacy.out").string();

        int err = swmm_run(inp.c_str(), rpt.c_str(), out.c_str());
        ASSERT_EQ(err, 0) << "Legacy engine failed on " << inp;

        // Read back results via legacy API
        err = swmm_open(inp.c_str(), rpt.c_str(), out.c_str());
        ASSERT_EQ(err, 0);

        // Use the API enums, never literals: the property codes below were
        // hard-coded as 96/121, which the legacy API rejects with
        // ERR_API_PROPERTY_TYPE (-999907). That went unnoticed because the
        // suite never actually ran (see the working-directory note above).
        int n_nodes = swmm_getCount(swmm_NODE);
        int n_links = swmm_getCount(swmm_LINK);

        err = swmm_start(1);
        ASSERT_EQ(err, 0);

        double t = 0.0;
        while (swmm_step(&t) == 0 && t > 0.0) {
            TimeStep ts;
            ts.time = t;
            ts.node_depths.resize(static_cast<size_t>(n_nodes));
            ts.link_flows.resize(static_cast<size_t>(n_links));

            for (int i = 0; i < n_nodes; ++i) {
                ts.node_depths[static_cast<size_t>(i)] = swmm_getValue(swmm_NODE_DEPTH, i);
            }
            for (int i = 0; i < n_links; ++i) {
                ts.link_flows[static_cast<size_t>(i)] = swmm_getValue(swmm_LINK_FLOW, i);
            }
            legacy_results_.push_back(ts);
        }

        swmm_end();
        swmm_close();
    }

    void RunNew(const std::string& inp) {
        const auto tmp = fs::temp_directory_path();
        std::string rpt = (tmp / "regression_new.rpt").string();
        std::string out = (tmp / "regression_new.out").string();

        // Full run first — mirrors how RunLegacy uses swmm_run()
        int err = swmm_engine_run(inp.c_str(), rpt.c_str(), out.c_str(), nullptr);
        ASSERT_EQ(err, 0) << "New engine run failed";

        // Re-open and step through to read per-timestep results
        SWMM_Engine e = swmm_engine_create();
        ASSERT_NE(e, nullptr);

        err = swmm_engine_open(e, inp.c_str(), rpt.c_str(), out.c_str(), nullptr);
        ASSERT_EQ(err, 0) << "New engine open failed: " << swmm_get_last_error_msg(e);

        err = swmm_engine_initialize(e);
        ASSERT_EQ(err, 0);

        err = swmm_engine_start(e, 1);
        ASSERT_EQ(err, 0);

        int n_nodes = swmm_node_count(e);
        int n_links = swmm_link_count(e);

        double t = 0.0;
        while (swmm_engine_step(e, &t) == 0 && t > 0.0) {
            TimeStep ts;
            ts.time = t;
            ts.node_depths.resize(static_cast<size_t>(n_nodes));
            ts.link_flows.resize(static_cast<size_t>(n_links));

            // Bulk read — single memcpy per array (SoA)
            swmm_node_get_depths_bulk(e, ts.node_depths.data(), n_nodes);
            swmm_link_get_flows_bulk(e, ts.link_flows.data(), n_links);

            new_results_.push_back(ts);
        }

        swmm_engine_end(e);
        swmm_engine_report(e);
        swmm_engine_close(e);
        swmm_engine_destroy(e);
    }

    void CompareOutputs(double abs_tol, double rel_tol) {
        // The two engines signal end-of-simulation one step apart: the
        // refactored engine emits a final step landing exactly on the end
        // time, while the legacy engine stops short by the remainder and
        // reports t == 0 on that call, so the collecting loop drops it.
        // (Example1: legacy ends at 1.249949884259 d, refactored at
        // 1.250000000000 d = the 01/02 06:00 end time.) Every preceding step
        // time agrees to within 1 ULP, and the reported .out series agree to
        // ~1e-6 ft / 2e-5 cfs, so this is a boundary-signalling difference
        // rather than a divergence. Allow exactly that one-step slack and
        // compare the common prefix; anything larger is a real disagreement.
        const size_t n_leg = legacy_results_.size();
        const size_t n_new = new_results_.size();
        const size_t n = std::min(n_leg, n_new);
        ASSERT_GT(n, 0u) << "No timesteps collected from one or both engines";
        const size_t slack = (n_leg > n_new) ? (n_leg - n_new) : (n_new - n_leg);
        ASSERT_LE(slack, 1u)
            << "Different number of output timesteps: legacy=" << n_leg
            << " new=" << n_new << " (only the end-of-run step may differ)";

        // The step cadence itself is part of the contract — if the engines
        // drift onto different routing steps, comparing them index-by-index
        // would silently compare unrelated instants.
        for (size_t t = 0; t < n; ++t) {
            ASSERT_NEAR(legacy_results_[t].time, new_results_[t].time, 1e-9)
                << "Routing step cadence diverged at step " << t;
        }

        int max_mismatches = 10;
        int mismatches = 0;

        for (size_t t = 0; t < n; ++t) {
            const auto& leg = legacy_results_[t];
            const auto& neo = new_results_[t];

            // Compare node depths
            ASSERT_EQ(leg.node_depths.size(), neo.node_depths.size());
            for (size_t i = 0; i < leg.node_depths.size(); ++i) {
                double delta = std::fabs(neo.node_depths[i] - leg.node_depths[i]);
                double ref = std::fabs(leg.node_depths[i]);
                double tol = std::max(abs_tol, rel_tol * ref);
                if (delta > tol) {
                    EXPECT_LE(delta, tol)
                        << "Node depth mismatch at step " << t << " node " << i
                        << ": legacy=" << leg.node_depths[i]
                        << " new=" << neo.node_depths[i];
                    if (++mismatches >= max_mismatches) return;
                }
            }

            // Compare link flows
            ASSERT_EQ(leg.link_flows.size(), neo.link_flows.size());
            for (size_t i = 0; i < leg.link_flows.size(); ++i) {
                double delta = std::fabs(neo.link_flows[i] - leg.link_flows[i]);
                double ref = std::fabs(leg.link_flows[i]);
                double tol = std::max(abs_tol, rel_tol * ref);
                if (delta > tol) {
                    EXPECT_LE(delta, tol)
                        << "Link flow mismatch at step " << t << " link " << i
                        << ": legacy=" << leg.link_flows[i]
                        << " new=" << neo.link_flows[i];
                    if (++mismatches >= max_mismatches) return;
                }
            }
        }
    }

    std::vector<TimeStep> legacy_results_;
    std::vector<TimeStep> new_results_;
};

// ============================================================================
// Parameterized test
// ============================================================================

TEST_P(RegressionTest, NewEngineMatchesLegacyWithinTolerance) {
    const auto& params = GetParam();

    if (!fs::exists(params.inp_path)) {
        GTEST_SKIP() << "Test data not found: " << params.inp_path;
    }

    RunLegacy(params.inp_path);
    RunNew(params.inp_path);
    CompareOutputs(params.abs_tol, params.rel_tol);
}

INSTANTIATE_TEST_SUITE_P(
    Examples,
    RegressionTest,
    // ctest runs this binary with its working directory set to tests/regression/data
    // (see CMakeLists.txt), so models are looked up relative to that.
    ::testing::ValuesIn(LoadRegressionCases("./")),
    [](const ::testing::TestParamInfo<RegressionParams>& info) {
        return info.param.model_name;
    }
);

// ============================================================================
// Force main under SURCHARGE_METHOD SLOT — refactored engine only
// ============================================================================
//
// A force main is full for the entire simulation, so under SLOT the slot is
// active on every step. That makes it the systematically affected case for
// the conveyance-area momentum change (issue #144) and the only coverage of
// processForceMainLink()'s conveyArea() path.
//
// It is deliberately NOT in the cross-engine table above: the legacy and
// refactored engines have a large pre-existing disagreement on force mains --
// 14.4 ft of node depth and 2.4 cfs of link flow on this very model under
// plain EXTRAN (measured 2026-08-22) -- which has nothing to do with #144 and
// would swamp any tolerance. So this asserts the properties that must hold of
// the refactored engine alone: the run completes, it conserves mass, and the
// slot is genuinely engaged.

TEST(SlotForceMain, RefactoredEngineRunsAndConservesMass) {
    const std::string inp = "./slot_force_main.inp";
    if (!fs::exists(inp)) {
        GTEST_SKIP() << "Test data not found: " << inp;
    }
    const auto tmp = fs::temp_directory_path();
    const std::string rpt = (tmp / "regression_slot_fm.rpt").string();
    const std::string out = (tmp / "regression_slot_fm.out").string();

    ASSERT_EQ(swmm_engine_run(inp.c_str(), rpt.c_str(), out.c_str(), nullptr), 0);

    // Flow routing continuity error, and evidence the pipes actually ran
    // surcharged (a force main that never fills would make this vacuous).
    std::FILE* f = std::fopen(rpt.c_str(), "r");
    ASSERT_NE(f, nullptr) << "report not written: " << rpt;
    char line[512];
    bool in_routing = false, saw_surcharge = false, saw_continuity = false;
    double continuity = 0.0;
    while (std::fgets(line, sizeof(line), f)) {
        const std::string s(line);
        if (s.find("Flow Routing Continuity") != std::string::npos) in_routing = true;
        if (in_routing && !saw_continuity &&
            s.find("Continuity Error (%)") != std::string::npos) {
            // "  Continuity Error (%) .....        -0.046" — take the last token.
            const auto end = s.find_last_not_of(" \t\r\n");
            const auto start = s.find_last_of(" \t", end) + 1;
            continuity = std::stod(s.substr(start, end - start + 1));
            saw_continuity = true;
        }
        if (s.find("Conduit Surcharge Summary") != std::string::npos) saw_surcharge = true;
    }
    std::fclose(f);

    ASSERT_TRUE(saw_continuity) << "no flow routing continuity error in report";
    EXPECT_LT(std::fabs(continuity), 1.0)
        << "flow routing continuity error " << continuity
        << " % exceeds the 1 % band for a force main under SLOT";
    EXPECT_TRUE(saw_surcharge)
        << "no Conduit Surcharge Summary — the force main never filled, so the "
           "Preissmann slot was never engaged and this test proves nothing";
}

} /* anonymous namespace */
