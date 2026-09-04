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
 * @file Divider.hpp
 * @brief Flow divider node logic — cutoff, overflow, tabular, weir.
 *
 * @details Divider nodes split incoming flow between a primary and diversion
 *          link. The split depends on divider type:
 *            - CUTOFF:   diversion gets flow above cutoff value
 *            - OVERFLOW: diversion gets flow exceeding primary link capacity
 *            - TABULAR:  fraction from lookup table
 *            - WEIR:     weir equation determines diversion flow
 *
 *          Batch: group dividers by type, compute split per type group.
 *
 * @note Legacy reference: src/legacy/engine/node.c (divider_getOutflow)
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_DIVIDER_HPP
#define OPENSWMM_DIVIDER_HPP

#include "../data/NodeSubtypes.hpp"
#include <vector>

namespace openswmm {

struct SimulationContext;

namespace divider {

enum class DividerMethod : int {
    CUTOFF       = 0,
    OVERFLOW_DIV = 1,  ///< Renamed to avoid macOS math.h OVERFLOW macro
    TABULAR      = 2,
    WEIR         = 3
};

/**
 * @brief Flow sent from a divider node into one of its outflow links.
 *
 * @details Legacy-faithful divider_getOutflow (node.c:1237-1312): the
 *          diversion link receives the diverted portion of the node's
 *          inflow + overflow per the divider method; any other outflow
 *          link receives the undiverted remainder. Requires links be
 *          routed in topological order so the non-diversion link is
 *          evaluated before the diversion link (OVERFLOW dividers read
 *          the node's accumulated outflow).
 *
 * @param ctx       Simulation context.
 * @param node_idx  Divider node index.
 * @param link_idx  Outflow link being evaluated.
 * @returns         Flow rate into the link (internal flow units).
 */
double getOutflow(SimulationContext& ctx, int node_idx, int link_idx);

} // namespace divider
} // namespace openswmm

#endif // OPENSWMM_DIVIDER_HPP
