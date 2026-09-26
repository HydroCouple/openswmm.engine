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
 * @file InitialQualityData.hpp
 * @brief SoA store for [INITIAL_QUALITY] per-element initial concentrations.
 *
 * @details Persistent data for .inp round-trip, mirroring ExtInflowData.
 *          Rows prescribe a per-node/per-link initial value for a pollutant
 *          or a reserved species (__WATER_AGE__ hours, __TEMPERATURE__ degC),
 *          overriding the global [POLLUTANTS] Cinit / sidecar INITIAL_STATE
 *          seeds. Raw names and raw values are retained for writer fidelity;
 *          unit conversion happens only at the consumption sites.
 *
 * @ingroup engine_data
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_INITIAL_QUALITY_DATA_HPP
#define OPENSWMM_ENGINE_INITIAL_QUALITY_DATA_HPP

#include <vector>
#include <string>

namespace openswmm {

// ============================================================================
// Per-element initial quality (from [INITIAL_QUALITY] section)
// ============================================================================

struct InitialQualityData {
    /// Classified constituent kinds (see `kind`).
    static constexpr int kKindWaterAge    = -1;  ///< __WATER_AGE__ (hours)
    static constexpr int kKindTemperature = -2;  ///< __TEMPERATURE__ (degC)
    static constexpr int kKindUnresolved  = -3;  ///< Not yet classified
    /// U2 (2026-09-07): MSX species m of the reactions component is encoded
    /// as kind = kKindMsxFirst - m (every value <= kKindMsxFirst). The row
    /// stays here for the writer ([INITIAL_QUALITY] is the canonical home)
    /// and is mirrored into ReactionData::init_elem_* for the engines by
    /// transport::mirrorInitialQualityMsxRows.
    static constexpr int kKindMsxFirst    = -100;
    static constexpr int msxKind(int species) noexcept { return kKindMsxFirst - species; }
    /// Species index for an MSX kind, -1 for any other kind.
    static constexpr int msxSpecies(int k) noexcept {
        return (k <= kKindMsxFirst) ? (kKindMsxFirst - k) : -1;
    }

    int count() const { return static_cast<int>(is_link.size()); }

    std::vector<uint8_t>     is_link;      ///< 0 = NODE row, 1 = LINK row
    std::vector<std::string> elem_name;    ///< Raw element name (post-parse re-resolution)
    std::vector<int>         elem_idx;     ///< Resolved node/link index (-1 until resolved)
    std::vector<std::string> constituent;  ///< Raw constituent name (writer round-trip)
    std::vector<int>         kind;         ///< >=0 pollutant idx; kKind* otherwise
    std::vector<double>      value;        ///< Raw user-units value
    /// U2: 1 when the row came from the `[INITIAL_QUALITY] FILE <csv>`
    /// sidecar. The writer emits the FILE line and the inline rows only;
    /// the sidecar is the source of its rows (edit or re-import it).
    std::vector<uint8_t>     from_file;

    /// U2: `[INITIAL_QUALITY] FILE <path>` as written (relative to the .inp);
    /// empty when none. Read at open, after the .inp parse.
    std::string file;

    void add(bool link, const std::string& name, const std::string& cons,
             double v, int ei = -1, int k = kKindUnresolved, bool fromFile = false) {
        is_link.push_back(link ? 1 : 0);
        elem_name.push_back(name);
        elem_idx.push_back(ei);
        constituent.push_back(cons);
        kind.push_back(k);
        value.push_back(v);
        from_file.push_back(fromFile ? 1 : 0);
    }

    /// Remove the entry at @p idx. No-op if out of range. Subsequent entries
    /// shift down by one — callers that hold cached indices must re-resolve.
    void erase(int idx) {
        if (idx < 0 || idx >= count()) return;
        const auto u = static_cast<std::size_t>(idx);
        is_link.erase(is_link.begin() + u);
        elem_name.erase(elem_name.begin() + u);
        elem_idx.erase(elem_idx.begin() + u);
        constituent.erase(constituent.begin() + u);
        kind.erase(kind.begin() + u);
        value.erase(value.begin() + u);
        if (u < from_file.size()) from_file.erase(from_file.begin() + u);
    }
};

} // namespace openswmm

#endif // OPENSWMM_ENGINE_INITIAL_QUALITY_DATA_HPP
