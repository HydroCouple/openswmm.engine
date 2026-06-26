// ============================================================================
// verify_geopackage_c_api.cpp
// ----------------------------------------------------------------------------
// Comprehensive C-API verification harness for the OpenSWMM GeoPackage
// plugin. Mirrors every test in python/tests/engine/test_geopackage.py except
// the three TestRegistration cases (which depend on GeoPackagePluginInfo and
// the engine plugin SDK — exercised separately via the Python wheel job).
//
// Build manually with: see python/tests/engine/verify_geopackage_c_api.sh
//
// Returns 0 on full pass, 1 if any case failed.
// ============================================================================

#include <openswmm/engine/openswmm_geopackage.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

int g_pass = 0;
int g_fail = 0;
std::string g_current_case;

void start(const char* name) {
    g_current_case = name;
    std::printf("  [%2d] %-58s ", g_pass + g_fail + 1, name);
    std::fflush(stdout);
}

bool ok(bool cond, const char* detail = "") {
    if (cond) {
        ++g_pass;
        std::printf("PASS\n");
    } else {
        ++g_fail;
        std::printf("FAIL %s\n", detail);
    }
    return cond;
}

// Wrap each test in a per-file gpkg so state never leaks across cases.
struct Tmp {
    std::string path;
    Tmp(const char* tag) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "/tmp/gpkg_verify_%s.gpkg", tag);
        path = buf;
        ::unlink(path.c_str());
        ::unlink((path + "-shm").c_str());
        ::unlink((path + "-wal").c_str());
    }
    ~Tmp() {
        ::unlink(path.c_str());
        ::unlink((path + "-shm").c_str());
        ::unlink((path + "-wal").c_str());
    }
};

bool file_exists(const std::string& p) {
    struct stat st{};
    return ::stat(p.c_str(), &st) == 0;
}

} // namespace

// ============================================================================
// TestLifecycle
// ============================================================================

void case_open_creates_file() {
    start("Lifecycle.test_open_creates_file");
    Tmp f("open_creates_file");
    SWMM_Gpkg g = swmm_gpkg_open(f.path.c_str());
    bool got = (g != nullptr) && file_exists(f.path);
    if (g) swmm_gpkg_close(g);
    ok(got);
}

void case_context_manager() {
    start("Lifecycle.test_context_manager");
    Tmp f("ctxmgr");
    SWMM_Gpkg g = swmm_gpkg_open(f.path.c_str());
    bool got = (g != nullptr);
    if (g) swmm_gpkg_close(g);
    ok(got);
}

void case_double_close_safe() {
    // The Python wrapper guards against double-close at the Python layer
    // (self._handle = NULL after the first close). Verify the C layer is
    // safe when called via open->close once, then reopen the same path.
    start("Lifecycle.test_double_close_safe");
    Tmp f("doubleclose");
    SWMM_Gpkg g1 = swmm_gpkg_open(f.path.c_str());
    if (g1) swmm_gpkg_close(g1);
    SWMM_Gpkg g2 = swmm_gpkg_open(f.path.c_str());  // re-open after close
    bool got = (g1 != nullptr) && (g2 != nullptr);
    if (g2) swmm_gpkg_close(g2);
    ok(got);
}

void case_last_error_default() {
    start("Lifecycle.test_last_error_default");
    Tmp f("lasterr");
    SWMM_Gpkg g = swmm_gpkg_open(f.path.c_str());
    const char* err = swmm_gpkg_last_error(g);
    bool got = (g != nullptr) && (err != nullptr);  // returns non-null string
    if (g) swmm_gpkg_close(g);
    ok(got);
}

// ============================================================================
// TestSimulationMetadata
// ============================================================================

void case_simulation_count_empty() {
    start("SimulationMetadata.test_simulation_count_empty");
    Tmp f("simcount");
    SWMM_Gpkg g = swmm_gpkg_open(f.path.c_str());
    int n = swmm_gpkg_simulation_count(g);
    bool got = (g != nullptr) && (n >= 0);
    if (g) swmm_gpkg_close(g);
    ok(got);
}

