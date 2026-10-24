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
 * @file      InfraData.hpp
 * @brief     SoA stores for transects, streets, inlets, and control rules.
 *
 * @details Persistent data for .inp round-trip.
 *
 * @ingroup engine_data
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
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
    /**
     * @brief Count stored rows from the names column.
     * @return names.size() converted to int; parallel-column alignment and referenced-object validity
     *     are not checked.
     */
    int count() const { return static_cast<int>(names.size()); }

    std::vector<std::string> names;
    std::vector<std::string> comments; ///< Free-form description per transect (DA-ENG-09)
    std::vector<double> n_left;
    std::vector<double> n_right;
    std::vector<double> n_channel;
    std::vector<double> x_left_bank;
    std::vector<double> x_right_bank;
    /// Encroachment stations — independent of bank stations (BQ-TR-02).
    /// Default to 0.0 at add-time; the GUI surfaces them as a distinct
    /// field set. An INP parser extension may default these to the bank
    /// stations on legacy files lacking the trailing columns.
    std::vector<double> x_left_encroachment;
    std::vector<double> x_right_encroachment;
    std::vector<double> x_factor;
    std::vector<double> y_factor;
    std::vector<double>
        length_factor; ///< Meander factor (channel/floodplain length ratio); default 1.0
    /// Station-elevation pairs per transect
    std::vector<std::vector<double>> stations;
    std::vector<std::vector<double>> elevations;
};

// ============================================================================
// Street definitions (from [STREETS] section)
// ============================================================================

struct StreetStore {
    /**
     * @brief Count stored rows from the names column.
     * @return names.size() converted to int; parallel-column alignment and referenced-object validity
     *     are not checked.
     */
    int count() const { return static_cast<int>(names.size()); }

    std::vector<std::string> names;
    std::vector<double> t_crown;
    std::vector<double> h_curb;
    std::vector<double> sx; ///< Cross slope (%)
    std::vector<double> n_road;
    std::vector<double> gutter_depres;
    std::vector<double> gutter_width;
    std::vector<int> sides;
    std::vector<double> back_width;
    std::vector<double> back_slope;
    std::vector<double> back_n;
};

// ============================================================================
// Inlet definitions (from [INLETS] section)
// ============================================================================

/// Inlet design store. All lengths/velocities are kept in USER (display)
/// units exactly as read from the .inp (as StreetStore does); the runtime
/// InletSolver converts to internal units at init.
///
/// Column usage by type (2026-09-05 schema, see
/// plans/INLET_JUNCTION_IMPLEMENTATION_PLAN_2026-09-05.md §2.5):
///   GRATE / DROP_GRATE : length, width, grate_type, open_area, splash_veloc
///   SLOTTED            : length, width
///   CURB / DROP_CURB   : curb_length, curb_height, curb_throat
///   COMBO              : grate columns + curb columns (legacy two-line form:
///                        a GRATE line and a CURB line sharing one name are
///                        merged into a single COMBO row by handle_inlets)
///   CUSTOM             : curve_id, curve_index, curve_kind
struct InletStore {
    /**
     * @brief Count stored rows from the names column.
     * @return names.size() converted to int; parallel-column alignment and referenced-object validity
     *     are not checked.
     */
    int count() const { return static_cast<int>(names.size()); }

    std::vector<std::string> names;
    std::vector<std::string> inlet_type; ///< GRATE/CURB/COMBO/SLOTTED/DROP_GRATE/DROP_CURB/CUSTOM
    std::vector<double> length;          ///< grate or slotted length
    std::vector<double> width;           ///< grate or slotted width
    std::vector<std::string>
        grate_type; ///< P_BAR-50, P_BAR-50x100, P_BAR-30, CURVED_VANE, TILT_BAR-45, TILT_BAR-30, RETICULINE, GENERIC
    std::vector<double> open_area;    ///< GENERIC only: open-area fraction (0,1]
    std::vector<double> splash_veloc; ///< GENERIC only: splash-over velocity
    std::vector<double> curb_length;  ///< CURB/DROP_CURB/COMBO
    std::vector<double> curb_height;  ///< CURB/DROP_CURB/COMBO
    std::vector<int>
        curb_throat; ///< 0=HORIZONTAL 1=INCLINED 2=VERTICAL (legacy ThroatAngleWords order); default 2
    std::vector<std::string> curve_id; ///< CUSTOM: capture curve name
    std::vector<int> curve_index; ///< CUSTOM: resolved curve index (-1 until PostParseResolver)
    std::vector<int>
        curve_kind; ///< CUSTOM: 0=unresolved, 1=DIVERSION (captured vs approach flow), 2=RATING (captured vs depth)
    std::vector<std::string> comments; ///< optional description (round-tripped as ';' comment)

