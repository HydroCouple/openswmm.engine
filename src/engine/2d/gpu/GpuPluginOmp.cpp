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
 * @file      GpuPluginOmp.cpp
 * @brief     Plugin entry points for the Phase 1 Kokkos/OpenMP surface solver.
 *
 * @details Exports the GpuPluginAbi.h contract for the openswmm_gpu_omp
 *          plugin. The probe advertises a usable Kokkos-OpenMP backend; the
 *          factory constructs an ExplicitKokkosSurfaceSolver. The core casts the
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
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include "../solver/GpuPluginAbi.h"
#include "../solver/GwRechargePluginAbi.h"
#include "../subsurface/SoilCharacteristic.hpp"
#include "../solver/ISurfaceSolver.hpp"
#include "GpuPluginSupport.hpp"
#include <stdexcept>

#include <Kokkos_Core.hpp>

#include <cstdlib>
#include <cmath>
#include <atomic>
#include <mutex>
#include <cstring>

namespace {
std::atomic<int> g_host_threads{0}; // effective runtime threads (0 = not initialized)
std::mutex g_initialization_mutex;
std::mutex g_recharge_mutex;

// Thread-count precedence (THREAD_LIMITS_AND_OVERSUBSCRIPTION_PLAN §5.2):
//   1. OPENSWMM_2D_THREADS env         — operator A/B override, always wins
//   2. requested_threads (ABI v4)      — [OPTIONS] THREADS handed over by the core
//   3. Kokkos default                  — OMP_NUM_THREADS, else omp_get_max_threads()
// Kokkos initialises once per process: the first solver built in a process
// fixes the count for every later one (the core warns when a later request
// differs — see SurfaceSolverFactory).
/**
 * @brief Initialize the plugin execution runtime once under a process-level mutex.
 * @throws std::runtime_error If Kokkos has already been finalized.
 * @note Existing initialized Kokkos state is reused; this helper does not finalize it.
 * Initialization exceptions propagate to the enclosing factory/operation handler. Caller uses
 * the execution-owner thread.
 * @param[in] requested_threads Positive preferred host thread count, or nonpositive for default.
 * OPENSWMM_2D_THREADS overrides it; later calls cannot change the first initialization and
 * refresh the effective atomic count.
 */
void ensureKokkosInitialized(int requested_threads) {
    std::lock_guard<std::mutex> initialization_lock(g_initialization_mutex);
    if (Kokkos::is_finalized())
        throw std::runtime_error("Kokkos runtime has already been finalized");
    if (!Kokkos::is_initialized() && !Kokkos::is_finalized()) {
        Kokkos::InitializationSettings settings;
        int chosen = 0;
        if (const char* env = std::getenv("OPENSWMM_2D_THREADS")) {
            char* endp = nullptr;
            const long n = std::strtol(env, &endp, 10);
            if (endp != env && n > 0)
                chosen = static_cast<int>(n);
        }
        if (chosen <= 0 && requested_threads > 0)
            chosen = requested_threads;
        if (chosen > 0)
            settings.set_num_threads(chosen);
        Kokkos::initialize(settings);
    }
#if defined(KOKKOS_ENABLE_OPENMP)
    g_host_threads = Kokkos::OpenMP().concurrency();
#else
    g_host_threads = Kokkos::DefaultHostExecutionSpace().concurrency();
#endif
}
} // namespace

extern "C" OPENSWMM_GPU_ABI int openswmm_gpu_probe(OpenSwmmGpuProbe* out) {
    if (out == nullptr)
        return 1;
    std::memset(out, 0, sizeof(*out));
    out->abi_version = OPENSWMM_GPU_ABI_VERSION;
    out->device_count = 1; // the OpenMP host backend is always usable
    out->vendor = OPENSWMM_GPU_VENDOR_OPENMP;
    std::strncpy(out->device_name, "Kokkos OpenMP (CPU, Phase 1)", sizeof(out->device_name) - 1);
    return 0; // usable backend present
}

extern "C" OPENSWMM_GPU_ABI int openswmm_gpu_host_threads(void) {
    return g_host_threads.load();
}

extern "C" OPENSWMM_GPU_ABI void* openswmm_make_gpu_explicit_solver(const OpenSwmmGpuProbe* probe) {
    return openswmm::twoD::gpu::plugin::make(
        [&] { ensureKokkosInitialized(probe ? probe->requested_threads : 0); });
}

