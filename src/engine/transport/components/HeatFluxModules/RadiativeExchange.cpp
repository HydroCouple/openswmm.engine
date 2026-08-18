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
 * @file RadiativeExchange.cpp
 * @brief Phase H3 body — shortwave/longwave radiation and its binding.
 *
 * @details Line-for-line provenance is `RHEComponent/src/element.cpp`:
 *          `computeNetSWSolarRadiation` (:106), `computeBackLWRadiation`
 *          (:113), `computeAtmosphericLWRadiation` (:119),
 *          `computeLandCoverLWRadiation` (:131).
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "RadiativeExchange.hpp"

#include <algorithm>
#include <cmath>

#include "SurfaceExchange.hpp"
#include "../../../core/SimulationContext.hpp"
#include "../../../core/UnitConversion.hpp"
#include "../../../hydraulics/Node.hpp"
#include "../../../hydraulics/XSectBatch.hpp"

namespace openswmm::transport::heat {

namespace {

constexpr double kSqFtToSqM = 0.09290304;
constexpr double kCuFtToCuM = 0.028316846592;

double airTempCelsius(const SimulationContext& ctx) noexcept {
    return (ctx.climate_state.temperature - 32.0) * 5.0 / 9.0;
}

/// T⁴ in Kelvin, written once so the offset cannot drift between terms.
double kelvin4(double t_c) noexcept {
    const double k = t_c + kKelvinOffset;
    return k * k * k * k;
}

}  // namespace

double netShortwave(double incoming_wm2, double albedo,
                    double shade_factor) noexcept {
    // max(0, 1 - fs) mirrors the reference: an over-unity shade factor
    // shades completely rather than turning into negative insolation.
    return (1.0 - albedo) * incoming_wm2 *
           std::max(0.0, 1.0 - shade_factor);
}

double backLongwave(double t_water_c, double emiss_water) noexcept {
    return emiss_water * kStefanBoltzmann * kelvin4(t_water_c);
}

double atmosphericEmissivity(double e_a_kpa,
                             double atm_emiss_coeff) noexcept {
    // Brunt (1932). The reference passes PASCALS into the square root
    // (`sqrt(vaporPressureAir * 1000)`); vapour pressure here is kPa, so
    // the ×1000 is the unit conversion, not a fudge. Using kPa understates
    // this term by sqrt(1000).
    return atm_emiss_coeff +
           0.0027 * std::sqrt(std::max(0.0, e_a_kpa) * 1000.0);
}

double atmosphericLongwave(double t_air_c, double humidity_pct,
                           double atm_emiss_coeff, double lw_reflection,
                           double sky_view) noexcept {
    const double e_a =
        (humidity_pct / 100.0) * saturationVapourPressure(t_air_c);
    return kStefanBoltzmann * kelvin4(t_air_c) *
           atmosphericEmissivity(e_a, atm_emiss_coeff) *
           (1.0 - lw_reflection) * sky_view;
}

double landCoverLongwave(double t_air_c, double emiss_landcover,
                         double sky_view) noexcept {
    // The reference carries a separate land-cover TEMPERATURE state; H3
    // uses air temperature, per plan §2.2. Recorded as a known difference:
    // a canopy warmer than the air (the usual daytime case) radiates more
    // than this predicts.
    return emiss_landcover * kStefanBoltzmann * (1.0 - sky_view) *
           kelvin4(t_air_c);
}

double netRadiativeFluxOut(double t_water_c, double t_air_c,
                           double humidity_pct,
                           const RadiativeConfig& cfg) noexcept {
    const double jsn = netShortwave(cfg.shortwave_wm2, cfg.albedo,
                                    cfg.shade_factor);
    const double jbr = backLongwave(t_water_c, cfg.emiss_water);
    const double jan = atmosphericLongwave(t_air_c, humidity_pct,
                                           cfg.atm_emiss_coeff,
                                           cfg.lw_reflection, cfg.sky_view);
    const double jlc = landCoverLongwave(t_air_c, cfg.emiss_landcover,
                                         cfg.sky_view);
    // Sign flip lives here and nowhere else: three terms warm the water,
    // one cools it, and this module's callers expect positive = OUT.
    return jbr - jsn - jan - jlc;
}

void applyRadiativeExchange(SimulationContext& ctx, double dt) {
    if (!ctx.options.heat_transport || !ctx.heat_config.radiative_exchange)
        return;
    if (!(dt > 0.0)) return;

    auto& hs  = ctx.heat_state;
    const auto& cfg = ctx.heat_config.radiative;
    const double t_air = airTempCelsius(ctx);
    const double rh    = ctx.climate_state.humidity;
    const double rho   = ctx.options.water_density;
    const double cp    = ctx.options.water_specific_heat;
    const int unit_sys =
        ucf::getUnitSystem(static_cast<int>(ctx.options.flow_units));

    // Same free surfaces as SurfaceExchange — storage nodes and open
    // conduits, which is where evaporation acts too. Sunlight does not
    // reach a closed pipe any more than wind does.
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

        const double j_out = netRadiativeFluxOut(hs.node_temp[ui], t_air, rh,
                                                 cfg);
        const double hc = rho * cp * vol_ft3 * kCuFtToCuM;
        if (hc > 0.0)
            hs.node_temp[ui] += -j_out * area_ft2 * kSqFtToSqM * dt / hc;
    }

    const int nl = ctx.n_links();
    const auto& CD = ctx.link_subtypes.conduits;
    for (int j = 0; j < nl; ++j) {
        const auto uj = static_cast<std::size_t>(j);
        if (uj >= hs.link_temp.size()) break;
        const double vol_ft3 = ctx.links.volume[uj];
        if (!(vol_ft3 > 0.0)) continue;
        const int cr = ctx.link_subtypes.conduit_row(j);
        if (cr < 0) continue;
        const auto ucr = static_cast<std::size_t>(cr);
        if (!xsect::isOpen(ctx.links.xsect_batch_shape[uj])) continue;

        double length = CD.length[ucr];
        if (!(length > 0.0)) length = CD.mod_length[ucr];
        if (!(length > 0.0)) continue;
        const double depth = ctx.links.depth[uj];
        if (!(depth > 0.0)) continue;

        XSectParams xs{};
        const auto ls = ctx.links.xsect_shape[uj];
        xs.type   = (ls == XsectShape::DUMMY) ? 0 : static_cast<int>(ls) + 1;
        xs.y_full = ctx.links.xsect_y_full[uj];
        xs.a_full = ctx.links.xsect_a_full[uj];
        xs.w_max  = ctx.links.xsect_w_max[uj];
        xs.r_full = ctx.links.xsect_r_full[uj];
        xs.s_full = ctx.links.xsect_s_full[uj];
        xs.s_max  = ctx.links.xsect_s_max[uj];
        xs.y_bot  = ctx.links.xsect_y_bot[uj];
        xs.a_bot  = ctx.links.xsect_a_bot[uj];
        xs.s_bot  = ctx.links.xsect_s_bot[uj];
        xs.r_bot  = ctx.links.xsect_r_bot[uj];
        const double top_width = xsect::getWofY(xs, depth);
        if (!(top_width > 0.0)) continue;

        const double area_ft2 = top_width * length * CD.barrels[ucr];
        const double j_out = netRadiativeFluxOut(hs.link_temp[uj], t_air, rh,
                                                 cfg);
        const double hc = rho * cp * vol_ft3 * kCuFtToCuM;
        if (hc > 0.0)
            hs.link_temp[uj] += -j_out * area_ft2 * kSqFtToSqM * dt / hc;
    }
}

}  // namespace openswmm::transport::heat
