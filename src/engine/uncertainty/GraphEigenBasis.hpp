/**
 * @file GraphEigenBasis.hpp
 * @brief Generic graph Laplacian eigenbasis via reorthogonalized Lanczos.
 *
 * @details Provides a shared spectral backend for uncertainty propagation in
 *          both 1D pipe networks and 2D surface meshes.  Given any symmetric
 *          positive-semi-definite CSR Laplacian, computes the k smallest
 *          nontrivial eigenpairs and stores them in column-major matrices.
 *
 *          The same Lanczos + QL implementation backs both SpectralROM1D
 *          (1D pipe network ROM) and MeshEigenBasis (2D surface mesh); each
 *          supplies its own Laplacian assembly and shares this eigensolver.
 *
 *          Typical use:
 *          @code
 *          CsrGraph L = buildMyLaplacian(...);
 *          GraphEigenBasis basis;
 *          basis.null_tol = 1e-8;
 *          if (basis.build(L, 10)) {
 *              // basis.P[j * n + i] = i-th component of mode j
 *              // basis.eigenvalues[j] = λ_j, ascending
 *          }
 *          @endcode
 *
 * @ingroup engine_uncertainty
 */

#ifndef OPENSWMM_ENGINE_UNCERTAINTY_GRAPH_EIGEN_BASIS_HPP
#define OPENSWMM_ENGINE_UNCERTAINTY_GRAPH_EIGEN_BASIS_HPP

#include <vector>
#include <utility>

namespace openswmm::uncertainty {

// ============================================================================
// CsrGraph — minimal symmetric CSR sparse matrix
// ============================================================================

/**
 * @brief Symmetric sparse matrix in Compressed Sparse Row format.
 *
 * Used to represent graph Laplacians.  Stores the full symmetric matrix
 * (both upper and lower triangular entries) so that matvec can be done
 * as a simple row scan without special-casing symmetry.
 */
struct CsrGraph {
    int n = 0;                   ///< Square dimension (number of nodes)
    std::vector<int>    row_ptr; ///< size n+1; row_ptr[i]..row_ptr[i+1]-1 = row i
    std::vector<int>    col_idx; ///< column indices of nonzeros
    std::vector<double> values;  ///< nonzero values

    int  nnz()   const noexcept { return static_cast<int>(values.size()); }
    bool empty() const noexcept { return n == 0; }
};

/// y += A * x  (no aliasing requirement between y and x)
void csr_matvec(const CsrGraph& A, const double* x, double* y);

/**
 * @brief Convert a coordinate list (vector-of-vectors) to CSR.
 *
 * Sorts columns within each row and merges duplicate entries by summation.
 * @param coo  COO accumulator: coo[i] = list of (col, val) pairs for row i.
 * @param n    Matrix dimension.
 */
CsrGraph coo_to_csr(std::vector<std::vector<std::pair<int,double>>>& coo, int n);

// ============================================================================
// GraphEigenBasis
// ============================================================================

/**
 * @brief Eigenbasis of a graph Laplacian (k smallest nontrivial eigenpairs).
 *
 * Algorithm:
 *   1. Reorthogonalized Lanczos with zero-mean linear-ramp start vector.
 *   2. Symmetric tridiagonal QL eigensolver (Numerical Recipes tqli).
 *   3. Null-mode filtering: discard eigenpairs with λ < null_tol.
 *   4. Store the first num_modes_req surviving eigenpairs in P / eigenvalues.
 *
 * The start vector is a zero-mean linear ramp so it is orthogonal to the
 * Laplacian null space (constant vector = 1/√n), preventing the immediate
 * breakdown that would occur with a constant start vector.
 *
 * Thread safety: build() modifies member state; is NOT thread-safe.
 *                 All query-only members (P, eigenvalues, n_nodes, num_kept)
 *                 are safe to read from multiple threads after build() returns.
 */
struct GraphEigenBasis {

    // ---- configuration (set before build()) ----

    double null_tol = 1.0e-8;  ///< Eigenvalue threshold for null-mode discard.

    // ---- set by build() ----

    int n_nodes  = 0;  ///< Graph size (n).
    int num_kept = 0;  ///< Nontrivial modes retained (≤ num_modes_req).
    int num_null = 0;  ///< Null modes discarded (eigenvalue < null_tol).

    /**
     * @brief Prolongation matrix P: n_nodes × num_kept, column-major.
     *
     * P[j * n_nodes + i] = i-th component of the j-th eigenvector.
     * Columns are orthonormal: P^T P = I_{num_kept}.
     */
    std::vector<double> P;

    /**
     * @brief Retained eigenvalues in ascending order (length num_kept).
     *
     * eigenvalues[0] is the smallest nontrivial eigenvalue of the Laplacian.
     */
    std::vector<double> eigenvalues;

    int last_error = 0;  ///< 0=ok, 1=graph too small, 2=eigensolver failed

    // ---- public methods ----

    /**
     * @brief Build the eigenbasis from a symmetric PSD Laplacian.
     *
     * @param L              Symmetric PSD CSR Laplacian of the graph.
     * @param num_modes_req  Number of nontrivial eigenmodes to retain.
     * @param v0_block       Optional warm-start block: column-major
     *                       n_nodes × num_modes_req matrix of starting vectors
     *                       (e.g., the previous eigenvectors P).  When non-null,
     *                       the first column is used as the Krylov starting vector
     *                       instead of the cold-start linear ramp.  Sign alignment
     *                       is applied after the solve so that each returned
     *                       eigenvector has a positive inner product with the
     *                       corresponding column of v0_block.
     *                       null → cold-start (original behaviour, unchanged).
     * @return true on success (num_kept > 0); false otherwise (last_error set).
     */
    bool build(const CsrGraph& L, int num_modes_req,
               const double* v0_block = nullptr);

    /// true if build() succeeded and num_kept > 0.
    bool is_ready() const noexcept { return num_kept > 0 && !P.empty(); }

private:
    bool lanczos(const CsrGraph& L, int k_want,
                 std::vector<double>& eigvals_out,
                 std::vector<double>& eigvecs_out,
                 const double* v0 = nullptr);

    static bool symTridiagQL(std::vector<double>& diag,
                             std::vector<double>& subdiag,
                             std::vector<double>& Z, int m);
};

} // namespace openswmm::uncertainty

#endif // OPENSWMM_ENGINE_UNCERTAINTY_GRAPH_EIGEN_BASIS_HPP
