// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
#pragma once

#include "NetworkMeshData.hpp"
#include "FvKernels.hpp"
#include "NativeGmres.hpp"
#include <array>

namespace openswmm::fv {

/// Standalone, first-order SLOT experiment, pressurized-only by default with
/// opt-in bounded wet free-surface/transition support. Uses every
/// original cell and shared face geometry, including oriented virtual faces.
/// Junctions have ZERO storage; bounded physical storage uses mesh V(h) tables.
/// Ponding/overflow, structures, losses,
/// dry states, TPA and transport are deliberately unsupported.
/// Ordinary degree-2 nodes retain algebraic heads in both standalone scopes.
/// Typed virtual faces stay direct.
/// No production routing option or GUI selects this solver.
class ImplicitNetworkSolver {
public:
    enum class Regime { PressurizedOnly, BoundedWetSlot };
    struct Problem {
        const NetworkMeshData* mesh = nullptr;
        std::vector<double> old_area, old_discharge;
        // Optional accepted depth, to retain head precision in a stiff slot.
        // Its closure area must equal old_area exactly; history remains Aold.
        std::vector<double> initial_depth;
        // One entry per mesh node. NaN means an unknown head (or an
        // inactive virtual node); finite means a prescribed absolute head.
        std::vector<double> fixed_head, initial_head;
        // Required when physical storage exists: accepted volumes (ft3), one
        // per node, zero for non-storage nodes. Storage initial_head is the
        // accepted old head and must reproduce old_node_volume exactly through
        // the mesh curve. It is never algebraically reinitialized. Prescribed
        // storage heads and depths above min(full_depth, table top) are rejected.
        std::vector<double> old_node_volume;
        // Rates, ft3/s, positive INTO the system. Empty means zero. Cell
        // sources carry zero momentum. Virtual-node lateral allocation must
        // already be explicit in cell_lateral; a virtual node cannot own it.
        std::vector<double> node_lateral, cell_lateral;
        Regime regime = Regime::PressurizedOnly;
        // Wet mode admits no dry cells or reconstructed sides, even during
        // line search. This is a rejection bound, never a depth/volume clamp.
        double minimum_wet_depth = .01;
        double minimum_slot_excess = .01, maximum_depth = 100.0;
    };
    struct Trial {
        std::vector<double> depth, discharge, head;
    };
    struct FaceReceipt {
        kernels::FaceFlux flux;
        double correction_left = 0.0, correction_right = 0.0;
    };
    struct Evaluation {
        // Cell rows: integrated volume (ft3) and momentum (ft4/s).
        // junction: instantaneous signed inflows + lateral (ft3/s), including
        // physical storage inflows. Only ordinary unknown heads balance to zero.
        // storage: Vnew - Vold - dt*inflow (ft3); zero at non-storage nodes.
        std::vector<double> volume, momentum, junction;
        std::vector<double> storage, node_volume;
        std::vector<FaceReceipt> faces; // ONE shared mass receipt per face
    };
    struct Endpoint : Trial {
        std::vector<double> area;
        std::vector<double> node_volume; // physical storage only; others zero
        std::vector<FaceReceipt> faces;
    };
    struct Matrix {
        std::vector<std::size_t> row;
        std::vector<int> column;
        std::vector<double> value;
        bool apply(const double* x, double* y) const;
    };
    struct Linearization {
        Matrix matrix;
        std::vector<double> residual, row_scale, column_scale;
        // Appended head row order, after interleaved cell (h,Q) rows.
        std::vector<int> nodes;
    };
    enum class CondensationStatus { NotAttempted, Converged, InvalidInput,
        UnsupportedStencil, BlockPivot, FillLimit, MemoryLimit,
        InterfaceFailure, ReconstructionFailure };
    enum class Preconditioner { ScalarIlu0, BlockIlu0 };
    struct WorkspaceDiagnostics {
        bool enabled = false, symbolic_reused = false, symbolic_rebuilt = false;
        bool cache_released = false;
        std::size_t retained_bytes = 0; // pattern + numeric buffers + Krylov capacities
    };
    /// One active symbolic layout per workspace; not shared by concurrent calls.
    /// Cached values are scratch only and are refilled/refactored on EVERY call.
    class LinearWorkspace {
    public:
        std::size_t bytes() const;
        void release();
    private:
        friend class ImplicitNetworkSolver;
        bool matches(const Matrix&, Preconditioner, std::size_t pairs) const;
        std::size_t factorBytes() const;
        bool valid_ = false;
        Preconditioner backend_ = Preconditioner::ScalarIlu0;
        std::size_t pairs_ = 0;
        std::vector<std::size_t> source_row_, row_, diagonal_, scatter_;
        std::vector<int> source_column_, column_;
        std::vector<double> scalar_lu_;
        std::vector<std::array<double,4>> block_lu_, inverses_;
        NativeGmres gmres_;
    };
    struct BlockIluDiagnostics {
        bool used = false; // attempted; may fail before factorization completes
        int blocks = 0, failed_block = -1;
        std::size_t entries = 0; // 1/2-variable blocks, not scalar nonzeros
        double minimum_pivot_ratio = 1.0, largest_shift = 0.0;
    };
    struct CondensationControls {
        int dense_limit = 128; // retained scalar unknowns; never a large fallback
        std::size_t max_schur_entries = 1000000;
        std::size_t workspace_limit_bytes = 64 * 1024 * 1024;
        bool reduced_gmres_only = false;
        bool tree_interface = false; // opt-in exact 1/2-variable leaf elimination
        Preconditioner preconditioner = Preconditioner::ScalarIlu0; // static solveCondensed selection
    };
    struct CondensationDiagnostics {
        CondensationStatus status = CondensationStatus::NotAttempted;
        int eliminated = 0, retained = 0, failed_cell = -1, failed_interface_pivot = -1;
        std::size_t schur_entries = 0, peak_workspace_bytes = 0;
        double minimum_block_pivot_ratio = 1.0, minimum_interface_pivot_ratio = 1.0;
        double full_true_residual = 0.0, full_tolerance = 0.0, reduced_true_residual = 0.0;
        bool used_dense = false, used_reduced_gmres = false;
        bool used_tree = false;
        int tree_eliminated = 0, core_scalars = 0, unsafe_leaf_pivots = 0;
        std::size_t core_entries = 0;
        double minimum_leaf_pivot_ratio = 1.0, core_true_residual = 0.0;
        NativeGmres::Result reduced; // preserve attempted work even after full fallback
        BlockIluDiagnostics reduced_block_ilu;
        WorkspaceDiagnostics reduced_workspace;
    };
    struct LinearResult {
        NativeGmres::Result gmres;
        int shifted_pivots = 0;
        std::size_t preconditioner_bytes = 0;
        CondensationDiagnostics condensation;
        BlockIluDiagnostics block_ilu;
        WorkspaceDiagnostics workspace;
        bool fallback = false;
        bool converged() const {
            return condensation.status==CondensationStatus::NotAttempted ? gmres.converged() :
                condensation.status==CondensationStatus::Converged || (fallback && gmres.converged());
        }
    };
    enum class LinearMethod { FullGmres, CondensedThenGmres, CondensedOnly };
    struct Controls {
        int max_newton = 30, max_backtracks = 24;
        double absolute_volume = 1.e-11, relative_volume = 1.e-12;
        double absolute_momentum = 1.e-11, relative_momentum = 1.e-12;
        double absolute_junction = 1.e-11, relative_junction = 1.e-12;
        double difference_step = 1.e-4;
        NativeGmres::Controls linear;
        LinearMethod method = LinearMethod::FullGmres; // preserve the comparator
        // Applies to full, reduced/core and fallback solves; overrides the
        // nested static-API selection when called through solve().
        Preconditioner preconditioner = Preconditioner::ScalarIlu0;
        bool reuse_linear_workspace = false; // instance-owned, opt-in; fresh numeric factors
        CondensationControls condensation;
    };
    enum class Status { Converged, InvalidInput, InitializationFailure,
                        LinearFailure, LineSearchFailed, IterationLimit, NonFinite };
    struct Result {
        Status status = Status::InvalidInput;
        int newton_iterations = 0, backtracks = 0, gmres_iterations = 0;
        double scaled_residual = 0.0;
        double max_volume_residual = 0.0, max_momentum_residual = 0.0;
        double max_junction_residual = 0.0;
        double max_storage_residual = 0.0; // ft3; uses volume tolerances
        LinearResult last_linear;
        std::vector<LinearResult> linear_history; // every attempted correction
        bool converged() const { return status == Status::Converged; }
    };

