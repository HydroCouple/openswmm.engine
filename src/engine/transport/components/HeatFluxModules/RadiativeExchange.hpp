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
 * @file RadiativeExchange.hpp
 * @brief Phase H3 — shortwave and longwave radiation at the water surface
 *        (heat plan §2.2; RHEComponent §6).
 *
 * @details Four terms, each a pure function so it can be gated against the
 *          reference implementation rather than against this engine's own
 *          output:
 *
 *          - **Net shortwave** `Jsn = (1 − Rs) Jin (1 − fs)` — absorbed
 *            solar.
 *          - **Back longwave** `Jbr = εw σ Tw⁴` — emitted by the water.
 *          - **Atmospheric longwave** `Jan = εatm σ Ta⁴ (1 − RL) fsky`,
 *            Brunt (1932) `εatm = Aa + 0.0027 √(e_a in PASCALS)`.
 *          - **Land-cover longwave** `Jlc = εlc σ Ta⁴ (1 − fsky)`.
 *
 * @par Two corrections to the plan text, taken from the reference
 *      The plan's §2.2 summary omits both, and either would have been a
 *      silent error (`RHEComponent/src/element.cpp:106-135`):
 *      1. **The sky-view factor splits the longwave budget.** `Jan` is
 *         multiplied by `fsky` and `Jlc` by `(1 − fsky)`; they are
 *         complementary shares of the same hemisphere, not independent
 *         terms. Written without it, an open-sky element would double-count
 *         and a fully shaded one would receive atmospheric longwave through
 *         a canopy.
 *      2. **Brunt's square root takes PASCALS**, not the kPa the vapour
 *         pressure is computed in — `0.0027 √(e_a · 1000)`. Feeding kPa
 *         understates the emissivity term by √1000 ≈ 31.6.
 *
 * @par Sign convention
 *      **This module returns POSITIVE = leaving the water**, matching
 *      SurfaceExchange, so the two modules can be summed. The reference
 *      uses the opposite sign (its `netMCRadiation` is positive into the
 *      water, with `backLWRadiation` pre-negated); `netRadiativeFluxOut`
 *      performs the flip in one named place.
 *
 * @par Not carried over from the reference: the sediment split
 *      RHE splits absorbed shortwave into a water share and a bed share
 *      with `exp(−extinction · depth)` and hands the latter to the sediment
 *      column. There is no sediment column until H4, so **H3 keeps all
 *      absorbed shortwave in the water**. That is a deliberate difference,
 *      not an omission: routing bed-bound energy into the water column
 *      overestimates warming in shallow, clear water, and H4 is where the
 *      receiving state appears.
 *
 * @see plans/transport/HEAT_TRANSPORT_PLAN.md §2.2, §6 H3
 * @ingroup engine_transport
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_TRANSPORT_RADIATIVE_EXCHANGE_HPP
#define OPENSWMM_ENGINE_TRANSPORT_RADIATIVE_EXCHANGE_HPP

namespace openswmm {
struct SimulationContext;
struct RadiativeConfig;
}

namespace openswmm::transport::heat {

/// Stefan–Boltzmann constant, W/m²/K⁴ (`rhemodel.cpp:46`).
inline constexpr double kStefanBoltzmann = 5.67e-8;

/// Celsius → Kelvin offset.
inline constexpr double kKelvinOffset = 273.15;

/// Net absorbed shortwave [W/m²], INTO the water.
double netShortwave(double incoming_wm2, double albedo,
                    double shade_factor) noexcept;

/// Back longwave emitted by the water [W/m²], OUT of the water.
double backLongwave(double t_water_c, double emiss_water) noexcept;

/// Brunt atmospheric emissivity [-]. `e_a_kpa` is converted to Pa inside.
double atmosphericEmissivity(double e_a_kpa, double atm_emiss_coeff) noexcept;

/// Atmospheric longwave [W/m²], INTO the water.
double atmosphericLongwave(double t_air_c, double humidity_pct,
                           double atm_emiss_coeff, double lw_reflection,
                           double sky_view) noexcept;

/// Land-cover longwave [W/m²], INTO the water.
double landCoverLongwave(double t_air_c, double emiss_landcover,
                         double sky_view) noexcept;

/// Net radiative flux [W/m²], **positive OUT of the water** so it sums with
/// SurfaceExchange's `Je + Jc`.
double netRadiativeFluxOut(double t_water_c, double t_air_c,
                           double humidity_pct,
                           const RadiativeConfig& cfg) noexcept;

/// Apply one step of radiative exchange to every exchanging element — the
/// same free surfaces SurfaceExchange uses. No-op unless HEAT_TRANSPORT is
/// on AND `[HEAT_FLUXES] RADIATIVE_EXCHANGE` is enabled.
void applyRadiativeExchange(SimulationContext& ctx, double dt);

}  // namespace openswmm::transport::heat

#endif  // OPENSWMM_ENGINE_TRANSPORT_RADIATIVE_EXCHANGE_HPP
