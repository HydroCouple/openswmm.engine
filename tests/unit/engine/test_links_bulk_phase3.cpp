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
 * @file test_links_bulk_phase3.cpp
 * @brief Unit tests for the Phase 3 link-bulk C API additions —
 *        @ref swmm_link_get_velocities_bulk, @ref swmm_link_get_capacities_bulk,
 *        @ref swmm_link_get_volumes_bulk,
 *        @ref swmm_link_get_control_settings_bulk,
 *        @ref swmm_link_get_target_settings_bulk,
 *        @ref swmm_link_get_hyd_powers_bulk, and
 *        @ref swmm_link_get_ids_bulk.
 *
 * @details Same contract as the Nodes Phase 3 tests
 *          (@c test_nodes_bulk_phase3.cpp): per-link parity with the scalar
 *          accessors, stride-packed ID round-trip, and bad-param contracts.
 *          Velocities, capacities, and hyd_powers are *derived* values whose
 *          bulk variants do a per-link loop in C — the equivalence tests
 *          assert bit-for-bit equality with the scalar getters because both
 *          use the same arithmetic.
 *
 *          Working directory is set to tests/unit/engine/data/ by the
 *          test CMakeLists.txt.
 *
 * @see docs/C_API_BINDINGS_MCP_IMPROVEMENT_PLAN.md Phase 3 — Links batch
 * @ingroup engine_tests
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include <gtest/gtest.h>
#include <cstring>
#include <string>
#include <vector>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_links.h>

namespace {

class LinksBulkPhase3Test : public ::testing::Test {
protected:
    SWMM_Engine engine_ = nullptr;
    int n_links_ = 0;

    void SetUp() override {
        engine_ = swmm_engine_create();
        ASSERT_NE(engine_, nullptr);
        ASSERT_EQ(swmm_engine_open(engine_,
                                    "site_drainage_model.inp",
                                    "site_drainage_model.rpt",
                                    "site_drainage_model.out",
                                    nullptr),
                  SWMM_OK)
            << "open failed: " << swmm_get_last_error_msg(engine_);
        ASSERT_EQ(swmm_engine_initialize(engine_), SWMM_OK);
        n_links_ = swmm_link_count(engine_);
        ASSERT_GT(n_links_, 0);
    }

