// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>
#include "core/PerfTimers.hpp"
#include <thread>
#include <vector>

namespace perf = openswmm::perf;

TEST(PerfCounters, ParallelNestedBatchesRetainEveryOperation) {
    if (!perf::enabled()) GTEST_SKIP() << "run with OPENSWMM_PERF=1";
    perf::reset_fv();
    constexpr int workers = 8;
    constexpr int operations = 100000;
    std::vector<std::thread> threads;
    for (int worker = 0; worker < workers; ++worker) {
        threads.emplace_back([] {
            perf::CounterBatch batch;
            for (int i = 0; i < operations; ++i) {
                perf::count(perf::n_fv_geom_area);
                perf::count(perf::n_fv_geom_width, 2);
            }
            {
                perf::CounterBatch nested;
                perf::count(perf::n_fv_alg_resid, 7);
            }
            perf::count(perf::n_fv_geom_i1, 3);
        });
    }
    for (auto& thread : threads) thread.join();
    EXPECT_EQ(perf::value(perf::n_fv_geom_area), workers * operations);
    EXPECT_EQ(perf::value(perf::n_fv_geom_width), 2 * workers * operations);
    EXPECT_EQ(perf::value(perf::n_fv_alg_resid), 7 * workers);
    EXPECT_EQ(perf::value(perf::n_fv_geom_i1), 3 * workers);
}

TEST(PerfCounters, BatchExitRestoresSerialDestinationAndResetClearsTotals) {
    if (!perf::enabled()) GTEST_SKIP() << "run with OPENSWMM_PERF=1";
    perf::reset_fv();
    {
        perf::CounterBatch batch;
        perf::count(perf::n_fv_substep, 2);
        {
            perf::CounterBatch nested;
            perf::count(perf::n_fv_substep, 3);
        }
        perf::count(perf::n_fv_substep, 5);
    }
    perf::count(perf::n_fv_substep, 7);
    EXPECT_EQ(perf::value(perf::n_fv_substep), 17);
    perf::reset_fv();
    for (std::size_t i = 0; i < perf::n_fv_counter_count; ++i)
        EXPECT_EQ(perf::value(static_cast<perf::FvCounter>(i)), 0);
}

TEST(PerfCounters, DisabledProfilingDoesNotAccumulate) {
    if (perf::enabled()) GTEST_SKIP() << "run without OPENSWMM_PERF";
    perf::reset_fv();
    perf::count(perf::n_fv_substep, 10);
    {
        perf::CounterBatch batch;
        perf::count(perf::n_fv_geom_area, 100);
    }
    EXPECT_EQ(perf::value(perf::n_fv_substep), 0);
    EXPECT_EQ(perf::value(perf::n_fv_geom_area), 0);
}
