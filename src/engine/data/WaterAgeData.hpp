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
 * @file WaterAgeData.hpp
 * @brief Water-age tracking data (water age plan §1–§2, phase A1a).
 *
 * @details Age is the reserved species `__WATER_AGE__` (registry kind
 *          RESERVED_AGE): zero-order growth d(age)/dt = 1 in every water
 *          parcel, volume-weighted mixing everywhere water mixes. Internal
 *          unit is SECONDS (the config file speaks HOURS — ×3600 at
 *          parse). Per-source initial ages come from the waterage
 *          component's `[WATER_AGE_SOURCES]` (`model.age`, D-UT8): each
 *          QualitySolver loader contributes `age_volume = q · age_source`
 *          to its node, and the transport engine mixes it in exactly like
 *          a pollutant load.
 *
 *          A1a scope: the age species rides the EULERIAN_ARD mesh; the
 *          LEGACY CSTR age mirror is phase A1b (warned until then);
 *          watershed/LID/GW age states are A3/A4.
 *
 * @ingroup engine_data
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_DATA_WATER_AGE_DATA_HPP
#define OPENSWMM_ENGINE_DATA_WATER_AGE_DATA_HPP

#include <vector>

namespace openswmm {

/// Source pathways with configurable initial age ([WATER_AGE_SOURCES]).
/// Order is the storage index of WaterAgeConfigData::global_age.
enum class WaterAgeSource : int {
    RAINFALL        = 0,  ///< washoff runoff (and LID drains until A4)
    DWF             = 1,
    GW              = 2,
    RDII            = 3,
    EXTERNAL_INFLOW = 4,
    IFACE           = 5,
    INITIAL_STATE   = 6,  ///< water in the network at t = 0
    COUNT_          = 7
};

/**
 * @brief Parsed `model.age` state (waterage component, phase A1a).
 *
 * @details Node-scope overrides are resolved to node indices at apply (the
 *          rows reference nothing that another component declares, so no
 *          post-apply resolution pass is needed — unlike E5a's transport
 *          rows). TIMESERIES ages and SUBCATCH/EDGE_BC scopes are later
 *          phases and refuse with precise deferral errors.
 */
struct WaterAgeConfigData {
    bool configured = false;

    /// GLOBAL initial age per source, SECONDS (default 0 = fresh water).
    double global_age[static_cast<int>(WaterAgeSource::COUNT_)] = {};

    // NODE-scope overrides (DWF / EXTERNAL_INFLOW), parallel arrays.
    std::vector<int>    node_over_source;  ///< WaterAgeSource as int
    std::vector<int>    node_over_node;    ///< node index
    std::vector<double> node_over_age;     ///< seconds

    /// Age of `source` water entering `node` (seconds).
    double source_age(WaterAgeSource s, int node) const noexcept {
        for (std::size_t i = 0; i < node_over_source.size(); ++i)
            if (node_over_source[i] == static_cast<int>(s) &&
                node_over_node[i] == node)
                return node_over_age[i];
        return global_age[static_cast<int>(s)];
    }
};

/**
 * @brief Runtime age state shared by the engines (phase A1a).
 *
 * @details `node_age_vol_in` is a RATE (age·ft³/s), the age analogue of
 *          nodes.qual_mass_in — loaders add `q · age_source` and the
 *          engine integrates it over its substeps. `node_age`/`link_age`
 *          are the PUBLISHED mean ages (seconds) the API and gates read.
 */
struct WaterAgeState {
    std::vector<double> node_age_vol_in;  ///< [node], age·ft³/s
    std::vector<double> node_age;         ///< [node], seconds
    std::vector<double> link_age;         ///< [link], seconds

    /// A1b: the LEGACY mirror seeds INITIAL_STATE on its first step
    /// (the ARD engine seeds at its own init instead).
    bool legacy_seeded = false;

    /// A2a: set by HotStartManager::apply when a V3 file restored ages —
    /// both engines then seed from node_age/link_age instead of
    /// INITIAL_STATE (the ARD engine consumes and clears it at init).
    bool hotstart_loaded = false;

    void resize(int n_nodes, int n_links) {
        node_age_vol_in.assign(static_cast<std::size_t>(n_nodes), 0.0);
        node_age.assign(static_cast<std::size_t>(n_nodes), 0.0);
        link_age.assign(static_cast<std::size_t>(n_links), 0.0);
        legacy_seeded   = false;
        hotstart_loaded = false;
    }
    void clear() { *this = WaterAgeState{}; }
};

}  // namespace openswmm

#endif  // OPENSWMM_ENGINE_DATA_WATER_AGE_DATA_HPP
