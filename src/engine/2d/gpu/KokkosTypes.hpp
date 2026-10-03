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
 * @file KokkosTypes.hpp
 * @brief Execution-space selection + view aliases for the Kokkos marcher plugin.
 *
 * @details One backend per plugin build, selected at compile time by the
 *          OPENSWMM_GPU_EXECSPACE_* define the plugin CMake sets:
 *
 *            omp  → Kokkos::OpenMP   (host-parallel; view copies are no-ops)
 *            cuda → Kokkos::Cuda     (NVIDIA)
 *            hip  → Kokkos::HIP      (AMD)
 *            sycl → Kokkos::SYCL     (Intel)
 *
 *          HIP and SYCL are top-level Kokkos:: names as of Kokkos 4.x (the
 *          project pins 4.x via vcpkg-overlays/kokkos). All buffers live in
 *          ExecSpace's memory_space: under OpenMP that is host memory, so the
 *          host↔device deep_copies in the solver are no-ops; under a device
 *          backend they become real transfers, confined to co-advance batch
 *          boundaries.
 *
 * @ingroup engine_2d_gpu
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_2D_GPU_KOKKOS_TYPES_HPP
#define OPENSWMM_ENGINE_2D_GPU_KOKKOS_TYPES_HPP

#include <Kokkos_Core.hpp>

namespace openswmm::twoD::gpu {

#if defined(OPENSWMM_GPU_EXECSPACE_CUDA)
using ExecSpace = Kokkos::Cuda;
#elif defined(OPENSWMM_GPU_EXECSPACE_HIP)
using ExecSpace = Kokkos::HIP;
#elif defined(OPENSWMM_GPU_EXECSPACE_SYCL)
using ExecSpace = Kokkos::SYCL;
#elif defined(OPENSWMM_GPU_EXECSPACE_OPENMP)
using ExecSpace = Kokkos::OpenMP;
#else
using ExecSpace = Kokkos::DefaultExecutionSpace;
#endif
using MemSpace  = ExecSpace::memory_space;

using DView  = Kokkos::View<double*, MemSpace>;        ///< mutable double array
using CDView = Kokkos::View<const double*, MemSpace>;  ///< const double array
using IView  = Kokkos::View<int*, MemSpace>;           ///< mutable int array
using CIView = Kokkos::View<const int*, MemSpace>;     ///< const int array

} // namespace openswmm::twoD::gpu

#endif // OPENSWMM_ENGINE_2D_GPU_KOKKOS_TYPES_HPP
