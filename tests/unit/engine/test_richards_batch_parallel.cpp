// SPDX-License-Identifier: Apache-2.0
#include "RichardsBatchFixture.hpp"
#include "core/ThreadInfo.hpp"
#include <gtest/gtest.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#ifdef OPENSWMM_TEST_HAS_OPENMP
#include <omp.h>
#endif
#ifndef OPENSWMM_RICHARDS_BATCH_TEST_DIR
#define OPENSWMM_RICHARDS_BATCH_TEST_DIR "tests/output/richards_batch_parallel"
#endif
using namespace openswmm;
namespace {
struct Workers {
#ifdef OPENSWMM_TEST_HAS_OPENMP
    int previous = omp_get_max_threads();
    explicit Workers(int n) { omp_set_num_threads(n); }
    ~Workers() { omp_set_num_threads(previous); }
#else
    explicit Workers(int) {}
#endif
};
void save(const std::string& name, int workers, const std::string& bytes) {
    const char* path = std::getenv("OPENSWMM_RICHARDS_BATCH_TEST_OUT");
    const std::filesystem::path folder = path ? path : OPENSWMM_RICHARDS_BATCH_TEST_DIR;
    std::filesystem::create_directories(folder);
    std::ofstream out(folder / (name + "_t" + std::to_string(workers) + ".bin"), std::ios::binary);
    out.write(bytes.data(), bytes.size());
    ASSERT_TRUE(out.good());
}
void valid(const std::vector<richards::Report>& reports) {
    for (const auto& r : reports) {
        ASSERT_TRUE(r.ok) << r.error;
        for (double value : {r.min_step, r.bottom_volume, r.evaporation, r.balance,
                             r.bottom_outflow, r.bottom_inflow, r.inflow})
            ASSERT_TRUE(std::isfinite(value));
    }
}
}
TEST(RichardsBatchParallel, CompleteHistoriesMatchSerialAcrossPackingAndGroupBoundaries) {
#ifndef OPENSWMM_TEST_HAS_OPENMP
    GTEST_SKIP() << "Worker control was not linked into this test; parallel coverage unavailable";
#endif
    if (!threadinfo::ompAvailable())
        GTEST_SKIP() << "Engine has no OpenMP support; original Richards tests cover serial kernels";
    for (int count : {0, 7, 8, 9, 63, 64, 65, 255, 256, 257, 1027})
        for (int budget : {0, 1, 2})
            for (bool mixed : {false, true}) {
                const auto name = std::to_string(count) + "_b" + std::to_string(budget) +
                                  (mixed ? "_mixed" : "_uniform");
                SCOPED_TRACE(name);
                std::string serial;
                for (int workers : {1, 4, 8}) {
                    SCOPED_TRACE(workers);
                    Workers team(workers);
                    ASSERT_EQ(threadinfo::ompMaxThreads(), workers);
                    auto columns = richards::test::columns(count, budget, mixed);
                    auto pointers = richards::test::pointers(columns);
                    detail::CompletedSourceArchive history;
                    for (int step = 0; step < 3; ++step) {
                        if (step)
                            for (std::size_t i = 0; i < columns.size(); ++i) {
                                auto& c = columns[i];
                                c.cells[1].material.alpha += .05;
                                c.cells[1].potential_et = 1.e-8 * ((i + step) % 4);
                                c.inflow = (i + step) % 2 ? 1.e-6 : 0.;
                            }
                        auto reports = richards::advance(pointers, 30.);
                        ASSERT_NO_FATAL_FAILURE(valid(reports));
                        richards::test::record(history, columns, reports);
                    }
                    if (workers == 1)
                        serial = history.bytes();
                    ASSERT_TRUE(history.bytes() == serial) << "Complete water/report history differs from serial";
                    save(name, workers, history.bytes());
                }
            }
}
TEST(RichardsBatchParallel, FailureReportPrefixWaterRollbackAndRecoveryMatchSerial) {
#ifndef OPENSWMM_TEST_HAS_OPENMP
    GTEST_SKIP() << "Worker control was not linked into this test; parallel coverage unavailable";
#endif
    if (!threadinfo::ompAvailable())
        GTEST_SKIP() << "Engine has no OpenMP support; original Richards tests cover serial kernels";
    for (int count : {257, 1027})
        for (int bad : {0, 65, count - 1, count - 2})
            for (int budget : {0, 1, 2}) {
                const auto name = "failure_" + std::to_string(count) + "_" + std::to_string(bad) +
                                  "_b" + std::to_string(budget);
                SCOPED_TRACE(name);
                std::string serial;
                for (int workers : {1, 4, 8}) {
                    SCOPED_TRACE(workers);
                    Workers team(workers);
                    ASSERT_EQ(threadinfo::ompMaxThreads(), workers);
                    auto columns = richards::test::columns(count, budget, true);
                    auto pointers = richards::test::pointers(columns);
                    const auto before = richards::test::water(columns);
                    auto& et = columns[bad].cells.back().potential_et;
                    const double original_et = et;
                    et = 1.e308;
                    auto failed = richards::advance(pointers, 30.);
                    for (const auto& r : failed) {
                        ASSERT_FALSE(r.ok);
                        ASSERT_FALSE(r.error.empty());
                    }
                    ASSERT_TRUE(richards::test::water(columns) == before) << "Caller water changed on refusal";
                    detail::CompletedSourceArchive history;
                    richards::test::record(history, columns, failed);
                    et = original_et;
                    auto recovered = richards::advance(pointers, 30.);
                    ASSERT_NO_FATAL_FAILURE(valid(recovered));
                    richards::test::record(history, columns, recovered);
                    if (workers == 1)
                        serial = history.bytes();
                    ASSERT_TRUE(history.bytes() == serial) << "Complete water/report history differs from serial";
                    save(name, workers, history.bytes());
                }
            }
}
