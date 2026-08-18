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
 * @file SurfaceExchange.hpp
 * @brief Phase H2 — latent and sensible heat exchange at the water surface
 *        (heat plan §2.1; CSHComponent §4.4–4.5).
 *
 * @details The formulations are exposed as PURE FUNCTIONS above the engine
 *          binding on purpose: they are the part with published reference
 *          values (CSH Table 4.1, Dingman 2008, Martin & McCutcheon 1998),
 *          so they can be gated against numbers from outside this codebase
 *          rather than against the engine's own output.
 *
 *          - `e_s(T) = 0.61275 exp(17.27 T / (237.3 + T))`   [kPa]
 *          - `Le(T)  = 1000 (2499 − 2.36 T)`                 [J/kg]
 *          - `f(w)   = a + b w`                              [m/s/kPa]
 *          - `E      = f(w) (e_s(Tw) − e_a)`                 [m/s]
 *          - `Je     = ρw Le E`                              [W/m²]
 *          - `Br     = CB (Pa/P) (Tw − Ta) / (e_s(Tw) − e_a)`
 *          - `Jc     = Br Je`                                [W/m²]
 *
 * @par Sign convention
 *      `Je` and `Jc` are positive when heat LEAVES the water — evaporation
 *      cools. The governing equation (plan §1) carries them as
 *      `− (Je + Jc) / Y`, and `applySurfaceExchange` applies that sign, so a
 *      caller never has to remember it.
 *
 * @par Which surfaces exchange, and why that question has a precedent
 *      Heat crosses the free surface exactly where evaporation does, so this
 *      module uses the ENGINE'S OWN evaporation surfaces rather than a new
 *      area model:
 *        - storage nodes → `node::getSurfArea` (`Routing.cpp:490`),
 *        - open conduits → `xsect::getWofY(y) · length · barrels`
 *          (`Routing.cpp:597`, `DynamicWave.cpp:2028`),
 *        - junctions, outfalls, dividers and CLOSED conduits → no free
 *          surface, no exchange (legacy's convention: `getSurfArea` returns
 *          0 for them and `xsect::isOpen` gates the conduit branch).
 *      Both of those are solver-independent — `initNodeFlows` and
 *      `computeConduitLosses` run under every routing model — which is what
 *      keeps this module from silently doing nothing under STEADY or
 *      KINWAVE. That was the open question when H2 was scoped.
 *
 * @par Units
 *      The engine is foot-second internally; these formulations are SI.
 *      Conversion happens once, at the binding, and is named
 *      `kSqFtToSqM` / `kCuFtToCuM` rather than folded into a magic number.
 *
 * @see plans/transport/HEAT_TRANSPORT_PLAN.md §2.1, §2.4, §6 H2
 * @ingroup engine_transport
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_TRANSPORT_SURFACE_EXCHANGE_HPP
#define OPENSWMM_ENGINE_TRANSPORT_SURFACE_EXCHANGE_HPP

namespace openswmm {
struct SimulationContext;
}

namespace openswmm::transport::heat {

/// Bowen's coefficient, kPa/°C (CSH §4.5).
inline constexpr double kBowenCoeff = 0.061;

/// Saturation vapour pressure over water at `t_c` degrees Celsius [kPa].
double saturationVapourPressure(double t_c) noexcept;

/// Latent heat of vaporization at `t_c` degrees Celsius [J/kg]
/// (Martin & McCutcheon 1998).
double latentHeatOfVaporization(double t_c) noexcept;

/// Mass-transfer wind function `a + b·w`, w in m/s [m/s/kPa].
double windFunction(double wind_ms, double a, double b) noexcept;

/// Evaporative mass-transfer rate [m/s]. Negative under condensation.
double evaporationRate(double t_water_c, double t_air_c, double humidity_pct,
                       double wind_ms, double a, double b) noexcept;

/// Latent heat flux [W/m²], POSITIVE out of the water.
double latentFlux(double t_water_c, double t_air_c, double humidity_pct,
                  double wind_ms, double a, double b,
                  double water_density) noexcept;

/// Bowen ratio [-]. Returns 0 when the vapour-pressure deficit vanishes,
/// which is the removable singularity in `Br`'s definition, not an error.
double bowenRatio(double t_water_c, double t_air_c, double humidity_pct,
                  double pressure_ratio) noexcept;

/// Sensible heat flux [W/m²], POSITIVE out of the water.
double sensibleFlux(double latent_flux, double bowen_ratio) noexcept;

/**
 * @brief Apply one step of surface exchange to every exchanging element.
 *
 * @details Called from `routeLegacyHeat` at the stage where the water-age
 *          mirror ages its cells — the source-term stage of plan §1's
 *          governing equation. Modifies `ctx.heat_state.node_temp` and
 *          `link_temp` in place. No-op unless HEAT_TRANSPORT is on AND the
 *          module is enabled in `[HEAT_FLUXES]`.
 */
void applySurfaceExchange(SimulationContext& ctx, double dt);

}  // namespace openswmm::transport::heat

#endif  // OPENSWMM_ENGINE_TRANSPORT_SURFACE_EXCHANGE_HPP
