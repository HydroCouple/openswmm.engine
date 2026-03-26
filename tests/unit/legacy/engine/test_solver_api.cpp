/*
 *   test_solver_api.cpp
 *
 *   Created: 02/01/2024
 *   Updated: 2026-03-25
 *
 *   Comprehensive unit tests for the SWMM solver API (Google Test).
 *
 *   Model used: site_drainage_model.inp
 *     - 11 junctions (J1–J11), 1 outfall (O1) = 12 nodes
 *     - 11 conduits (C1–C11) — mix of CIRCULAR and TRAPEZOIDAL
 *     - 7 subcatchments (S1–S7), 1 rain gage
 *     - Flow units: CFS, simulation period 1998-01-01 to 1998-01-02 06:00
 */

#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>

#include "openswmm_solver.h"

// Paths relative to the test working directory (tests/unit/legacy/engine/data)
#define MODEL_INP "./hotstart/site_drainage_model.inp"
#define API_RPT   "./hotstart/_api_test.rpt"
#define API_OUT   "./hotstart/_api_test.out"

// ============================================================
// Standalone (no-fixture) tests — no simulation state required
// ============================================================

TEST(SolverVersion, GetVersion) {
    int version = swmm_getVersion();
    EXPECT_GE(version, 50000);
    EXPECT_LT(version, 70000);
}

TEST(SolverRun, FullRunReturnsSuccess) {
    int err = swmm_run(MODEL_INP, API_RPT, API_OUT);
    EXPECT_EQ(err, 0);
}

TEST(SolverRun, OutputFileCreated) {
    swmm_run(MODEL_INP, API_RPT, API_OUT);
    std::ifstream f(API_OUT);
    EXPECT_TRUE(f.good()) << "Output file was not created";
}

TEST(SolverDate, DecodeDateRoundtrip) {
    double encoded = swmm_encodeDate(1998, 1, 15, 8, 30, 45);
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0, dow = 0;
    swmm_decodeDate(encoded, &y, &mo, &d, &h, &mi, &s, &dow);
    EXPECT_EQ(y,  1998);
    EXPECT_EQ(mo, 1);
    EXPECT_EQ(d,  15);
    EXPECT_EQ(h,  8);
    EXPECT_EQ(mi, 30);
    EXPECT_EQ(s,  45);
}

TEST(SolverDate, EncodeDateDayIncrement) {
    double t1 = swmm_encodeDate(2000, 1, 1, 0, 0, 0);
    double t2 = swmm_encodeDate(2000, 1, 2, 0, 0, 0);
    EXPECT_NEAR(t2 - t1, 1.0, 1e-9);
}

TEST(SolverDate, DecodeDateStartOfSimulation) {
    // Model START_DATE = 01/01/1998 00:00:00
    double encoded = swmm_encodeDate(1998, 1, 1, 0, 0, 0);
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0, dow = 0;
    swmm_decodeDate(encoded, &y, &mo, &d, &h, &mi, &s, &dow);
    EXPECT_EQ(y, 1998);
    EXPECT_EQ(mo, 1);
    EXPECT_EQ(d,  1);
    EXPECT_EQ(h,  0);
    EXPECT_EQ(mi, 0);
    EXPECT_EQ(s,  0);
}

// ============================================================
// Fixture: model is open (not started) — static properties available
// ============================================================

class SolverOpenFixture : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_EQ(swmm_open(MODEL_INP, API_RPT, API_OUT), 0) << "swmm_open failed";
    }
    void TearDown() override {
        swmm_close();
    }
};

// --- Object counts ---

TEST_F(SolverOpenFixture, GetCountNodes) {
    // 11 junctions + 1 outfall = 12 nodes
    EXPECT_EQ(swmm_getCount(swmm_NODE), 12);
}

TEST_F(SolverOpenFixture, GetCountLinks) {
    EXPECT_EQ(swmm_getCount(swmm_LINK), 11);
}

TEST_F(SolverOpenFixture, GetCountSubcatchments) {
    EXPECT_EQ(swmm_getCount(swmm_SUBCATCH), 7);
}

TEST_F(SolverOpenFixture, GetCountGages) {
    EXPECT_EQ(swmm_getCount(swmm_GAGE), 1);
}

// --- Object names ---

TEST_F(SolverOpenFixture, GetFirstNodeName) {
    char name[64] = {};
    ASSERT_EQ(swmm_getName(swmm_NODE, 0, name, sizeof(name)), 0);
    EXPECT_STREQ(name, "J1");
}

