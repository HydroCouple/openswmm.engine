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
 * @file MsxInitialQuality.cpp
 * @brief U2 — [INITIAL_QUALITY] MSX rows mirrored into ReactionData (see header).
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "MsxInitialQuality.hpp"

#include "../core/SimulationContext.hpp"
#include "../data/InitialQualityData.hpp"

namespace openswmm::transport {

bool initialQualityHasMsxRow(const SimulationContext& ctx, bool is_link,
                             int elem_idx, int species) {
    const auto& iq = ctx.initial_quality;
    const int want = InitialQualityData::msxKind(species);
    for (int r = 0; r < iq.count(); ++r) {
        const auto ur = static_cast<std::size_t>(r);
        if ((iq.is_link[ur] != 0) == is_link && iq.elem_idx[ur] == elem_idx &&
            iq.kind[ur] == want)
            return true;
    }
    return false;
}

std::vector<std::string> mirrorInitialQualityMsxRows(SimulationContext& ctx) {
    std::vector<std::string> errs;
    auto& rx = ctx.reactions;
    const auto& iq = ctx.initial_quality;
    if (!rx.configured) return errs;

    for (int r = 0; r < iq.count(); ++r) {
        const auto ur = static_cast<std::size_t>(r);
        const int m = InitialQualityData::msxSpecies(iq.kind[ur]);
        if (m < 0 || m >= rx.n_species()) continue;
        const int  ei   = iq.elem_idx[ur];
        const bool link = iq.is_link[ur] != 0;
        if (ei < 0) continue;

        bool found = false;
        for (std::size_t k = 0; k < rx.init_elem_idx.size(); ++k) {
            if ((rx.init_elem_is_link[k] != 0) == link &&
                rx.init_elem_idx[k] == ei && rx.init_elem_species[k] == m) {
                if (rx.init_elem_value[k] != iq.value[ur]) {
                    // A different value from the .rxn's [REACTION_QUALITY]
                    // NODE|LINK row: the same ambiguity [INITIAL_QUALITY]
                    // refuses for its own duplicates.
                    errs.push_back(
                        "[INITIAL_QUALITY] '" + iq.constituent[ur] + "' at " +
                        (link ? "link '" : "node '") + iq.elem_name[ur] +
                        "' conflicts with a [REACTION_QUALITY] " +
                        (link ? "LINK" : "NODE") +
                        " row in the reactions config — keep one.");
                }
                found = true;
                break;
            }
        }
        if (!found) {
            rx.init_elem_is_link.push_back(link ? 1 : 0);
            rx.init_elem_idx.push_back(ei);
            rx.init_elem_species.push_back(m);
            rx.init_elem_value.push_back(iq.value[ur]);
        }
    }
    return errs;
}

}  // namespace openswmm::transport
