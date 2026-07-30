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

/** ABI version. The core refuses a plugin whose abi_version disagrees.
 *  v2: SurfaceStateData grew the active_set pointer (dry-cell masking) —
 *  a v1 plugin would mis-read the struct layout. */
#define OPENSWMM_GPU_ABI_VERSION 2

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
OPENSWMM_GPU_ABI int openswmm_gpu_probe(OpenSwmmGpuProbe* out);

/**
 * @brief Construct the GPU surface solver.
 *
 * @param probe  The probe result the core obtained from openswmm_gpu_probe().
 * @return An ISurfaceSolver* (as void*) owned by the caller, or NULL on
 *         failure. The core deletes it via the ISurfaceSolver destructor.
 */
OPENSWMM_GPU_ABI void* openswmm_make_gpu_surface_solver(const OpenSwmmGpuProbe* probe);

/**
 * @brief Construct the GPU LOCAL-INERTIAL surface solver (MOMENTUM=inertial).
 *
 * OPTIONAL ABI symbol (does not bump OPENSWMM_GPU_ABI_VERSION): the core
 * dlsym()s it only when the model selects the local-inertial momentum closure,
 * and falls back to the serial CPU ArkodeSurfaceSolver when a plugin does not
 * export it. Returns an ISurfaceSolver* (as void*) owned by the caller, or NULL.
 */
OPENSWMM_GPU_ABI void* openswmm_make_gpu_inertial_solver(const OpenSwmmGpuProbe* probe);

/**
 * @brief Construct the GPU EXPLICIT LTS MARCHER surface solver
 *        (INTEGRATOR=EXPLICIT — P5 Kokkos port of ExplicitInertialSolver).
 *
 * OPTIONAL ABI symbol (does not bump OPENSWMM_GPU_ABI_VERSION): the core
 * dlsym()s it only when the model selects the explicit marcher, and falls back
 * to the serial CPU ExplicitInertialSolver when a plugin does not export it.
 * Unlike the CVODE-family plugin solvers this one implements BOTH cell
 * closures (FLAT and VFR). Returns an ISurfaceSolver* (as void*) owned by the
 * caller, or NULL.
 */
OPENSWMM_GPU_ABI void* openswmm_make_gpu_explicit_solver(const OpenSwmmGpuProbe* probe);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* OPENSWMM_ENGINE_2D_GPU_PLUGIN_ABI_H */
