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
#include "../../include/openswmm/engine/openswmm_output.h"
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
    // Load all report-period results from a binary .out file into `results`.
    // Works for both legacy (swmm_run) and new-engine (swmm_engine_run) output
    // because both engines write the same binary format.
    void LoadFromOutputFile(const std::string& path, std::vector<TimeStep>& results) {
        SWMM_Output out = swmm_output_open(path.c_str());
        ASSERT_NE(out, nullptr) << "Failed to open output file: " << path;

        int n_nodes  = swmm_output_get_node_count(out);
        int n_links  = swmm_output_get_link_count(out);
        int nperiods = swmm_output_get_period_count(out);
        int rs_sec   = swmm_output_get_report_step(out);
        double rs_day = rs_sec / 86400.0;

        std::vector<float> node_buf(static_cast<size_t>(n_nodes));
        std::vector<float> link_buf(static_cast<size_t>(n_links));

        for (int p = 0; p < nperiods; ++p) {
            TimeStep ts;
            ts.time = (p + 1) * rs_day;  // elapsed days from simulation start

            swmm_output_get_node_result(out, p, SWMM_OUT_NODE_DEPTH, node_buf.data());
            swmm_output_get_link_result(out, p, SWMM_OUT_LINK_FLOW,  link_buf.data());

            ts.node_depths.assign(node_buf.begin(), node_buf.end());
            ts.link_flows.assign(link_buf.begin(), link_buf.end());
            results.push_back(ts);
        }

        swmm_output_close(out);
    }

    void RunLegacy(const std::string& inp) {
        std::string rpt = "/tmp/regression_legacy.rpt";
        std::string out = "/tmp/regression_legacy.out";

        int err = swmm_run(inp.c_str(), rpt.c_str(), out.c_str());
        ASSERT_EQ(err, 0) << "Legacy engine failed on " << inp;

        LoadFromOutputFile(out, legacy_results_);
    }

    void RunNew(const std::string& inp) {
        std::string rpt = "/tmp/regression_new.rpt";
        std::string out = "/tmp/regression_new.out";

        int err = swmm_engine_run(inp.c_str(), rpt.c_str(), out.c_str(), nullptr);
        ASSERT_EQ(err, 0) << "New engine run failed";

        LoadFromOutputFile(out, new_results_);
    }

    void CompareOutputs(double abs_tol, double rel_tol) {
        // Times are in days. Match periods by time value with a 5-second tolerance
        // to handle any residual clock-offset differences.
        const double time_tol = 5.0 / 86400.0;  // 5 seconds in days

        size_t li = 0, ni = 0;
        size_t matched = 0;
        int max_mismatches = 10;
        int mismatches = 0;

        while (li < legacy_results_.size() && ni < new_results_.size()) {
            const auto& leg = legacy_results_[li];
            const auto& neo = new_results_[ni];

            double dt = leg.time - neo.time;
            if (dt > time_tol)  { ++ni; continue; }
            if (dt < -time_tol) { ++li; continue; }
            ++li; ++ni; ++matched;

            ASSERT_EQ(leg.node_depths.size(), neo.node_depths.size());
            for (size_t i = 0; i < leg.node_depths.size(); ++i) {
                double delta = std::fabs(neo.node_depths[i] - leg.node_depths[i]);
                double ref   = std::fabs(leg.node_depths[i]);
                double tol   = std::max(abs_tol, rel_tol * ref);
                if (delta > tol) {
                    EXPECT_LE(delta, tol)
                        << "Node depth mismatch at t=" << leg.time << " node " << i
                        << ": legacy=" << leg.node_depths[i]
                        << " new="    << neo.node_depths[i];
                    if (++mismatches >= max_mismatches) return;
                }
            }

            ASSERT_EQ(leg.link_flows.size(), neo.link_flows.size());
            for (size_t i = 0; i < leg.link_flows.size(); ++i) {
                double delta = std::fabs(neo.link_flows[i] - leg.link_flows[i]);
                double ref   = std::fabs(leg.link_flows[i]);
                double tol   = std::max(abs_tol, rel_tol * ref);
                if (delta > tol) {
                    EXPECT_LE(delta, tol)
                        << "Link flow mismatch at t=" << leg.time << " link " << i
                        << ": legacy=" << leg.link_flows[i]
                        << " new="    << neo.link_flows[i];
                    if (++mismatches >= max_mismatches) return;
                }
            }
        }

        ASSERT_GT(matched, 0u) << "No matching timesteps found between legacy and new engine";
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
