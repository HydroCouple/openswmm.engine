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
 * @file test_transect_table_stability.cpp
 * @brief The lifecycle guard that keeps cached transect-table pointers valid.
 *
 * @details `ctx.transect_tables` is a std::vector<TransectData>, and two
 *          consumers cache raw pointers INTO its elements:
 *
 *            - XSectBatch keeps `std::vector<const double*> area_tables`
 *              pointing at `TransectData::area_tbl` (XSectBatch.cpp);
 *            - Link's xsect struct does the same (Link.cpp).
 *
 *          Both capture during initialize(). A push_back that reallocated the
 *          vector after that point would dangle every one of them.
 *
 *          What makes this safe is not the container — it is that every API
 *          which can append a transect table is gated to BUILDING or OPENED by
 *          CHECK_GEOMETRY / CHECK_TOPOLOGY, so no append can happen once the
 *          pointers exist. That guard is load-bearing rather than incidental,
 *          and nothing else pinned it, so this test does.
 *
 *          If a future change relaxes the lifecycle gate on
 *          swmm_link_set_xsect (or adds another post-initialize path that
 *          appends to transect_tables), this test fails — and the fix is to
 *          give transect_tables stable element addresses (a std::deque is a
 *          drop-in; nothing indexes the store contiguously), not to delete the
 *          assertion.
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @license  Apache-2.0
 */

#include <gtest/gtest.h>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_links.h>

namespace {

constexpr const char* kInp = "street_xsect.inp";
constexpr const char* kRpt = "_transect_stability.rpt";
constexpr const char* kOut = "_transect_stability.out";

class TransectTableStability : public ::testing::Test {
protected:
    void SetUp() override {
        engine_ = swmm_engine_create();
        ASSERT_NE(engine_, nullptr);
        ASSERT_EQ(swmm_engine_open(engine_, kInp, kRpt, kOut, nullptr), SWMM_OK);
    }

    void TearDown() override {
        if (engine_ != nullptr) {
            swmm_engine_close(engine_);
            swmm_engine_destroy(engine_);
            engine_ = nullptr;
        }
    }

    SWMM_Engine engine_ = nullptr;
};

// ---------------------------------------------------------------------------
// Appending a STREET table is legal while OPENED — pointers are not cached yet
// ---------------------------------------------------------------------------

TEST_F(TransectTableStability, StreetXsectAppendAllowedBeforeInitialize) {
    const int link = swmm_link_index(engine_, "C1");
    ASSERT_GE(link, 0);

    // Several appends, each of which grows ctx.transect_tables. Safe here: no
    // consumer has captured element pointers yet.
    for (int i = 0; i < 8; ++i) {
        ASSERT_EQ(swmm_link_set_xsect(engine_, link, SWMM_XSECT_STREET,
                                      /*street index*/ 0.0, 0.0, 0.0, 0.0),
                  SWMM_OK) << "append " << i;
    }

    // The model still initializes and routes with the last-attached table.
    ASSERT_EQ(swmm_engine_initialize(engine_), SWMM_OK);
    ASSERT_EQ(swmm_engine_start(engine_, 1), SWMM_OK);
    double elapsed_days = 0.0;
    int    steps        = 0;
    do {
        ASSERT_EQ(swmm_engine_step(engine_, &elapsed_days), SWMM_OK);
    } while (elapsed_days > 0.0 && ++steps < 100000);
    EXPECT_GT(steps, 0);
    ASSERT_EQ(swmm_engine_end(engine_), SWMM_OK);
}

// ---------------------------------------------------------------------------
// …and refused once initialize() has cached pointers into the store
// ---------------------------------------------------------------------------

TEST_F(TransectTableStability, StreetXsectAppendRefusedAfterInitialize) {
    const int link = swmm_link_index(engine_, "C1");
    ASSERT_GE(link, 0);

    ASSERT_EQ(swmm_engine_initialize(engine_), SWMM_OK);

    // This is the assertion that keeps the raw-pointer caching sound. If it
    // ever returns SWMM_OK, transect_tables can reallocate under live
    // pointers — see the file comment.
    EXPECT_EQ(swmm_link_set_xsect(engine_, link, SWMM_XSECT_STREET,
                                  0.0, 0.0, 0.0, 0.0),
              SWMM_ERR_LIFECYCLE);

    // Same gate after the run has started.
    ASSERT_EQ(swmm_engine_start(engine_, 1), SWMM_OK);
    EXPECT_EQ(swmm_link_set_xsect(engine_, link, SWMM_XSECT_STREET,
                                  0.0, 0.0, 0.0, 0.0),
              SWMM_ERR_LIFECYCLE);
    ASSERT_EQ(swmm_engine_end(engine_), SWMM_OK);
}

}  // namespace
