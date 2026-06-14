/*
 *   test_solver_errors.cpp
 *
 *   Created: 02/01/2024
 *   Updated: 2026-03-25
 *
 *   Unit tests for SWMM solver error-handling paths (Google Test).
 *
 *   Tests cover:
 *     - Calling API functions before swmm_open()
 *     - Calling API functions before swmm_start()
 *     - Non-existent / invalid input files
 *     - Out-of-bounds object indices and unknown names
 *     - Error-code retrieval functions
 */

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#include "openswmm_solver.h"

#define MODEL_INP "./hotstart/site_drainage_model.inp"
#define ERR_RPT   "./hotstart/_err_test.rpt"
#define ERR_OUT   "./hotstart/_err_test.out"

// ============================================================
// Before swmm_open() — model is in initial uninitialized state
// ============================================================

TEST(SolverPreOpenErrors, StepBeforeOpen) {
    // swmm_step without an open model should not succeed
    double elapsed = 0.0;
    int err = swmm_step(&elapsed);
    EXPECT_NE(err, 0);
}

TEST(SolverPreOpenErrors, EndBeforeOpen) {
    // swmm_end without an open model should not succeed
    int err = swmm_end();
    EXPECT_NE(err, 0);
}

TEST(SolverPreOpenErrors, GetCountBeforeOpen) {
    // Without a loaded model, count should be 0 (not negative crash)
    int count = swmm_getCount(swmm_NODE);
    EXPECT_EQ(count, 0);
}

// ============================================================
// Invalid file paths
// ============================================================

TEST(SolverFileErrors, OpenNonExistentFile) {
    int err = swmm_open("./does_not_exist.inp", ERR_RPT, ERR_OUT);
    EXPECT_NE(err, 0);
    swmm_close();
}

TEST(SolverFileErrors, RunNonExistentFile) {
    int err = swmm_run("./does_not_exist.inp", ERR_RPT, ERR_OUT);
    EXPECT_NE(err, 0);
}

TEST(SolverFileErrors, OpenEmptyStringPath) {
    int err = swmm_open("", ERR_RPT, ERR_OUT);
    EXPECT_NE(err, 0);
    swmm_close();
}

// ============================================================
// Fixture: model is open but not started
// ============================================================

class SolverOpenErrorFixture : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_EQ(swmm_open(MODEL_INP, ERR_RPT, ERR_OUT), 0);
    }
    void TearDown() override {
        swmm_close();
    }
};

TEST_F(SolverOpenErrorFixture, StepBeforeStart) {
    double elapsed = 0.0;
    int err = swmm_step(&elapsed);
    EXPECT_NE(err, 0);
}

TEST_F(SolverOpenErrorFixture, GetNameOutOfBounds) {
    char name[64] = {};
    int nodeCount = swmm_getCount(swmm_NODE);
    ASSERT_GT(nodeCount, 0);
    // Index equal to count is past the end — should return an error
    int err = swmm_getName(swmm_NODE, nodeCount, name, sizeof(name));
    EXPECT_NE(err, 0);
}

TEST_F(SolverOpenErrorFixture, GetLinkNameOutOfBounds) {
    char name[64] = {};
    int linkCount = swmm_getCount(swmm_LINK);
    ASSERT_GT(linkCount, 0);
    int err = swmm_getName(swmm_LINK, linkCount, name, sizeof(name));
    EXPECT_NE(err, 0);
}

TEST_F(SolverOpenErrorFixture, GetIndexNotFound_Node) {
    int idx = swmm_getIndex(swmm_NODE, "NODE_DOES_NOT_EXIST");
    EXPECT_LT(idx, 0);
}

TEST_F(SolverOpenErrorFixture, GetIndexNotFound_Link) {
    int idx = swmm_getIndex(swmm_LINK, "LINK_DOES_NOT_EXIST");
    EXPECT_LT(idx, 0);
}

TEST_F(SolverOpenErrorFixture, GetSubcatchStatsBeforeEnd) {
    // Stats require swmm_end() to have been called first
    swmm_SubcatchStats stats = {};
    int err = swmm_getSubcatchStats(0, &stats);
    EXPECT_NE(err, 0);
}

