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
 * @file HeatFluxes.cpp
 * @brief Plan D-H5e — the single node/link surface-flux binding.
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "HeatFluxes.hpp"

#include <cstddef>

#include "RadiativeExchange.hpp"
#include "SolarRadiation.hpp"
#include "SurfaceExchange.hpp"
#include "../../../core/SimulationContext.hpp"
#include "../../../core/UnitConversion.hpp"
#include "../../../hydraulics/Node.hpp"
#include "../../../hydraulics/XSectBatch.hpp"

namespace openswmm::transport::heat {

namespace {

/// Cross-section parameters for a link. Mirrors `Routing.cpp:54`'s local
/// helper — the same one the non-DYNWAVE evaporation path uses, so an open
/// conduit's exchange area is built exactly the way its evaporation area is.
///
/// D-H5e: there was one of these in SurfaceExchange.cpp and a hand-inlined
/// near-copy in RadiativeExchange.cpp. Now there is one.
XSectParams buildXsp(const LinkData& links, std::size_t uk) {
    XSectParams xs{};
    const auto ls = links.xsect_shape[uk];
    xs.type   = (ls == XsectShape::DUMMY) ? 0 : static_cast<int>(ls) + 1;
    xs.y_full = links.xsect_y_full[uk];
    xs.a_full = links.xsect_a_full[uk];
    xs.w_max  = links.xsect_w_max[uk];
    xs.r_full = links.xsect_r_full[uk];
    xs.s_full = links.xsect_s_full[uk];
    xs.s_max  = links.xsect_s_max[uk];
    xs.y_bot  = links.xsect_y_bot[uk];
    xs.a_bot  = links.xsect_a_bot[uk];
    xs.s_bot  = links.xsect_s_bot[uk];
    xs.r_bot  = links.xsect_r_bot[uk];
    return xs;
}

}  // namespace

double netFluxOut(const SimulationContext& ctx, double t_w) noexcept {
    // Every family, one sign convention (positive OUT of the water), one
    // sum. H6b's SEDIMENT_EXCHANGE is a fourth term on this line.
    //
    // H6a deliberately adds NO term here: it changes where `Jin` comes from
    // inside radiativeFluxOut, not how many families are summed.
    return surfaceFluxOut(ctx, t_w) + radiativeFluxOut(ctx, t_w);
}

void applyHeatFluxes(SimulationContext& ctx, double dt) {
    if (!ctx.options.heat_transport) return;
    if (!ctx.heat_config.surface_exchange &&
        !ctx.heat_config.radiative_exchange)
        return;
    if (!(dt > 0.0)) return;

    // H6a: resolve this step's Jin and cloud fraction BEFORE any flux call.
    // `netFluxOut` is const and reads the cache; nothing downstream can
    // refresh it.
    updateSolarForcing(ctx);

    auto& hs = ctx.heat_state;
    const double rho = ctx.options.water_density;
    const double cp  = ctx.options.water_specific_heat;
    const int unit_sys =
        ucf::getUnitSystem(static_cast<int>(ctx.options.flow_units));

    // One relaxation per element against the SUMMED flux. Stepping each
    // module separately is what made the answer depend on module order —
    // see the header.
    const auto step = [&](double t_w, double area_ft2, double vol_ft3) {
        return relaxT(netFluxOut(ctx, t_w),
                      netFluxOut(ctx, t_w + kProbeC), kProbeC,
                      area_ft2 * kSqFtToSqM, vol_ft3 * kCuFtToCuM,
                      dt, rho, cp);
    };

    // ---- Nodes. Only STORAGE nodes have a free surface: node::getSurfArea
    //      returns 0 for JUNCTION/OUTFALL/DIVIDER, which is legacy's own
    //      convention and the same one its evaporation obeys. A manhole is
    //      closed; it does not exchange with the atmosphere. ---------------
    const int nn = ctx.n_nodes();
    for (int i = 0; i < nn; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        if (ui >= hs.node_temp.size()) break;
        const double vol_ft3 = ctx.nodes.volume[ui];
        if (!(vol_ft3 > 0.0)) continue;

        const double area_ft2 = node::getSurfArea(
            ctx.nodes, i, ctx.nodes.depth[ui], &ctx.tables, unit_sys,
            &ctx.node_subtypes);
        if (!(area_ft2 > 0.0)) continue;

        hs.node_temp[ui] += step(hs.node_temp[ui], area_ft2, vol_ft3);
    }

    // ---- Links. Open conduits only, area = top width x length x barrels,
    //      exactly the expression Routing.cpp:597 uses for evaporation. ----
    const int nl = ctx.n_links();
    const auto& CD = ctx.link_subtypes.conduits;
    for (int j = 0; j < nl; ++j) {
        const auto uj = static_cast<std::size_t>(j);
        if (uj >= hs.link_temp.size()) break;
        const double vol_ft3 = ctx.links.volume[uj];
        if (!(vol_ft3 > 0.0)) continue;

        const int cr = ctx.link_subtypes.conduit_row(j);
        if (cr < 0) continue;                       // regulators have no surface
        const auto ucr = static_cast<std::size_t>(cr);
        if (!xsect::isOpen(ctx.links.xsect_batch_shape[uj])) continue;

        double length = CD.length[ucr];
        if (!(length > 0.0)) length = CD.mod_length[ucr];
        if (!(length > 0.0)) continue;

        const double depth = ctx.links.depth[uj];
        if (!(depth > 0.0)) continue;
        const auto xs = buildXsp(ctx.links, uj);
        const double top_width = xsect::getWofY(xs, depth);
        if (!(top_width > 0.0)) continue;

        const double area_ft2 = top_width * length * CD.barrels[ucr];
        hs.link_temp[uj] += step(hs.link_temp[uj], area_ft2, vol_ft3);
    }
}

}  // namespace openswmm::transport::heat