/**
 * @brief Advertise the host-parallel pure recharge extension without starting Kokkos.
 * @param[in,out] out Required caller record whose struct_size equals sizeof(*out).
 * @return Zero after replacing the capability record, one for NULL/shape mismatch without
 * modification.
 * @note Supports CLOSED_FORM, all four soil laws, and host-owned water/quality/heat. Label
 * storage is inline and NUL-terminated. No pointer retention or allocation.
 */
extern "C" OPENSWMM_GPU_ABI int openswmm_gw_capabilities_v1(OpenSwmmGwCapabilitiesV1* out) {
    if (!out || out->struct_size != sizeof(*out))
        return 1;
    std::memset(out, 0, sizeof(*out));
    out->struct_size = sizeof(*out);
    out->version = OPENSWMM_GW_RECHARGE_ABI_VERSION;
    out->vendor = OPENSWMM_GPU_VENDOR_OPENMP;
    out->execution_kind = OPENSWMM_GW_EXECUTION_HOST_OPENMP;
    out->operators = OPENSWMM_GW_OPERATOR_CLOSED_FORM_RECHARGE;
    out->closures = 1u;
    out->soil_laws = 15u;
    out->host_owned = 15u;
    out->time_integrators = 3u;
    out->canonical_host_restart = 1;
    std::strncpy(out->label,
                 "Kokkos OpenMP CLOSED_FORM recharge; host-owned water/receipts/quality/heat",
                 sizeof(out->label) - 1);
    return 0;
}
/**
 * @brief Evaluate validated raw recharge rates using the shared soil law on OpenMP.
 * @param[in] input Borrowed count-element host input records; SI units and bounds are specified
 * by openswmm_gw_recharge_v1's ABI contract. NULL only for count zero.
 * @param[in] count Nonnegative batch cell count; zero returns success without initialization.
 * @param[out] output Caller-owned count-double recharge array, m/s downward-positive. NULL only
 * for zero count; must not overlap inputs.
 * @param[in] threads Preferred first-initialization worker count; environment override and an
 * existing Kokkos runtime take precedence.
 * @return Zero after a successful fence and finite output check; one for preflight input
 * refusal; two for runtime exceptions or nonfinite output. A runtime failure can leave partial
 * output.
 * @note Complete preflight precedes writes. A mutex serializes recharge launches; no
 * water/quality/heat state or receipts are mutated, and no pointers retained. The core owns
 * availability limits and acceptance.
 */
extern "C" OPENSWMM_GPU_ABI int openswmm_gw_recharge_v1(const OpenSwmmGwRechargeInputV1* input,
                                                        int count, double* output, int threads) {
    if (count < 0 || (count > 0 && (!input || !output)))
        return 1;
    for (int i = 0; i < count; ++i) {
        const auto& v = input[i];
        if (v.struct_size != sizeof(v) || v.law < 0 || v.law > 3 || !std::isfinite(v.Ks) ||
            v.Ks < 0 || !std::isfinite(v.theta_s) || !std::isfinite(v.theta_r) || v.theta_r < 0 ||
            v.theta_s <= v.theta_r || v.theta_s > 1 || !std::isfinite(v.alpha) || v.alpha <= 0 ||
            !std::isfinite(v.psi_b) || v.psi_b <= 0 || !std::isfinite(v.lambda) || v.lambda <= 0 ||
            !std::isfinite(v.vg_n) || v.vg_n <= 1 || !std::isfinite(v.vg_L) ||
            !std::isfinite(v.length) || v.length <= 0 || !std::isfinite(v.hu) || v.hu < 0)
            return 1;
    }
    if (count == 0)
        return 0;
    try {
        std::lock_guard<std::mutex> recharge_lock(g_recharge_mutex);
        ensureKokkosInitialized(threads);
        Kokkos::parallel_for("OpenSWMM CLOSED_FORM groundwater recharge",
                             Kokkos::RangePolicy<Kokkos::OpenMP>(0, count), [=](int i) {
                                 const auto& v = input[i];
                                 openswmm::twoD::soil::Params p;
                                 p.law = static_cast<openswmm::twoD::SoilChar>(v.law);
                                 p.Ks = v.Ks;
                                 p.theta_s = v.theta_s;
                                 p.theta_r = v.theta_r;
                                 p.alpha = v.alpha;
                                 p.psi_b = v.psi_b;
                                 p.lambda = v.lambda;
                                 p.vg_n = v.vg_n;
                                 p.vg_L = v.vg_L;
                                 output[i] = openswmm::twoD::soil::rechargeQ0(p, v.length, v.hu);
                             });
        Kokkos::fence();
        for (int i = 0; i < count; ++i)
            if (!std::isfinite(output[i]))
                return 2;
        return 0;
    } catch (...) {
        return 2;
    }
}
