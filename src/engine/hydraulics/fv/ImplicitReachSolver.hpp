// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin

/**
 * @file      ImplicitReachSolver.hpp
 * @brief     Standalone implicit finite-volume reach solver.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#pragma once

#include <array>
#include <vector>

#include "NetworkMeshData.hpp"
#include "FvKernels.hpp"
#include "NativeGmres.hpp"

namespace openswmm::fv {

/// Standalone P1 experiment. No routing factory, input option or GUI selects it.
/// Backward Euler on the original cell (A,Q) equations with the existing HLL
/// flux. Only a flat, prismatic, single reach, fully above the SLOT taper is
/// supported. Each end is a wall or a prescribed stage with extrapolated
/// interior velocity. No nodes, structures, transport, TPA or forcing are solved.
class ImplicitReachSolver {
public:
    using Pair = std::array<double, 2>;
    using Block = std::array<double, 4>; // row major
    struct Problem {
        const NetworkMeshData* mesh = nullptr;
        std::vector<double> old_area, old_discharge;
        // NaN denotes a wall; finite values are depths over the flat bed (ft).
        double left_depth = 0.0, right_depth = 0.0;
        double lower_depth = 0.0, upper_depth = 0.0;
    };
    struct Endpoint {
        std::vector<double> area, discharge, depth;
        std::vector<kernels::FaceFlux> faces;
    };
    enum class LinearMethod { StructuredThenGmres, StructuredOnly, GmresOnly };
    enum class Preconditioner { BlockLine, BlockJacobi };
    struct BlockDiagnostics {
        int failed_block = -1;
        // Elimination pivot/row-scale ratio, NOT a condition-number estimate.
        double minimum_pivot_ratio = 1.0;
    };
    struct LinearControls {
        LinearMethod method = LinearMethod::StructuredThenGmres;
        Preconditioner preconditioner = Preconditioner::BlockLine;
        NativeGmres::Controls gmres;
    };
    struct LinearResult {
        bool converged = false, used_gmres = false, fallback = false;
        int identity_preconditioner_blocks = 0;
        int shifted_preconditioner_blocks = 0;
        double maximum_preconditioner_shift = 0.0;
        std::size_t wrapper_bytes = 0;
        BlockDiagnostics direct;
        NativeGmres::Result gmres;
        double relative_residual = 0.0;
    };
    struct Controls {
        int max_newton = 30, max_backtracks = 24;
        double absolute_volume = 1.e-11, relative_volume = 1.e-12;
        double absolute_momentum = 1.e-11, relative_momentum = 1.e-12;
        // Local finite-difference Jacobian for the first reference prototype;
        // all trial residuals re-evaluate the real closure and HLL selections.
        double difference_step = 1.e-4;
        LinearControls linear;
    };
    enum class Status {
        Converged,
        InvalidInput,
        NonFinite,
        LinearFailure,
        LineSearchFailed,
        IterationLimit
    };
    struct Result {
        Status status = Status::InvalidInput;
        int newton_iterations = 0, backtracks = 0;
        double scaled_residual = 0.0;
        double max_volume_residual = 0.0, max_momentum_residual = 0.0;
        double linear_relative_residual = 0.0;
        int gmres_iterations = 0, gmres_solves = 0, linear_fallbacks = 0;
        std::size_t max_gmres_workspace_bytes = 0;
        LinearResult last_linear;
        /**
         * @brief Report whether the nonlinear reach residual passed every physical budget.
         * @return True only for Status::Converged; no additional validation is performed.
         */
        bool converged() const { return status == Status::Converged; }
    };
    struct Linearization {
        std::vector<Block> lower, diagonal, upper;
        std::vector<Pair> residual, row_scale;
        Pair column_scale{};
    };

    /**
     * @brief Evaluate backward-Euler integrated volume and momentum residuals.
     *
     * Volume uses dx*(A(h)-old_area)+dt*(Fright-Fleft). Momentum uses
     * dx*(Q-oldQ)+dt*(Gright-Gleft+dx*friction). Face fluxes re-evaluate the actual shared HLL
     * branches; prescribed stages extrapolate interior velocity and walls reflect it. Allocations can
     * throw. A valid physical closure with positive wet area/radius remains a caller precondition.
     *
     * @param[in] p Borrowed mesh and copied old-state arrays for >=2 cells in one flat prismatic closed
     *     reach, one geometry and one conduit. Old A/Q must be finite and within
     *     [lower_depth,upper_depth] after inversion; lower_depth>=y_full, upper_depth>lower_depth. End
     *     stages are finite depths within bounds or NaN for reflective walls. No gates, culverts,
     *     offsets, local losses, force mains, TPA, nodes, structures or species are supported; no
     *     pointer is retained.
     * @param[in] trial One {depth ft, aggregate discharge ft3/s} pair per cell, finite and within the
     *     problem depth bounds.
     * @param[in] dt Finite positive substep in seconds.
     * @param[out] output Receives per-cell {volume defect ft3, integrated-discharge defect ft4/s} only
     *     after all checks pass.
     * @param[out] faces Optional writable n+1 face-flux array, or nullptr; published only on success.
     * @return True for a supported problem and finite admissible residual/flux evaluation; false leaves
     *     outputs unchanged.
     */
    static bool residual(const Problem& p, const std::vector<Pair>& trial, double dt,
                         std::vector<Pair>& output,
                         std::vector<kernels::FaceFlux>* faces = nullptr);
    /**
     * @brief Build the scaled local block-tridiagonal finite-difference Jacobian.
     *
     * Only the two faces adjoining each perturbed cell and its friction term are recomputed, giving
     * O(N) local work. Depth scale is 1 ft; discharge scale is max(c*T) over the old state. Physical
     * row scales are dx*T_old and dx*qscale. No global dense Jacobian is formed. Allocations propagate;
     * finite coefficients are checked by the subsequent linear solve.
     *
     * @param[in] p Borrowed mesh and copied old-state arrays for >=2 cells in one flat prismatic closed
     *     reach, one geometry and one conduit. Old A/Q must be finite and within
     *     [lower_depth,upper_depth] after inversion; lower_depth>=y_full, upper_depth>lower_depth. End
     *     stages are finite depths within bounds or NaN for reflective walls. No gates, culverts,
     *     offsets, local losses, force mains, TPA, nodes, structures or species are supported; no
     *     pointer is retained.
     * @param[in] trial One {depth ft, aggregate discharge ft3/s} pair per cell, finite and within the
     *     problem depth bounds.
     * @param[in] dt Finite positive substep in seconds.
     * @param[in] difference_step Finite positive relative perturbation, multiplied by
     *     max(1,abs(component)); depth perturbations clip to problem bounds.
     * @param[out] out Receives a complete scaled Jacobian/residual and row/column scales only on
     *     success.
     * @return False for invalid problem/trial, collapsed perturbation or invalid scaling/residual; true
     *     publishes the linearization.
     */
    static bool linearize(const Problem& p, const std::vector<Pair>& trial, double dt,
                          double difference_step, Linearization& out);
    /**
     * @brief Solve the scaled block-tridiagonal system with native two-by-two Thomas elimination.
     *
     * Allocates O(N) temporary factors/solution without a workspace cap. Output may alias rhs because
     * publication follows every read. Allocation exceptions propagate.
     *
     * @param[in] l Nonempty equal-sized lower/diagonal/upper block arrays; each block is row-major. No
     *     cross-block pivoting is available.
     * @param[in] rhs Finite two-component scaled right-hand side per block.
     * @param[out] output Receives scaled solution pairs only after finite and true-residual acceptance.
     * @param[in,out] relative_residual Written after residual evaluation as ||Ax-b||/||b|| (or absolute
     *     norm for zero RHS); earlier pivot/shape failures leave the prior value.
     * @param[out] diagnostics Optional writable pivot diagnostics; reset on entry, then records minimum
     *     elimination-pivot/scale ratio and failing block.
     * @return True if local pivots exceed 1e-14 of block scale and true error <=1e-12+1e-10*||rhs||;
     *     false leaves solution unchanged.
     */
    static bool solveBlocks(const Linearization& l, const std::vector<Pair>& rhs,
                            std::vector<Pair>& output, double& relative_residual,
                            BlockDiagnostics* diagnostics = nullptr);
    /**
     * @brief Solve the original scaled reach system with structured elimination or fixed
     * right-preconditioned GMRES.
     *
     * StructuredThenGmres attempts direct Thomas first; StructuredOnly returns its failure. Unsafe line
     * preconditioner blocks receive bounded diagonal shifts, while unsafe Jacobi blocks use identity;
     * the actual matrix is never altered. GMRES budget includes wrapper buffers plus native workspace,
     * but the preceding direct solve is not workspace-capped. Allocation exceptions outside NativeGmres
     * propagate. No mesh/state is retained or modified.
     *
     * @param[in] l Equal-sized finite nonempty block-tridiagonal matrix; row/column scaling metadata is
     *     not consumed here.
     * @param[in] rhs Finite scaled RHS pair per block.
     * @param[in] controls Structured/GMRES selection, BlockLine/BlockJacobi preconditioner and native
     *     GMRES budgets.
     * @param[out] output Receives scaled correction pairs only on convergence; failure preserves prior
     *     contents.
     * @return Convergence/fallback/residual/preconditioner/workspace diagnostics.
     */
    static LinearResult solveLinear(const Linearization& l, const std::vector<Pair>& rhs,
                                    const LinearControls& controls, std::vector<Pair>& output);
    /**
     * @brief Solve one supported implicit reach timestep with bounded Newton/backtracking.
     *
     * Starts from actual old conserved area/discharge, re-evaluates true integrated equations every
     * trial, and accepts only budget satisfaction or Armijo reduction. Halves line-search steps without
     * clipping/repairing accepted area or momentum. The endpoint is built locally before publication,
     * so returned failures preserve output. Allocations can throw; this standalone experiment is not
     * selected by routing/input/GUI options.
     *
     * @param[in] p Borrowed mesh and copied old-state arrays for >=2 cells in one flat prismatic closed
     *     reach, one geometry and one conduit. Old A/Q must be finite and within
     *     [lower_depth,upper_depth] after inversion; lower_depth>=y_full, upper_depth>lower_depth. End
     *     stages are finite depths within bounds or NaN for reflective walls. No gates, culverts,
     *     offsets, local losses, force mains, TPA, nodes, structures or species are supported; no
     *     pointer is retained.
     * @param[in] dt Finite positive timestep in seconds.
     * @param[in] controls Nonnegative Newton/backtrack limits; positive difference_step; nonnegative
     *     finite absolute/relative volume and momentum budgets, with a positive budget combination for
     *     each row kind.
     * @param[out] output Receives complete area ft2, discharge ft3/s, depth ft and n+1 face-flux arrays
     *     only on convergence.
     * @return Converged, InvalidInput, NonFinite, LinearFailure, LineSearchFailed or IterationLimit
     *     with residual/work counters.
     */
    Result solve(const Problem& p, double dt, const Controls& controls, Endpoint& output);
};
} // namespace openswmm::fv
