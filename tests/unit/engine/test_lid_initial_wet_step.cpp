/**
 * @file test_lid_initial_wet_step.cpp
 * @brief A LID unit that starts with water makes the first runoff step a wet one.
 *
 * Legacy lid_initState sets HasWetLids for any unit with an initial saturation,
 * so runoff_getTimeStep takes the WET step first. v6 started every unit's wet
 * flag at 0, so in dry weather its first runoff step was the DRY step: in
 * benchmarks greenroofs a green roof starting 1 % saturated exfiltrated its
 * stored water over one hour instead of 30 minutes, and subcatchment 11's
 * infiltration loss read half of legacy's for the first two reports. Both
 * engines run the same deck here and their LID report files must agree row
 * for row (the header's project title is left out).
 */
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/legacy/engine/openswmm_solver.h>

namespace fs = std::filesystem;

namespace {

const char* kOutDir = "lid_initial_wet_step_out"; // *_out/ is the gitignored scratch convention

/// No rain; the bioretention cell's storage starts 1 % full and seeps out
/// within the first wet step.
const char* kDeck = R"([OPTIONS]
FLOW_UNITS           CFS
INFILTRATION         HORTON
FLOW_ROUTING         KINWAVE
START_DATE           01/01/2020
START_TIME           00:00:00
END_DATE             01/01/2020
END_TIME             03:00:00
REPORT_STEP          00:05:00
WET_STEP             00:05:00
DRY_STEP             01:00:00
ROUTING_STEP         0:00:30

[RAINGAGES]
G1  INTENSITY  1:00  1.0  TIMESERIES  Dry

[SUBCATCHMENTS]
S1  G1  J1  10  50  500  0.5  0

[SUBAREAS]
S1  0.01  0.1  0.05  0.05  25  OUTLET

[INFILTRATION]
S1  3  0.5  4  7  0

[LID_CONTROLS]
BC1  BC
BC1  SURFACE  6   0     0.1  1    5
BC1  SOIL     24  0.5   0.2  0.1  0.5  10  3.5
BC1  STORAGE  12  0.75  10   0
BC1  DRAIN    0   0.5   6    6

[LID_USAGE]
S1  BC1  1  1000  10  1  0  0  "@RPT@"

[JUNCTIONS]
J1  100  5  0  0  0

[OUTFALLS]
O1  99  FREE

[CONDUITS]
C1  J1  O1  400  0.013  0  0  0  0

[XSECTIONS]
C1  CIRCULAR  1  0  0  0  1

[TIMESERIES]
Dry  0:00  0
Dry  3:00  0
)";

/// The deck with its LID report file at rpt.
std::string deckWithReport(const std::string& rpt) {
    std::string deck = kDeck;
    deck.replace(deck.find("@RPT@"), 5, rpt);
    return deck;
}

/// The report's data rows: everything after the dashed line under the header.
std::vector<std::string> dataRows(const std::string& path) {
    std::ifstream in(path);
    std::vector<std::string> rows;
    std::string line;
    bool data = false;
    while (std::getline(in, line)) {
        if (data)
            rows.push_back(line.substr(0, line.find_last_not_of(" \t\r") + 1));
        else if (line.rfind("-----------", 0) == 0)
            data = true;
    }
    return rows;
}

} // namespace

TEST(LidInitialWetStep, FirstRunoffStepIsWetAsInLegacy) {
    fs::create_directories(kOutDir);
    const fs::path dir = fs::absolute(kOutDir);
    const std::string v6_inp = (dir / "lid_initial_wet_step.v6.inp").string();
    const std::string legacy_inp = (dir / "lid_initial_wet_step.legacy.inp").string();
    const std::string v6_lid = (dir / "lid_initial_wet_step.v6.lid.txt").string();
    const std::string legacy_lid = (dir / "lid_initial_wet_step.legacy.lid.txt").string();
    std::ofstream(v6_inp) << deckWithReport(v6_lid);
    std::ofstream(legacy_inp) << deckWithReport(legacy_lid);

    SWMM_Engine e = swmm_engine_create();
    ASSERT_EQ(swmm_engine_open(e, v6_inp.c_str(), (v6_inp + ".rpt").c_str(),
                               (v6_inp + ".out").c_str(), nullptr),
              0)
        << swmm_get_last_error_msg(e);
    ASSERT_EQ(swmm_engine_initialize(e), 0) << swmm_get_last_error_msg(e);
    ASSERT_EQ(swmm_engine_start(e, 0), 0) << swmm_get_last_error_msg(e);
    double elapsed = 0.0;
    do {
        ASSERT_EQ(swmm_engine_step(e, &elapsed), 0);
    } while (elapsed > 0.0);
    swmm_engine_end(e);
    swmm_engine_close(e);
    swmm_engine_destroy(e);

    ASSERT_EQ(swmm_open(legacy_inp.c_str(), (legacy_inp + ".rpt").c_str(), ""), 0);
    ASSERT_EQ(swmm_start(0), 0);
    do {
        ASSERT_EQ(swmm_step(&elapsed), 0);
    } while (elapsed > 0.0);
    swmm_end();
    swmm_close();

    const auto legacy_rows = dataRows(legacy_lid);
    const auto v6_rows = dataRows(v6_lid);
    ASSERT_FALSE(legacy_rows.empty());
    // The first wet step ends at 0:05 and seeps the stored water out.
    EXPECT_EQ(legacy_rows.front().rfind(" 01/01/2020 00:05:00", 0), 0u) << legacy_rows.front();
    EXPECT_EQ(v6_rows, legacy_rows);
}
