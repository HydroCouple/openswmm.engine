// SPDX-License-Identifier: Apache-2.0
//
// Copyright 2026 Caleb Buahin
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/**
 * @file test_link_flow_roundtrip.cpp
 * @brief Unit tests for engine gaps BN-LINK-01a / -01b — symmetric
 *        getters for the existing @ref swmm_link_set_initial_flow and
 *        @ref swmm_link_set_max_flow setters.
 *
 * @details The setters write to `ctx.links.flow[idx]` and
 *          `ctx.links.q_limit[idx]` respectively. The new getters read
 *          from those same SoA slots; tests assert bit-for-bit
 *          round-trip plus 0.0-default and negative-flow round-trip.
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @license  Apache-2.0
 */

#include <gtest/gtest.h>
#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_links.h>
#include <openswmm/engine/openswmm_nodes.h>

namespace {

class LinkFlowRoundTripTest : public ::testing::Test {
protected:
    SWMM_Engine engine_ = nullptr;
    int li_ = -1;

    void SetUp() override {
        engine_ = swmm_engine_new();
        ASSERT_NE(engine_, nullptr);
        ASSERT_EQ(swmm_node_add(engine_, "J1", SWMM_NODE_JUNCTION), SWMM_OK);
        ASSERT_EQ(swmm_node_add(engine_, "J2", SWMM_NODE_JUNCTION), SWMM_OK);
        ASSERT_EQ(swmm_link_add(engine_, "C1", /*Conduit=*/0), SWMM_OK);
        li_ = swmm_link_index(engine_, "C1");
        ASSERT_GE(li_, 0);
        const int j1 = swmm_node_index(engine_, "J1");
        const int j2 = swmm_node_index(engine_, "J2");
        ASSERT_EQ(swmm_link_set_nodes(engine_, li_, j1, j2), SWMM_OK);
    }

    void TearDown() override {
        if (engine_) swmm_engine_destroy(engine_);
        engine_ = nullptr;
    }
};

TEST_F(LinkFlowRoundTripTest, InitialFlowDefaultsToZero) {
    double v = -1.0;
    EXPECT_EQ(swmm_link_get_initial_flow(engine_, li_, &v), SWMM_OK);
    EXPECT_DOUBLE_EQ(v, 0.0);
}

TEST_F(LinkFlowRoundTripTest, InitialFlowSetReadBack) {
    EXPECT_EQ(swmm_link_set_initial_flow(engine_, li_, 12.5), SWMM_OK);
    double v = 0.0;
    EXPECT_EQ(swmm_link_get_initial_flow(engine_, li_, &v), SWMM_OK);
    EXPECT_DOUBLE_EQ(v, 12.5);
}

TEST_F(LinkFlowRoundTripTest, InitialFlowNegativeRoundTrips) {
    // Reverse-flow initial condition is physically meaningful; the SoA
    // slot is a plain double with no sign filter. Lock in the contract.
    EXPECT_EQ(swmm_link_set_initial_flow(engine_, li_, -3.25), SWMM_OK);
    double v = 0.0;
    EXPECT_EQ(swmm_link_get_initial_flow(engine_, li_, &v), SWMM_OK);
    EXPECT_DOUBLE_EQ(v, -3.25);
}

TEST_F(LinkFlowRoundTripTest, MaxFlowDefaultsToZero) {
    // 0.0 == "no limit" per the setter docstring.
    double v = -1.0;
    EXPECT_EQ(swmm_link_get_max_flow(engine_, li_, &v), SWMM_OK);
    EXPECT_DOUBLE_EQ(v, 0.0);
}

TEST_F(LinkFlowRoundTripTest, MaxFlowSetReadBack) {
    EXPECT_EQ(swmm_link_set_max_flow(engine_, li_, 200.0), SWMM_OK);
    double v = 0.0;
    EXPECT_EQ(swmm_link_get_max_flow(engine_, li_, &v), SWMM_OK);
    EXPECT_DOUBLE_EQ(v, 200.0);
}

TEST_F(LinkFlowRoundTripTest, BadIndexReturnsError) {
    double v = 0.0;
    EXPECT_NE(swmm_link_get_initial_flow(engine_, -1, &v), SWMM_OK);
    EXPECT_NE(swmm_link_get_initial_flow(engine_, 9999, &v), SWMM_OK);
    EXPECT_NE(swmm_link_get_max_flow(engine_, -1, &v), SWMM_OK);
    EXPECT_NE(swmm_link_get_max_flow(engine_, 9999, &v), SWMM_OK);
}

TEST_F(LinkFlowRoundTripTest, NullOutPointerAccepted) {
    // Existing scalar getters in the engine accept null out-pointers
    // (early-return after the index check). Lock in the same contract.
    EXPECT_EQ(swmm_link_get_initial_flow(engine_, li_, nullptr), SWMM_OK);
    EXPECT_EQ(swmm_link_get_max_flow    (engine_, li_, nullptr), SWMM_OK);
}

// ============================================================================
// Engine gap BN-LINK-02 — orifice type (SIDE / BOTTOM) accessor pair.
// Separate fixture because we need an ORIFICE link, not a CONDUIT.
// ============================================================================

class LinkOrificeTypeTest : public ::testing::Test {
protected:
    SWMM_Engine engine_ = nullptr;
    int orifIdx_  = -1;
    int condIdx_  = -1;

