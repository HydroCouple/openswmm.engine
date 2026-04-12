/**
 * @file HydrologyData.hpp
 * @brief SoA stores for snowpacks, aquifers, LID controls, and LID usage.
 *
 * @details Persistent data for .inp round-trip of hydrology-related sections.
 *
 * @ingroup engine_data
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_ENGINE_HYDROLOGY_DATA_HPP
#define OPENSWMM_ENGINE_HYDROLOGY_DATA_HPP

#include <vector>
#include <string>
#include <array>

namespace openswmm {

// ============================================================================
// Snowpack definitions (from [SNOWPACKS] section)
// ============================================================================

/**
 * @brief SoA storage for snowpack parameter sets.
 *
 * @details Each snowpack has PLOWABLE, IMPERVIOUS, PERVIOUS surface params
 *          (7 values each) and optional REMOVAL params (7 values).
 *          Subcatchments reference snowpacks by index via SubcatchData::snowpack.
 *
 * @ingroup engine_data
 */
struct SnowpackStore {
    int count() const { return static_cast<int>(names.size()); }

    std::vector<std::string> names;

    // PLOWABLE surface: cmin, cmax, tbase, fwfrac, sd0, fw0, snn0
    std::vector<std::array<double, 7>> plowable;

    // IMPERVIOUS surface: cmin, cmax, tbase, fwfrac, sd0, fw0, snn0
    std::vector<std::array<double, 7>> impervious;

    // PERVIOUS surface: cmin, cmax, tbase, fwfrac, sd0, fw0, snn0
    std::vector<std::array<double, 7>> pervious;

    // REMOVAL: dSnow, fOut, fImp, fPerv, fImelt, fToSubcatch, subcatch_name
    std::vector<std::array<double, 6>> removal;
    std::vector<std::string>           removal_subcatch;
};

// ============================================================================
// Aquifer definitions (from [AQUIFERS] section)
// ============================================================================

/**
 * @brief SoA storage for aquifer parameter sets.
 *
 * @details Subcatchments reference aquifers via SubcatchData::gw_aquifer index.
 *
 * @ingroup engine_data
 */
struct AquiferStore {
    int count() const { return static_cast<int>(names.size()); }

    std::vector<std::string> names;
    std::vector<double>      porosity;
    std::vector<double>      wilting_point;
    std::vector<double>      field_capacity;
    std::vector<double>      conductivity;
    std::vector<double>      conduct_slope;
    std::vector<double>      tension_slope;
    std::vector<double>      upper_evap;
    std::vector<double>      lower_evap;
    std::vector<double>      lower_loss;
    std::vector<double>      bottom_elev;
    std::vector<double>      water_table_elev;
    std::vector<double>      upper_moist;
    std::vector<std::string> upper_evap_pat;   ///< ET pattern name (optional)
};

// ============================================================================
// LID control definitions (from [LID_CONTROLS] section)
// ============================================================================

/**
 * @brief SoA storage for LID control type definitions.
 *
 * @details Each LID control has a type code and up to 7 parameter layers
 *          (SURFACE, SOIL, PAVEMENT, STORAGE, DRAIN, DRAINMAT, REMOVALS).
 *          Parameters are stored as raw arrays since the LID module expects
 *          specific data layout.
 *
 * @ingroup engine_data
 */
struct LidControlStore {
    int count() const { return static_cast<int>(names.size()); }

    std::vector<std::string> names;

    /** @brief LID type code string: BC, RG, GR, IT, PP, RB, RD, VS. */
    std::vector<std::string> lid_type;

    /** @brief SURFACE layer params (up to 5 values). */
    std::vector<std::array<double, 5>> surface;

    /** @brief SOIL layer params (up to 7 values). */
    std::vector<std::array<double, 7>> soil;

    /** @brief PAVEMENT layer params (up to 6 values). */
    std::vector<std::array<double, 6>> pavement;

    /** @brief STORAGE layer params (up to 4 values). */
    std::vector<std::array<double, 4>> storage;

    /** @brief DRAIN layer params (up to 6 values). */
    std::vector<std::array<double, 6>> drain;

    /** @brief DRAINMAT layer params (up to 3 values). */
    std::vector<std::array<double, 3>> drainmat;

    /** @brief REMOVALS: per-LID pollutant removal fractions.
     *  removals[lid_index] = vector of {pollutant_index, fraction} pairs.
     */
    std::vector<std::vector<std::pair<int, double>>> removals;
};

// ============================================================================
// LID usage assignments (from [LID_USAGE] section)
// ============================================================================

/**
 * @brief SoA storage for LID usage assignments to subcatchments.
 *
 * @ingroup engine_data
 */
struct LidUsageStore {
    int count() const { return static_cast<int>(subcatch_index.size()); }

    std::vector<int>         subcatch_index;   ///< Subcatchment index
    std::vector<int>         lid_index;        ///< Index into LidControlStore
    std::vector<int>         number;           ///< Number of replicate units
    std::vector<double>      area;             ///< Area of each unit (ft2 or m2)
    std::vector<double>      width;            ///< Top width of overland flow (ft or m)
    std::vector<double>      init_sat;         ///< Initial saturation (0–1)
    std::vector<double>      from_imperv;      ///< % of impervious area routed to LID
    std::vector<int>         to_perv;          ///< 1 = route outflow to pervious area
    std::vector<std::string> rpt_file;         ///< Report file name (optional)
    std::vector<std::string> drain_to;         ///< Drain-to subcatchment name (optional)
    std::vector<double>      from_perv;        ///< % of pervious area routed to LID
};

} // namespace openswmm

#endif // OPENSWMM_ENGINE_HYDROLOGY_DATA_HPP
