/**
 * @file SpectralPrecond2D.hpp
 * @brief Spectral Laplacian preconditioner for the 2D CVODE surface solver.
 *
 * @details Builds the k smallest non-trivial eigenmodes of the geometric graph
 *          Laplacian of the triangular mesh via reorthogonalized Lanczos, then
 *          applies the spectral projection preconditioner
 *
 *              M_k^{-1}(gamma) r = r + P * diag(s_j - 1) * P^T * r
 *
 *          where s_j = 1 / (1 + gamma * lambda_j), to approximate the inverse
 *          of the BDF Newton operator (I + gamma * L) in the k-dimensional
 *          low-frequency subspace.
 *
 *          Cost: build — O(n * k * m) Lanczos with m = O(k).
 *                apply — O(n * k) per CVODE linear iteration.
 *
 * @ingroup engine_2d
 */

#ifndef OPENSWMM_ENGINE_2D_SPECTRAL_PRECOND_2D_HPP
#define OPENSWMM_ENGINE_2D_SPECTRAL_PRECOND_2D_HPP

#include "../data/MeshData.hpp"
#include <vector>

namespace openswmm::twoD {

// ============================================================================
// Minimal CSR matrix — private to spectral 2D utilities
// ============================================================================

struct Csr2D {
    int n = 0;
    std::vector<int>    row_ptr;
    std::vector<int>    col_idx;
    std::vector<double> values;
};

// ============================================================================
// SpectralPrecond2D
// ============================================================================

/**
 * @brief Spectral preconditioner for the 2D CVODE surface routing solver.
 *
 * Lifecycle:
 *   1. Set num_modes (default 10).
 *   2. Call build() once after mesh topology is finalized (in
 *      CvodeSurfaceSolver::initialize()).
 *   3. Pass apply() to CVODE via CVodeSetPreconditioner.
 */
struct SpectralPrecond2D {

    // ---- configuration (set before build()) ----
    int num_modes = 10;

    // ---- set by build() ----
    int n_triangles   = 0;
    int num_kept      = 0;    ///< modes actually retained (may be < num_modes)
    int num_null      = 0;    ///< near-zero modes discarded (graph null space)
    bool depth_weighted = false;  ///< true after buildDepthWeighted() succeeds

    std::vector<double> P;            ///< n_triangles × num_kept, col-major
    std::vector<double> eigenvalues;  ///< length num_kept, ascending

    // ---- status ----
    int last_error = 0;   ///< 0=ok, 1=too few cells, 2=eigensolver failed

    /**
     * @brief Build eigenbasis from mesh geometry (geometric weights).
     *
     * Constructs the edge-length–weighted geometric graph Laplacian from the
     * triangle neighbour adjacency (tri_nbr0/1/2), runs reorthogonalized
     * Lanczos to obtain the num_modes smallest non-trivial eigenpairs, and
     * stores the result in P and eigenvalues.
     *
     * @return true on success (num_kept > 0); false otherwise.
     */
    bool build(const MeshData& mesh, int num_modes_req);

    /**
     * @brief Rebuild eigenbasis using depth-weighted edge conductances.
     *
     * Replaces the geometric edge weight len/d with D_ij * len/d, where
     * D_ij is the harmonic mean of D_cell[i] and D_cell[j].  Typically
     * D_cell[t] = h[t]^(5/3) (depth-based diffusivity, scale-invariant for
     * eigenvectors).  If the Lanczos solve fails, the existing basis (built
     * by build()) is preserved unchanged.
     *
     * Call once after the initial depth field is known (e.g. before the first
     * ROM advance step).
     *
     * @param mesh          Mesh geometry.
     * @param num_modes_req Number of modes to retain.
     * @param D_cell        Per-triangle diffusivity (length n_triangles).
     * @return true on success; false if Lanczos fails (basis unchanged).
     */
    bool buildDepthWeighted(const MeshData& mesh, int num_modes_req,
                            const double* D_cell);

    /**
     * @brief Apply spectral preconditioner: z = M_k^{-1}(gamma) * r.
     *
     * z = r + P * diag(1/(1+gamma*lambda_j) - 1) * (P^T * r)
     *
     * @param r     Input (length n_triangles)
     * @param z     Output (length n_triangles)
     * @param gamma BDF coefficient x step size (provided by CVODE psolve)
     */
    void apply(const double* r, double* z, double gamma) const;

private:
    mutable std::vector<double> c_buf_;  ///< working buffer, length num_kept

    Csr2D buildGeometricLaplacian(const MeshData& mesh) const;
    Csr2D buildDepthWeightedLaplacian(const MeshData& mesh,
                                      const double* D_cell) const;

    /// Shared Lanczos + null-mode filtering + P/eigenvalues population.
    /// On failure returns false and leaves P/eigenvalues unchanged.
    bool buildFromLaplacian(Csr2D L, int num_modes_req);

    bool lanczos(const Csr2D& L, int k_want,
                 std::vector<double>& eigvals_out,
                 std::vector<double>& eigvecs_out);

    // Symmetric tridiagonal QL (Numerical Recipes tqli, with ascending sort).
    // diag/subdiag modified in place; Z accumulates eigenvectors (col-major).
    static bool symTridiagQL(std::vector<double>& diag,
                             std::vector<double>& subdiag,
                             std::vector<double>& Z, int m);
};

} // namespace openswmm::twoD

#endif // OPENSWMM_ENGINE_2D_SPECTRAL_PRECOND_2D_HPP
