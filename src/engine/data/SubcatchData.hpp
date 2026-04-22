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
#include <string>
#include <cstdint>
#include <algorithm>
#include <limits>

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
     * @brief Outlet name string for deferred resolution.
     * @details Stored during parsing so PostParseResolver can re-resolve
     *          outlet_node / outlet_subcatch when sections appear out of order.
     */
    std::vector<std::string> outlet_name;

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
    // Inter-subarea routing (from [SUBAREAS] section)
    // -----------------------------------------------------------------------

    /**
     * @brief Inter-subarea routing mode.
     * @details 0 = TO_OUTLET (all runoff goes to outlet),
     *          1 = TO_IMPERV (pervious → impervious),
     *          2 = TO_PERV   (impervious → pervious).
     * @see Legacy: Subcatch[i].subArea[k].routeTo
     */
    std::vector<int>    subarea_routing;

    /**
     * @brief Fraction of runoff routed between subareas (0–1).
     * @details The remainder (1 - pct_routed) goes to the outlet.
     *          fOutlet = 1 - pct_routed for the routed subarea.
     * @see Legacy: Subcatch[i].subArea[k].fOutlet = 1 - pctRouted
     */
    std::vector<double> pct_routed;

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

    /** @brief GW flow at the previous runoff evaluation (for interpolation). */
    std::vector<double> old_gw_flow;

    // -----------------------------------------------------------------------
    // Runon coupling — subcatch-to-subcatch routing
    // -----------------------------------------------------------------------

    /** @brief Runoff inflow from upstream subcatchments (project flow units).
     *  @details Assembled by assembleRunon(); zero if no upstream subcatch. */
    std::vector<double> runon_inflow;

    /** @brief Previous-step runon inflow (for interpolation). */
    std::vector<double> old_runon_inflow;

    /** @brief Gap #28: accumulated outfall-routed volume (ft³) between runoff steps.
     *  @details Matches legacy Outfall[i].vRouted. Drained to runon_inflow at
     *           each assembleRunon() call, then reset to 0. */
    std::vector<double> outfall_runon_vol;

    // -----------------------------------------------------------------------
    // Groundwater surface water head coupling
    // -----------------------------------------------------------------------

    /** @brief Surface water head at GW receiving node (project length units).
     *  @details Assembled from nodes.depth + nodes.invert_elev - aquifer bottom. */
    std::vector<double> gw_sw_head;

    /** @brief Available node flow for GW negative flow limit (cfs/ft2). */
    std::vector<double> gw_node_avail_flow;

    /**
     * @brief Gap #40: max infiltration volume (ft) upper GW zone can accept next step.
     * @details = (total_depth - lower_depth) * (porosity - theta) / frac_perv.
     *          Updated after each GW step; used by Runoff to cap infil rate (vol/dt).
     *          DBL_MAX (no constraint) when no GW is active.
     */
    std::vector<double> gw_max_infil_vol;

    // -----------------------------------------------------------------------
    // Quality washoff output
    // -----------------------------------------------------------------------

    /** @brief Washoff mass rate per (subcatch, pollutant) (mass/sec).
     *  @details Flat 2D: [subcatch * n_pollutants + pollutant]. */
    std::vector<double> washoff_load;

    // -----------------------------------------------------------------------
    // Per-subcatch quality state — flat 2D: [subcatch * n_pollutants + pollutant]
    // -----------------------------------------------------------------------

    /**
     * @brief Current quality concentration in subcatchment runoff.
     * @details Size = n_subcatches * n_pollutants.
     * @see Legacy: Subcatch[i].newQual[]
     */
    std::vector<double> conc;

    /** @brief Previous-step quality in subcatchment runoff. */
    std::vector<double> conc_old;

    /** @brief Ponded surface water quality mass per (subcatch, pollutant).
     *  @details Persists between wet/dry events. Updated each timestep as:
     *           wPonded = pondedQual + rain_deposition + runon_load
     *           cPonded = wPonded / V_inflow
     *           pondedQual_new = cPonded * ponded_depth * non_lid_area
     *  @see Legacy: Subcatch[i].pondedQual[] */
    std::vector<double> ponded_qual;

    /** @brief Number of pollutants in the quality arrays. */
    int                 conc_n_pollutants = 0;

    // -----------------------------------------------------------------------
    // Report flag — per-object output filter
    // -----------------------------------------------------------------------

    /** @brief Whether this subcatchment is included in report/output (0=no, 1=yes).
     *  @see Legacy: Subcatch[j].rptFlag */
    std::vector<char>       rpt_flag;

    // -----------------------------------------------------------------------
    // Cumulative statistics
    // -----------------------------------------------------------------------

    /**
     * @brief Total precipitation volume (project volume units).
     * @see Legacy: SubcatchStats[i].precip
     */
    std::vector<double> stat_precip_vol;
    std::vector<double> stat_evap_vol;     ///< Cumulative evaporation volume (ft3)
    std::vector<double> stat_infil_vol;    ///< Cumulative infiltration volume (ft3)
    std::vector<double> stat_imperv_vol;   ///< Cumulative impervious runoff volume (ft3)
    std::vector<double> stat_perv_vol;     ///< Cumulative pervious runoff volume (ft3)

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

    // Gap #63: Per-subcatch groundwater statistics (accumulated each GW step)
    std::vector<double> stat_gw_infil_vol;      ///< Cumulative infiltration to GW (ft³)
    std::vector<double> stat_gw_upper_evap_vol; ///< Cumulative upper zone evap (ft³)
    std::vector<double> stat_gw_lower_evap_vol; ///< Cumulative lower zone evap (ft³)
    std::vector<double> stat_gw_deep_perc_vol;  ///< Cumulative deep percolation (ft³)
    std::vector<double> stat_gw_flow_vol;       ///< Cumulative lateral GW outflow (ft³)
    std::vector<double> stat_gw_max_flow;       ///< Peak lateral GW outflow (CFS)
    std::vector<double> stat_gw_sum_theta;      ///< Sum of upper-zone theta (for time-avg)
    std::vector<double> stat_gw_sum_depth;      ///< Sum of water table height (ft, for time-avg)
    std::vector<double> stat_gw_final_theta;    ///< Upper-zone theta at last GW step
    std::vector<double> stat_gw_final_depth;    ///< Water table height at last GW step (ft)
    std::vector<long>   stat_gw_steps;          ///< Step count for GW averages

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

    /**
     * @brief Snow-modified net precipitation for impervious subareas (ft/sec).
     * @details Set by SWMMEngine after each snow execute step.
     *          Combines plowable + non-plowable imperv: imelt + rainfall*(1-asc).
     *          -1.0 means no snowpack active (use raw rainfall instead).
     * @see Legacy: netPrecip[SNOW_PLOWABLE/SNOW_IMPERV] in snow.c
     */
    std::vector<double> snow_net_imperv;

    /**
     * @brief Snow-modified net precipitation for pervious subarea (ft/sec).
     * @details Set by SWMMEngine after each snow execute step.
     *          = imelt_perv + rainfall*(1-asc_perv).
     *          -1.0 means no snowpack active (use raw rainfall instead).
     * @see Legacy: netPrecip[SNOW_PERV] in snow.c
     */
    std::vector<double> snow_net_perv;

    /**
     * @brief Total LID area for this subcatchment (ft²).
     * @details Sum of all LID unit areas (ft²) that belong to this subcatchment.
     *          Set by LIDSolver::init(). Used by snow plowing and snow cover
     *          calculations to exclude LID area (matching legacy Build 5.2.0).
     * @see Legacy: Subcatch[i].lidArea (snow.c Build 5.2.0 note)
     */
    std::vector<double> total_lid_area_ft2;

    /**
     * @brief LID surface return flow to pervious area (CFS).
     * @details Set by LIDSolver output routing when to_perv==1. Consumed by
     *          RunoffSolver as additional pervious-subarea inflow next step.
     *          Matches legacy lid_getFlowToPerv() called in subcatch_getRunon().
     * @see Legacy: lidUnit->surfaceOutflow (lid.c), lid_getFlowToPerv()
     */
    std::vector<double> lid_return_to_perv_cfs;

    /**
     * @brief LID drain flow routed to a target subcatchment (CFS).
     * @details Accumulated each runoff step when drain_subcatch >= 0.
     *          Drained into runon_inflow[] by assembleRunon() (after the clear),
     *          then reset, so the drain reaches the target as runon next step.
     *          Matches legacy lid_addDrainRunon() in lid.c.
     * @see Legacy: lid.c lid_addDrainRunon()
     */
    std::vector<double> lid_drain_runon_cfs;

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

    /** @brief Number of landuses stored in the coverage and sweep matrices. */
    int coverage_n_landuses = 0;

    /** @brief Gap #34: days since last swept per (subcatch x landuse).
     *  @details Flat 2D: [sc_idx * n_landuses + lu_idx].
     *           Matches legacy Subcatch[i].landFactor[j].lastSwept.
     *           Per-(subcatch,landuse) tracking so each subcatchment sweeps
     *           independently on its own schedule. */
    std::vector<double> sweep_last_swept;

    /**
     * @brief Resize the coverage and sweep matrices.
     * @param n_sc      Number of subcatchments.
     * @param n_lu      Number of landuses.
     */
    void resize_coverage(int n_sc, int n_lu) {
        coverage_n_landuses = n_lu;
        auto sz = static_cast<std::size_t>(n_sc) * static_cast<std::size_t>(n_lu);
        coverage.assign(sz, 0.0);
        sweep_last_swept.assign(sz, 0.0);
    }

    // -----------------------------------------------------------------------
    // Capacity management
    // -----------------------------------------------------------------------

    int count() const noexcept { return static_cast<int>(area.size()); }

    void resize(int n) {
        const auto un = static_cast<std::size_t>(n);

        outlet_node.assign(un, -1);
        outlet_subcatch.assign(un, -1);
        outlet_name.resize(un);
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
        subarea_routing.assign(un, 0);  // TO_OUTLET
        pct_routed.assign(un, 0.0);

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
        old_gw_flow.assign(un, 0.0);
        runon_inflow.assign(un, 0.0);
        old_runon_inflow.assign(un, 0.0);
        outfall_runon_vol.assign(un, 0.0);
        gw_sw_head.assign(un, 0.0);
        gw_node_avail_flow.assign(un, 0.0);
        gw_max_infil_vol.assign(un, std::numeric_limits<double>::max());

        rpt_flag.assign(un, 0);

        stat_precip_vol.assign(un, 0.0);
        stat_evap_vol.assign(un, 0.0);
        stat_infil_vol.assign(un, 0.0);
        stat_imperv_vol.assign(un, 0.0);
        stat_perv_vol.assign(un, 0.0);
        stat_runoff_vol.assign(un, 0.0);
        stat_max_runoff.assign(un, 0.0);
        stat_gw_infil_vol.assign(un, 0.0);
        stat_gw_upper_evap_vol.assign(un, 0.0);
        stat_gw_lower_evap_vol.assign(un, 0.0);
        stat_gw_deep_perc_vol.assign(un, 0.0);
        stat_gw_flow_vol.assign(un, 0.0);
        stat_gw_max_flow.assign(un, 0.0);
        stat_gw_sum_theta.assign(un, 0.0);
        stat_gw_sum_depth.assign(un, 0.0);
        stat_gw_final_theta.assign(un, 0.0);
        stat_gw_final_depth.assign(un, 0.0);
        stat_gw_steps.assign(un, 0L);

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
        snow_net_imperv.assign(un, -1.0);
        snow_net_perv.assign(un, -1.0);
        total_lid_area_ft2.assign(un, 0.0);
        lid_return_to_perv_cfs.assign(un, 0.0);
        lid_drain_runon_cfs.assign(un, 0.0);
    }

    /**
     * @brief Grow all arrays to hold at least `n` subcatchments, preserving existing data.
     */
    void grow_to(int n) {
        if (n <= count()) return;
        const auto un = static_cast<std::size_t>(n);
        auto g = [&](auto& vec, auto def) { vec.resize(un, def); };
        g(outlet_node, -1); g(outlet_subcatch, -1);
        outlet_name.resize(un); g(gage, -1);
        g(area, 0.0); g(width, 0.0); g(slope, 0.0); g(curb_length, 0.0);
        g(frac_imperv, 0.0); g(frac_imperv_no_store, 0.0);
        g(n_imperv, 0.013); g(n_perv, 0.1);
        g(ds_imperv, 0.0); g(ds_perv, 0.0);
        g(subarea_routing, 0); g(pct_routed, 0.0);
        g(infil_model, 0);
        g(infil_p1, 0.0); g(infil_p2, 0.0); g(infil_p3, 0.0);
        g(infil_p4, 0.0); g(infil_p5, 0.0);
        g(runoff, 0.0); g(rainfall, 0.0);
        g(evap_loss, 0.0); g(infil_loss, 0.0);
        g(ponded_depth, 0.0); g(gw_flow, 0.0);
        g(old_runoff, 0.0); g(old_gw_flow, 0.0);
        g(runon_inflow, 0.0); g(old_runon_inflow, 0.0);
        g(gw_sw_head, 0.0); g(gw_node_avail_flow, 0.0);
        g(gw_max_infil_vol, std::numeric_limits<double>::max());
        g(outfall_runon_vol, 0.0);
        g(rpt_flag, static_cast<char>(0));
        g(stat_precip_vol, 0.0); g(stat_evap_vol, 0.0);
        g(stat_infil_vol, 0.0); g(stat_imperv_vol, 0.0);
        g(stat_perv_vol, 0.0); g(stat_runoff_vol, 0.0);
        g(stat_max_runoff, 0.0);
        g(stat_gw_infil_vol, 0.0); g(stat_gw_upper_evap_vol, 0.0);
        g(stat_gw_lower_evap_vol, 0.0); g(stat_gw_deep_perc_vol, 0.0);
        g(stat_gw_flow_vol, 0.0); g(stat_gw_max_flow, 0.0);
        g(stat_gw_sum_theta, 0.0); g(stat_gw_sum_depth, 0.0);
        g(stat_gw_final_theta, 0.0); g(stat_gw_final_depth, 0.0);
        stat_gw_steps.resize(un, 0L);
        g(gw_aquifer, -1); g(gw_node, -1); g(gw_surf_elev, 0.0);
        g(gw_a1, 0.0); g(gw_b1, 0.0); g(gw_a2, 0.0); g(gw_b2, 0.0);
        g(gw_a3, 0.0); g(gw_tw, 0.0); g(gw_hstar, 0.0);
        g(snowpack, -1);
        g(snow_net_imperv, -1.0);
        g(snow_net_perv,   -1.0);
        g(total_lid_area_ft2, 0.0);
        g(lid_return_to_perv_cfs, 0.0);
        g(lid_drain_runon_cfs, 0.0);
        // Note: conc, conc_old, ponded_qual, washoff_load handled by resize_quality()
        // Note: coverage, total_load handled separately
    }

    /**
     * @brief Resize per-subcatch quality arrays after pollutant count is known.
     */
    void resize_quality(int n_pollutants) {
        conc_n_pollutants = n_pollutants;
        if (n_pollutants > 0) {
            auto total = static_cast<std::size_t>(count()) *
                         static_cast<std::size_t>(n_pollutants);
            conc.assign(total, 0.0);
            conc_old.assign(total, 0.0);
            ponded_qual.assign(total, 0.0);
            washoff_load.assign(total, 0.0);
        }
    }

    void resize_washoff_load(int n_pollutants) {
        if (n_pollutants > 0) {
            washoff_load.assign(
                static_cast<std::size_t>(count()) *
                static_cast<std::size_t>(n_pollutants), 0.0);
        }
    }

    /**
     * @brief Release excess vector capacity accumulated during parsing.
     */
    void shrink_to_fit() {
        outlet_node.shrink_to_fit();
        outlet_subcatch.shrink_to_fit();
        outlet_name.shrink_to_fit();
        gage.shrink_to_fit();
        area.shrink_to_fit();
        width.shrink_to_fit();
        slope.shrink_to_fit();
        curb_length.shrink_to_fit();
        frac_imperv.shrink_to_fit();
        frac_imperv_no_store.shrink_to_fit();
        n_imperv.shrink_to_fit();
        n_perv.shrink_to_fit();
        ds_imperv.shrink_to_fit();
        ds_perv.shrink_to_fit();
        subarea_routing.shrink_to_fit();
        pct_routed.shrink_to_fit();

        infil_model.shrink_to_fit();
        infil_p1.shrink_to_fit();
        infil_p2.shrink_to_fit();
        infil_p3.shrink_to_fit();
        infil_p4.shrink_to_fit();
        infil_p5.shrink_to_fit();

        runoff.shrink_to_fit();
        rainfall.shrink_to_fit();
        evap_loss.shrink_to_fit();
        infil_loss.shrink_to_fit();
        ponded_depth.shrink_to_fit();
        gw_flow.shrink_to_fit();
        old_runoff.shrink_to_fit();
        old_gw_flow.shrink_to_fit();
        runon_inflow.shrink_to_fit();
        old_runon_inflow.shrink_to_fit();
        outfall_runon_vol.shrink_to_fit();
        gw_sw_head.shrink_to_fit();
        gw_node_avail_flow.shrink_to_fit();
        gw_max_infil_vol.shrink_to_fit();

        rpt_flag.shrink_to_fit();

        stat_precip_vol.shrink_to_fit();
        stat_evap_vol.shrink_to_fit();
        stat_infil_vol.shrink_to_fit();
        stat_imperv_vol.shrink_to_fit();
        stat_perv_vol.shrink_to_fit();
        stat_runoff_vol.shrink_to_fit();
        stat_max_runoff.shrink_to_fit();

        gw_aquifer.shrink_to_fit();
        gw_node.shrink_to_fit();
        gw_surf_elev.shrink_to_fit();
        gw_a1.shrink_to_fit();
        gw_b1.shrink_to_fit();
        gw_a2.shrink_to_fit();
        gw_b2.shrink_to_fit();
        gw_a3.shrink_to_fit();
        gw_tw.shrink_to_fit();
        gw_hstar.shrink_to_fit();
        snowpack.shrink_to_fit();
        snow_net_imperv.shrink_to_fit();
        snow_net_perv.shrink_to_fit();

        conc.shrink_to_fit();
        conc_old.shrink_to_fit();
        ponded_qual.shrink_to_fit();
        washoff_load.shrink_to_fit();
        total_load.shrink_to_fit();
        coverage.shrink_to_fit();
        sweep_last_swept.shrink_to_fit();
    }

    void save_state() noexcept {
        std::copy(runoff.begin(),        runoff.end(),        old_runoff.begin());
        std::copy(runon_inflow.begin(),  runon_inflow.end(),  old_runon_inflow.begin());
        std::copy(conc.begin(),          conc.end(),          conc_old.begin());
    }

    void reset_state() noexcept {
        std::fill(runoff.begin(),       runoff.end(),       0.0);
        std::fill(rainfall.begin(),     rainfall.end(),     0.0);
        std::fill(snow_net_imperv.begin(), snow_net_imperv.end(), -1.0);
        std::fill(snow_net_perv.begin(),   snow_net_perv.end(),   -1.0);
        std::fill(evap_loss.begin(),    evap_loss.end(),    0.0);
        std::fill(infil_loss.begin(),   infil_loss.end(),   0.0);
        std::fill(ponded_depth.begin(), ponded_depth.end(), 0.0);
        std::fill(old_runoff.begin(),   old_runoff.end(),   0.0);
        std::fill(old_gw_flow.begin(),  old_gw_flow.end(),  0.0);
        std::fill(runon_inflow.begin(), runon_inflow.end(), 0.0);
        std::fill(old_runon_inflow.begin(), old_runon_inflow.end(), 0.0);
        std::fill(gw_sw_head.begin(),   gw_sw_head.end(),   0.0);
        std::fill(gw_node_avail_flow.begin(), gw_node_avail_flow.end(), 0.0);
        std::fill(washoff_load.begin(), washoff_load.end(), 0.0);
        std::fill(conc.begin(), conc.end(), 0.0);
        std::fill(conc_old.begin(), conc_old.end(), 0.0);
    }
};

} /* namespace openswmm */

#endif /* OPENSWMM_ENGINE_SUBCATCH_DATA_HPP */
