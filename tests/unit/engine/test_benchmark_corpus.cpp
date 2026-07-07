/**
 * @file test_benchmark_corpus.cpp
 * @brief Presence guard for the manufactured-solution benchmark corpus.
 *
 * The data-driven benchmark tests (in test_routing, test_infiltration,
 * test_ode_solver, test_groundwater, test_exfiltration, test_lid,
 * test_quality_routing, test_xsection) each GTEST_SKIP() when their
 * reference.csv is missing. A skipped gtest is reported as a PASS, so if the
 * benchmark tree were ever moved, renamed, or truncated, that validation
 * coverage would silently vanish with a still-green CI.
 *
 * This test makes the corpus a hard requirement: it ASSERTs every expected
 * reference.csv exists and carries at least its documented number of data
 * rows. It only skips when BENCHMARK_DATA_DIR is the empty-string fallback,
 * i.e. a build that genuinely opted out of wiring the benchmark tree.
 *
 * @see tests/benchmarks/manufactured/
 * @ingroup engine_tests
 */

#include <gtest/gtest.h>

#include <fstream>
#include <string>
#include <vector>

#ifndef BENCHMARK_DATA_DIR
#  define BENCHMARK_DATA_DIR ""
#endif

namespace {

struct Expected {
    const char* relpath;       // path under BENCHMARK_DATA_DIR
    int         min_data_rows; // non-comment rows excluding the header
};

// One entry per manufactured benchmark consumed by a unit test. min_data_rows
// is the row count committed with each reference.csv; the check is `>=`, so
// adding rows is fine but losing/truncating a file fails loudly.
const std::vector<Expected>& corpus() {
    static const std::vector<Expected> kCorpus = {
        {"/manufactured/dw-ritter-drybed-strip/reference.csv",                   204},
        {"/manufactured/dynwave-gvf-backwater-m1/reference.csv",                   6},
        {"/manufactured/exfil-cylindrical-storage-greenampt/reference.csv",       11},
        {"/manufactured/exfil-storage-constant-area/reference.csv",                7},
        {"/manufactured/exfil-storage-geometry-greenampt/reference.csv",          11},
        {"/manufactured/forcemain-friction-reference-curves/reference.csv",       12},
        {"/manufactured/grnampt-saturated-trajectory/reference.csv",               6},
        {"/manufactured/groundwater-linearized-recession/reference.csv",           7},
        {"/manufactured/infil-horton-constant-rainfall/reference.csv",             7},
        {"/manufactured/kinwave-normal-depth-rect-open/reference.csv",             7},
        {"/manufactured/kinwave-step-inflow-rectangular-conduit/reference.csv",    1},
        {"/manufactured/modified-horton-fmax-saturation-recovery/reference.csv",  22},
        {"/manufactured/odesolve-exponential-decay/reference.csv",                 6},
        {"/manufactured/odesolve-logistic-growth/reference.csv",                   8},
        {"/manufactured/odesolve-sir-epidemic/reference.csv",                     13},
        {"/manufactured/quality-cstr-first-order-decay/reference.csv",            11},
        {"/manufactured/xsect-circular-ellipse-reference/reference.csv",          21},
    };
    return kCorpus;
}

// Count non-empty, non-comment lines (lines whose first non-space char is '#'
// are comments). The first such line is the header; the rest are data rows.
int countDataRows(const std::string& path, bool* opened) {
    std::ifstream f(path);
    *opened = f.is_open();
    if (!*opened) return -1;

    int non_comment = 0;
    std::string line;
    while (std::getline(f, line)) {
        size_t i = line.find_first_not_of(" \t\r\n");
        if (i == std::string::npos) continue;   // blank
        if (line[i] == '#') continue;            // comment
        ++non_comment;
    }
    return non_comment - 1;  // subtract the header row
}

}  // namespace

TEST(BenchmarkCorpus, AllReferenceDatasetsPresent) {
    const std::string base = BENCHMARK_DATA_DIR;
    if (base.empty()) {
        GTEST_SKIP() << "BENCHMARK_DATA_DIR not configured (benchmark tree opted out)";
    }

    for (const auto& e : corpus()) {
        const std::string path = base + e.relpath;
        bool opened = false;
        const int rows = countDataRows(path, &opened);
        ASSERT_TRUE(opened) << "Missing benchmark reference: " << path;
        ASSERT_GE(rows, e.min_data_rows)
            << "Benchmark reference truncated: " << path
            << " has " << rows << " data rows, expected >= " << e.min_data_rows;
    }
}
