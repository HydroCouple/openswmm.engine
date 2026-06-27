/**
 * @file SurfaceJacobian.hpp
 * @brief CSR assembly of the approximate Newton matrix M = I − γ·J for the
 *        2D diffusive-wave surface operator, for use as a preconditioner
 *        matrix (hypre BoomerAMG).
 *
 * @details The Jacobi preconditioner keeps only diag(J); AMG needs the full
 *          near-symmetric M-matrix. Both use the SAME per-edge transmissivity
 *          the Jacobi psetup computes:
 *
 *            T_e   = |edge_flux_e| / max(|h_i − h_j|, dh_floor)
 *            J_ii  = −(1/A_i) Σ_e T_e        (diagonal, == the Jacobi heuristic)
 *            J_ij  = +(1/A_i) T_e            (off-diagonal, neighbour j over e)
 *
 *          so the Newton matrix is
 *            M_ii  = 1 + (γ/A_i) Σ_e T_e
 *            M_ij  = −(γ/A_i) T_e.
 *
 *          The sparsity is fixed by the static mesh topology (diagonal + up to
 *          three neighbours per row), built once; only the values are refreshed
 *          each preconditioner setup.
 *
 * @ingroup engine_2d
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_ENGINE_2D_SURFACE_JACOBIAN_HPP
#define OPENSWMM_ENGINE_2D_SURFACE_JACOBIAN_HPP

#include "../data/MeshData.hpp"
#include "../data/SurfaceStateData.hpp"

#include <vector>

namespace openswmm::twoD {

/**
 * @brief Assembles M = I − γ·J (diffusion stencil) in CSR for AMG.
 */
class SurfaceJacobian {
public:
    /// Build the static CSR sparsity from the mesh topology. Each row holds the
    /// diagonal followed by one entry per distinct non-boundary neighbour
    /// (duplicate neighbours, should they occur, are merged). Call once after
    /// the topology is final; values start zeroed.
    void buildSparsity(const MeshData& mesh);

    /// Refresh the CSR values for M = I − γ·J from the current head/edge_flux.
    /// Cheap: writes only the precomputed entry positions, no reallocation.
    void assemble(const MeshData& mesh, const SurfaceStateData& state,
                  double gamma, double dh_floor = 1.0e-9);

    int           rows()   const noexcept { return n_; }
    int           nnz()    const noexcept { return static_cast<int>(col_idx_.size()); }
    const int*    rowPtr() const noexcept { return row_ptr_.data(); }
    const int*    colIdx() const noexcept { return col_idx_.data(); }
    const double* values() const noexcept { return values_.data(); }
    double*       values()       noexcept { return values_.data(); }

private:
    int                 n_ = 0;
    std::vector<int>    row_ptr_;        ///< size n_+1
    std::vector<int>    col_idx_;        ///< size nnz
    std::vector<double> values_;         ///< size nnz (M = I − γJ)
    std::vector<int>    diag_pos_;       ///< per row: index into values_ of M_ii
    std::vector<int>    edge_pos_;       ///< per (cell*3+edge): index of M_ij, −1 if boundary
};

} // namespace openswmm::twoD

#endif // OPENSWMM_ENGINE_2D_SURFACE_JACOBIAN_HPP
