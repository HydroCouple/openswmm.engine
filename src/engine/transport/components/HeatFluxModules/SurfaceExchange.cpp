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
 * @file      SurfaceExchange.cpp
 * @brief     Phase H2 body — latent/sensible surface exchange.
 *
 * @details D-H5e moved the node and link BINDING out of this file into
 *          HeatFluxes.cpp. What remains is the formulation plus one
 *          evaluator, `surfaceFluxOut`, which is this module's contribution
 *          to a single net flux. Owning a binding of its own is what let
 *          this module and RadiativeExchange each relax separately toward
 *          their own equilibrium.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include "SurfaceExchange.hpp"

#include <algorithm>
#include <cmath>

#include "../../../core/SimulationContext.hpp"

namespace openswmm::transport::heat {

namespace {

/**
 * @brief The module's own net outward flux at a given water temperature, W/m². Written once
 * because `relaxT` needs it evaluated TWICE per element — at `T` and at `T + kProbeC` — and two
 * hand-inlined copies of a three-call expression is how a probe silently comes to measure a
 * different function than the one being stepped.
 *
 * @details Sum pure latent and Bowen sensible surface flux at held SI weather.
 * @param[in] t_w Water degC, finite qualified physical temperature.
 * @param[in] t_air Air degC.
 * @param[in] rh Relative humidity percent, normally[0,100].
 * @param[in] wind_ms Wind speed m/s, negative floored0 by wind function.
 * @param[in] a Wind-function intercept m/s/kPa.
 * @param[in] b Wind-function slope giving m/s/kPa per m/s wind.
 * @param[in] rho Positive water density kg/m3.
 * @param[in] p_ratio Dimensionless atmospheric pressure ratio for Bowen correction.
 * @return Latent+sensible W/m2, positive heat out of water; condensation may be negative.
 * @note Pure/allocation-free, no parameter/finite/domain validation. Temperature
 * singularity/overflow or nonfinite weather can propagate; near-zero vapor deficit gives Bowen0,
 * not an error channel.
 */
double netFluxOut(double t_w, double t_air, double rh, double wind_ms, double a, double b,
                  double rho, double p_ratio) noexcept {
    const double je = latentFlux(t_w, t_air, rh, wind_ms, a, b, rho);
    return je + sensibleFlux(je, bowenRatio(t_w, t_air, rh, p_ratio));
}

} // namespace

/**
 * @brief Convert held native climate air temperature from Fahrenheit to Celsius.
 * @param[in] ctx Stable climate_state.temperature in degF.
 * @return (F-32)*5/9 in degC, without finite repair or element override lookup. Read-only/no
 * allocation.
 */
double airTempCelsius(const SimulationContext& ctx) noexcept {
    return (ctx.climate_state.temperature - 32.0) * 5.0 / 9.0;
}

/**
 * @param[in] dt Positive accepted interval s; nonpositive/NaN returns0.
 * @param[in] rho Water density kg/m3; rho*cp*vol_m3 must be positive finite for qualified
 * physics.
 * @param[in] cp Water specific heat J/(kg*K).
 * @note Inputs are read-only and borrowed only as values. Positive damping uses exact linearized
 * exponential relaxation, with finite-result check; invalid/nonpositive
 * probe/derivative/coefficient or nonfinite relaxed result falls back to explicit increment.
 * This fallback is not guaranteed finite, admissible or non-overshooting. Nonpositive/NaN heat
 * capacity/area returns0; no independent upper/finite check on all inputs. No water/energy
 * owner/clock mutation or allocation.
 */
double relaxT(double j0, double j1, double h, double area_m2, double vol_m3, double dt, double rho,
              double cp) noexcept {
    const double heat_capacity = rho * cp * vol_m3;
    if (!(heat_capacity > 0.0) || !(area_m2 > 0.0) || !(dt > 0.0))
        return 0.0;

    // The explicit step, kept as the fallback and as the small-dt limit this
    // must agree with. It is deliberately NOT exported: it is the form that
    // diverged, and the only safe place to reach it is from inside here.
    const double explicit_dT = -j0 * area_m2 * dt / heat_capacity;
    if (!(h > 0.0))
        return explicit_dT;

    const double djdt = (j1 - j0) / h; // W/m²/°C
    // J′ ≤ 0 means the outward flux does not grow with temperature, so the
    // linearized system has no fixed point to relax onto. There is nothing
    // to be stable about, so take the explicit step and let the caller's
    // finiteness check catch it.
    if (!(djdt > 0.0))
        return explicit_dT;

    const double k = area_m2 * djdt / heat_capacity; // 1/s
    if (!(k > 0.0))
        return explicit_dT;

    // ΔT = (J₀/J′)·expm1(−k·dt). expm1 rather than exp(x)−1 so the small-dt
    // limit is accurate to full precision instead of cancelling to noise —
    // that limit is what makes this agree with H2/H3's existing answers.
    const double dT = (j0 / djdt) * std::expm1(-k * dt);
    return std::isfinite(dT) ? dT : explicit_dT;
}

/**
 * @brief Compute the positive-damping linearized equilibrium temperature.
 * @param[in] t0_c Initial water degC.
 * @param[in] j0 Outward flux W/m2 at initial temperature.
 * @param[in] j1 Outward flux at t0_c+h.
 * @param[in] h Positive finite probe offset degC.
 * @return t0_c-j0/((j1-j0)/h), or t0_c for nonpositive/NaN probe or derivative. No finite-result
 * repair; qualified inputs avoid singular physics. Pure, allocation-free, no owner/clock
 * mutation.
 */
double equilibriumT(double t0_c, double j0, double j1, double h) noexcept {
    if (!(h > 0.0))
        return t0_c;
    const double djdt = (j1 - j0) / h;
    if (!(djdt > 0.0))
        return t0_c;
    return t0_c - j0 / djdt;
}

/**
 * @brief Compute saturation vapor pressure from the Magnus water-temperature formula.
 * @param[in] t_c Qualified finite water/air temperature degC, away from -237.3 singular
 * denominator.
 * @return 0.61275*exp(17.27*T/(237.3+T)) kPa; no domain/finite/range repair.
 * @note Pure/allocation-free, no state/clock effect; caller validates physical applicability.
 */
double saturationVapourPressure(double t_c) noexcept {
    return 0.61275 * std::exp(17.27 * t_c / (237.3 + t_c));
}

/**
 * @brief Compute temperature-dependent latent heat of vaporization.
 * @param[in] t_c Qualified water temperature degC.
 * @return 1000*(2499-2.36*T) J/kg, without sign/finite clamp. Pure/allocation-free, no state
 * mutation.
 */
double latentHeatOfVaporization(double t_c) noexcept {
    return 1000.0 * (2499.0 - 2.36 * t_c);
}

/**
 * @brief Evaluate the mass-transfer wind function with nonnegative wind speed.
 * @param[in] wind_ms Wind m/s, negative floored0; no general NaN/finite repair.
 * @param[in] a Intercept m/s/kPa.
 * @param[in] b Slope in (m/s/kPa)/(m/s).
 * @return a+b*max(wind_ms,0), m/s/kPa. No coefficient positivity validation/allocation/state
 * mutation.
 */
double windFunction(double wind_ms, double a, double b) noexcept {
    return a + b * std::max(wind_ms, 0.0);
}

/**
 * @brief Compute signed vapor-deficit-driven water surface evaporation velocity.
 * @param[in] t_water_c Qualified water degC.
 * @param[in] t_air_c Qualified air degC.
 * @param[in] humidity_pct Relative humidity percent, normally[0,100]; not clamped here.
 * @param[in] wind_ms Wind m/s, negative floored0.
 * @param[in] a Mass-transfer intercept m/s/kPa.
 * @param[in] b Mass-transfer wind slope (m/s/kPa)/(m/s).
 * @return f(w)*(es(Twater)-RH/100*es(Tair)) m/s; positive evaporation, negative condensation.
 * @note Pure/allocation-free; no temperature/weather/finite validation or canonical water
 * withdrawal.
 */
double evaporationRate(double t_water_c, double t_air_c, double humidity_pct, double wind_ms,
                       double a, double b) noexcept {
    const double es_w = saturationVapourPressure(t_water_c);
    const double e_a = (humidity_pct / 100.0) * saturationVapourPressure(t_air_c);
    return windFunction(wind_ms, a, b) * (es_w - e_a);
}

/**
 * @brief Convert signed surface evaporation into outward latent heat flux.
 * @param[in] t_water_c Water degC for vapor pressure and latent heat.
 * @param[in] t_air_c Air degC.
 * @param[in] humidity_pct Relative humidity percent.
 * @param[in] wind_ms Wind m/s, negative floored0.
 * @param[in] a Wind-function intercept m/s/kPa.
 * @param[in] b Wind-function slope (m/s/kPa)/(m/s).
 * @param[in] water_density Positive water density kg/m3.
 * @return rho*Le(Twater)*evaporationRate in W/m2, positive cooling, negative condensation
 * heating.
 * @note Pure/no allocation; physical range/finite checks and accepted evaporation-water pairing
 * are caller responsibilities. No canonical water/heat or engine-clock mutation.
 */
double latentFlux(double t_water_c, double t_air_c, double humidity_pct, double wind_ms, double a,
                  double b, double water_density) noexcept {
    const double e = evaporationRate(t_water_c, t_air_c, humidity_pct, wind_ms, a, b);
    return water_density * latentHeatOfVaporization(t_water_c) * e;
}

/**
 * @brief Compute the dimensionless Bowen sensible-to-latent ratio at held weather.
 * @param[in] t_water_c Water degC.
 * @param[in] t_air_c Air degC.
 * @param[in] humidity_pct Relative humidity percent, normally[0,100].
 * @param[in] pressure_ratio Dimensionless atmospheric pressure ratio.
 * @return kBowenCoeff*pressure_ratio*(Twater-Tair)/vapor_deficit, or0 when absolute
 * deficit<1e-12kPa.
 * @note No finite/range validation; near-zero return implements current singular fallback.
 * Pure/no allocation.
 */
double bowenRatio(double t_water_c, double t_air_c, double humidity_pct,
                  double pressure_ratio) noexcept {
    const double es_w = saturationVapourPressure(t_water_c);
    const double e_a = (humidity_pct / 100.0) * saturationVapourPressure(t_air_c);
    const double deficit = es_w - e_a;
    // Removable singularity: as the deficit vanishes so does Je, and Jc =
    // Br·Je tends to a finite limit. Returning 0 here keeps Jc finite
    // instead of producing inf/NaN that would then propagate into every
    // downstream temperature.
    if (std::fabs(deficit) < 1.0e-12)
        return 0.0;
    return kBowenCoeff * pressure_ratio * (t_water_c - t_air_c) / deficit;
}

/**
 * @brief Form signed outward sensible flux from latent flux and Bowen ratio.
 * @param[in] latent_flux Signed latent W/m2.
 * @param[in] bowen_ratio Dimensionless held ratio.
 * @return Product W/m2. Pure/no allocation; no sign/finite checks or physical/clock mutation.
 */
double sensibleFlux(double latent_flux, double bowen_ratio) noexcept {
    return bowen_ratio * latent_flux;
}

/**
 * @brief Evaluate one element's enabled latent+sensible outward atmospheric flux.
 * @param[in] ctx Stable heat/process configuration, native climate and already prepared element
 * forcing providers/caches; query callbacks must be nonthrowing for this noexcept path.
 * @param[in] elem Borrowed node/link/subcatch/global identity selecting held weather overrides.
 * @param[in] t_w Qualified water temperature degC.
 * @return 0 unless heat transport+surface exchange enabled; otherwise signed W/m2 positive
 * cooling.
 * @note Reads element air degF/humidity percent/wind mph with climate fallback, converts weather
 * to SI and delegates pure flux sum with configured coefficients/density/pressure ratio. Does
 * not sample a new weather calendar, apply a water sink, alter thermal energy or advance clock.
 * Stable exclusive/ const-consistent provider binding; no general finite/physical validation or
 * allocation by helper.
 */
double surfaceFluxOut(const SimulationContext& ctx, const HeatElement& elem, double t_w) noexcept {
    if (!ctx.options.heat_transport || !ctx.heat_config.surface_exchange)
        return 0.0;
    // PE4: air temperature, humidity and wind are GLOBAL in the deck and
    // per-element only through the forcing API. Resolving them HERE rather
    // than writing them into `climate_state` is what keeps a per-link push
    // out of snowmelt and evaporation — see ForcingData::elementClimate.
    const double t_air = ctx.forcing.elementAirTempF(elem, ctx.climate_state.temperature);
    const double rh = ctx.forcing.elementHumidity(elem, ctx.climate_state.humidity);
    const double wind = ctx.forcing.elementWindMph(elem, ctx.climate_state.wind_speed);
    return netFluxOut(t_w, (t_air - 32.0) * 5.0 / 9.0, rh, wind * kMphToMs,
                      ctx.options.wind_func_coeff_a, ctx.options.wind_func_coeff_b,
                      ctx.options.water_density, ctx.options.pressure_ratio);
}

} // namespace openswmm::transport::heat
