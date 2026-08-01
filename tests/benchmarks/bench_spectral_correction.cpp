/**
 * @file bench_spectral_correction.cpp
 * @brief Benchmark: AA-only vs AA + spectral coarse correction.
 *
 * @details For each model, runs two configurations:
 *   - baseline : Anderson acceleration only (SPECTRAL_ACCEL NO)
 *   - spectral : Anderson acceleration + spectral coarse correction (SPECTRAL_ACCEL YES)
 *
 *   Reports per run:
 *     - wall-clock time (ms)
 *     - total Picard sweeps
 *     - spectral corrections attempted / accepted / rejected
 *     - max relative node-depth difference vs baseline (spectral run only)
 *     - max relative link-flow  difference vs baseline (spectral run only)
 *
 * @section bench_models Models
 *
 * | Tag                | File                                         | Nodes | Conduits |
 * |--------------------|----------------------------------------------|-------|----------|
 * | Example1           | tests/benchmarks/data/Example1.inp           |   12  |    11    |
 * | SynCity01          | ~/Projects/swmm6_generated_networks/01/...   | ~14k  | ~14k     |
 * | SynCity04          | ~/Projects/swmm6_generated_networks/04/...   |  ~7k  |  ~7k     |
 *
 * @note Run with:
 *   ./bench_spectral_correction \
 *       --benchmark_repetitions=3 \
 *       --benchmark_format=json \
 *       --benchmark_out=spectral_bench.json
 *
 * @note Large synthetic networks are loaded from
 *   SWMM_GENERATED_NETWORKS_DIR (environment variable) or
 *   ~/Projects/swmm6_generated_networks/ (default fallback).
 *   Benchmarks that cannot find their input file are skipped automatically.
 */

#include <benchmark/benchmark.h>
#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_statistics.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// ============================================================================
// Network paths
// ============================================================================

static const char* EXAMPLE1_INP = "data/Example1.inp";

static std::string generated_dir() {
    const char* env = std::getenv("SWMM_GENERATED_NETWORKS_DIR");
    if (env && *env) return std::string(env);
    const char* home = std::getenv("HOME");
    return std::string(home ? home : "") + "/Projects/swmm6_generated_networks";
}

static std::string synth_inp(const char* slug) {
    return generated_dir() + "/" + slug + "/network.inp";
}

// ============================================================================
// Baseline storage (keyed by base_inp path, populated by baseline run,
// consumed by spectral run to compute correctness metrics).
// ============================================================================

struct BaselineSnapshot {
    std::vector<double> node_max_depth;
    std::vector<double> link_max_flow;
};

static std::mutex              g_baseline_mu;
static std::map<std::string, BaselineSnapshot> g_baselines;

// ============================================================================
// Helpers
// ============================================================================

// Writes a copy of base_inp with "SPECTRAL_ACCEL  YES/NO" injected immediately
// after the [OPTIONS] header line.  Returns the tmp file path.
//
// Hydraulic-stress strategy (required for spectral correction to fire):
//   1. Set ROUTING_STEP = 30 s (6× the original 5 s).  Larger dt → the
//      previous-step solution is a worse initial guess → more Picard sweeps
//      needed before convergence, eventually exceeding MAX_TRIALS.
//   2. Inject a triangular direct-inflow hydrograph into every junction and
//      storage node via [TIMESERIES] + [INFLOWS].  The hydrograph ramps from
//      0 to peak at T=20 min and back to 0 at T=40 min.  Combined with the
//      larger timestep, the rising and falling limbs create rapid head changes
//      that Picard must resolve under tight HEAD_TOLERANCE, forcing 5+ sweeps.
//
// Flow scales are chosen to achieve near-full-pipe conditions:
//   US (CFS): 2 CFS/node  SI (LPS): 30 LPS/node.
// Steady DWF is kept as background flow (at its original level).
// Flow per node sized to produce realistic near-capacity conditions (not overload).
// Example1: 10 CFS/node × 11 nodes = 110 CFS total; circular bottlenecks (C3: 2.25ft,
// C7: 3.5ft, C11: 4.75ft) reach full flow, forcing some Picard stress without 100x overload.
// SynCity (LPS): 10 LPS/node × ~7k nodes ≈ 70 m³/s — in the range of a 2-5 year design
// storm for a 70 km² catchment; surcharges trunk pipes without unphysical overloading.
static const double INFLOW_PEAK_US = 10.0;   // CFS per node
static const double INFLOW_PEAK_SI = 10.0;   // LPS per node (LPS / MLD / CMS networks)
static const double T_PEAK_HR      = 0.333;  // peak at 20 min (0.333 h)
static const double T_END_HR       = 0.667;  // back to zero at 40 min
static const double DWF_FLOW_PER_NODE = 0.5; // kept for background loading (SI)

