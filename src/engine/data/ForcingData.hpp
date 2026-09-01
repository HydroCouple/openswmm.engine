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
 * @file ForcingData.hpp
 * @brief Per-element runtime forcing state — SoA layout.
 *
 * @details Stores user-injected forcing mode, value, and persistence for every
 *          forceable quantity (lateral inflow, head boundary, rainfall,
 *          evaporation, link setting, quality mass flux). Each forcing channel
 *          has three parallel arrays:
 *            - mode   : NONE / OVERRIDE / ADD
 *            - value  : the user-supplied value
 *            - persist: RESET (auto-clear after each step) / PERSIST (keep)
 *
 *          Applied by SWMMEngine::applyForcings() at the start of each
 *          routing step. Auto-cleared by clear_reset_entries() at end of step.
 *
 * @see include/openswmm/engine/openswmm_forcing.h  (C API)
 * @see src/engine/core/SWMMEngine.cpp               (integration)
 * @ingroup engine_data
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_FORCING_DATA_HPP
#define OPENSWMM_FORCING_DATA_HPP

#include <cstdint>
#include <vector>

#include "HeatOverrideData.hpp"   // HeatElement — PE4

namespace openswmm {

// ============================================================================
// Forcing enums (C++ scoped — mirrors C API enums in openswmm_forcing.h)
// ============================================================================

enum class ForcingMode : int8_t {
    NONE     = 0,   ///< Use model-computed value (no forcing)
    OVERRIDE = 1,   ///< Replace computed value with user value
    ADD      = 2    ///< Add user value to computed value
};

enum class ForcingPersist : int8_t {
    RESET   = 0,    ///< Auto-clear after each timestep
    PERSIST = 1     ///< Keep until explicitly cleared
};

// ============================================================================
// ForcingData — SoA storage for all forcing channels
// ============================================================================

struct ForcingData {

    // ------ Node forcing (sized to n_nodes) ---------------------------------

    std::vector<ForcingMode>    node_lat_inflow_mode;
    std::vector<double>         node_lat_inflow_value;
    std::vector<ForcingPersist> node_lat_inflow_persist;

    std::vector<ForcingMode>    node_head_boundary_mode;
    std::vector<double>         node_head_boundary_value;
    std::vector<ForcingPersist> node_head_boundary_persist;

    // Node quality: flat 2D [node_idx * n_pollutants + pollutant_idx]
    std::vector<ForcingMode>    node_quality_mode;
    std::vector<double>         node_quality_value;     ///< mass rate (mass/sec)
    std::vector<ForcingPersist> node_quality_persist;

    // ------ Link forcing (sized to n_links) ---------------------------------

    std::vector<ForcingMode>    link_flow_mode;
    std::vector<double>         link_flow_value;
    std::vector<ForcingPersist> link_flow_persist;

    std::vector<ForcingMode>    link_setting_mode;
    std::vector<double>         link_setting_value;     ///< 0.0–1.0 for pump/orifice/weir
    std::vector<ForcingPersist> link_setting_persist;

    std::vector<ForcingMode>    link_quality_mode;      ///< flattened link × pollutant
    std::vector<double>         link_quality_value;     ///< OVERRIDE: concentration; ADD: mass rate (mass/sec)
    std::vector<ForcingPersist> link_quality_persist;

    // ------ Subcatchment forcing (sized to n_subcatches) --------------------

    std::vector<ForcingMode>    subcatch_rainfall_mode;
    std::vector<double>         subcatch_rainfall_value; ///< user units (in/hr or mm/hr)
    std::vector<ForcingPersist> subcatch_rainfall_persist;

    std::vector<ForcingMode>    subcatch_evap_mode;
    std::vector<double>         subcatch_evap_value;     ///< prescribed PET rate, ft/sec (internal units; converted from in/day or mm/day at the C API boundary)
    std::vector<ForcingPersist> subcatch_evap_persist;

    std::vector<ForcingMode>    subcatch_snowfall_mode;
    std::vector<double>         subcatch_snowfall_value; ///< ft/sec (internal; converted from in/hr or mm/hr at the C API boundary)
    std::vector<ForcingPersist> subcatch_snowfall_persist;

