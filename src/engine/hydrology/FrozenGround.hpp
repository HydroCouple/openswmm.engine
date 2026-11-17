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
 * @file      FrozenGround.hpp
 * @brief     Frozen-ground slab: heat conduction with freeze-thaw, and the infiltration limit.
 *
 * @details One slab of user thickness per infiltrating surface holds a single enthalpy H
 *          [J/m2] measured from unfrozen ground at 0 C. Temperature and the frozen fraction
 *          are functions of H and are never stored on their own:
 *
 *            H >= 0            T = H / (C D)            phi = 0
 *            -Hf < H < 0       T = 0                    phi = -H / Hf
 *            H <= -Hf          T = (H + Hf) / (C D)     phi = 1
 *
 *          with Hf = Lf rho_w theta D the latent heat held by the slab's freezable water.
 *          The slab exchanges heat with any number of linear contacts G (T_ext - T), W/m2:
 *          the bare or ponded surface, a snow-covered surface behind the pack's resistance,
 *          the deep ground, and the water that infiltrates. One step is a backward-Euler
 *          solve of H1 = H0 + dt sum G (T_ext - T(H1)); T(H) is monotone and piecewise linear,
 *          so the solve is exact, unconditionally stable and cannot overshoot a contact.
 *
 *          The infiltration limit is factor = (1 - phi)^m: the fraction of the surface that
 *          still takes water. It is 1 on unfrozen ground and exactly 0 once the slab's
 *          freezable water is all ice.
 *
 *          All quantities are SI (m, s, J, W, K, kg); temperatures are Celsius.
 *
 * @see plans/workplans/FROZEN_GROUND_INFILTRATION_PLAN_2026-11-17.md
 * @ingroup engine_hydrology
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_HYDROLOGY_FROZEN_GROUND_HPP
#define OPENSWMM_ENGINE_HYDROLOGY_FROZEN_GROUND_HPP

#include <cmath>
#include <string>
#include <utility>
#include <vector>

namespace openswmm {
struct SimulationContext;
namespace runoff {
struct RunoffSoA;
}
} // namespace openswmm

namespace openswmm::frost {

constexpr double kLatentFusion = 334000.0;   ///< J/kg, latent heat of fusion of water
constexpr double kWaterDensity = 1000.0;     ///< kg/m3
constexpr double kWaterCapacity = 4186000.0; ///< J/(m3 K), volumetric heat capacity of water
constexpr int kColumns = 10;                 ///< value columns of one [FROZEN_GROUND] row

/// One [FROZEN_GROUND] row. All SI, Celsius.
struct Slab {
    double thickness = 0.15;         ///< m, frost-active layer
    double conductivity = 1.5;       ///< W/(m K), bulk ground
    double heat_capacity = 2.4e6;    ///< J/(m3 K), bulk ground
    double water_content = 0.30;     ///< m3/m3, water that can freeze
    double deep_temp = 8.0;          ///< C, ground temperature at deep_depth
    double deep_depth = 3.0;         ///< m below the surface, > thickness / 2
    double init_temp = 5.0;          ///< C, slab temperature at the start
    double exponent = 11.0;          ///< m in (1 - phi)^m, >= 1
    double snow_conductivity = 0.25; ///< W/(m K)
    double snow_density = 250.0;     ///< kg/m3
};

/// A linear heat contact: flux into the slab = conductance * (temperature - T_slab).
struct Contact {
    double conductance = 0.0; ///< W/(m2 K), >= 0
    double temperature = 0.0; ///< C
};

/// Latent heat held by the slab's freezable water, J/m2.
inline double latentStore(const Slab& s) noexcept {
    return kLatentFusion * kWaterDensity * s.water_content * s.thickness;
}

/// Sensible heat capacity of the slab, J/(m2 K).
inline double sensibleStore(const Slab& s) noexcept {
    return s.heat_capacity * s.thickness;
}

/// Enthalpy of a slab at a temperature. A slab at exactly 0 C is taken as unfrozen.
inline double enthalpyAt(const Slab& s, double temperature) noexcept {
    return temperature >= 0.0 ? sensibleStore(s) * temperature
                              : sensibleStore(s) * temperature - latentStore(s);
}

inline double temperatureOf(const Slab& s, double enthalpy) noexcept {
    if (enthalpy >= 0.0)
        return enthalpy / sensibleStore(s);
    const double hf = latentStore(s);
    return enthalpy > -hf ? 0.0 : (enthalpy + hf) / sensibleStore(s);
}

inline double frozenFraction(const Slab& s, double enthalpy) noexcept {
    if (enthalpy >= 0.0)
        return 0.0;
    const double hf = latentStore(s);
    return hf > 0.0 && enthalpy > -hf ? -enthalpy / hf : 1.0;
}

/// Fraction of the surface that still infiltrates: (1 - phi)^m, exactly 0 when phi = 1.
inline double infiltrationFactor(const Slab& s, double enthalpy) noexcept {
    const double open = 1.0 - frozenFraction(s, enthalpy);
    return open <= 0.0 ? 0.0 : open >= 1.0 ? 1.0 : std::pow(open, s.exponent);
}

/// Conductance from the slab's center to its upper face, W/(m2 K).
inline double halfSlabConductance(const Slab& s) noexcept {
    return 2.0 * s.conductivity / s.thickness;
}

/// Conductance from the slab's center through a snow pack of the given water equivalent [m].
inline double snowContactConductance(const Slab& s, double snow_water_m) noexcept {
    const double depth = snow_water_m * kWaterDensity / s.snow_density;
    return 1.0 / (1.0 / halfSlabConductance(s) + depth / s.snow_conductivity);
}

/// Conductance from the slab's center to the deep ground temperature, W/(m2 K).
inline double deepConductance(const Slab& s) noexcept {
    return s.conductivity / (s.deep_depth - 0.5 * s.thickness);
}

/**
 * @brief Advance the slab enthalpy one backward-Euler step against linear contacts.
 * @param s Validated slab.
 * @param enthalpy Starting enthalpy, J/m2.
 * @param contacts Borrowed contacts; a zero conductance contributes nothing.
 * @param n Contact count.
 * @param dt Step, s, > 0.
 * @param[out] received Optional n doubles: joules per m2 received from each contact over the
 *             step. Their sum is the enthalpy change up to round-off.
 * @return Ending enthalpy, J/m2.
 */
inline double advance(const Slab& s, double enthalpy, const Contact* contacts, int n, double dt,
                      double* received = nullptr) noexcept {
    double g = 0.0, gt = 0.0;
    for (int i = 0; i < n; ++i) {
        g += contacts[i].conductance;
        gt += contacts[i].conductance * contacts[i].temperature;
    }
    const double c = sensibleStore(s), hf = latentStore(s);
    // Unfrozen branch: H = c T, T >= 0.
    double t = (enthalpy + dt * gt) / (c + dt * g);
    if (t < 0.0) {
        // Frozen branch: H = c T - hf, T <= 0.
        t = (enthalpy + hf + dt * gt) / (c + dt * g);
        if (t > 0.0)
            t = 0.0; // on the plateau: the slab stays at 0 C and only its ice changes
    }
    double sum = 0.0;
    for (int i = 0; i < n; ++i) {
        const double j = dt * contacts[i].conductance * (contacts[i].temperature - t);
        if (received)
            received[i] = j;
        sum += j;
    }
    return enthalpy + sum;
}

/// Validate one row. Returns an empty string when the row can be used.
std::string validate(const Slab& s);

/**
 * @brief Authored rows, resolved per-subcatchment parameters, and the slab state.
 * @details Inactive (every vector empty, no per-step work) until a [FROZEN_GROUND] section
 *          supplies at least one row. `factor` is what the runoff solver reads.
 */
struct FrozenGroundData {
    /// Authored rows in file order; the name "*" is the default for unlisted subcatchments.
    std::vector<std::pair<std::string, Slab>> rows;
    /// Authored [2D_FROZEN_GROUND] rows for mesh cells; only the "*" row is supported.
    std::vector<std::pair<std::string, Slab>> mesh_rows;