static std::string make_inp_with_spectral(const std::string& base_inp, bool on) {
    std::string tmp = std::string("/tmp/bench_spectral_")
                    + (on ? "on" : "off") + "_"
                    + fs::path(base_inp).stem().string() + ".inp";

    std::ifstream  in(base_inp);
    std::ofstream out(tmp);
    if (!in || !out) return {};

    // Capture START_DATE and FLOW_UNITS.
    std::string start_date, flow_units = "CFS";
    {
        std::ifstream scan(base_inp);
        for (std::string l; std::getline(scan, l); ) {
            if (l.find("START_DATE") != std::string::npos) {
                auto pos = l.find_last_of(" \t");
                if (pos != std::string::npos) start_date = l.substr(pos + 1);
            }
            if (l.find("FLOW_UNITS") != std::string::npos) {
                std::istringstream ss(l);
                std::string key, val;
                if (ss >> key >> val) flow_units = val;
            }
        }
    }
    // SI flow unit systems: LPS, LPM, MLD, CMS
    bool is_si = (flow_units == "LPS" || flow_units == "LPM" ||
                  flow_units == "MLD" || flow_units == "CMS");
    double inflow_peak = is_si ? INFLOW_PEAK_SI : INFLOW_PEAK_US;

    // Collect junction and storage node names.
    std::vector<std::string> junction_nodes;
    {
        std::ifstream scan(base_inp);
        bool in_nodes = false;
        for (std::string l; std::getline(scan, l); ) {
            if (l.find("[JUNCTIONS]") != std::string::npos ||
                l.find("[STORAGE]")   != std::string::npos) {
                in_nodes = true; continue;
            }
            if (!l.empty() && l[0] == '[') { in_nodes = false; continue; }
            if (!in_nodes || l.empty() || l[0] == ';') continue;
            std::string name;
            if (std::istringstream(l) >> name) junction_nodes.push_back(name);
        }
    }

    bool injected = false;
    for (std::string line; std::getline(in, line); ) {
        // Clamp to 1-hour simulation.
        if (line.find("END_DATE") != std::string::npos && !start_date.empty())
            line = "END_DATE             " + start_date;
        if (line.find("END_TIME") != std::string::npos)
            line = "END_TIME             01:00:00";
        // 30-second fixed routing step: larger dt stresses Picard convergence.
        if (line.find("ROUTING_STEP") != std::string::npos)
            line = "ROUTING_STEP         0:00:30";
        if (line.find("VARIABLE_STEP") != std::string::npos)
            line = "VARIABLE_STEP        0";
        // Tighten HEAD_TOLERANCE so Picard needs more sweeps to declare convergence.
        // Default 0.005 ft rarely requires more than 2 sweeps; 0.0005 forces 3+.
        if (line.find("HEAD_TOLERANCE") != std::string::npos)
            line = "HEAD_TOLERANCE       0.0005";
        out << line << '\n';
        if (!injected && line.find("[OPTIONS]") != std::string::npos) {
            out << "SPECTRAL_ACCEL       " << (on ? "YES" : "NO") << '\n';
            injected = true;
        }
    }

    if (!junction_nodes.empty()) {
        // Triangular direct-inflow hydrograph: 0 → peak at 20 min → 0 at 40 min.
        // Time series values are normalized [0, 1]; Sfactor scales to actual flow.
        out << "\n[TIMESERIES]\n";
        out << ";;Name             Hours    Value\n";
        out << "swmm6_bench_inflow 0.000    0.0\n";
        out << "swmm6_bench_inflow " << T_PEAK_HR << " 1.0\n";
        out << "swmm6_bench_inflow " << T_END_HR  << " 0.0\n";
        out << "swmm6_bench_inflow 1.000    0.0\n";

        // Direct inflow to every junction/storage: FLOW type, Sfactor = peak.
        out << "\n[INFLOWS]\n";
        out << ";;Node           Constituent  Series              Type  Mfactor  Sfactor  Baseline\n";
        for (const auto& node : junction_nodes)
            out << node << "  FLOW  swmm6_bench_inflow  FLOW  1.0  "
                << inflow_peak << "  0.0\n";

        // Steady DWF background (SI networks only — US networks have subcatchments).
        if (is_si) {
            out << "\n[DWF]\n";
            out << ";;Node           Constituent      Baseline   Patterns\n";
            for (const auto& node : junction_nodes)
                out << node << "  FLOW  " << DWF_FLOW_PER_NODE << "\n";
        }
    }

    return tmp;
}