    // ------ Gage forcing (sized to n_gages) ---------------------------------

    std::vector<ForcingMode>    gage_rainfall_mode;
    std::vector<double>         gage_rainfall_value;     ///< user units (in/hr or mm/hr)
    std::vector<ForcingPersist> gage_rainfall_persist;

    // ------ Climate forcing (scalar — system-wide) ---------------------------

    ForcingMode    climate_temperature_mode    = ForcingMode::NONE;
    double         climate_temperature_value   = 0.0;  ///< deg F (internal; converted from deg C at the C API boundary for SI)
    ForcingPersist climate_temperature_persist = ForcingPersist::RESET;

    ForcingMode    climate_wind_mode    = ForcingMode::NONE;
    double         climate_wind_value   = 0.0;         ///< mph (internal; converted from km/hr at the C API boundary for SI)
    ForcingPersist climate_wind_persist = ForcingPersist::RESET;

    ForcingMode    climate_evap_mode    = ForcingMode::NONE;
    double         climate_evap_value   = 0.0;         ///< ft/sec (internal; converted from in/day or mm/day at the C API boundary)
    ForcingPersist climate_evap_persist = ForcingPersist::RESET;

    // ------ PE4: per-ELEMENT climate (API only; no deck syntax) -------------
    //
    // Air temperature, humidity, wind and incoming shortwave are GLOBAL in
    // every deck and per-element ONLY through this API. The caller is a
    // coupled driver — an MCP session, a calibration loop, or a HydroCouple
    // composition where an atmospheric or riparian-shade model owns the
    // near-surface field — asserting the heterogeneity deliberately, per
    // step, and answerable for it. That is how the reference works too:
    // RHEComponent and CSHComponent receive per-element meteorology through
    // EXCHANGE ITEMS, never through their input files.
    //
    // ⚠ These are resolved AT THE FLUX CALL (SurfaceExchange/RadiativeExchange),
    // never written into ClimateState. That is the whole safety property:
    // ClimateState is shared with hydrology, snowmelt and evaporation, and a
    // per-link push must not reach them. A "simplification" that assigned
    // these into climate_state would silently give the snowpack above a
    // conduit that conduit's air temperature.
    //
    // Scope is LINK and NODE only. Neither has a competing consumer for air
    // temperature — snowmelt runs on subcatchments, evaporation on
    // subcatchment and storage surfaces — so no subsystem divergence is
    // possible. SUBCATCH is refused at the API boundary, naming that reason.
    //
    // Sized lazily on first use (the D-PE2 pattern): a model that never
    // calls these allocates nothing and pays one `.empty()` check per flux.
    struct ElemClimateChannel {
        std::vector<ForcingMode>    mode;
        std::vector<double>         value;
        std::vector<ForcingPersist> persist;

        void ensure(std::size_t n) {
            if (mode.size() == n) return;
            mode.assign(n, ForcingMode::NONE);
            value.assign(n, 0.0);
            persist.assign(n, ForcingPersist::RESET);
        }
        /// Apply this channel at `i` to `base`; `base` when unset.
        double apply(int i, double base) const noexcept {
            if (i < 0) return base;
            const auto u = static_cast<std::size_t>(i);
            if (u >= mode.size() || mode[u] == ForcingMode::NONE) return base;
            return (mode[u] == ForcingMode::OVERRIDE) ? value[u]
                                                      : base + value[u];
        }
        void resetPerStep() {
            for (std::size_t i = 0; i < mode.size(); ++i)
                if (persist[i] == ForcingPersist::RESET)
                    mode[i] = ForcingMode::NONE;
        }
        void clear() { *this = ElemClimateChannel{}; }
    };

    /// [0] = LINK, [1] = NODE. Indexed by `elemSlot` below so the two kinds
    /// cannot be confused at a call site.
    ElemClimateChannel elem_air_temp[2];   ///< deg F
    ElemClimateChannel elem_humidity[2];   ///< %
    ElemClimateChannel elem_wind[2];       ///< mph
    ElemClimateChannel elem_shortwave[2];  ///< W/m2

