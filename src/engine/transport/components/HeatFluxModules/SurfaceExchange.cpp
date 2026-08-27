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
 * @brief Phase H2 body — latent/sensible surface exchange.
 *
 * @details D-H5e moved the node and link BINDING out of this file into
 *          HeatFluxes.cpp. What remains is the formulation plus one
 *          evaluator, `surfaceFluxOut`, which is this module's contribution
 *          to a single net flux. Owning a binding of its own is what let
 *          this module and RadiativeExchange each relax separately toward
 *          their own equilibrium.
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "SurfaceExchange.hpp"

#include <algorithm>
#include <cmath>

#include "../../../core/SimulationContext.hpp"

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

double surfaceFluxOut(const SimulationContext& ctx, double t_w) noexcept {
    if (!ctx.options.heat_transport || !ctx.heat_config.surface_exchange)
        return 0.0;
    return netFluxOut(t_w, airTempCelsius(ctx), ctx.climate_state.humidity,
                      ctx.climate_state.wind_speed * kMphToMs,
                      ctx.options.wind_func_coeff_a,
                      ctx.options.wind_func_coeff_b,
                      ctx.options.water_density,
                      ctx.options.pressure_ratio);
}

}  // namespace openswmm::transport::heat