TEST_F(SolverOpenFixture, GetLastJunctionName) {
    char name[64] = {};
    ASSERT_EQ(swmm_getName(swmm_NODE, 10, name, sizeof(name)), 0);
    EXPECT_STREQ(name, "J11");
}

TEST_F(SolverOpenFixture, GetOutfallName) {
    char name[64] = {};
    ASSERT_EQ(swmm_getName(swmm_NODE, 11, name, sizeof(name)), 0);
    EXPECT_STREQ(name, "O1");
}

TEST_F(SolverOpenFixture, GetFirstLinkName) {
    char name[64] = {};
    ASSERT_EQ(swmm_getName(swmm_LINK, 0, name, sizeof(name)), 0);
    EXPECT_STREQ(name, "C1");
}

TEST_F(SolverOpenFixture, GetLastLinkName) {
    char name[64] = {};
    ASSERT_EQ(swmm_getName(swmm_LINK, 10, name, sizeof(name)), 0);
    EXPECT_STREQ(name, "C11");
}

TEST_F(SolverOpenFixture, GetFirstSubcatchName) {
    char name[64] = {};
    ASSERT_EQ(swmm_getName(swmm_SUBCATCH, 0, name, sizeof(name)), 0);
    EXPECT_STREQ(name, "S1");
}

// --- Index lookups ---

TEST_F(SolverOpenFixture, GetIndexByName_Nodes) {
    EXPECT_EQ(swmm_getIndex(swmm_NODE, "J1"),  0);
    EXPECT_EQ(swmm_getIndex(swmm_NODE, "J5"),  4);
    EXPECT_EQ(swmm_getIndex(swmm_NODE, "J11"), 10);
    EXPECT_EQ(swmm_getIndex(swmm_NODE, "O1"),  11);
}

TEST_F(SolverOpenFixture, GetIndexByName_Links) {
    EXPECT_EQ(swmm_getIndex(swmm_LINK, "C1"),  0);
    EXPECT_EQ(swmm_getIndex(swmm_LINK, "C6"),  5);
    EXPECT_EQ(swmm_getIndex(swmm_LINK, "C11"), 10);
}

TEST_F(SolverOpenFixture, GetIndexByName_NotFound) {
    EXPECT_LT(swmm_getIndex(swmm_NODE, "NONEXISTENT"), 0);
    EXPECT_LT(swmm_getIndex(swmm_LINK, "NONEXISTENT"), 0);
}

// --- Static node properties ---

TEST_F(SolverOpenFixture, GetNodeElevations) {
    struct { const char* name; double elev; } expected[] = {
        {"J1",  4973.0}, {"J2",  4969.0}, {"J11", 4963.0}
    };
    for (const auto& e : expected) {
        int idx = swmm_getIndex(swmm_NODE, e.name);
        ASSERT_GE(idx, 0) << "Node " << e.name << " not found";
        EXPECT_NEAR(swmm_getValue(swmm_NODE_ELEV, idx), e.elev, 0.01)
            << "Node " << e.name << " elevation mismatch";
    }
}

TEST_F(SolverOpenFixture, GetNodeType_AllJunctions) {
    for (int i = 0; i < 11; ++i)
        EXPECT_EQ(static_cast<int>(swmm_getValue(swmm_NODE_TYPE, i)), swmm_JUNCTION)
            << "Node " << i << " should be JUNCTION";
}

TEST_F(SolverOpenFixture, GetNodeType_Outfall) {
    EXPECT_EQ(static_cast<int>(swmm_getValue(swmm_NODE_TYPE, 11)), swmm_OUTFALL);
}

// --- Static link properties ---

TEST_F(SolverOpenFixture, GetLinkLengths) {
    struct { const char* name; double len; } expected[] = {
        {"C1", 185.0}, {"C2", 526.0}, {"C3", 109.0},
        {"C7",  95.0}, {"C11", 89.0}
    };
    for (const auto& e : expected) {
        int idx = swmm_getIndex(swmm_LINK, e.name);
        ASSERT_GE(idx, 0) << "Link " << e.name << " not found";
        EXPECT_NEAR(swmm_getValue(swmm_LINK_LENGTH, idx), e.len, 0.1)
            << "Link " << e.name << " length mismatch";
    }
}

TEST_F(SolverOpenFixture, GetLinkType_AllConduits) {
    int nLinks = swmm_getCount(swmm_LINK);
    EXPECT_GT(nLinks, 0);
    for (int i = 0; i < nLinks; ++i)
        EXPECT_EQ(static_cast<int>(swmm_getValue(swmm_LINK_TYPE, i)), swmm_CONDUIT)
            << "Link " << i << " should be CONDUIT";
}

