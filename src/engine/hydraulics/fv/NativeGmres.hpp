// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
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
    enum class Status { Converged, InvalidInput, MemoryLimit, NonFinite,
                        OperatorFailure, PreconditionerFailure, Breakdown,
                        Stagnated, IterationLimit };
    struct Result {
        Status status = Status::InvalidInput;
        int iterations = 0, restarts = 0, matvecs = 0;
        int preconditioner_applications = 0, reorthogonalizations = 0;
        std::size_t workspace_bytes = 0;
        double true_residual = 0.0, estimated_residual = 0.0, tolerance = 0.0;
        bool happy_breakdown = false;
        int workspace_growths = 0; // vector capacity increases in this call
        bool reused_workspace = false; // every required vector reused without growth
        bool converged() const { return status == Status::Converged; }
    };
    /// Zero initial correction. Acceptance requires a fresh ||b-A*x||_2 <=
    /// absolute + relative*||b||_2, including after a happy breakdown.
    /// No partial iterate is published on failure. Empty inverse means I.
    /// Budget covers retained vector capacities, excluding caller input/output,
    /// callback storage and vector-object/allocator overhead. Allocation sizes
    /// are overflow-checked before allocation. No allocation in Arnoldi loops.
    Result solve(const std::vector<double>& rhs, const Apply& matrix,
                 const Apply& inverse, const Controls&, std::vector<double>& output);
    std::size_t workspaceBytes() const { return bytes(); }
    void clearWorkspace() { release(); }

private:
    std::vector<double> basis_, hessenberg_, cosine_, sine_, g_, y_;
    std::vector<double> x_, r_, w_, z_, update_, image_;
    std::size_t bytes() const;
    void release();
};
} // namespace openswmm::fv