    void SetUp() override {
        engine_ = swmm_engine_new();
        ASSERT_NE(engine_, nullptr);
        ASSERT_EQ(swmm_node_add(engine_, "J1", SWMM_NODE_JUNCTION), SWMM_OK);
        ASSERT_EQ(swmm_node_add(engine_, "J2", SWMM_NODE_JUNCTION), SWMM_OK);
        ASSERT_EQ(swmm_link_add(engine_, "O1", SWMM_LINK_ORIFICE), SWMM_OK);
        ASSERT_EQ(swmm_link_add(engine_, "C1", SWMM_LINK_CONDUIT), SWMM_OK);
        orifIdx_ = swmm_link_index(engine_, "O1");
        condIdx_ = swmm_link_index(engine_, "C1");
        ASSERT_GE(orifIdx_, 0);
        ASSERT_GE(condIdx_, 0);
    }
    void TearDown() override {
        if (engine_) swmm_engine_destroy(engine_);
        engine_ = nullptr;
    }
};

TEST_F(LinkOrificeTypeTest, DefaultsToSide) {
    // links.param1 is zero-initialized on link_add, which means SIDE per
    // the legacy convention (LinkData.hpp:379 + LinksHandler.cpp:138).
    // Hmm — actually param1=0 means BOTTOM by the legacy encoding, and
    // the new accessor maps engine 0.0=BOTTOM → GUI 1=BOTTOM. So the
    // default is BOTTOM, not SIDE. Pin the contract.
    int t = -1;
    EXPECT_EQ(swmm_link_get_orifice_type(engine_, orifIdx_, &t), SWMM_OK);
    EXPECT_EQ(t, SWMM_ORIFICE_BOTTOM);
}

TEST_F(LinkOrificeTypeTest, SetSideReadBack) {
    EXPECT_EQ(swmm_link_set_orifice_type(engine_, orifIdx_, SWMM_ORIFICE_SIDE),
              SWMM_OK);
    int t = -1;
    EXPECT_EQ(swmm_link_get_orifice_type(engine_, orifIdx_, &t), SWMM_OK);
    EXPECT_EQ(t, SWMM_ORIFICE_SIDE);
}

TEST_F(LinkOrificeTypeTest, SetBottomReadBack) {
    EXPECT_EQ(swmm_link_set_orifice_type(engine_, orifIdx_, SWMM_ORIFICE_SIDE),
              SWMM_OK);     // flip to SIDE first to confirm we toggle off it
    EXPECT_EQ(swmm_link_set_orifice_type(engine_, orifIdx_, SWMM_ORIFICE_BOTTOM),
              SWMM_OK);
    int t = -1;
    EXPECT_EQ(swmm_link_get_orifice_type(engine_, orifIdx_, &t), SWMM_OK);
    EXPECT_EQ(t, SWMM_ORIFICE_BOTTOM);
}

TEST_F(LinkOrificeTypeTest, RejectsNonOrificeLink) {
    // Conduit's param1 means something different — refuse the write to
    // protect that slot.
    int t = 0;
    EXPECT_NE(swmm_link_set_orifice_type(engine_, condIdx_, SWMM_ORIFICE_SIDE),
              SWMM_OK);
    EXPECT_NE(swmm_link_get_orifice_type(engine_, condIdx_, &t), SWMM_OK);
}

TEST_F(LinkOrificeTypeTest, RejectsOutOfRangeType) {
    EXPECT_NE(swmm_link_set_orifice_type(engine_, orifIdx_, -1),  SWMM_OK);
    EXPECT_NE(swmm_link_set_orifice_type(engine_, orifIdx_,  2),  SWMM_OK);
    EXPECT_NE(swmm_link_set_orifice_type(engine_, orifIdx_, 99),  SWMM_OK);
}

// ============================================================================
// Engine gap BN-LINK-03 — weir TYPE (5 values) accessor pair.
// ============================================================================

class LinkWeirTypeTest : public ::testing::Test {
protected:
    SWMM_Engine engine_ = nullptr;
    int weirIdx_   = -1;
    int condIdx_   = -1;