TEST_F(SolverOpenFixture, GetLinkFullDepths) {
    // CIRCULAR: C3=2.25, C7=3.5, C11=4.75
    // TRAPEZOIDAL: C1=3, C2=1, C4–C6,C8–C10=3
    struct { const char* name; double depth; } expected[] = {
        {"C1", 3.0}, {"C2", 1.0}, {"C3", 2.25},
        {"C7", 3.5}, {"C11", 4.75}
    };
    for (const auto& e : expected) {
        int idx = swmm_getIndex(swmm_LINK, e.name);
        ASSERT_GE(idx, 0) << "Link " << e.name << " not found";
        EXPECT_NEAR(swmm_getValue(swmm_LINK_FULLDEPTH, idx), e.depth, 0.001)
            << "Link " << e.name << " full depth mismatch";
    }
}

TEST_F(SolverOpenFixture, GetLinkConnectivity_C1) {
    int c1 = swmm_getIndex(swmm_LINK, "C1");
    int j1 = swmm_getIndex(swmm_NODE, "J1");
    int j5 = swmm_getIndex(swmm_NODE, "J5");
    ASSERT_GE(c1, 0); ASSERT_GE(j1, 0); ASSERT_GE(j5, 0);
    EXPECT_EQ(static_cast<int>(swmm_getValue(swmm_LINK_NODE1, c1)), j1);
    EXPECT_EQ(static_cast<int>(swmm_getValue(swmm_LINK_NODE2, c1)), j5);
}

TEST_F(SolverOpenFixture, GetLinkConnectivity_C11) {
    int c11 = swmm_getIndex(swmm_LINK, "C11");
    int j11 = swmm_getIndex(swmm_NODE, "J11");
    int o1  = swmm_getIndex(swmm_NODE, "O1");
    ASSERT_GE(c11, 0); ASSERT_GE(j11, 0); ASSERT_GE(o1, 0);
    EXPECT_EQ(static_cast<int>(swmm_getValue(swmm_LINK_NODE1, c11)), j11);
    EXPECT_EQ(static_cast<int>(swmm_getValue(swmm_LINK_NODE2, c11)), o1);
}

// --- System properties ---

TEST_F(SolverOpenFixture, GetSystemFlowUnits) {
    // Model uses CFS
    EXPECT_EQ(static_cast<int>(swmm_getValue(swmm_FLOWUNITS, 0)), swmm_CFS);
}

// ============================================================
// Fixture: full simulation cycle (open → start → step → end)
// Stats and mass-balance functions are valid after swmm_end()
// but before swmm_close().
// ============================================================

class SolverFullRunFixture : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_EQ(swmm_open(MODEL_INP, API_RPT, API_OUT), 0);
        ASSERT_EQ(swmm_start(/*saveFlag=*/1), 0);
        double elapsed = 1.0;
        while (elapsed > 0.0)
            ASSERT_EQ(swmm_step(&elapsed), 0);
        ASSERT_EQ(swmm_end(), 0);
    }
    void TearDown() override {
        swmm_close();
    }
};

TEST_F(SolverFullRunFixture, NoWarnings) {
    EXPECT_EQ(swmm_getWarnings(), 0);
}

TEST_F(SolverFullRunFixture, MassBalanceErrors_Small) {
    float runoffErr = 0.0f, flowErr = 0.0f, qualErr = 0.0f;
    ASSERT_EQ(swmm_getMassBalErr(&runoffErr, &flowErr, &qualErr), 0);
    EXPECT_LT(std::abs(runoffErr), 10.0f);
    EXPECT_LT(std::abs(flowErr),   10.0f);
}

TEST_F(SolverFullRunFixture, SubcatchStats_FirstSubcatch) {
    swmm_SubcatchStats stats = {};
    ASSERT_EQ(swmm_getSubcatchStats(0, &stats), 0);
    EXPECT_GT(stats.precip,  0.0);
    EXPECT_GE(stats.runoff,  0.0);
    EXPECT_GE(stats.maxFlow, 0.0);
    EXPECT_GE(stats.impervRunoff, 0.0);
}

TEST_F(SolverFullRunFixture, SubcatchStats_AllSubcatches) {
    int n = swmm_getCount(swmm_SUBCATCH);
    ASSERT_EQ(n, 7);
    for (int i = 0; i < n; ++i) {
        swmm_SubcatchStats stats = {};
        EXPECT_EQ(swmm_getSubcatchStats(i, &stats), 0)
            << "getSubcatchStats failed for subcatch " << i;
        EXPECT_GE(stats.precip, 0.0) << "Subcatch " << i << " negative precipitation";
        EXPECT_GE(stats.runoff, 0.0) << "Subcatch " << i << " negative runoff";
    }
}

