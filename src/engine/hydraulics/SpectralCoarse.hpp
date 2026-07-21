/**
 * @file SpectralCoarse.hpp
 * @brief Two-level spectral coarse correction for the DW Picard solver.
 *
 * @details Builds a graph Laplacian from the SWMM conduit topology, computes
 *          the k smallest nontrivial eigenvectors via Lanczos (reorthogonalized),
 *          and uses them as a coarse space to accelerate convergence of the
 *          nodal head/depth Picard iteration.
 *
 *          This file is self-contained: it depends only on the C++ standard
 *          library and is compiled into the engine via SpectralCoarse.cpp.
 *          It is NOT part of the public API — do not include from public headers.
 *
 * @ingroup new_engine
 */

#ifndef OPENSWMM_SPECTRAL_COARSE_HPP
#define OPENSWMM_SPECTRAL_COARSE_HPP

#include <vector>
#include <cstdint>

namespace openswmm {
namespace dynwave {

// ============================================================================
// Minimal CSR sparse matrix (private to spectral module)
// ============================================================================

struct CsrMatrix {
    int n = 0;                   ///< Square dimension
    std::vector<int>    row_ptr; ///< size n+1; row_ptr[i]..row_ptr[i+1]-1 = row i entries
    std::vector<int>    col_idx; ///< column indices
    std::vector<double> values;  ///< nonzero values

    int nnz() const { return static_cast<int>(values.size()); }
    bool empty() const { return n == 0; }
};

/// y += A * x  (no aliasing required between y and x)
void csr_matvec(const CsrMatrix& A, const double* x, double* y);

/// Add value v symmetrically: A[i][j] += v and A[j][i] += v.
/// Used during Laplacian construction — assumes COO-style accumulation before
/// converting to CSR. Not used after CSR is finalized.
void csr_add_entry(std::vector<std::vector<std::pair<int,double>>>& coo,
                   int i, int j, double v);

/// Convert COO (vector-of-vectors) to CSR. Sorts columns, sums duplicates.
CsrMatrix coo_to_csr(std::vector<std::vector<std::pair<int,double>>>& coo, int n);

// ============================================================================
// SpectralCoarse — two-level coarse correction state
// ============================================================================

/**
 * @brief Spectral coarse correction for the DW Picard solver.
 *
 * Lifecycle:
 *   1. Call init() once after network topology is loaded (in DWSolver::init()).
 *   2. Before each coarse correction attempt:
 *        a. Call assembleCoarseOperator() with current dqdh/surf_area state.
 *        b. Call applyCorrection() with the current depth residual.
 *   3. If applyCorrection() returns > 0, add the correction to nodes.depth
 *      (and update nodes.head) for all active_map[i] nodes.
 *
 * All state is instance-scoped; no global mutable memory.
 */
struct SpectralCoarse {

    // -------------------------------------------------------------------------
    // Configuration (set before init())
    // -------------------------------------------------------------------------

    int    num_modes      = 10;    ///< Nontrivial coarse modes to retain
    double null_tol       = 1e-8;  ///< Eigenvalue threshold for null-mode discard
    double accept_tol     = 0.01;  ///< Fractional residual non-increase tolerance

    // -------------------------------------------------------------------------
    // Dimensions (set by init())
    // -------------------------------------------------------------------------

    int N          = 0;   ///< Full node count (including outfalls)
    int n_active   = 0;   ///< Active (non-outfall) node count
    int num_kept   = 0;   ///< Nontrivial modes actually retained (<= num_modes)
    int num_null   = 0;   ///< Null modes detected (eigenvalue < null_tol)

    // -------------------------------------------------------------------------
    // Index maps (set by init())
    // -------------------------------------------------------------------------

    /// active_map[i] = full node index for active node i   (length n_active)
    std::vector<int> active_map;

    /// full_to_active[j] = active index for full node j; -1 if outfall/excluded
    ///   (length N)
    std::vector<int> full_to_active;

    // -------------------------------------------------------------------------
    // Eigenbasis (set by init())
    // -------------------------------------------------------------------------

    /// Retained nontrivial eigenvalues, sorted ascending (length num_kept)
    std::vector<double> eigenvalues;

    /// Prolongation matrix P: n_active × num_kept, column-major.
    /// Column j = j-th coarse basis vector (eigenvector of graph Laplacian).
    std::vector<double> P;

    // -------------------------------------------------------------------------
    // Coarse operator (set by assembleCoarseOperator())
    // -------------------------------------------------------------------------

    /// A_coarse = P^T * A_fine * P  (num_kept × num_kept dense, column-major)
    std::vector<double> A_coarse;

    /// LU factorization workspace (copy of A_coarse after factoring)
    std::vector<double> A_coarse_lu;

    /// LU pivot indices (length num_kept)
    std::vector<int> A_coarse_piv;

    /// Intermediate: W = A_fine * P  (n_active × num_kept, column-major)
    std::vector<double> W_work;

    // -------------------------------------------------------------------------
    // Working vectors
    // -------------------------------------------------------------------------

    std::vector<double> work_coarse;  ///< length num_kept
    std::vector<double> work_fine;    ///< length n_active

    // -------------------------------------------------------------------------
    // State and diagnostics
    // -------------------------------------------------------------------------