struct SpectralStats {
    int64_t picard_sweeps  = 0;
    int     cc_attempted   = 0;
    int     cc_accepted    = 0;
    int     cc_rejected    = 0;
    std::vector<double> node_max_depth;
    std::vector<double> link_max_flow;
};

// Run a complete simulation and return solver stats.  Returns false if the
// input file does not exist or the run fails.
static bool run_and_collect(const std::string& inp, SpectralStats& out) {
    if (!fs::exists(inp)) return false;

    SWMM_Engine eng = swmm_engine_create();
    if (!eng) return false;

    auto cleanup = [&]{ swmm_engine_close(eng); swmm_engine_destroy(eng); };

    if (swmm_engine_open(eng, inp.c_str(),
                         "/tmp/bench_spectral.rpt",
                         "/tmp/bench_spectral.out",
                         nullptr) != SWMM_OK) { cleanup(); return false; }
    if (swmm_engine_initialize(eng) != SWMM_OK) { cleanup(); return false; }
    if (swmm_engine_start(eng, 0 /*no output file*/) != SWMM_OK) { cleanup(); return false; }

    double elapsed = 1.0;
    while (elapsed > 0.0) {
        if (swmm_engine_step(eng, &elapsed) != SWMM_OK) break;
    }
    swmm_engine_end(eng);

    swmm_stat_routing_picard_sweeps(eng, &out.picard_sweeps);
    swmm_stat_spectral_corrections(eng, &out.cc_attempted, &out.cc_accepted, &out.cc_rejected);

    int n_nodes = 0, n_links = 0;
    swmm_engine_node_count(eng, &n_nodes);
    swmm_engine_link_count(eng, &n_links);

    if (n_nodes > 0) {
        out.node_max_depth.resize(static_cast<std::size_t>(n_nodes));
        swmm_stat_node_max_depth_bulk(eng, out.node_max_depth.data(), n_nodes);
    }
    if (n_links > 0) {
        out.link_max_flow.resize(static_cast<std::size_t>(n_links));
        swmm_stat_link_max_flow_bulk(eng, out.link_max_flow.data(), n_links);
    }

    cleanup();
    return true;
}

// Compute max relative difference between two equal-length vectors.
// Relative to the reference value; nodes/links with ref <= eps are skipped.
static double max_rel_diff(const std::vector<double>& ref,
                           const std::vector<double>& tst,
                           double eps = 1e-6) {
    double worst = 0.0;
    const std::size_t n = std::min(ref.size(), tst.size());
    for (std::size_t i = 0; i < n; ++i) {
        if (std::abs(ref[i]) <= eps) continue;
        worst = std::max(worst, std::abs(tst[i] - ref[i]) / std::abs(ref[i]));
    }
    return worst;
}

// ============================================================================
// Benchmark template
// ============================================================================

