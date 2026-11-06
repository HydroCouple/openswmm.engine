// SPDX-License-Identifier: Apache-2.0

/**
 * @file      GpuPluginSupport.hpp
 * @brief     Shared support for GPU execution plugins.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#pragma once
#include "../solver/GpuPluginAbi.h"
#include "ExplicitKokkosSurfaceSolver.hpp"
#include <cstdio>
#include <exception>

#ifndef OPENSWMM_GPU_BUILD_CONTRACT
#error "GPU plugins require the generated build contract"
#endif
namespace openswmm::twoD::gpu::plugin {
inline thread_local char error[512]{};
/**
 * @brief Construct a plugin-owned Kokkos surface marcher with thread-local diagnostics.
 * @tparam Initialize Callable type invocable with no arguments.
 * @param[in] initialize Borrowed/copied synchronous initialization callable; not retained. May
 * throw to refuse construction.
 * @return Owned ISurfaceSolver pointer as void*, or NULL on any exception, with bounded text
 * copied to the calling thread's error buffer.
 * @note Clears the previous diagnostic first. Requires the plugin's controlling thread;
 * destruction uses the matching plugin entry point. No exception escapes, and process-runtime
 * finalization is outside this helper.
 */
template <class Initialize> void* make(Initialize initialize) noexcept {
    error[0] = '\0';
    try {
        initialize();
        return static_cast<ISurfaceSolver*>(new ExplicitKokkosSurfaceSolver());
    } catch (const std::exception& e) {
        std::snprintf(error, sizeof error, "%s", e.what());
    } catch (...) {
        std::snprintf(error, sizeof error, "Unknown plugin initialization failure");
    }
    return nullptr;
}
}
extern "C" OPENSWMM_GPU_ABI const char* openswmm_gpu_build_contract() {
    return OPENSWMM_GPU_BUILD_CONTRACT;
}
extern "C" OPENSWMM_GPU_ABI const char* openswmm_gpu_last_error() {
    return openswmm::twoD::gpu::plugin::error;
}
extern "C" OPENSWMM_GPU_ABI void openswmm_destroy_gpu_explicit_solver(void* solver) {
    delete static_cast<openswmm::twoD::ISurfaceSolver*>(solver);
}
