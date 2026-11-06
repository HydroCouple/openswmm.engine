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
 * @file      NetworkSolverFactory.hpp
 * @brief     Runtime backend selection for the explicit FV 1D network solver.
 *
 * @details Mirrors 2d/solver/SurfaceSolverFactory.hpp: prefer a Kokkos plugin
 *          when one is present, is new enough, reports a usable device, and the
 *          problem is large enough to amortize per-kernel launch overhead;
 *          otherwise fall back to the serial/OpenMP CPU ExplicitFvSolver.
 *
 *          Lives in the core — it owns the CPU↔plugin decision — but pulls in
 *          no Kokkos or GPU dependency itself.
 *
 * @see plans/EXPLICIT_FV_KOKKOS_1D_SOLVER_PLAN.md §4, §5
 * @ingroup engine_fv
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_FV_NETWORK_SOLVER_FACTORY_HPP
#define OPENSWMM_ENGINE_FV_NETWORK_SOLVER_FACTORY_HPP

#include <memory>
#include <string>

namespace openswmm::fv {

class INetworkSolver;
struct FvOptions;

/**
 * @brief Construct an uninitialized CPU or compatible plugin network solver.
 *
 * OPENSWMM_FV_BACKEND overrides opts.backend after lowercasing; unknown names fall back to CPU.
 * pressurized_implicit forces CPU even for an explicit plugin request. AUTO tries cuda, hip, sycl,
 * omp in order unless below min_parallel_cells (overridden by
 * atol(OPENSWMM_FV_MIN_PARALLEL_CELLS)). Plugin absence, load/probe/ABI/device/factory failures
 * fall through to CPU. Filesystem/string/allocation errors and plugin-thrown exceptions can
 * propagate; this is not a no-throw factory. Loaded modules remain mapped for process lifetime.
 *
 * @param[in] opts Options used synchronously for backend, minimum cell count and CPU-only
 *     pressurized_implicit selection; not retained.
 * @param[out] chosen Optional writable backend label, or nullptr; receives a CPU label or
 *     backend/device label before successful return.
 * @param[in] n_cells Mesh cell count for AUTO gating; <=0 disables that gate. Explicit backend
 *     requests bypass it.
 * @return An owning non-null solver on normal return; caller initializes it against the actual
 *     mesh/state/options.
 */
std::unique_ptr<INetworkSolver> makeNetworkSolver(const FvOptions& opts, std::string* chosen,
                                                  int n_cells);

} // namespace openswmm::fv

#endif // OPENSWMM_ENGINE_FV_NETWORK_SOLVER_FACTORY_HPP
