// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
/**
 * @file test_engine_abi.cpp
 * @brief Verify the loaded library agrees with the public ABI identity compiled by its consumer.
 * @details Identity queries require no model, remain stable across engine lifecycles and do not
 * transfer ownership of the returned string. This catches stale/mismatched runtime packaging.
 * @author Caleb Buahin <caleb.buahin@gmail.com>
 * @date 2026
 * @copyright Copyright (c) 2026 Caleb Buahin
 * @license Apache-2.0
 */
#include "openswmm/engine/openswmm_engine.h"
#include "openswmm_abi_tag.h"
#include <gtest/gtest.h>

TEST(EngineAbi, RuntimeMatchesInstalledHeader)
{
    ASSERT_NE(swmm_engine_abi_tag(), nullptr);
    EXPECT_STREQ(swmm_engine_abi_tag(), OPENSWMM_ENGINE_ABI_TAG);
}

TEST(EngineAbi, StableBeforeAndAfterModelLifetime)
{
    const auto *identity = swmm_engine_abi_tag();
    auto engine = swmm_engine_create();
    ASSERT_NE(engine, nullptr);
    EXPECT_EQ(identity, swmm_engine_abi_tag());
    swmm_engine_destroy(engine);
    EXPECT_EQ(identity, swmm_engine_abi_tag());
}
