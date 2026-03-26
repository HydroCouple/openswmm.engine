/**
 * @file SubcatchData.hpp
 * @brief Structure-of-Arrays (SoA) storage for subcatchments.
 *
 * @details Replaces the global `Subcatch[]` array and `TSubcatch` struct from
 *          src/solver/objects.h. Subcatchments produce runoff that drains into
 *          nodes or other subcatchments.
 *
 * @see Legacy reference: src/solver/objects.h  — TSubcatch
 * @see Legacy reference: src/solver/globals.h  — Subcatch[], Nsubcatch
 * @ingroup engine_data
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_ENGINE_SUBCATCH_DATA_HPP
#define OPENSWMM_ENGINE_SUBCATCH_DATA_HPP

#include <vector>
#include <cstdint>

namespace openswmm {

// ============================================================================
// SubcatchData — SoA layout
// ============================================================================

/**
 * @brief Structure-of-Arrays storage for all subcatchments.
 *
 * @details Pollutant-related arrays are stored separately in PollutantData,
 *          indexed as [subcatch_idx * n_pollutants + pollutant_idx].
 *
 * @ingroup engine_data
 */
struct SubcatchData {

    // -----------------------------------------------------------------------
    // Static properties — set at parse time
    // -----------------------------------------------------------------------

    /**
     * @brief Index of the drain-to node or subcatchment.
     * @details >= 0 means drains to a node; negative value used internally to
     *          encode a subcatchment drain-to relationship.
     * @see Legacy: Subcatch[i].outNode / Subcatch[i].outSubcatch
     */
    std::vector<int>    outlet_node;

    /**
     * @brief Index of the subcatch that receives overflow (-1 if none).
     * @see Legacy: Subcatch[i].outSubcatch
     */
    std::vector<int>    outlet_subcatch;

    /**
     * @brief Rain gage index for this subcatchment.
     * @see Legacy: Subcatch[i].gage
     */
    std::vector<int>    gage;

    /**
     * @brief Subcatchment area (project area units).
     * @see Legacy: Subcatch[i].area
     */
    std::vector<double> area;

    /**
     * @brief Width of overland flow path (project length units).
     * @see Legacy: Subcatch[i].width
     */
    std::vector<double> width;

    /**
     * @brief Average slope of catchment (fraction).
     * @see Legacy: Subcatch[i].slope
     */
    std::vector<double> slope;

    /**
     * @brief Total curb length (project length units).
     * @see Legacy: Subcatch[i].curbLength
     */
    std::vector<double> curb_length;

    /**
     * @brief Fraction of area that is impervious (0–1).
     * @see Legacy: Subcatch[i].fracImperv
     */
    std::vector<double> frac_imperv;

    /**
     * @brief Fraction of impervious area with no depression storage (0–1).
     * @see Legacy: Subcatch[i].fracImperv2  (EPA calls it fracStoreImperv)
     */
    std::vector<double> frac_imperv_no_store;

    /**
     * @brief Manning's n for impervious area.
     * @see Legacy: Subcatch[i].subArea[IMPERV0].N
     */
    std::vector<double> n_imperv;

    /**
     * @brief Manning's n for pervious area.
     * @see Legacy: Subcatch[i].subArea[PERV].N
     */
    std::vector<double> n_perv;

    /**
     * @brief Depression storage depth for impervious area (project length units).
     * @see Legacy: Subcatch[i].subArea[IMPERV0].dStore
     */
    std::vector<double> ds_imperv;

    /**
     * @brief Depression storage depth for pervious area (project length units).
     * @see Legacy: Subcatch[i].subArea[PERV].dStore
     */
    std::vector<double> ds_perv;

    // -----------------------------------------------------------------------
    // Infiltration parameters (per subcatchment)
    // -----------------------------------------------------------------------

    /** @brief Infiltration model: 0=HORTON, 1=MOD_HORTON, 2=GREEN_AMPT, 3=MOD_GREEN_AMPT, 4=CURVE_NUMBER */
    std::vector<int>    infil_model;

