// SPDX-License-Identifier: Apache-2.0
//
// Copyright 2026 Caleb Buahin
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/**
 * @file bench_model_load.cpp
 * @brief Model load / initialization benchmark (optimization plan Phase 0.3).
 *
 * @details The window a user experiences between clicking Run and the first
 *          routing step is open() + initialize() + start(). This benchmark
 *          times three cuts of that window for every model in the synthetic
 *          corpus, so an optimization can be attributed to the phase it
 *          actually changed:
 *
 *          | Cut         | Calls                                          |
 *          |-------------|------------------------------------------------|
 *          | `open`      | open → close                                   |
 *          | `open_init` | open → initialize → close                      |
 *          | `full`      | open → initialize → start → end → report → close |
 *
 *          No stepping happens in any cut — this measures load, not routing.
 *
 *          Models are discovered at run time from the corpus directory rather
 *          than hard-coded, because the corpus is generated on demand and is
 *          deliberately not committed (the large ones are hundreds of MB). Set
 *          `OPENSWMM_BENCH_CORPUS` to point elsewhere; the default is
 *          `tests/benchmarks/generated` relative to the working directory.
 *
 *          Peak RSS is reported as a counter. It is the process high-water
 *          mark from getrusage(), so it is monotonic across iterations within
 *          one process — read it as "this model needed at least this much",
 *          and compare it between runs of the same single model, not between
 *          benchmarks inside one run.
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @license  Apache-2.0
 */

#include <benchmark/benchmark.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include <sys/resource.h>

#include <openswmm/engine/openswmm_engine.h>

namespace {

namespace fs = std::filesystem;

/** @brief Which calls a benchmark iteration makes. */
enum class Cut { Open, OpenInit, Full };

/** @returns process peak resident set size in bytes, 0 if unavailable. */
std::size_t peak_rss_bytes() {
    rusage ru{};
    if (getrusage(RUSAGE_SELF, &ru) != 0) return 0;
#if defined(__APPLE__)
    return static_cast<std::size_t>(ru.ru_maxrss);          // bytes
#else
    return static_cast<std::size_t>(ru.ru_maxrss) * 1024u;  // kilobytes
#endif
}

/** @brief Scratch output paths — the .rpt/.out content is not of interest. */
std::string scratch(const std::string& stem, const char* ext) {
    return (fs::temp_directory_path() / ("bench_load_" + stem + ext)).string();
}

/**
 * @brief One iteration of the requested cut.
 * @returns false if the engine rejected the model, which fails the benchmark
 *          loudly instead of reporting a fast time for work that never ran.
 */
bool run_cut(const std::string& inp, const std::string& stem, Cut cut) {
    SWMM_Engine eng = swmm_engine_create();
    if (eng == nullptr) return false;

    const std::string rpt = scratch(stem, ".rpt");
    const std::string out = scratch(stem, ".out");

    bool ok = swmm_engine_open(eng, inp.c_str(), rpt.c_str(), out.c_str(),
                               nullptr) == SWMM_OK;
    if (ok && cut != Cut::Open) {
        ok = swmm_engine_initialize(eng) == SWMM_OK;
    }
    if (ok && cut == Cut::Full) {
        ok = swmm_engine_start(eng, 1) == SWMM_OK;
        // Deliberately no step() calls — this is a load benchmark.
        if (ok) ok = swmm_engine_end(eng) == SWMM_OK;
        if (ok) ok = swmm_engine_report(eng) == SWMM_OK;
    }
    swmm_engine_close(eng);
    swmm_engine_destroy(eng);
    return ok;
}

void bm_load(benchmark::State& state, std::string inp, std::string stem,
             Cut cut) {
    for (auto _ : state) {
        if (!run_cut(inp, stem, cut)) {
            state.SkipWithError("engine rejected the model");
            break;
        }
    }
    state.counters["peak_rss_MB"] = benchmark::Counter(
        static_cast<double>(peak_rss_bytes()) / (1024.0 * 1024.0));
    state.SetLabel(stem);
}

const char* cut_suffix(Cut c) {
    switch (c) {
        case Cut::Open:     return "open";
        case Cut::OpenInit: return "open_init";
        case Cut::Full:     return "full";
    }
    return "?";
}

}  // namespace

int main(int argc, char** argv) {
    benchmark::Initialize(&argc, argv);

    const char* env = std::getenv("OPENSWMM_BENCH_CORPUS");
    const fs::path corpus = env != nullptr
        ? fs::path(env)
        : fs::path("tests") / "benchmarks" / "generated";

    std::error_code ec;
    if (!fs::is_directory(corpus, ec)) {
        std::fprintf(stderr,
            "bench_model_load: corpus directory '%s' not found.\n"
            "Generate it first:\n"
            "  python3 tests/benchmarks/scripts/gen_load_bench.py\n"
            "or point OPENSWMM_BENCH_CORPUS at an existing one.\n",
            corpus.string().c_str());
        return 1;
    }

    std::vector<fs::path> models;
    for (const auto& e : fs::directory_iterator(corpus, ec)) {
        if (e.is_regular_file(ec) && e.path().extension() == ".inp")
            models.push_back(e.path());
    }
    // directory_iterator order is unspecified; sort so successive runs report
    // in the same order and diffs between result files stay readable.
    std::sort(models.begin(), models.end());

    if (models.empty()) {
        std::fprintf(stderr, "bench_model_load: no .inp files in '%s'\n",
                     corpus.string().c_str());
        return 1;
    }

    for (const auto& m : models) {
        const std::string stem = m.stem().string();
        const std::string inp  = m.string();
        for (const Cut cut : {Cut::Open, Cut::OpenInit, Cut::Full}) {
            const std::string name =
                "BM_Load/" + stem + "/" + cut_suffix(cut);
            benchmark::RegisterBenchmark(
                name.c_str(),
                [inp, stem, cut](benchmark::State& st) {
                    bm_load(st, inp, stem, cut);
                })
                ->Unit(benchmark::kMillisecond)
                ->UseRealTime()
                ->MinTime(0.5)
                ->Repetitions(5)
                ->ReportAggregatesOnly(true);
        }
    }

    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    return 0;
}
