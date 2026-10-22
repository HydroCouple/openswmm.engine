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
 * @file      SubsurfaceTransportState.hpp
 * @brief     T7.1 — species / age / enthalpy carried by the two-zone groundwater
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
 *          whatever closure produced it. Explicit LAYERED transport instead
 *          owns species-major `layer_mass` on the accepted SIGMA column;
 *          `unsat_mass` is then only its derived sum. Default BULK retains
 *          the existing well-mixed unsaturated approximation.
 *
 *          The temperature row remains a liquid-water temperature-volume,
 *          exactly as on the surface. The separate ground thermal columns
 *          own soil-matrix plus porewater energy, conduction and thermal
 *          boundaries; accepted water transfers carry sensible energy between
 *          those stores without putting solid heat into `layer_mass`.
 *
 * @ingroup engine_2d
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_2D_SUBSURFACE_TRANSPORT_STATE_HPP
#define OPENSWMM_ENGINE_2D_SUBSURFACE_TRANSPORT_STATE_HPP

#include <cmath>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace openswmm::twoD {

/**
 * @brief Species mass in both aquifer zones, and where mass crossed a boundary.
 */
struct SubsurfaceTransportState {
    // ---- rows (TransportPolicy's layout, copied at initialize) ------------
    double c_diff = 0.4; ///< explicit solute diffusion safety factor
    int n_species = 0;
    int n_cells = 0;
    int n_pollut = 0;
    int n_msx = 0;
    int age_row = -1;
    int temp_row = -1;
    std::vector<std::string> row_names;

    /**
     * @brief A row whose mass may legitimately be negative — the temperature row (°C·m³ below
     * zero is a state, not an error), so no clamp applies.
     *
     * @param[in] s Valid zero-based species index; caller excludes the -1 unset sentinel.
     * @return True exactly when s == temp_row. Read-only and unchecked; no allocation.
     */
    bool signedRow(int s) const noexcept { return s == temp_row; }

    /**
     * @brief Row index of a species by name, or −1 — the `[GW_*]` sections address species by
     * name and resolve through this.
     *
     * @param[in] name Borrowed nonowning exact species name view, not retained.
     * @return First case-sensitive row index or -1 for no match. Read-only linear search; no
     * allocation.
     */
    int rowIndex(std::string_view name) const noexcept {
        for (std::size_t i = 0; i < row_names.size(); ++i)
            if (row_names[i] == name)
                return static_cast<int>(i);
        return -1;
    }

    // ---- the two stores ---------------------------------------------------
    // T7.2: a store holds the cell's TOTAL mass of a species — dissolved
    // plus whatever is sorbed on the grains. Only the dissolved part
    // travels, and `dissolvedFraction` is what every flux multiplies by, so
    // retardation is a property of the CONCENTRATION the channels read
    // rather than a factor sprinkled through them.
    std::vector<double> sat_mass;   ///< [s*n_cells+c] in the saturated water
    std::vector<double> unsat_mass; ///< [s*n_cells+c] in the column store
    // In LAYERED mode this is the canonical unsaturated inventory. The
    // public bulk field above is its derived sum, never a second owner.
    int n_layers = 0;
    std::vector<double> layer_mass; ///< [(s*n_layers+j)*n_cells+c]
    // Above-table receiving slots are fixed at producer time. Water is a
    // routing split of nacc/lacc, not additional storage. The paired masses
    // live here instead of nacc_mass/lacc_mass until the owner fires.
    std::vector<double> contact_water;                      ///< [j*n_cells+c], SI m3
    std::vector<double> contact_node_out, contact_link_out; ///< gross promised water, per bed/cell
    std::vector<double> contact_node_mass, contact_link_mass; /**
     * @brief < layerIdx
     *
     * @details Map a species/layer/cell triple into canonical layer-major storage.
     * @param[in] s Valid zero-based species row.
     * @param[in] j Valid zero-based unsaturated layer in [0,n_layers), surface first.
     * @param[in] c Valid zero-based cell in [0,n_cells).
     * @return size_t offset (s*n_layers+j)*n_cells+c; no bounds/overflow checks, allocation or
     * mutation.
     */
    std::size_t layerIdx(int s, int j, int c) const noexcept {
        return (std::size_t(s) * n_layers + j) * n_cells + c;
    }
    /**
     * @brief Refresh one derived unsaturated bulk inventory from canonical layers.
     * @param[in] s Valid zero-based species row.
     * @param[in] c Valid zero-based cell index.
     * @post Writes unsat_mass[idx(s,c)] as the sum over all layer_mass entries; zero when
     * n_layers == 0. Units are native concentration*m3, age s*m3, temperature degrees C*m3.
     * @pre Corresponding arrays are coherently sized; this operation belongs to LAYERED
     * ownership and must not replace an independently owned BULK inventory. Exclusive state
     * access, no allocation or ledger changes.
     */
    void refreshUnsat(int s, int c) noexcept {
        double sum = 0;
        for (int j = 0; j < n_layers; ++j)
            sum += layer_mass[layerIdx(s, j, c)];
        unsat_mass[idx(s, c)] = sum;
    }