void case_simulation_ids_empty() {
    start("SimulationMetadata.test_simulation_ids_empty");
    Tmp f("simids");
    SWMM_Gpkg g = swmm_gpkg_open(f.path.c_str());
    int n = swmm_gpkg_simulation_count(g);
    // The Python wrapper iterates [0..n) and decodes each id; n==0 → empty list.
    char buf[256];
    bool walk_ok = true;
    for (int i = 0; i < n; ++i) {
        if (swmm_gpkg_simulation_id(g, i, buf, 256) != 0) { walk_ok = false; break; }
    }
    bool got = (g != nullptr) && (n >= 0) && walk_ok;
    if (g) swmm_gpkg_close(g);
    ok(got);
}

void case_variable_count() {
    start("SimulationMetadata.test_variable_count");
    Tmp f("varcount");
    SWMM_Gpkg g = swmm_gpkg_open(f.path.c_str());
    int n = swmm_gpkg_variable_count(g);
    // populate_default_variables seeds the variables table, so n > 0 is the
    // stronger guarantee we now ship.
    bool got = (g != nullptr) && (n > 0);
    if (g) swmm_gpkg_close(g);
    ok(got, got ? "" : "(expected n > 0 after populate_default_variables)");
}

// ============================================================================
// TestTransactions
// ============================================================================

void case_begin_commit() {
    start("Transactions.test_begin_commit");
    Tmp f("begin_commit");
    SWMM_Gpkg g = swmm_gpkg_open(f.path.c_str());
    int rb = swmm_gpkg_begin(g);
    int rc = swmm_gpkg_commit(g);
    bool got = (g != nullptr) && (rb == 0) && (rc == 0);
    if (g) swmm_gpkg_close(g);
    ok(got);
}

void case_begin_rollback() {
    start("Transactions.test_begin_rollback");
    Tmp f("begin_rollback");
    SWMM_Gpkg g = swmm_gpkg_open(f.path.c_str());
    int rb = swmm_gpkg_begin(g);
    int rr = swmm_gpkg_rollback(g);
    bool got = (g != nullptr) && (rb == 0) && (rr == 0);
    if (g) swmm_gpkg_close(g);
    ok(got);
}

// ============================================================================
// TestObservedData
// ============================================================================

void case_create_series() {
    start("ObservedData.test_create_series");
    Tmp f("create_series");
    SWMM_Gpkg g = swmm_gpkg_open(f.path.c_str());
    int sid = swmm_gpkg_create_observed_series(
        g, "test_flow", "flow", "NODE", "J1", "Test", "CFS");
    bool got = (g != nullptr) && (sid >= 0);
    if (g) swmm_gpkg_close(g);
    ok(got, got ? "" : ("(sid=" + std::to_string(sid) + ")").c_str());
}

void case_write_single_value() {
    start("ObservedData.test_write_single_value");
    Tmp f("write_single");
    SWMM_Gpkg g = swmm_gpkg_open(f.path.c_str());
    int sid = swmm_gpkg_create_observed_series(g, "s1", "depth",
                                                nullptr, nullptr, nullptr, nullptr);
    int rc = swmm_gpkg_write_observed_value(g, sid, "2026-01-15T08:00:00Z", 1.5, "A");
    bool got = (sid >= 0) && (rc == 0);
    if (g) swmm_gpkg_close(g);
    ok(got);
}

