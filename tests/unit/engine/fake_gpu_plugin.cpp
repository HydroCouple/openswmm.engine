// SPDX-License-Identifier: Apache-2.0
// Deliberately never constructs a solver. Verifies rejection before device
// probing and containment of a failed factory without accelerator hardware.

/**
 * @file      fake_gpu_plugin.cpp
 * @brief     Tests for fake gpu plugin.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include "2d/solver/GpuPluginAbi.h"
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
static std::atomic<int> probes{0}, factories{0};
/**
 * @brief Read the plugin-instance probe-attempt counter.
 * @return Atomic count since plugin load, including all probe invocations; not reset by reading.
 */
extern "C" OPENSWMM_GPU_ABI int openswmm_test_probe_count() {
    return probes.load();
}
/**
 * @brief Read the plugin-instance factory-attempt counter.
 * @return Atomic count since plugin load, including factory attempts that throw; not reset by
 *     reading.
 */
extern "C" OPENSWMM_GPU_ABI int openswmm_test_factory_count() {
    return factories.load();
}
/**
 * @brief Return a deliberately stale or current GPU build-layout contract.
 *
 * Reads process environment only; no GPU probing, factory call or counter change occurs. Caller
 * excludes competing environment mutation.
 * @return Borrowed static string: stale-v4-layout when OPENSWMM_TEST_STALE_GPU is present
 *     (regardless of value), otherwise OPENSWMM_GPU_BUILD_CONTRACT; never free it.
 */
extern "C" OPENSWMM_GPU_ABI const char* openswmm_gpu_build_contract() {
    return std::getenv("OPENSWMM_TEST_STALE_GPU") ? "stale-v4-layout" : OPENSWMM_GPU_BUILD_CONTRACT;
}
/**
 * @brief Advertise a synthetic CUDA device without touching accelerator hardware.
 *
 * Increments the atomic probe counter. Reports ABI version one lower when OPENSWMM_TEST_OLD_GPU_ABI
 * is present, otherwise current ABI, one CUDA-vendor device and a fixed injection label.
 * Environment access is unsynchronized; null output is not checked.
 *
 * @param[out] out Required non-null writable probe object, zeroed before fields are assigned.
 * @return Always 0 after writing the synthetic probe.
 */
extern "C" OPENSWMM_GPU_ABI int openswmm_gpu_probe(OpenSwmmGpuProbe* out) {
    ++probes;
    *out = {};
    out->abi_version = std::getenv("OPENSWMM_TEST_OLD_GPU_ABI") ? OPENSWMM_GPU_ABI_VERSION - 1
                                                                : OPENSWMM_GPU_ABI_VERSION;
    out->device_count = 1;
    out->vendor = OPENSWMM_GPU_VENDOR_CUDA;
    std::strcpy(out->device_name, "failure injection; no device used");
    return 0;
}
/**
 * @brief Inject a factory exception to test loader failure containment.
 *
 * This intentionally throwing test plugin exercises the C++ loader containment path; constructs no
 * device runtime or solver.
 *
 * @param[in] probe Ignored borrowed probe pointer; may be null because it is never dereferenced.
 * @return Never returns a solver; always throws std::runtime_error after incrementing the atomic
 *     factory counter.
 */
extern "C" OPENSWMM_GPU_ABI void* openswmm_make_gpu_explicit_solver(const OpenSwmmGpuProbe* probe) {
    ++factories;
    throw std::runtime_error("injected factory failure");
}
/**
 * @brief Accept a destroy call without any action in the never-constructing test plugin.
 *
 * Only appropriate for this injection plugin, whose factory never returns an object. Does not reset
 * counters or access device hardware.
 *
 * @param[in] solver Ignored pointer, including null; no object is freed or dereferenced.
 */
extern "C" OPENSWMM_GPU_ABI void openswmm_destroy_gpu_explicit_solver(void* solver) {}