    // ---- T7.2 resolved parameters ----------------------------------------
    /// `[GW_TRANSPORT_PARAMS]` per cell, after `* < TAG < CELL` resolution.
    std::vector<double> alpha_L; ///< longitudinal dispersivity (m)
    std::vector<double> alpha_T; ///< transverse dispersivity (m)
    std::vector<double> D_m;     ///< molecular diffusivity (m²/s)
    std::vector<double> rho_b;   ///< bulk density ρ_s(1 − θ_s) (kg/m³)
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

    /**
     * @brief The share of a cell's stored mass that is DISSOLVED and therefore mobile: `1 / R`
     * with the retardation factor `R = 1 + ρ_b·K_d/θ`. Age and temperature never sorb — they are
     * properties of the water, not solutes — so they are always fully mobile.
     *
     * @param[in] s Valid zero-based species row, unchecked.
     * @param[in] c Valid zero-based cell with rho_b and kd storage when sorption is active.
     * @param[in] theta Finite volumetric water content, dimensionless.
     * @return Dissolved share 1/R in (0,1] for valid physical data; one for age/temperature,
     * absent kd, nonpositive theta/kd, or R <= 1.
     * @note rho_b uses kg/m3 and kd m3/kg. This does not multiply/move stored inventory; stable
     * read-only arrays are required. No allocation or bounds validation.
     */
    double dissolvedFraction(int s, int c, double theta) const noexcept {
        if (s == age_row || s == temp_row)
            return 1.0;
        if (kd.empty() || !(theta > 0.0))
            return 1.0;
        const double k = kd[idx(s, c)];
        if (!(k > 0.0))
            return 1.0;
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
    /// `[s * n_cells + c]` mass a GAINING conduit already delivered to its
    /// end nodes (review, 2026-10-15), debited from the saturated store when
    /// the cell fires — the twin of the water's negative `lacc`. Booked into
    /// `lost_link` at that debit, so the aquifer ledger is unchanged in form.
    std::vector<double> lout_mass;
    /// `[s]` informational: gain mass the 1D received that the cell could not
    /// supply at its firing (its store had already been drawn below it).
    std::vector<double> link_gain_shortfall;
    /// `[s * n_nodes]` mass the aquifer owes each 1D node, booked at the
    /// cell's firing and flushed by the router into the node's coupling
    /// queues. Reset per routing batch, exactly as `node_exchange_vol_` is.
    std::vector<double> node_out_mass;

    // ---- ledgers (per species, cumulative) --------------------------------
    // Signs follow the water ledger they mirror: a "lost_" row is mass that
    // left the aquifer, a "gained_" row is mass that arrived.
    std::vector<double> lost_deep;    ///< deep percolation
    std::vector<double> lost_node;    ///< to a 1D node through its bed
    std::vector<double> lost_link;    ///< to a gaining conduit (G-X4)
    std::vector<double> lost_dunne;   ///< saturation excess + rejection, to the surface
    std::vector<double> lost_et;      ///< the intensive rows ET carried off
    std::vector<double> gained_infil; ///< from the surface's infiltration
    std::vector<double> gained_node;  ///< from a recharging node (T7.4 seam)
    std::vector<double> gained_link;  ///< from a leaking conduit (T7.4 seam)
    std::vector<double> net_lateral;  ///< net across the domain edge (0 on a closed mesh)
    std::vector<double> gained_source, lost_source; ///< wells, native concentration x m3
    std::vector<double> boundary_in, boundary_out, boundary_flux_in,
        boundary_flux_out;             ///< gross split of net_lateral
    std::vector<double> lost_reaction; ///< T7.2: first-order decay
    std::vector<double> generated_age,
        gained_contact; ///< explicit finite-porewater model only; empty in compatibility mode
    /// Mass in both stores at the start of the run — the conservation gate's
    /// reference (the aquifer is seeded from `[GW_INITIAL_QUALITY]`).
    std::vector<double> init_mass;
    /// Internal, informational: the unsaturated ⇄ saturated handover. Not in
    /// the balance — both ends are inside the same cell.
    std::vector<double> internal_recharge;

    /**
     * @brief Test whether the subsurface transport layout is nonempty.
     * @return True when n_species and n_cells are positive; does not verify vector extents or
     * owner activation. Read-only, allocation-free.
     */
    bool active() const noexcept { return n_species > 0 && n_cells > 0; }

    /**
     * @brief Map a species/cell pair into bulk species-major storage.
     * @param[in] s Valid nonnegative species row in [0,n_species).
     * @param[in] c Valid nonnegative cell in [0,n_cells).
     * @return size_t offset s*n_cells+c; bounds and arithmetic overflow are unchecked. No
     * allocation/mutation.
     */
    std::size_t idx(int s, int c) const noexcept {
        return static_cast<std::size_t>(s) * static_cast<std::size_t>(n_cells) +
               static_cast<std::size_t>(c);
    }

    /**
     * @brief Allocate/reset the core two-zone transport arrays and ledgers.
     * @param[in] n_spec Requested species count; nonpositive clamps to zero.
     * @param[in] n_cell Requested cell count; nonpositive clamps to zero.
     * @param[in] n_edges Requested lateral-face count; nonpositive clamps to zero.
     * @post Zeroes bulk inventories, face/surface/conduit accumulators and cumulative ledgers;
     * resets parameter defaults. Solver separately sizes node-bed/node outputs and
     * layered/contact stores and refines row layout.
     * @note Existing n_layers, row names/layout fields and those separately owned arrays are not
     * reset here. Allocations may throw after partial resizing, invalidating references.
     * Requires exclusive state access; no rollback or unit conversion.
     */
    void resize(int n_spec, int n_cell, int n_edges) {
        n_species = (n_spec > 0) ? n_spec : 0;
        n_cells = (n_cell > 0) ? n_cell : 0;
        const auto ns = static_cast<std::size_t>(n_species);
        const auto nc = static_cast<std::size_t>(n_cells);
        const auto ne = static_cast<std::size_t>(n_edges > 0 ? n_edges : 0);
        sat_mass.assign(ns * nc, 0.0);
        unsat_mass.assign(ns * nc, 0.0);
        sacc_L.assign(ns * ne, 0.0);
        sacc_R.assign(ns * ne, 0.0);
        xacc_from_surface.assign(ns * nc, 0.0);
        xacc_to_surface.assign(ns * nc, 0.0);
        lacc_mass.assign(ns * nc, 0.0); // T7.4
        lout_mass.assign(ns * nc, 0.0);
        link_gain_shortfall.assign(ns, 0.0);
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
        gained_source.assign(ns, 0.0);
        lost_source.assign(ns, 0.0);
        boundary_in.assign(ns, 0);
        boundary_out.assign(ns, 0);
        boundary_flux_in.assign(ns, 0);
        boundary_flux_out.assign(ns, 0);
        lost_reaction.assign(ns, 0.0); // T7.2
        generated_age.clear();
        gained_contact.clear();
        init_mass.assign(ns, 0.0);
        internal_recharge.assign(ns, 0.0);
        alpha_L.assign(nc, 1.0); // T7.2: the row defaults
        alpha_T.assign(nc, 0.1);
        D_m.assign(nc, 1.0e-9);
        rho_b.assign(nc, 0.0);
        kd.assign(ns * nc, 0.0);
        decay.assign(ns * nc, 0.0);
        dispersion_limiter_binds = 0;
    }

    /**
     * @brief Reset the complete subsurface transport state to empty defaults.
     * @post Releases all bulk/layered inventories, accumulators, parameters, names and ledgers;
     * invalidates borrowed storage. Requires exclusive state access.
     */
    void clear() { *this = SubsurfaceTransportState{}; }

    /**
     * @brief T7.4: mass in flight on the two 1D seams — booked by the producer, not yet taken by
     * the owner. Part of `storage`, for the same reason the surface accumulators are.
     *
     * @param[in] s Valid zero-based species row with coherently sized mandatory and enabled
     * optional arrays.
     * @return Pending node/conduit arrival and layer-contact inventory in native
     * concentration*m3; excludes node-out already delivered to the network.
     * @pre LAYERED callers have refreshed derived unsat_mass; nacc_mass extent is species-major,
     * and generated_age/gained_contact are enabled/sized together.
     * @note Read-only accepted-state snapshot; no bounds/finite checks, allocation or mutation.
     * Serialize with producer/gather/ledger updates so pending inventories are sampled
     * consistently.
     */
    double inFlight1D(int s) const noexcept {
        double m = 0.0;
        const auto ns = static_cast<std::size_t>(n_species > 0 ? n_species : 1);
        if (!nacc_mass.empty()) {
            const auto nb = nacc_mass.size() / ns;
            for (std::size_t b = 0; b < nb; ++b)
                m += nacc_mass[static_cast<std::size_t>(s) * nb + b];
        }
        const auto base = static_cast<std::size_t>(s) * static_cast<std::size_t>(n_cells);
        for (int c = 0; c < n_cells; ++c)
            m += lacc_mass[base + static_cast<std::size_t>(c)];
        for (int j = 0; j < n_layers; ++j)
            for (int c = 0; c < n_cells; ++c) {
                const auto k = layerIdx(s, j, c);
                m += contact_node_mass[k] + contact_link_mass[k];
            }
        return m;
    }

    /**
     * @brief Everything the aquifer HOLDS of one species, including mass in flight in a side
     * accumulator (the water's `liveStorage` rule).
     *
     * @param[in] s Valid zero-based species row with coherently sized mandatory and enabled
     * optional arrays.
     * @return Live bulk inventories plus pending surface, lateral and 1D/contact receipts,
     * native concentration*m3.
     * @pre LAYERED callers have refreshed derived unsat_mass; nacc_mass extent is species-major,
     * and generated_age/gained_contact are enabled/sized together.
     * @note Read-only accepted-state snapshot; no bounds/finite checks, allocation or mutation.
     * Serialize with producer/gather/ledger updates so pending inventories are sampled
     * consistently.
     */
    double storage(int s) const noexcept {
        double m = 0.0;
        const auto base = static_cast<std::size_t>(s) * static_cast<std::size_t>(n_cells);
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
        return m + inFlight1D(s); // T7.4
    }

    /**
     * @brief The storage the LEDGER can account for: holdings less the two surface accumulators,
     * which are always one firing out of phase with `gained_infil` (booked when the cell
     * gathers) and `lost_dunne` (booked when the cell pushes). The water side's
     * `ledgeredStorage` makes the identical exclusion for the identical reason — the two pending
     * terms cancel on both sides of the balance.
     *
     * @param[in] s Valid zero-based species row with coherently sized mandatory and enabled
     * optional arrays.
     * @return Bulk inventories plus lateral and 1D/contact pending receipts, excluding both
     * surface cross-cadence accumulators, native concentration*m3.
     * @pre LAYERED callers have refreshed derived unsat_mass; nacc_mass extent is species-major,
     * and generated_age/gained_contact are enabled/sized together.
     * @note Read-only accepted-state snapshot; no bounds/finite checks, allocation or mutation.
     * Serialize with producer/gather/ledger updates so pending inventories are sampled
     * consistently.
     */
    double ledgeredStorage(int s) const noexcept {
        double m = 0.0;
        const auto base = static_cast<std::size_t>(s) * static_cast<std::size_t>(n_cells);
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

    /**
     * @brief T7.4 (2026-09-21): the scale a residual is judged against — what the aquifer
     * started with plus **every** route mass can arrive by. It lives here, beside `residual()`,
     * because the two must name the same channels: T7.4 added the node and link seams as routes
     * and the `.rpt` block's own copy of this sum was not updated, so a deck fed only through
     * its beds divided a machine-precision residual by a machine-precision denominator and
     * printed 6 198 889.840 % beside an exact balance. One definition, one place to add a
     * channel to.
     *
     * @param[in] s Valid zero-based species row with coherently sized mandatory and enabled
     * optional arrays.
     * @return Nonnegative absolute initial/inflow scale in native concentration*m3; includes
     * net-lateral magnitude and optional generated-age/contact receipts.
     * @pre LAYERED callers have refreshed derived unsat_mass; nacc_mass extent is species-major,
     * and generated_age/gained_contact are enabled/sized together.
     * @note Read-only accepted-state snapshot; no bounds/finite checks, allocation or mutation.
     * Serialize with producer/gather/ledger updates so pending inventories are sampled
     * consistently.
     */
    double continuityDenominator(int s) const noexcept {
        const auto us = static_cast<std::size_t>(s);
        double total = std::fabs(init_mass[us]) + std::fabs(gained_infil[us]) +
                       std::fabs(gained_node[us]) + std::fabs(gained_link[us]) +
                       std::fabs(net_lateral[us]) + std::fabs(gained_source[us]);
        if (!generated_age.empty())
            total += std::abs(generated_age[us]) + std::abs(gained_contact[us]);
        return total;
    }

    /**
     * @brief T7.1's conservation statement, the twin of
     * `SurfaceTransportState::totalIncludingLedgers`: what the aquifer holds now, plus
     * everything that left, minus everything that arrived, equals what it started with. Zero to
     * machine precision for a conserving kernel — GW plan gate 6's stub.
     *
     * @param[in] s Valid zero-based species row with coherently sized mandatory and enabled
     * optional arrays.
     * @return Signed ledgered storage + cumulative outputs - cumulative inputs - initial
     * inventory, native concentration*m3; age s*m3 and temperature degrees C*m3.
     * @pre LAYERED callers have refreshed derived unsat_mass; nacc_mass extent is species-major,
     * and generated_age/gained_contact are enabled/sized together.
     * @note Read-only accepted-state snapshot; no bounds/finite checks, allocation or mutation.
     * Serialize with producer/gather/ledger updates so pending inventories are sampled
     * consistently.
     */
    double residual(int s) const noexcept {
        const auto us = static_cast<std::size_t>(s);
        const double out = lost_deep[us] + lost_node[us] + lost_link[us] + lost_dunne[us] +
                           lost_et[us] + lost_reaction[us] + lost_source[us];
        double in = gained_infil[us] + gained_node[us] + gained_link[us] + net_lateral[us] +
                    gained_source[us];
        if (!generated_age.empty())
            in += generated_age[us] + gained_contact[us];
        return ledgeredStorage(s) + out - in - init_mass[us];
    }
};

} // namespace openswmm::twoD

#endif // OPENSWMM_ENGINE_2D_SUBSURFACE_TRANSPORT_STATE_HPP
