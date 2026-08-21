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
 * @file WatershedCommon.cpp
 * @brief The arriving-water rate at a subarea (S1 — snow mixing volume).
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "WatershedCommon.hpp"

#include "../../core/SimulationContext.hpp"

namespace openswmm::transport {

double arrivingPrecipRate(const SimulationContext& ctx, std::size_t ui,
                          int subarea) noexcept {
    const double gage = (ui < ctx.subcatches.rainfall.size())
                            ? ctx.subcatches.rainfall[ui]
                            : 0.0;

    // The IGNORE_SNOWMELT fallback is not redundant, though a deck cannot
    // show it: for such a DECK the whole snow block is skipped and
    // `snow_net_*` never leave the sentinel, so the test below answers the
    // same way. It earns its place when the flag is raised on a run whose
    // pack has already published a rate — the stale arrays Runoff.cpp:551
    // calls harmless — which is reachable because IGNORE_SNOWMELT is
    // settable at runtime (openswmm_model_impl.cpp:1189).
    if (ctx.options.ignore_snow_melt) return gage;
    if (ui >= ctx.subcatches.snowpack.size()) return gage;
    // This one IS redundant, and measurably so: deleting it fails nothing,
    // because `snow_net_*` are written only for subcatchments with a pack
    // (SWMMEngine.cpp:1595) and initialise to -1.0, so the sentinel test
    // below already answers "no pack" correctly. Kept because it mirrors the
    // solver's own two-part guard (Runoff.cpp:548) and would be the thing
    // that saves this function if those arrays ever became unconditional.
    if (ctx.subcatches.snowpack[ui] < 0) return gage;

    // `>= 0.0`, not `> 0.0`: a pack that melted nothing this step publishes a
    // genuine 0.0, and only the -1.0 sentinel means "no pack". This mirrors
    // Runoff.cpp:551-552 exactly — the solver's own test — so the transport
    // mixing volume and the hydrology cannot disagree about whether the pack
    // is in charge.
    const bool perv = (subarea == kSubPERV);
    const auto& src = perv ? ctx.subcatches.snow_net_perv
                           : ctx.subcatches.snow_net_imperv;
    if (ui >= src.size()) return gage;
    const double net = src[ui];
    return (net >= 0.0) ? net : gage;
}

double arrivingMeltFraction(const SimulationContext& ctx, std::size_t ui,
                            int subarea) noexcept {
    if (ctx.options.ignore_snow_melt) return 0.0;
    if (ui >= ctx.subcatches.snowpack.size()) return 0.0;
    if (ctx.subcatches.snowpack[ui] < 0) return 0.0;

    const bool perv = (subarea == kSubPERV);
    const auto& net_v  = perv ? ctx.subcatches.snow_net_perv
                              : ctx.subcatches.snow_net_imperv;
    const auto& melt_v = perv ? ctx.subcatches.snow_melt_perv
                              : ctx.subcatches.snow_melt_imperv;
    if (ui >= net_v.size() || ui >= melt_v.size()) return 0.0;

    const double net  = net_v[ui];
    const double melt = melt_v[ui];
    // The -1.0 sentinel on either array means the pack published nothing.
    if (!(net > 0.0) || !(melt >= 0.0)) return 0.0;
    // Clamped rather than trusted: `net` and `melt` are blended through the
    // same area weights, so melt <= net holds by construction — but a future
    // change to one blend and not the other would otherwise hand a caller a
    // fraction above 1 and silently invert a mixture.
    const double f = melt / net;
    return (f < 0.0) ? 0.0 : (f > 1.0 ? 1.0 : f);
}

double arrivingPrecipTemperature(const SimulationContext& ctx, std::size_t ui,
                                 int subarea) noexcept {
    const double t_rain = ctx.heat_config.global_temp[
        static_cast<int>(HeatSource::RAINFALL)];
    const double f = arrivingMeltFraction(ctx, ui, subarea);
    if (f <= 0.0) return t_rain;
    return (1.0 - f) * t_rain + f * kMeltwaterTempC;
}

double arrivingPrecipAge(const SimulationContext& ctx, std::size_t ui,
                         int subarea) noexcept {
    const double a_rain = ctx.water_age_config.global_age[
        static_cast<int>(WaterAgeSource::RAINFALL)];
    // The SAME fraction the temperature blend uses. See the header.
    const double f = arrivingMeltFraction(ctx, ui, subarea);
    if (f <= 0.0) return a_rain;

    const bool perv = (subarea == kSubPERV);
    const auto& age_v = perv ? ctx.subcatches.snow_melt_age_perv
                             : ctx.subcatches.snow_melt_age_imperv;
    if (ui >= age_v.size()) return a_rain;
    const double a_pack = age_v[ui];
    // The -1.0 sentinel means the pack published no age. Falling back to the
    // rain age rather than to 0 is deliberate: 0 is a REAL age -- the age of
    // water that fell this instant -- so using it as a "missing" marker would
    // make an unpublished pack look like the freshest water in the model.
    // A4's dry-element column has the same shape and H1 left it open.
    if (!(a_pack >= 0.0)) return a_rain;

    return (1.0 - f) * a_rain + f * a_pack;
}

}  // namespace openswmm::transport
