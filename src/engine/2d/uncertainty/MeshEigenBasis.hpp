/**
 * @file MeshEigenBasis.hpp
 * @brief Geometric graph-Laplacian eigenbasis of a 2D triangular mesh.
 *
 * @details Builds the k smallest non-trivial eigenmodes of the mesh's geometric
 *          graph Laplacian (edge conductance w_ij = edge_length / centroid
 *          distance) and stores them as a column-major prolongation matrix P
 *          with the matching eigenvalues Λ.  This is the reduced basis the 2D
 *          uncertainty ROM (SpectralROM) and the Fiedler diagnostic project
 *          onto.
 *
 *          The basis depends on mesh geometry alone (plus, for the
 *          depth-weighted variant, a supplied per-cell diffusivity).  It reads
 *          no solver state and is independent of the 2D integrator, so it is
 *          built once when mesh topology is finalized and reused for the whole
 *          run.
 *
 *          The Lanczos + QL eigensolve is delegated to
 *          `uncertainty::GraphEigenBasis`, the same backend the 1D network ROM
 *          uses — this class contributes the mesh-specific Laplacian assembly
 *          and nothing else.  One eigensolver implementation, two dimensions.
 *
 *          Cost: O(n · k · m) with m = O(k), once per basis build.
 *
 * @ingroup engine_2d
 */

#ifndef OPENSWMM_ENGINE_2D_MESH_EIGEN_BASIS_HPP
#define OPENSWMM_ENGINE_2D_MESH_EIGEN_BASIS_HPP

#include "../data/MeshData.hpp"
#include "../../uncertainty/GraphEigenBasis.hpp"

#include <vector>

namespace openswmm::twoD {

// ============================================================================
// MeshEigenBasis
// ============================================================================

/**
 * @brief Eigenbasis of the mesh geometric graph Laplacian.
 *
 * Lifecycle:
 *   1. Call build() once after mesh topology is finalized (buildMeshTopology).
 *   2. Optionally call buildDepthWeighted() once a depth field exists, to
 *      re-solve with depth-dependent edge conductances.
 *   3. Point consumers (SpectralROM::basis, FiedlerDiagnostic::basis) at it.
 *
 * The basis is a function of mesh geometry only (plus, for the depth-weighted
 * variant, a supplied per-cell diffusivity) — it is independent of the 2D
 * integrator and never reads solver internals.
 */
struct MeshEigenBasis {

    // ---- configuration (set before build()) ----

    int    num_modes = 10;        ///< Modes requested; build() records the last value used.
    double null_tol  = 1.0e-8;    ///< Eigenvalue threshold for null-mode discard.

    // ---- set by build() ----

    int  n_triangles    = 0;
    int  num_kept       = 0;      ///< modes actually retained (may be < num_modes)
    int  num_null       = 0;      ///< near-zero modes discarded (graph null space)
    bool depth_weighted = false;  ///< true after buildDepthWeighted() succeeds

    std::vector<double> P;            ///< n_triangles × num_kept, col-major
    std::vector<double> eigenvalues;  ///< length num_kept, ascending

    /// true after build() with area_weights: solves L v = lambda A v, P is
    /// A-orthonormal (P^T A P = I) and eigenvalues carry 1/area units.
    bool area_weighted = false;
    std::vector<double> area;         ///< A = diag(area), length n_triangles (empty unless area_weighted)
    std::vector<double> inv_sqrt_area;///< A^{-1/2} (empty unless area_weighted)

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
     * @param ground_w Optional per-cell grounding conductance (length
     *        n_triangles, same dimensionless len/d convention as interior
     *        edges; 0 for cells with no open boundary). Grounding adds the
     *        weight to the cell's diagonal only — an edge to a zero-deviation
     *        ghost across an OPEN boundary face (outlet, specified stage).
     *        Physically: deviations flush out of the domain there, so the
     *        operator is not pure Neumann; the constant vector stops being a
     *        null mode and the basis gains a quasi-uniform drain mode. Without
     *        it a domain-wide parameter shift — whose steady response is
     *        mostly a uniform profile offset — projects to almost nothing on
     *        the zero-mean modes and the ensemble spread collapses. This is
     *        the mesh analogue of NetworkLaplacian1D's outfall grounding, and
     *        it exists for the same reason. Null = pure Neumann (closed box).
     *
     * @param area_weights Optional per-cell weights A (length n_triangles, all
     *        > 0, normally cell areas). Non-null solves the generalized problem
     *        L v = lambda A v through A^{-1/2} L A^{-1/2} (the H18 trick of
     *        GraphEigenBasis::buildGeneralized): P = A^{-1/2} V is
     *        A-orthonormal. The scaled matrix is divided by
     *        rho = mean(diag L)/mean(A) before the solve and eigenvalues are
     *        multiplied back, so null_tol keeps its meaning. Null = today's
     *        plain orthonormal basis, bit-identical.
     *
     * @return true on success (num_kept > 0); false otherwise.
     */
    bool build(const MeshData& mesh, int num_modes_req,
               const double* ground_w = nullptr,
               const double* area_weights = nullptr);

    /// out[j] = (P^T A field)_j, j < num_kept. Requires area_weighted
    /// (otherwise A = I, i.e. the plain projection P^T field).
    void projectArea(const double* field, double* out) const;

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
     * @note No area_weights argument: the depth-weighted rebuild stays the
     *       plain orthonormal basis (and clears area_weighted on success).
     * @return true on success; false if Lanczos fails (basis unchanged).
     */
    bool buildDepthWeighted(const MeshData& mesh, int num_modes_req,
                            const double* D_cell,
                            const double* ground_w = nullptr);

    /// true if a build succeeded and modes were retained.
    bool is_ready() const noexcept { return num_kept > 0 && !P.empty(); }

private:
    uncertainty::CsrGraph buildGeometricLaplacian(
        const MeshData& mesh, const double* ground_w) const;
    uncertainty::CsrGraph buildDepthWeightedLaplacian(
        const MeshData& mesh, const double* D_cell,
        const double* ground_w) const;

    /// Shared eigensolve + null-mode filtering + P/eigenvalues population.
    /// On failure returns false and leaves P/eigenvalues unchanged.
    bool buildFromLaplacian(const uncertainty::CsrGraph& L, int num_modes_req,
                            const double* area_weights = nullptr);
};

} // namespace openswmm::twoD

#endif // OPENSWMM_ENGINE_2D_MESH_EIGEN_BASIS_HPP
