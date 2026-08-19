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
#include "../../../hydraulics/Node.hpp"
#include "../../../hydraulics/XSectBatch.hpp"

namespace openswmm::transport::heat {

namespace {

/// The module's own net outward flux at a given water temperature, W/m².
/// Written once because `relaxT` needs it evaluated TWICE per element — at
/// `T` and at `T + kProbeC` — and two hand-inlined copies of a three-call
/// expression is how a probe silently comes to measure a different function
/// than the one being stepped.
double netFluxOut(double t_w, double t_air, double rh, double wind_ms,
                  double a, double b, double rho, double p_ratio) noexcept {
    const double je = latentFlux(t_w, t_air, rh, wind_ms, a, b, rho);
    return je + sensibleFlux(je, bowenRatio(t_w, t_air, rh, p_ratio));
}

/// Cross-section parameters for a link. Mirrors `Routing.cpp:54`'s local
/// helper — the same one the non-DYNWAVE evaporation path uses, so an open
/// conduit's exchange area is built exactly the way its evaporation area is.
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

double airTempCelsius(const SimulationContext& ctx) noexcept {
    return (ctx.climate_state.temperature - 32.0) * 5.0 / 9.0;
}

double relaxT(double j0, double j1, double h, double area_m2, double vol_m3,
              double dt, double rho, double cp) noexcept {
    const double heat_capacity = rho * cp * vol_m3;
    if (!(heat_capacity > 0.0) || !(area_m2 > 0.0) || !(dt > 0.0)) return 0.0;

    // The explicit step, kept as the fallback and as the small-dt limit this
    // must agree with. It is deliberately NOT exported: it is the form that
    // diverged, and the only safe place to reach it is from inside here.
    const double explicit_dT = -j0 * area_m2 * dt / heat_capacity;
    if (!(h > 0.0)) return explicit_dT;

    const double djdt = (j1 - j0) / h;                 // W/m²/°C
    // J′ ≤ 0 means the outward flux does not grow with temperature, so the
    // linearized system has no fixed point to relax onto. There is nothing
    // to be stable about, so take the explicit step and let the caller's
    // finiteness check catch it.
    if (!(djdt > 0.0)) return explicit_dT;

    const double k = area_m2 * djdt / heat_capacity;   // 1/s
    if (!(k > 0.0)) return explicit_dT;

    // ΔT = (J₀/J′)·expm1(−k·dt). expm1 rather than exp(x)−1 so the small-dt
    // limit is accurate to full precision instead of cancelling to noise —
    // that limit is what makes this agree with H2/H3's existing answers.
    const double dT = (j0 / djdt) * std::expm1(-k * dt);
    return std::isfinite(dT) ? dT : explicit_dT;
}

double equilibriumT(double t0_c, double j0, double j1, double h) noexcept {
    if (!(h > 0.0)) return t0_c;
    const double djdt = (j1 - j0) / h;
    if (!(djdt > 0.0)) return t0_c;
    return t0_c - j0 / djdt;
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
        hs.node_temp[ui] += relaxT(netFluxOut(t_w, t_air, rh, wind_ms, a, b,
                                              rho, p_ratio),
                                   netFluxOut(t_w + kProbeC, t_air, rh,
                                              wind_ms, a, b, rho, p_ratio),
                                   kProbeC, area_ft2 * kSqFtToSqM,
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
        const auto xs = buildXsp(ctx.links, uj);
        const double top_width = xsect::getWofY(xs, depth);
        if (!(top_width > 0.0)) continue;
        const double area_ft2 = top_width * length * CD.barrels[ucr];

        const double t_w = hs.link_temp[uj];
        hs.link_temp[uj] += relaxT(netFluxOut(t_w, t_air, rh, wind_ms, a, b,
                                              rho, p_ratio),
                                   netFluxOut(t_w + kProbeC, t_air, rh,
                                              wind_ms, a, b, rho, p_ratio),
                                   kProbeC, area_ft2 * kSqFtToSqM,
                                   vol_ft3 * kCuFtToCuM, dt, rho, cp);
    }
}

}  // namespace openswmm::transport::heat