    void SetUp() override {
        engine_ = swmm_engine_new();
        ASSERT_NE(engine_, nullptr);
        ASSERT_EQ(swmm_node_add(engine_, "J1", SWMM_NODE_JUNCTION), SWMM_OK);
        ASSERT_EQ(swmm_node_add(engine_, "J2", SWMM_NODE_JUNCTION), SWMM_OK);
        ASSERT_EQ(swmm_link_add(engine_, "W1", SWMM_LINK_WEIR),    SWMM_OK);
        ASSERT_EQ(swmm_link_add(engine_, "C1", SWMM_LINK_CONDUIT), SWMM_OK);
        weirIdx_ = swmm_link_index(engine_, "W1");
        condIdx_ = swmm_link_index(engine_, "C1");
        ASSERT_GE(weirIdx_, 0);
        ASSERT_GE(condIdx_, 0);
    }
    void TearDown() override {
        if (engine_) swmm_engine_destroy(engine_);
        engine_ = nullptr;
    }
};

TEST_F(LinkWeirTypeTest, DefaultsToTransverse) {
    // links.param1 starts at 0.0 = TRANSVERSE_WEIR per
    // legacy/engine/enums.h:925 enum ordering.
    int t = -1;
    EXPECT_EQ(swmm_link_get_weir_type(engine_, weirIdx_, &t), SWMM_OK);
    EXPECT_EQ(t, SWMM_WEIR_TRANSVERSE);
}

TEST_F(LinkWeirTypeTest, AllFiveValuesRoundTrip) {
    for (int v : { SWMM_WEIR_TRANSVERSE, SWMM_WEIR_SIDEFLOW,
                    SWMM_WEIR_VNOTCH, SWMM_WEIR_TRAPEZOIDAL,
                    SWMM_WEIR_ROADWAY }) {
        EXPECT_EQ(swmm_link_set_weir_type(engine_, weirIdx_, v), SWMM_OK)
            << "set " << v << " failed";
        int t = -1;
        EXPECT_EQ(swmm_link_get_weir_type(engine_, weirIdx_, &t), SWMM_OK);
        EXPECT_EQ(t, v) << "read-back mismatch for " << v;
    }
}

TEST_F(LinkWeirTypeTest, RejectsNonWeirLink) {
    int t = 0;
    EXPECT_NE(swmm_link_set_weir_type(engine_, condIdx_, SWMM_WEIR_TRANSVERSE),
              SWMM_OK);
    EXPECT_NE(swmm_link_get_weir_type(engine_, condIdx_, &t), SWMM_OK);
}

TEST_F(LinkWeirTypeTest, RejectsOutOfRangeType) {
    EXPECT_NE(swmm_link_set_weir_type(engine_, weirIdx_, -1), SWMM_OK);
    EXPECT_NE(swmm_link_set_weir_type(engine_, weirIdx_,  5), SWMM_OK);
    EXPECT_NE(swmm_link_set_weir_type(engine_, weirIdx_, 99), SWMM_OK);
}

// ============================================================================
// Engine gap BN-LINK-04 — outlet rating type (4 values) + exponent.
// ============================================================================

class LinkOutletRatingTest : public ::testing::Test {
protected:
    SWMM_Engine engine_ = nullptr;
    int outletIdx_ = -1;
    int condIdx_   = -1;