    /**
     * @brief Append a copied design name/type and default parameter columns.
     *
     * Numeric dimensions/open area start at zero, curb throat VERTICAL, curve unresolved (-1/kind0),
     * text empty. No name-index update or solver conversion; allocation can interrupt parallel appends.
     *
     * @param[in] name Borrowed design identifier copied verbatim; uniqueness is caller-managed.
     * @param[in] type Borrowed inlet-type token copied verbatim; not validated here.
     * @return New row index.
     */
    int add_row(const std::string& name, const std::string& type) {
        names.push_back(name);
        inlet_type.push_back(type);
        length.push_back(0.0);
        width.push_back(0.0);
        grate_type.push_back("");
        open_area.push_back(0.0);
        splash_veloc.push_back(0.0);
        curb_length.push_back(0.0);
        curb_height.push_back(0.0);
        curb_throat.push_back(2);
        curve_id.push_back("");
        curve_index.push_back(-1);
        curve_kind.push_back(0);
        comments.push_back("");
        return count() - 1;
    }

    /**
     * @brief Erase one inlet design from every owned column.
     *
     * Following design rows shift down and references are invalidated. Caller repairs usage design
     * indices and external indexes; this helper does not.
     *
     * @param[in] idx Existing row index in all aligned columns; unchecked, including negative values.
     */
    void erase_row(int idx) {
        auto u = static_cast<std::size_t>(idx);
        names.erase(names.begin() + static_cast<std::ptrdiff_t>(u));
        inlet_type.erase(inlet_type.begin() + static_cast<std::ptrdiff_t>(u));
        length.erase(length.begin() + static_cast<std::ptrdiff_t>(u));
        width.erase(width.begin() + static_cast<std::ptrdiff_t>(u));
        grate_type.erase(grate_type.begin() + static_cast<std::ptrdiff_t>(u));
        open_area.erase(open_area.begin() + static_cast<std::ptrdiff_t>(u));
        splash_veloc.erase(splash_veloc.begin() + static_cast<std::ptrdiff_t>(u));
        curb_length.erase(curb_length.begin() + static_cast<std::ptrdiff_t>(u));
        curb_height.erase(curb_height.begin() + static_cast<std::ptrdiff_t>(u));
        curb_throat.erase(curb_throat.begin() + static_cast<std::ptrdiff_t>(u));
        curve_id.erase(curve_id.begin() + static_cast<std::ptrdiff_t>(u));
        curve_index.erase(curve_index.begin() + static_cast<std::ptrdiff_t>(u));
        curve_kind.erase(curve_kind.begin() + static_cast<std::ptrdiff_t>(u));
        comments.erase(comments.begin() + static_cast<std::ptrdiff_t>(u));
    }
};

// ============================================================================
// Inlet usage (from [INLET_USAGE] section)
// ============================================================================

/// One row per inlet placement. Two host kinds share the store:
///   * conduit-attribute usage ([INLET_USAGE]): link_index >= 0, node_host == -1
///   * inlet junction ([INLET_JUNCTIONS]):      link_index == -1, node_host >= 0
/// flow_limit / local_depress / local_width are kept in USER units (as read);
/// InletSolver converts at init.
struct InletUsageStore {
    /**
     * @brief Count stored rows from the design_index column.
     * @return design_index.size() converted to int; parallel-column alignment and referenced-object
     *     validity are not checked.
     */
    int count() const { return static_cast<int>(design_index.size()); }

    std::vector<int> link_index;     ///< Host conduit link index (-1 for an inlet junction)
    std::vector<int> node_host;      ///< Host inlet-junction node index (-1 for a conduit usage)
    std::vector<int> design_index;   ///< Index into InletStore
    std::vector<int> node_index;     ///< Capture (receiving / underdrain) node index
    std::vector<int> num_inlets;     ///< Number of inlets per side
    std::vector<int> placement;      ///< 0=auto, 1=on_grade, 2=on_sag
    std::vector<double> clog_factor; ///< 1.0 - pctClogged/100
    std::vector<double> flow_limit;  ///< Max capture flow per inlet (user flow units), 0=unlimited
    std::vector<double> local_depress; ///< Local gutter depression (user length units)
    std::vector<double> local_width;   ///< Local depression width (user length units)
    std::vector<int>
        street_index; ///< Index into StreetStore, resolved by PostParseResolver (-1 if host xsect is not STREET)
    /// Design / capture-node names of an [INLET_JUNCTIONS] row, kept until
    /// PostParseResolver turns them into design_index / node_index. Empty on a
    /// row whose references were already resolved at parse time.
    std::vector<std::string> pending_design_name;
    std::vector<std::string> pending_capture_name;

