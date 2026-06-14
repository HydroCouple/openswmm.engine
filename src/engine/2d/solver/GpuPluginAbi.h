/**
 * @file GpuPluginAbi.h
 * @brief Stable C ABI between the core engine and an optional GPU solver plugin.
 *
 * @details Phase 0 scaffolding for the portable GPU CVODE strategy
 *          (docs/2D_GPU_PORTABLE_CVODE_STRATEGY.md §4.1). The GPU backend
 *          ships as a SEPARATE shared library that the core discovers and
 *          dlopen()s at runtime; it is never linked into openswmm_engine.
 *          Communication therefore crosses a plain C ABI so it is robust to
 *          compiler/toolchain differences between the core and the plugin
 *          (which may be built with nvcc/hipcc/icpx against Kokkos).
 *
 *          Two symbols form the contract:
 *            - openswmm_gpu_probe()              — cheap capability query
 *            - openswmm_make_gpu_surface_solver()— factory for the backend
 *
 *          The factory returns an ISurfaceSolver* as a void* (the core casts
 *          it back); ownership transfers to the core, which deletes through
 *          the ISurfaceSolver virtual destructor.
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_ENGINE_2D_GPU_PLUGIN_ABI_H
#define OPENSWMM_ENGINE_2D_GPU_PLUGIN_ABI_H

#ifdef __cplusplus
extern "C" {
#endif

/** ABI version. The core refuses a plugin whose abi_version disagrees. */
#define OPENSWMM_GPU_ABI_VERSION 1

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
int openswmm_gpu_probe(OpenSwmmGpuProbe* out);

/**
 * @brief Construct the GPU surface solver.
 *
 * @param probe  The probe result the core obtained from openswmm_gpu_probe().
 * @return An ISurfaceSolver* (as void*) owned by the caller, or NULL on
 *         failure. The core deletes it via the ISurfaceSolver destructor.
 */
void* openswmm_make_gpu_surface_solver(const OpenSwmmGpuProbe* probe);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* OPENSWMM_ENGINE_2D_GPU_PLUGIN_ABI_H */