    /// -1 for a kind that carries no per-element climate (SUBCATCH, LID).
    static int elemSlot(HeatElemKind k) noexcept {
        if (k == HeatElemKind::LINK) return 0;
        if (k == HeatElemKind::NODE) return 1;
        return -1;
    }

    double elementAirTempF(const HeatElement& e, double base) const noexcept {
        const int s = elemSlot(e.kind);
        return (s < 0) ? base : elem_air_temp[s].apply(e.index, base);
    }
    double elementHumidity(const HeatElement& e, double base) const noexcept {
        const int s = elemSlot(e.kind);
        return (s < 0) ? base : elem_humidity[s].apply(e.index, base);
    }
    double elementWindMph(const HeatElement& e, double base) const noexcept {
        const int s = elemSlot(e.kind);
        return (s < 0) ? base : elem_wind[s].apply(e.index, base);
    }
    double elementShortwave(const HeatElement& e, double base) const noexcept {
        const int s = elemSlot(e.kind);
        return (s < 0) ? base : elem_shortwave[s].apply(e.index, base);
    }

    // ------ Counts (for iteration) ------------------------------------------

    int n_nodes_      = 0;
    int n_links_      = 0;
    int n_subcatches_ = 0;
    int n_gages_      = 0;
    int n_pollutants_ = 0;

    // ========================================================================
    // Methods
    // ========================================================================

    /**
     * @brief Allocate all arrays and initialise to NONE / 0 / RESET.
     */
    void resize(int n_nodes, int n_links, int n_subcatches,
                int n_gages, int n_pollutants) {
        n_nodes_      = n_nodes;
        n_links_      = n_links;
        n_subcatches_ = n_subcatches;
        n_gages_      = n_gages;
        n_pollutants_ = n_pollutants;

        auto un  = static_cast<std::size_t>(n_nodes);
        auto ul  = static_cast<std::size_t>(n_links);
        auto us  = static_cast<std::size_t>(n_subcatches);
        auto ug  = static_cast<std::size_t>(n_gages);
        auto unp = static_cast<std::size_t>(n_nodes) *
                   static_cast<std::size_t>(n_pollutants);

        node_lat_inflow_mode.assign(un, ForcingMode::NONE);
        node_lat_inflow_value.assign(un, 0.0);
        node_lat_inflow_persist.assign(un, ForcingPersist::RESET);

        node_head_boundary_mode.assign(un, ForcingMode::NONE);
        node_head_boundary_value.assign(un, 0.0);
        node_head_boundary_persist.assign(un, ForcingPersist::RESET);

        node_quality_mode.assign(unp, ForcingMode::NONE);
        node_quality_value.assign(unp, 0.0);
        node_quality_persist.assign(unp, ForcingPersist::RESET);

        link_flow_mode.assign(ul, ForcingMode::NONE);
        link_flow_value.assign(ul, 0.0);
        link_flow_persist.assign(ul, ForcingPersist::RESET);

        link_setting_mode.assign(ul, ForcingMode::NONE);
        link_setting_value.assign(ul, 0.0);
        link_setting_persist.assign(ul, ForcingPersist::RESET);

        auto ulp = static_cast<std::size_t>(n_links) *
                   static_cast<std::size_t>(n_pollutants);
        link_quality_mode.assign(ulp, ForcingMode::NONE);
        link_quality_value.assign(ulp, 0.0);
        link_quality_persist.assign(ulp, ForcingPersist::RESET);

        subcatch_rainfall_mode.assign(us, ForcingMode::NONE);
        subcatch_rainfall_value.assign(us, 0.0);
        subcatch_rainfall_persist.assign(us, ForcingPersist::RESET);

        subcatch_evap_mode.assign(us, ForcingMode::NONE);
        subcatch_evap_value.assign(us, 0.0);
        subcatch_evap_persist.assign(us, ForcingPersist::RESET);

        subcatch_snowfall_mode.assign(us, ForcingMode::NONE);
        subcatch_snowfall_value.assign(us, 0.0);
        subcatch_snowfall_persist.assign(us, ForcingPersist::RESET);

        gage_rainfall_mode.assign(ug, ForcingMode::NONE);
        gage_rainfall_value.assign(ug, 0.0);
        gage_rainfall_persist.assign(ug, ForcingPersist::RESET);
    }

