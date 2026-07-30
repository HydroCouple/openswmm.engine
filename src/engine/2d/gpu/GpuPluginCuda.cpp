/**
 * @file GpuPluginCuda.cpp
 * @brief Plugin entry points for the Phase 2 Kokkos/CUDA surface solver.
 *
 * @details Exports the GpuPluginAbi.h contract for the openswmm_gpu_cuda
 *          plugin. Identical in shape to GpuPluginOmp.cpp — the probe advertises
 *          an NVIDIA device and the factory constructs the Kokkos marcher
 *          — but the solver's execution space is Kokkos::Cuda (selected by the
 *          OPENSWMM_GPU_EXECSPACE_CUDA define the CMake target sets), so the
 *          whole RHS / preconditioner / vector pipeline runs device-resident.
 *
 *          Kokkos is initialized lazily on first solver construction and is
 *          intentionally NOT finalized (same rationale as the OpenMP plugin):
 *          only this plugin uses Kokkos, and finalizing at dlclose risks
 *          destroying device Views that outlive the call.
 *
 *          Compiles only with a CUDA toolchain (nvcc / CUDA-clang) against a
 *          CUDA-enabled Kokkos. Not buildable on macOS/Apple Silicon.
 *
 * @ingroup engine_2d_gpu
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#include "../solver/GpuPluginAbi.h"
#include "../solver/ISurfaceSolver.hpp"
#include "ExplicitKokkosSurfaceSolver.hpp"

#include <Kokkos_Core.hpp>
#include <cuda_runtime.h>

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
    out->vendor      = OPENSWMM_GPU_VENDOR_CUDA;

    int device_count = 0;
    if (cudaGetDeviceCount(&device_count) != cudaSuccess || device_count <= 0) {
        // No usable CUDA device — tell the core to fall back to the CPU solver.
        std::strncpy(out->device_name, "no CUDA device",
                     sizeof(out->device_name) - 1);
        return 1;
    }
    out->device_count = device_count;

    cudaDeviceProp prop{};
    if (cudaGetDeviceProperties(&prop, 0) == cudaSuccess) {
        std::strncpy(out->device_name, prop.name, sizeof(out->device_name) - 1);
        out->device_mem_bytes = static_cast<unsigned long long>(prop.totalGlobalMem);
    } else {
        std::strncpy(out->device_name, "CUDA device", sizeof(out->device_name) - 1);
    }
    return 0;  // usable device present
}

extern "C" OPENSWMM_GPU_ABI void*
openswmm_make_gpu_explicit_solver(const OpenSwmmGpuProbe* /*probe*/) {
    ensureKokkosInitialized();
    openswmm::twoD::ISurfaceSolver* solver =
        new openswmm::twoD::gpu::ExplicitKokkosSurfaceSolver();
    return static_cast<void*>(solver);
}
