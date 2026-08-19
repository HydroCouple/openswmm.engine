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
 *      `− (Je + Jc) / Y`, and `relaxT` applies that sign, so a
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

/// Air temperature in °C. `ClimateState` carries °F (a legacy convention);
/// every formulation in this program is Celsius. Exported at H5a so the
/// watershed binding cannot acquire a second, drifting copy of the
/// conversion — there is exactly one place that knows the units.
double airTempCelsius(const SimulationContext& ctx) noexcept;

/// Probe offset for the flux derivative, °C. Small enough that `J′` is the
/// local slope, large enough that the difference is not float noise.
inline constexpr double kProbeC = 1.0e-3;

/**
 * @brief One element's temperature change, integrated SEMI-IMPLICITLY [°C].
 *
 * @details Plan D-H5d. The energy balance `ρ cp V dT/dt = −A·J(T)` is
 *          linearized about the current temperature and the resulting linear
 *          ODE is integrated exactly:
 *
 *          ```
 *          k    = A·J′ / (ρ cp V)        [1/s]
 *          T_eq = T₀ − J₀/J′
 *          ΔT   = (J₀/J′)·expm1(−k·dt)
 *          ```
 *
 *          **This replaces a forward-Euler step that diverged to NaN.** Heat
 *          capacity is `ρ cp V`, so a thin film has almost none: a 0.52 ft³
 *          film over 27,226 ft² took a +862 °C step in 60 s and the sequence
 *          ran `5 → 182 → −1.8e4 → −3.9e9 → inf → NaN`. The explicit form had
 *          simply stopped representing the ODE.
 *
 *          Three properties earned this scheme its place, and each is gated:
 *          - **`|ΔT| ≤ |T_eq − T₀|` always** — the step cannot overshoot
 *            equilibrium, whatever `dt` is. That is an assertion needing no
 *            reference value, so no deck or timestep change can stale it.
 *          - **No iteration**, hence no cap to tune and no failure mode when
 *            the cap is reached.
 *          - **Degrades to forward Euler as `dt → 0`** (`expm1(−x) ≈ −x`),
 *            so every small-step answer is unchanged from H2/H3.
 *
 *          The flux FORMULATIONS of H2 and H3 were never wrong; only the
 *          stepping was. CSH runs these same formulas at a 1e-4 s timestep
 *          with a selectable ODE solver — we took its physics and not its
 *          integrator.
 *
 * @param j0      Net flux OUT of the water at `T₀`, W/m² (sum of every
 *                enabled module: `Je + Jc` from here, `Jr` from
 *                RadiativeExchange — all signed positive out, so they add).
 * @param j1      The same sum re-evaluated at `T₀ + h`.
 * @param h       The probe offset used for `j1`; `kProbeC` unless testing.
 * @param area_m2 Exchange area.
 * @param vol_m3  Element volume — the thermal mass, and the reason this
 *                function exists.
 *
 * @return ΔT in °C. Zero for a volume-less or area-less element. Falls back
 *         to the explicit step when `J′ ≤ 0` (anti-damping: the linearized
 *         system has no fixed point to relax onto, so there is nothing to be
 *         stable about).
 */
double relaxT(double j0, double j1, double h, double area_m2, double vol_m3,
              double dt, double rho, double cp) noexcept;

/// The equilibrium temperature the linearization relaxes toward, °C —
/// `T₀ − J₀/J′`. Exported so a gate can assert the no-overshoot property
/// against it directly rather than recomputing the slope by hand.
/// Returns `t0_c` when `J′ ≤ 0`, matching `relaxT`'s fallback.
double equilibriumT(double t0_c, double j0, double j1, double h) noexcept;

/// ft² → m². Exported with `relaxT`, which is useless without it.
inline constexpr double kSqFtToSqM = 0.09290304;
/// ft³ → m³.
inline constexpr double kCuFtToCuM = 0.028316846592;
/// mph → m/s. `ClimateState::wind_speed` is mph.
inline constexpr double kMphToMs = 0.44704;

/**
 * @brief This module's contribution to the net outward flux at `t_w` [W/m²].
 *
 * @details Returns 0 unless HEAT_TRANSPORT is on AND `[HEAT_FLUXES]
 *          SURFACE_EXCHANGE` is enabled, so a caller sums it
 *          unconditionally and the toggle stays this module's business.
 *
 * @par Why this module no longer has a binding of its own (D-H5e)
 *      It did, and so did RadiativeExchange, and each relaxed FULLY toward
 *      its own equilibrium. Under forward Euler the two increments were
 *      linear and added exactly; under relaxation they do NOT commute, so
 *      with both modules on the pair overshot the true combined equilibrium
 *      and the answer depended on module order — at large `k·dt` it landed
 *      on whichever module ran last. Every flux family now sums into one
 *      `J(T)` before a single relaxation, in `HeatFluxes.cpp`.
 */
double surfaceFluxOut(const SimulationContext& ctx, double t_w) noexcept;

}  // namespace openswmm::transport::heat

#endif  // OPENSWMM_ENGINE_TRANSPORT_SURFACE_EXCHANGE_HPP
