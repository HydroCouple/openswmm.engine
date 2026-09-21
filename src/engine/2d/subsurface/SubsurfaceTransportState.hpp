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
 * @file SubsurfaceTransportState.hpp
 * @brief T7.1 — species / age / enthalpy carried by the two-zone groundwater
 *        kernel (GW transport plan §3.4).
 *
 * @details The subsurface twin of `SurfaceTransportState`, and deliberately
 *          the same shape so the two read alike and the seam between them is
 *          one concentration:
 *
 *          - **Mass, not concentration.** A store holds `conc × m³`; the age
 *            row holds age-volume and the temperature row temperature-volume
 *            (°C·m³, signed). A report divides by the SI water volume.
 *          - **Species-major**, `[s * n_cells + c]`, so a species sweep walks
 *            contiguous cells.
 *          - **Rows come from `TransportPolicy`** — pollutants, MSX,
 *            `__WATER_AGE__`, `__TEMPERATURE__` last — never re-derived here
 *            (GW plan §3.4: "do not re-derive np/nm/na/nt a fourth time").
 *
 *          **Two bulk stores, one per zone.** `sat_mass` rides the saturated
 *          water `h_g·θ_s·A`; `unsat_mass` rides the column store `h_u·A`,
 *          whatever closure produced it. The plan's third store, the
 *          per-layer `layer_mass` for closure B, is **T7.2's**: it is a
 *          resolution refinement, and a bulk unsaturated store already
 *          conserves exactly — which is what T7.1 is for. Under closure B
 *          the bulk store means the column is treated as well mixed for
 *          transport while its WATER stays σ-resolved.
 *
 *          **Heat is water-only in T7.1.** The temperature row is a water
 *          temperature-volume, exactly as on the surface. The soil matrix's
 *          own heat capacity (`[GW_PARAMS] rho_s / c_s`), conduction and the
 *          thermal boundaries are T7.3; until then a temperature row in the
 *          aquifer transports, but it does not yet exchange heat with the
 *          solids.
 *
 * @ingroup engine_2d
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_2D_SUBSURFACE_TRANSPORT_STATE_HPP
#define OPENSWMM_ENGINE_2D_SUBSURFACE_TRANSPORT_STATE_HPP

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace openswmm::twoD {

/**
 * @brief Species mass in both aquifer zones, and where mass crossed a
 *        boundary.
 */
struct SubsurfaceTransportState {
    // ---- rows (TransportPolicy's layout, copied at initialize) ------------
    int n_species = 0;
    int n_cells   = 0;
    int n_pollut  = 0;
    int n_msx     = 0;
    int age_row   = -1;
    int temp_row  = -1;
    std::vector<std::string> row_names;

    /// A row whose mass may legitimately be negative — the temperature row
    /// (°C·m³ below zero is a state, not an error), so no clamp applies.
    bool signedRow(int s) const noexcept { return s == temp_row; }

    /// Row index of a species by name, or −1 — the `[GW_*]` sections address
    /// species by name and resolve through this.
    int rowIndex(std::string_view name) const noexcept {
        for (std::size_t i = 0; i < row_names.size(); ++i)
            if (row_names[i] == name) return static_cast<int>(i);
        return -1;
    }

    // ---- the two stores ---------------------------------------------------
    // T7.2: a store holds the cell's TOTAL mass of a species — dissolved
    // plus whatever is sorbed on the grains. Only the dissolved part
    // travels, and `dissolvedFraction` is what every flux multiplies by, so
    // retardation is a property of the CONCENTRATION the channels read
    // rather than a factor sprinkled through them.
    std::vector<double> sat_mass;     ///< [s*n_cells+c] in the saturated water
    std::vector<double> unsat_mass;   ///< [s*n_cells+c] in the column store

    // ---- T7.2 resolved parameters ----------------------------------------
    /// `[GW_TRANSPORT_PARAMS]` per cell, after `* < TAG < CELL` resolution.
    std::vector<double> alpha_L;      ///< longitudinal dispersivity (m)
    std::vector<double> alpha_T;      ///< transverse dispersivity (m)
    std::vector<double> D_m;          ///< molecular diffusivity (m²/s)
    std::vector<double> rho_b;        ///< bulk density ρ_s(1 − θ_s) (kg/m³)
    /// `[GW_SORPTION]` per cell and species, `[s*n_cells+c]`: the linear
    /// partition coefficient (m³/kg — the row is L/kg and converts once)
    /// and the first-order decay constant (1/s).
    std::vector<double> kd;
    std::vector<double> decay;
    /// Whether dispersion runs at all (`[GW_TRANSPORT_OPTIONS] DISPERSION`).
    bool dispersion_on = true;
    /// T7.2 telemetry: faces whose dispersive exchange hit the limiter. A
    /// bind is not an error — it is the face telling the modeller the
    /// dispersion is under-resolved at this Δt, which is the surface's
    /// rule too (`dispersion_limiter_binds`).
    long dispersion_limiter_binds = 0;

    /// The share of a cell's stored mass that is DISSOLVED and therefore
    /// mobile: `1 / R` with the retardation factor `R = 1 + ρ_b·K_d/θ`.
    /// Age and temperature never sorb — they are properties of the water,
    /// not solutes — so they are always fully mobile.
    double dissolvedFraction(int s, int c, double theta) const noexcept {
        if (s == age_row || s == temp_row) return 1.0;
        if (kd.empty() || !(theta > 0.0)) return 1.0;
        const double k = kd[idx(s, c)];
        if (!(k > 0.0)) return 1.0;
        const double R = 1.0 + rho_b[static_cast<std::size_t>(c)] * k / theta;
        return (R > 1.0) ? 1.0 / R : 1.0;
    }

    // ---- cross-cadence accumulators (paired with the water's) -------------
    /// Lateral Darcy face sides, `[s*ne + e]` — the twin of `eacc_L/eacc_R`,
    /// gathered by the owning cell at ITS firing (GW plan §3.1).
    std::vector<double> sacc_L, sacc_R;
    /// `[s*n_cells+c]` mass the SURFACE handed down with its infiltration,
    /// gathered when the GW cell fires — the twin of `xacc_from_surface`.
    std::vector<double> xacc_from_surface;
    /// …and the mass owed back to the surface with saturation excess and
    /// column rejection — the twin of `xacc_to_surface`, drained by the
    /// surface cell.
    std::vector<double> xacc_to_surface;

    // ---- T7.4: the 1D seams ----------------------------------------------
    /// `[s * n_beds + b]` mass a RECHARGING node pushed down its bed,
    /// sampled at the node's own concentration at tier-0 cadence and
    /// gathered by the cell when it fires — the twin of the water's `nacc`.
    std::vector<double> nacc_mass;
    /// `[s * n_cells + c]` mass a LEAKING conduit delivered, at the
    /// conduit's own concentration — the twin of the water's `lacc`.
    std::vector<double> lacc_mass;
    /// `[s * n_nodes]` mass the aquifer owes each 1D node, booked at the
    /// cell's firing and flushed by the router into the node's coupling
    /// queues. Reset per routing batch, exactly as `node_exchange_vol_` is.
    std::vector<double> node_out_mass;

    // ---- ledgers (per species, cumulative) --------------------------------
    // Signs follow the water ledger they mirror: a "lost_" row is mass that
    // left the aquifer, a "gained_" row is mass that arrived.
    std::vector<double> lost_deep;       ///< deep percolation
    std::vector<double> lost_node;       ///< to a 1D node through its bed
    std::vector<double> lost_link;       ///< to a gaining conduit (G-X4)
    std::vector<double> lost_dunne;      ///< saturation excess + rejection, to the surface
    std::vector<double> lost_et;         ///< the intensive rows ET carried off
    std::vector<double> gained_infil;    ///< from the surface's infiltration
    std::vector<double> gained_node;     ///< from a recharging node (T7.4 seam)
    std::vector<double> gained_link;     ///< from a leaking conduit (T7.4 seam)
    std::vector<double> net_lateral;     ///< net across the domain edge (0 on a closed mesh)
    std::vector<double> lost_reaction;   ///< T7.2: first-order decay
    /// Mass in both stores at the start of the run — the conservation gate's
    /// reference (the aquifer is seeded from `[GW_INITIAL_QUALITY]`).
    std::vector<double> init_mass;
    /// Internal, informational: the unsaturated ⇄ saturated handover. Not in
    /// the balance — both ends are inside the same cell.
    std::vector<double> internal_recharge;

    bool active() const noexcept { return n_species > 0 && n_cells > 0; }

    std::size_t idx(int s, int c) const noexcept {
        return static_cast<std::size_t>(s) * static_cast<std::size_t>(n_cells) +
               static_cast<std::size_t>(c);
    }

    void resize(int n_spec, int n_cell, int n_edges) {
        n_species = (n_spec > 0) ? n_spec : 0;
        n_cells   = (n_cell > 0) ? n_cell : 0;
        const auto ns = static_cast<std::size_t>(n_species);
        const auto nc = static_cast<std::size_t>(n_cells);
        const auto ne = static_cast<std::size_t>(n_edges > 0 ? n_edges : 0);
        sat_mass.assign(ns * nc, 0.0);
        unsat_mass.assign(ns * nc, 0.0);
        sacc_L.assign(ns * ne, 0.0);
        sacc_R.assign(ns * ne, 0.0);
        xacc_from_surface.assign(ns * nc, 0.0);
        xacc_to_surface.assign(ns * nc, 0.0);
        lacc_mass.assign(ns * nc, 0.0);          // T7.4
        // nacc_mass and node_out_mass are sized by the solver, which knows
        // the bed and node counts.
        lost_deep.assign(ns, 0.0);
        lost_node.assign(ns, 0.0);
        lost_link.assign(ns, 0.0);
        lost_dunne.assign(ns, 0.0);
        lost_et.assign(ns, 0.0);
        gained_infil.assign(ns, 0.0);
        gained_node.assign(ns, 0.0);
        gained_link.assign(ns, 0.0);
        net_lateral.assign(ns, 0.0);
        lost_reaction.assign(ns, 0.0);          // T7.2
        init_mass.assign(ns, 0.0);
        internal_recharge.assign(ns, 0.0);
        alpha_L.assign(nc, 1.0);                // T7.2: the row defaults
        alpha_T.assign(nc, 0.1);
        D_m.assign(nc, 1.0e-9);
        rho_b.assign(nc, 0.0);
        kd.assign(ns * nc, 0.0);
        decay.assign(ns * nc, 0.0);
        dispersion_limiter_binds = 0;
    }

    void clear() { *this = SubsurfaceTransportState{}; }

    /// T7.4: mass in flight on the two 1D seams — booked by the producer,
    /// not yet taken by the owner. Part of `storage`, for the same reason
    /// the surface accumulators are.
    double inFlight1D(int s) const noexcept {
        double m = 0.0;
        const auto ns = static_cast<std::size_t>(n_species > 0 ? n_species : 1);
        if (!nacc_mass.empty()) {
            const auto nb = nacc_mass.size() / ns;
            for (std::size_t b = 0; b < nb; ++b)
                m += nacc_mass[static_cast<std::size_t>(s) * nb + b];
        }
        const auto base = static_cast<std::size_t>(s) *
                          static_cast<std::size_t>(n_cells);
        for (int c = 0; c < n_cells; ++c)
            m += lacc_mass[base + static_cast<std::size_t>(c)];
        return m;
    }

    /// Everything the aquifer HOLDS of one species, including mass in flight
    /// in a side accumulator (the water's `liveStorage` rule).
    double storage(int s) const noexcept {
        double m = 0.0;
        const auto base = static_cast<std::size_t>(s) *
                          static_cast<std::size_t>(n_cells);
        for (int c = 0; c < n_cells; ++c) {
            m += sat_mass[base + static_cast<std::size_t>(c)];
            m += unsat_mass[base + static_cast<std::size_t>(c)];
            m += xacc_from_surface[base + static_cast<std::size_t>(c)];
            m += xacc_to_surface[base + static_cast<std::size_t>(c)];
        }
        const auto ne = sacc_L.size() / (n_species > 0 ? static_cast<std::size_t>(n_species) : 1);
        for (std::size_t e = 0; e < ne; ++e) {
            const auto k = static_cast<std::size_t>(s) * ne + e;
            m += sacc_L[k] + sacc_R[k];
        }
        return m + inFlight1D(s);   // T7.4
    }

    /// The storage the LEDGER can account for: holdings less the two surface
    /// accumulators, which are always one firing out of phase with
    /// `gained_infil` (booked when the cell gathers) and `lost_dunne`
    /// (booked when the cell pushes). The water side's `ledgeredStorage`
    /// makes the identical exclusion for the identical reason — the two
    /// pending terms cancel on both sides of the balance.
    double ledgeredStorage(int s) const noexcept {
        double m = 0.0;
        const auto base = static_cast<std::size_t>(s) *
                          static_cast<std::size_t>(n_cells);
        for (int c = 0; c < n_cells; ++c)
            m += sat_mass[base + static_cast<std::size_t>(c)] +
                 unsat_mass[base + static_cast<std::size_t>(c)];
        const auto ne = sacc_L.size() / (n_species > 0 ? static_cast<std::size_t>(n_species) : 1);
        for (std::size_t e = 0; e < ne; ++e) {
            const auto k = static_cast<std::size_t>(s) * ne + e;
            m += sacc_L[k] + sacc_R[k];
        }
        // T7.4: the two 1D arrival accumulators are booked into
        // `gained_node` / `gained_link` only when the CELL gathers them, so
        // until then they are storage the ledger has not yet named — the
        // same phase rule the surface accumulators follow, and the reason
        // they are counted here while `xacc_*` are not.
        return m + inFlight1D(s);
    }

    /// T7.1's conservation statement, the twin of
    /// `SurfaceTransportState::totalIncludingLedgers`: what the aquifer holds
    /// now, plus everything that left, minus everything that arrived, equals
    /// what it started with. Zero to machine precision for a conserving
    /// kernel — GW plan gate 6's stub.
    double residual(int s) const noexcept {
        const auto us = static_cast<std::size_t>(s);
        const double out = lost_deep[us] + lost_node[us] + lost_link[us] +
                           lost_dunne[us] + lost_et[us] + lost_reaction[us];
        const double in  = gained_infil[us] + gained_node[us] + gained_link[us] +
                           net_lateral[us];
        return ledgeredStorage(s) + out - in - init_mass[us];
    }
};

}  // namespace openswmm::twoD

#endif  // OPENSWMM_ENGINE_2D_SUBSURFACE_TRANSPORT_STATE_HPP
