// SPDX-License-Identifier: Apache-2.0
//
// Copyright 2026 Caleb Buahin
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/**
 * @file      KokkosSpeciesKernels.hpp
 * @brief     Device transport rows (species, age, temperature) of the Kokkos 2D marcher.
 *
 * @details Each routine is the device form of one ExplicitInertialSolver routine and keeps its
 *          arithmetic and its order, so the OpenMP backend reproduces the CPU marcher's cell
 *          masses bit for bit. Arrays use the CPU layouts: cell mass is row-major
 *          `[row * n_cells + cell]`, face accumulators are face-major `[face * n_rows + row]`.
 *          Run-wide totals are never summed on the device: every kernel adds into a slot owned
 *          by its cell, face, boundary entry or exchange point, and the host folds the slots.
 *
 * @ingroup engine_2d_gpu
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_2D_GPU_KOKKOS_SPECIES_KERNELS_HPP
#define OPENSWMM_ENGINE_2D_GPU_KOKKOS_SPECIES_KERNELS_HPP

#include <Kokkos_Core.hpp>
#include <cmath>
#include <cstddef>

#include "KokkosSourceKernels.hpp"
#include "KokkosTypes.hpp"

namespace openswmm::twoD::gpu {

/// std::max(a, b) with its exact operand choice (signed zeros and NaN included).
KOKKOS_INLINE_FUNCTION double hostMax(double a, double b) {
    return (a < b) ? b : a;
}
/// std::min(a, b) with its exact operand choice.
KOKKOS_INLINE_FUNCTION double hostMin(double a, double b) {
    return (b < a) ? b : a;
}

/// Per-cell ledger kinds a cell firing books (ExplicitInertialSolver::SourceLedger subset).
enum SpeciesLedger : int {
    kLedgerRainfall = 0,
    kLedgerCouplingIn = 1,
    kLedgerInfiltration = 2,
    kLedgerCouplingOut = 3,
    kSpeciesLedgers = 4
};

/// Cells summed by one kernel thread when the per-cell ledgers are folded for publication.
/// Fixed, so a ledger total does not depend on the thread count or the device.
inline constexpr std::size_t kSpeciesLedgerRun = 64;

/**
 * @brief Views and constants of the transport rows, captured by value in a kernel.
 * @note Views not used by a model are empty and guarded by their flag.
 */
struct SpeciesRows {
    DView mass;         ///< [row * nt + cell] row inventory (concentration x m3)
    DView saccL, saccR; ///< [face * ns + row] pending face transfers, left and right side
    DView vol;          ///< cell water volume, m3 (donor concentration)
    DView rain_conc;    ///< [row] concentration carried by rainfall
    DView csrc;         ///< [row * nt + cell] mass-rate density riding positive coupling flux
    DView runoff;       ///< [cell] net runoff volume since the last runoff step, m3
    DView ledger;       ///< [(kind * ns + row) * nt + cell] mass booked since publication
    IView binds;        ///< [face] dispersion limiter binds since publication
    IView ptr, edge;    ///< cell-to-face CSR
    DView sign;         ///< +1 where the cell is the face's left cell
    IView tier, ftier, nv;
    int ns = 0, nt = 0, temp_row = -1, age_row = -1;
    double dispersion = 0.0, beta3 = 0.0, beta4 = 0.0;
    bool has_csrc = false, has_runoff = false;

    /// Concentration water leaving @p cell carries (ExplicitInertialSolver::donorConc).
    KOKKOS_INLINE_FUNCTION double conc(int s, int cell) const {
        const double v = vol(cell);
        if (!(v > 0.0))
            return 0.0;
        return mass(s * nt + cell) / v;
    }

    /// Mass @p volume takes from @p cell at its concentration (takenAtCellConc).
    KOKKOS_INLINE_FUNCTION double taken(int s, int cell, double volume) const {
        const double dm = volume * conc(s, cell);
        const double m = mass(s * nt + cell);
        return s == temp_row ? dm : ((dm < m) ? dm : m);
    }

