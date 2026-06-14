/**
 * @file test_concurrent_engines.cpp
 * @brief Thread-safety verification: two SWMM_Engine instances on separate threads.
 *
 * @details Creates two independent engine instances, runs them concurrently
 *          on separate std::threads with the same input model, and verifies
 *          that each concurrent run produces identical results to a
 *          single-threaded baseline.
 *
 * @see docs/thread_safety_verification.md
 * @ingroup engine_unit_tests
 */

#include <gtest/gtest.h>
#include <openswmm/engine/openswmm_engine.h>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

// ============================================================================
// Tolerances (matching regression suite)
// ============================================================================

constexpr double ABS_TOL = 0.001;
constexpr double REL_TOL = 0.001;  // 0.1%

// ============================================================================
// Captured time series for one run
// ============================================================================

struct TimeStep {
    double elapsed;
    std::vector<double> node_depths;
    std::vector<double> link_flows;
};

struct RunResult {
    int error_code = 0;
    std::vector<TimeStep> steps;
};

// ============================================================================
// Run one engine to completion and capture per-step results
// ============================================================================

RunResult RunEngine(const std::string& inp,
                    const std::string& rpt,
                    const std::string& out) {
    RunResult result;

    SWMM_Engine engine = swmm_engine_create();
    if (!engine) {
        result.error_code = -1;
        return result;
    }

    result.error_code = swmm_engine_open(engine, inp.c_str(), rpt.c_str(),
                                          out.c_str(), nullptr);
    if (result.error_code != SWMM_OK) {
        swmm_engine_destroy(engine);
        return result;
    }

    result.error_code = swmm_engine_initialize(engine);
    if (result.error_code != SWMM_OK) {
        swmm_engine_close(engine);
        swmm_engine_destroy(engine);
        return result;
    }

    result.error_code = swmm_engine_start(engine, 0);
    if (result.error_code != SWMM_OK) {
        swmm_engine_end(engine);
        swmm_engine_close(engine);
        swmm_engine_destroy(engine);
        return result;
    }

    // Query model dimensions via the node/link count API
    int n_nodes = swmm_node_count(engine);
    int n_links = swmm_link_count(engine);
    if (n_nodes < 0 || n_links < 0) {
        result.error_code = SWMM_ERR_BADHANDLE;
        swmm_engine_end(engine);
        swmm_engine_close(engine);
        swmm_engine_destroy(engine);
        return result;
    }

    double elapsed = 0.0;
    while (true) {
        int err = swmm_engine_step(engine, &elapsed);
        if (err != SWMM_OK) {
            result.error_code = err;
            break;
        }
        if (elapsed <= 0.0) break;

        TimeStep ts;
        ts.elapsed = elapsed;
        ts.node_depths.resize(static_cast<size_t>(n_nodes));
        ts.link_flows.resize(static_cast<size_t>(n_links));

        // Read node depths
        for (int i = 0; i < n_nodes; ++i) {
            double val = 0.0;
            swmm_node_get_depth(engine, i, &val);
            ts.node_depths[static_cast<size_t>(i)] = val;
        }

        // Read link flows
        for (int i = 0; i < n_links; ++i) {
            double val = 0.0;
            swmm_link_get_flow(engine, i, &val);
            ts.link_flows[static_cast<size_t>(i)] = val;
        }

        result.steps.push_back(std::move(ts));
    }

    swmm_engine_end(engine);
    swmm_engine_close(engine);
    swmm_engine_destroy(engine);

    return result;
}

// ============================================================================
// Compare two run results within tolerance
// ============================================================================

void CompareResults(const RunResult& a, const RunResult& b,
                    const std::string& label) {
    ASSERT_EQ(a.error_code, SWMM_OK) << label << ": run A failed";
    ASSERT_EQ(b.error_code, SWMM_OK) << label << ": run B failed";
    ASSERT_EQ(a.steps.size(), b.steps.size())
        << label << ": step count mismatch";

    for (size_t s = 0; s < a.steps.size(); ++s) {
        const auto& sa = a.steps[s];
        const auto& sb = b.steps[s];

        ASSERT_NEAR(sa.elapsed, sb.elapsed, 1e-12)
            << label << " step " << s << ": elapsed time mismatch";

        ASSERT_EQ(sa.node_depths.size(), sb.node_depths.size());
        for (size_t n = 0; n < sa.node_depths.size(); ++n) {
            double ref = std::abs(sa.node_depths[n]);
            double tol = std::max(ABS_TOL, ref * REL_TOL);
            EXPECT_NEAR(sa.node_depths[n], sb.node_depths[n], tol)
                << label << " step " << s << " node " << n;
        }

        ASSERT_EQ(sa.link_flows.size(), sb.link_flows.size());
        for (size_t l = 0; l < sa.link_flows.size(); ++l) {
            double ref = std::abs(sa.link_flows[l]);
            double tol = std::max(ABS_TOL, ref * REL_TOL);
            EXPECT_NEAR(sa.link_flows[l], sb.link_flows[l], tol)
                << label << " step " << s << " link " << l;
        }
    }
}

}  // namespace

// ============================================================================
// Test: concurrent engines produce same results as sequential baselines
// ============================================================================

TEST(ConcurrentEngines, TwoInstancesDeterministic) {
    // Locate input model
    std::string inp = "site_drainage_model.inp";
    if (!fs::exists(inp)) {
        GTEST_SKIP() << "site_drainage_model.inp not found in working directory";
    }

    // --- Phase 1: Sequential baselines ---
    RunResult baseline_a = RunEngine(inp,
                                      "baseline_a.rpt",
                                      "baseline_a.out");
    ASSERT_EQ(baseline_a.error_code, SWMM_OK) << "Baseline A failed";
    ASSERT_GT(baseline_a.steps.size(), 0u) << "Baseline A produced no steps";

    RunResult baseline_b = RunEngine(inp,
                                      "baseline_b.rpt",
                                      "baseline_b.out");
    ASSERT_EQ(baseline_b.error_code, SWMM_OK) << "Baseline B failed";

    // Sanity: sequential runs should be identical
    CompareResults(baseline_a, baseline_b, "sequential-check");

    // --- Phase 2: Concurrent runs ---
    RunResult concurrent_a, concurrent_b;

    std::thread thread_a([&]() {
        concurrent_a = RunEngine(inp,
                                  "concurrent_a.rpt",
                                  "concurrent_a.out");
    });

    std::thread thread_b([&]() {
        concurrent_b = RunEngine(inp,
                                  "concurrent_b.rpt",
                                  "concurrent_b.out");
    });

    thread_a.join();
    thread_b.join();

    // --- Phase 3: Compare concurrent results to baselines ---
    CompareResults(baseline_a, concurrent_a, "baseline-vs-concurrent-A");
    CompareResults(baseline_a, concurrent_b, "baseline-vs-concurrent-B");

    // Cleanup temp files
    for (const char* f : {"baseline_a.rpt", "baseline_a.out",
                           "baseline_b.rpt", "baseline_b.out",
                           "concurrent_a.rpt", "concurrent_a.out",
                           "concurrent_b.rpt", "concurrent_b.out"}) {
        std::remove(f);
    }
}
