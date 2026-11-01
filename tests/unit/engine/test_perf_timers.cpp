// SPDX-License-Identifier: Apache-2.0

/**
 * @file      test_perf_timers.cpp
 * @brief     Tests for perf timers.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include <gtest/gtest.h>
#include "core/PerfTimers.hpp"
#include "core/ProcessPerf.hpp"
#include <thread>
#include <vector>

namespace perf = openswmm::perf;

TEST(PerfCounters, ParallelNestedBatchesRetainEveryOperation) {
    if (!perf::enabled())
        GTEST_SKIP() << "run with OPENSWMM_PERF=1";
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
    for (auto& thread : threads)
        thread.join();
    EXPECT_EQ(perf::value(perf::n_fv_geom_area), workers * operations);
    EXPECT_EQ(perf::value(perf::n_fv_geom_width), 2 * workers * operations);
    EXPECT_EQ(perf::value(perf::n_fv_alg_resid), 7 * workers);
    EXPECT_EQ(perf::value(perf::n_fv_geom_i1), 3 * workers);
}

TEST(PerfCounters, BatchExitRestoresSerialDestinationAndResetClearsTotals) {
    if (!perf::enabled())
        GTEST_SKIP() << "run with OPENSWMM_PERF=1";
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
    if (perf::enabled())
        GTEST_SKIP() << "run without OPENSWMM_PERF";
    perf::reset_fv();
    perf::count(perf::n_fv_substep, 10);
    {
        perf::CounterBatch batch;
        perf::count(perf::n_fv_geom_area, 100);
    }
    EXPECT_EQ(perf::value(perf::n_fv_substep), 0);
    EXPECT_EQ(perf::value(perf::n_fv_geom_area), 0);
}

TEST(ProcessPerf, NestedTimersBookSelfTimeAndCountCalls) {
    if (!perf::enabled())
        GTEST_SKIP() << "run with OPENSWMM_PERF=1";
    perf::reset_process();
    const auto start = perf::now();
    {
        perf::ProcessTimer parent(perf::ProcessPhase::common_source);
        {
            perf::ProcessTimer child(perf::ProcessPhase::gw_cells);
            perf::SigmaEvaluations firing;
            for (int i = 0; i < 40; ++i) firing.evaluate();
        }
    }
    const double wall = perf::since(start);
    const auto parent = std::size_t(perf::ProcessPhase::common_source);
    const auto child = std::size_t(perf::ProcessPhase::gw_cells);
    EXPECT_EQ(perf::process_calls[parent].load(), 1);
    EXPECT_EQ(perf::process_calls[child].load(), 1);
    EXPECT_GE(perf::process_seconds[parent].load(), 0);
    EXPECT_GE(perf::process_seconds[child].load(), 0);
    EXPECT_LE(perf::process_seconds[parent].load() + perf::process_seconds[child].load(), wall);
    EXPECT_EQ(perf::value(perf::ProcessCounter::sigma_evaluations), 40);
    EXPECT_EQ(perf::value(perf::ProcessCounter::sigma_firings), 1);
    EXPECT_EQ(perf::value(perf::ProcessCounter::sigma_le_64), 1);
    perf::reset_process();
    EXPECT_EQ(perf::process_calls[parent].load(), 0);
    EXPECT_EQ(perf::process_seconds[parent].load(), 0);
    EXPECT_EQ(perf::value(perf::ProcessCounter::sigma_firings), 0);
}

TEST(ProcessPerf, ParallelScopesAndExceptionExitRetainCounts) {
    if (!perf::enabled())
        GTEST_SKIP() << "run with OPENSWMM_PERF=1";
    perf::reset_process();
    std::vector<std::thread> threads;
    for (int worker = 0; worker < 8; ++worker)
        threads.emplace_back([] {
            for (int i = 0; i < 1000; ++i) {
                perf::ProcessTimer timer(perf::ProcessPhase::surface_species);
                perf::count(perf::ProcessCounter::water_trial_copies);
            }
        });
    for (auto& thread : threads) thread.join();
    EXPECT_EQ(perf::process_calls[std::size_t(perf::ProcessPhase::surface_species)].load(), 8000);
    EXPECT_EQ(perf::value(perf::ProcessCounter::water_trial_copies), 8000);
    try {
        perf::ProcessTimer timer(perf::ProcessPhase::gw_cells);
        throw 1;
    } catch (int) {}
    EXPECT_EQ(perf::process_calls[std::size_t(perf::ProcessPhase::gw_cells)].load(), 1);
}

TEST(ProcessPerf, DisabledProfilingDoesNotAccumulate) {
    if (perf::enabled())
        GTEST_SKIP() << "run without OPENSWMM_PERF";
    perf::reset_process();
    {
        perf::ProcessTimer timer(perf::ProcessPhase::gw_cells);
        perf::count(perf::ProcessCounter::water_trial_copies);
        perf::SigmaEvaluations firing;
        firing.evaluate();
    }
    EXPECT_EQ(perf::value(perf::ProcessCounter::water_trial_copies), 0);
    EXPECT_EQ(perf::value(perf::ProcessCounter::sigma_evaluations), 0);
    EXPECT_EQ(perf::process_calls[std::size_t(perf::ProcessPhase::gw_cells)].load(), 0);
    EXPECT_EQ(perf::process_seconds[std::size_t(perf::ProcessPhase::gw_cells)].load(), 0);
}

TEST(ProcessPerf, OptionalMallocProbeCountsOnlyTheMeasuredScope) {
    if (!perf::enabled() || !perf::allocation_snapshot())
        GTEST_SKIP() << "run with OPENSWMM_PERF and the optional malloc probe";
    perf::reset_process();
    {
        perf::ProcessTimer timer(perf::ProcessPhase::gw_cells);
        void* volatile memory = std::malloc(4096);
        std::free(memory);
        timer.stop();
        timer.stop(); // explicit stop and destruction cannot book twice
    }
    const auto index = std::size_t(perf::ProcessPhase::gw_cells);
    EXPECT_EQ(perf::process_calls[index].load(), 1);
    EXPECT_EQ(perf::process_allocations[index].load(), 1);
    EXPECT_EQ(perf::process_allocated_bytes[index].load(), 4096);
}

TEST(ProcessPerf, OverlappingModelsStayInvalidUntilTheEntireCohortCloses) {
    if (!perf::enabled())
        GTEST_SKIP() << "run with OPENSWMM_PERF=1";
    int a = 0, b = 0, c = 0;
    perf::begin_process_model(&a);
    {
        perf::ProcessTimer timer(perf::ProcessPhase::gw_cells);
    }
    EXPECT_EQ(perf::process_calls[std::size_t(perf::ProcessPhase::gw_cells)].load(), 1);
    perf::begin_process_model(&b);
    EXPECT_FALSE(perf::process_attribution_valid.load());
    testing::internal::CaptureStderr();
    perf::end_process_model(&a);
    perf::end_process_model(&a); // repeated close cannot unregister another model
    EXPECT_FALSE(perf::process_attribution_valid.load());
    perf::begin_process_model(&c); // still overlaps b, even though a closed
    {
        perf::ProcessTimer timer(perf::ProcessPhase::gw_cells);
        perf::count(perf::ProcessCounter::water_trial_copies);
    }
    perf::end_process_model(&b);
    perf::end_process_model(&c);
    const auto output = testing::internal::GetCapturedStderr();
    EXPECT_NE(output.find("[PERF-PROCESS-INVALID]"), std::string::npos);
    EXPECT_EQ(output.find("[PERF-GW2D]"), std::string::npos);
    EXPECT_EQ(perf::value(perf::ProcessCounter::water_trial_copies), 0);
    EXPECT_FALSE(perf::process_attribution_valid.load());
    perf::begin_process_model(&a);
    EXPECT_TRUE(perf::process_attribution_valid.load());
    EXPECT_EQ(perf::process_calls[std::size_t(perf::ProcessPhase::gw_cells)].load(), 0);
    perf::end_process_model(&a, false);
}

TEST(ProcessPerf, SimultaneousRegistrationsInvalidateOneCohort) {
    if (!perf::enabled())
        GTEST_SKIP() << "run with OPENSWMM_PERF=1";
    int models[8] = {};
    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i)
        threads.emplace_back([&, i] { perf::begin_process_model(&models[i]); });
    for (auto& thread : threads) thread.join();
    EXPECT_FALSE(perf::process_attribution_valid.load());
    for (auto& model : models) perf::end_process_model(&model, false);
    perf::begin_process_model(&models[0]);
    EXPECT_TRUE(perf::process_attribution_valid.load());
    perf::end_process_model(&models[0], false);
}

TEST(ProcessPerf, ResetWithAnActiveModelInvalidatesWithoutClearingTotals) {
    if (!perf::enabled())
        GTEST_SKIP() << "run with OPENSWMM_PERF=1";
    int model = 0;
    perf::begin_process_model(&model);
    perf::count(perf::ProcessCounter::water_trial_copies, 3);
    perf::reset_process();
    EXPECT_FALSE(perf::process_attribution_valid.load());
    EXPECT_EQ(perf::value(perf::ProcessCounter::water_trial_copies), 3);
    perf::begin_process_model(&model);
    EXPECT_FALSE(perf::process_attribution_valid.load());
    perf::end_process_model(&model, false);
    perf::reset_process();
    EXPECT_TRUE(perf::process_attribution_valid.load());
    EXPECT_EQ(perf::value(perf::ProcessCounter::water_trial_copies), 0);
}
