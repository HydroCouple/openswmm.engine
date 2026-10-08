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

#include "../../../data/HeatOverrideData.hpp"   // HeatElement (PE1)

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
///
/// H6a: `cloud_factor` is Bolz's `1 + k_lw C²` from
/// `SolarRadiation::cloudLongwaveFactor`. It DEFAULTS TO 1.0 and the
/// default is not decorative — every H3 gate calls the two-argument form,
/// so the clear-sky value is unchanged by construction rather than by
/// arithmetic that happens to round the same way.
double atmosphericEmissivity(double e_a_kpa, double atm_emiss_coeff,
                             double cloud_factor = 1.0) noexcept;

/// Atmospheric longwave [W/m²], INTO the water.
double atmosphericLongwave(double t_air_c, double humidity_pct,
                           double atm_emiss_coeff, double lw_reflection,
                           double sky_view,
                           double cloud_factor = 1.0) noexcept;

/// Land-cover longwave [W/m²], INTO the water.
double landCoverLongwave(double t_air_c, double emiss_landcover,
                         double sky_view) noexcept;

/// Net radiative flux [W/m²], **positive OUT of the water** so it sums with
/// SurfaceExchange's `Je + Jc`.
///
/// H6a adds two optional arguments and changes nothing without them:
///   - `jin_wm2 < 0` (the default) means "use `cfg.shortwave_wm2`", the H3
///     behaviour. A caller with a resolved per-step `Jin` passes it here.
///     The sentinel is negative rather than NaN because a deck may legally
///     mean 0 W/m² (night), and 0 must not be mistaken for "unset".
///   - `cloud_factor` defaults to 1.0 — see `atmosphericEmissivity`.
double netRadiativeFluxOut(double t_water_c, double t_air_c,
                           double humidity_pct, const RadiativeConfig& cfg,
                           double jin_wm2 = -1.0,
                           double cloud_factor = 1.0) noexcept;

/// This module's contribution to the net outward flux at `t_w` [W/m²].
/// Returns 0 unless HEAT_TRANSPORT is on AND `[HEAT_FLUXES]
/// RADIATIVE_EXCHANGE` is enabled, so a caller sums it unconditionally.
///
/// D-H5e: this module no longer has a binding of its own. It had one, with
/// its own element traversal and its own explicit conversion, and that is
/// how it came to relax separately from SurfaceExchange toward a different
/// equilibrium — making the answer depend on which module ran last.
double radiativeFluxOut(const SimulationContext& ctx,
                        const HeatElement& elem, double t_w) noexcept;

}  // namespace openswmm::transport::heat

#endif  // OPENSWMM_ENGINE_TRANSPORT_RADIATIVE_EXCHANGE_HPP
