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
 * @license  MIT License
 */

#ifndef OPENSWMM_FORCING_DATA_HPP
#define OPENSWMM_FORCING_DATA_HPP

#include <cstdint>
#include <vector>

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

    // ------ Subcatchment forcing (sized to n_subcatches) --------------------

    std::vector<ForcingMode>    subcatch_rainfall_mode;
    std::vector<double>         subcatch_rainfall_value; ///< user units (in/hr or mm/hr)
    std::vector<ForcingPersist> subcatch_rainfall_persist;

    std::vector<ForcingMode>    subcatch_evap_mode;
    std::vector<double>         subcatch_evap_value;     ///< ft/sec (internal units)
    std::vector<ForcingPersist> subcatch_evap_persist;

    // ------ Gage forcing (sized to n_gages) ---------------------------------

    std::vector<ForcingMode>    gage_rainfall_mode;
    std::vector<double>         gage_rainfall_value;     ///< user units (in/hr or mm/hr)
    std::vector<ForcingPersist> gage_rainfall_persist;

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

        subcatch_rainfall_mode.assign(us, ForcingMode::NONE);
        subcatch_rainfall_value.assign(us, 0.0);
        subcatch_rainfall_persist.assign(us, ForcingPersist::RESET);

        subcatch_evap_mode.assign(us, ForcingMode::NONE);
        subcatch_evap_value.assign(us, 0.0);
        subcatch_evap_persist.assign(us, ForcingPersist::RESET);

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
        set_none(subcatch_rainfall_mode);
        set_none(subcatch_evap_mode);
        set_none(gage_rainfall_mode);
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
        clear_resets(subcatch_rainfall_mode,     subcatch_rainfall_persist);
        clear_resets(subcatch_evap_mode,         subcatch_evap_persist);
        clear_resets(gage_rainfall_mode,         gage_rainfall_persist);
    }
};

} // namespace openswmm

#endif // OPENSWMM_FORCING_DATA_HPP
