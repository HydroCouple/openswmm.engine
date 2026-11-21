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
 * @file      LidLayerCommon.cpp
 * @brief     LID layer topology and geometry, shared by every species track.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include "LidLayerCommon.hpp"

namespace openswmm::transport {

/**
 * @brief Resolve immediate inflow donors/physical vertical adjacency for each LID layer.
 * @param[in] t Conventional LID type; unknown value uses surface external and other layers
 * absent.
 * @param[in] has_soil For permeable pavement, storage donor is soil when true, pavement
 * otherwise.
 * @return Four selectors surface/pavement/soil/storage: layer ID, kExternal or kAbsent; no
 * allocation, context mutation or runtime geometry validation.
 */
Donors donorsFor(lid::LIDType t, bool has_soil) {
    switch (t) {
    case lid::LIDType::BIO_CELL:
    case lid::LIDType::RAIN_GARDEN:
        // Rain garden is the storage-less bio-cell; its stor_thick is 0,
        // so in_stor stays 0 and the storage row never mixes.
        return {kExternal, kAbsent, kSurf, kSoil};
    case lid::LIDType::GREEN_ROOF:
        // "Storage" is the drainage mat, fed by soil percolation.
        return {kExternal, kAbsent, kSurf, kSoil};
    case lid::LIDType::INFIL_TRENCH:
        return {kExternal, kAbsent, kAbsent, kSurf};
    case lid::LIDType::PERM_PAVEMENT:
        // The only four-layer stack, and the only conditional donor:
        // storage takes soil percolation when there is a soil layer and
        // pavement percolation when there is not, exactly as the solver's
        // storageInflow_local resolves it.
        return {kExternal, kSurf, kPave, has_soil ? kSoil : kPave};
    case lid::LIDType::RAIN_BARREL:
        // Storage is the ONLY layer, and it is therefore the topmost
        // present one — its inflow is external, not from a layer above.
        return {kAbsent, kAbsent, kAbsent, kExternal};
    case lid::LIDType::VEG_SWALE:
    case lid::LIDType::ROOF_DISCON:
        return {kExternal, kAbsent, kAbsent, kAbsent};
    }
    return {kExternal, kAbsent, kAbsent, kAbsent};
}

/**
 * @brief Void-weighted water held in each layer, as a depth per unit area (ft). The four terms
 * are exactly those `LIDSolver::storedVolume()` sums, so a layer's age is weighted by the same
 * water the mass balance counts.
 *
 * @details Compute four void-weighted retained layer water depths per unit footprint area.
 * @param[in] g Prepared parallel LID depth, porosity, thickness and moisture arrays.
 * @param[in] ui Valid unit index covered by all accessed arrays, unchecked.
 * @param[out] v Exactly kNL depths ft: surface, pavement, soil, storage. These are not total
 * ft3; caller multiplies by footprint area if absolute water volume is required.
 * @note Allocation-free, no finite/sign checks or geometry/clock mutation; output must not alias
 * inputs.
 */
void layerVolumes(const lid::LIDGroupSoA& g, std::size_t ui, double (&v)[kNL]) {
    v[kSurf] = g.surf_depth[ui] * g.surf_void_frac[ui];
    v[kPave] = g.pave_depth[ui] * g.pave_void[ui] * (1.0 - g.pave_imperv_frac[ui]);
    v[kSoil] = g.soil_moist[ui] * g.soil_thick[ui];
    v[kStor] = g.stor_depth[ui] * g.stor_void[ui];
}

/**
 * @brief Build flattened conventional-LID group/unit prefix offsets.
 * @param[in] solver Stable group counts/order; nonnegative counts and signed-int sum must fit.
 * @return Owning numGroups()+1 offsets starting0 and ending total units; no units still
 * returns[0]. No context mutation or count/overflow validation; vector allocation can throw.
 */
std::vector<int> buildOffsets(const lid::LIDSolver& solver) {
    std::vector<int> off;
    off.reserve(static_cast<std::size_t>(solver.numGroups()) + 1);
    int running = 0;
    for (int t = 0; t < solver.numGroups(); ++t) {
        off.push_back(running);
        running += solver.group(t).count;
    }
    off.push_back(running);
    return off;
}

} // namespace openswmm::transport
