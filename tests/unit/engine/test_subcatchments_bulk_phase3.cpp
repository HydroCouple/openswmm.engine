/**
 * @file test_subcatchments_bulk_phase3.cpp
 * @brief Unit tests for the Phase 3 subcatchment-bulk C API additions —
 *        @ref swmm_subcatch_get_rainfall_bulk,
 *        @ref swmm_subcatch_get_evap_bulk,
 *        @ref swmm_subcatch_get_infil_bulk,
 *        @ref swmm_subcatch_get_snow_depth_bulk, and
 *        @ref swmm_subcatch_get_ids_bulk.
 *
 * @details Mirrors the contract style established by the Nodes / Links
 *          Phase 3 tests: per-subcatch parity with the scalar accessor,
 *          stride-packed ID round-trip + truncation, and bad-param contracts.
 *
 *          Snow depth currently returns zeros from both the scalar and bulk
 *          variants pending full snow-state integration (see the placeholder
 *          comment in @c swmm_subcatch_get_snow_depth). The equivalence test
 *          below intentionally exercises that path.
 *
 * @see docs/C_API_BINDINGS_MCP_IMPROVEMENT_PLAN.md Phase 3 — Subcatchments batch
 * @ingroup engine_tests
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#include <gtest/gtest.h>
#include <cstring>
#include <string>
#include <vector>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_subcatchments.h>

namespace {

class SubcatchmentsBulkPhase3Test : public ::testing::Test {
protected:
    SWMM_Engine engine_ = nullptr;
    int n_subs_ = 0;

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
        n_subs_ = swmm_subcatch_count(engine_);
        ASSERT_GT(n_subs_, 0);
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
// subcatchment.
// ---------------------------------------------------------------------------

TEST_F(SubcatchmentsBulkPhase3Test, RainfallBulkMatchesScalarPerSubcatch) {
    std::vector<double> bulk(n_subs_, -42.0);
    ASSERT_EQ(swmm_subcatch_get_rainfall_bulk(engine_, bulk.data(), n_subs_),
              SWMM_OK);
    for (int i = 0; i < n_subs_; ++i) {
        double scalar = -1.0;
        ASSERT_EQ(swmm_subcatch_get_rainfall(engine_, i, &scalar), SWMM_OK);
        EXPECT_EQ(bulk[i], scalar) << "subcatch " << i;
    }
}

TEST_F(SubcatchmentsBulkPhase3Test, EvapBulkMatchesScalarPerSubcatch) {
    std::vector<double> bulk(n_subs_, -42.0);
    ASSERT_EQ(swmm_subcatch_get_evap_bulk(engine_, bulk.data(), n_subs_),
              SWMM_OK);
    for (int i = 0; i < n_subs_; ++i) {
        double scalar = -1.0;
        ASSERT_EQ(swmm_subcatch_get_evap(engine_, i, &scalar), SWMM_OK);
        EXPECT_EQ(bulk[i], scalar) << "subcatch " << i;
    }
}

TEST_F(SubcatchmentsBulkPhase3Test, InfilBulkMatchesScalarPerSubcatch) {
    std::vector<double> bulk(n_subs_, -42.0);
    ASSERT_EQ(swmm_subcatch_get_infil_bulk(engine_, bulk.data(), n_subs_),
              SWMM_OK);
    for (int i = 0; i < n_subs_; ++i) {
        double scalar = -1.0;
        ASSERT_EQ(swmm_subcatch_get_infil(engine_, i, &scalar), SWMM_OK);
        EXPECT_EQ(bulk[i], scalar) << "subcatch " << i;
    }
}

TEST_F(SubcatchmentsBulkPhase3Test, SnowDepthBulkMatchesScalarPerSubcatch) {
    // Both scalar and bulk currently return 0.0 for every subcatch — the
    // test verifies that documented placeholder behaviour explicitly.
    std::vector<double> bulk(n_subs_, -42.0);
    ASSERT_EQ(swmm_subcatch_get_snow_depth_bulk(engine_, bulk.data(), n_subs_),
              SWMM_OK);
    for (int i = 0; i < n_subs_; ++i) {
        double scalar = -1.0;
        ASSERT_EQ(swmm_subcatch_get_snow_depth(engine_, i, &scalar), SWMM_OK);
        EXPECT_EQ(bulk[i], scalar) << "subcatch " << i;
        EXPECT_EQ(bulk[i], 0.0) << "subcatch " << i
                                << " (placeholder snow state should be zero)";
    }
}

// ---------------------------------------------------------------------------
// IDs bulk — stride-packed UTF-8 format.
// ---------------------------------------------------------------------------

TEST_F(SubcatchmentsBulkPhase3Test, IdsBulkMatchesScalarPerSubcatch) {
    constexpr int kStride = 64;
    std::vector<char> buf(static_cast<std::size_t>(n_subs_) * kStride, '\xAA');
    ASSERT_EQ(swmm_subcatch_get_ids_bulk(engine_, buf.data(), kStride, n_subs_),
              SWMM_OK);
    for (int i = 0; i < n_subs_; ++i) {
        const char* bulk_id = buf.data() + i * kStride;
        const char* scalar_id = swmm_subcatch_id(engine_, i);
        ASSERT_NE(scalar_id, nullptr) << "subcatch " << i;
        EXPECT_STREQ(bulk_id, scalar_id) << "subcatch " << i;
    }
}

TEST_F(SubcatchmentsBulkPhase3Test, IdsBulkTruncatesAtStride) {
    int longest = 0;
    for (int i = 0; i < n_subs_; ++i) {
        int L = static_cast<int>(std::strlen(swmm_subcatch_id(engine_, i)));
        if (L > longest) longest = L;
    }
    if (longest <= 1) {
        GTEST_SKIP() << "Fixture's longest subcatch ID is too short to "
                        "meaningfully test truncation.";
    }
    const int stride = longest;
    std::vector<char> buf(static_cast<std::size_t>(n_subs_) * stride, '\xAA');
    ASSERT_EQ(swmm_subcatch_get_ids_bulk(engine_, buf.data(), stride, n_subs_),
              SWMM_OK);
    for (int i = 0; i < n_subs_; ++i) {
        const char* slot = buf.data() + i * stride;
        const std::size_t len = std::strlen(slot);
        EXPECT_LE(len, static_cast<std::size_t>(stride - 1)) << "subcatch " << i;
    }
}

// ---------------------------------------------------------------------------
// Bad-param contracts.
// ---------------------------------------------------------------------------

TEST_F(SubcatchmentsBulkPhase3Test, RejectsNullBuffer) {
    EXPECT_EQ(swmm_subcatch_get_rainfall_bulk(engine_, nullptr, n_subs_),
              SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_subcatch_get_evap_bulk(engine_, nullptr, n_subs_),
              SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_subcatch_get_infil_bulk(engine_, nullptr, n_subs_),
              SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_subcatch_get_snow_depth_bulk(engine_, nullptr, n_subs_),
              SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_subcatch_get_ids_bulk(engine_, nullptr, 64, n_subs_),
              SWMM_ERR_BADPARAM);
}

TEST_F(SubcatchmentsBulkPhase3Test, RejectsNonPositiveCount) {
    std::vector<double> v(n_subs_, 0.0);
    EXPECT_EQ(swmm_subcatch_get_rainfall_bulk(engine_, v.data(), 0),
              SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_subcatch_get_rainfall_bulk(engine_, v.data(), -2),
              SWMM_ERR_BADPARAM);
}

TEST_F(SubcatchmentsBulkPhase3Test, IdsBulkRejectsTinyStride) {
    std::vector<char> buf(n_subs_, '\0');
    EXPECT_EQ(swmm_subcatch_get_ids_bulk(engine_, buf.data(), 1, n_subs_),
              SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_subcatch_get_ids_bulk(engine_, buf.data(), 0, n_subs_),
              SWMM_ERR_BADPARAM);
}

TEST_F(SubcatchmentsBulkPhase3Test, RejectsNullEngine) {
    std::vector<double> v(n_subs_, 0.0);
    EXPECT_EQ(swmm_subcatch_get_rainfall_bulk(nullptr, v.data(), n_subs_),
              SWMM_ERR_BADHANDLE);
    EXPECT_EQ(swmm_subcatch_get_evap_bulk(nullptr, v.data(), n_subs_),
              SWMM_ERR_BADHANDLE);
    EXPECT_EQ(swmm_subcatch_get_infil_bulk(nullptr, v.data(), n_subs_),
              SWMM_ERR_BADHANDLE);
    EXPECT_EQ(swmm_subcatch_get_snow_depth_bulk(nullptr, v.data(), n_subs_),
              SWMM_ERR_BADHANDLE);
    std::vector<char> buf(n_subs_ * 64, '\0');
    EXPECT_EQ(swmm_subcatch_get_ids_bulk(nullptr, buf.data(), 64, n_subs_),
              SWMM_ERR_BADHANDLE);
}

// ---------------------------------------------------------------------------
// Count clipping.
// ---------------------------------------------------------------------------

TEST_F(SubcatchmentsBulkPhase3Test, OversizedCountClippedAtNSubcatches) {
    constexpr double kSentinel = -42.0;
    const int oversize = n_subs_ + 5;
    std::vector<double> v(oversize, kSentinel);
    ASSERT_EQ(swmm_subcatch_get_rainfall_bulk(engine_, v.data(), oversize),
              SWMM_OK);
    for (int i = n_subs_; i < oversize; ++i) {
        EXPECT_EQ(v[i], kSentinel) << "tail index " << i;
    }
}

} // namespace