    /** @brief Infiltration param 1: f0 (Horton), suction (GA), CN (CN). */
    std::vector<double> infil_p1;
    /** @brief Infiltration param 2: fmin (Horton), conductivity (GA), 0 (CN). */
    std::vector<double> infil_p2;
    /** @brief Infiltration param 3: decay (Horton), initial deficit (GA), 0 (CN). */
    std::vector<double> infil_p3;
    /** @brief Infiltration param 4: dry time (Horton), 0 (GA/CN). */
    std::vector<double> infil_p4;
    /** @brief Infiltration param 5: max infil (Horton), 0 (GA/CN). */
    std::vector<double> infil_p5;

    // -----------------------------------------------------------------------
    // State variables — updated each timestep
    // -----------------------------------------------------------------------

    /**
     * @brief Current total runoff flow rate (project flow units).
     * @see Legacy: Subcatch[i].newRunoff
     */
    std::vector<double> runoff;

    /**
     * @brief Current total rainfall depth rate (project length/time units).
     * @see Legacy: Subcatch[i].rainfall
     */
    std::vector<double> rainfall;

    /**
     * @brief Current evaporation loss rate (project length/time units).
     * @see Legacy: Subcatch[i].evapLoss
     */
    std::vector<double> evap_loss;

    /**
     * @brief Current infiltration loss rate (project length/time units).
     * @see Legacy: Subcatch[i].infilLoss
     */
    std::vector<double> infil_loss;

    /**
     * @brief Current total ponded depth over subcatchment (project length units).
     * @see Legacy: Subcatch[i].newSnowDepth (repurposed for liquid depth)
     */
    std::vector<double> ponded_depth;

    /**
     * @brief Groundwater outflow rate (project flow units).
     * @see Legacy: Subcatch[i].groundwater->newFlow
     */
    std::vector<double> gw_flow;

    // -----------------------------------------------------------------------
    // Previous-step state
    // -----------------------------------------------------------------------

    /** @brief Runoff at the previous timestep. */
    std::vector<double> old_runoff;

    // -----------------------------------------------------------------------
    // Cumulative statistics
    // -----------------------------------------------------------------------

    /**
     * @brief Total precipitation volume (project volume units).
     * @see Legacy: SubcatchStats[i].precip
     */
    std::vector<double> stat_precip_vol;

    /**
     * @brief Total runoff volume (project volume units).
     * @see Legacy: SubcatchStats[i].runoff
     */
    std::vector<double> stat_runoff_vol;

    /**
     * @brief Maximum reported runoff rate (project flow units).
     * @see Legacy: SubcatchStats[i].maxFlow
     */
    std::vector<double> stat_max_runoff;

    // -----------------------------------------------------------------------
    // Groundwater parameters (from [GROUNDWATER] section)
    // -----------------------------------------------------------------------

    /** @brief Aquifer index for this subcatchment (-1 = none). */
    std::vector<int>    gw_aquifer;

    /** @brief Receiving node index for groundwater flow (-1 = none). */
    std::vector<int>    gw_node;

    /** @brief Surface elevation for groundwater calculations. */
    std::vector<double> gw_surf_elev;

    /** @brief Groundwater flow coefficient A1. */
    std::vector<double> gw_a1;

    /** @brief Groundwater flow exponent B1. */
    std::vector<double> gw_b1;

    /** @brief Groundwater flow coefficient A2. */
    std::vector<double> gw_a2;

    /** @brief Groundwater flow exponent B2. */
    std::vector<double> gw_b2;

    /** @brief Groundwater flow coefficient A3. */
    std::vector<double> gw_a3;

    /** @brief Threshold groundwater table elevation. */
    std::vector<double> gw_tw;

    /** @brief Water table elevation at which lateral GW flow ceases. */
    std::vector<double> gw_hstar;

    // -----------------------------------------------------------------------
    // Snowpack assignment (index into SnowpackStore, -1 = none)
    // -----------------------------------------------------------------------

    /** @brief Snowpack index for this subcatchment (-1 = none). */
    std::vector<int>    snowpack;

    // -----------------------------------------------------------------------
    // Cumulative pollutant washoff loads — flat 2D: [subcatch * n_pollutants + pollutant]
    // -----------------------------------------------------------------------