TEST_F(SolverOpenErrorFixture, GetNodeStatsBeforeEnd) {
    swmm_NodeStats stats = {};
    int err = swmm_getNodeStats(0, &stats);
    EXPECT_NE(err, 0);
}

TEST_F(SolverOpenErrorFixture, GetLinkStatsBeforeEnd) {
    swmm_LinkStats stats = {};
    int err = swmm_getLinkStats(0, &stats);
    EXPECT_NE(err, 0);
}

// ============================================================
// Error retrieval and message inspection
// ============================================================

TEST(SolverErrorRetrieval, GetErrorAfterFailedOpen) {
    swmm_open("./no_such_file.inp", ERR_RPT, ERR_OUT);
    char errMsg[512] = {};
    int code = swmm_getError(errMsg, sizeof(errMsg));
    // A non-zero code should have been recorded
    EXPECT_NE(code, 0);
    swmm_close();
}

TEST(SolverErrorRetrieval, GetErrorFromCode) {
    // Should not crash; the returned message may be empty for unknown codes
    char buf[256] = "original";
    char* pBuf = buf;
    swmm_getErrorFromCode(ERR_API_NOT_OPEN, &pBuf);
    // Just verify the call doesn't crash
}

TEST(SolverErrorRetrieval, GetVersionIsPositive) {
    // Sanity: version is always accessible regardless of simulation state
    EXPECT_GT(swmm_getVersion(), 0);
}

// ============================================================
// Error 227: Zero channel Manning's n in TRANSECTS
// ============================================================

// Minimal .inp content for a conduit with a transect whose channel n = 0.
// swmm_open must reject this and set error code 227.
static const char *kZeroManningInp = R"(
[TITLE]
Zero Manning transect test

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
;;Name  Elev  MaxDepth InitDepth SurDepth Aponded
J1      100   5        0         0        0
J2      99    5        0         0        0

[OUTFALLS]

[CONDUITS]
;;Name  From  To   Length  Manning  InOffset OutOffset InitFlow MaxFlow
C1      J1    J2   100     0.013    0        0         0        0

[XSECTIONS]
;;Name  Shape     Geom1 Geom2 Geom3 Geom4 Barrels
C1      IRREGULAR TZN   0     0     0     1

[TRANSECTS]
NC 0.04 0.04 0.0
X1 TZN  3  1.0  9.0  0  0  0  0  0
GR 100 0  90 5  100 10

[REPORT]
SUBCATCHMENTS  NONE
NODES          NONE
LINKS          NONE
)";

TEST(SolverTransectErrors, ZeroChannelManningIsError227) {
    // Write minimal .inp with zero channel Manning's n to a temp file.
    char *tname = std::tmpnam(nullptr);
    std::string inpPath = std::string(tname) + "_zero_n.inp";
    std::string rptPath = std::string(tname) + "_zero_n.rpt";
    std::string outPath = std::string(tname) + "_zero_n.out";

    {
        std::FILE *f = std::fopen(inpPath.c_str(), "w");
        ASSERT_NE(f, nullptr);
        std::fputs(kZeroManningInp, f);
        std::fclose(f);
    }

    int rc = swmm_open(inpPath.c_str(), rptPath.c_str(), outPath.c_str());
    EXPECT_NE(rc, 0) << "swmm_open should fail for zero channel Manning's n";

    // Since commit b0cdd62b the zero-n NC line is rejected at PARSE time
    // (transect.c setManning), so the run finishes with the canonical
    // line-level input error code 200 (ERR_INPUT) — legacy semantics for
    // any per-line input error — while the specific ERROR 227 detail is
    // written to the report file. Pin both.
    char errMsg[512] = {};
    int code = swmm_getError(errMsg, sizeof(errMsg));
    EXPECT_EQ(code, 200) << "Expected generic input error 200, got " << code
                          << " message: " << errMsg;

    swmm_close();

    {
        std::ifstream rpt(rptPath);
        ASSERT_TRUE(rpt.good()) << "report file missing: " << rptPath;
        std::stringstream ss;
        ss << rpt.rdbuf();
        EXPECT_NE(ss.str().find("ERROR 227"), std::string::npos)
            << "report must carry the specific ERROR 227 detail";
    }

    std::remove(inpPath.c_str());
    std::remove(rptPath.c_str());
    std::remove(outPath.c_str());
}