TEST_F(SolverFullRunFixture, NodeStats_FirstJunction) {
    swmm_NodeStats stats = {};
    ASSERT_EQ(swmm_getNodeStats(0, &stats), 0);
    EXPECT_GE(stats.maxDepth,   0.0);
    EXPECT_GE(stats.maxInflow,  0.0);
    EXPECT_GE(stats.totLatFlow, 0.0);
}

TEST_F(SolverFullRunFixture, NodeStats_AllNodes) {
    int n = swmm_getCount(swmm_NODE);
    ASSERT_EQ(n, 12);
    for (int i = 0; i < n; ++i) {
        swmm_NodeStats stats = {};
        EXPECT_EQ(swmm_getNodeStats(i, &stats), 0)
            << "getNodeStats failed for node " << i;
    }
}

TEST_F(SolverFullRunFixture, LinkStats_FirstConduit) {
    swmm_LinkStats stats = {};
    ASSERT_EQ(swmm_getLinkStats(0, &stats), 0);
    EXPECT_GE(stats.maxFlow,  0.0);
    EXPECT_GE(stats.maxVeloc, 0.0);
    EXPECT_GE(stats.maxDepth, 0.0);
}

TEST_F(SolverFullRunFixture, LinkStats_AllLinks) {
    int n = swmm_getCount(swmm_LINK);
    ASSERT_EQ(n, 11);
    for (int i = 0; i < n; ++i) {
        swmm_LinkStats stats = {};
        EXPECT_EQ(swmm_getLinkStats(i, &stats), 0)
            << "getLinkStats failed for link " << i;
    }
}

TEST_F(SolverFullRunFixture, SystemRoutingTotals) {
    swmm_RoutingTotals totals = {};
    ASSERT_EQ(swmm_getSystemRoutingTotals(&totals), 0);
    EXPECT_GE(totals.outflow, 0.0);
    EXPECT_LT(std::abs(totals.pctError), 10.0);
}

TEST_F(SolverFullRunFixture, SystemRunoffTotals) {
    swmm_RunoffTotals totals = {};
    ASSERT_EQ(swmm_getSystemRunoffTotals(&totals), 0);
    EXPECT_GT(totals.rainfall,      0.0);
    EXPECT_GE(totals.runoff,        0.0);
    EXPECT_GE(totals.infil,         0.0);
    EXPECT_LT(std::abs(totals.pctError), 10.0);
}

// ============================================================
// During-simulation queries (open + started, not yet ended)
// ============================================================

class SolverStepFixture : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_EQ(swmm_open(MODEL_INP, API_RPT, API_OUT), 0);
        ASSERT_EQ(swmm_start(/*saveFlag=*/1), 0);
        // Advance a few steps so dynamic values are populated
        double elapsed = 1.0;
        for (int i = 0; i < 10 && elapsed > 0.0; ++i)
            ASSERT_EQ(swmm_step(&elapsed), 0);
    }
    void TearDown() override {
        double elapsed = 1.0;
        while (elapsed > 0.0)
            swmm_step(&elapsed);
        swmm_end();
        swmm_close();
    }
};

TEST_F(SolverStepFixture, DynamicNodeDepthNonNegative) {
    // After stepping, node depths should be >= 0
    int n = swmm_getCount(swmm_NODE);
    EXPECT_GT(n, 0);
    for (int i = 0; i < n; ++i) {
        double depth = swmm_getValue(swmm_NODE_DEPTH, i);
        EXPECT_GE(depth, 0.0) << "Node " << i << " depth is negative";
    }
}

TEST_F(SolverStepFixture, GetValueByExpandedAPI_NodeElev) {
    // swmm_getValueExpanded should return same elevation as swmm_getValue
    int j1 = swmm_getIndex(swmm_NODE, "J1");
    ASSERT_GE(j1, 0);
    double v = swmm_getValueExpanded(swmm_NODE, swmm_NODE_ELEV, j1, 0, 0);
    EXPECT_NEAR(v, 4973.0, 0.01);
}

TEST_F(SolverStepFixture, GetValueByExpandedAPI_LinkFullDepth) {
    int c3 = swmm_getIndex(swmm_LINK, "C3");
    ASSERT_GE(c3, 0);
    double v = swmm_getValueExpanded(swmm_LINK, swmm_LINK_FULLDEPTH, c3, 0, 0);
    EXPECT_NEAR(v, 2.25, 0.001);
}
