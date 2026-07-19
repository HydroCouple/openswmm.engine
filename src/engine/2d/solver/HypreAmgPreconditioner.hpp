/**
 * @file HypreAmgPreconditioner.hpp
 * @brief hypre BoomerAMG algebraic-multigrid preconditioner for the 2D
 *        surface CVODE solver (PRECONDITIONER=AMG).
 *
 * @details Wraps a hypre IJ/ParCSR matrix + BoomerAMG hierarchy behind the
 *          CVODE CVodeSetPreconditioner(psetup, psolve) surface — the same
 *          surface the Jacobi heuristic uses, so the linear-solver stack is
 *          unchanged: GMRES stays matrix-free (finite-difference J·v) for the
 *          true operator, and this preconditioner inverts the *assembled*
 *          approximate Newton matrix M = I − γ·J (the diffusion stencil from
 *          SurfaceJacobian). One BoomerAMG V-cycle per psolve gives
 *          near-mesh-independent Krylov iteration counts.
 *
 *          hypre is called single-process (sequential build, MPI_COMM_WORLD =
 *          mpistubs no-op). Only compiled when OPENSWMM_WITH_HYPRE is ON; the
 *          header is hypre-free (opaque handles) so it is safe to include from
 *          guarded code without leaking HYPRE.h.
 *
 * @ingroup engine_2d
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_ENGINE_2D_HYPRE_AMG_PRECONDITIONER_HPP
#define OPENSWMM_ENGINE_2D_HYPRE_AMG_PRECONDITIONER_HPP

#include "SurfaceJacobian.hpp"

#include <vector>

namespace openswmm::twoD {

class MeshData;
class SurfaceStateData;

/**
 * @brief BoomerAMG preconditioner over the 2D diffusion Newton matrix.
 */
class HypreAmgPreconditioner {
public:
    HypreAmgPreconditioner() = default;
    ~HypreAmgPreconditioner();

    HypreAmgPreconditioner(const HypreAmgPreconditioner&)            = delete;
    HypreAmgPreconditioner& operator=(const HypreAmgPreconditioner&) = delete;

    /// Build the static CSR sparsity and allocate the hypre IJ matrix/vectors
    /// for an n-cell mesh. Call once per solver initialize().
    void initialize(const MeshData& mesh);

    /// Assemble M = I − γ·J from the current state and (re)build the AMG
    /// hierarchy. CVODE decides the cadence (lagged) via its psetup policy.
    ///
    /// When @p recompute is false AND a hierarchy already exists this is a
    /// no-op: the previously built matrix + multigrid hierarchy are reused
    /// (CVODE passed jok == SUNTRUE — the saved Jacobian is still current and
    /// only γ drifted). GMRES preconditions with the true matrix-free operator,
    /// so a slightly stale hierarchy only affects the Krylov iteration count,
    /// never the converged solution. The first call always builds.
    void setup(const MeshData& mesh, const SurfaceStateData& state, double gamma,
               bool recompute = true, const double* deta_dv = nullptr);

    /// Apply one BoomerAMG V-cycle: z ≈ M⁻¹ r  (n entries each).
    void solve(const double* r, double* z, int n);

    /// Release all hypre objects.
    void finalize();

    bool ready() const noexcept { return amg_ != nullptr; }

    /// True once the multigrid hierarchy has been built at least once.
    bool hierarchyBuilt() const noexcept { return hierarchy_built_; }

    /// Drop the lagged-reuse cache: the next setup() rebuilds the matrix and
    /// hierarchy regardless of the recompute flag. Call after the solver state
    /// is re-seeded (CVodeReInit).
    void invalidate() noexcept { hierarchy_built_ = false; }

private:
    SurfaceJacobian   jac_;
    int               n_ = 0;
    bool              hierarchy_built_ = false;  ///< set by the first setup()
    std::vector<int>  rows_;     ///< 0..n−1 global row ids (sequential)
    std::vector<int>  ncols_;    ///< entries per row (for IJMatrixSetValues)

    // Opaque hypre handles (HYPRE_IJMatrix / HYPRE_IJVector / HYPRE_Solver are
    // pointer typedefs; kept as void* so this header needs no HYPRE.h).
    void* A_   = nullptr;        ///< HYPRE_IJMatrix
    void* b_   = nullptr;        ///< HYPRE_IJVector (rhs)
    void* x_   = nullptr;        ///< HYPRE_IJVector (solution)
    void* amg_ = nullptr;        ///< HYPRE_Solver (BoomerAMG)
};

} // namespace openswmm::twoD

#endif // OPENSWMM_ENGINE_2D_HYPRE_AMG_PRECONDITIONER_HPP
