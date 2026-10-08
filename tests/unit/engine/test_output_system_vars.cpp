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
 * @file test_output_system_vars.cpp
 * @brief SWMM_OutSystemVar names address the slots the .out writer fills.
 *
 * DefaultOutputPlugin writes the 15 system values in legacy SysResults
 * order and stores TOTAL_INFLOW as the sum of RUNOFF, DW_INFLOW, GW_INFLOW,
 * RDII_INFLOW and EXT_INFLOW. System RUNOFF is the sum of subcatchment
 * runoff. Before 2026-10-02 the enum put RUNOFF at slot 5 (the DWF slot),
 * which the runoff identity catches; the inflow identity alone is vacuous on
 * this fixture because it has no DWF, GW, RDII or external inflow.
 * Each test generates its own output with the current engine: the shared,
 * checked-in .out predates the report-time runoff aggregation and can also
 * be overwritten by other test targets.
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include <gtest/gtest.h>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_output.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <numeric>
#include <vector>

namespace {

constexpr const char* kInputPath = "site_drainage_model.inp";

class OutputSystemVarsTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto output_dir = std::filesystem::path("output_system_vars") /
            ::testing::UnitTest::GetInstance()->current_test_info()->name();
        std::filesystem::create_directories(output_dir);
        const auto report = (output_dir / "run.rpt").string();
        const auto output = (output_dir / "run.out").string();
        ASSERT_EQ(swmm_engine_run(kInputPath, report.c_str(), output.c_str(), nullptr), 0)
            << "Failed to generate output; see " << report;
        handle_ = swmm_output_open(output.c_str());
        ASSERT_NE(handle_, nullptr) << "Failed to open generated output '" << output << "'";
    }
    void TearDown() override {
        if (handle_) swmm_output_close(handle_);
    }
    double sys(int period, SWMM_OutSystemVar v) {
        float value = NAN;
        EXPECT_EQ(swmm_output_get_system_result(handle_, period, v, &value), 0) << "var " << v;
        return value;
    }
    SWMM_Output handle_ = nullptr;
};

} // anonymous

TEST_F(OutputSystemVarsTest, TotalInflowIsSumOfLateralSources)
{
    const int n = swmm_output_get_period_count(handle_);
    ASSERT_GT(n, 0);
    double peakRunoff = 0.0;
    for (int p = 0; p < n; ++p) {
        const double runoff = sys(p, SWMM_OUT_SYS_RUNOFF);
        const double sum = runoff + sys(p, SWMM_OUT_SYS_DW_INFLOW) + sys(p, SWMM_OUT_SYS_GW_INFLOW) +
                           sys(p, SWMM_OUT_SYS_RDII_INFLOW) + sys(p, SWMM_OUT_SYS_EXT_INFLOW);
        const double total = sys(p, SWMM_OUT_SYS_TOTAL_INFLOW);
        EXPECT_NEAR(total, sum, 1e-5 * (1.0 + std::fabs(sum))) << "period " << p;
        peakRunoff = std::max(peakRunoff, runoff);
    }
    // The identity is vacuous on an all-zero file; the fixture has storm runoff.
    EXPECT_GT(peakRunoff, 0.0);
}

TEST_F(OutputSystemVarsTest, RunoffIsSumOfSubcatchmentRunoff)
{
    const int n = swmm_output_get_period_count(handle_);
    const int ns = swmm_output_get_subcatch_count(handle_);
    ASSERT_GT(ns, 0);
    std::vector<float> runoff(static_cast<size_t>(ns));
    double peak = 0.0;
    for (int p = 0; p < n; ++p) {
        ASSERT_EQ(swmm_output_get_subcatch_result(handle_, p, SWMM_OUT_SUBCATCH_RUNOFF, runoff.data()), 0);
        const double sum = std::accumulate(runoff.begin(), runoff.end(), 0.0);
        EXPECT_NEAR(sys(p, SWMM_OUT_SYS_RUNOFF), sum, 1e-5 * (1.0 + sum)) << "period " << p;
        peak = std::max(peak, sum);
    }
    EXPECT_GT(peak, 0.0);
}

TEST_F(OutputSystemVarsTest, LastSlotIsPetAndLatInflowAliasesTotal)
{
    EXPECT_EQ(SWMM_OUT_SYS_PET, 14);
    EXPECT_EQ(SWMM_OUT_SYS_LAT_INFLOW, SWMM_OUT_SYS_TOTAL_INFLOW);
    float value = NAN;
    EXPECT_EQ(swmm_output_get_system_result(handle_, 0, SWMM_OUT_SYS_PET, &value), 0);
    EXPECT_TRUE(std::isfinite(value));
}
