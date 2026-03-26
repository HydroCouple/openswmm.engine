/**
 * @file InfraData.hpp
 * @brief SoA stores for transects, streets, inlets, and control rules.
 *
 * @details Persistent data for .inp round-trip.
 *
 * @ingroup engine_data
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_ENGINE_INFRA_DATA_HPP
#define OPENSWMM_ENGINE_INFRA_DATA_HPP

#include <vector>
#include <string>

namespace openswmm {

// ============================================================================
// Transect definitions (from [TRANSECTS] section)
// ============================================================================

struct TransectStore {
    int count() const { return static_cast<int>(names.size()); }

    std::vector<std::string> names;
    std::vector<double>      n_left;
    std::vector<double>      n_right;
    std::vector<double>      n_channel;
    std::vector<double>      x_left_bank;
    std::vector<double>      x_right_bank;
    std::vector<double>      x_factor;
    std::vector<double>      y_factor;
    /// Station-elevation pairs per transect
    std::vector<std::vector<double>> stations;
    std::vector<std::vector<double>> elevations;
};

// ============================================================================
// Street definitions (from [STREETS] section)
// ============================================================================

struct StreetStore {
    int count() const { return static_cast<int>(names.size()); }

    std::vector<std::string> names;
    std::vector<double>      t_crown;
    std::vector<double>      h_curb;
    std::vector<double>      sx;           ///< Cross slope (%)
    std::vector<double>      n_road;
    std::vector<double>      gutter_depres;
    std::vector<double>      gutter_width;
    std::vector<int>         sides;
    std::vector<double>      back_width;
    std::vector<double>      back_slope;
    std::vector<double>      back_n;
};

// ============================================================================
// Inlet definitions (from [INLETS] section)
// ============================================================================

struct InletStore {
    int count() const { return static_cast<int>(names.size()); }

    std::vector<std::string> names;
    std::vector<std::string> inlet_type;   ///< GRATE/CURB/SLOTTED/DROP_GRATE/DROP_CURB/CUSTOM
    std::vector<double>      length;
    std::vector<double>      width;        ///< or height for curb
    std::vector<std::string> grate_type;   ///< P-50, P-50x100, CURVED_VANE, etc.
    std::vector<double>      open_area;
    std::vector<double>      splash_veloc;
};

// ============================================================================
// Inlet usage (from [INLET_USAGE] section)
// ============================================================================

struct InletUsageStore {
    int count() const { return static_cast<int>(link_index.size()); }

    std::vector<int>         link_index;     ///< Conduit link index
    std::vector<int>         design_index;   ///< Index into InletStore
    std::vector<int>         node_index;     ///< Receiving node index
    std::vector<int>         num_inlets;     ///< Number of inlets per side
    std::vector<int>         placement;      ///< 0=auto, 1=on_grade, 2=on_sag
    std::vector<double>      clog_factor;    ///< 1.0 - pctClogged/100
    std::vector<double>      flow_limit;     ///< Max capture flow (cfs), 0=unlimited
    std::vector<double>      local_depress;  ///< Local gutter depression (ft)
    std::vector<double>      local_width;    ///< Local depression width (ft)
    std::vector<int>         street_index;   ///< Index into StreetStore (-1 if none)
};

// ============================================================================
// Control rule text (from [CONTROLS] section)
// ============================================================================

struct ControlRuleStore {
    int count() const { return static_cast<int>(rule_text.size()); }

    /// Each element is the full multi-line text block for one rule:
    /// "RULE name\nIF ...\nTHEN ...\nELSE ...\nPRIORITY ..."
    std::vector<std::string> rule_text;
};

} // namespace openswmm

#endif // OPENSWMM_ENGINE_INFRA_DATA_HPP