    /**
     * @brief Book one face's advective and dispersive row transfers (bookFaceSpecies).
     * @param[in] e Face.
     * @param[in] a Left cell.
     * @param[in] b Right cell.
     * @param[in] dM Water moved left to right this firing, m3.
     * @param[in] hf Face depth of the dispersive conductance, m.
     * @param[in] dt_f Face step, seconds.
     * @param[in] global_step True for a tail step, where every face fires once.
     * @param[in] xi Face length, m.
     * @param[in] inv_dx Inverse centroid distance along the normal, 1/m.
     * @param[in] cfac Face conveyance under LOCAL_INERTIAL, otherwise one.
     */
    KOKKOS_INLINE_FUNCTION void bookFace(int e, int a, int b, double dM, double hf, double dt_f,
                                         bool global_step, double xi, double inv_dx,
                                         double cfac) const {
        if (dM != 0.0) {
            const int donor = (dM > 0.0) ? a : b;
            for (int s = 0; s < ns; ++s) {
                const double c = conc(s, donor);
                if (c == 0.0)
                    continue;
                const double dMs = dM * c;
                saccL(e * ns + s) -= dMs;
                saccR(e * ns + s) += dMs;
            }
        }
        if (!(dispersion > 0.0))
            return;
        const double va = vol(a), vb = vol(b);
        if (!(va > 0.0 && vb > 0.0))
            return;
        const double cond = dispersion * hf * xi * inv_dx * dt_f * cfac; // m3 exchanged
        for (int s = 0; s < ns; ++s) {
            const double ma = mass(s * nt + a), mb = mass(s * nt + b);
            const double ca = ma / va, cb = mb / vb;
            double dMd = cond * (ca - cb); // + = a to b
            if (dMd == 0.0)
                continue;
            const int giver = (dMd > 0.0) ? a : b;
            const int receiver = (dMd > 0.0) ? b : a;
            const double eq = (ca - cb) * va * vb / (va + vb) / ((nv(receiver) == 4) ? 4.0 : 3.0);
            const int refire = global_step ? 1 : (1 << (tier(giver) - ftier(e)));
            const double share =
                ((nv(giver) == 4) ? beta4 : beta3) / refire * std::fabs((dMd > 0.0) ? ma : mb);
            const double cap = (s == temp_row) ? std::fabs(eq) : hostMin(std::fabs(eq), share);
            if (std::fabs(dMd) > cap) {
                dMd = (dMd > 0.0) ? cap : -cap;
                ++binds(e);
            }
            saccL(e * ns + s) -= dMd;
            saccR(e * ns + s) += dMd;
        }
    }

    /// Gather and clear @p cell's side of every incident face accumulator for row @p s.
    KOKKOS_INLINE_FUNCTION double gather(int s, int cell) const {
        double dm = 0.0;
        for (int p = ptr(cell); p < ptr(cell + 1); ++p) {
            const int e = edge(p);
            if (sign(p) > 0.0) {
                dm += saccL(e * ns + s);
                saccL(e * ns + s) = 0.0;
            } else {
                dm += saccR(e * ns + s);
                saccR(e * ns + s) = 0.0;
            }
        }
        return dm;
    }

    /// A firing cell lands its pending transfers (the row gather of fireCellsImpl).
    KOKKOS_INLINE_FUNCTION void land(int cell, double flux_m3) const {
        if (has_runoff)
            runoff(cell) -= flux_m3;
        for (int s = 0; s < ns; ++s) {
            double& m = mass(s * nt + cell);
            m += gather(s, cell);
            if (s != temp_row && m < 0.0)
                m = 0.0;
        }
    }

