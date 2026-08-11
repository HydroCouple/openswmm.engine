/**
 * @file GpuPluginOmp.cpp
 * @brief Plugin entry points for the Phase 1 Kokkos/OpenMP surface solver.
 *
 * @details Exports the GpuPluginAbi.h contract for the openswmm_gpu_omp
 *          plugin. The probe advertises a usable Kokkos-OpenMP backend; the
 *          factory constructs a CvodeKokkosSurfaceSolver. The core casts the
 *          returned void* back to ISurfaceSolver* and owns it.
 *
 *          Kokkos is initialized lazily on first solver construction and is
 *          intentionally NOT finalized: only this plugin uses Kokkos, and
 *          finalizing at plugin/process teardown risks destroying Views that
 *          outlive the call. Leaving the runtime resident is the safe choice
 *          for a dlopen()ed plugin.
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

#include <cstdlib>
#include <cstring>

namespace {
void ensureKokkosInitialized() {
    if (!Kokkos::is_initialized() && !Kokkos::is_finalized()) {
        // Thread count: OPENSWMM_2D_THREADS wins, else Kokkos' own defaults
        // apply (OMP_NUM_THREADS, else all hardware threads — including
        // efficiency cores on Apple Silicon, which drag the fenced kernel
        // loops; set OPENSWMM_2D_THREADS to the performance-core count there).
        // [OPTIONS] THREADS deliberately does NOT govern the plugin: it caps
        // the host/serial-path loops only.
        Kokkos::InitializationSettings settings;
        if (const char* env = std::getenv("OPENSWMM_2D_THREADS")) {
            char* endp = nullptr;
            const long n = std::strtol(env, &endp, 10);
            if (endp != env && n > 0)
                settings.set_num_threads(static_cast<int>(n));
        }
        Kokkos::initialize(settings);
    }
}
} // namespace

extern "C" OPENSWMM_GPU_ABI int
openswmm_gpu_probe(OpenSwmmGpuProbe* out) {
    if (out == nullptr) return 1;
    std::memset(out, 0, sizeof(*out));
    out->abi_version  = OPENSWMM_GPU_ABI_VERSION;
    out->device_count = 1;  // the OpenMP host backend is always usable
    out->vendor       = OPENSWMM_GPU_VENDOR_OPENMP;
    std::strncpy(out->device_name, "Kokkos OpenMP (CPU, Phase 1)",
                 sizeof(out->device_name) - 1);
    return 0;  // usable backend present
}

extern "C" OPENSWMM_GPU_ABI void*
openswmm_make_gpu_explicit_solver(const OpenSwmmGpuProbe* /*probe*/) {
    ensureKokkosInitialized();
    openswmm::twoD::ISurfaceSolver* solver =
        new openswmm::twoD::gpu::ExplicitKokkosSurfaceSolver();
    return static_cast<void*>(solver);
}