    /**
     * @brief Reset ALL forcing modes to NONE (called on simulation restart).
     */
    void clear_all() {
        auto set_none = [](auto& mode_vec) {
            for (auto& m : mode_vec) m = ForcingMode::NONE;
        };
        set_none(node_lat_inflow_mode);
        set_none(node_head_boundary_mode);
        set_none(node_quality_mode);
        set_none(link_flow_mode);
        set_none(link_setting_mode);
        set_none(link_quality_mode);
        set_none(subcatch_rainfall_mode);
        set_none(subcatch_evap_mode);
        set_none(subcatch_snowfall_mode);
        set_none(gage_rainfall_mode);
        climate_temperature_mode = ForcingMode::NONE;
        climate_wind_mode        = ForcingMode::NONE;
        climate_evap_mode        = ForcingMode::NONE;
    }

    /**
     * @brief Clear only RESET-persistence entries (called at end of each step).
     *
     * @details After the forcing has been applied for this timestep, entries
     *          with RESET persistence are set back to NONE so they do not
     *          carry forward to the next step. PERSIST entries are untouched.
     */
    void clear_reset_entries() {
        auto clear_resets = [](auto& mode_vec, const auto& persist_vec) {
            for (std::size_t i = 0; i < mode_vec.size(); ++i) {
                if (persist_vec[i] == ForcingPersist::RESET)
                    mode_vec[i] = ForcingMode::NONE;
            }
        };
        clear_resets(node_lat_inflow_mode,      node_lat_inflow_persist);
        clear_resets(node_head_boundary_mode,    node_head_boundary_persist);
        clear_resets(node_quality_mode,          node_quality_persist);
        clear_resets(link_flow_mode,             link_flow_persist);
        clear_resets(link_setting_mode,          link_setting_persist);
        clear_resets(link_quality_mode,          link_quality_persist);
        clear_resets(subcatch_rainfall_mode,     subcatch_rainfall_persist);
        clear_resets(subcatch_evap_mode,         subcatch_evap_persist);
        clear_resets(subcatch_snowfall_mode,     subcatch_snowfall_persist);
        clear_resets(gage_rainfall_mode,         gage_rainfall_persist);
        if (climate_temperature_persist == ForcingPersist::RESET)
            climate_temperature_mode = ForcingMode::NONE;
        if (climate_wind_persist == ForcingPersist::RESET)
            climate_wind_mode = ForcingMode::NONE;
        if (climate_evap_persist == ForcingPersist::RESET)
            climate_evap_mode = ForcingMode::NONE;
        // PE4: the per-element channels join the SAME sweep, so RESET
        // semantics cannot drift between the global and element spellings.
        // RESET is the documented default for a coupled driver: under
        // PERSIST, a driver that pushes on some steps and not others
        // silently reuses a stale field that looks like data and is hours
        // old. Under RESET the value falls back to the global broadcast the
        // moment the driver stops feeding it.
        for (int k = 0; k < 2; ++k) {
            elem_air_temp[k].resetPerStep();
            elem_humidity[k].resetPerStep();
            elem_wind[k].resetPerStep();
            elem_shortwave[k].resetPerStep();
        }
    }

    /**
     * @brief Resolve the effective air temperature (deg F internal).
     *
     * @param broadcast  Climate-derived temperature (deg F).
     * @return Prescribed (OVERRIDE), augmented (ADD), or broadcast value.
     */
    double effective_temperature(double broadcast) const noexcept {
        switch (climate_temperature_mode) {
            case ForcingMode::OVERRIDE: return climate_temperature_value;
            case ForcingMode::ADD:      return broadcast + climate_temperature_value;
            default:                    return broadcast;
        }
    }

