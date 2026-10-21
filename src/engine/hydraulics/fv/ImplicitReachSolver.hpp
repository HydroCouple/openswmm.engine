// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
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
    enum class Status { Converged, InvalidInput, NonFinite, LinearFailure,
                        LineSearchFailed, IterationLimit };
    struct Result {
        Status status = Status::InvalidInput;
        int newton_iterations = 0, backtracks = 0;
        double scaled_residual = 0.0;
        double max_volume_residual = 0.0, max_momentum_residual = 0.0;
        double linear_relative_residual = 0.0;
        int gmres_iterations = 0, gmres_solves = 0, linear_fallbacks = 0;
        std::size_t max_gmres_workspace_bytes = 0;
        LinearResult last_linear;
        bool converged() const { return status == Status::Converged; }
    };
    struct Linearization {
        std::vector<Block> lower, diagonal, upper;
        std::vector<Pair> residual, row_scale;
        Pair column_scale{};
    };

    /// Physical integrated residuals: dx*(A-Aold)+dt*(Fright-Fleft),
    /// dx*(Q-Qold)+dt*(Gright-Gleft+dx*friction(Q,A)).
    /// Unknowns are trial depths and discharges; old_area is never reconstructed.
    static bool residual(const Problem&, const std::vector<Pair>& trial, double dt,
                         std::vector<Pair>& output,
                         std::vector<kernels::FaceFlux>* faces = nullptr);
    /// Block-tridiagonal Jacobian after physical row/column scaling. Local FD
    /// work is O(N), with no dense global Jacobian or global FD perturbations.
    static bool linearize(const Problem&, const std::vector<Pair>& trial, double dt,
                          double difference_step, Linearization&);
    /// Native O(N) block Thomas reference kernel; partial pivoting within each
    /// 2x2 block, guarded pivots and independently recomputed true residual.
    /// No cross-block pivoting. Failed solves never publish output.
    static bool solveBlocks(const Linearization&, const std::vector<Pair>& rhs,
                            std::vector<Pair>& output, double& relative_residual,
                            BlockDiagnostics* diagnostics = nullptr);
    /// Direct first, then native fixed block-line right-preconditioned GMRES.
    /// Unsafe line pivots are shifted ONLY in the preconditioner. Block Jacobi
    /// is a comparator (unsafe diagonal blocks use identity). This never changes
    /// the matrix being solved. Budget includes wrapper
    /// buffers and GMRES workspace. Failure leaves output unchanged.
    static LinearResult solveLinear(const Linearization&, const std::vector<Pair>& rhs,
                                    const LinearControls&, std::vector<Pair>& output);
    /// On any failure output is unchanged. No post-solve area or momentum repair.
    Result solve(const Problem&, double dt, const Controls&, Endpoint& output);
};
} // namespace openswmm::fv