    void SetUp() override {
        engine_ = swmm_engine_new();
        ASSERT_NE(engine_, nullptr);
        ASSERT_EQ(swmm_node_add(engine_, "J1", SWMM_NODE_JUNCTION), SWMM_OK);
        ASSERT_EQ(swmm_node_add(engine_, "J2", SWMM_NODE_JUNCTION), SWMM_OK);
        ASSERT_EQ(swmm_link_add(engine_, "OT1", SWMM_LINK_OUTLET),  SWMM_OK);
        ASSERT_EQ(swmm_link_add(engine_, "C1",  SWMM_LINK_CONDUIT), SWMM_OK);
        outletIdx_ = swmm_link_index(engine_, "OT1");
        condIdx_   = swmm_link_index(engine_, "C1");
        ASSERT_GE(outletIdx_, 0);
        ASSERT_GE(condIdx_,   0);
    }
    void TearDown() override {
        if (engine_) swmm_engine_destroy(engine_);
        engine_ = nullptr;
    }
};

TEST_F(LinkOutletRatingTest, DefaultsToFunctionalHead) {
    int t = -1;
    EXPECT_EQ(swmm_link_get_outlet_rating_type(engine_, outletIdx_, &t), SWMM_OK);
    EXPECT_EQ(t, SWMM_OUTLET_FUNCTIONAL_HEAD);
}

TEST_F(LinkOutletRatingTest, AllFourTypesRoundTrip) {
    for (int v : { SWMM_OUTLET_FUNCTIONAL_HEAD, SWMM_OUTLET_FUNCTIONAL_DEPTH,
                    SWMM_OUTLET_TABULAR_HEAD,    SWMM_OUTLET_TABULAR_DEPTH }) {
        EXPECT_EQ(swmm_link_set_outlet_rating_type(engine_, outletIdx_, v),
                  SWMM_OK)
            << "set " << v << " failed";
        int t = -1;
        EXPECT_EQ(swmm_link_get_outlet_rating_type(engine_, outletIdx_, &t),
                  SWMM_OK);
        EXPECT_EQ(t, v);
    }
}

TEST_F(LinkOutletRatingTest, ExponRoundTrip) {
    EXPECT_EQ(swmm_link_set_outlet_expon(engine_, outletIdx_, 0.5), SWMM_OK);
    double v = 0.0;
    EXPECT_EQ(swmm_link_get_outlet_expon(engine_, outletIdx_, &v), SWMM_OK);
    EXPECT_DOUBLE_EQ(v, 0.5);

    EXPECT_EQ(swmm_link_set_outlet_expon(engine_, outletIdx_, 1.75), SWMM_OK);
    EXPECT_EQ(swmm_link_get_outlet_expon(engine_, outletIdx_, &v), SWMM_OK);
    EXPECT_DOUBLE_EQ(v, 1.75);
}

TEST_F(LinkOutletRatingTest, RejectsNonOutletLink) {
    int t = 0;
    double e = 0.0;
    EXPECT_NE(swmm_link_set_outlet_rating_type(engine_, condIdx_,
                                                 SWMM_OUTLET_FUNCTIONAL_HEAD),
              SWMM_OK);
    EXPECT_NE(swmm_link_get_outlet_rating_type(engine_, condIdx_, &t), SWMM_OK);
    EXPECT_NE(swmm_link_set_outlet_expon(engine_, condIdx_, 0.5), SWMM_OK);
    EXPECT_NE(swmm_link_get_outlet_expon(engine_, condIdx_, &e), SWMM_OK);
}

TEST_F(LinkOutletRatingTest, RejectsOutOfRangeType) {
    EXPECT_NE(swmm_link_set_outlet_rating_type(engine_, outletIdx_, -1), SWMM_OK);
    EXPECT_NE(swmm_link_set_outlet_rating_type(engine_, outletIdx_,  4), SWMM_OK);
    EXPECT_NE(swmm_link_set_outlet_rating_type(engine_, outletIdx_, 99), SWMM_OK);
}

// ============================================================================
// Engine gap BN-LINK-05 — pump startup / shutoff depth.
// Engine gap BN-LINK-06 — orifice open/close rate.
// ============================================================================

class LinkPumpOrificeMisc : public ::testing::Test {
protected:
    SWMM_Engine engine_ = nullptr;
    int pumpIdx_   = -1;
    int orifIdx_   = -1;
    int condIdx_   = -1;

