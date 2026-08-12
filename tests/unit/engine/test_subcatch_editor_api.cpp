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
 * @file test_subcatch_editor_api.cpp
 * @brief Round-trip tests for the subcatchment editor C API additions backing
 *        the GUI Property Browser / Attribute Table wiring:
 *          - @ref swmm_subcatch_set_infil_model
 *          - @ref swmm_subcatch_set_aquifer / @ref swmm_subcatch_get_aquifer
 *          - @ref swmm_subcatch_set_gw_node / @ref swmm_subcatch_get_gw_node
 *          - @ref swmm_subcatch_set_gw_params / @ref swmm_subcatch_get_gw_params
 *          - @ref swmm_lid_usage_add / _count / _get / _remove
 *
 * @details Config setters are guarded BUILDING/OPENED (CHECK_GEOMETRY), so the
 *          fixture opens the model but does NOT initialize it (state stays
 *          OPENED). Mirrors the contract style of
 *          @c test_subcatchments_bulk_phase3.cpp. Working directory is
 *          @c tests/unit/engine/data/ (set by CMakeLists).
 *
 * @ingroup engine_tests
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include <gtest/gtest.h>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_subcatchments.h>
#include <openswmm/engine/openswmm_infrastructure.h>

namespace {

class SubcatchEditorApiTest : public ::testing::Test {
protected:
    SWMM_Engine engine_ = nullptr;
    int n_subs_ = 0;

