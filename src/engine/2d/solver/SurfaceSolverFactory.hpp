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
 * @file      SurfaceSolverFactory.hpp
 * @brief     Runtime selection of the 2D surface solver backend (Phase 4).
 *
 * @details Resolves which ISurfaceSolver to construct based on the
 *          OPENSWMM_2D_BACKEND policy, discovering and dlopen()ing a GPU
 *          plugin when one is present and usable, and falling back to the
 *          serial CPU ExplicitInertialSolver otherwise. This is the runtime
 *          half of the portable-GPU strategy — the core
 *          stays Kokkos/GPU-free; the device backend lives entirely in a
 *          separately built plugin loaded here at runtime.
 *
 *          Selection policy. The mode comes from the OPENSWMM_2D_BACKEND
 *          environment variable when set (case-insensitive), else from the
 *          model's [2D_OPTIONS] BACKEND (SolverOptions2D::backend):
 *            "auto" (default) : try device plugins (cuda, hip, sycl) in order
 *                             above the device mesh-size floor, then the
 *                             OpenMP plugin above its own (higher) floor; if
 *                             none loads with a usable device, use the CPU
 *                             solver.
 *            "cpu"          : always the serial CPU solver (no discovery).
 *            "omp"|"cuda"|"hip"|"sycl" : load exactly that plugin, bypassing
 *                             the size floors; if it is absent or has no
 *                             usable device, warn and fall back to CPU
 *                             (never hard-fail on GPU absence).
 *
 *          Plugin search path: OPENSWMM_GPU_PLUGIN_PATH (os-separated list),
 *          then the directory of the engine shared library, then its `gpu`
 *          subdirectory.
 *
 * @ingroup engine_2d
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_2D_SURFACE_SOLVER_FACTORY_HPP
#define OPENSWMM_ENGINE_2D_SURFACE_SOLVER_FACTORY_HPP

#include <memory>
#include <string>
#include "GwRechargeBackend.hpp"

namespace openswmm::twoD {

struct SolverOptions2D;
class ISurfaceSolver;

/**
 * @brief Construct the 2D surface solver per the runtime backend policy.
 *
 * Never throws on plugin absence or load failure — it resolves to the serial CPU solver instead.
 * A successfully loaded GPU plugin is reported on stderr; AUTO CPU fallback may also report its
 * backend selection rationale.
 *
 * @param[in] opts    Solver options; `backend` ([2D_OPTIONS] BACKEND) is the model's request,
 * overridden by OPENSWMM_2D_BACKEND when set.
 * @param[out] chosen  Optional out-param; receives a human-readable backend label (e.g. "cpu
 * (explicit marcher)" or "cuda (NVIDIA A100)").
 * @param[in] n_cells Triangle count of the mesh (0 = unknown). Under the default `auto` policy a
 * mesh below the parallel-worthwhile threshold (env OPENSWMM_2D_MIN_PARALLEL_CELLS, default
 * 50000) selects the serial CPU solver even when a GPU/OpenMP plugin is present — Kokkos'
 * per-kernel launch overhead makes the accelerated path slower than serial on small meshes. An
 * explicit backend (option or env) always wins (no size gate).
 * @return An owned ISurfaceSolver. For a plugin-backed solver the owning shared library is kept
 * resident for the process lifetime.
 *
 * @param[in] plugin_capable Caller-established compatibility: false forces the built-in marcher
 * for mixed quads, non-local-inertial momentum, transport or other unsupported plugin physics.
 * @throws std::invalid_argument For an unknown OPENSWMM_2D_BACKEND override.
 * @note Plugin probing/construction failures are caught and fall back to CPU; allocations for
 * the selected solver/label may still throw. Solver is constructed but not initialized; caller
 * supplies model arrays later.
 * @note AUTO uses a default 10000-cell device floor and 50000-cell OpenMP floor, each overridden
 * by its documented environment variable; nonpositive n_cells bypasses size gates. Explicit
 * named backends bypass both. Session auto_backend_cpu applies only to AUTO without an
 * environment override.
 * @note Plugin objects are wrapped by a core adapter; the adapter dispatches use/destruction to
 * the resident execution-owner thread. CPU fallback under AUTO may print its selection rationale
 * to stderr.
 */
std::unique_ptr<ISurfaceSolver> makeSurfaceSolver(const SolverOptions2D& opts,
                                                  std::string* chosen = nullptr, int n_cells = 0,
                                                  bool plugin_capable = true);

/**
 * @brief Optional stateless host recharge operator. Surface/water/transfer ownership stays with
 * the built-in marcher; absent/unsupported extensions select CPU.
 *
 * @details Discover an optional independently scheduled raw-recharge operator.
 * @par Argument contract
 * - Argument 1 (in): Borrowed validated SolverOptions2D, including backend request/session
 * preference. Environment override takes precedence.
 * - n_cells (in): Mesh-cell count, currently unused; no qualified AUTO mesh-size policy is
 * applied.
 * @return Value-owned backend descriptor. Empty evaluate/threads callbacks select scalar CPU and
 * diagnostic explains why. Explicit OMP is required for host-parallel recharge; AUTO remains
 * CPU.
 * @note Discovery never transfers groundwater state ownership. Captured execution owner and
 * resident library keep callbacks valid; their array arguments are borrowed synchronously.
 * Allocation/module-pin exceptions can propagate.
 */
GwRechargeBackend makeGwRechargeBackend(const SolverOptions2D&, int n_cells);

} // namespace openswmm::twoD

#endif // OPENSWMM_ENGINE_2D_SURFACE_SOLVER_FACTORY_HPP