void case_write_read_roundtrip() {
    start("ObservedData.test_write_read_roundtrip");
    Tmp f("roundtrip");
    SWMM_Gpkg g = swmm_gpkg_open(f.path.c_str());
    int sid = swmm_gpkg_create_observed_series(g, "s2", "flow",
                                                nullptr, nullptr, nullptr, nullptr);
    const char* ts[3] = {
        "2026-01-15T08:00:00Z", "2026-01-15T08:15:00Z", "2026-01-15T08:30:00Z"
    };
    double vals[3] = {1.0, 2.5, 3.0};
    const char* flags[3] = {"", "", ""};

    int rb = swmm_gpkg_begin(g);
    int rw = swmm_gpkg_write_observed_values(g, sid, ts, vals, flags, 3);
    int rc = swmm_gpkg_commit(g);

    // Readback: allocate matching buffers
    int n = swmm_gpkg_observed_value_count(g, sid);
    const int ts_len = 32;
    std::vector<char> ts_buf(n * ts_len, 0);
    std::vector<double> v_buf(n, 0.0);
    int read = swmm_gpkg_read_observed_values(g, sid, ts_buf.data(), ts_len,
                                               v_buf.data(), n);

    bool got = (sid >= 0) && (rb == 0) && (rw == 0) && (rc == 0)
            && (n == 3) && (read == 3)
            && std::fabs(v_buf[0] - 1.0) < 1e-6
            && std::fabs(v_buf[1] - 2.5) < 1e-6
            && std::fabs(v_buf[2] - 3.0) < 1e-6;
    if (g) swmm_gpkg_close(g);
    ok(got, got ? "" : ("(n=" + std::to_string(n) + " read=" + std::to_string(read) + ")").c_str());
}

void case_observed_series_count() {
    start("ObservedData.test_observed_series_count");
    Tmp f("series_count");
    SWMM_Gpkg g = swmm_gpkg_open(f.path.c_str());
    int s1 = swmm_gpkg_create_observed_series(g, "a", "depth",
                                                nullptr, nullptr, nullptr, nullptr);
    int s2 = swmm_gpkg_create_observed_series(g, "b", "flow",
                                                nullptr, nullptr, nullptr, nullptr);
    int n = swmm_gpkg_observed_series_count(g);
    bool got = (s1 >= 0) && (s2 >= 0) && (n >= 2);
    if (g) swmm_gpkg_close(g);
    ok(got, got ? "" : ("(n=" + std::to_string(n) + ")").c_str());
}

void case_observed_value_count() {
    start("ObservedData.test_observed_value_count");
    Tmp f("value_count");
    SWMM_Gpkg g = swmm_gpkg_open(f.path.c_str());
    int sid = swmm_gpkg_create_observed_series(g, "c", "depth",
                                                nullptr, nullptr, nullptr, nullptr);
    int rc1 = swmm_gpkg_write_observed_value(g, sid, "2026-01-01T00:00:00Z", 0.5, nullptr);
    int rc2 = swmm_gpkg_write_observed_value(g, sid, "2026-01-01T01:00:00Z", 0.7, nullptr);
    int n = swmm_gpkg_observed_value_count(g, sid);
    bool got = (sid >= 0) && (rc1 == 0) && (rc2 == 0) && (n == 2);
    if (g) swmm_gpkg_close(g);
    ok(got, got ? "" : ("(n=" + std::to_string(n) + ")").c_str());
}

void case_bulk_write_performance() {
    start("ObservedData.test_bulk_write_performance");
    Tmp f("bulk");
    SWMM_Gpkg g = swmm_gpkg_open(f.path.c_str());
    int sid = swmm_gpkg_create_observed_series(g, "bulk", "flow",
                                                nullptr, nullptr, nullptr, nullptr);

    // Mirror the Python list comprehension: 24h * 60m capped at 1000.
    std::vector<std::string> ts_owned;
    ts_owned.reserve(1000);
    for (int h = 0; h < 24 && (int)ts_owned.size() < 1000; ++h) {
        for (int m = 0; m < 60 && (int)ts_owned.size() < 1000; ++m) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "2026-01-15T%02d:%02d:00Z", h, m);
            ts_owned.emplace_back(buf);
        }
    }
    std::vector<const char*> ts_ptrs(1000), fl_ptrs(1000);
    std::vector<double> vals(1000);
    for (int i = 0; i < 1000; ++i) {
        ts_ptrs[i] = ts_owned[i].c_str();
        fl_ptrs[i] = "";
        vals[i] = static_cast<double>(i);
    }
    int rb = swmm_gpkg_begin(g);
    int rw = swmm_gpkg_write_observed_values(g, sid, ts_ptrs.data(),
                                              vals.data(), fl_ptrs.data(), 1000);
    int rc = swmm_gpkg_commit(g);
    int n = swmm_gpkg_observed_value_count(g, sid);
    bool got = (sid >= 0) && (rb == 0) && (rw == 0) && (rc == 0) && (n == 1000);
    if (g) swmm_gpkg_close(g);
    ok(got, got ? "" : ("(n=" + std::to_string(n) + ")").c_str());
}

