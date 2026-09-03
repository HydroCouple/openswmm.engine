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
 * @file GpuPluginAbi.h
 * @brief Stable C ABI between the core engine and an optional GPU solver plugin.
 *
 * @details The GPU backend
 *          ships as a SEPARATE shared library that the core discovers and
 *          dlopen()s at runtime; it is never linked into openswmm_engine.
 *          Communication therefore crosses a plain C ABI so it is robust to
 *          compiler/toolchain differences between the core and the plugin
 *          (which may be built with nvcc/hipcc/icpx against Kokkos).
 *
 *          Two symbols form the contract:
 *            - openswmm_gpu_probe()              — cheap capability query
 *            - openswmm_make_gpu_explicit_solver() — marcher factory
 *
 *          The factory returns an ISurfaceSolver* as a void* (the core casts
 *          it back); ownership transfers to the core, which deletes through
 *          the ISurfaceSolver virtual destructor.
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_2D_GPU_PLUGIN_ABI_H
#define OPENSWMM_ENGINE_2D_GPU_PLUGIN_ABI_H

/*
 * Export/visibility marker for the two ABI entry points.
 *
 * The declaration here and the definition in each plugin .cpp must agree on
 * linkage or MSVC rejects the definition with C2375 ("redefinition; different
 * linkage") — a plain `extern "C"` declaration followed by an
 * `__declspec(dllexport)` definition is a mismatch. A plugin translation unit
 * defines OPENSWMM_GPU_PLUGIN_BUILD (see the gpu CMakeLists) so it both
 * declares and defines the symbols as dllexport. The core never link-imports
 * these (it resolves them via dlsym/GetProcAddress), so the non-plugin case is
 * a no-op marker.
 */
#if defined(_WIN32)
#  if defined(OPENSWMM_GPU_PLUGIN_BUILD)
#    define OPENSWMM_GPU_ABI __declspec(dllexport)
#  else
#    define OPENSWMM_GPU_ABI
#  endif
#elif defined(__GNUC__)
#  define OPENSWMM_GPU_ABI __attribute__((visibility("default")))
#else
#  define OPENSWMM_GPU_ABI
#endif

#ifdef __cplusplus
extern "C" {
#endif

/** ABI version. The core refuses a plugin whose abi_version disagrees,
 *  except that a v3 plugin is still accepted through the thread-count
 *  environment fallback (see requested_threads below).
 *  v3: CVODE/ARKODE plugin solvers retired (D2, 2026-07-29) — the explicit
 *  marcher factory is the only solver entry, and SurfaceStateData dropped the
 *  active_set pointer, so a v2 plugin would mis-read the struct layout.
 *  v4: OpenSwmmGpuProbe gained requested_threads (core → plugin, appended
 *  so the v3 prefix layout is unchanged) and the optional entry point
 *  openswmm_gpu_host_threads() (plugin → core). */
#define OPENSWMM_GPU_ABI_VERSION 4
#define OPENSWMM_GPU_ABI_VERSION_ENV_THREADS_FALLBACK 3

/** GPU vendor / backend identifier reported by a plugin. */
typedef enum OpenSwmmGpuVendor {
    OPENSWMM_GPU_VENDOR_NONE   = 0, /**< no usable device / stub          */
    OPENSWMM_GPU_VENDOR_CUDA   = 1, /**< NVIDIA (Kokkos CUDA)             */
    OPENSWMM_GPU_VENDOR_HIP    = 2, /**< AMD (Kokkos HIP)                 */
    OPENSWMM_GPU_VENDOR_SYCL   = 3, /**< Intel (Kokkos SYCL)             */
    OPENSWMM_GPU_VENDOR_OPENMP = 4  /**< CPU multithreaded (Kokkos OpenMP, Phase 1) */
} OpenSwmmGpuVendor;

/** Result of a capability probe. Filled by openswmm_gpu_probe(). */
typedef struct OpenSwmmGpuProbe {
    int                abi_version;       /**< must equal OPENSWMM_GPU_ABI_VERSION */
    int                device_count;      /**< number of usable devices (0 = none) */
    int                vendor;            /**< an OpenSwmmGpuVendor value           */
    char               device_name[128];  /**< human-readable device name           */
    unsigned long long device_mem_bytes; /**< global memory of selected device     */
    /** v4, core → plugin: [OPTIONS] THREADS for the host (OpenMP) backend.
     *  0 = plugin default. Written by the core AFTER openswmm_gpu_probe()
     *  returns and BEFORE openswmm_make_gpu_explicit_solver() is called; the
     *  plugin zero-fills it in the probe. Device backends ignore it. The
     *  OPENSWMM_2D_THREADS environment variable still overrides it. */
    int                requested_threads;
} OpenSwmmGpuProbe;

/**
 * @brief Query the plugin for a usable GPU device.
 *
 * Must be cheap and side-effect-free beyond backend initialization.
 *
 * @param out  Caller-allocated struct to fill. Zero-initialized on entry by
 *             the plugin.
 * @return 0 on success with at least one usable device; non-zero if no
 *         device is available or the probe failed. A non-zero return tells
 *         the core to fall back to the CPU solver.
 */
OPENSWMM_GPU_ABI int openswmm_gpu_probe(OpenSwmmGpuProbe* out);

/**
 * @brief Construct the Kokkos explicit LTS marcher surface solver.
 *
 * The only solver factory in the contract: the core dlsym()s it and falls
 * back to the serial CPU ExplicitInertialSolver when a plugin does not
 * export it. Implements BOTH cell closures (FLAT and VFR). Returns an
 * ISurfaceSolver* (as void*) owned by the caller, or NULL.
 */
OPENSWMM_GPU_ABI void* openswmm_make_gpu_explicit_solver(const OpenSwmmGpuProbe* probe);

/**
 * @brief (v4, optional) Host thread count the plugin's Kokkos runtime is
 *        initialised with; 0 when not yet initialised or not a host backend.
 *
 * Kokkos initialises once per process, so this is the number every later
 * solver in the process will run with regardless of requested_threads. The
 * core dlsym()s it optionally — a plugin that does not export it is still
 * accepted.
 */
OPENSWMM_GPU_ABI int openswmm_gpu_host_threads(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* OPENSWMM_ENGINE_2D_GPU_PLUGIN_ABI_H */
