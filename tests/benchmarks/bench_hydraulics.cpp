/**
 * @file bench_hydraulics.cpp
 * @brief Benchmark: SIMD vs scalar hydraulic routing kernels.
 *
 * @see src/engine/math/SIMD.hpp (Phase 2)
 * @see src/engine/hydraulics/DynamicWave.hpp (Phase 2)
 */

#include <benchmark/benchmark.h>
#include <vector>
#include <cmath>
#include <numeric>

static constexpr int N_LINKS = 1024;

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
