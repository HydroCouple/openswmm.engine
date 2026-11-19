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
 * @file      CudaArchitectureMatch.hpp
 * @brief     Decide whether a CUDA device can run the plugin's device code.
 *
 * @details The CUDA plugin links relocatable device code, which holds one
 *          device binary per architecture it was built for. A binary built for
 *          compute capability X.y runs on devices X.z with z >= y and on no
 *          other major revision. Kokkos aborts the process when it starts on a
 *          device with no matching binary, so the probe checks first and
 *          reports "no usable device" instead.
 *
 * @ingroup engine_2d_gpu
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_2D_GPU_CUDA_ARCHITECTURE_MATCH_HPP
#define OPENSWMM_2D_GPU_CUDA_ARCHITECTURE_MATCH_HPP

namespace openswmm::twoD::gpu {

/**
 * @brief Whether device code built for @p built runs on a device of compute capability
 *        @p major.@p minor.
 * @param[in] built Architectures as nvcc numbers them (89 is 8.9, 120 is 12.0). A value of 0 or
 *                  less means the build did not record its architectures.
 * @param[in] count Number of entries in @p built.
 * @param[in] major Device compute capability, major part.
 * @param[in] minor Device compute capability, minor part.
 * @return true when one built architecture has the device's major revision and a minor revision
 *         no greater than the device's, or when the build did not record its architectures.
 */
constexpr bool cudaDeviceRunsBuiltCode(const int* built, int count, int major, int minor) noexcept {
    for (int i = 0; i < count; ++i) {
        if (built[i] <= 0)
            return true;
        if (built[i] / 10 == major && built[i] % 10 <= minor)
            return true;
    }
    return count <= 0;
}

} // namespace openswmm::twoD::gpu

#endif // OPENSWMM_2D_GPU_CUDA_ARCHITECTURE_MATCH_HPP
