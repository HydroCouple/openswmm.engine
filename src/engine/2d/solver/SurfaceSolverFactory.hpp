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
 * @file SurfaceSolverFactory.hpp
 * @brief Runtime selection of the 2D surface solver backend (Phase 4).
 *
 * @details Resolves which ISurfaceSolver to construct based on the
 *          OPENSWMM_2D_BACKEND policy, discovering and dlopen()ing a GPU
 *          plugin when one is present and usable, and falling back to the
 *          serial CPU ExplicitInertialSolver otherwise. This is the runtime
 *          half of the portable-GPU strategy — the core
 *          stays Kokkos/GPU-free; the device backend lives entirely in a
 *          separately built plugin loaded here at runtime.
 *
 *          Selection policy (env OPENSWMM_2D_BACKEND, case-insensitive):
 *            unset / "auto" : try device plugins (cuda, hip, sycl) in order;
 *                             if none load with a usable device, use the CPU
 *                             solver. The CPU-threaded "omp" plugin is NOT
 *                             auto-selected (it would change numerics on a
 *                             machine that merely has it installed) — request
 *                             it explicitly.
 *            "cpu"          : always the serial CPU solver (no discovery).
 *            "omp"|"cuda"|"hip"|"sycl" : load exactly that plugin; if it is
 *                             absent or has no usable device, warn and fall
 *                             back to CPU (never hard-fail on GPU absence).
 *
 *          Plugin search path: OPENSWMM_GPU_PLUGIN_PATH (os-separated list),
 *          then the directory of the engine shared library, then its `gpu`
 *          subdirectory.
 *
 * @ingroup engine_2d
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_2D_SURFACE_SOLVER_FACTORY_HPP
#define OPENSWMM_ENGINE_2D_SURFACE_SOLVER_FACTORY_HPP

#include <memory>
#include <string>

namespace openswmm::twoD {

struct SolverOptions2D;
class ISurfaceSolver;

/**
 * @brief Construct the 2D surface solver per the runtime backend policy.
 *
 * Never throws on plugin absence or load failure — it resolves to the serial
 * CPU solver instead. A successfully loaded GPU plugin is reported on stderr;
 * the default CPU path is silent so ordinary runs produce no extra output.
 *
 * @param opts    Solver options (reserved for future per-model backend hints).
 * @param chosen  Optional out-param; receives a human-readable backend label
 *                (e.g. "cpu (serial CVODE)" or "cuda (NVIDIA A100)").
 * @param n_cells Triangle count of the mesh (0 = unknown). Under the default
 *                `auto` policy a mesh below the parallel-worthwhile threshold
 *                (env OPENSWMM_2D_MIN_PARALLEL_CELLS, default 20000) selects the
 *                serial CPU solver even when a GPU/OpenMP plugin is present —
 *                Kokkos' per-kernel launch overhead makes the accelerated path
 *                slower than serial on small meshes. An explicit
 *                OPENSWMM_2D_BACKEND always wins (no size gate).
 * @return An owned ISurfaceSolver. For a plugin-backed solver the owning
 *         shared library is kept resident for the process lifetime.
 */
std::unique_ptr<ISurfaceSolver> makeSurfaceSolver(const SolverOptions2D& opts,
                                                  std::string* chosen = nullptr,
                                                  int n_cells = 0);

} // namespace openswmm::twoD

#endif // OPENSWMM_ENGINE_2D_SURFACE_SOLVER_FACTORY_HPP
