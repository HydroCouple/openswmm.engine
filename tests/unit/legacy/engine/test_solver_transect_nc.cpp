/*
 *   test_solver_transect_nc.cpp
 *
 *   Created: 2026-08-16
 *
 *   Regression tests for [TRANSECTS] NC roughness inheritance (issue #132).
 *
 *   EPA SWMM 5.2.4 semantics: a zero component in an NC record means the
 *   corresponding Manning's n is UNCHANGED from the preceding NC record.
 *   transect.c::setManning() used to reject n[3] == 0 outright, before the
 *   inheritance assignments ran, so a valid continuation record such as
 *
 *       NC 0.04 0.05 0.03
 *       X1 ... GR ...
 *       NC 0 0 0            <-- inherit all three
 *       X1 ... GR ...
 *
 *   failed to load with ERROR 227. Error 227 must still be raised when no
 *   positive channel roughness has ever been established.
 *
 *   Each test writes a self-contained .inp to a temp path and loads it
 *   through swmm_open(); the models differ only in their [TRANSECTS] block.
 */

#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <string>

#include "openswmm_solver.h"

namespace {

constexpr int ERR_TRANSECT_MANNING = 227;

// Two conduits with IRREGULAR cross-sections T1 and T2. The [TRANSECTS]
// block is supplied per-test so only the NC records vary.
constexpr const char *kInpHead = R"(
[TITLE]
TRANSECTS NC inheritance regression (issue #132)

[OPTIONS]
FLOW_UNITS           CFS
INFILTRATION         HORTON
FLOW_ROUTING         DYNWAVE
START_DATE           01/01/2020
START_TIME           00:00:00
END_DATE             01/01/2020
END_TIME             00:30:00
REPORT_STEP          00:05:00
WET_STEP             00:05:00
DRY_STEP             01:00:00
ROUTING_STEP         00:00:15
LINK_OFFSETS         DEPTH

[JUNCTIONS]
;;Name Elev MaxDepth InitDepth SurDepth Aponded
J1      10   10       0         0        0
J2       9   10       0         0        0

[OUTFALLS]
;;Name Elev Type StageData Gated
O1       8   FREE           NO

[CONDUITS]
;;Name From To Length Rough InOff OutOff InitFlow MaxFlow
C1     J1   J2 400    0.01  0     0      0        0
C2     J2   O1 400    0.01  0     0      0        0

[XSECTIONS]
C1     IRREGULAR T1
C2     IRREGULAR T2

[TRANSECTS]
)";

constexpr const char *kInpTail = R"(
[REPORT]
SUBCATCHMENTS NONE
NODES         NONE
LINKS         NONE
)";

// Writes "head + transectBlock + tail" to a unique temp path.
std::string writeTempInp(const std::string &transectBlock) {
    static int counter = 0;
    std::string path = std::string(std::tmpnam(nullptr)) + "_nc" +
                       std::to_string(counter++) + ".inp";
    std::ofstream out(path);
    out << kInpHead << transectBlock << kInpTail;
    return path;
}

// Opens a model built from `transectBlock` and returns swmm_open's code.
// On success the caller is responsible for swmm_close().
int openWithTransects(const std::string &transectBlock,
                      std::string &inpPathOut) {
    inpPathOut = writeTempInp(transectBlock);
    const std::string rpt = inpPathOut + ".rpt";
    const std::string out = inpPathOut + ".out";
    return swmm_open(inpPathOut.c_str(), rpt.c_str(), out.c_str());
}

// swmm_open() reports input problems with the aggregate code ERR_INPUT (200);
// the specific code (e.g. 227) is written to the report file, so that is where
// a test has to look to tell one input error from another.
bool reportMentions(const std::string &inpPath, const std::string &needle) {
    std::ifstream rpt(inpPath + ".rpt");
    std::string line;
    while (std::getline(rpt, line)) {
        if (line.find(needle) != std::string::npos) return true;
    }
    return false;
}

void cleanup(const std::string &inpPath) {
    swmm_close();
    std::remove(inpPath.c_str());
    std::remove((inpPath + ".rpt").c_str());
    std::remove((inpPath + ".out").c_str());
}