    void SetUp() override {
        engine_ = swmm_engine_create();
        ASSERT_NE(engine_, nullptr);
        // Open only (no initialize) so the engine stays in OPENED state and
        // the BUILDING/OPENED-guarded config setters are exercisable.
        ASSERT_EQ(swmm_engine_open(engine_,
                                   "site_drainage_model.inp",
                                   "site_drainage_model.rpt",
                                   "site_drainage_model.out",
                                   nullptr),
                  SWMM_OK)
            << "open failed: " << swmm_get_last_error_msg(engine_);
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
// Infiltration model code: every valid code round-trips; out-of-range rejected
// without mutating the stored value; bad handle/index contracts.
// ---------------------------------------------------------------------------
TEST_F(SubcatchEditorApiTest, InfilModelRoundTrip) {
    for (int model = 0; model <= 4; ++model) {
        EXPECT_EQ(swmm_subcatch_set_infil_model(engine_, 0, model), SWMM_OK)
            << "model=" << model;
        int got = -1;
        EXPECT_EQ(swmm_subcatch_get_infil_model(engine_, 0, &got), SWMM_OK);
        EXPECT_EQ(got, model);
    }
}

TEST_F(SubcatchEditorApiTest, InfilModelRejectsOutOfRange) {
    EXPECT_EQ(swmm_subcatch_set_infil_model(engine_, 0, 2), SWMM_OK);
    int before = -1;
    EXPECT_EQ(swmm_subcatch_get_infil_model(engine_, 0, &before), SWMM_OK);

    EXPECT_EQ(swmm_subcatch_set_infil_model(engine_, 0, 5),  SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_subcatch_set_infil_model(engine_, 0, -1), SWMM_ERR_BADPARAM);

    int after = -2;
    EXPECT_EQ(swmm_subcatch_get_infil_model(engine_, 0, &after), SWMM_OK);
    EXPECT_EQ(after, before) << "rejected write must not mutate the model code";

    EXPECT_EQ(swmm_subcatch_set_infil_model(nullptr, 0, 0), SWMM_ERR_BADHANDLE);
    EXPECT_EQ(swmm_subcatch_set_infil_model(engine_, n_subs_, 0), SWMM_ERR_BADINDEX);
}

// ---------------------------------------------------------------------------
// Aquifer assignment + receiving node round-trip (including the -1 "none"
// sentinel).
// ---------------------------------------------------------------------------
TEST_F(SubcatchEditorApiTest, AquiferAndGwNodeRoundTrip) {
    EXPECT_EQ(swmm_subcatch_set_aquifer(engine_, 0, 2), SWMM_OK);
    int aq = -99;
    EXPECT_EQ(swmm_subcatch_get_aquifer(engine_, 0, &aq), SWMM_OK);
    EXPECT_EQ(aq, 2);

    EXPECT_EQ(swmm_subcatch_set_aquifer(engine_, 0, -1), SWMM_OK);
    EXPECT_EQ(swmm_subcatch_get_aquifer(engine_, 0, &aq), SWMM_OK);
    EXPECT_EQ(aq, -1);

    EXPECT_EQ(swmm_subcatch_set_gw_node(engine_, 0, 3), SWMM_OK);
    int nd = -99;
    EXPECT_EQ(swmm_subcatch_get_gw_node(engine_, 0, &nd), SWMM_OK);
    EXPECT_EQ(nd, 3);
}

// ---------------------------------------------------------------------------
// Groundwater flow params round-trip with eight distinct values (stored raw).
// ---------------------------------------------------------------------------
TEST_F(SubcatchEditorApiTest, GwParamsRoundTrip) {
    EXPECT_EQ(swmm_subcatch_set_gw_params(engine_, 0,
                                          100.5, 0.1, 1.1, 0.2, 1.2, 0.3, 5.5, 6.6),
              SWMM_OK);
    double surf = 0, a1 = 0, b1 = 0, a2 = 0, b2 = 0, a3 = 0, tw = 0, hstar = 0;
    EXPECT_EQ(swmm_subcatch_get_gw_params(engine_, 0,
                                          &surf, &a1, &b1, &a2, &b2, &a3, &tw, &hstar),
              SWMM_OK);
    EXPECT_DOUBLE_EQ(surf,  100.5);
    EXPECT_DOUBLE_EQ(a1,    0.1);
    EXPECT_DOUBLE_EQ(b1,    1.1);
    EXPECT_DOUBLE_EQ(a2,    0.2);
    EXPECT_DOUBLE_EQ(b2,    1.2);
    EXPECT_DOUBLE_EQ(a3,    0.3);
    EXPECT_DOUBLE_EQ(tw,    5.5);
    EXPECT_DOUBLE_EQ(hstar, 6.6);

    // Null out-params are tolerated.
    EXPECT_EQ(swmm_subcatch_get_gw_params(engine_, 0,
                                          nullptr, nullptr, nullptr, nullptr,
                                          nullptr, nullptr, nullptr, nullptr),
              SWMM_OK);
}

// ---------------------------------------------------------------------------
// LID usage: add validates the control index, get reads the row back, remove
// erases it and shifts later rows down.
// ---------------------------------------------------------------------------
TEST_F(SubcatchEditorApiTest, LidUsageAddGetRemove) {
    ASSERT_EQ(swmm_lid_add(engine_, "BC_TEST", 0 /*BIO_CELL*/), SWMM_OK);
    const int lid = swmm_lid_index(engine_, "BC_TEST");
    ASSERT_GE(lid, 0);

    const int before = swmm_lid_usage_count(engine_);
    ASSERT_GE(before, 0);

    // Bad control index is rejected.
    EXPECT_EQ(swmm_lid_usage_add(engine_, 0, 9999, 1, 50.0, 5.0, 0.1, 0.4),
              SWMM_ERR_BADINDEX);

    ASSERT_EQ(swmm_lid_usage_add(engine_, 0, lid, 3, 100.0, 10.0, 0.2, 0.5), SWMM_OK);
    ASSERT_EQ(swmm_lid_usage_add(engine_, 1, lid, 1, 25.0,  4.0, 0.3, 0.6), SWMM_OK);
    EXPECT_EQ(swmm_lid_usage_count(engine_), before + 2);

    // Read back the first row we added (global index = before).
    int sc = -1, li = -1, num = -1, tp = -1;
    double area = 0, width = 0, isat = 0, fimp = 0, fperv = -1;
    EXPECT_EQ(swmm_lid_usage_get(engine_, before, &sc, &li, &num,
                                 &area, &width, &isat, &fimp, &tp, &fperv),
              SWMM_OK);
    EXPECT_EQ(sc, 0);
    EXPECT_EQ(li, lid);
    EXPECT_EQ(num, 3);
    EXPECT_DOUBLE_EQ(area, 100.0);
    EXPECT_DOUBLE_EQ(width, 10.0);
    EXPECT_DOUBLE_EQ(isat, 0.2);
    EXPECT_DOUBLE_EQ(fimp, 0.5);
    EXPECT_EQ(tp, 0);
    EXPECT_DOUBLE_EQ(fperv, 0.0);

    // Remove the first row; the second should shift down into its slot.
    ASSERT_EQ(swmm_lid_usage_remove(engine_, before), SWMM_OK);
    EXPECT_EQ(swmm_lid_usage_count(engine_), before + 1);
    EXPECT_EQ(swmm_lid_usage_get(engine_, before, &sc, &li, &num,
                                 &area, &width, &isat, &fimp, &tp, &fperv),
              SWMM_OK);
    EXPECT_EQ(sc, 1);
    EXPECT_EQ(num, 1);
    EXPECT_DOUBLE_EQ(area, 25.0);

    // Out-of-range get/remove contracts.
    EXPECT_EQ(swmm_lid_usage_get(engine_, 99999, &sc, &li, &num,
                                 &area, &width, &isat, &fimp, &tp, &fperv),
              SWMM_ERR_BADINDEX);
    EXPECT_EQ(swmm_lid_usage_remove(engine_, 99999), SWMM_ERR_BADINDEX);
    EXPECT_EQ(swmm_lid_usage_count(nullptr), -1);
}

// ---------------------------------------------------------------------------
// Zero-depression-storage impervious percentage ([SUBAREAS] PctZero). The
// accessors speak percent while the context stores a fraction, mirroring
// swmm_subcatch_set/get_imperv_pct.
// ---------------------------------------------------------------------------
TEST_F(SubcatchEditorApiTest, ZeroImpervPctReadsParsedValue) {
    // The fixture's [SUBAREAS] rows all carry PctZero = 25.
    double pct = -1.0;
    EXPECT_EQ(swmm_subcatch_get_zero_imperv_pct(engine_, 0, &pct), SWMM_OK);
    EXPECT_DOUBLE_EQ(pct, 25.0);
}

TEST_F(SubcatchEditorApiTest, ZeroImpervPctRoundTrip) {
    EXPECT_EQ(swmm_subcatch_set_zero_imperv_pct(engine_, 0, 62.5), SWMM_OK);
    double pct = -1.0;
    EXPECT_EQ(swmm_subcatch_get_zero_imperv_pct(engine_, 0, &pct), SWMM_OK);
    EXPECT_DOUBLE_EQ(pct, 62.5);

    // 0 and 100 are both meaningful endpoints, not sentinels.
    EXPECT_EQ(swmm_subcatch_set_zero_imperv_pct(engine_, 0, 0.0), SWMM_OK);
    EXPECT_EQ(swmm_subcatch_get_zero_imperv_pct(engine_, 0, &pct), SWMM_OK);
    EXPECT_DOUBLE_EQ(pct, 0.0);
    EXPECT_EQ(swmm_subcatch_set_zero_imperv_pct(engine_, 0, 100.0), SWMM_OK);
    EXPECT_EQ(swmm_subcatch_get_zero_imperv_pct(engine_, 0, &pct), SWMM_OK);
    EXPECT_DOUBLE_EQ(pct, 100.0);

    // Writing one subcatchment must not disturb its neighbour.
    double other = -1.0;
    EXPECT_EQ(swmm_subcatch_get_zero_imperv_pct(engine_, 1, &other), SWMM_OK);
    EXPECT_DOUBLE_EQ(other, 25.0);
}

TEST_F(SubcatchEditorApiTest, ZeroImpervPctHandleAndIndexContracts) {
    double pct = 0.0;
    EXPECT_EQ(swmm_subcatch_set_zero_imperv_pct(nullptr, 0, 10.0), SWMM_ERR_BADHANDLE);
    EXPECT_EQ(swmm_subcatch_get_zero_imperv_pct(nullptr, 0, &pct), SWMM_ERR_BADHANDLE);
    EXPECT_EQ(swmm_subcatch_set_zero_imperv_pct(engine_, n_subs_, 10.0), SWMM_ERR_BADINDEX);
    EXPECT_EQ(swmm_subcatch_get_zero_imperv_pct(engine_, n_subs_, &pct), SWMM_ERR_BADINDEX);
    EXPECT_EQ(swmm_subcatch_set_zero_imperv_pct(engine_, -1, 10.0), SWMM_ERR_BADINDEX);
    // A NULL out-pointer is tolerated, not a crash.
    EXPECT_EQ(swmm_subcatch_get_zero_imperv_pct(engine_, 0, nullptr), SWMM_OK);
}

// ---------------------------------------------------------------------------
// Curve Number: cn + drying time round-trip, and the model code is stamped.
// ---------------------------------------------------------------------------
TEST_F(SubcatchEditorApiTest, CurveNumberRoundTrip) {
    EXPECT_EQ(swmm_subcatch_set_infil_curve_number(engine_, 0, 80.0, 7.0), SWMM_OK);

    double cn = 0.0, dry = 0.0;
    EXPECT_EQ(swmm_subcatch_get_infil_curve_number(engine_, 0, &cn, &dry), SWMM_OK);
    EXPECT_DOUBLE_EQ(cn, 80.0);
    EXPECT_DOUBLE_EQ(dry, 7.0);

    // The setter stamps CURVE_NUMBER like the other family setters do.
    int model = -1;
    EXPECT_EQ(swmm_subcatch_get_infil_model(engine_, 0, &model), SWMM_OK);
    EXPECT_EQ(model, 4);

    // Either out-param may be NULL.
    cn = dry = 0.0;
    EXPECT_EQ(swmm_subcatch_get_infil_curve_number(engine_, 0, &cn, nullptr), SWMM_OK);
    EXPECT_DOUBLE_EQ(cn, 80.0);
    EXPECT_EQ(swmm_subcatch_get_infil_curve_number(engine_, 0, nullptr, &dry), SWMM_OK);
    EXPECT_DOUBLE_EQ(dry, 7.0);
    EXPECT_EQ(swmm_subcatch_get_infil_curve_number(engine_, 0, nullptr, nullptr), SWMM_OK);
}

TEST_F(SubcatchEditorApiTest, CurveNumberHandleAndIndexContracts) {
    double cn = 0.0;
    EXPECT_EQ(swmm_subcatch_set_infil_curve_number(nullptr, 0, 80.0, 7.0), SWMM_ERR_BADHANDLE);
    EXPECT_EQ(swmm_subcatch_get_infil_curve_number(nullptr, 0, &cn, nullptr), SWMM_ERR_BADHANDLE);
    EXPECT_EQ(swmm_subcatch_set_infil_curve_number(engine_, n_subs_, 80.0, 7.0), SWMM_ERR_BADINDEX);
    EXPECT_EQ(swmm_subcatch_get_infil_curve_number(engine_, n_subs_, &cn, nullptr), SWMM_ERR_BADINDEX);
}

// ---------------------------------------------------------------------------
// Slot agreement: the drying time the C API reads must be the one the
// [INFILTRATION] parser wrote — i.e. the THIRD column, matching legacy
// curvenum_setParams() p[2]. Reading a different slot silently zeroed the
// regeneration constant for every file-loaded CN subcatchment.
// ---------------------------------------------------------------------------
class CurveNumberInfilTest : public ::testing::Test {
protected:
    SWMM_Engine engine_ = nullptr;

    void SetUp() override {
        engine_ = swmm_engine_create();
        ASSERT_NE(engine_, nullptr);
        ASSERT_EQ(swmm_engine_open(engine_,
                                   "curve_number_infil.inp",
                                   "curve_number_infil.rpt",
                                   "curve_number_infil.out",
                                   nullptr),
                  SWMM_OK)
            << "open failed: " << swmm_get_last_error_msg(engine_);
    }

    void TearDown() override {
        if (engine_) {
            swmm_engine_close(engine_);
            swmm_engine_destroy(engine_);
            engine_ = nullptr;
        }
    }
};

TEST_F(CurveNumberInfilTest, DryingTimeParsedFromThirdColumn) {
    int model = -1;
    ASSERT_EQ(swmm_subcatch_get_infil_model(engine_, 0, &model), SWMM_OK);
    EXPECT_EQ(model, 4) << "project option INFILTRATION CURVE_NUMBER";

    double cn = 0.0, dry = 0.0;
    ASSERT_EQ(swmm_subcatch_get_infil_curve_number(engine_, 0, &cn, &dry), SWMM_OK);
    EXPECT_DOUBLE_EQ(cn, 80.0);
    EXPECT_DOUBLE_EQ(dry, 7.0) << "third [INFILTRATION] column is the drying time";

    ASSERT_EQ(swmm_subcatch_get_infil_curve_number(engine_, 1, &cn, &dry), SWMM_OK);
    EXPECT_DOUBLE_EQ(cn, 65.0);
    EXPECT_DOUBLE_EQ(dry, 3.0);
}

TEST_F(CurveNumberInfilTest, ZeroImpervPctParsedPerSubcatchment) {
    double pct = -1.0;
    ASSERT_EQ(swmm_subcatch_get_zero_imperv_pct(engine_, 0, &pct), SWMM_OK);
    EXPECT_DOUBLE_EQ(pct, 25.0);
    ASSERT_EQ(swmm_subcatch_get_zero_imperv_pct(engine_, 1, &pct), SWMM_OK);
    EXPECT_DOUBLE_EQ(pct, 60.0);
}

} // namespace
