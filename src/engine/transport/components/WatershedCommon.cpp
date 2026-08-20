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

}  // namespace openswmm::transport
