/**
 * @file      bench_hydraulics.cpp
 * @brief     Benchmark: SIMD vs scalar hydraulic routing kernels.
 *
 * @see src/engine/math/SIMD.hpp (Phase 2)
 * @see src/engine/hydraulics/DynamicWave.hpp (Phase 2)
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include <benchmark/benchmark.h>
#include <vector>
#include <cmath>
#include <numeric>

static constexpr int N_LINKS = 1024;

/**
 * @brief Time scalar depth-times-area assignments across 1024 synthetic links.
 *
 * Allocates/initializes three vectors before the state loop, writes each flow every iteration and
 * prevents optimization. Uses fixed dimensionless synthetic values and sets the scalar label; no
 * routing simulation is run.
 *
 * @param[in,out] state Borrowed Google Benchmark iteration/counter state; labels/counters or skip
 *     status are written during this call.
 */
static void BM_ScalarFlowUpdate(benchmark::State& state) {
    std::vector<double> flows(N_LINKS, 1.0);
    std::vector<double> depths(N_LINKS, 0.5);
    std::vector<double> areas(N_LINKS, 2.0);

    for (auto _ : state) {
        for (int i = 0; i < N_LINKS; i++) {
            benchmark::DoNotOptimize(flows[i] = depths[i] * areas[i]);
        }
    }
    state.SetLabel("scalar, 1024 links");
}
BENCHMARK(BM_ScalarFlowUpdate)->Unit(benchmark::kNanosecond);

/**
 * @brief Measure the current SIMD-benchmark placeholder loop.
 *
 * Allocates the synthetic vectors, then only protects flows[0] against optimization. The SIMD
 * multiply call remains commented out, so this timing does not measure a SIMD hydraulic kernel.
 *
 * @param[in,out] state Borrowed Google Benchmark iteration/counter state; labels/counters or skip
 *     status are written during this call.
 */
static void BM_SimdFlowUpdate(benchmark::State& state) {
    std::vector<double> flows(N_LINKS, 1.0);
    std::vector<double> depths(N_LINKS, 0.5);
    std::vector<double> areas(N_LINKS, 2.0);

    for (auto _ : state) {
        // TODO Phase 2: use openswmm::simd::multiply()
        // openswmm::simd::multiply(depths.data(), areas.data(), flows.data(), N_LINKS);
        benchmark::DoNotOptimize(flows[0]);
    }
    state.SetLabel("SIMD, 1024 links");
}
BENCHMARK(BM_SimdFlowUpdate)->Unit(benchmark::kNanosecond);

BENCHMARK_MAIN();
