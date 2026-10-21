// SPDX-License-Identifier: Apache-2.0
// Deliberately never constructs a solver. Verifies rejection before device
// probing and containment of a failed factory without accelerator hardware.
#include "2d/solver/GpuPluginAbi.h"
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
static std::atomic<int> probes{0},factories{0};
extern "C" OPENSWMM_GPU_ABI int openswmm_test_probe_count() { return probes.load(); }
extern "C" OPENSWMM_GPU_ABI int openswmm_test_factory_count() { return factories.load(); }
extern "C" OPENSWMM_GPU_ABI const char* openswmm_gpu_build_contract() {
    return std::getenv("OPENSWMM_TEST_STALE_GPU") ? "stale-v4-layout" : OPENSWMM_GPU_BUILD_CONTRACT;
}
extern "C" OPENSWMM_GPU_ABI int openswmm_gpu_probe(OpenSwmmGpuProbe* out) {
    ++probes;
    *out={}; out->abi_version=std::getenv("OPENSWMM_TEST_OLD_GPU_ABI")?6:OPENSWMM_GPU_ABI_VERSION;
    out->device_count=1; out->vendor=OPENSWMM_GPU_VENDOR_CUDA;
    std::strcpy(out->device_name,"failure injection; no device used");
    return 0;
}
extern "C" OPENSWMM_GPU_ABI void* openswmm_make_gpu_explicit_solver(const OpenSwmmGpuProbe*) {
    ++factories;throw std::runtime_error("injected factory failure");
}
extern "C" OPENSWMM_GPU_ABI void openswmm_destroy_gpu_explicit_solver(void*) {}
