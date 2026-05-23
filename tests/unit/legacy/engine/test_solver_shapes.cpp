/*
 *   test_solver_shapes.cpp
 *
 *   Created: 02/01/2024
 *   Updated: 2026-03-25
 *
 *   Unit tests for SWMM conduit cross-section geometry (Google Test).
 *
 *   Tests verify that the geometry loaded from site_drainage_model.inp matches
 *   the expected values parsed from the [XSECTIONS] and [CONDUITS] sections:
 *
 *     Conduit  Shape        geom1(full depth ft)   Length(ft)
 *     -------  -----------  ----------------------  ---------
 *     C1       TRAPEZOIDAL  3.0                     185
 *     C2       TRAPEZOIDAL  1.0                     526
 *     C3       CIRCULAR     2.25                    109
 *     C4       TRAPEZOIDAL  3.0                     133
 *     C5       TRAPEZOIDAL  3.0                     207
 *     C6       TRAPEZOIDAL  3.0                     140
 *     C7       CIRCULAR     3.5                      95
 *     C8       TRAPEZOIDAL  3.0                     166
 *     C9       TRAPEZOIDAL  3.0                     320
 *     C10      TRAPEZOIDAL  3.0                     145
 *     C11      CIRCULAR     4.75                     89
 */

#include <gtest/gtest.h>

#include <array>
#include <string>

#include "openswmm_solver.h"

#define MODEL_INP "./hotstart/site_drainage_model.inp"
#define SHAP_RPT  "./hotstart/_shap_test.rpt"
#define SHAP_OUT  "./hotstart/_shap_test.out"

// ============================================================
// Fixture: model open so static geometry properties are readable
// ============================================================

class SolverShapeFixture : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_EQ(swmm_open(MODEL_INP, SHAP_RPT, SHAP_OUT), 0);
    }
    void TearDown() override {
        swmm_close();
    }
};

// ============================================================
// Full depth (FULLDEPTH) — diameter for CIRCULAR, max depth for TRAPEZOIDAL
// ============================================================

TEST_F(SolverShapeFixture, CircularFullDepth_C3) {
    int idx = swmm_getIndex(swmm_LINK, "C3");
    ASSERT_GE(idx, 0);
    EXPECT_NEAR(swmm_getValue(swmm_LINK_FULLDEPTH, idx), 2.25, 1e-3);
}

TEST_F(SolverShapeFixture, CircularFullDepth_C7) {
    int idx = swmm_getIndex(swmm_LINK, "C7");
    ASSERT_GE(idx, 0);
    EXPECT_NEAR(swmm_getValue(swmm_LINK_FULLDEPTH, idx), 3.5, 1e-3);
}

TEST_F(SolverShapeFixture, CircularFullDepth_C11) {
    int idx = swmm_getIndex(swmm_LINK, "C11");
    ASSERT_GE(idx, 0);
    EXPECT_NEAR(swmm_getValue(swmm_LINK_FULLDEPTH, idx), 4.75, 1e-3);
}

TEST_F(SolverShapeFixture, TrapezoidalFullDepth_C1) {
    int idx = swmm_getIndex(swmm_LINK, "C1");
    ASSERT_GE(idx, 0);
    EXPECT_NEAR(swmm_getValue(swmm_LINK_FULLDEPTH, idx), 3.0, 1e-3);
}

TEST_F(SolverShapeFixture, TrapezoidalFullDepth_C2) {
    int idx = swmm_getIndex(swmm_LINK, "C2");
    ASSERT_GE(idx, 0);
    EXPECT_NEAR(swmm_getValue(swmm_LINK_FULLDEPTH, idx), 1.0, 1e-3);
}

TEST_F(SolverShapeFixture, TrapezoidalFullDepth_3ft_Batch) {
    // C4, C5, C6, C8, C9, C10 are all TRAPEZOIDAL with depth 3.0 ft
    static const std::array<const char*, 6> names = {
        "C4", "C5", "C6", "C8", "C9", "C10"
    };
    for (const char* name : names) {
        int idx = swmm_getIndex(swmm_LINK, name);
        ASSERT_GE(idx, 0) << "Link " << name << " not found";
        EXPECT_NEAR(swmm_getValue(swmm_LINK_FULLDEPTH, idx), 3.0, 1e-3)
            << "Link " << name << " full depth mismatch";
    }
}

// ============================================================
// Link lengths (from [CONDUITS] section)
// ============================================================

TEST_F(SolverShapeFixture, LinkLengths) {
    struct { const char* name; double len; } expected[] = {
        {"C1", 185.0}, {"C2", 526.0}, {"C3", 109.0},
        {"C4", 133.0}, {"C5", 207.0}, {"C6", 140.0},
        {"C7",  95.0}, {"C8", 166.0}, {"C9", 320.0},
        {"C10",145.0}, {"C11", 89.0}
    };
    for (const auto& e : expected) {
        int idx = swmm_getIndex(swmm_LINK, e.name);
        ASSERT_GE(idx, 0) << "Link " << e.name << " not found";
        EXPECT_NEAR(swmm_getValue(swmm_LINK_LENGTH, idx), e.len, 0.1)
            << "Link " << e.name << " length mismatch";
    }
}