    /// A cell leaving its cadence lands what is pending (the row part of settleAccumulators).
    KOKKOS_INLINE_FUNCTION void settle(int cell) const {
        for (int s = 0; s < ns; ++s) {
            const double dm = gather(s, cell);
            if (dm != 0.0) {
                double& m = mass(s * nt + cell);
                m += dm;
                if (m < 0.0 && s != temp_row)
                    m = 0.0;
            }
        }
    }

    /**
     * @brief Mix a cell's accepted arrivals into every row and take its sinks at the mixed
     * concentration (prepareCellSources' runoff booking, then applyCellSourceRow per row).
     * @param[in] cell Cell whose source budget was accepted.
     * @param[in] b The water budget of this firing; @c applied must be true.
     * @param[in] area_dt Cell area times the firing duration, m2 s.
     */
    KOKKOS_INLINE_FUNCTION void applySources(int cell, const SourceBudget& b,
                                             double area_dt) const {
        const double out = hostMax(-b.coupling_request, 0.0) * b.scale;
        if (has_runoff)
            runoff(cell) += out - hostMax(b.coupling_request, 0.0);
        for (int s = 0; s < ns; ++s) {
            double& m = mass(s * nt + cell);
            const double rain_mass = b.rain * rain_conc(s);
            const double coupling_mass =
                (b.coupling_request > 0.0 && has_csrc) ? csrc(s * nt + cell) * area_dt : 0.0;
            m += rain_mass + coupling_mass;
            const double concentration = b.available > 0.0 ? m / b.available : 0.0;
            const double mi = concentration * b.infiltration;
            const double mc = concentration * out;
            const double me = (s == age_row || s == temp_row) ? concentration * b.evaporation : 0.0;
            m -= mi + mc + me;
            if (s != temp_row && m < 0.0)
                m = 0.0;
            ledger((kLedgerRainfall * ns + s) * nt + cell) += rain_mass;
            ledger((kLedgerCouplingIn * ns + s) * nt + cell) += coupling_mass;
            ledger((kLedgerInfiltration * ns + s) * nt + cell) += mi;
            ledger((kLedgerCouplingOut * ns + s) * nt + cell) += mc;
        }
    }

    /**
     * @brief Water leaving @p cell through an open edge or a drain takes its rows along
     * (sinkMassAtCellConc).
     * @param[in] cell Cell, read at its volume before the water leaves.
     * @param[in] dv Volume leaving, m3.
     * @param[in] out Per-entry totals the taken mass is added to.
     * @param[in] base Index of row zero in @p out.
     */
    KOKKOS_INLINE_FUNCTION void sink(int cell, double dv, const DView& out, int base) const {
        if (!(dv > 0.0))
            return;
        if (has_runoff)
            runoff(cell) += dv;
        for (int s = 0; s < ns; ++s) {
            if (conc(s, cell) == 0.0)
                continue;
            const double t = taken(s, cell, dv);
            mass(s * nt + cell) -= t;
            out(base + s) += t;
        }
    }

    /**
     * @brief Water arriving in @p cell at given row concentrations (boundary inflow, node spill).
     * @param[in] cell Receiving cell.
     * @param[in] dv Volume arriving, m3.
     * @param[in] conc_in Row concentrations of the arriving water.
     * @param[in] cbase Index of row zero in @p conc_in.
     * @param[in] gain Per-entry totals the arriving mass is added to.
     * @param[in] gbase Index of row zero in @p gain.
     */
    KOKKOS_INLINE_FUNCTION void arrive(int cell, double dv, const DView& conc_in, int cbase,
                                       const DView& gain, int gbase) const {
        if (has_runoff)
            runoff(cell) -= dv;
        for (int s = 0; s < ns; ++s) {
            const double c = conc_in(cbase + s);
            if (c == 0.0)
                continue;
            const double g = dv * c;
            mass(s * nt + cell) += g;
            gain(gbase + s) += g;
        }
    }
};

} // namespace openswmm::twoD::gpu

#endif // OPENSWMM_ENGINE_2D_GPU_KOKKOS_SPECIES_KERNELS_HPP
