/**
 * @file GpuPluginHip.cpp
 * @brief Plugin entry points for the Phase 5 Kokkos/HIP surface solver (AMD).
 *
 * @details Exports the GpuPluginAbi.h contract for the openswmm_gpu_hip
 *          plugin. Identical in shape to GpuPluginCuda.cpp — the probe advertises
 *          an AMD device and the factory constructs a CvodeKokkosSurfaceSolver
 *          — but the solver's execution space is Kokkos::HIP (selected by the
 *          OPENSWMM_GPU_EXECSPACE_HIP define the CMake target sets), so the
 *          whole RHS / preconditioner / vector pipeline runs device-resident.
 *
 *          Per strategy §6.1, this file plus a CMake branch, the ExecSpace
 *          typedef, and a vcpkg feature are the entire AMD delta: the solver and
 *          kernel sources are reused verbatim from the CUDA backend.
 *
 *          Kokkos is initialized lazily on first solver construction and is
 *          intentionally NOT finalized (same rationale as the CUDA plugin):
 *          only this plugin uses Kokkos, and finalizing at dlclose risks
 *          destroying device Views that outlive the call.
 *
 *          Compiles only with a ROCm toolchain (hipcc) against a HIP-enabled
 *          Kokkos. Not buildable on macOS/Apple Silicon.
 *
 * @ingroup engine_2d_gpu
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#include "../solver/GpuPluginAbi.h"
#include "../solver/ISurfaceSolver.hpp"
#include "CvodeKokkosSurfaceSolver.hpp"
#include "ExplicitKokkosSurfaceSolver.hpp"

#include <Kokkos_Core.hpp>
#include <hip/hip_runtime.h>

#include <cstring>

namespace {
void ensureKokkosInitialized() {
    if (!Kokkos::is_initialized() && !Kokkos::is_finalized()) {
        Kokkos::initialize();
    }
}
} // namespace

extern "C" OPENSWMM_GPU_ABI int
openswmm_gpu_probe(OpenSwmmGpuProbe* out) {
    if (out == nullptr) return 1;
    std::memset(out, 0, sizeof(*out));
    out->abi_version = OPENSWMM_GPU_ABI_VERSION;
    out->vendor      = OPENSWMM_GPU_VENDOR_HIP;

    int device_count = 0;
    if (hipGetDeviceCount(&device_count) != hipSuccess || device_count <= 0) {
        // No usable HIP device — tell the core to fall back to the CPU solver.
        std::strncpy(out->device_name, "no HIP device",
                     sizeof(out->device_name) - 1);
        return 1;
    }
    out->device_count = device_count;

    hipDeviceProp_t prop{};
    if (hipGetDeviceProperties(&prop, 0) == hipSuccess) {
        std::strncpy(out->device_name, prop.name, sizeof(out->device_name) - 1);
        out->device_mem_bytes = static_cast<unsigned long long>(prop.totalGlobalMem);
    } else {
        std::strncpy(out->device_name, "HIP device", sizeof(out->device_name) - 1);
    }
    return 0;  // usable device present
}

extern "C" OPENSWMM_GPU_ABI void*
openswmm_make_gpu_surface_solver(const OpenSwmmGpuProbe* /*probe*/) {
    ensureKokkosInitialized();
    // Up-cast to the interface BEFORE erasing to void* so the core's
    // static_cast<ISurfaceSolver*> recovers a correctly-adjusted pointer.
    openswmm::twoD::ISurfaceSolver* solver =
        new openswmm::twoD::gpu::CvodeKokkosSurfaceSolver();
    return static_cast<void*>(solver);
}

extern "C" OPENSWMM_GPU_ABI void*
openswmm_make_gpu_explicit_solver(const OpenSwmmGpuProbe* /*probe*/) {
    ensureKokkosInitialized();
    openswmm::twoD::ISurfaceSolver* solver =
        new openswmm::twoD::gpu::ExplicitKokkosSurfaceSolver();
    return static_cast<void*>(solver);
}
