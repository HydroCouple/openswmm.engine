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
 * @file BedZoneData.hpp
 * @brief Plan H6b — the bed / hyporheic transient-storage zone.
 *
 * @details A second, immobile water-and-sediment body beneath each conduit.
 *          It exchanges with the channel by conduction and by hyporheic
 *          advection, and it conducts to a deep-ground boundary held at a
 *          fixed or time-varying temperature. It is the zone
 *          HydroCouple's `HTSComponent` models as a coupled component; here
 *          it is internal state, because SWMM has no component to couple to.
 *
 * @par The reference, verbatim
 *      `HTSComponent/src/element.cpp:132-141`:
 *      @code
 *      mainChannelConductionHeat = alpha * W * L * (T_ch - T) * rho_s * c_s / depth;
 *      groundConductionHeat      = alpha * W * L * (T_gr - T) * rho_s * c_s / groundConductionDepth;
 *      mainChannelAdvectionHeat  = rho_w * c_p * Q_hts * (T_ch - T);
 *      DTDt = (cond_mc + cond_gr + adv + ext + rad*L*W) / (rho_s * c_s * V);
 *      @endcode
 *      and `elementoutput.cpp:104,117` hands the channel back **`-adv` and
 *      `-cond_mc`** — exact reciprocity. Only the ground term is a true
 *      source or sink; everything else moves energy between two bodies that
 *      this engine owns, so their sum is conserved and a gate asserts it.
 *
 * @par Three deliberate divergences from the reference, and why
 *      1. **Contact area is the WETTED PERIMETER, not the top width.**
 *         `HTSComponent` is an open-channel model where `W` is both. A SWMM
 *         conduit is usually a closed pipe, whose contact with the
 *         surrounding soil is `P = A/R`, and for a full circular pipe the
 *         top width is **zero** — the reference's spelling would switch the
 *         whole module off exactly when a surcharged pipe conducts most.
 *         This is the "clear conceptual issue" exception to parity.
 *      2. **Hyporheic exchange is a VELOCITY (m/s), not a discharge.**
 *         `HTSComponent` receives `mainChannelAdvectionCoeff` per element
 *         from a coupled subsurface model (`elementinput.cpp:368`). SWMM has
 *         no such supplier, and one absolute m³/s applied to every conduit in
 *         a network of mixed lengths is not a physical statement. `Q_hts =
 *         v_hyp · A_bed` scales with the element the way the conduction term
 *         already does.
 *      3. **Sediment properties default to the STREAMBED pair (1670 kg/m³,
 *         1807 J/kg/K)**, which is `HTSComponent`'s own
 *         (`htsmodel.cpp:50-51`) — deliberately NOT `ConductionConfig`'s
 *         1970/2758, which came from `GWComponent` and describes a
 *         bioretention soil column. `HeatData.hpp:128` already records that
 *         the two are different materials; this is the file that needs the
 *         other one.
 *
 * @see plans/transport/HEAT_TRANSPORT_PLAN.md §2.3, §6.5 D-H6b
 * @ingroup engine_data
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_DATA_BED_ZONE_DATA_HPP
#define OPENSWMM_ENGINE_DATA_BED_ZONE_DATA_HPP

#include <cstddef>
#include <vector>

namespace openswmm {

/**
 * @brief `[SEDIMENT_EXCHANGE]` — the bed zone's material and geometry.
 *
 * @details Every field has a defensible default, so `[HEAT_FLUXES]
 *          SEDIMENT_EXCHANGE ON` alone is a runnable configuration. Two
 *          fields deliberately do NOT: `ground_temp` and the initial bed
 *          temperature. See `has_ground_temp`.
 */
struct SedimentConfig {
    /// Bed thermal diffusivity alpha_sed, m²/s. `HTSComponent` takes this
    /// per element from a coupled model; 1.0e-6 is the usual saturated-sand
    /// value and is what the reference's own test decks carry.
    double thermal_diffusivity = 1.0e-6;

    /// Bed effective solute diffusivity D_sed, m²/s. ONE value for every
    /// species: `HTSComponent` carries a per-solute array
    /// (`sedSoluteDiffCoefficients`) fed element by element, which SWMM
    /// cannot supply. A per-species spelling is a later key, not a later
    /// meaning — the physics below is already per species.
    double solute_diffusivity = 1.0e-9;

    /// Bed layer thickness Y_hts, m. The conduction length between the
    /// channel and the bed's own mean temperature, AND the thickness that
    /// sets the bed's heat capacity.
    double bed_thickness = 0.20;

    /// Depth from the bed to the deep-ground boundary Y_gr, m. The reference
    /// defaults this to 0.01 m (`element.cpp:38`), which makes the ground
    /// term dominate everything else; that is a placeholder awaiting a
    /// coupled `GWComponent`, not a recommendation. 2 m is the depth below
    /// which the annual signal is small for typical soils.
    double ground_depth = 2.0;

