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
 * @ingroup engine_regression
 */

#include <gtest/gtest.h>
#include <string>
#include <vector>
#include <cmath>
#include <filesystem>
#include <cstdio>

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
        std::string rpt = "/tmp/regression_legacy.rpt";
        std::string out = "/tmp/regression_legacy.out";

        int err = swmm_run(inp.c_str(), rpt.c_str(), out.c_str());
        ASSERT_EQ(err, 0) << "Legacy engine failed on " << inp;

        // Read back results via legacy API
        err = swmm_open(inp.c_str(), rpt.c_str(), out.c_str());
        ASSERT_EQ(err, 0);

        int n_nodes = swmm_getCount(2);  // NODE type = 2
        int n_links = swmm_getCount(3);  // LINK type = 3

        err = swmm_start(1);
        ASSERT_EQ(err, 0);

        double t = 0.0;
        while (swmm_step(&t) == 0 && t > 0.0) {
            TimeStep ts;
            ts.time = t;
            ts.node_depths.resize(static_cast<size_t>(n_nodes));
            ts.link_flows.resize(static_cast<size_t>(n_links));

            for (int i = 0; i < n_nodes; ++i) {
                ts.node_depths[static_cast<size_t>(i)] = swmm_getValue(96, i);  // NODE_DEPTH = 96
            }
            for (int i = 0; i < n_links; ++i) {
                ts.link_flows[static_cast<size_t>(i)] = swmm_getValue(121, i);  // LINK_FLOW = 121
            }
            legacy_results_.push_back(ts);
        }

        swmm_end();
        swmm_close();
    }

    void RunNew(const std::string& inp) {
        std::string rpt = "/tmp/regression_new.rpt";
        std::string out = "/tmp/regression_new.out";

        SWMM_Engine e = swmm_engine_create();
        ASSERT_NE(e, nullptr);

        int err = swmm_engine_open(e, inp.c_str(), rpt.c_str(), out.c_str());
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
        ASSERT_EQ(legacy_results_.size(), new_results_.size())
            << "Different number of output timesteps";

        int max_mismatches = 10;
        int mismatches = 0;

        for (size_t t = 0; t < legacy_results_.size(); ++t) {
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
    ::testing::ValuesIn(LoadRegressionCases("data/")),
    [](const ::testing::TestParamInfo<RegressionParams>& info) {
        return info.param.model_name;
    }
);

} /* anonymous namespace */
