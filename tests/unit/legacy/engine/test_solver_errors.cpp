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

#include <cstring>

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