    void SetUp() override {
        engine_ = swmm_engine_new();
        ASSERT_NE(engine_, nullptr);
        ASSERT_EQ(swmm_node_add(engine_, "J1", SWMM_NODE_JUNCTION), SWMM_OK);
        ASSERT_EQ(swmm_node_add(engine_, "J2", SWMM_NODE_JUNCTION), SWMM_OK);
        ASSERT_EQ(swmm_link_add(engine_, "P1",  SWMM_LINK_PUMP),    SWMM_OK);
        ASSERT_EQ(swmm_link_add(engine_, "OR1", SWMM_LINK_ORIFICE), SWMM_OK);
        ASSERT_EQ(swmm_link_add(engine_, "C1",  SWMM_LINK_CONDUIT), SWMM_OK);
        pumpIdx_ = swmm_link_index(engine_, "P1");
        orifIdx_ = swmm_link_index(engine_, "OR1");
        condIdx_ = swmm_link_index(engine_, "C1");
        ASSERT_GE(pumpIdx_, 0);
        ASSERT_GE(orifIdx_, 0);
        ASSERT_GE(condIdx_, 0);
    }
    void TearDown() override {
        if (engine_) swmm_engine_destroy(engine_);
        engine_ = nullptr;
    }
};

TEST_F(LinkPumpOrificeMisc, PumpStartupShutoffDefaultsZero) {
    double s = -1.0, t = -1.0;
    EXPECT_EQ(swmm_link_get_pump_startup_depth(engine_, pumpIdx_, &s), SWMM_OK);
    EXPECT_EQ(swmm_link_get_pump_shutoff_depth(engine_, pumpIdx_, &t), SWMM_OK);
    EXPECT_DOUBLE_EQ(s, 0.0);
    EXPECT_DOUBLE_EQ(t, 0.0);
}

TEST_F(LinkPumpOrificeMisc, PumpDepthsRoundTrip) {
    EXPECT_EQ(swmm_link_set_pump_startup_depth(engine_, pumpIdx_, 2.5), SWMM_OK);
    EXPECT_EQ(swmm_link_set_pump_shutoff_depth(engine_, pumpIdx_, 0.5), SWMM_OK);
    double s = 0, t = 0;
    EXPECT_EQ(swmm_link_get_pump_startup_depth(engine_, pumpIdx_, &s), SWMM_OK);
    EXPECT_EQ(swmm_link_get_pump_shutoff_depth(engine_, pumpIdx_, &t), SWMM_OK);
    EXPECT_DOUBLE_EQ(s, 2.5);
    EXPECT_DOUBLE_EQ(t, 0.5);
}

TEST_F(LinkPumpOrificeMisc, PumpDepthsRejectNonPump) {
    double v = 0;
    EXPECT_NE(swmm_link_set_pump_startup_depth(engine_, condIdx_, 1.0), SWMM_OK);
    EXPECT_NE(swmm_link_get_pump_startup_depth(engine_, condIdx_, &v),  SWMM_OK);
    EXPECT_NE(swmm_link_set_pump_shutoff_depth(engine_, condIdx_, 1.0), SWMM_OK);
    EXPECT_NE(swmm_link_get_pump_shutoff_depth(engine_, condIdx_, &v),  SWMM_OK);
}

TEST_F(LinkPumpOrificeMisc, OrificeRateDefaultZero) {
    double r = -1.0;
    EXPECT_EQ(swmm_link_get_orifice_open_close_rate(engine_, orifIdx_, &r),
              SWMM_OK);
    EXPECT_DOUBLE_EQ(r, 0.0);
}

TEST_F(LinkPumpOrificeMisc, OrificeRateRoundTrip) {
    EXPECT_EQ(swmm_link_set_orifice_open_close_rate(engine_, orifIdx_, 0.05),
              SWMM_OK);
    double r = 0;
    EXPECT_EQ(swmm_link_get_orifice_open_close_rate(engine_, orifIdx_, &r),
              SWMM_OK);
    EXPECT_DOUBLE_EQ(r, 0.05);
}

TEST_F(LinkPumpOrificeMisc, OrificeRateRejectsNonOrifice) {
    double v = 0;
    EXPECT_NE(swmm_link_set_orifice_open_close_rate(engine_, pumpIdx_, 0.1),
              SWMM_OK);
    EXPECT_NE(swmm_link_get_orifice_open_close_rate(engine_, pumpIdx_, &v),
              SWMM_OK);
    EXPECT_NE(swmm_link_set_orifice_open_close_rate(engine_, condIdx_, 0.1),
              SWMM_OK);
    EXPECT_NE(swmm_link_get_orifice_open_close_rate(engine_, condIdx_, &v),
              SWMM_OK);
}

} // namespace