    /**
     * @brief Total washoff load per (subcatchment x pollutant) (mass units).
     * @see Legacy: Subcatch[i].totalLoad[p]
     */
    std::vector<double> total_load;

    /** @brief Number of pollutants in total_load matrix. */
    int total_load_n_pollutants = 0;

    void resize_total_load(int n_sc, int n_poll) {
        total_load_n_pollutants = n_poll;
        total_load.assign(
            static_cast<std::size_t>(n_sc) * static_cast<std::size_t>(n_poll), 0.0);
    }

    // -----------------------------------------------------------------------
    // Land-use coverage — flat 2D: [subcatch * n_landuses + landuse]
    // -----------------------------------------------------------------------

    /**
     * @brief Coverage fraction per (subcatchment x landuse).
     * @details Indexed as [sc_idx * n_landuses + lu_idx].  Values are 0–1.
     */
    std::vector<double> coverage;

    /** @brief Number of landuses stored in the coverage matrix. */
    int coverage_n_landuses = 0;

    /**
     * @brief Resize the coverage matrix.
     * @param n_sc      Number of subcatchments.
     * @param n_lu      Number of landuses.
     */
    void resize_coverage(int n_sc, int n_lu) {
        coverage_n_landuses = n_lu;
        coverage.assign(
            static_cast<std::size_t>(n_sc) * static_cast<std::size_t>(n_lu),
            0.0);
    }

    // -----------------------------------------------------------------------
    // Capacity management
    // -----------------------------------------------------------------------

    int count() const noexcept { return static_cast<int>(area.size()); }

    void resize(int n) {
        const auto un = static_cast<std::size_t>(n);

        outlet_node.assign(un, -1);
        outlet_subcatch.assign(un, -1);
        gage.assign(un, -1);
        area.assign(un, 0.0);
        width.assign(un, 0.0);
        slope.assign(un, 0.0);
        curb_length.assign(un, 0.0);
        frac_imperv.assign(un, 0.0);
        frac_imperv_no_store.assign(un, 0.0);
        n_imperv.assign(un, 0.013);
        n_perv.assign(un, 0.1);
        ds_imperv.assign(un, 0.0);
        ds_perv.assign(un, 0.0);

        infil_model.assign(un, 0);
        infil_p1.assign(un, 0.0);
        infil_p2.assign(un, 0.0);
        infil_p3.assign(un, 0.0);
        infil_p4.assign(un, 0.0);
        infil_p5.assign(un, 0.0);

        runoff.assign(un, 0.0);
        rainfall.assign(un, 0.0);
        evap_loss.assign(un, 0.0);
        infil_loss.assign(un, 0.0);
        ponded_depth.assign(un, 0.0);
        gw_flow.assign(un, 0.0);
        old_runoff.assign(un, 0.0);

        stat_precip_vol.assign(un, 0.0);
        stat_runoff_vol.assign(un, 0.0);
        stat_max_runoff.assign(un, 0.0);

        gw_aquifer.assign(un, -1);
        gw_node.assign(un, -1);
        gw_surf_elev.assign(un, 0.0);
        gw_a1.assign(un, 0.0);
        gw_b1.assign(un, 0.0);
        gw_a2.assign(un, 0.0);
        gw_b2.assign(un, 0.0);
        gw_a3.assign(un, 0.0);
        gw_tw.assign(un, 0.0);
        gw_hstar.assign(un, 0.0);
        snowpack.assign(un, -1);
    }

    void save_state() noexcept {
        old_runoff = runoff;
    }

    void reset_state() noexcept {
        std::fill(runoff.begin(),       runoff.end(),       0.0);
        std::fill(rainfall.begin(),     rainfall.end(),     0.0);
        std::fill(evap_loss.begin(),    evap_loss.end(),    0.0);
        std::fill(infil_loss.begin(),   infil_loss.end(),   0.0);
        std::fill(ponded_depth.begin(), ponded_depth.end(), 0.0);
        std::fill(old_runoff.begin(),   old_runoff.end(),   0.0);
    }
};

} /* namespace openswmm */

#endif /* OPENSWMM_ENGINE_SUBCATCH_DATA_HPP */