// ============================================================
// All links are conduits in this model
// ============================================================

TEST_F(SolverShapeFixture, AllLinksAreConduits) {
    int n = swmm_getCount(swmm_LINK);
    ASSERT_EQ(n, 11);
    for (int i = 0; i < n; ++i)
        EXPECT_EQ(static_cast<int>(swmm_getValue(swmm_LINK_TYPE, i)), swmm_CONDUIT)
            << "Link " << i << " is not a CONDUIT";
}

// ============================================================
// Link connectivity (upstream / downstream nodes)
// from [CONDUITS]: Name  From  To
// ============================================================

TEST_F(SolverShapeFixture, Connectivity_C1_J1_to_J5) {
    int c1 = swmm_getIndex(swmm_LINK, "C1");
    int j1 = swmm_getIndex(swmm_NODE, "J1");
    int j5 = swmm_getIndex(swmm_NODE, "J5");
    ASSERT_GE(c1, 0); ASSERT_GE(j1, 0); ASSERT_GE(j5, 0);
    EXPECT_EQ(static_cast<int>(swmm_getValue(swmm_LINK_NODE1, c1)), j1);
    EXPECT_EQ(static_cast<int>(swmm_getValue(swmm_LINK_NODE2, c1)), j5);
}

TEST_F(SolverShapeFixture, Connectivity_C3_J3_to_J4) {
    int c3 = swmm_getIndex(swmm_LINK, "C3");
    int j3 = swmm_getIndex(swmm_NODE, "J3");
    int j4 = swmm_getIndex(swmm_NODE, "J4");
    ASSERT_GE(c3, 0); ASSERT_GE(j3, 0); ASSERT_GE(j4, 0);
    EXPECT_EQ(static_cast<int>(swmm_getValue(swmm_LINK_NODE1, c3)), j3);
    EXPECT_EQ(static_cast<int>(swmm_getValue(swmm_LINK_NODE2, c3)), j4);
}

TEST_F(SolverShapeFixture, Connectivity_C6_J7_to_J6) {
    int c6 = swmm_getIndex(swmm_LINK, "C6");
    int j7 = swmm_getIndex(swmm_NODE, "J7");
    int j6 = swmm_getIndex(swmm_NODE, "J6");
    ASSERT_GE(c6, 0); ASSERT_GE(j7, 0); ASSERT_GE(j6, 0);
    EXPECT_EQ(static_cast<int>(swmm_getValue(swmm_LINK_NODE1, c6)), j7);
    EXPECT_EQ(static_cast<int>(swmm_getValue(swmm_LINK_NODE2, c6)), j6);
}

TEST_F(SolverShapeFixture, Connectivity_C11_J11_to_O1) {
    int c11 = swmm_getIndex(swmm_LINK, "C11");
    int j11 = swmm_getIndex(swmm_NODE, "J11");
    int o1  = swmm_getIndex(swmm_NODE, "O1");
    ASSERT_GE(c11, 0); ASSERT_GE(j11, 0); ASSERT_GE(o1, 0);
    EXPECT_EQ(static_cast<int>(swmm_getValue(swmm_LINK_NODE1, c11)), j11);
    EXPECT_EQ(static_cast<int>(swmm_getValue(swmm_LINK_NODE2, c11)), o1);
}

// ============================================================
// Node elevations (from [JUNCTIONS] section)
// ============================================================

TEST_F(SolverShapeFixture, NodeElevations) {
    struct { const char* name; double elev; } expected[] = {
        {"J1",  4973.0}, {"J2",  4969.0}, {"J3",  4973.0},
        {"J4",  4971.0}, {"J5",  4969.8}, {"J6",  4969.0},
        {"J7",  4971.5}, {"J8",  4966.5}, {"J9",  4964.8},
        {"J10", 4963.8}, {"J11", 4963.0}
    };
    for (const auto& e : expected) {
        int idx = swmm_getIndex(swmm_NODE, e.name);
        ASSERT_GE(idx, 0) << "Node " << e.name << " not found";
        EXPECT_NEAR(swmm_getValue(swmm_NODE_ELEV, idx), e.elev, 0.01)
            << "Node " << e.name << " elevation mismatch";
    }
}

// ============================================================
// Node types
// ============================================================

TEST_F(SolverShapeFixture, AllJunctionsAreJunctionType) {
    static const char* junctions[] = {
        "J1","J2","J3","J4","J5","J6","J7","J8","J9","J10","J11"
    };
    for (const char* name : junctions) {
        int idx = swmm_getIndex(swmm_NODE, name);
        ASSERT_GE(idx, 0) << name << " not found";
        EXPECT_EQ(static_cast<int>(swmm_getValue(swmm_NODE_TYPE, idx)), swmm_JUNCTION)
            << name << " should be JUNCTION";
    }
}

TEST_F(SolverShapeFixture, OutfallIsOutfallType) {
    int o1 = swmm_getIndex(swmm_NODE, "O1");
    ASSERT_GE(o1, 0);
    EXPECT_EQ(static_cast<int>(swmm_getValue(swmm_NODE_TYPE, o1)), swmm_OUTFALL);
}