    /**
     * @brief Append an unresolved/default inlet usage with explicit host references.
     *
     * Defaults one inlet, AUTO placement, unclogged, zero limits/depression and street=-1; pending
     * names empty. Does not grow statistic vectors, resolve a street, or enforce exactly one host.
     * Allocation can leave partial appends.
     *
     * @param[in] link_idx Host conduit index, or -1 for node hosting; copied without validation.
     * @param[in] node_host_idx Host inlet-junction index, or -1 for conduit hosting.
     * @param[in] design_idx InletStore design index; caller validates or resolves later.
     * @param[in] capture_node_idx Receiving node index; copied without topology validation.
     * @return New usage row index.
     */
    int add_row(int link_idx, int node_host_idx, int design_idx, int capture_node_idx) {
        link_index.push_back(link_idx);
        node_host.push_back(node_host_idx);
        design_index.push_back(design_idx);
        node_index.push_back(capture_node_idx);
        num_inlets.push_back(1);
        placement.push_back(0);
        clog_factor.push_back(1.0);
        flow_limit.push_back(0.0);
        local_depress.push_back(0.0);
        local_width.push_back(0.0);
        street_index.push_back(-1);
        pending_design_name.emplace_back();
        pending_capture_name.emplace_back();
        return count() - 1;
    }

    /**
     * @brief Erase a usage row from each column that contains it.
     *
     * Also visits optional statistic vectors. Does not guarantee alignment if columns were already
     * inconsistent, or renumber model references stored within surviving rows.
     *
     * @param[in] idx Row index; negative or out-of-range values skip each column independently.
     */
    void erase_row(int idx) {
        auto e = [idx](auto& v) {
            if (static_cast<std::size_t>(idx) < v.size())
                v.erase(v.begin() + static_cast<std::ptrdiff_t>(idx));
        };
        e(link_index);
        e(node_host);
        e(design_index);
        e(node_index);
        e(num_inlets);
        e(placement);
        e(clog_factor);
        e(flow_limit);
        e(local_depress);
        e(local_width);
        e(street_index);
        e(pending_design_name);
        e(pending_capture_name);
        e(stat_capture_vol);
        e(stat_bypass_vol);
        e(stat_backflow_vol);
        e(stat_peak_flow);
    }

    /**
     * @brief Find the first usage with a matching conduit-host key.
     *
     * Linear scan using design_index count; link_index must contain at least that many entries.
     *
     * @param[in] link_idx Stored host key; no link bounds/type validation, and -1 may match a
     *     node-hosted row.
     * @return First matching usage row, or -1.
     */
    int find_by_link(int link_idx) const {
        for (int i = 0; i < count(); ++i)
            if (link_index[static_cast<std::size_t>(i)] == link_idx)
                return i;
        return -1;
    }
    /**
     * @brief Find the first usage with a matching inlet-junction host key.
     *
     * Linear scan using design_index count; node_host must contain at least that many entries.
     *
     * @param[in] node_idx Stored host key; no node bounds/type validation, and -1 may match a
     *     conduit-hosted row.
     * @return First matching usage row, or -1.
     */
    int find_by_node_host(int node_idx) const {
        for (int i = 0; i < count(); ++i)
            if (node_host[static_cast<std::size_t>(i)] == node_idx)
                return i;
        return -1;
    }

    // Stats (populated by InletSolver::gatherStats() before reporting)
    std::vector<double> stat_capture_vol;  ///< Total captured volume (ft³)
    std::vector<double> stat_bypass_vol;   ///< Total bypassed volume (ft³)
    std::vector<double> stat_backflow_vol; ///< Total backflow volume (ft³)
    std::vector<double> stat_peak_flow;    ///< Peak captured flow (cfs)

    /**
     * @brief Replace inlet usage statistics with zeroed columns.
     *
     * Volumes use ft3 and peak flow cfs. Preserves definitions; independent assignments can fail
     * partway on allocation.
     *
     * @param[in] n Nonnegative row count, normally count(); no negative-value check.
     */
    void resize_stats(int n) {
        auto un = static_cast<std::size_t>(n);
        stat_capture_vol.assign(un, 0.0);
        stat_bypass_vol.assign(un, 0.0);
        stat_backflow_vol.assign(un, 0.0);
        stat_peak_flow.assign(un, 0.0);
    }
};

// ============================================================================
// Control rule text (from [CONTROLS] section)
// ============================================================================

struct ControlRuleStore {
    /**
     * @brief Count stored rows from the rule_text column.
     * @return rule_text.size() converted to int; parallel-column alignment and referenced-object
     *     validity are not checked.
     */
    int count() const { return static_cast<int>(rule_text.size()); }

    /// Each element is the full multi-line text block for one rule:
    /// "RULE name\nIF ...\nTHEN ...\nELSE ...\nPRIORITY ..."
    std::vector<std::string> rule_text;
};

} // namespace openswmm

#endif // OPENSWMM_ENGINE_INFRA_DATA_HPP