    void TearDown() override {
        if (engine_) {
            swmm_engine_close(engine_);
            swmm_engine_destroy(engine_);
            engine_ = nullptr;
        }
    }
};

// ---------------------------------------------------------------------------
// Equivalence — each bulk getter must match the scalar accessor for every
// link, including the derived (per-link computed) ones.
// ---------------------------------------------------------------------------

TEST_F(LinksBulkPhase3Test, VelocitiesBulkMatchesScalarPerLink) {
    std::vector<double> bulk(n_links_, -42.0);
    ASSERT_EQ(swmm_link_get_velocities_bulk(engine_, bulk.data(), n_links_),
              SWMM_OK);
    for (int i = 0; i < n_links_; ++i) {
        double scalar = -1.0;
        ASSERT_EQ(swmm_link_get_velocity(engine_, i, &scalar), SWMM_OK);
        EXPECT_EQ(bulk[i], scalar) << "link " << i;
    }
}

TEST_F(LinksBulkPhase3Test, CapacitiesBulkMatchesScalarPerLink) {
    std::vector<double> bulk(n_links_, -42.0);
    ASSERT_EQ(swmm_link_get_capacities_bulk(engine_, bulk.data(), n_links_),
              SWMM_OK);
    for (int i = 0; i < n_links_; ++i) {
        double scalar = -1.0;
        ASSERT_EQ(swmm_link_get_capacity(engine_, i, &scalar), SWMM_OK);
        EXPECT_EQ(bulk[i], scalar) << "link " << i;
    }
}

TEST_F(LinksBulkPhase3Test, VolumesBulkMatchesScalarPerLink) {
    std::vector<double> bulk(n_links_, -42.0);
    ASSERT_EQ(swmm_link_get_volumes_bulk(engine_, bulk.data(), n_links_),
              SWMM_OK);
    for (int i = 0; i < n_links_; ++i) {
        double scalar = -1.0;
        ASSERT_EQ(swmm_link_get_volume(engine_, i, &scalar), SWMM_OK);
        EXPECT_EQ(bulk[i], scalar) << "link " << i;
    }
}

TEST_F(LinksBulkPhase3Test, ControlSettingsBulkMatchesScalarPerLink) {
    std::vector<double> bulk(n_links_, -42.0);
    ASSERT_EQ(swmm_link_get_control_settings_bulk(engine_, bulk.data(),
                                                   n_links_),
              SWMM_OK);
    for (int i = 0; i < n_links_; ++i) {
        double scalar = -1.0;
        ASSERT_EQ(swmm_link_get_control_setting(engine_, i, &scalar), SWMM_OK);
        EXPECT_EQ(bulk[i], scalar) << "link " << i;
    }
}

TEST_F(LinksBulkPhase3Test, TargetSettingsBulkMatchesScalarPerLink) {
    std::vector<double> bulk(n_links_, -42.0);
    ASSERT_EQ(swmm_link_get_target_settings_bulk(engine_, bulk.data(),
                                                   n_links_),
              SWMM_OK);
    for (int i = 0; i < n_links_; ++i) {
        double scalar = -1.0;
        ASSERT_EQ(swmm_link_get_target_setting(engine_, i, &scalar), SWMM_OK);
        EXPECT_EQ(bulk[i], scalar) << "link " << i;
    }
}

TEST_F(LinksBulkPhase3Test, HydPowersBulkMatchesScalarPerLink) {
    std::vector<double> bulk(n_links_, -42.0);
    ASSERT_EQ(swmm_link_get_hyd_powers_bulk(engine_, bulk.data(), n_links_),
              SWMM_OK);
    for (int i = 0; i < n_links_; ++i) {
        double scalar = -1.0;
        ASSERT_EQ(swmm_link_get_hyd_power(engine_, i, &scalar), SWMM_OK);
        EXPECT_EQ(bulk[i], scalar) << "link " << i;
    }
}

// ---------------------------------------------------------------------------
// IDs bulk — stride-packed UTF-8 format, same contract as the Nodes variant.
// ---------------------------------------------------------------------------

TEST_F(LinksBulkPhase3Test, IdsBulkMatchesScalarPerLink) {
    constexpr int kStride = 64;
    std::vector<char> buf(static_cast<std::size_t>(n_links_) * kStride, '\xAA');
    ASSERT_EQ(swmm_link_get_ids_bulk(engine_, buf.data(), kStride, n_links_),
              SWMM_OK);
    for (int i = 0; i < n_links_; ++i) {
        const char* bulk_id = buf.data() + i * kStride;
        const char* scalar_id = swmm_link_id(engine_, i);
        ASSERT_NE(scalar_id, nullptr) << "link " << i;
        EXPECT_STREQ(bulk_id, scalar_id) << "link " << i;
    }
}

TEST_F(LinksBulkPhase3Test, IdsBulkTruncatesAtStride) {
    int longest = 0;
    for (int i = 0; i < n_links_; ++i) {
        int L = static_cast<int>(std::strlen(swmm_link_id(engine_, i)));
        if (L > longest) longest = L;
    }
    if (longest <= 1) {
        GTEST_SKIP() << "Fixture's longest link ID is too short to "
                        "meaningfully test truncation.";
    }
    const int stride = longest;  // forces 1-char truncation
    std::vector<char> buf(static_cast<std::size_t>(n_links_) * stride, '\xAA');
    ASSERT_EQ(swmm_link_get_ids_bulk(engine_, buf.data(), stride, n_links_),
              SWMM_OK);
    for (int i = 0; i < n_links_; ++i) {
        const char* slot = buf.data() + i * stride;
        const std::size_t len = std::strlen(slot);
        EXPECT_LE(len, static_cast<std::size_t>(stride - 1)) << "link " << i;
    }
}

// ---------------------------------------------------------------------------
// Bad-param contracts.
// ---------------------------------------------------------------------------

TEST_F(LinksBulkPhase3Test, RejectsNullBuffer) {
    EXPECT_EQ(swmm_link_get_velocities_bulk(engine_, nullptr, n_links_),
              SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_link_get_capacities_bulk(engine_, nullptr, n_links_),
              SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_link_get_volumes_bulk(engine_, nullptr, n_links_),
              SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_link_get_control_settings_bulk(engine_, nullptr, n_links_),
              SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_link_get_target_settings_bulk(engine_, nullptr, n_links_),
              SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_link_get_hyd_powers_bulk(engine_, nullptr, n_links_),
              SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_link_get_ids_bulk(engine_, nullptr, 64, n_links_),
              SWMM_ERR_BADPARAM);
}

TEST_F(LinksBulkPhase3Test, RejectsNonPositiveCount) {
    std::vector<double> v(n_links_, 0.0);
    EXPECT_EQ(swmm_link_get_velocities_bulk(engine_, v.data(), 0),
              SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_link_get_velocities_bulk(engine_, v.data(), -1),
              SWMM_ERR_BADPARAM);
}

TEST_F(LinksBulkPhase3Test, IdsBulkRejectsTinyStride) {
    std::vector<char> buf(n_links_, '\0');
    EXPECT_EQ(swmm_link_get_ids_bulk(engine_, buf.data(), 1, n_links_),
              SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_link_get_ids_bulk(engine_, buf.data(), 0, n_links_),
              SWMM_ERR_BADPARAM);
}

TEST_F(LinksBulkPhase3Test, RejectsNullEngine) {
    std::vector<double> v(n_links_, 0.0);
    EXPECT_EQ(swmm_link_get_velocities_bulk(nullptr, v.data(), n_links_),
              SWMM_ERR_BADHANDLE);
    EXPECT_EQ(swmm_link_get_capacities_bulk(nullptr, v.data(), n_links_),
              SWMM_ERR_BADHANDLE);
    EXPECT_EQ(swmm_link_get_volumes_bulk(nullptr, v.data(), n_links_),
              SWMM_ERR_BADHANDLE);
    EXPECT_EQ(swmm_link_get_hyd_powers_bulk(nullptr, v.data(), n_links_),
              SWMM_ERR_BADHANDLE);
    std::vector<char> buf(n_links_ * 64, '\0');
    EXPECT_EQ(swmm_link_get_ids_bulk(nullptr, buf.data(), 64, n_links_),
              SWMM_ERR_BADHANDLE);
}

// ---------------------------------------------------------------------------
// Count clipping.
// ---------------------------------------------------------------------------

TEST_F(LinksBulkPhase3Test, OversizedCountClippedAtNLinks) {
    constexpr double kSentinel = -42.0;
    const int oversize = n_links_ + 5;
    std::vector<double> v(oversize, kSentinel);
    ASSERT_EQ(swmm_link_get_volumes_bulk(engine_, v.data(), oversize),
              SWMM_OK);
    for (int i = n_links_; i < oversize; ++i) {
        EXPECT_EQ(v[i], kSentinel) << "tail index " << i;
    }
}

} // namespace
