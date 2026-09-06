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
 * @license  Apache-2.0
 */

#include "../solver/GpuPluginAbi.h"
#include "../solver/ISurfaceSolver.hpp"
#include "ExplicitKokkosSurfaceSolver.hpp"

#include <Kokkos_Core.hpp>

#include <cstdlib>
#include <cstring>

namespace {
int g_host_threads = 0;   // threads Kokkos was initialised with (0 = not yet)

// Thread-count precedence (THREAD_LIMITS_AND_OVERSUBSCRIPTION_PLAN §5.2):
//   1. OPENSWMM_2D_THREADS env         — operator A/B override, always wins
//   2. requested_threads (ABI v4)      — [OPTIONS] THREADS handed over by the core
//   3. Kokkos default                  — OMP_NUM_THREADS, else omp_get_max_threads()
// Kokkos initialises once per process: the first solver built in a process
// fixes the count for every later one (the core warns when a later request
// differs — see SurfaceSolverFactory).
void ensureKokkosInitialized(int requested_threads) {
    if (!Kokkos::is_initialized() && !Kokkos::is_finalized()) {
        Kokkos::InitializationSettings settings;
        int chosen = 0;
        if (const char* env = std::getenv("OPENSWMM_2D_THREADS")) {
            char* endp = nullptr;
            const long n = std::strtol(env, &endp, 10);
            if (endp != env && n > 0) chosen = static_cast<int>(n);
        }
        if (chosen <= 0 && requested_threads > 0) chosen = requested_threads;
        if (chosen > 0) settings.set_num_threads(chosen);
        Kokkos::initialize(settings);
    }
#if defined(KOKKOS_ENABLE_OPENMP)
    g_host_threads = Kokkos::OpenMP().concurrency();
#else
    g_host_threads = Kokkos::DefaultHostExecutionSpace().concurrency();
#endif
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

extern "C" OPENSWMM_GPU_ABI int
openswmm_gpu_host_threads(void) {
    return g_host_threads;
}

extern "C" OPENSWMM_GPU_ABI void*
openswmm_make_gpu_explicit_solver(const OpenSwmmGpuProbe* probe) {
    ensureKokkosInitialized(probe ? probe->requested_threads : 0);
    openswmm::twoD::ISurfaceSolver* solver =
        new openswmm::twoD::gpu::ExplicitKokkosSurfaceSolver();
    return static_cast<void*>(solver);
}
