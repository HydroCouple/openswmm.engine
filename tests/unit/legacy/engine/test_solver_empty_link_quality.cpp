/**
 * @file test_solver_empty_link_quality.cpp
 * @brief An empty link's pollutant concentration is 0, not NaN.
 *
 * The external pollutant-flux block in qualrout.c divided by v1 + qIn*tStep,
 * which is 0 for a link holding no water and receiving none: every such link
 * reported a NaN concentration (benchmarks parity: ejemplo-epa, treattest1,
 * greenroofs, lid-imperviouscapture, 487-nodes-copper; v6 reports 0).
 */
#include <gtest/gtest.h>

#include <cmath>
#include <fstream>
#include <string>

#include "openswmm_solver.h"

namespace {

/// J1 receives nothing, so C1 stays empty; J2 has a dry-weather flow carrying
/// TSS through C2 to the outfall.
void write_deck(const std::string& path, const char* routing) {
    std::ofstream f(path);
    f << "[OPTIONS]\n"
      << "FLOW_UNITS           CFS\n"
      << "FLOW_ROUTING         " << routing << "\n"
      << "START_DATE           01/01/2026\nSTART_TIME           00:00:00\n"
      << "REPORT_START_DATE    01/01/2026\nREPORT_START_TIME    00:00:00\n"
      << "END_DATE             01/01/2026\nEND_TIME             01:00:00\n"
      << "ROUTING_STEP         0:00:30\nREPORT_STEP          0:05:00\n\n"
      << "[JUNCTIONS]\nJ1  101  5  0  0  0\nJ2  100  5  0  0  0\n\n"
      << "[OUTFALLS]\nO1  99  FREE\n\n"
      << "[CONDUITS]\nC1  J1  J2  100  0.013  0  0  0  0\n"
      << "C2  J2  O1  100  0.013  0  0  0  0\n\n"
      << "[XSECTIONS]\nC1  CIRCULAR  1  0  0  0  1\nC2  CIRCULAR  1  0  0  0  1\n\n"
      << "[POLLUTANTS]\nTSS  MG/L  0  0  0  0  NO  *  0  0  0\n\n"
      << "[DWF]\nJ2  FLOW  1.0\nJ2  TSS  10.0\n";
}

struct QualityRun {
    bool ok = false;
    bool empty_link_always_finite_zero = true;
    double carrying_link_max = 0.0;
};

/// Run the deck, checking the empty link's concentration at every step.
QualityRun run_deck(const std::string& tag, const char* routing) {
    QualityRun r;
    const std::string inp = tag + ".inp";
    write_deck(inp, routing);
    if (swmm_open(inp.c_str(), (tag + ".rpt").c_str(), (tag + ".out").c_str()) != 0) {
        ADD_FAILURE() << "swmm_open failed for " << inp;
        return r;
    }
    const int c1 = swmm_getIndex(swmm_LINK, "C1");
    const int c2 = swmm_getIndex(swmm_LINK, "C2");
    if (c1 < 0 || c2 < 0 || swmm_start(1) != 0) {
        ADD_FAILURE() << "lookup or swmm_start failed for " << inp;
        swmm_close();
        return r;
    }
    double elapsed = 0.0;
    int guard = 0;
    do {
        if (swmm_step(&elapsed) != 0) {
            ADD_FAILURE() << "swmm_step failed for " << inp;
            break;
        }
        const double empty =
            swmm_getValueExpanded(swmm_LINK, swmm_LINK_POLLUTANT_CONCENTRATION, c1, 0, 0);
        const double carrying =
            swmm_getValueExpanded(swmm_LINK, swmm_LINK_POLLUTANT_CONCENTRATION, c2, 0, 0);
        if (!std::isfinite(empty) || empty != 0.0)
            r.empty_link_always_finite_zero = false;
        if (std::isfinite(carrying))
            r.carrying_link_max = std::max(r.carrying_link_max, carrying);
    } while (elapsed > 0.0 && ++guard < 100000);
    swmm_end();
    swmm_close();
    r.ok = true;
    return r;
}

} // namespace

TEST(LegacyEmptyLinkQuality, KinematicWaveEmptyLinkIsZeroNotNan) {
    const QualityRun r = run_deck("_empty_link_kw", "KINWAVE");
    ASSERT_TRUE(r.ok);
    EXPECT_TRUE(r.empty_link_always_finite_zero);
    EXPECT_GT(r.carrying_link_max, 0.0) << "the loaded link must still carry TSS";
}

TEST(LegacyEmptyLinkQuality, SteadyFlowEmptyLinkIsZeroNotNan) {
    const QualityRun r = run_deck("_empty_link_sf", "STEADY");
    ASSERT_TRUE(r.ok);
    EXPECT_TRUE(r.empty_link_always_finite_zero);
    EXPECT_GT(r.carrying_link_max, 0.0) << "the loaded link must still carry TSS";
}