// T1 declares the roughness triple; T2's NC record is the variable part.
std::string block(const std::string &firstNc, const std::string &secondNc) {
    return firstNc + "\n" +
           "X1 T1 7 10 20 0 0 0 0 0\n"
           "GR 15 0 10 5 10 10 5 15 10 20 10 25 15 30\n" +
           secondNc + "\n" +
           "X1 T2 7 10 20 0 0 0 0 0\n"
           "GR 15 0 10 5 10 10 5 15 10 20 10 25 15 30\n";
}

// ============================================================
// Inheritance
// ============================================================

// An all-zero continuation record must load, not fail with error 227.
TEST(SolverTransectNc, AllZeroNcRecordInheritsAndOpens) {
    std::string inp;
    ASSERT_EQ(openWithTransects(block("NC 0.04 0.05 0.03", "NC 0 0 0"), inp), 0);
    cleanup(inp);
}

// The inherited values must be the preceding ones, not zero. Full-capacity
// flow is computed from the transect's channel roughness, so an inherited
// T2 must have exactly the same qFull as an explicitly declared one.
TEST(SolverTransectNc, InheritedRoughnessMatchesExplicitRoughness) {
    std::string inpInherited;
    ASSERT_EQ(openWithTransects(block("NC 0.04 0.05 0.03", "NC 0 0 0"),
                                inpInherited), 0);
    int idx = swmm_getIndex(swmm_LINK, "C2");
    ASSERT_GE(idx, 0);
    const double qFullInherited = swmm_getValue(swmm_LINK_FULLFLOW, idx);
    cleanup(inpInherited);

    std::string inpExplicit;
    ASSERT_EQ(openWithTransects(block("NC 0.04 0.05 0.03", "NC 0.04 0.05 0.03"),
                                inpExplicit), 0);
    idx = swmm_getIndex(swmm_LINK, "C2");
    ASSERT_GE(idx, 0);
    const double qFullExplicit = swmm_getValue(swmm_LINK_FULLFLOW, idx);
    cleanup(inpExplicit);

    EXPECT_GT(qFullInherited, 0.0);
    EXPECT_DOUBLE_EQ(qFullInherited, qFullExplicit);
}

// Inheritance is per-component: a positive entry overwrites, a zero inherits.
// Overwriting the channel roughness alone must change qFull.
TEST(SolverTransectNc, PositiveComponentOverwritesInheritedChannelValue) {
    std::string inpInherited;
    ASSERT_EQ(openWithTransects(block("NC 0.04 0.05 0.03", "NC 0 0 0"),
                                inpInherited), 0);
    int idx = swmm_getIndex(swmm_LINK, "C2");
    ASSERT_GE(idx, 0);
    const double qFullInherited = swmm_getValue(swmm_LINK_FULLFLOW, idx);
    cleanup(inpInherited);

    std::string inpOverwritten;
    ASSERT_EQ(openWithTransects(block("NC 0.04 0.05 0.03", "NC 0 0 0.06"),
                                inpOverwritten), 0);
    idx = swmm_getIndex(swmm_LINK, "C2");
    ASSERT_GE(idx, 0);
    const double qFullOverwritten = swmm_getValue(swmm_LINK_FULLFLOW, idx);
    cleanup(inpOverwritten);

    // Doubling n roughly halves qFull — the point is only that the explicit
    // channel value took effect instead of being inherited.
    EXPECT_LT(qFullOverwritten, qFullInherited);
}

// ============================================================
// Error 227 is still raised when there is nothing to inherit
// ============================================================

// A zero channel component with no positive predecessor is a real error.
TEST(SolverTransectNc, ZeroChannelWithNoPredecessorIsError227) {
    std::string inp;
    EXPECT_NE(openWithTransects(block("NC 0 0 0", "NC 0.04 0.05 0.03"), inp), 0);
    swmm_close();   // flush the report before reading the error code out of it
    EXPECT_TRUE(reportMentions(inp, std::to_string(ERR_TRANSECT_MANNING)))
        << "Expected ERROR 227 in the report when no channel roughness has "
           "ever been established";
    cleanup(inp);
}

// A negative component remains invalid input, never an inheritance request.
TEST(SolverTransectNc, NegativeComponentIsRejected) {
    std::string inp;
    EXPECT_NE(openWithTransects(block("NC 0.04 0.05 0.03", "NC -0.01 0 0"),
                                inp), 0);
    cleanup(inp);
}

} // namespace