    /// Pure evaluation; no clock, ledger, node volume, carry or accepted-state
    /// writes. Bed/section forces belong ONLY to the corrected face divergence.
    static bool residual(const Problem&, const Trial&, double dt, Evaluation&);
    /// Local face/source differences, analytic storage/history derivatives.
    /// Sparse graph work; never finite-differences the full network per column.
    static bool linearize(const Problem&, const Trial&, double dt, double step, Linearization&);
    /// Native scalar ILU(0) comparator with fixed right-preconditioned GMRES.
    /// Guarded diagonal shifts affect ONLY the preconditioner, never A or b.
    /// Full-matrix comparator and fallback for solveCondensed below.
    static LinearResult solveLinear(const Matrix&, const std::vector<double>& rhs,
        const NativeGmres::Controls&, std::vector<double>&, LinearWorkspace* = nullptr);
    /// Native mixed-block ILU(0), fixed for one right-preconditioned GMRES solve.
    /// The first 2*cell_blocks scalars are cell pairs; the suffix is scalar heads.
    /// Complete existing blocks and add missing PRECONDITIONER diagonals only;
    /// no off-block fill, equation pins or changes to the original matrix/RHS.
    /// Guarded shifts affect only factors. Budget includes all block CSR/factor
    /// and Krylov buffers. Failure never publishes a correction.
    static LinearResult solveBlockLinear(const Matrix&, const std::vector<double>& rhs,
        std::size_t cell_blocks, const NativeGmres::Controls&, std::vector<double>&, LinearWorkspace* = nullptr);
    /// Exact eligible conduit-interior block elimination, sparse interface
    /// Schur solve and reconstruction of EVERY original scalar correction.
    /// Endpoint cells/heads are protected. No shifted exact factors or head pins.
    /// An unsafe block/stencil, resource limit, interface or full-residual failure
    /// may fall back to original-matrix GMRES after temporary storage is freed.
    /// Budget covers owned dynamic buffers, not caller matrix/input/output or
    /// allocator/vector-object/stack overhead. Failure never publishes output.
    static LinearResult solveCondensed(const NetworkMeshData&, const Matrix&,
        const std::vector<double>& rhs, const NativeGmres::Controls&,
        const CondensationControls&, bool allow_fallback, std::vector<double>&, LinearWorkspace* = nullptr);
    /// Bracketed algebraic-head initialization with cells held fixed. Does not
    /// alter conserved initial area/Q or invent a startup volume step.
    static bool initializeHeads(const Problem&, Trial&, double tolerance);
    /// Failure leaves Endpoint unchanged; no post-convergence water/Q repairs.
    Result solve(const Problem&, double dt, const Controls&, Endpoint&);
    std::size_t linearWorkspaceBytes() const { return linear_workspace_.bytes(); }
    void clearLinearWorkspace() { linear_workspace_.release(); }
private:
    LinearWorkspace linear_workspace_;
};
} // namespace openswmm::fv
