/**
 * @file test_engine_final_boundary_stall.cpp
 * @brief Regression test for PR 11 — final-boundary time-stepping stall.
 *
 * @details With adaptive routing active, swmm_engine_step can stall making
 *          sub-nanosecond time progress at the final report boundary due to
 *          accumulated floating-point error in current_time. The fix snaps
 *          current_time to the total duration when within 1 ms of the end.
 *
 *          This test uses a fixture whose END_TIME lands exactly on a report
 *          boundary (60 s simulation, 30 s report step) and asserts:
 *          1. The simulation terminates (elapsed_time reaches 0.0).
 *          2. The step count is sane (not thousands of micro-steps).
 *          3. The final current_time matches the expected duration.
 *
 * @ingroup engine_integration
 */

#include <gtest/gtest.h>

#include "openswmm/engine/openswmm_engine.h"
#include "core/SWMMEngine.hpp"

#include <cmath>
#include <cstdio>
#include <string>

namespace {

static const char* k_inp_path = "pr11_stall_test.inp";
static const char* k_rpt_path = "/tmp/pr11_stall_test.rpt";

} // anonymous namespace

// ============================================================================
// Test: Final-boundary stall regression
// ============================================================================

TEST(EngineFinalBoundaryStall, TerminatesAtExactBoundary) {
    SWMM_Engine handle = swmm_engine_create();
    ASSERT_NE(handle, nullptr) << "Failed to create engine";

    int error = swmm_engine_open(handle, k_inp_path, k_rpt_path, nullptr, nullptr);
    ASSERT_EQ(error, 0) << "Failed to open engine: " << error;

    error = swmm_engine_initialize(handle);
    ASSERT_EQ(error, 0) << "Failed to initialize engine: " << error;

    error = swmm_engine_start(handle, 0);
    ASSERT_EQ(error, 0) << "Failed to start engine: " << error;

    // Run the simulation, counting steps. The stall bug would cause
    // thousands of sub-nanosecond steps before termination.
    double elapsed_time = 1.0;
    int step_count = 0;
    const int MAX_SANE_STEPS = 1000;  // 60s sim / 0.5s min step = 120 max

    while (elapsed_time > 0.0) {
        error = swmm_engine_step(handle, &elapsed_time);
        ASSERT_EQ(error, 0) << "Failed to step engine at step " << step_count;
        ++step_count;

        // Stall guard: if we exceed a sane step count, the stall bug is present
        ASSERT_LT(step_count, MAX_SANE_STEPS)
            << "Simulation stalled: " << step_count << " steps for a 60s run "
            << "indicates sub-nanosecond time-stepping stall at the final boundary";

        if (elapsed_time <= 0.0) break;
    }

    // Assert simulation terminated
    EXPECT_EQ(elapsed_time, 0.0) << "Simulation should report elapsed=0 on completion";

    // Assert step count is reasonable (60s / 10s routing_step = ~6 steps minimum)
    EXPECT_GT(step_count, 1) << "Simulation should take at least a few steps";
    EXPECT_LT(step_count, MAX_SANE_STEPS) << "Step count should be sane";

    // Verify the engine reached the correct end time
    auto* engine = static_cast<openswmm::SWMMEngine*>(handle);
    double current_time = engine->context().current_time;
    // Total duration = 60 seconds (01/01/2025 00:00:00 → 00:01:00)
    EXPECT_NEAR(current_time, 60.0, 0.001)
        << "Final current_time should be 60 seconds (within 1 ms tolerance)";

    error = swmm_engine_end(handle);
    ASSERT_EQ(error, 0) << "Failed to end engine";

    error = swmm_engine_close(handle);
    ASSERT_EQ(error, 0) << "Failed to close engine";

    std::remove(k_rpt_path);
}

// ============================================================================
// Test: Short simulation with END_TIME on report boundary
// ============================================================================

TEST(EngineFinalBoundaryStall, ShortRunTerminatesCleanly) {
    SWMM_Engine handle = swmm_engine_create();
    ASSERT_NE(handle, nullptr);

    int error = swmm_engine_open(handle, k_inp_path, k_rpt_path, nullptr, nullptr);
    ASSERT_EQ(error, 0);

    error = swmm_engine_initialize(handle);
    ASSERT_EQ(error, 0);

    error = swmm_engine_start(handle, 0);
    ASSERT_EQ(error, 0);

    // Run to completion
    double elapsed_time = 1.0;
    int step_count = 0;
    while (elapsed_time > 0.0 && step_count < 500) {
        error = swmm_engine_step(handle, &elapsed_time);
        ASSERT_EQ(error, 0);
        ++step_count;
    }

    // Must have terminated (elapsed_time == 0.0), not hit the guard
    EXPECT_EQ(elapsed_time, 0.0)
        << "Simulation should terminate naturally, not via step guard";

    swmm_engine_end(handle);
    swmm_engine_close(handle);
    std::remove(k_rpt_path);
}