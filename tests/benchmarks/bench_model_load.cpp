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
 * @file      bench_model_load.cpp
 * @brief     Model load / initialization benchmark (optimization plan Phase 0.3).
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
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @license   Apache-2.0
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
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

/**
 * @brief Read the process peak resident-set high-water mark.
 *
 * Monotonic process-wide high-water value across benchmark iterations; no per-model reset or
 * allocation occurs.
 * @return Peak bytes, or zero when getrusage fails; Apple reports bytes directly and other
 *     supported systems are interpreted as KiB.
 */
std::size_t peak_rss_bytes() {
    rusage ru{};
    if (getrusage(RUSAGE_SELF, &ru) != 0)
        return 0;
#if defined(__APPLE__)
    return static_cast<std::size_t>(ru.ru_maxrss); // bytes
#else
    return static_cast<std::size_t>(ru.ru_maxrss) * 1024u; // kilobytes
#endif
}

/**
 * @brief Construct a deterministic temporary benchmark artifact path.
 *
 * Names are reused across iterations/processes for the same stem, so concurrent runs can collide.
 * temp_directory_path errors can throw.
 *
 * @param[in] stem Model stem incorporated into bench_load_<stem>; caller must supply a
 *     filename-safe stem.
 * @param[in] ext Non-null null-terminated extension, normally .rpt or .out.
 * @return Path string under the process temporary directory; no file is created here.
 */
std::string scratch(const std::string& stem, const char* ext) {
    return (fs::temp_directory_path() / ("bench_load_" + stem + ext)).string();
}

/**
 * @brief Execute a model-load lifecycle cut without simulation steps.
 *
 * Owns/destroys its handle, calling close after every non-null-handle path. Close status is
 * ignored. Creates/overwrites scratch report/output files and leaves them in the temporary
 * directory; filesystem path exceptions can propagate.
 *
 * @param[in] inp Input-deck path borrowed for the call.
 * @param[in] stem Filename stem for deterministic temporary report/output artifacts.
 * @param[in] cut Open only; OpenInit adds initialize; Full adds start/end/report with saved
 *     results.
 * @return True if all requested open/initialize/start/end/report operations succeed; false on
 *     allocation or a rejected operation.
 */
bool run_cut(const std::string& inp, const std::string& stem, Cut cut) {
    SWMM_Engine eng = swmm_engine_create();
    if (eng == nullptr)
        return false;

    const std::string rpt = scratch(stem, ".rpt");
    const std::string out = scratch(stem, ".out");

    bool ok = swmm_engine_open(eng, inp.c_str(), rpt.c_str(), out.c_str(), nullptr) == SWMM_OK;
    if (ok && cut != Cut::Open) {
        ok = swmm_engine_initialize(eng) == SWMM_OK;
    }
    if (ok && cut == Cut::Full) {
        ok = swmm_engine_start(eng, 1) == SWMM_OK;
        // Deliberately no step() calls — this is a load benchmark.
        if (ok)
            ok = swmm_engine_end(eng) == SWMM_OK;
        if (ok)
            ok = swmm_engine_report(eng) == SWMM_OK;
    }
    swmm_engine_close(eng);
    swmm_engine_destroy(eng);
    return ok;
}

/**
 * @brief Time repeated model-load cuts and expose a process peak-RSS counter.
 *
 * Stops iteration with SkipWithError if a cut fails. Reports peak_rss_MB as process high-water
 * bytes divided by 1024^2, including earlier process work; the label is the stem.
 *
 * @param[in,out] state Borrowed Google Benchmark iteration/counter state; labels/counters or skip
 *     status are written during this call.
 * @param[in] inp Owned input path value for this benchmark invocation.
 * @param[in] stem Owned model stem used for artifact names and result label.
 * @param[in] cut Lifecycle cut applied on every iteration.
 */
void bm_load(benchmark::State& state, std::string inp, std::string stem, Cut cut) {
    for (auto _ : state) {
        if (!run_cut(inp, stem, cut)) {
            state.SkipWithError("engine rejected the model");
            break;
        }
    }
    state.counters["peak_rss_MB"] =
        benchmark::Counter(static_cast<double>(peak_rss_bytes()) / (1024.0 * 1024.0));
    state.SetLabel(stem);
}

/**
 * @brief Choose benchmark repetition count from OPENSWMM_BENCH_REPS.
 *
 * The minimum supplies aggregate median rows; this environment-selected Repetitions value takes
 * priority over the ordinary command-line repetition default.
 * @return Five when unset; otherwise atoi value clamped to at least two (including nonnumeric text
 *     yielding two).
 */
int repetitions_from_env() {
    const char* s = std::getenv("OPENSWMM_BENCH_REPS");
    if (s == nullptr)
        return 5;
    const int n = std::atoi(s);
    return n >= 2 ? n : 2;
}

/**
 * @brief Map a lifecycle cut to a stable benchmark-name suffix.
 *
 * @param[in] c Open, OpenInit or Full.
 * @return Borrowed static literal open, open_init or full; ? for an invalid enum. Caller must not
 *     free it.
 */
const char* cut_suffix(Cut c) {
    switch (c) {
    case Cut::Open:
        return "open";
    case Cut::OpenInit:
        return "open_init";
    case Cut::Full:
        return "full";
    }
    return "?";
}

} // namespace

/**
 * @brief Discover corpus decks and register three load benchmarks per model.
 *
 * Uses OPENSWMM_BENCH_CORPUS or tests/benchmarks/generated, sorts regular .inp paths, registers
 * real-time millisecond cuts with at least 0.5 seconds per sample and environment-selected
 * repetitions. Iterations write deterministic temporary artifacts; filesystem operations can throw
 * where not using error_code.
 *
 * @param[in] argc Initial argument count, passed by address to Google Benchmark initialization for
 *     flag consumption.
 * @param[in] argv Borrowed process argument strings, also passed to Google Benchmark.
 * @return One when the corpus is missing or contains no .inp files; otherwise zero after benchmark
 *     shutdown, even if a benchmark was skipped with an engine error.
 */
int main(int argc, char** argv) {
    benchmark::Initialize(&argc, argv);

    const char* env = std::getenv("OPENSWMM_BENCH_CORPUS");
    const fs::path corpus =
        env != nullptr ? fs::path(env) : fs::path("tests") / "benchmarks" / "generated";

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
        std::fprintf(stderr, "bench_model_load: no .inp files in '%s'\n", corpus.string().c_str());
        return 1;
    }

    const int reps = repetitions_from_env();

    for (const auto& m : models) {
        const std::string stem = m.stem().string();
        const std::string inp = m.string();
        for (const Cut cut : {Cut::Open, Cut::OpenInit, Cut::Full}) {
            const std::string name = "BM_Load/" + stem + "/" + cut_suffix(cut);
            benchmark::RegisterBenchmark(
                name.c_str(),
                [inp, stem, cut](benchmark::State& st) { bm_load(st, inp, stem, cut); })
                ->Unit(benchmark::kMillisecond)
                ->UseRealTime()
                ->MinTime(0.5)
                ->Repetitions(reps)
                ->ReportAggregatesOnly(true);
        }
    }

    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    return 0;
}
