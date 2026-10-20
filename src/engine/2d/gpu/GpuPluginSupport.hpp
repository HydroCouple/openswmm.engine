// SPDX-License-Identifier: Apache-2.0
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
template<class Initialize> void* make(Initialize initialize) noexcept {
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