    double eigengap        = 0.0;  ///< Ratio eigenvalue[1] / eigenvalue[0] (quality indicator)
    int    enabled         = 0;    ///< 1 if init succeeded and correction is usable
    int    needs_refresh   = 0;    ///< Reserved for future eigenbasis refresh; never set here
    int    refresh_count   = 0;    ///< Reserved
    int    last_error      = 0;    ///< 0=ok, 1=too few nodes, 2=eigensolver failed,
                                   ///<       3=factorization failed, 4=too few modes

    /// Per-timestep correction diagnostics (reset by reset())
    int corrections_attempted = 0;
    int corrections_accepted  = 0;
    int corrections_rejected  = 0;

    // -------------------------------------------------------------------------
    // Public methods
    // -------------------------------------------------------------------------

    /**
     * @brief Build eigenbasis from network topology (called once in DWSolver::init()).
     *
     * @param N_full         Total number of nodes (including outfalls)
     * @param n_conduits     Number of conduit links
     * @param is_outfall     Array of length N_full; 1 if node is an outfall, 0 otherwise
     * @param conduit_n1     Array of length n_conduits; upstream node index for conduit k
     * @param conduit_n2     Array of length n_conduits; downstream node index for conduit k
     * @param num_modes_req  Number of nontrivial modes to retain (will be clamped)
     * @return true on success (enabled=1); false if spectral correction cannot be used
     */
    bool init(int N_full, int n_conduits,
              const int* is_outfall,
              const int* conduit_n1,
              const int* conduit_n2,
              int num_modes_req);

    /**
     * @brief Assemble and factor the coarse operator A_coarse = P^T * A_fine * P.
     *
     * Must be called after each pre-smoothing sweep (Steps 1-5 of Picard),
     * before applyCorrection(), using fresh dqdh/surf_area from the current iterate.
     *
     * @param active_diag     Diagonal of A_fine per active node:
     *                        new_surf_area[active_map[i]] + 0.5*dt*sumdqdh[active_map[i]].
     *                        The sign of sumdqdh must be verified (see implementation notes).
     *                        Length n_active.
     * @param n_conduits      Number of conduits
     * @param conduit_n1      Full node indices, length n_conduits
     * @param conduit_n2      Full node indices, length n_conduits
     * @param conduit_off     0.5*dt*dqdh[k] for conduit k (positive conductance weight).
     *                        Length n_conduits.
     * @return true if factorization succeeded; false if A_coarse is singular/ill-conditioned
     */
    bool assembleCoarseOperator(const double* active_diag,
                                int n_conduits,
                                const int* conduit_n1,
                                const int* conduit_n2,
                                const double* conduit_off);

    /**
     * @brief Apply one coarse correction with backtracking line search.
     *
     * Computes: r_coarse = P^T * r, solves A_coarse * e = r_coarse,
     * then delta = P * e. Tries omega = 1.0, 0.5, 0.25 until the
     * max-norm depth residual decreases.
     *
     * @param depth_resid    Per-active-node depth residual (g_k - y_last), length n_active
     * @param correction_out Per-active-node depth correction to apply, length n_active.
     *                       Zeroed and filled by this function.
     * @param head_tol       Convergence tolerance (same value used by updateNodeDepths)
     * @return Damping factor used (1.0, 0.5, or 0.25) if accepted; 0.0 if rejected
     */
    double applyCorrection(const double* depth_resid,
                           double* correction_out,
                           double head_tol);

    /// Reset per-timestep diagnostics. Call at the start of each DW timestep.
    void reset();

private:
    // Laplacian construction
    CsrMatrix buildTopologicalLaplacian(int n_active, int n_conduits,
                                        const int* conduit_n1,
                                        const int* conduit_n2) const;

    // Lanczos eigensolver: computes the m smallest eigenpairs of L.
    // L must be symmetric PSD. Starting vector is 1/sqrt(n).
    // eigvals_out: sorted ascending, length k_want
    // eigvecs_out: n × k_want column-major
    // Returns false if eigensolver fails to converge.
    bool lanczos(const CsrMatrix& L, int k_want,
                 std::vector<double>& eigvals_out,
                 std::vector<double>& eigvecs_out);

    // Symmetric tridiagonal QL eigensolver (LAPACK tqli style).
    // diag:    length m, in/out (eigenvalues on return)
    // subdiag: length m, in (subdiag[0] unused; subdiag[j] = beta_{j} for j=1..m-1)
    // Z:       m×m column-major eigenvector matrix (in: identity; out: eigenvectors)
    // Returns false if any eigenvalue fails to converge within max_iter sweeps.
    static bool symTridiagQL(std::vector<double>& diag,
                             std::vector<double>& subdiag,
                             std::vector<double>& Z,
                             int m);

    // Dense LU factorization with partial pivoting (in-place).
    // A is n×n column-major. piv is length n.
    // Returns false if matrix is singular (pivot < 1e-14).
    static bool luFactor(std::vector<double>& A, std::vector<int>& piv, int n);

    // LU solve: solves A*x=b in-place in b, using factors from luFactor.
    static void luSolve(const std::vector<double>& A,
                        const std::vector<int>& piv,
                        std::vector<double>& b, int n);
};

} // namespace dynwave
} // namespace openswmm

#endif // OPENSWMM_SPECTRAL_COARSE_HPP