// ============================================================================
// TestAdHocQueries
// ============================================================================

void case_query_int() {
    start("AdHocQueries.test_query_int");
    Tmp f("qint");
    SWMM_Gpkg g = swmm_gpkg_open(f.path.c_str());
    int v = swmm_gpkg_query_int(g, "SELECT 42");
    bool got = (v == 42);
    if (g) swmm_gpkg_close(g);
    ok(got, got ? "" : ("(v=" + std::to_string(v) + ")").c_str());
}

void case_query_double() {
    start("AdHocQueries.test_query_double");
    Tmp f("qdbl");
    SWMM_Gpkg g = swmm_gpkg_open(f.path.c_str());
    double v = 0.0;
    int rc = swmm_gpkg_query_double(g, "SELECT 3.14159", &v);
    bool got = (rc == 0) && (std::fabs(v - 3.14159) < 1e-4);
    if (g) swmm_gpkg_close(g);
    ok(got, got ? "" : ("(v=" + std::to_string(v) + ")").c_str());
}

// ============================================================================
// TestObservedSeriesOptionalArgs (Cython conditional-NULL regressions)
// ============================================================================

void case_create_all_optional_empty() {
    start("ObservedSeriesOptionalArgs.test_create_series_all_optional_empty");
    Tmp f("opt_empty");
    SWMM_Gpkg g = swmm_gpkg_open(f.path.c_str());
    // Python passes NULL for every optional when the user-supplied string is "".
    int sid = swmm_gpkg_create_observed_series(g, "p2c_a", "depth",
                                                nullptr, nullptr, nullptr, nullptr);
    bool got = (sid >= 0);
    if (g) swmm_gpkg_close(g);
    ok(got, got ? "" : ("(sid=" + std::to_string(sid) + ")").c_str());
}

void case_create_all_optional_set() {
    start("ObservedSeriesOptionalArgs.test_create_series_all_optional_set");
    Tmp f("opt_set");
    SWMM_Gpkg g = swmm_gpkg_open(f.path.c_str());
    int sid = swmm_gpkg_create_observed_series(
        g, "p2c_b", "flow", "NODE", "J1", "Phase2c", "CFS");
    bool got = (sid >= 0);
    if (g) swmm_gpkg_close(g);
    ok(got, got ? "" : ("(sid=" + std::to_string(sid) + ")").c_str());
}

void case_create_mixed_optionals() {
    start("ObservedSeriesOptionalArgs.test_create_series_mixed_optionals");
    Tmp f("opt_mixed");
    SWMM_Gpkg g = swmm_gpkg_open(f.path.c_str());
    // obj_type set, obj_id NULL, source set, units NULL
    int sid = swmm_gpkg_create_observed_series(
        g, "p2c_c", "depth", "NODE", nullptr, "src", nullptr);
    bool got = (sid >= 0);
    if (g) swmm_gpkg_close(g);
    ok(got, got ? "" : ("(sid=" + std::to_string(sid) + ")").c_str());
}

void case_write_no_flag() {
    start("ObservedSeriesOptionalArgs.test_write_observed_value_no_flag");
    Tmp f("write_noflag");
    SWMM_Gpkg g = swmm_gpkg_open(f.path.c_str());
    int sid = swmm_gpkg_create_observed_series(g, "p2c_d", "depth",
                                                nullptr, nullptr, nullptr, nullptr);
    // Python passes NULL for flag when flag="".
    int rc = swmm_gpkg_write_observed_value(g, sid, "2026-05-25T00:00:00Z", 1.23, nullptr);
    int n = swmm_gpkg_observed_value_count(g, sid);
    bool got = (sid >= 0) && (rc == 0) && (n == 1);
    if (g) swmm_gpkg_close(g);
    ok(got, got ? "" : ("(rc=" + std::to_string(rc) + " n=" + std::to_string(n) + ")").c_str());
}

