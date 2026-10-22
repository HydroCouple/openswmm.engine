// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin

/**
 * @file      NonlinearPressureStorage.hpp
 * @brief     Nonlinear pressure and physical storage equations for finite-volume trials.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#pragma once

#include <vector>

#include "FvKernels.hpp"

namespace openswmm::fv {

/// P0.5 standalone candidate, NOT selected by a production routing option.
/// Frozen face laws q = q* - C (H_right - H_left), with the actual SLOT
/// area difference in continuity. All original cells remain unknowns.
class NonlinearPressureStorage {
public:
    struct Row {
        // Null geometry means a zero-storage algebraic junction. Real node
        // storage curves, TPA, wet/dry switches and active sets are not supported.
        const FvGeometry* geometry = nullptr;
        double length = 0.0;     // ft, positive for cells
        double bed = 0.0;        // ft
        double old_head = 0.0;   // ft, initial Newton guess
        double source = 0.0;     // cfs INTO row, includes frozen q* divergence
        double lower_head = 0.0; // admissible bounds, not post-solve clamps
        double upper_head = 0.0;
        double old_area = 0.0; // ft^2, conserved cell state, NOT h->A inversion
    };
    struct Edge {
        int left = -1;
        int right = -1;             // -1: prescribed boundary
        double conductance = 0.0;   // cfs/ft, frozen and strictly positive
        double boundary_head = 0.0; // used only if right == -1
    };
    struct Controls {
        int max_newton = 40;
        int max_linear = 400;
        int max_backtracks = 30;
        double absolute_volume = 1.e-10; // ft^3, per row
        double relative_volume = 1.e-12; // scale = max(1 ft^3, old V, dt |source|)
        double linear_relative = 1.e-12;
        double linear_absolute = 1.e-14; // ft^3, Euclidean residual norm
    };
    enum class Status {
        Converged,
        InvalidInput,
        UnanchoredComponent,
        NonFinite,
        LinearBreakdown,
        LinearIterationLimit,
        LinearResidualFailure,
        LineSearchFailed,
        NonlinearIterationLimit,
        EndpointResidualFailure
    };
    struct Result {
        Status status = Status::InvalidInput;
        int newton_iterations = 0;
        int linear_iterations = 0;
        int residual_evaluations = 0;
        int backtracks = 0;
        double max_volume_residual = 0.0;   // ft^3
        double sum_absolute_residual = 0.0; // ft^3
        double scaled_residual = 0.0;       // <= 1 required, independently recomputed
        double linear_residual = 0.0;       // true ||b - J delta||_2, last linear solve
        double linear_tolerance = 0.0;
        /**
         * @brief Test whether independently recomputed endpoint residuals passed acceptance.
         * @return True only for Status::Converged; does not reevaluate equations.
         */
        bool converged() const noexcept { return status == Status::Converged; }
    };

    /**
     * @brief Solve frozen-conductance nonlinear slot-storage continuity for candidate heads.
     *
     * Solves V(H)-length*old_area+dt*sum(C*(H-Hneighbor))-dt*source=0 using bounded Newton/line search
     * and true linear residual checks. Every connected component needs physical storage derivative or
     * prescribed-head anchor. Supports no real-node curves, TPA, wet/dry switch or active sets. Owns
     * reusable work arrays; allocations can throw, instance requires serialized use. Candidate-head
     * transaction does not modify input geometry or engine state.
     *
     * @param[in] rows Nonempty borrowed rows, count<=INT_MAX, with finite bounded old_head/source/bed.
     *     Geometry pointer NULL means algebraic zero storage; physical closed slot cells require
     *     positive length/t_slot/y_full and nonnegative finite old_area. Geometries remain live
     *     throughout.
     * @param[in] edges Borrowed valid distinct row endpoints with positive finite conductance cfs/ft;
     *     right=-1 uses a finite prescribed boundary_head.
     * @param[in] dt Positive finite interval seconds.
     * @param[in] controls Nonnegative Newton/linear/backtrack limits, positive finite absolute_volume
     *     and linear_absolute, nonnegative finite relative_volume, and0<linear_relative<1.
     * @param[out] output_heads Accepted ft head vector assigned only on convergence; every returned
     *     failure preserves prior contents.
     * @return Convergence/status and iteration/volume/linear-residual diagnostics; invalid inputs
     *     return InvalidInput.
     */
    Result solve(const std::vector<Row>& rows, const std::vector<Edge>& edges, double dt,
                 const Controls& controls, std::vector<double>& output_heads);

private:
    // Persistent O(rows + edges) workspace; no per-iteration allocations.
    std::vector<double> head_, trial_, residual_, trial_residual_, budget_;
    std::vector<double> diag_, delta_, r_, z_, p_, ap_, weight_;
    std::vector<int> parent_;
    std::vector<unsigned char> anchored_;
};

} // namespace openswmm::fv
