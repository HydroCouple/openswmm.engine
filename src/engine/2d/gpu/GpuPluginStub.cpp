/**
 * @file GpuPluginStub.cpp
 * @brief Placeholder GPU surface-solver plugin (Phase 0 scaffold).
 *
 * @details Implements the GpuPluginAbi.h contract with NO actual GPU backend.
 *          It exists so the runtime discovery / dlopen / fallback wiring
 *          (docs/2D_GPU_PORTABLE_CVODE_STRATEGY.md §4.2) can be built and
 *          exercised before the Kokkos kernel lands (Phase 2). The probe
 *          reports "no device", so the core's auto-selection falls back to
 *          the serial CPU CvodeSurfaceSolver.
 *
 *          This translation unit deliberately depends on NOTHING beyond the
 *          C ABI header and the C++ standard library — no Kokkos, no SUNDIALS
 *          — so it compiles with a plain host compiler on every platform.
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#include "../solver/GpuPluginAbi.h"

#include <cstring>

#if defined(_WIN32)
#  define OPENSWMM_GPU_EXPORT __declspec(dllexport)
#else
#  define OPENSWMM_GPU_EXPORT __attribute__((visibility("default")))
#endif

extern "C" OPENSWMM_GPU_EXPORT int
openswmm_gpu_probe(OpenSwmmGpuProbe* out) {
    if (out == nullptr) {
        return 1;
    }
    std::memset(out, 0, sizeof(*out));
    out->abi_version  = OPENSWMM_GPU_ABI_VERSION;
    out->device_count = 0;
    out->vendor       = OPENSWMM_GPU_VENDOR_NONE;
    std::strncpy(out->device_name, "stub (no GPU backend wired)",
                 sizeof(out->device_name) - 1);
    // No device: tell the core to use the CPU solver.
    return 1;
}

extern "C" OPENSWMM_GPU_EXPORT void*
openswmm_make_gpu_surface_solver(const OpenSwmmGpuProbe* /*probe*/) {
    // The Kokkos/CUDA/HIP/SYCL solver is not implemented until Phase 2.
    return nullptr;
}
