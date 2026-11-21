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
 * @file      test_backend_info.cpp
 * @brief     The compute-backend queries a client uses to fill its backend choices:
 *            swmm_backend_info and swmm_fv_device_supported.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_model.h>

namespace fs = std::filesystem;

namespace {

const char* kOutDir = "backend_info_out";

std::string outPath(const std::string& name) {
    fs::create_directories(kOutDir);
    return (fs::path(kOutDir) / name).string();
}

/// One pipe between a junction and a free outfall, routed with the finite volume solver.
std::string writeDeck(const std::string& base, const std::string& extra_options) {
    const std::string inp = outPath(base + ".inp");
    std::ofstream f(inp);
    f << "[OPTIONS]\nFLOW_UNITS CFS\nFLOW_ROUTING FV\nSTART_DATE 01/01/2026\n"
         "START_TIME 00:00:00\nEND_DATE 01/01/2026\nEND_TIME 00:10:00\nROUTING_STEP 1\n"
      << extra_options
      << "\n[JUNCTIONS]\nJ1 10 5\n\n[OUTFALLS]\nO1 9 FREE NO\n\n[CONDUITS]\n"
         "C1 J1 O1 100 0.013 0 0\n\n[XSECTIONS]\nC1 CIRCULAR 1 0 0 0 1\n";
    return inp;
}

SWMM_Engine openDeck(const std::string& base, const std::string& extra_options) {
    const std::string inp = writeDeck(base, extra_options);
    SWMM_Engine e = swmm_engine_create();
    EXPECT_EQ(swmm_engine_open(e, inp.c_str(), outPath(base + ".rpt").c_str(),
                               outPath(base + ".out").c_str(), nullptr),
              0);
    return e;
}

} // namespace

TEST(BackendInfo, CpuAndAutoAreAlwaysUsable) {
    for (int solver : {SWMM_BACKEND_SOLVER_FV_NETWORK, SWMM_BACKEND_SOLVER_2D_SURFACE})
        for (const char* name : {"CPU", "cpu", "AUTO"}) {
            SWMM_BackendInfo info;
            std::memset(&info, 0x5a, sizeof(info));
            ASSERT_EQ(swmm_backend_info(solver, name, &info), SWMM_OK) << name;
#ifndef OPENSWMM_HAS_2D
            if (solver == SWMM_BACKEND_SOLVER_2D_SURFACE) {
                EXPECT_EQ(info.usable, 0);
                continue;
            }
#endif
            EXPECT_EQ(info.usable, 1) << name;
            EXPECT_STREQ(info.reason, "") << name;
            EXPECT_STREQ(info.device_name, "CPU") << name;
        }
}

TEST(BackendInfo, EveryPluginBackendAnswersWithADeviceOrAReason) {
    for (int solver : {SWMM_BACKEND_SOLVER_FV_NETWORK, SWMM_BACKEND_SOLVER_2D_SURFACE})
        for (const char* name : {"OMP", "CUDA", "HIP", "SYCL"}) {
            SWMM_BackendInfo info;
            std::memset(&info, 0x5a, sizeof(info));
            ASSERT_EQ(swmm_backend_info(solver, name, &info), SWMM_OK) << name;
            if (info.usable) {
                EXPECT_STRNE(info.device_name, "") << name;
                EXPECT_STREQ(info.reason, "") << name;
            } else {
                EXPECT_STRNE(info.reason, "") << name;
            }
        }
}

#if defined(OPENSWMM_TEST_HAVE_OMP_PLUGIN) && defined(OPENSWMM_HAS_2D)
// The build that registered this test also built the OpenMP plugin, so both
// solvers must find it and no usable GPU plugin.
TEST(BackendInfo, TheBuiltOpenMpPluginIsFoundForBothSolvers) {
    for (int solver : {SWMM_BACKEND_SOLVER_FV_NETWORK, SWMM_BACKEND_SOLVER_2D_SURFACE}) {
        SWMM_BackendInfo info{};
        ASSERT_EQ(swmm_backend_info(solver, "omp", &info), SWMM_OK);
        EXPECT_EQ(info.usable, 1) << info.reason;
        EXPECT_NE(std::string(info.device_name).find("OpenMP"), std::string::npos)
            << info.device_name;
    }
    SWMM_BackendInfo cuda{};
    ASSERT_EQ(swmm_backend_info(SWMM_BACKEND_SOLVER_FV_NETWORK, "CUDA", &cuda), SWMM_OK);
    EXPECT_EQ(cuda.usable, 0);
    // Windows stages the 2D safety test's fake CUDA plugin, which has no 1D
    // solver, beside every test executable, where this engine also looks.
    const std::string reason = cuda.reason;
    EXPECT_TRUE(reason.find("not installed") != std::string::npos ||
                reason.find("does not provide this solver") != std::string::npos)
        << reason;
}
#endif

TEST(BackendInfo, BadArgumentsAreRefused) {
    SWMM_BackendInfo info{};
    EXPECT_EQ(swmm_backend_info(SWMM_BACKEND_SOLVER_FV_NETWORK, nullptr, &info), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_backend_info(SWMM_BACKEND_SOLVER_FV_NETWORK, "CPU", nullptr), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_backend_info(7, "CPU", &info), SWMM_ERR_BADPARAM);
    ASSERT_EQ(swmm_backend_info(SWMM_BACKEND_SOLVER_FV_NETWORK, "GPU", &info), SWMM_OK);
    EXPECT_EQ(info.usable, 0);
    EXPECT_NE(std::string(info.reason).find("not a backend name"), std::string::npos);
}

TEST(FvDeviceSupported, DefaultOptionsAreInScope) {
    SWMM_Engine e = openDeck("in_scope", "");
    int supported = -1;
    char why[128] = "x";
    ASSERT_EQ(swmm_fv_device_supported(e, &supported, why, sizeof(why)), SWMM_OK);
    EXPECT_EQ(supported, 1);
    EXPECT_STREQ(why, "");
    swmm_engine_close(e);
    swmm_engine_destroy(e);
}

TEST(FvDeviceSupported, NamesTheFirstOptionOutOfScope) {
    const struct {
        const char* base;
        const char* options;
        const char* expect;
    } cases[] = {
        {"order2", "FV_ORDER 2\n", "FV_ORDER 2"},
        {"rk2", "FV_TIME_INTEGRATION RK2\n", "RK2"},
        {"tpa", "FV_PRESSURE_CLOSURE TPA\n", "TPA"},
        {"implicit", "FV_PRESSURIZED_IMPLICIT YES\n", "FV_PRESSURIZED_IMPLICIT"},
    };
    for (const auto& c : cases) {
        SWMM_Engine e = openDeck(c.base, c.options);
        int supported = -1;
        char why[128] = "";
        ASSERT_EQ(swmm_fv_device_supported(e, &supported, why, sizeof(why)), SWMM_OK) << c.base;
        EXPECT_EQ(supported, 0) << c.base;
        EXPECT_NE(std::string(why).find(c.expect), std::string::npos) << c.base << ": " << why;
        swmm_engine_close(e);
        swmm_engine_destroy(e);
    }
}

TEST(FvDeviceSupported, BadArgumentsAreRefused) {
    int supported = 0;
    EXPECT_EQ(swmm_fv_device_supported(nullptr, &supported, nullptr, 0), SWMM_ERR_BADHANDLE);
    SWMM_Engine e = openDeck("bad_args", "");
    EXPECT_EQ(swmm_fv_device_supported(e, nullptr, nullptr, 0), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_fv_device_supported(e, &supported, nullptr, 0), SWMM_OK);
    swmm_engine_close(e);
    swmm_engine_destroy(e);
}
