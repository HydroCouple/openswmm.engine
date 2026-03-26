/**
 * @file bench_timeseries_lookup.cpp
 * @brief Benchmark: cursor-based vs linear-scan time series lookup.
 *
 * @details Compares lookup performance for a 10,000-point rainfall time series
 *          using the new bidirectional cursor vs a linear scan from index 0.
 *
 * @see src/engine/data/TableData.hpp (Phase 2)
 * @see Legacy reference: src/solver/table.c — table_lookup()
 */

#include <benchmark/benchmark.h>
#include <vector>
#include <numeric>
#include <cmath>

// TODO Phase 2:
// #include "data/TableData.hpp"

static constexpr int N_POINTS = 10000;

static void BM_LinearScanLookup(benchmark::State& state) {
    // Simulate legacy linear scan from index 0 for each query
    std::vector<double> x(N_POINTS), y(N_POINTS);
    std::iota(x.begin(), x.end(), 0.0);
    for (int i = 0; i < N_POINTS; i++) y[i] = std::sin(i * 0.01);

    for (auto _ : state) {
        // Sequential queries (monotonically increasing) — worst case for linear scan
        double sum = 0.0;
        for (int q = 0; q < N_POINTS; q++) {
            // Simulate linear scan from 0:
            int idx = 0;
            while (idx < N_POINTS - 1 && x[idx + 1] <= static_cast<double>(q)) idx++;
            benchmark::DoNotOptimize(sum += y[idx]);
        }
    }
    state.SetLabel("linear scan, 10k points, sequential queries");
}
BENCHMARK(BM_LinearScanLookup)->Unit(benchmark::kMicrosecond);

static void BM_CursorLookup(benchmark::State& state) {
    // Simulate cursor-based lookup — O(1) amortized for sequential queries
    for (auto _ : state) {
        // TODO Phase 2: use openswmm::table_lookup_cursor()
        benchmark::DoNotOptimize(0.0);
    }
    state.SetLabel("cursor lookup, 10k points, sequential queries");
}
BENCHMARK(BM_CursorLookup)->Unit(benchmark::kMicrosecond);

BENCHMARK_MAIN();
