/**
 * @file GpuPluginSycl.cpp
 * @brief Plugin entry points for the Phase 5 Kokkos/SYCL surface solver (Intel).
 *
 * @details Exports the GpuPluginAbi.h contract for the openswmm_gpu_sycl
 *          plugin. Same shape as GpuPluginCuda.cpp — the probe advertises an
 *          Intel device and the factory constructs a CvodeKokkosSurfaceSolver
 *          — but the solver's execution space is Kokkos::SYCL (selected by the
 *          OPENSWMM_GPU_EXECSPACE_SYCL define the CMake target sets), so the
 *          whole RHS / preconditioner / vector pipeline runs device-resident.
 *
 *          The only structural difference from the CUDA/HIP probes is device
 *          discovery: SYCL enumerates devices through the SYCL runtime API
 *          (<sycl/sycl.hpp>) rather than a C device-count call, and it reports
 *          errors by throwing, so the probe wraps enumeration in a try/catch and
 *          maps any failure to the "no device -> CPU fallback" contract.
 *
 *          Per strategy §6.1, this file plus a CMake branch, the ExecSpace
 *          typedef, and a vcpkg feature are the entire Intel delta: the solver
 *          and kernel sources are reused verbatim from the CUDA backend.
 *
 *          Kokkos is initialized lazily on first solver construction and is
 *          intentionally NOT finalized (same rationale as the CUDA plugin):
 *          only this plugin uses Kokkos, and finalizing at dlclose risks
 *          destroying device Views that outlive the call.
 *
 *          Compiles only with a oneAPI toolchain (icpx) against a SYCL-enabled
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

#include <Kokkos_Core.hpp>
#include <sycl/sycl.hpp>

#include <cstring>
#include <vector>

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
    out->vendor      = OPENSWMM_GPU_VENDOR_SYCL;

    try {
        std::vector<sycl::device> devices =
            sycl::device::get_devices(sycl::info::device_type::gpu);
        if (devices.empty()) {
            // No usable SYCL GPU — tell the core to fall back to the CPU solver.
            std::strncpy(out->device_name, "no SYCL device",
                         sizeof(out->device_name) - 1);
            return 1;
        }
        out->device_count = static_cast<int>(devices.size());

        const sycl::device& dev = devices.front();
        const std::string name = dev.get_info<sycl::info::device::name>();
        std::strncpy(out->device_name, name.c_str(), sizeof(out->device_name) - 1);
        out->device_mem_bytes = static_cast<unsigned long long>(
            dev.get_info<sycl::info::device::global_mem_size>());
        return 0;  // usable device present
    } catch (const sycl::exception&) {
        // Any runtime/driver failure -> no usable device, CPU fallback.
        std::strncpy(out->device_name, "no SYCL device",
                     sizeof(out->device_name) - 1);
        return 1;
    }
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