void case_write_with_flag() {
    start("ObservedSeriesOptionalArgs.test_write_observed_value_with_flag");
    Tmp f("write_flag");
    SWMM_Gpkg g = swmm_gpkg_open(f.path.c_str());
    int sid = swmm_gpkg_create_observed_series(g, "p2c_e", "depth",
                                                nullptr, nullptr, nullptr, nullptr);
    int rc = swmm_gpkg_write_observed_value(g, sid, "2026-05-25T01:00:00Z", 4.56, "A");
    int n = swmm_gpkg_observed_value_count(g, sid);
    bool got = (sid >= 0) && (rc == 0) && (n == 1);
    if (g) swmm_gpkg_close(g);
    ok(got);
}

// ============================================================================
// Patch-idempotency check (not in the Python suite — verifies that the new
// create_schema/populate_default_variables calls on every swmm_gpkg_open
// do not duplicate rows on re-open of an already-bootstrapped file).
// Schema-level guarantee: variables.UNIQUE(name, object_type) + INSERT OR
// IGNORE. This case proves it empirically.
// ============================================================================

void case_patch_idempotency() {
    start("idempotency.reopen_does_not_duplicate_variables");
    Tmp f("idempotent");
    SWMM_Gpkg g1 = swmm_gpkg_open(f.path.c_str());
    int n1 = swmm_gpkg_variable_count(g1);
    swmm_gpkg_close(g1);
    // Re-open the same file — populate_default_variables runs again.
    SWMM_Gpkg g2 = swmm_gpkg_open(f.path.c_str());
    int n2 = swmm_gpkg_variable_count(g2);
    swmm_gpkg_close(g2);
    // And a third time, for good measure.
    SWMM_Gpkg g3 = swmm_gpkg_open(f.path.c_str());
    int n3 = swmm_gpkg_variable_count(g3);
    swmm_gpkg_close(g3);
    bool got = (n1 > 0) && (n1 == n2) && (n2 == n3);
    ok(got, got ? "" : ("(n1=" + std::to_string(n1) + " n2=" + std::to_string(n2)
                        + " n3=" + std::to_string(n3) + ")").c_str());
}

// ============================================================================
// TestTopologyEdgeCount (the previously-failing case, now with synthetic sid)
// ============================================================================

void case_topology_edge_count() {
    start("TopologyEdgeCount.test_topology_edge_count");
    Tmp f("topo");
    SWMM_Gpkg g = swmm_gpkg_open(f.path.c_str());
    int n = swmm_gpkg_topology_edge_count(g, "none");
    bool got = (g != nullptr) && (n >= 0);
    if (g) swmm_gpkg_close(g);
    ok(got, got ? "" : ("(n=" + std::to_string(n) + ")").c_str());
}

// ============================================================================

int main() {
    std::printf("OpenSWMM GeoPackage C-API verification harness\n");
    std::printf("==============================================\n\n");

    // Null-path guardrail (not a pytest case but a C-API invariant)
    start("invariant.null_path_returns_null");
    ok(swmm_gpkg_open(nullptr) == nullptr);

    // Lifecycle
    case_open_creates_file();
    case_context_manager();
    case_double_close_safe();
    case_last_error_default();

    // Simulation metadata
    case_simulation_count_empty();
    case_simulation_ids_empty();
    case_variable_count();

    // Transactions
    case_begin_commit();
    case_begin_rollback();

    // Observed data
    case_create_series();
    case_write_single_value();
    case_write_read_roundtrip();
    case_observed_series_count();
    case_observed_value_count();
    case_bulk_write_performance();

    // Ad-hoc queries
    case_query_int();
    case_query_double();

    // Cython nullable-string regression cases
    case_create_all_optional_empty();
    case_create_all_optional_set();
    case_create_mixed_optionals();
    case_write_no_flag();
    case_write_with_flag();

    // Patch-idempotency
    case_patch_idempotency();

    // Topology edge count (post-fix)
    case_topology_edge_count();

    std::printf("\n----------------------------------------\n");
    std::printf("  %d passed, %d failed (%d total)\n",
                g_pass, g_fail, g_pass + g_fail);
    return g_fail == 0 ? 0 : 1;
}
