/**
 * @file SurfaceJacobian.cpp
 * @brief Implementation of the CSR Newton-matrix assembly.
 *
 * @see SurfaceJacobian.hpp
 * @ingroup engine_2d
 */

#include "SurfaceJacobian.hpp"

#include <cmath>
#include <algorithm>

namespace openswmm::twoD {

namespace {
inline int tri_nbr(const MeshData& mesh, int t, int e) {
    switch (e) {
        case 0:  return mesh.tri_nbr0[t];
        case 1:  return mesh.tri_nbr1[t];
        case 2:  return mesh.tri_nbr2[t];
        default: return -1;
    }
}
} // namespace

void SurfaceJacobian::buildSparsity(const MeshData& mesh) {
    n_ = mesh.n_triangles();
    row_ptr_.assign(static_cast<std::size_t>(n_) + 1, 0);
    diag_pos_.assign(static_cast<std::size_t>(n_), -1);
    edge_pos_.assign(static_cast<std::size_t>(n_) * 3, -1);
    col_idx_.clear();
    col_idx_.reserve(static_cast<std::size_t>(n_) * 4);

    for (int i = 0; i < n_; ++i) {
        const int base = static_cast<int>(col_idx_.size());

        // Append `col` to this row if not already present; return its position.
        // Rows have at most 4 entries, so the linear scan is trivial — and it
        // merges any duplicate-neighbour edges onto one matrix entry.
        auto find_or_add = [&](int col) -> int {
            for (int p = base; p < static_cast<int>(col_idx_.size()); ++p)
                if (col_idx_[p] == col) return p;
            col_idx_.push_back(col);
            return static_cast<int>(col_idx_.size()) - 1;
        };

        diag_pos_[i] = find_or_add(i);                 // diagonal first
        for (int e = 0; e < 3; ++e) {
            const int nb = tri_nbr(mesh, i, e);
            if (nb < 0) continue;                       // boundary edge → no entry
            edge_pos_[static_cast<std::size_t>(i) * 3 + e] = find_or_add(nb);
        }
        row_ptr_[i + 1] = static_cast<int>(col_idx_.size());
    }

    values_.assign(col_idx_.size(), 0.0);
}

void SurfaceJacobian::assemble(const MeshData& mesh, const SurfaceStateData& state,
                                double gamma, double dh_floor,
                                const double* deta_dv) {
    std::fill(values_.begin(), values_.end(), 0.0);

    // ∂F/∂V = (∂F/∂η)·(dη/dV). Under FLAT, dη/dV = 1/A. Under VFR the caller
    // passes a per-cell dη/dV array (1/(A·max(w,ε))) so the diagonal uses cell
    // i's factor and each off-diagonal uses the NEIGHBOUR's factor — the AMG
    // preconditioner is otherwise mis-scaled at wetting fronts, where it can
    // cost more Krylov iterations than no preconditioner at all.
    for (int i = 0; i < n_; ++i) {
        const double inv_area_i = (mesh.tri_area[i] > 1.0e-30)
                                      ? 1.0 / mesh.tri_area[i] : 0.0;
        const double dedv_i = deta_dv ? deta_dv[i] : inv_area_i;
        double diag = 1.0;  // identity term of M = I − γJ
        for (int e = 0; e < 3; ++e) {
            const int nb = tri_nbr(mesh, i, e);
            if (nb < 0) continue;
            const double dh = std::abs(state.head[i] - state.head[nb]);
            const double F  = std::abs(state.edge_flux[i * 3 + e]);
            const double T  = F / std::max(dh, dh_floor);   // transmissivity
            // FLAT keeps the original approximation (cell i's inv_area for the
            // off-diagonal too) so the flat path is bit-identical; VFR uses the
            // neighbour's true dη/dV.
            const double dedv_nb = deta_dv ? deta_dv[nb] : inv_area_i;
            const double m_off = -gamma * T * dedv_nb;       // M_ij = −γ T dη/dV_j
            diag += gamma * T * dedv_i;                      // M_ii += γ T dη/dV_i
            values_[static_cast<std::size_t>(
                edge_pos_[static_cast<std::size_t>(i) * 3 + e])] += m_off;
        }
        values_[static_cast<std::size_t>(diag_pos_[i])] += diag;
    }
}

void SurfaceJacobian::assembleFromTangents(const MeshData& mesh,
                                            const double* diag,
                                            const double* dfdvi,
                                            const double* dfdvnbr,
                                            double gamma) {
    std::fill(values_.begin(), values_.end(), 0.0);

    for (int i = 0; i < n_; ++i) {
        // Mirror applyTangentJv exactly: dfdvi accumulates on the diagonal for
        // every edge (boundary tangents live there — no column exists for
        // them), dfdvnbr lands on the neighbour column.
        double jii = diag[i];
        for (int e = 0; e < 3; ++e) {
            const int slot = static_cast<int>(i) * 3 + e;
            jii += dfdvi[slot];
            const int nb = tri_nbr(mesh, i, e);
            if (nb < 0) continue;
            values_[static_cast<std::size_t>(
                edge_pos_[static_cast<std::size_t>(slot)])] +=
                -gamma * dfdvnbr[slot];
        }
        values_[static_cast<std::size_t>(diag_pos_[i])] += 1.0 - gamma * jii;
    }
}

} // namespace openswmm::twoD
