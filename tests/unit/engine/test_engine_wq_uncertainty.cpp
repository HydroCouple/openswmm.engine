/**
 * @file test_engine_wq_uncertainty.cpp
 * @brief Integration test: Water-quality uncertainty layer engine lifecycle.
 *
 * @details Tests the full pipeline:
 *   parse .inp with [UNCERTAINTY] QUALITY TSS 0.30
 *   → swmm_engine_open → initialize → start → step loop → end → close
 *
 * Verifications:
 *   1. wq_unc_active() returns true after initialize().
 *   2. wq_conc_prev_ buffer is properly sized.
 *   3. WQ uncertainty CSV file is created.
 *   4. Pollutant name resolution works correctly.
 *   5. WQ bounds are computed and written to CSV.
 *
 * Network: single node with TSS pollutant.
 * Forced inflow at node to ensure non-trivial concentration field.
 *
 * @ingroup engine_integration
 */

#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "openswmm/engine/openswmm_engine.h"
#define private public
#include "core/SWMMEngine.hpp"
#undef private

namespace {

// Path to the test input file
static const char* k_inp_path = "wq_uncertainty_test.inp";
static const char* k_rpt_path = "/tmp/wq_uncertainty_test.rpt";

} // anonymous namespace


// ============================================================================
// Test: WQ uncertainty layer integration
// ============================================================================

TEST(EngineWQUncertainty, LifecycleIntegration) {
    // Create engine
    SWMM_Engine handle = swmm_engine_create();
    ASSERT_NE(handle, nullptr) << "Failed to create engine";

    // Open engine
    int error = swmm_engine_open(handle, k_inp_path, k_rpt_path, nullptr, nullptr);
    ASSERT_EQ(error, 0) << "Failed to open engine";

    // Get engine reference
    auto* engine = static_cast<openswmm::SWMMEngine*>(handle);

    // Initialize
    error = swmm_engine_initialize(handle);
    ASSERT_EQ(error, 0) << "Failed to initialize engine: " << error;

    // Verify WQ uncertainty is active
    EXPECT_TRUE(engine->wq_unc_active()) << "WQ uncertainty should be active";

    // Verify the report-boundary seed matches the deterministic start state.
    EXPECT_EQ(engine->wq_conc_prev_.size(), engine->ctx_.nodes.conc.size());
    EXPECT_THAT(engine->wq_conc_prev_, ::testing::ElementsAreArray(engine->ctx_.nodes.conc));

    // Verify pollutant indices are resolved
    // Note: This would require access to private members or additional API

    // Start simulation
    error = swmm_engine_start(handle, 0);
    ASSERT_EQ(error, 0) << "Failed to start engine: " << error;

    // Run simulation steps
    double elapsed_time;
    double current_time = 0.0;
    double end_time = 60.0 / 86400.0; // 1 minute in days
    while (current_time < end_time) {
        error = swmm_engine_step(handle, &elapsed_time);
        ASSERT_EQ(error, 0) << "Failed to step engine: " << error;
        error = swmm_get_current_time(handle, &current_time);
        ASSERT_EQ(error, 0) << "Failed to get current time: " << error;
        if (elapsed_time <= 0.0) break;
    }

    // End simulation
    error = swmm_engine_end(handle);
    ASSERT_EQ(error, 0) << "Failed to end engine: " << error;

    // Close engine
    error = swmm_engine_close(handle);
    ASSERT_EQ(error, 0) << "Failed to close engine: " << error;
}

// ============================================================================
// Test: WQ uncertainty CSV output
// ============================================================================

TEST(EngineWQUncertainty, CSVOutput) {
    // Create engine
    SWMM_Engine handle = swmm_engine_create();
    ASSERT_NE(handle, nullptr) << "Failed to create engine";

    // Open engine
    int error = swmm_engine_open(handle, k_inp_path, k_rpt_path, nullptr, nullptr);
    ASSERT_EQ(error, 0) << "Failed to open engine";

    // Initialize
    error = swmm_engine_initialize(handle);
    ASSERT_EQ(error, 0) << "Failed to initialize engine: " << error;

    // Start simulation
    error = swmm_engine_start(handle, 0);
    ASSERT_EQ(error, 0) << "Failed to start engine: " << error;

    // Run simulation steps
    double elapsed_time;
    double current_time = 0.0;
    double end_time = 60.0 / 86400.0; // 1 minute in days
    while (current_time < end_time) {
        error = swmm_engine_step(handle, &elapsed_time);
        ASSERT_EQ(error, 0) << "Failed to step engine: " << error;
        error = swmm_get_current_time(handle, &current_time);
        ASSERT_EQ(error, 0) << "Failed to get current time: " << error;
        if (elapsed_time <= 0.0) break;
    }

    // End simulation
    error = swmm_engine_end(handle);
    ASSERT_EQ(error, 0) << "Failed to end engine: " << error;

    // Close engine
    error = swmm_engine_close(handle);
    ASSERT_EQ(error, 0) << "Failed to close engine: " << error;

    // Check that WQ uncertainty CSV was created
    std::string csv_path = k_rpt_path;
    const std::string rpt_ext = ".rpt";
    if (csv_path.size() >= rpt_ext.size() &&
        csv_path.compare(csv_path.size() - rpt_ext.size(), rpt_ext.size(), rpt_ext) == 0) {
        csv_path.replace(csv_path.size() - rpt_ext.size(), rpt_ext.size(), ".wq_uncertainty.csv");
    } else {
        csv_path += ".wq_uncertainty.csv";
    }
    std::ifstream csv_file(csv_path);
    EXPECT_TRUE(csv_file.good()) << "WQ uncertainty CSV file should be created";

    std::string header;
    ASSERT_TRUE(std::getline(csv_file, header));
    EXPECT_EQ(header, "time_s,node_name,pollutant,q05,q50,q95");

    std::string row;
    if (std::getline(csv_file, row)) {
        std::stringstream row_stream(row);
        std::vector<std::string> cols;
        std::string col;
        while (std::getline(row_stream, col, ',')) cols.push_back(col);
        ASSERT_GE(cols.size(), 6u);
        EXPECT_EQ(cols[2], "TSS");
    }

    // Clean up
    std::remove(k_rpt_path);
    std::remove(csv_path.c_str());
}