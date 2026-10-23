// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin

/**
 * @file      NativeGmres.hpp
 * @brief     Native GMRES iteration for finite-volume linear systems.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#pragma once

#include <cstddef>
#include <functional>
#include <vector>

namespace openswmm::fv {

/// Native restarted GMRES with FIXED LINEAR right preconditioning. These
/// callbacks overwrite all n entries, allocate no iteration workspace, and
/// must not change their operator during solve(). Variable inner solves need
/// FGMRES, which this class does not implement. No third-party solver code.
class NativeGmres {
public:
    using Apply = std::function<bool(const double*, double*)>;
    struct Controls {
        int restart = 40, max_iterations = 200;
        int stagnant_restarts = 3;
        double absolute_tolerance = 1.e-12, relative_tolerance = 1.e-10;
        std::size_t workspace_limit_bytes = 64 * 1024 * 1024;
    };
    enum class Status {
        Converged,
        InvalidInput,
        MemoryLimit,
        NonFinite,
        OperatorFailure,
        PreconditionerFailure,
        Breakdown,
        Stagnated,
        IterationLimit
    };
    struct Result {
        Status status = Status::InvalidInput;
        int iterations = 0, restarts = 0, matvecs = 0;
        int preconditioner_applications = 0, reorthogonalizations = 0;
        std::size_t workspace_bytes = 0;
        double true_residual = 0.0, estimated_residual = 0.0, tolerance = 0.0;
        bool happy_breakdown = false;
        int workspace_growths = 0;     // vector capacity increases in this call
        bool reused_workspace = false; // every required vector reused without growth
        /**
         * @brief Test whether the solve result passed true-residual acceptance.
         * @return True only for Status::Converged; does not rerun the operator or validate diagnostics.
         */
        bool converged() const { return status == Status::Converged; }
    };
    /**
     * @brief Solve a dense-vector linear system with restarted fixed-right-preconditioned GMRES.
     *
     * Starts from zero correction and accepts fresh norm(rhs-A*x)<=absolute+relative*norm(rhs),
     * including happy breakdown. Retained capacity budget excludes caller vectors/callback
     * storage/allocator overhead. Workspace bad_alloc maps to MemoryLimit; callback exceptions and
     * final output-copy allocation can propagate. Mutable workspace makes one instance non-reentrant;
     * caller serializes solve/clear operations.
     *
     * @param[in] rhs Borrowed nonempty finite n-vector; output may alias it because final publication follows all
     *     RHS use.
     * @param[in] matrix Required synchronous fixed linear operator; reads n entries, overwrites all n
     *     outputs and returns success. Must not retain work spans or reenter solver.
     * @param[in] inverse Optional fixed linear right-preconditioner with identical span contract; empty
     *     means identity. Variable inner solves are unsupported.
     * @param[in] controls restart>0, max_iterations>=0, stagnant_restarts>0; finite nonnegative
     *     absolute/relative tolerances with at least one positive; byte budget bounds retained numeric
     *     vectors.
     * @param[out] output Assigned the accepted correction only on convergence; no partial iterate
     *     published on a returned failure.
     * @return Status and iteration/restart/operator/true-residual/workspace diagnostics.
     */
    Result solve(const std::vector<double>& rhs, const Apply& matrix, const Apply& inverse,
                 const Controls& controls, std::vector<double>& output);
    /**
     * @brief Count retained numerical workspace capacity in bytes.
     * @return Sum of twelve vector capacities times sizeof(double); excludes vector
     *     objects/callback/caller memory.
     */
    std::size_t workspaceBytes() const { return bytes(); }
    /**
     * @brief Release all retained numerical workspace allocations.
     *
     * Swaps every work vector with empty storage; invalidates borrowed work pointers and requires no
     * active solve. Does not change caller results.
     */
    void clearWorkspace() { release(); }

private:
    std::vector<double> basis_, hessenberg_, cosine_, sine_, g_, y_;
    std::vector<double> x_, r_, w_, z_, update_, image_;
    /**
     * @brief Compute retained numerical-vector capacity without changing workspace.
     * @return Capacity total times sizeof(double); object/allocator overhead omitted.
     */
    std::size_t bytes() const;
    /**
     * @brief Drop every owned work vector allocation.
     *
     * Private implementation of clearWorkspace; never call concurrently with a solve.
     */
    void release();
};
} // namespace openswmm::fv