    // Resolved by initialize(): one entry per subcatchment.
    std::vector<char> active;
    std::vector<Slab> slab;
    // State.
    std::vector<double> enthalpy; ///< J/m2
    // Derived from enthalpy after every step.
    std::vector<double> temperature, frozen, factor;
    // Cumulative heat received per m2 of pervious area since the start, J/m2.
    std::vector<double> initial_j, surface_j, deep_j, water_j;

    bool enabled() const noexcept { return !enthalpy.empty(); }
    /// Factor for a subcatchment: 1 when the model is off or the subcatchment has no row.
    double factorOf(std::size_t i) const noexcept { return i < factor.size() ? factor[i] : 1.0; }
    /// Recompute temperature, frozen fraction and factor from enthalpy for one subcatchment.
    void derive(std::size_t i) noexcept {
        temperature[i] = temperatureOf(slab[i], enthalpy[i]);
        frozen[i] = frozenFraction(slab[i], enthalpy[i]);
        factor[i] = infiltrationFactor(slab[i], enthalpy[i]);
    }
};

/**
 * @brief Resolve authored rows against the subcatchment names and seed the slab state.
 * @return Empty on success, otherwise the first problem (unknown subcatchment, invalid row).
 * @post With no authored rows every vector is empty and the model is off.
 */
std::string initialize(SimulationContext& ctx);

/**
 * @brief Advance every active subcatchment slab over one runoff step.
 * @param ctx Climate air temperature, ponded temperatures (when heat transport is on) and the
 *            infiltration rates the runoff step just produced.
 * @param soa Runoff state of the same step (pervious ponded depth).
 * @param dt Runoff step, s.
 * @param snow_water Per-subcatchment pervious snow water equivalent, ft; may be empty.
 * @param snow_cover Per-subcatchment pervious snow-covered fraction in [0, 1]; may be empty.
 * @post Updates enthalpy, the derived rows and the cumulative heat ledgers. The new factor is
 *       used by the NEXT runoff step.
 */
void advanceSubcatchments(SimulationContext& ctx, const runoff::RunoffSoA& soa, double dt,
                          const std::vector<double>& snow_water,
                          const std::vector<double>& snow_cover);

} // namespace openswmm::frost

#endif // OPENSWMM_ENGINE_HYDROLOGY_FROZEN_GROUND_HPP
