/**
 * @file test_thread_info.cpp
 * @brief Unit tests for the thread-capability query and the THREADS
 *        resolution rules (THREAD_LIMITS_AND_OVERSUBSCRIPTION_PLAN_2026-09-03).
 *
 * @details Covers:
 *   - swmm_get_thread_info(): sane hardware / OpenMP numbers.
 *   - threadinfo::resolveRequested / dwThreads / twoDThreads: 0 = auto,
 *     explicit N honoured, warnings on oversubscription and size gates.
 *   - swmm_get_effective_threads(): mirrors the engine's own resolution.
 *   - A full run with THREADS above the logical CPU count completes, emits
 *     the oversubscription warning, and produces results identical to
 *     THREADS = 1 (the any-thread-count contract).
 *
 * Artefacts are written under tests/output/thread_info (CLAUDE.md §4.1).
 */

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_model.h>
#include <openswmm/engine/openswmm_nodes.h>
#include <openswmm/engine/openswmm_links.h>

#include "core/ThreadInfo.hpp"

namespace fs = std::filesystem;
namespace ti = openswmm::threadinfo;

namespace {

const char* kDataDir = OPENSWMM_THREAD_TEST_DATA_DIR;

std::string outDir() {
    const std::string d = std::string(OPENSWMM_THREAD_TEST_OUT_DIR) + "/thread_info";
    std::error_code ec;
    fs::create_directories(d, ec);
    return d;
}

/// Copy Example1.inp with its THREADS line replaced (or appended).
std::string writeVariant(const std::string& name, int threads) {
    std::ifstream in(std::string(kDataDir) + "/Example1.inp");
    EXPECT_TRUE(in.good()) << "cannot open the reference model";
    const std::string path = outDir() + "/" + name + ".inp";
    std::ofstream os(path);
    std::string line;
    bool wrote = false;
    while (std::getline(in, line)) {
        if (line.rfind("THREADS", 0) == 0) {
            os << "THREADS              " << threads << "\n";
            wrote = true;
        } else if (!wrote && line.rfind("FLOW_ROUTING", 0) == 0) {
            os << line << "\n";
            os << "THREADS              " << threads << "\n";
            wrote = true;
        } else {
            os << line << "\n";
        }
    }
    return path;
}

bool hasWarningContaining(const std::vector<std::string>& w, const char* needle) {
    for (const auto& s : w)
        if (s.find(needle) != std::string::npos) return true;
    return false;
}

std::vector<std::string> engineWarnings(SWMM_Engine e) {
    std::vector<std::string> out;
    const int n = swmm_get_warning_count(e);
    for (int i = 0; i < n; ++i) out.emplace_back(swmm_get_warning_at(e, i));
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// Capability query
// ---------------------------------------------------------------------------

TEST(ThreadInfo, QueryReportsSaneNumbers) {
    SWMM_ThreadInfo info{};
    ASSERT_EQ(swmm_get_thread_info(&info), SWMM_OK);
    EXPECT_GE(info.logical_cpus, 1);
    EXPECT_GE(info.omp_max_threads, 1);
    EXPECT_TRUE(info.omp_available == 0 || info.omp_available == 1);
    EXPECT_GE(info.perf_cores, 0);
    EXPECT_GE(info.kokkos_omp_threads, 0);
    // Without OpenMP the runtime limit is 1 by definition; with it the
    // default limit can only be lowered by the environment / affinity.
    if (!info.omp_available) EXPECT_EQ(info.omp_max_threads, 1);
    EXPECT_EQ(swmm_get_thread_info(nullptr), SWMM_ERR_BADPARAM);
}

// ---------------------------------------------------------------------------
// Resolution rules
// ---------------------------------------------------------------------------

TEST(ThreadInfo, AutoUsesRuntimeLimitWithoutWarnings) {
    std::vector<std::string> w;
    EXPECT_EQ(ti::resolveRequested(0, "test", &w), ti::ompMaxThreads());
    EXPECT_TRUE(w.empty());
}

TEST(ThreadInfo, ExplicitWithinLimitIsHonouredSilently) {
    std::vector<std::string> w;
    EXPECT_EQ(ti::resolveRequested(1, "test", &w), 1);
    EXPECT_TRUE(w.empty()) << w.front();
}

TEST(ThreadInfo, ExplicitAboveLogicalIsHonouredAndWarned) {
    const int logical = ti::logicalCpus();
    ASSERT_GE(logical, 1);
    std::vector<std::string> w;
    const int req = logical + 3;
    EXPECT_EQ(ti::resolveRequested(req, "test", &w), req);   // NOT clamped
    EXPECT_TRUE(hasWarningContaining(w, "logical processors"));
    EXPECT_TRUE(ti::isOversubscribed(req));
    EXPECT_FALSE(ti::isOversubscribed(logical));
}

TEST(ThreadInfo, DwSizeGateReducesExplicitRequestWithWarning) {
    std::vector<std::string> w;
    // 40 conduits / 100 per thread → cap 1.
    EXPECT_EQ(ti::dwThreads(4, 40, &w), 1);
    EXPECT_TRUE(hasWarningContaining(w, "conduits"));
    // 1000 conduits → cap 10; a request of 4 within the machine passes.
    w.clear();
    if (ti::logicalCpus() >= 4) {
        EXPECT_EQ(ti::dwThreads(4, 1000, &w), 4);
        EXPECT_TRUE(w.empty()) << w.front();
    }
}

TEST(ThreadInfo, DwAutoAppliesSizeGateAndStaysWithinRuntime) {
    std::vector<std::string> w;
    const int nt = ti::dwThreads(0, 100000, &w);
    EXPECT_GE(nt, 1);
    EXPECT_LE(nt, ti::ompMaxThreads());
    EXPECT_TRUE(w.empty());
    // Tiny model → serial regardless of hardware.
    EXPECT_EQ(ti::dwThreads(0, 10, &w), 1);
}

TEST(ThreadInfo, TwoDTriangleGate) {
    std::vector<std::string> w;
    EXPECT_EQ(ti::twoDThreads(4, 3, &w), 1);        // 3 < 4*4
    EXPECT_TRUE(hasWarningContaining(w, "triangles"));
    w.clear();
    EXPECT_EQ(ti::twoDThreads(1, 3, &w), 1);        // serial request: no warning
    EXPECT_TRUE(w.empty());
}

// ---------------------------------------------------------------------------
// Effective-thread query on a built model
// ---------------------------------------------------------------------------

TEST(ThreadInfo, EffectiveThreadsMirrorsRulesForSmallModel) {
    SWMM_Engine e = swmm_engine_new();
    ASSERT_NE(e, nullptr);
    ASSERT_EQ(swmm_node_add(e, "J1", SWMM_NODE_JUNCTION), SWMM_OK);
    ASSERT_EQ(swmm_node_add(e, "O1", SWMM_NODE_OUTFALL),  SWMM_OK);
    ASSERT_EQ(swmm_link_add(e, "L1", SWMM_LINK_CONDUIT),  SWMM_OK);
    ASSERT_EQ(swmm_options_set(e, "FLOW_ROUTING", "DYNWAVE"), SWMM_OK);

    int g = -1, dw = -1, td = -1;
    ASSERT_EQ(swmm_get_effective_threads(e, 8, &g, &dw, &td), SWMM_OK);
    EXPECT_EQ(g, 8);        // explicit request honoured for the general team
    EXPECT_EQ(dw, 1);       // one conduit → size gate
    EXPECT_EQ(td, 0);       // no 2D mesh

    ASSERT_EQ(swmm_get_effective_threads(e, 0, &g, &dw, &td), SWMM_OK);
    EXPECT_EQ(g, ti::ompMaxThreads());
    EXPECT_EQ(dw, 1);

    EXPECT_EQ(swmm_get_effective_threads(nullptr, 0, &g, &dw, &td), SWMM_ERR_BADPARAM);
    swmm_engine_destroy(e);
}

// ---------------------------------------------------------------------------
// Full run: oversubscribed THREADS completes, warns, and matches THREADS = 1
// ---------------------------------------------------------------------------

namespace {
struct RunResult {
    int err = 0;
    std::vector<std::string> warnings;
    std::string rptDigest;   // continuity block of the .rpt, for comparison
};

RunResult runVariant(const std::string& name, int threads) {
    RunResult r;
    const std::string inp = writeVariant(name, threads);
    const std::string rpt = outDir() + "/" + name + ".rpt";
    const std::string out = outDir() + "/" + name + ".out";
    SWMM_Engine e = swmm_engine_create();
    if (!e) { r.err = SWMM_ERR_NOMEM; return r; }
    r.err = swmm_engine_open(e, inp.c_str(), rpt.c_str(), out.c_str(), nullptr);
    if (r.err == SWMM_OK) r.err = swmm_engine_initialize(e);
    if (r.err == SWMM_OK) r.err = swmm_engine_start(e, 1);
    if (r.err == SWMM_OK) {
        double elapsed = 0.0;
        do { r.err = swmm_engine_step(e, &elapsed); } while (elapsed > 0.0 && r.err == SWMM_OK);
    }
    swmm_engine_end(e);
    swmm_engine_report(e);
    r.warnings = engineWarnings(e);
    swmm_engine_close(e);
    swmm_engine_destroy(e);

    // Digest: every .rpt line from "Flow Routing Continuity" to the first
    // blank line after it (the volumes are what a thread-count regression
    // would perturb; timing lines are deliberately excluded).
    std::ifstream in(rpt);
    std::string line;
    bool capture = false;
    while (std::getline(in, line)) {
        if (line.find("Flow Routing Continuity") != std::string::npos) capture = true;
        if (capture) {
            if (line.find("Continuity Error") != std::string::npos) { r.rptDigest += line + "\n"; break; }
            r.rptDigest += line + "\n";
        }
    }
    return r;
}
} // namespace

TEST(ThreadInfo, OversubscribedRunCompletesWarnsAndMatchesSerial) {
    const int logical = ti::logicalCpus();
    ASSERT_GE(logical, 1);

    const RunResult serial = runVariant("example1_threads_1", 1);
    ASSERT_EQ(serial.err, SWMM_OK);
    EXPECT_FALSE(hasWarningContaining(serial.warnings, "logical processors"));

    const RunResult over = runVariant("example1_threads_over", logical + 4);
    ASSERT_EQ(over.err, SWMM_OK);
    EXPECT_TRUE(hasWarningContaining(over.warnings, "logical processors"))
        << "expected the oversubscription warning";

    // Bit-identical continuity block: the thread count is timing-only.
    EXPECT_FALSE(serial.rptDigest.empty());
    EXPECT_EQ(over.rptDigest, serial.rptDigest);
}
