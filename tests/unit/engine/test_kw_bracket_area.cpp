/**
 * @file test_kw_bracket_area.cpp
 * @brief A kinematic-wave solve brackets the outlet area where legacy does.
 *
 * Legacy solveContinuity (kinwave.c) takes the lower bracket end as
 * xsect_getAmax() / aFull = (Amax[type] * aFull) / aFull. v6 used the ratio
 * Amax[type] itself, which for a 10-inch circle is 1 ulp below legacy's round
 * trip. The bracket end feeds the root finder's start guess and bisection, so
 * as a full, nearly flat pipe receded the two engines took different paths:
 * in benchmarks 487-nodes-copper link flows parted by up to 5 % after 103 hours.
 * This deck is that pipe chain: a 10-inch line laid flat (the minimum
 * elevation drop) that runs full and then recedes. Both engines run it,
 * stepped together.
 */
#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_links.h>
#include <openswmm/legacy/engine/openswmm_solver.h>

namespace fs = std::filesystem;

namespace {

const char* kOutDir = "kw_bracket_area_out"; // *_out/ is the gitignored scratch convention

/// J1..J4 share one invert; 0.3 cfs (above C1's full flow) for 1.5 hours,
/// then a 4-hour recession.
const char* kDeck = R"([OPTIONS]
FLOW_UNITS           CFS
FLOW_ROUTING         KINWAVE
MIN_SLOPE            0
START_DATE           01/01/2020
START_TIME           00:00:00
END_DATE             01/01/2020
END_TIME             08:00:00
REPORT_STEP          00:01:00
ROUTING_STEP         0:01:00

[JUNCTIONS]
J1  100  0  0  0  0
J2  100  0  0  0  0
J3  100  0  0  0  0
J4  100  0  0  0  0

[OUTFALLS]
O1  99  FREE

[CONDUITS]
C1  J1  J2  31.741462   0.01  0  0  0  0
C2  J2  J3  214.343821  0.01  0  0  0  0
C3  J3  J4  115.989799  0.01  0  0  0  0
C4  J4  O1  341.86073   0.01  0  0  0  0

[XSECTIONS]
C1  CIRCULAR  0.833333  0  0  0  1
C2  CIRCULAR  0.833333  0  0  0  1
C3  CIRCULAR  0.833333  0  0  0  1
C4  CIRCULAR  0.833333  0  0  0  1

[INFLOWS]
J1  FLOW  TS_IN  FLOW  1.0  1.0

[TIMESERIES]
TS_IN  0:00  0
TS_IN  0:30  0.3
TS_IN  2:00  0.3
TS_IN  6:00  0
)";

} // namespace

TEST(KinematicWaveBracket, RecedingFullPipeMatchesLegacy) {
    fs::create_directories(kOutDir);
    const std::string inp = (fs::path(kOutDir) / "kw_bracket_area.inp").string();
    std::ofstream(inp) << kDeck;

    SWMM_Engine e = swmm_engine_create();
    ASSERT_EQ(swmm_engine_open(e, inp.c_str(), (inp + ".v6.rpt").c_str(), (inp + ".v6.out").c_str(),
                               nullptr),
              0)
        << swmm_get_last_error_msg(e);
    ASSERT_EQ(swmm_engine_initialize(e), 0) << swmm_get_last_error_msg(e);
    ASSERT_EQ(swmm_engine_start(e, 0), 0) << swmm_get_last_error_msg(e);
    ASSERT_EQ(swmm_open(inp.c_str(), (inp + ".legacy.rpt").c_str(), ""), 0);
    ASSERT_EQ(swmm_start(0), 0);

    const char* names[] = {"C1", "C2", "C3", "C4"};
    int v6_idx[4], legacy_idx[4];
    for (int k = 0; k < 4; ++k) {
        v6_idx[k] = swmm_link_index(e, names[k]);
        legacy_idx[k] = swmm_getIndex(swmm_LINK, names[k]);
        ASSERT_GE(v6_idx[k], 0);
        ASSERT_GE(legacy_idx[k], 0);
    }

    double v6_elapsed = 0.0, legacy_elapsed = 0.0;
    int step = 0;
    do {
        ++step;
        ASSERT_EQ(swmm_engine_step(e, &v6_elapsed), 0);
        ASSERT_EQ(swmm_step(&legacy_elapsed), 0);
        for (int k = 0; k < 4; ++k) {
            double q_v6 = 0.0;
            ASSERT_EQ(swmm_link_get_flow(e, v6_idx[k], &q_v6), 0);
            const double q_legacy = swmm_getValue(swmm_LINK_FLOW, legacy_idx[k]);
            ASSERT_NEAR(q_v6, q_legacy, 1e-9 * std::max(1e-3, std::fabs(q_legacy)))
                << names[k] << " at step " << step;
        }
    } while (v6_elapsed > 0.0 && legacy_elapsed > 0.0);
    EXPECT_GT(step, 326) << "the run must reach the recession where the engines parted";

    swmm_end();
    swmm_close();
    swmm_engine_end(e);
    swmm_engine_close(e);
    swmm_engine_destroy(e);
}
