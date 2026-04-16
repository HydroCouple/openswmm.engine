/**
 * @file bench_operator_snapshot.cpp
 * @brief Benchmark: operator snapshot overhead measurement.
 *
 * @details Measures three scenarios on the same model:
 *   1. **Baseline** — no callback registered (snapshot disabled).
 *   2. **No-op callback** — callback registered but does minimal work.
 *   3. **Copy callback** — callback copies all snapshot arrays to user buffers
 *      (represents a realistic consumer workflow).
 *
 * The overhead of the snapshot layer is (2)-(1) for registration cost, and
 * (3)-(1) for a realistic consumer scenario.
 *
 * @note Run with: ./bench_operator_snapshot --benchmark_repetitions=5
 *                                           --benchmark_format=json
 * @see include/openswmm/engine/openswmm_operator_snapshot.h
 */

#include <benchmark/benchmark.h>
#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_operator_snapshot.h>

#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace {

std::string benchModel() {
    // Resolve relative to source tree — same model as unit tests
    namespace fs = std::filesystem;
    fs::path p = fs::path(__FILE__).parent_path().parent_path()
                 / "unit" / "engine" / "data" / "site_drainage_model.inp";
    return p.string();
}

} // namespace

// ============================================================================
// Helpers
// ============================================================================

namespace {

/// Run a full simulation, returning 0 on success.
int runSim(SWMM_Engine engine, const char* inp) {
    std::string rpt = std::string(inp) + ".bench.rpt";
    std::string out = std::string(inp) + ".bench.out";

    int rc = swmm_engine_open(engine, inp, rpt.c_str(), out.c_str(), nullptr);
    if (rc != SWMM_OK) return rc;

    rc = swmm_engine_initialize(engine);
    if (rc != SWMM_OK) { swmm_engine_close(engine); return rc; }

    rc = swmm_engine_start(engine, 0);
    if (rc != SWMM_OK) { swmm_engine_close(engine); return rc; }

    double elapsed = 0.0;
    while (true) {
        rc = swmm_engine_step(engine, &elapsed);
        if (rc != SWMM_OK || elapsed == 0.0) break;
    }

    swmm_engine_end(engine);
    swmm_engine_close(engine);
    std::remove(rpt.c_str());
    std::remove(out.c_str());
    return SWMM_OK;
}

/// No-op callback: does nothing (measures pure snapshot population overhead).
void noopCallback(SWMM_Engine, const SWMM_OperatorSnapshot*, void*) {
    // Intentionally empty
}

/// Copy callback: copies all arrays to user buffers (realistic consumer).
struct CopyState {
    std::vector<double> flows;
    std::vector<double> dqdh;
    std::vector<double> heads;
    std::vector<double> depths;
    std::vector<double> sumdqdh;
    int count = 0;
};

void copyCallback(SWMM_Engine, const SWMM_OperatorSnapshot* s, void* ud) {
    auto* st = static_cast<CopyState*>(ud);
    st->count++;

    auto un = static_cast<size_t>(s->n_nodes);
    auto ul = static_cast<size_t>(s->n_links);

    if (st->flows.size() != ul) {
        st->flows.resize(ul);
        st->dqdh.resize(ul);
        st->heads.resize(un);
        st->depths.resize(un);
        st->sumdqdh.resize(un);
    }

    std::memcpy(st->flows.data(),   s->link_flow,  ul * sizeof(double));
    std::memcpy(st->dqdh.data(),    s->dqdh,       ul * sizeof(double));
    std::memcpy(st->heads.data(),   s->node_head,  un * sizeof(double));
    std::memcpy(st->depths.data(),  s->node_depth, un * sizeof(double));
    std::memcpy(st->sumdqdh.data(), s->sumdqdh,    un * sizeof(double));
}

} // namespace

// ============================================================================
// Benchmarks
// ============================================================================

/// Baseline: no callback, pure routing performance.
static void BM_Snapshot_Baseline(benchmark::State& state) {
    for (auto _ : state) {
        SWMM_Engine engine = swmm_engine_create();
        int rc = runSim(engine, benchModel().c_str());
        benchmark::DoNotOptimize(rc);
        swmm_engine_destroy(engine);
    }
    state.SetLabel("no callback (baseline)");
}
BENCHMARK(BM_Snapshot_Baseline)
    ->Unit(benchmark::kMillisecond)
    ->Repetitions(3)
    ->ReportAggregatesOnly(true);

/// No-op callback: measures snapshot population overhead.
static void BM_Snapshot_NoopCallback(benchmark::State& state) {
    for (auto _ : state) {
        SWMM_Engine engine = swmm_engine_create();
        swmm_set_operator_snapshot_callback(engine, noopCallback, nullptr);
        int rc = runSim(engine, benchModel().c_str());
        benchmark::DoNotOptimize(rc);
        swmm_engine_destroy(engine);
    }
    state.SetLabel("no-op callback");
}
BENCHMARK(BM_Snapshot_NoopCallback)
    ->Unit(benchmark::kMillisecond)
    ->Repetitions(3)
    ->ReportAggregatesOnly(true);

/// Copy callback: realistic consumer overhead.
static void BM_Snapshot_CopyCallback(benchmark::State& state) {
    for (auto _ : state) {
        SWMM_Engine engine = swmm_engine_create();
        CopyState cs;
        swmm_set_operator_snapshot_callback(engine, copyCallback, &cs);
        int rc = runSim(engine, benchModel().c_str());
        benchmark::DoNotOptimize(rc);
        benchmark::DoNotOptimize(cs.count);
        swmm_engine_destroy(engine);
    }
    state.SetLabel("copy callback (realistic)");
}
BENCHMARK(BM_Snapshot_CopyCallback)
    ->Unit(benchmark::kMillisecond)
    ->Repetitions(3)
    ->ReportAggregatesOnly(true);
