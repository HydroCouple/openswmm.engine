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
 * @file SurfaceExchange.cpp
 * @brief Phase H2 body — latent/sensible surface exchange and its binding.
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "SurfaceExchange.hpp"

#include <algorithm>
#include <cmath>

#include "../../../core/SimulationContext.hpp"
#include "../../../core/UnitConversion.hpp"
#include "../../../hydraulics/Link.hpp"
#include "../../../hydraulics/Node.hpp"
#include "../../../hydraulics/XSectBatch.hpp"

namespace openswmm::transport::heat {

namespace {

/// Cross-section parameters for a link. Mirrors `Routing.cpp:54`'s local
/// helper — the same one the non-DYNWAVE evaporation path uses, so an open
/// conduit's exchange area is built exactly the way its evaporation area is.
XSectParams buildXsp(const SimulationContext& ctx, std::size_t uk) {
    const LinkData& links = ctx.links;
    XSectParams xs{};
    // link::translateShape is the canonical LinkData-enum -> batch-enum
    // translation (Link.cpp) — POLYGON=26 was appended to both enums at the
    // same numeric value, breaking the flat +1 offset every earlier shape
    // follows, so this delegates rather than re-deriving the mapping here.
    xs.type = link::translateShape(links.xsect_shape[uk]);
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
    {
        const int ci = links.xsect_cheb_idx[uk];
        if (ci >= 0 && static_cast<std::size_t>(ci) < ctx.cheb_sections.size())
            xs.cheb = &ctx.cheb_sections[static_cast<std::size_t>(ci)];
    }
    return xs;
}

}  // namespace

double airTempCelsius(const SimulationContext& ctx) noexcept {
    return (ctx.climate_state.temperature - 32.0) * 5.0 / 9.0;
}

double deltaT(double je, double jc, double area_m2, double vol_m3, double dt,
              double rho, double cp) noexcept {
    const double heat_capacity = rho * cp * vol_m3;
    if (!(heat_capacity > 0.0) || !(area_m2 > 0.0)) return 0.0;
    return -(je + jc) * area_m2 * dt / heat_capacity;
}

double saturationVapourPressure(double t_c) noexcept {
    return 0.61275 * std::exp(17.27 * t_c / (237.3 + t_c));
}

double latentHeatOfVaporization(double t_c) noexcept {
    return 1000.0 * (2499.0 - 2.36 * t_c);
}

double windFunction(double wind_ms, double a, double b) noexcept {
    return a + b * std::max(wind_ms, 0.0);
}

double evaporationRate(double t_water_c, double t_air_c, double humidity_pct,
                       double wind_ms, double a, double b) noexcept {
    const double es_w = saturationVapourPressure(t_water_c);
    const double e_a  = (humidity_pct / 100.0) *
                        saturationVapourPressure(t_air_c);
    return windFunction(wind_ms, a, b) * (es_w - e_a);
}

double latentFlux(double t_water_c, double t_air_c, double humidity_pct,
                  double wind_ms, double a, double b,
                  double water_density) noexcept {
    const double e = evaporationRate(t_water_c, t_air_c, humidity_pct,
                                     wind_ms, a, b);
    return water_density * latentHeatOfVaporization(t_water_c) * e;
}

double bowenRatio(double t_water_c, double t_air_c, double humidity_pct,
                  double pressure_ratio) noexcept {
    const double es_w = saturationVapourPressure(t_water_c);
    const double e_a  = (humidity_pct / 100.0) *
                        saturationVapourPressure(t_air_c);
    const double deficit = es_w - e_a;
    // Removable singularity: as the deficit vanishes so does Je, and Jc =
    // Br·Je tends to a finite limit. Returning 0 here keeps Jc finite
    // instead of producing inf/NaN that would then propagate into every
    // downstream temperature.
    if (std::fabs(deficit) < 1.0e-12) return 0.0;
    return kBowenCoeff * pressure_ratio * (t_water_c - t_air_c) / deficit;
}

double sensibleFlux(double latent_flux, double bowen_ratio) noexcept {
    return bowen_ratio * latent_flux;
}

void applySurfaceExchange(SimulationContext& ctx, double dt) {
    if (!ctx.options.heat_transport || !ctx.heat_config.surface_exchange)
        return;
    if (!(dt > 0.0)) return;

    auto& hs = ctx.heat_state;
    const double t_air    = airTempCelsius(ctx);
    const double rh       = ctx.climate_state.humidity;
    const double wind_ms  = ctx.climate_state.wind_speed * kMphToMs;
    const double a        = ctx.options.wind_func_coeff_a;
    const double b        = ctx.options.wind_func_coeff_b;
    const double rho      = ctx.options.water_density;
    const double cp       = ctx.options.water_specific_heat;
    const double p_ratio  = ctx.options.pressure_ratio;
    const int unit_sys =
        ucf::getUnitSystem(static_cast<int>(ctx.options.flow_units));

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

        const double t_w = hs.node_temp[ui];
        const double je  = latentFlux(t_w, t_air, rh, wind_ms, a, b, rho);
        const double jc  = sensibleFlux(je, bowenRatio(t_w, t_air, rh, p_ratio));
        hs.node_temp[ui] += deltaT(je, jc, area_ft2 * kSqFtToSqM,
                                   vol_ft3 * kCuFtToCuM, dt, rho, cp);
    }

    // ---- Links. Open conduits only, area = top width x length x barrels,
    //      exactly the expression Routing.cpp:597 uses for evaporation. ----
    const int nl = ctx.n_links();
    for (int j = 0; j < nl; ++j) {
        const auto uj = static_cast<std::size_t>(j);
        if (uj >= hs.link_temp.size()) break;
        const double vol_ft3 = ctx.links.volume[uj];
        if (!(vol_ft3 > 0.0)) continue;

        const int cr = ctx.link_subtypes.conduit_row(j);
        if (cr < 0) continue;                       // regulators have no surface
        const auto ucr = static_cast<std::size_t>(cr);
        if (!xsect::isOpen(ctx.links.xsect_batch_shape[uj])) continue;

        const auto& CD = ctx.link_subtypes.conduits;
        double length = CD.length[ucr];
        if (!(length > 0.0)) length = CD.mod_length[ucr];
        if (!(length > 0.0)) continue;

        const double depth = ctx.links.depth[uj];
        if (!(depth > 0.0)) continue;
        const auto xs = buildXsp(ctx, uj);
        const double top_width = xsect::getWofY(xs, depth);
        if (!(top_width > 0.0)) continue;
        const double area_ft2 = top_width * length * CD.barrels[ucr];

        const double t_w = hs.link_temp[uj];
        const double je  = latentFlux(t_w, t_air, rh, wind_ms, a, b, rho);
        const double jc  = sensibleFlux(je, bowenRatio(t_w, t_air, rh, p_ratio));
        hs.link_temp[uj] += deltaT(je, jc, area_ft2 * kSqFtToSqM,
                                   vol_ft3 * kCuFtToCuM, dt, rho, cp);
    }
}

}  // namespace openswmm::transport::heat
