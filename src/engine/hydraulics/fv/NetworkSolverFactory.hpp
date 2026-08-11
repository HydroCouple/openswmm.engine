/**
 * @file NetworkSolverFactory.hpp
 * @brief Runtime backend selection for the explicit FV 1D network solver.
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
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_ENGINE_FV_NETWORK_SOLVER_FACTORY_HPP
#define OPENSWMM_ENGINE_FV_NETWORK_SOLVER_FACTORY_HPP

#include <memory>
#include <string>

namespace openswmm::fv {

class INetworkSolver;
struct FvOptions;

/**
 * @brief Construct the FV network solver for @p opts and a mesh of @p n_cells.
 *
 * @param opts     Solver options — `backend` and `min_parallel_cells` drive the
 *                 choice. The `OPENSWMM_FV_BACKEND` environment variable
 *                 (cpu|auto|omp|cuda|hip|sycl) overrides `backend`, matching the
 *                 2D module's `OPENSWMM_2D_BACKEND`.
 * @param chosen   [out, optional] Human-readable label of the selected backend.
 * @param n_cells  Mesh size for the small-problem gate; 0 = unknown ⇒ no gate.
 * @return Never null — falls back to the CPU solver on any failure.
 */
std::unique_ptr<INetworkSolver> makeNetworkSolver(const FvOptions& opts,
                                                  std::string* chosen,
                                                  int n_cells);

} // namespace openswmm::fv

#endif // OPENSWMM_ENGINE_FV_NETWORK_SOLVER_FACTORY_HPP
