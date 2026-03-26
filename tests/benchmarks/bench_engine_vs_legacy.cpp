/**
 * @file bench_engine_vs_legacy.cpp
 * @brief Benchmark: new openswmm_engine vs legacy openswmm_legacy_engine.
 *
 * @details Measures total wall-time to run a complete simulation using each
 *          engine. Results are reported in milliseconds.
 *
 * @section bench_models Benchmark Models
 *
 * | Model          | Nodes | Links | Duration | Output interval |
 * |----------------|-------|-------|----------|-----------------|
 * | Example1.inp   | 10    | 10    | 6 h      | 5 min           |
 * | LargeCity.inp  | 1000  | 1000  | 24 h     | 5 min           |
 *
 * @note Run with: ./bench_engine_vs_legacy --benchmark_repetitions=5
 *                                          --benchmark_format=json
 *                                          --benchmark_out=bench_results.json
 *
 * @see docs/MASTER_IMPLEMENTATION_PLAN.md Phase 9
 */

#include <benchmark/benchmark.h>
#include <string>

// TODO Phase 7 (when engines are implemented):
// #include <openswmm/legacy/engine/openswmm_solver.h>
// #include <openswmm/engine/openswmm_engine.h>

static const char* EXAMPLE1_INP = "data/Example1.inp";
static const char* LARGE_INP    = "data/LargeCity.inp";

// ============================================================================
// Legacy engine benchmarks
// ============================================================================

/**
 * @brief Benchmark the legacy swmm_run() function on Example1.
 *
 * @note Legacy reference: src/solver/swmm5.c::swmm_run()
 */
static void BM_LegacyEngine_Example1(benchmark::State& state) {
    for (auto _ : state) {
        // swmm_run(EXAMPLE1_INP, "/tmp/bench_legacy.rpt", "/tmp/bench_legacy.out");
        benchmark::DoNotOptimize(0);
    }
    state.SetLabel("legacy engine, Example1.inp");
}
BENCHMARK(BM_LegacyEngine_Example1)
    ->Unit(benchmark::kMillisecond)
    ->Repetitions(3)
    ->ReportAggregatesOnly(true);

/**
 * @brief Benchmark legacy engine on LargeCity (stress test).
 */
static void BM_LegacyEngine_LargeCity(benchmark::State& state) {
    for (auto _ : state) {
        // swmm_run(LARGE_INP, "/tmp/bench_legacy_large.rpt", "/tmp/bench_legacy_large.out");
        benchmark::DoNotOptimize(0);
    }
    state.SetLabel("legacy engine, LargeCity.inp");
}
BENCHMARK(BM_LegacyEngine_LargeCity)
    ->Unit(benchmark::kMillisecond)
    ->Repetitions(3)
    ->ReportAggregatesOnly(true);

// ============================================================================
// New engine benchmarks
// ============================================================================

/**
 * @brief Benchmark the new engine on Example1.
 */
static void BM_NewEngine_Example1(benchmark::State& state) {
    for (auto _ : state) {
        // auto e = swmm_engine_create();
        // swmm_engine_open(e, EXAMPLE1_INP, "/tmp/bench_new.rpt", "/tmp/bench_new.out");
        // swmm_engine_initialize(e);
        // swmm_engine_start(e, 1);
        // double t = 0.0;
        // while (swmm_engine_step(e, &t) == 0 && t > 0.0) {}
        // swmm_engine_end(e);
        // swmm_engine_close(e);
        // swmm_engine_destroy(e);
        benchmark::DoNotOptimize(0);
    }
    state.SetLabel("new engine, Example1.inp");
}
BENCHMARK(BM_NewEngine_Example1)
    ->Unit(benchmark::kMillisecond)
    ->Repetitions(3)
    ->ReportAggregatesOnly(true);

/**
 * @brief Benchmark the new engine on LargeCity (stress test).
 */
static void BM_NewEngine_LargeCity(benchmark::State& state) {
    for (auto _ : state) {
        benchmark::DoNotOptimize(0);
    }
    state.SetLabel("new engine, LargeCity.inp");
}
BENCHMARK(BM_NewEngine_LargeCity)
    ->Unit(benchmark::kMillisecond)
    ->Repetitions(3)
    ->ReportAggregatesOnly(true);

BENCHMARK_MAIN();
