/**
 * @file test_fv_slot_volume.cpp
 * @brief The FV slot-storage diagnostic: links.slot_volume must equal the
 *        closure's own slot content when a conduit is pressurized, read 0
 *        below the crown and under the dynamic-wave router, and surface in
 *        the report as the "Final Slot Storage" continuity line.
 *
 * The pressurized gate leans on an exact identity: with every cell above
 * a_crown, the published link depth is depthOfArea(a_mean) = y_full +
 * (a_mean - a_crown)/t_slot, and the slot volume is (a_mean - a_crown)*L,
 * so slot_volume == t_slot * (depth - y_full) * L to round-off. t_slot is
 * recomputed here from first principles (g*A_full/c^2, 5%-of-Wmax cap),
 * so a wrong crown reference in the accumulation (e.g. a_full instead of
 * a_crown) shifts the magnitude and the gate bites.
 */

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_links.h>

namespace fs = std::filesystem;

namespace {

const char* kOutDir = "fv_slot_volume_out";

std::string outPath(const std::string& name) {
    fs::create_directories(kOutDir);
    return (fs::path(kOutDir) / name).string();
}

void writeFile(const std::string& path, const std::string& text) {
    std::ofstream f(path);
    f << text;
}

std::string readFile(const std::string& path) {
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// One 300-ft, 3-ft circular conduit from a junction to an outfall. The
// pressurized variant pins the outfall stage 12 ft above the crown so the
// whole barrel sits in the slot at steady state; the free variant trickles
// 1 cfs to a free outfall and never nears the crown.
std::string deck(const std::string& routing, bool pressurized) {
    std::ostringstream s;
    s << "[OPTIONS]\n"
         "FLOW_UNITS           CFS\n"
         "FLOW_ROUTING         " << routing << "\n"
         "START_DATE           01/01/2026\n"
         "START_TIME           00:00:00\n"
         "END_DATE             01/01/2026\n"
         "END_TIME             01:00:00\n"
         "REPORT_STEP          00:05:00\n"
         "ROUTING_STEP         0.5\n"
         "FV_SLOT_CELERITY     100\n"
         "LENGTHENING_STEP     0\n"
         "\n"
         "[JUNCTIONS]\n"
         ";;Name Elev MaxDepth InitDepth SurDepth Aponded\n"
         "J0     100.0 40.0 0 0 0\n"
         "\n"
         "[OUTFALLS]\n"
         ";;Name Elev Type StageData Gated\n";
    if (pressurized)
        s << "OF     100.0 FIXED 115.0 NO\n";
    else
        s << "OF     100.0 FREE NO\n";
    s << "\n"
         "[CONDUITS]\n"
         ";;Name From To Length N Zin Zout Q0\n"
         "C1     J0   OF 300 0.013 0 0 0\n"
         "\n"
         "[XSECTIONS]\n"
         ";;Link Shape G1 G2 G3 G4\n"
         "C1     CIRCULAR 3 0 0 0 1\n"
         "\n"
         "[INFLOWS]\n"
         ";;Node Constituent TimeSeries Type Mfactor Sfactor Baseline\n"
         "J0     FLOW \"\" FLOW 1.0 1.0 " << (pressurized ? 20.0 : 1.0) << "\n"
         "\n"
         "[REPORT]\n"
         "CONTINUITY YES\n";
    return s.str();
}

struct RunResult {
    double depth       = -1.0;   // published link depth (piezometric under FV)
    double volume      = -1.0;
    double slot_volume = -1.0;
    std::string rpt;
};

RunResult run(const std::string& base, const std::string& text) {
    const std::string inp = outPath(base + ".inp");
    const std::string rpt = outPath(base + ".rpt");
    writeFile(inp, text);
    RunResult r;
    SWMM_Engine e = swmm_engine_create();
    EXPECT_EQ(swmm_engine_open(e, inp.c_str(), rpt.c_str(),
                               outPath(base + ".out").c_str(), nullptr), 0)
        << swmm_get_last_error_msg(e);
    EXPECT_EQ(swmm_engine_initialize(e), 0) << swmm_get_last_error_msg(e);
    EXPECT_EQ(swmm_engine_start(e, 0), 0) << swmm_get_last_error_msg(e);
    double elapsed = 0.0;
    do {
        EXPECT_EQ(swmm_engine_step(e, &elapsed), 0)
            << swmm_get_last_error_msg(e);
    } while (elapsed > 0.0);
    const int c1 = swmm_link_index(e, "C1");
    EXPECT_GE(c1, 0);
    if (c1 >= 0) {
        EXPECT_EQ(swmm_link_get_depth(e, c1, &r.depth), 0);
        EXPECT_EQ(swmm_link_get_volume(e, c1, &r.volume), 0);
        EXPECT_EQ(swmm_link_get_slot_volume(e, c1, &r.slot_volume), 0);
    }
    swmm_engine_end(e);
    swmm_engine_report(e);   // writes the summary sections (incl. continuity)
    swmm_engine_close(e);
    swmm_engine_destroy(e);
    r.rpt = readFile(rpt);
    return r;
}

}  // namespace

TEST(FvSlotVolume, pressurizedSlotVolumeMatchesTheClosure) {
    const RunResult r = run("pressurized_fv", deck("FV", true));

    // Fully pressurized: published depth is piezometric and well above the
    // 3-ft crown (downstream boundary alone pins 15 ft over the invert).
    ASSERT_GT(r.depth, 3.5);

    const double a_full = M_PI * 1.5 * 1.5;
    const double t_req  = 32.2 * a_full / (100.0 * 100.0);  // g*A_full/c^2
    const double t_slot = std::min(t_req, 0.05 * 3.0);      // 5%-of-Wmax cap
    const double expected = t_slot * (r.depth - 3.0) * 300.0;

    ASSERT_GT(r.slot_volume, 0.0);
    EXPECT_NEAR(r.slot_volume, expected, 1.0e-6 * expected);

    // Always a subset of the link's total stored volume.
    EXPECT_LT(r.slot_volume, r.volume);

    // And the continuity block surfaces it.
    EXPECT_NE(r.rpt.find("Final Slot Storage"), std::string::npos);
}

TEST(FvSlotVolume, belowCrownReadsZeroAndReportStaysSilent) {
    const RunResult r = run("free_fv", deck("FV", false));
    ASSERT_GE(r.depth, 0.0);
    EXPECT_LT(r.depth, 2.9);                       // never near the crown band
    EXPECT_EQ(r.slot_volume, 0.0);
    EXPECT_EQ(r.rpt.find("Final Slot Storage"), std::string::npos);
}

TEST(FvSlotVolume, dynamicWaveNeverPopulatesTheChannel) {
    const RunResult r = run("pressurized_dw", deck("DYNWAVE", true));
    EXPECT_EQ(r.slot_volume, 0.0);
    EXPECT_EQ(r.rpt.find("Final Slot Storage"), std::string::npos);
}