static void BM_Spectral(benchmark::State& state,
                        const std::string& base_inp,
                        bool spectral_on)
{
    const std::string inp = make_inp_with_spectral(base_inp, spectral_on);
    if (inp.empty() || !fs::exists(base_inp)) {
        state.SkipWithError("input file not found");
        return;
    }

    SpectralStats stats;

    for (auto _ : state) {
        if (!run_and_collect(inp, stats)) {
            state.SkipWithError("run failed");
            return;
        }
    }

    // Report counters as per-iteration averages so multiple repetitions
    // give sensible values.
    const double iters = static_cast<double>(state.iterations());
    state.counters["picard_sweeps"] = benchmark::Counter(
        static_cast<double>(stats.picard_sweeps) / iters,
        benchmark::Counter::kDefaults);
    state.counters["cc_attempted"] = benchmark::Counter(
        static_cast<double>(stats.cc_attempted) / iters,
        benchmark::Counter::kDefaults);
    state.counters["cc_accepted"] = benchmark::Counter(
        static_cast<double>(stats.cc_accepted) / iters,
        benchmark::Counter::kDefaults);
    state.counters["cc_rejected"] = benchmark::Counter(
        static_cast<double>(stats.cc_rejected) / iters,
        benchmark::Counter::kDefaults);

    if (!spectral_on) {
        // Store last-iteration hydraulic snapshot as the reference.
        std::lock_guard<std::mutex> lk(g_baseline_mu);
        g_baselines[base_inp] = { stats.node_max_depth, stats.link_max_flow };
    } else {
        // Compare against stored baseline and report correctness metrics.
        std::lock_guard<std::mutex> lk(g_baseline_mu);
        auto it = g_baselines.find(base_inp);
        if (it != g_baselines.end()) {
            const double depth_err = max_rel_diff(it->second.node_max_depth,
                                                  stats.node_max_depth);
            const double flow_err  = max_rel_diff(it->second.link_max_flow,
                                                  stats.link_max_flow);
            state.counters["max_rel_depth_err"] = benchmark::Counter(
                depth_err, benchmark::Counter::kDefaults);
            state.counters["max_rel_flow_err"] = benchmark::Counter(
                flow_err,  benchmark::Counter::kDefaults);
        }
    }

    state.SetLabel(spectral_on ? "AA+spectral" : "AA-only");
}

// ============================================================================
// Benchmark registrations
// ============================================================================

// Small model: Example1 (12 nodes, 11 conduits)
BENCHMARK_CAPTURE(BM_Spectral, Example1_baseline, std::string(EXAMPLE1_INP), false)
    ->Unit(benchmark::kMillisecond)->Repetitions(3)->ReportAggregatesOnly(false);
BENCHMARK_CAPTURE(BM_Spectral, Example1_spectral, std::string(EXAMPLE1_INP), true)
    ->Unit(benchmark::kMillisecond)->Repetitions(3)->ReportAggregatesOnly(false);

// Medium model: Edinburgh steep supercritical (~7k conduits)
BENCHMARK_CAPTURE(BM_Spectral, SynCity04_baseline,
                  synth_inp("synthetic_city_04_steep_supercritical"), false)
    ->Unit(benchmark::kMillisecond)->Repetitions(3)->ReportAggregatesOnly(false);
BENCHMARK_CAPTURE(BM_Spectral, SynCity04_spectral,
                  synth_inp("synthetic_city_04_steep_supercritical"), true)
    ->Unit(benchmark::kMillisecond)->Repetitions(3)->ReportAggregatesOnly(false);

// Large model: Essex/London basic large (~14k conduits)
BENCHMARK_CAPTURE(BM_Spectral, SynCity01_baseline,
                  synth_inp("synthetic_city_01_basic_large"), false)
    ->Unit(benchmark::kMillisecond)->Repetitions(1)->ReportAggregatesOnly(false);
BENCHMARK_CAPTURE(BM_Spectral, SynCity01_spectral,
                  synth_inp("synthetic_city_01_basic_large"), true)
    ->Unit(benchmark::kMillisecond)->Repetitions(1)->ReportAggregatesOnly(false);
