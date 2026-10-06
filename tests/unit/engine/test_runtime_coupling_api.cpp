// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>
#include <openswmm/engine/openswmm_coupling.h>
#include <openswmm/engine/openswmm_2d.h>
#include <filesystem>
#include <fstream>
#include <limits>

namespace {
class RuntimeCouplingApi : public ::testing::Test {
protected:
    SWMM_Engine engine = nullptr;
    std::filesystem::path directory;
    void SetUp() override {
        directory = std::filesystem::path("runtime_coupling_api") /
            ::testing::UnitTest::GetInstance()->current_test_info()->name();
        std::filesystem::create_directories(directory);
        const auto inp = directory / "model.inp";
        std::ofstream(inp) << R"([OPTIONS]
FLOW_UNITS CMS
FLOW_ROUTING DYNWAVE
START_DATE 01/01/2026
END_DATE 01/01/2026
END_TIME 00:10:00
ROUTING_STEP 5
REPORT_STEP 00:01:00
[POLLUTANTS]
TSS MG/L 0 0 0 0
[JUNCTIONS]
J1 0 3
[OUTFALLS]
O1 -1 FREE
[CONDUITS]
C1 J1 O1 10 0.013 0 0
[XSECTIONS]
C1 CIRCULAR 0.5 0 0 0
[2D_OPTIONS]
MAX_TIMESTEP 5
DRY_DEPTH 0.00000001
[2D_VERTICES]
0 0 0
10 0 0
10 10 0
0 10 0
[2D_TRIANGLES]
0 1 2 0.03
0 2 3 0.03
)";
        engine = swmm_engine_create();
        ASSERT_NE(engine, nullptr);
        ASSERT_EQ(swmm_engine_open(engine, inp.string().c_str(),
            (directory / "model.rpt").string().c_str(),
            (directory / "model.out").string().c_str(), nullptr), SWMM_OK);
        ASSERT_EQ(swmm_engine_initialize(engine), SWMM_OK);
        ASSERT_EQ(swmm_engine_start(engine, 0), SWMM_OK);
    }
    void TearDown() override {
        if (engine) { swmm_engine_close(engine); swmm_engine_destroy(engine); }
    }
    double advance(double seconds) {
        double actual = -1;
        EXPECT_EQ(swmm_coupling_advance_to(engine, seconds, &actual), SWMM_OK);
        EXPECT_DOUBLE_EQ(actual, seconds);
        return actual;
    }
    double volume() {
        double value = -1;
        EXPECT_EQ(swmm_2d_get_total_volume(engine, &value), SWMM_OK);
        return value;
    }
};

TEST_F(RuntimeCouplingApi, ScalarCopiesBuffersAndClearPreservesReceipt) {
    advance(1.375);
    double concentration = 12, rate = 0;
    ASSERT_EQ(swmm_coupling_set_source(engine, "provider", SWMM_COUPLING_SURFACE,
        0, .01, 0, &concentration, &rate, 1, 0), SWMM_OK);
    concentration = 99; // Caller ownership; later mutation cannot change the prescription.
    advance(11.375);
    EXPECT_NEAR(volume(), .1, 1e-12);
    ASSERT_EQ(swmm_coupling_clear_source(engine, SWMM_COUPLING_SURFACE, "provider"), SWMM_OK);
    advance(16.375);
    EXPECT_NEAR(volume(), .1, 1e-12);
    SWMM_CouplingReceipt receipt{};
    double requested = 0, applied = 0;
    ASSERT_EQ(swmm_coupling_get_receipt(engine, 0, "provider", 0, &receipt,
        &requested, &applied, 1), SWMM_OK);
    EXPECT_NEAR(receipt.applied_m3, .1, 1e-12);
    EXPECT_NEAR(applied, 1.2, 1e-12);
}

TEST_F(RuntimeCouplingApi, InvalidBatchLeavesPriorRateAndClockUnchanged) {
    double conc = 4, rate = 0;
    ASSERT_EQ(swmm_coupling_set_source(engine, "provider", 0, 0, .001, 0,
        &conc, &rate, 1, 0), SWMM_OK);
    advance(2.375);
    SWMM_CouplingSource batch[] = {
        {sizeof(SWMM_CouplingSource), "provider", 0, 0, 1, 0, 0, 1, &conc, &rate},
        {sizeof(SWMM_CouplingSource), "other", 0, 99, 1, 0, 0, 1, &conc, &rate}
    };
    ASSERT_EQ(swmm_coupling_set_sources(engine, batch, 2), SWMM_ERR_BADINDEX);
    double seconds = -1;
    ASSERT_EQ(swmm_engine_get_elapsed_seconds(engine, &seconds), SWMM_OK);
    EXPECT_DOUBLE_EQ(seconds, 2.375);
    advance(7.375);
    EXPECT_NEAR(volume(), .007375, 1e-12);
}

TEST_F(RuntimeCouplingApi, ExpiryAndIndependentSpeciesSinkAreConservative) {
    double conc = 0, rate = .02;
    ASSERT_EQ(swmm_coupling_set_source(engine, "load", 0, 0, .01, 0,
        &conc, &rate, 1, 3.125), SWMM_OK);
    advance(10.125);
    EXPECT_NEAR(volume(), .03125, 1e-12);
    rate = -100;
    ASSERT_EQ(swmm_coupling_set_source(engine, "remove", 0, 0, 0, 0,
        &conc, &rate, 1, 0), SWMM_OK);
    advance(20.125);
    SWMM_CouplingReceipt receipt{};
    double requested = 0, applied = 0;
    ASSERT_EQ(swmm_coupling_get_receipt(engine, 0, "remove", 0, &receipt,
        &requested, &applied, 1), SWMM_OK);
    EXPECT_NEAR(requested, -1000, 1e-9);
    EXPECT_NEAR(applied, -.0625, 1e-12);
    EXPECT_EQ(receipt.applied_m3, 0);
}

TEST_F(RuntimeCouplingApi, DirectBoundaryUsesTotalFlowAndRejectsDisabledHeat) {
    double conc = 7, rate = 0;
    ASSERT_EQ(swmm_2d_set_runtime_boundary(engine, 0, 0, 2, -.01, &conc, 1), SWMM_OK);
    advance(10.375);
    EXPECT_NEAR(volume(), .10375, 1e-12);
    ASSERT_EQ(swmm_2d_clear_edge_bc(engine, 0, 0), SWMM_OK);
    ASSERT_EQ(swmm_coupling_set_source(engine, "heat", 0, 0, 0, 100,
        &conc, &rate, 1, 0), SWMM_ERR_BADPARAM);
    advance(20.375);
    EXPECT_NEAR(volume(), .10375, 1e-12);
    unsigned flags = 0;
    ASSERT_EQ(swmm_coupling_capabilities(engine, &flags), SWMM_OK);
    EXPECT_EQ(flags, 1u | 8u | 64u | 128u | 512u);
}
} // namespace
