/**
 * @file test_massbalance.cpp
 * @brief Deterministic continuity-fixture tests for the public mass-balance API.
 *
 * @details These tests exercise the C API accessors against a fresh engine
 *          context populated with known bookkeeping totals. They provide a
 *          closed-system verification surface for runoff, routing, and quality
 *          continuity without depending on a full simulation.
 *
 * @see include/openswmm/engine/openswmm_massbalance.h
 * @see src/engine/core/openswmm_massbalance_impl.cpp
 */

#include <gtest/gtest.h>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_massbalance.h>

#include "core/SWMMEngine.hpp"

namespace {

static openswmm::SWMMEngine& as_cpp_engine(SWMM_Engine engine) {
    return *static_cast<openswmm::SWMMEngine*>(engine);
}

class MassBalanceApiTest : public ::testing::Test {
protected:
    void SetUp() override {
        engine_ = swmm_engine_create();
        ASSERT_NE(engine_, nullptr);
    }

    void TearDown() override {
        if (engine_ != nullptr) {
            swmm_engine_destroy(engine_);
            engine_ = nullptr;
        }
    }

    openswmm::SimulationContext::MassBalance& mb() {
        return as_cpp_engine(engine_).context().mass_balance;
    }

    SWMM_Engine engine_ = nullptr;
};

}  // namespace

TEST_F(MassBalanceApiTest, RunoffClosedSystemReportsZeroError) {
    auto& mass_balance = mb();
    mass_balance.runoff_rainfall = 100.0;
    mass_balance.runoff_init_store = 20.0;
    mass_balance.runoff_evap = 10.0;
    mass_balance.runoff_infil = 15.0;
    mass_balance.runoff_runoff = 70.0;
    mass_balance.runoff_final_store = 25.0;

    double error = -1.0;
    ASSERT_EQ(swmm_get_runoff_continuity_error(engine_, &error), SWMM_OK);
    EXPECT_NEAR(error, 0.0, 1e-12);
}

TEST_F(MassBalanceApiTest, RoutingClosedSystemReportsZeroError) {
    auto& mass_balance = mb();
    mass_balance.routing_dry_weather = 20.0;
    mass_balance.routing_wet_weather = 30.0;
    mass_balance.routing_gw_inflow = 10.0;
    mass_balance.routing_rdii = 5.0;
    mass_balance.routing_external = 15.0;
    mass_balance.routing_init_storage = 20.0;

    mass_balance.routing_flooding = 8.0;
    mass_balance.routing_outflow = 60.0;
    mass_balance.routing_evap_loss = 4.0;
    mass_balance.routing_seep_loss = 3.0;
    mass_balance.routing_final_storage = 25.0;

    double error = -1.0;
    ASSERT_EQ(swmm_get_routing_continuity_error(engine_, &error), SWMM_OK);
    EXPECT_NEAR(error, 0.0, 1e-12);
}

TEST_F(MassBalanceApiTest, QualityClosedSystemReportsZeroError) {
    auto& mass_balance = mb();
    mass_balance.resize_quality(1);
    mass_balance.qual_wet_deposition[0] = 10.0;
    mass_balance.qual_runoff_load[0] = 30.0;
    mass_balance.qual_routing_wet[0] = 40.0;
    mass_balance.qual_routing_ii_in[0] = 20.0;
    mass_balance.qual_routing_init[0] = 5.0;

    mass_balance.qual_routing_outflow[0] = 70.0;
    mass_balance.qual_routing_flood[0] = 20.0;
    mass_balance.qual_routing_reacted[0] = 10.0;
    mass_balance.qual_routing_final[0] = 5.0;

    double error = -1.0;
    ASSERT_EQ(swmm_get_quality_continuity_error(engine_, 0, &error), SWMM_OK);
    EXPECT_NEAR(error, 0.0, 1e-12);
}

TEST_F(MassBalanceApiTest, RunoffKnownImbalanceMatchesExpected) {
    auto& mass_balance = mb();
    mass_balance.runoff_rainfall = 100.0;
    mass_balance.runoff_init_store = 20.0;
    mass_balance.runoff_evap = 10.0;
    mass_balance.runoff_infil = 20.0;
    mass_balance.runoff_runoff = 70.0;
    mass_balance.runoff_final_store = 15.0;

    const double expected = 5.0 / 120.0;
    double error = -1.0;
    ASSERT_EQ(swmm_get_runoff_continuity_error(engine_, &error), SWMM_OK);
    EXPECT_NEAR(error, expected, 1e-12);
}

TEST_F(MassBalanceApiTest, RoutingKnownImbalanceMatchesExpected) {
    auto& mass_balance = mb();
    mass_balance.routing_dry_weather = 20.0;
    mass_balance.routing_wet_weather = 30.0;
    mass_balance.routing_gw_inflow = 10.0;
    mass_balance.routing_rdii = 5.0;
    mass_balance.routing_external = 15.0;
    mass_balance.routing_init_storage = 20.0;

    mass_balance.routing_flooding = 8.0;
    mass_balance.routing_outflow = 60.0;
    mass_balance.routing_evap_loss = 4.0;
    mass_balance.routing_seep_loss = 3.0;
    mass_balance.routing_final_storage = 20.0;

    const double expected = 5.0 / 100.0;
    double error = -1.0;
    ASSERT_EQ(swmm_get_routing_continuity_error(engine_, &error), SWMM_OK);
    EXPECT_NEAR(error, expected, 1e-12);
}

TEST_F(MassBalanceApiTest, QualityKnownImbalanceMatchesExpected) {
    auto& mass_balance = mb();
    mass_balance.resize_quality(1);
    mass_balance.qual_wet_deposition[0] = 10.0;
    mass_balance.qual_runoff_load[0] = 30.0;
    mass_balance.qual_routing_wet[0] = 40.0;
    mass_balance.qual_routing_ii_in[0] = 20.0;
    mass_balance.qual_routing_init[0] = 5.0;

    mass_balance.qual_routing_outflow[0] = 68.0;
    mass_balance.qual_routing_flood[0] = 20.0;
    mass_balance.qual_routing_reacted[0] = 10.0;
    mass_balance.qual_routing_final[0] = 5.0;

    const double expected = 2.0 / 100.0;
    double error = -1.0;
    ASSERT_EQ(swmm_get_quality_continuity_error(engine_, 0, &error), SWMM_OK);
    EXPECT_NEAR(error, expected, 1e-12);
}

TEST_F(MassBalanceApiTest, QualityZeroFluxStillReportsZero) {
    auto& mass_balance = mb();
    mass_balance.resize_quality(2);

    double error = -1.0;
    ASSERT_EQ(swmm_get_quality_continuity_error(engine_, 1, &error), SWMM_OK);
    EXPECT_NEAR(error, 0.0, 1e-12);
}