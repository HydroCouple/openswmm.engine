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
 * @file Divider.cpp
 * @brief Flow divider node logic — legacy-faithful divider_getOutflow.
 *
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "Divider.hpp"
#include "../core/SimulationContext.hpp"
#include "../core/UnitConversion.hpp"
#include <cmath>
#include <algorithm>

namespace openswmm {
namespace divider {

/// Legacy FLOW_TOL (consts.h): minimum signficant flow, cfs.
static constexpr double FLOW_TOL = 0.00001;

double getOutflow(SimulationContext& ctx, int node_idx, int link_idx) {
    // Legacy node.c divider_getOutflow(j, k). Requires links be routed so the
    // non-diversion link is evaluated before the diversion link (the topo sort
    // guarantees it, as in legacy).
    const int r = ctx.node_subtypes.divider_row(node_idx);
    if (r < 0) return 0.0;
    const auto ur = static_cast<std::size_t>(r);
    const auto& divs = ctx.node_subtypes.dividers;
    const auto un = static_cast<std::size_t>(node_idx);

    const double ucf_flow =
        ucf::Qcf[static_cast<int>(ctx.options.flow_units)];

    const double qIn = ctx.nodes.inflow[un] + ctx.nodes.overflow[un];
    const int div_link = divs.link[ur];
    double qOut = 0.0;

    switch (static_cast<DividerMethod>(static_cast<int>(divs.method[ur]))) {
        case DividerMethod::CUTOFF:
            // cutoff holds qMin, pre-converted to internal flow units
            if (qIn <= divs.cutoff[ur]) qOut = 0.0;
            else qOut = qIn - divs.cutoff[ur];
            break;

        case DividerMethod::OVERFLOW_DIV:
            // Non-diversion link simply carries the node's inflow; the
            // diversion link receives the excess of inflow over what was
            // already sent into the non-diversion link (node.c:1265-1273).
            if (link_idx != div_link) qOut = qIn;
            else qOut = qIn - ctx.nodes.outflow[un];
            if (qOut < FLOW_TOL) qOut = 0.0;
            return qOut;

        case DividerMethod::WEIR: {
            // cutoff = qMin (internal units); max_depth = dhMax and
            // cd = cWeir stay in USER units (legacy node.c:192-194).
            const double qMin  = divs.cutoff[ur];
            const double dhMax = divs.max_depth[ur];
            const double cWeir = divs.cd[ur];
            // qMax derived as in legacy divider_validate (node.c:1227)
            const double qMax = cWeir * std::pow(dhMax, 1.5) / ucf_flow;
            if (qIn <= qMin) qOut = 0.0;
            else {
                const double f = (qIn - qMin) / (qMax - qMin);
                if (f > 1.0) qOut = qMax * std::sqrt(f);
                else qOut = cWeir * std::pow(f * dhMax, 1.5) / ucf_flow;
            }
            break;
        }

        case DividerMethod::TABULAR: {
            const int m = divs.curve[ur];
            if (m >= 0 && m < static_cast<int>(ctx.tables.tables.size()))
                qOut = table_lookup_cursor(
                           ctx.tables.tables[static_cast<std::size_t>(m)],
                           qIn * ucf_flow) / ucf_flow;
            else qOut = 0.0;
            break;
        }
    }

    // Outflow cannot exceed inflow; a non-diversion link carries the
    // undiverted remainder (node.c:1305-1312).
    if (qOut > qIn) qOut = qIn;
    if (link_idx != div_link) qOut = qIn - qOut;
    return qOut;
}

} // namespace divider
} // namespace openswmm
