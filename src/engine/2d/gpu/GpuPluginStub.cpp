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
 * @file GpuPluginStub.cpp
 * @brief Placeholder GPU surface-solver plugin (Phase 0 scaffold).
 *
 * @details Implements the GpuPluginAbi.h contract with NO actual GPU backend.
 *          It exists so the runtime discovery / dlopen / fallback wiring can be
 *          built and exercised without a device toolchain. The probe reports
 *          "no device", so the core's auto-selection falls back to the serial
 *          CPU ExplicitInertialSolver.
 *
 *          This translation unit deliberately depends on NOTHING beyond the
 *          C ABI header and the C++ standard library — no Kokkos — so it
 *          compiles with a plain host compiler on every platform.
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "../solver/GpuPluginAbi.h"

#include <cstring>

extern "C" OPENSWMM_GPU_ABI int
openswmm_gpu_probe(OpenSwmmGpuProbe* out) {
    if (out == nullptr) {
        return 1;
    }
    std::memset(out, 0, sizeof(*out));
    out->abi_version  = OPENSWMM_GPU_ABI_VERSION;
    out->device_count = 0;
    out->vendor       = OPENSWMM_GPU_VENDOR_NONE;
    std::strncpy(out->device_name, "stub (no GPU backend wired)",
                 sizeof(out->device_name) - 1);
    // No device: tell the core to use the CPU solver.
    return 1;
}

extern "C" OPENSWMM_GPU_ABI void*
openswmm_make_gpu_explicit_solver(const OpenSwmmGpuProbe* /*probe*/) {
    // The Kokkos/CUDA/HIP/SYCL solver is not implemented until Phase 2.
    return nullptr;
}
