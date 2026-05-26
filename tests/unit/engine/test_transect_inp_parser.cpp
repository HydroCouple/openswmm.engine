/**
 * @file test_transect_inp_parser.cpp
 * @brief Pins the [TRANSECTS] X1-line column mapping in the .inp parser.
 *
 *  EPA SWMM 5 (legacy transect.c::setParams) layout:
 *      X1  Name  Nsta  Xleft  Xright  0  0  Lfactor  Xfactor  Yfactor
 *  Tokens                 3       4    5  6     7        8        9
 *
 *  Two earlier bugs the parser had:
 *    1. length_factor was hardcoded to 1.0 (tok[7] dropped on the floor);
 *       x_factor read tok[8] OK but y_factor read tok[9] OK — column
 *       mapping was right by coincidence on the multiplier pair, but
 *       Lfactor was always lost.
 *    2. Zero values for Lfactor / Xfactor were stored verbatim instead of
 *       being defaulted to 1.0 (legacy SWMM behaviour). Combined with EPA-
 *       generated .inp files that emit "0" placeholders, this collapsed
 *       every plotted station to x*0 = 0 — the "vertical line" cross-
 *       section symptom in the GUI's Transect Editor.
 *
 *  These tests load minimal .inp files on disk through swmm_engine_open
 *  and read back the modifiers via swmm_transect_get_modifiers.
 */

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_infrastructure.h>

namespace {

// Minimal valid .inp skeleton with a single placeholder conduit/transect.
// Just enough sections for swmm_engine_open() to succeed.
constexpr const char *kInpSkeletonHead = R"(
[TITLE]
Transect parser test

[OPTIONS]
FLOW_UNITS           CFS
INFILTRATION         HORTON
FLOW_ROUTING         KINWAVE
LINK_OFFSETS         DEPTH
MIN_SLOPE            0
ALLOW_PONDING        NO
SKIP_STEADY_STATE    NO
START_DATE           01/01/2024
START_TIME           00:00:00
REPORT_START_DATE    01/01/2024
REPORT_START_TIME    00:00:00
END_DATE             01/01/2024
END_TIME             00:30:00
SWEEP_START          01/01
SWEEP_END            12/31
DRY_DAYS             0
REPORT_STEP          00:01:00
WET_STEP             00:01:00
DRY_STEP             00:01:00
ROUTING_STEP         00:00:30

[JUNCTIONS]
;;Name           Elev  MaxDepth InitDepth SurDepth Aponded
J1               100   5        0         0        0
J2               99    5        0         0        0

[OUTFALLS]
;;Name           Elev  Type      StageData          Gated   RouteTo

[CONDUITS]
;;Name           From  To   Length    Manning   InOffset  OutOffset  InitFlow  MaxFlow
C1               J1    J2   100       0.013     0         0          0         0

[XSECTIONS]
;;Name           Shape       Geom1   Geom2 Geom3 Geom4 Barrels
C1               CIRCULAR    1       0     0     0     1

[TRANSECTS]
)";

constexpr const char *kInpTail = R"(

[REPORT]
SUBCATCHMENTS  NONE
NODES          NONE
LINKS          NONE
)";

// Write `inpContents` (which is "skeleton + transect_block + tail") to a
// temp file and return its path. The caller must delete the path.
std::string writeTempInp(const std::string &transectBlock) {
    std::string path;
    {
        // mkstemp pattern — XXXXXX is replaced in place.
        char tmpl[] = "/tmp/swmm_transect_test_XXXXXX.inp";
        // Use a deterministic-ish path; mkstemps would need <unistd.h> on
        // macOS. tmpnam is deprecated but adequate here for a unit test.
        char *tname = std::tmpnam(nullptr);
        path = tname;
        path += ".inp";
        (void)tmpl;
    }
    std::ofstream out(path);
    out << kInpSkeletonHead << transectBlock << kInpTail;
    return path;
}

class TransectInpParserTest : public ::testing::Test {
protected:
    SWMM_Engine eng = nullptr;
    std::string inpPath;
    std::string rptPath;
    std::string outPath;

    void TearDown() override {
        if (eng) {
            swmm_engine_close(eng);
            swmm_engine_destroy(eng);
            eng = nullptr;
        }
        if (!inpPath.empty()) std::remove(inpPath.c_str());
        if (!rptPath.empty()) std::remove(rptPath.c_str());
        if (!outPath.empty()) std::remove(outPath.c_str());
    }