    /**
     * @brief Resolve the effective wind speed (mph internal).
     *
     * @param broadcast  Climate-derived wind speed (mph).
     * @return Prescribed (OVERRIDE), augmented (ADD), or broadcast value.
     */
    double effective_wind(double broadcast) const noexcept {
        switch (climate_wind_mode) {
            case ForcingMode::OVERRIDE: return climate_wind_value;
            case ForcingMode::ADD:      return broadcast + climate_wind_value;
            default:                    return broadcast;
        }
    }

    /**
     * @brief Resolve the effective rainfall for a subcatchment.
     *
     * Applies any subcatchment rainfall forcing to the gage-derived rate.
     * Units are the caller's (user units, in/hr or mm/hr — matching the
     * C API contract of swmm_forcing_subcatch_rainfall).
     *
     * @param ui             Subcatchment index.
     * @param gage_rainfall  Gage-derived rainfall in user units.
     * @return Effective rainfall in user units.
     */
    double effective_rainfall(std::size_t ui, double gage_rainfall) const noexcept {
        if (ui >= subcatch_rainfall_mode.size()) return gage_rainfall;
        switch (subcatch_rainfall_mode[ui]) {
            case ForcingMode::OVERRIDE: return subcatch_rainfall_value[ui];
            case ForcingMode::ADD:      return gage_rainfall + subcatch_rainfall_value[ui];
            default:                    return gage_rainfall;
        }
    }

    /**
     * @brief Resolve the effective system-wide evaporation rate (ft/sec).
     *
     * @param broadcast  Climate-derived evap rate (ft/sec), post-adjustment.
     * @return Prescribed (OVERRIDE), augmented (ADD), or broadcast value.
     */
    double effective_climate_evap(double broadcast) const noexcept {
        switch (climate_evap_mode) {
            case ForcingMode::OVERRIDE: return climate_evap_value;
            case ForcingMode::ADD:      return broadcast + climate_evap_value;
            default:                    return broadcast;
        }
    }

    /**
     * @brief Resolve the effective snowfall for a subcatchment (ft/sec).
     *
     * Applies any subcatchment snowfall forcing to the gage-derived
     * (temperature-split) snowfall rate.
     *
     * @param ui             Subcatchment index.
     * @param gage_snowfall  Gage-derived snowfall (ft/sec).
     * @return Effective snowfall (ft/sec).
     */
    double effective_snowfall(std::size_t ui, double gage_snowfall) const noexcept {
        if (ui >= subcatch_snowfall_mode.size()) return gage_snowfall;
        switch (subcatch_snowfall_mode[ui]) {
            case ForcingMode::OVERRIDE: return subcatch_snowfall_value[ui];
            case ForcingMode::ADD:      return gage_snowfall + subcatch_snowfall_value[ui];
            default:                    return gage_snowfall;
        }
    }

    /**
     * @brief Resolve the effective evaporation rate for a subcatchment.
     *
     * Applies any prescribed PET forcing to the broadcast climate rate.
     * An OVERRIDE prescription is used as-is (it replaces the climate
     * rate, including any DRY_ONLY suppression already folded into
     * @p broadcast_rate by the caller); ADD augments it.
     *
     * @param ui              Subcatchment index.
     * @param broadcast_rate  Climate-derived evap rate (ft/sec), after any
     *                        caller-side DRY_ONLY handling.
     * @return Effective potential evaporation rate (ft/sec).
     */
    double effective_evap_rate(std::size_t ui, double broadcast_rate) const noexcept {
        // Total over unallocated forcing: callers (Runoff, Groundwater, LID)
        // run against hand-built / partially-initialized contexts in tests
        // and via the builder API before the forcing arrays are sized.
        if (ui >= subcatch_evap_mode.size()) return broadcast_rate;
        switch (subcatch_evap_mode[ui]) {
            case ForcingMode::OVERRIDE: return subcatch_evap_value[ui];
            case ForcingMode::ADD:      return broadcast_rate + subcatch_evap_value[ui];
            default:                    return broadcast_rate;
        }
    }
};

} // namespace openswmm

#endif // OPENSWMM_FORCING_DATA_HPP
