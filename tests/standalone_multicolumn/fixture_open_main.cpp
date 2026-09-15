// SPDX-License-Identifier: Apache-2.0
//
// Acceptance check for the Rich_BC_CSO_.../*.dat placeholder fixtures.
//
// refactored_small.inp and legacy_small.inp both carry two
// [TIMESERIES] … FILE rows whose data was never distributable. A missing FILE
// series is now ERROR 361 (matching legacy) instead of a silent empty series,
// so these two models only open if the placeholders exist. This harness pins
// BOTH halves of the requirement:
//   1. a strict open succeeds (the placeholders are found and parseable), and
//   2. rainfall stays ZERO — the placeholders must not perturb the numerics of
//      the ~18 python engine tests that use these fixtures.
// Run with CWD = tests/unit/engine/data/.

#include <cstdio>
#include <string>
#include <vector>

#include "openswmm/engine/openswmm_engine.h"
#include "openswmm/engine/openswmm_gages.h"
#include "openswmm/engine/openswmm_tables.h"

static int g_failures = 0;
static int g_checks   = 0;

#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        ++g_checks;                                                          \
        if (cond) { std::printf("PASS  %s\n", msg); }                        \
        else      { std::printf("FAIL  %s\n", msg); ++g_failures; }          \
    } while (0)

static std::string art(const char* name) {
    return std::string("../../../standalone_multicolumn/output/") + name;
}

// Open strictly, assert the open succeeded, then assert every gage's series is
// all zeros and the two placeholder tables loaded the expected row count.
static void checkFixture(const char* inp, const char* tag) {
    SWMM_Engine e = swmm_engine_create();
    const int rc = swmm_engine_open(e, inp,
                                    art((std::string(tag) + ".rpt").c_str()).c_str(),
                                    art((std::string(tag) + ".out").c_str()).c_str(),
                                    nullptr);
    std::printf("\n-- %s (rc=%d) --\n", inp, rc);
    if (rc != SWMM_OK) {
        const char* msg = swmm_get_last_error_msg(e);
        std::printf("      first error: %s\n", msg ? msg : "(none)");
    }
    CHECK(rc == SWMM_OK, (std::string(inp) + ": strict open succeeds").c_str());

    if (rc == SWMM_OK) {
        // Every FILE/TIMESERIES gage must still read zero rainfall.
        bool all_zero = true;
        long total_rows = 0;
        for (int g = 0; g < swmm_gage_count(e); ++g) {
            int n = 0;
            if (swmm_gage_get_rainfall_series_count(e, g, &n) != SWMM_OK || n <= 0)
                continue;
            std::vector<double> t(static_cast<std::size_t>(n));
            std::vector<double> v(static_cast<std::size_t>(n));
            swmm_gage_get_rainfall_series(e, g, t.data(), v.data(), n);
            total_rows += n;
            for (const double x : v) if (x != 0.0) all_zero = false;
        }
        CHECK(all_zero, (std::string(inp) + ": every gage series is all zeros"
                         " (placeholders add no rainfall)").c_str());

        // The placeholder tables themselves: 367 daily rows each.
        for (const char* id : {"NOAA_RIC_2004_2022", "USGS_James_River_2002_2022"}) {
            const int t = swmm_table_index(e, id);
            int n = 0;
            if (t >= 0) swmm_table_get_point_count(e, t, &n);
            bool zeros = t >= 0 && n > 0;
            for (int k = 0; zeros && k < n; ++k) {
                double x = 0.0, y = 0.0;
                swmm_table_get_point(e, t, k, &x, &y);
                if (y != 0.0) zeros = false;
            }
            std::printf("      %s: %d points\n", id, n);
            CHECK(t >= 0 && n == 367 && zeros,
                  (std::string(id) + ": 367 all-zero daily rows").c_str());
        }
        (void)total_rows;
    }
    swmm_engine_close(e);
    swmm_engine_destroy(e);
}

int main() {
    checkFixture("refactored_small.inp", "_sa_refsmall");
    checkFixture("legacy_small.inp",     "_sa_legsmall");
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