    // Helper — write the transect block, open, return the engine.
    bool loadWithTransects(const std::string &transectBlock) {
        inpPath = writeTempInp(transectBlock);
        rptPath = inpPath + ".rpt";
        outPath = inpPath + ".out";
        eng = swmm_engine_create();
        if (!eng) return false;
        return swmm_engine_open(eng, inpPath.c_str(),
                                  rptPath.c_str(), outPath.c_str(), nullptr)
               == SWMM_OK;
    }
};

// ---------------------------------------------------------------------------
// All-zero modifiers (EPA-generated files) must default to Lfactor=1,
// Xfactor=1 — otherwise station*0 collapses the cross-section.
// ---------------------------------------------------------------------------

TEST_F(TransectInpParserTest, ZeroModifiersDefaultToOne) {
    const std::string block =
        "NC 0.04 0.04 0.04\n"
        "X1 TRZ              4       2.0 8.0 0 0 0 0 0\n"
        "GR 105 0  100 2  100 8  105 10\n";
    ASSERT_TRUE(loadWithTransects(block));

    const int idx = swmm_transect_index(eng, "TRZ");
    ASSERT_GE(idx, 0);

    double xF = -1, yF = -1, lF = -1;
    ASSERT_EQ(swmm_transect_get_modifiers(eng, idx, &xF, &yF, &lF), SWMM_OK);
    EXPECT_DOUBLE_EQ(lF, 1.0);   // Lfactor zero -> default 1.0
    EXPECT_DOUBLE_EQ(xF, 1.0);   // Xfactor zero -> default 1.0
    EXPECT_DOUBLE_EQ(yF, 0.0);   // Yfactor 0 is a valid offset (no shift).

    // Stations come through raw (un-multiplied) on the engine side.
    ASSERT_EQ(swmm_transect_get_station_count(eng, idx), 4);
    double s = -1, e = -1;
    ASSERT_EQ(swmm_transect_get_station(eng, idx, 0, &s, &e), SWMM_OK);
    EXPECT_DOUBLE_EQ(s, 0.0);
    EXPECT_DOUBLE_EQ(e, 105.0);
    ASSERT_EQ(swmm_transect_get_station(eng, idx, 2, &s, &e), SWMM_OK);
    EXPECT_DOUBLE_EQ(s, 8.0);
    EXPECT_DOUBLE_EQ(e, 100.0);
}

// ---------------------------------------------------------------------------
// Explicit non-default modifiers must be read from the correct token columns.
//   X1 ... 0 0  Lfactor=1.5  Xfactor=2.0  Yfactor=3.0
// Before the fix, length_factor was hardcoded to 1.0 (1.5 lost).
// ---------------------------------------------------------------------------

TEST_F(TransectInpParserTest, ExplicitModifiersReadFromCorrectColumns) {
    const std::string block =
        "NC 0.04 0.04 0.04\n"
        "X1 TRM              3       1.0 9.0 0 0 1.5 2.0 3.0\n"
        "GR 100 0  90 5  100 10\n";
    ASSERT_TRUE(loadWithTransects(block));

    const int idx = swmm_transect_index(eng, "TRM");
    ASSERT_GE(idx, 0);

    double xF = -1, yF = -1, lF = -1;
    ASSERT_EQ(swmm_transect_get_modifiers(eng, idx, &xF, &yF, &lF), SWMM_OK);
    EXPECT_DOUBLE_EQ(lF, 1.5);
    EXPECT_DOUBLE_EQ(xF, 2.0);
    EXPECT_DOUBLE_EQ(yF, 3.0);
}

// ---------------------------------------------------------------------------
// Bank stations also live in tok[3]/tok[4] — pin them so a future re-shuffle
// doesn't silently misread them.
// ---------------------------------------------------------------------------

TEST_F(TransectInpParserTest, BankStationsReadFromCorrectColumns) {
    const std::string block =
        "NC 0.04 0.04 0.04\n"
        "X1 TBK              3       2.5 7.5 0 0 0 0 0\n"
        "GR 100 0  90 5  100 10\n";
    ASSERT_TRUE(loadWithTransects(block));

    const int idx = swmm_transect_index(eng, "TBK");
    ASSERT_GE(idx, 0);

    double xLb = -1, xRb = -1;
    ASSERT_EQ(swmm_transect_get_bank_stations(eng, idx, &xLb, &xRb), SWMM_OK);
    EXPECT_DOUBLE_EQ(xLb, 2.5);
    EXPECT_DOUBLE_EQ(xRb, 7.5);
}

} // namespace
