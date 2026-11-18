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
 * @file      KokkosNetworkMarcher.hpp
 * @brief     Factory of the Kokkos substep marcher for the 1D FV solver.
 *
 * @details Kept free of Kokkos so the per-backend plugin wrappers can include
 *          it beside the surface solver's support header.
 *
 * @see KokkosNetworkMarcher.cpp, ../FvDeviceMarcher.hpp
 * @ingroup engine_fv
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_FV_GPU_KOKKOS_NETWORK_MARCHER_HPP
#define OPENSWMM_ENGINE_FV_GPU_KOKKOS_NETWORK_MARCHER_HPP

namespace openswmm::fv {
class IFvDeviceMarcher;
}

namespace openswmm::fv::gpu {

/// Create a marcher on the plugin's execution space. Kokkos must already be
/// initialized. The caller owns the result and deletes it through
/// IFvDeviceMarcher.
IFvDeviceMarcher* makeKokkosNetworkMarcher();

} // namespace openswmm::fv::gpu

#endif // OPENSWMM_ENGINE_FV_GPU_KOKKOS_NETWORK_MARCHER_HPP