    /// Deep-ground temperature T_gr, °C. Constant spelling.
    double ground_temp = 12.0;

    /// Set by the parser when the deck stated `GROUND_TEMPERATURE`. The
    /// module RUNS without it — 12 °C is a real number, not a sentinel — but
    /// the ground term is often the largest one in a buried pipe, so an
    /// unstated value is WARNED about rather than silently accepted. This is
    /// the `SolarConfig::has_timezone` precedent, not the `has_latitude` one:
    /// warn, do not refuse.
    bool has_ground_temp = false;

    /// `GROUND_TEMPERATURE TIMESERIES <name>` — index into `ctx.tables`,
    /// -1 when the constant spelling is in force.
    int ground_ts_index = -1;

    /// Hyporheic exchange velocity v_hyp, m/s, across the bed interface.
    /// `Q_hts = v_hyp * A_bed`. Defaults to 0 — conduction only — so a deck
    /// that turns the module on without naming an exchange rate gets the
    /// conduction the user asked for and no invented advection.
    double hyporheic_velocity = 0.0;

    /// Bed bulk density rho_sed, kg/m³ (`htsmodel.cpp:50`).
    double sed_density = 1670.0;

    /// Bed specific heat c_sed, J/kg/K (`htsmodel.cpp:51`).
    double sed_specific_heat = 1807.0;

    /// Initial bed temperature, °C. When the deck does not state one the
    /// bed starts at `ground_temp`, which is the only temperature in the
    /// configuration that describes the subsurface.
    double initial_temp = 12.0;
    bool   has_initial_temp = false;
};

/**
 * @brief Runtime bed state, one entry per LINK.
 *
 * @details Sized to links rather than to the active engine's own elements
 *          (ARD cells, LARD parcels) because the bed does not move and does
 *          not subdivide: it is a property of the conduit, and every engine
 *          can address it by link index without agreeing on a mesh.
 *
 * @warning Under `EULERIAN_ARD` this means the bed exchanges with the link's
 *          VOLUME-WEIGHTED MEAN temperature, not cell by cell, so a bed
 *          under a long conduit cannot resolve a front the way the water
 *          above it does. That is a real loss of resolution against the
 *          reference (whose HTS elements map 1:1 onto channel elements) and
 *          it is recorded here rather than in a handoff because the next
 *          person to add a cell-resolved bed needs to find it attached to
 *          the array they would have to widen.
 */
struct BedZoneState {
    std::vector<double> link_temp;   ///< [link], °C
    /// [link * n_species + s], concentration in the bed water, in each
    /// species' own units. Species-minor to match `node_mass_`'s layout in
    /// the ARD store, which is the binding most likely to index it.
    std::vector<double> link_conc;
    int  n_species = 0;
    bool seeded    = false;

    // ---- ARD (cell-resolved) bed. The per-link arrays above are the
    //      LEGACY and LARD substrate; under EULERIAN_ARD every CELL gets
    //      its own bed slice, 1:1 — which is exactly the reference's
    //      element mapping (HTSComponent pairs one HTS element with one
    //      channel element) and answers "which cell does the bed under a
    //      400 ft conduit exchange with?" with "its own". The two shapes
    //      coexist because engine choice is per model and this state is
    //      runtime-only (not persisted), so nothing ever has to convert
    //      one into the other.
    std::vector<double> cell_temp;   ///< [cell], °C
    /// SPECIES-MAJOR [s * n_cells + c] — `cell_phi`'s own layout, because
    /// the ARD binding walks both arrays in the same loop and two layouts
    /// in one loop is how a stride bug reads plausibly.
    std::vector<double> cell_conc;
    int  cell_n_species = 0;
    bool cells_seeded   = false;

    void resizeCells(int n_cells, int n_spec, double t_init) {
        const auto nc = static_cast<std::size_t>(n_cells > 0 ? n_cells : 0);
        const auto ns = static_cast<std::size_t>(n_spec > 0 ? n_spec : 0);
        cell_n_species = static_cast<int>(ns);
        cell_temp.assign(nc, t_init);
        cell_conc.assign(ns * nc, 0.0);
        cells_seeded = true;
    }

    void resize(int n_links, int n_spec, double t_init) {
        const auto nl = static_cast<std::size_t>(n_links > 0 ? n_links : 0);
        const auto ns = static_cast<std::size_t>(n_spec > 0 ? n_spec : 0);
        n_species = static_cast<int>(ns);
        link_temp.assign(nl, t_init);
        link_conc.assign(nl * ns, 0.0);
        seeded = false;
    }

    bool sized(int n_links) const noexcept {
        return link_temp.size() == static_cast<std::size_t>(n_links > 0 ? n_links : 0);
    }

    void clear() { *this = BedZoneState{}; }
};

}  // namespace openswmm

#endif  // OPENSWMM_ENGINE_DATA_BED_ZONE_DATA_HPP
