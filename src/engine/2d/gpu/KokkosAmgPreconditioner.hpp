/**
 * @file KokkosAmgPreconditioner.hpp
 * @brief hypre BoomerAMG preconditioner for the Kokkos GPU plugin
 *        (PRECONDITIONER=AMG) — host (OpenMP) and device (CUDA/HIP) paths.
 *
 * @details Mirrors the serial HypreAmgPreconditioner but assembles M = I − γ·J
 *          directly from the plugin's Kokkos mesh/state Views (a Kokkos kernel
 *          that runs in the active ExecSpace), and drives hypre in the matching
 *          memory location:
 *            - OpenMP backend  → MemSpace is host  → HYPRE_MEMORY_HOST / EXEC_HOST
 *            - CUDA/HIP backend → MemSpace is device→ HYPRE_MEMORY_DEVICE / EXEC_DEVICE
 *          so the IJ matrix/vectors and the BoomerAMG V-cycle live in the same
 *          space as the Kokkos N_Vector data, avoiding host↔device round-trips
 *          inside the linear solve. Only compiled when OPENSWMM_WITH_HYPRE is ON.
 *
 *          The static CSR sparsity (diagonal + up to three neighbours per row)
 *          is built once on the host from the mesh topology and mirrored to the
 *          ExecSpace; only the values are refreshed each setup.
 *
 * @ingroup engine_2d_gpu
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_ENGINE_2D_GPU_KOKKOS_AMG_PRECONDITIONER_HPP
#define OPENSWMM_ENGINE_2D_GPU_KOKKOS_AMG_PRECONDITIONER_HPP

#include "KokkosSurfaceKernels.hpp"

namespace openswmm::twoD::gpu {

/**
 * @brief BoomerAMG preconditioner over the Kokkos surface Newton matrix.
 *
 * Lives entirely in the plugin's ExecSpace/MemSpace. Construct once, then per
 * CVODE psetup call setup(), per psolve call solve().
 */
class KokkosAmgPreconditioner {
public:
    KokkosAmgPreconditioner() = default;
    ~KokkosAmgPreconditioner();

    KokkosAmgPreconditioner(const KokkosAmgPreconditioner&)            = delete;
    KokkosAmgPreconditioner& operator=(const KokkosAmgPreconditioner&) = delete;

    /// Build static CSR sparsity from the mesh topology + allocate hypre objects
    /// in the active memory location. Call once per solver initialize().
    void initialize(const MeshViews& mesh);

    /// Assemble M = I − γ·J from the current Views and (re)build the hierarchy.
    /// When `recompute` is false and a hierarchy exists, the prior matrix +
    /// hierarchy are reused verbatim (CVODE's lagged-preconditioner policy —
    /// mirrors the serial HypreAmgPreconditioner).
    void setup(const MeshViews& mesh, const StateViews& state, double gamma,
               bool recompute);

    /// Apply one BoomerAMG V-cycle: z ≈ M⁻¹ r (Views in the plugin's MemSpace).
    void solve(DView r, DView z, double gamma);

    void finalize();
    bool ready() const noexcept { return amg_ != nullptr; }

    /// Drop the lagged-reuse cache: the next setup() rebuilds the matrix and
    /// hierarchy regardless of the recompute flag. Call after the solver state
    /// is re-seeded (CVodeReInit).
    void invalidate() noexcept { hierarchy_built_ = false; }

private:
    int n_ = 0;
    bool hierarchy_built_ = false;  ///< set by the first (re)build in setup()

    // Static CSR sparsity, resident in the ExecSpace memory space.
    IView  row_ptr_;     ///< [n+1]
    IView  col_idx_;     ///< [nnz]
    IView  rows_;        ///< [n] global row ids 0..n−1
    IView  ncols_;       ///< [n] entries per row
    IView  diag_pos_;    ///< [n] index into values_ of M_ii
    IView  edge_pos_;    ///< [n*3] index of M_ij (−1 if boundary)
    DView  values_;      ///< [nnz] matrix entries (M = I − γJ)

    // Opaque hypre handles (see HypreAmgPreconditioner.hpp rationale).
    void* A_   = nullptr;   ///< HYPRE_IJMatrix
    void* b_   = nullptr;   ///< HYPRE_IJVector
    void* x_   = nullptr;   ///< HYPRE_IJVector
    void* amg_ = nullptr;   ///< HYPRE_Solver (BoomerAMG)
};

} // namespace openswmm::twoD::gpu

#endif // OPENSWMM_ENGINE_2D_GPU_KOKKOS_AMG_PRECONDITIONER_HPP
