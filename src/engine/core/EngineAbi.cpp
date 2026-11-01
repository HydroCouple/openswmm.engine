// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
/**
 * @file EngineAbi.cpp
 * @brief Engine-owned runtime ABI identity for native plugin and Python handle sharing.
 * @details The tag is compiled into this engine image from its build-generated header. Querying
 * it allocates nothing, creates no engine instance and is safe from every thread and lifecycle
 * state. It identifies interface/build compatibility rather than model or solver behavior.
 * @author Caleb Buahin <caleb.buahin@gmail.com>
 * @date 2026
 * @copyright Copyright (c) 2026 Caleb Buahin
 * @license Apache-2.0
 */
#include "openswmm/engine/openswmm_engine.h"
#include "openswmm_abi_tag.h"

extern "C" const char *swmm_engine_abi_tag(void)
{
    return OPENSWMM_ENGINE_ABI_TAG;
}
