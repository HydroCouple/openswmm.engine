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
 * @file      FrozenGround.cpp
 * @brief     Subcatchment binding of the frozen-ground slab.
 * @ingroup   engine_hydrology
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include "FrozenGround.hpp"

#include "../core/SimulationContext.hpp"
#include "Runoff.hpp"

#include <algorithm>

namespace openswmm::frost {

namespace {
constexpr double kFootInMeters = 0.3048;
}

std::string validate(const Slab& s) {
    const auto positive = [](double v) { return std::isfinite(v) && v > 0.0; };
    if (!positive(s.thickness))
        return "thickness must be positive";
    if (!positive(s.conductivity))
        return "conductivity must be positive";
    if (!positive(s.heat_capacity))
        return "heat capacity must be positive";
    if (!std::isfinite(s.water_content) || s.water_content < 0.0 || s.water_content > 1.0)
        return "water content must lie between 0 and 1";
    if (!std::isfinite(s.deep_temp) || !std::isfinite(s.init_temp))
        return "temperatures must be finite";
    if (!std::isfinite(s.deep_depth) || s.deep_depth <= 0.5 * s.thickness)
        return "deep depth must exceed half the thickness";
    if (!std::isfinite(s.exponent) || s.exponent < 1.0)
        return "exponent must be at least 1";
    if (!positive(s.snow_conductivity))
        return "snow conductivity must be positive";
    if (!positive(s.snow_density))
        return "snow density must be positive";
    return {};
}

std::string initialize(SimulationContext& ctx) {
    auto& f = ctx.frozen_ground;
    const auto rows = std::move(f.rows);
    auto mesh_rows = std::move(f.mesh_rows);
    f = {};
    f.rows = rows;
    f.mesh_rows = std::move(mesh_rows);
    if (rows.empty())
        return {};
    const auto n = static_cast<std::size_t>(ctx.n_subcatches());
    std::vector<char> active(n, 0);
    std::vector<Slab> slab(n);
    // The default row first, so a named row wins wherever the two appear.
    for (const bool named : {false, true})
        for (const auto& [name, row] : rows) {
            if ((name != "*") != named)
                continue;
            if (const std::string problem = validate(row); !problem.empty())
                return "[FROZEN_GROUND] " + name + ": " + problem + ".";
            if (!named) {
                std::fill(slab.begin(), slab.end(), row);
                std::fill(active.begin(), active.end(), char(1));
                continue;
            }
            const int i = ctx.subcatch_names.find(name);
            if (i < 0)
                return "[FROZEN_GROUND] names an unknown subcatchment " + name + ".";
            slab[static_cast<std::size_t>(i)] = row;
            active[static_cast<std::size_t>(i)] = 1;
        }
    f.active = std::move(active);
    f.slab = std::move(slab);
    f.enthalpy.assign(n, 0.0);
    f.temperature.assign(n, 0.0);
    f.frozen.assign(n, 0.0);
    f.factor.assign(n, 1.0);
    f.initial_j.assign(n, 0.0);
    f.surface_j.assign(n, 0.0);
    f.deep_j.assign(n, 0.0);
    f.water_j.assign(n, 0.0);
    for (std::size_t i = 0; i < n; ++i)
        if (f.active[i]) {
            f.enthalpy[i] = f.initial_j[i] = enthalpyAt(f.slab[i], f.slab[i].init_temp);
            f.derive(i);
        }
    return {};
}

void advanceSubcatchments(SimulationContext& ctx, const runoff::RunoffSoA& soa, double dt,
                          const std::vector<double>& snow_water,
                          const std::vector<double>& snow_cover) {
    auto& f = ctx.frozen_ground;
    if (!f.enabled() || dt <= 0.0)
        return;
    const double air = (ctx.climate_state.temperature - 32.0) / 1.8;
    const auto& heat = ctx.heat_state;
    const bool heat_on = ctx.options.heat_transport;
    const std::size_t n = f.enthalpy.size();
    for (std::size_t i = 0; i < n; ++i) {
        if (!f.active[i])
            continue;
        const Slab& s = f.slab[i];
        const double pervious_ft2 = soa.area[i] * (1.0 - soa.imperv_pct[i]);
        if (pervious_ft2 <= 0.0)
            continue;
        const double cover = i < snow_cover.size() ? std::clamp(snow_cover[i], 0.0, 1.0) : 0.0;
        const double swe =
            i < snow_water.size() ? std::max(snow_water[i], 0.0) * kFootInMeters : 0.0;
        // The open surface is at the ponded water's temperature when heat transport tracks it,
        // and at the air temperature otherwise.
        const std::size_t perv = i * 3 + 2;
        const bool ponded = soa.depth_perv[i] > 0.0;
        const double open_temp =
            heat_on && ponded && perv < heat.subarea_temp.size() ? heat.subarea_temp[perv] : air;
        // Water that infiltrated this step, m/s over the pervious area. Melt reaches the ground
        // at 0 C; rain and ponded water cannot be colder than that.
        const double rate = soa.infil_vol[i] / (pervious_ft2 * dt) * kFootInMeters;
        const Contact contacts[4] = {
            {(1.0 - cover) * halfSlabConductance(s), open_temp},
            {cover * snowContactConductance(s, swe), std::min(air, 0.0)},
            {deepConductance(s), s.deep_temp},
            {kWaterCapacity * std::max(rate, 0.0), std::max(open_temp, 0.0)},
        };
        double received[4];
        f.enthalpy[i] = advance(s, f.enthalpy[i], contacts, 4, dt, received);
        f.surface_j[i] += received[0] + received[1];
        f.deep_j[i] += received[2];
        f.water_j[i] += received[3];
        f.derive(i);
    }
}

} // namespace openswmm::frost
