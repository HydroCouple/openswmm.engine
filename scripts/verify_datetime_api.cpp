// Self-contained sanity check for the new datetime C API.
//
// Builds the new `openswmm_datetime_impl.cpp` against the inline
// `DateTime.hpp` and exercises the round-trip behaviour. No engine
// linkage required — meant to be runnable from this script alone.
//
// build (from repo root):
//   g++ -std=c++20 -I include/openswmm/engine \
//       -DOPENSWMM_ENGINE_STATIC \
//       src/engine/core/openswmm_datetime_impl.cpp \
//       scripts/verify_datetime_api.cpp -o /tmp/verify_datetime_api
//
// run:
//   /tmp/verify_datetime_api
//
// Exit code 0 on success, non-zero on any check failure.

#include "openswmm_datetime.h"

#include <cassert>
#include <cmath>
#include <cstdio>

static int failures = 0;

#define EXPECT(cond) do { \
    if (!(cond)) { \
        std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        ++failures; \
    } \
} while (0)

int main() {
    // --- encode / decode date round-trip ---
    double d = 0.0;
    EXPECT(swmm_datetime_encode_date(2024, 6, 15, &d) == 0);
    int y = 0, m = 0, day = 0;
    EXPECT(swmm_datetime_decode_date(d, &y, &m, &day) == 0);
    EXPECT(y == 2024 && m == 6 && day == 15);

    // The OADate epoch — 1899-12-30 — must round-trip to 0.0.
    EXPECT(swmm_datetime_encode_date(1899, 12, 30, &d) == 0);
    EXPECT(d == 0.0);

    // Invalid date returns -1 and writes the legacy sentinel.
    int rc = swmm_datetime_encode_date(2024, 13, 1, &d);
    EXPECT(rc == -1);
    EXPECT(d == static_cast<double>(-SWMM_DATETIME_DATE_DELTA));

    // --- encode / decode time round-trip ---
    double t = 0.0;
    EXPECT(swmm_datetime_encode_time(13, 30, 45, &t) == 0);
    int h = 0, mi = 0, s = 0;
    EXPECT(swmm_datetime_decode_time(t, &h, &mi, &s) == 0);
    EXPECT(h == 13 && mi == 30 && s == 45);

    // Negative inputs are rejected.
    EXPECT(swmm_datetime_encode_time(-1, 0, 0, &t) == -1);
    EXPECT(t == 0.0);

    // --- combined date + time ---
    EXPECT(swmm_datetime_encode_date(2024, 6, 15, &d) == 0);
    EXPECT(swmm_datetime_encode_time(13, 30, 45, &t) == 0);
    double dt = d + t;
    EXPECT(swmm_datetime_decode_date(dt, &y, &m, &day) == 0);
    EXPECT(swmm_datetime_decode_time(dt, &h, &mi, &s) == 0);
    EXPECT(y == 2024 && m == 6 && day == 15);
    EXPECT(h == 13 && mi == 30 && s == 45);

    // --- addSeconds: cross a midnight boundary ---
    double midnight = 0.0;
    EXPECT(swmm_datetime_encode_date(2024, 6, 15, &midnight) == 0);
    double t1 = 0.0;
    EXPECT(swmm_datetime_encode_time(23, 59, 30, &t1) == 0);
    double before = midnight + t1;
    double after = 0.0;
    EXPECT(swmm_datetime_add_seconds(before, 60.0, &after) == 0);  // +60 s
    EXPECT(swmm_datetime_decode_date(after, &y, &m, &day) == 0);
    EXPECT(swmm_datetime_decode_time(after, &h, &mi, &s) == 0);
    EXPECT(y == 2024 && m == 6 && day == 16);
    EXPECT(h == 0 && mi == 0 && s == 30);

    // --- timeDiff: simple delta ---
    long diff = 0;
    EXPECT(swmm_datetime_time_diff(after, before, &diff) == 0);
    EXPECT(diff == 60);

    // --- NULL output rejected ---
    EXPECT(swmm_datetime_encode_date(2024, 6, 15, nullptr) == -1);
    EXPECT(swmm_datetime_encode_time(0, 0, 0, nullptr) == -1);
    EXPECT(swmm_datetime_add_seconds(0.0, 1.0, nullptr) == -1);
    EXPECT(swmm_datetime_time_diff(0.0, 0.0, nullptr) == -1);

    // decode_* accept any combination of NULLs as long as one is set.
    EXPECT(swmm_datetime_decode_date(0.0, nullptr, nullptr, nullptr) == -1);
    EXPECT(swmm_datetime_decode_time(0.0, nullptr, nullptr, nullptr) == -1);

    if (failures == 0) {
        std::printf("OK — all datetime API checks passed.\n");
        return 0;
    }
    std::printf("FAIL — %d check(s) failed.\n", failures);
    return 1;
}
