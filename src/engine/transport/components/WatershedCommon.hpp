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
 * @file WatershedCommon.hpp
 * @brief The arriving-water rate at a subarea — shared by every species
 *        track on the watershed surface.
 *
 * @details A3 and H5a each read `ctx.subcatches.rainfall` as "how much water
 *          landed on this subarea this step". On a subcatchment with a
 *          SNOWPACK that is the wrong field, and wrong in two directions at
 *          once. This header is the one place that knows which field is
 *          right, so the two tracks cannot drift apart on it — the
 *          `LidLayerCommon` move, for the same reason.
 *
 * @see plans/transport/SNOW_MIXING_VOLUME_FINDING_2026-08-20.md
 * @ingroup engine_transport
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_TRANSPORT_WATERSHED_COMMON_HPP
#define OPENSWMM_ENGINE_TRANSPORT_WATERSHED_COMMON_HPP

#include <cstddef>

namespace openswmm {
struct SimulationContext;
}

namespace openswmm::transport {

/// Subarea ordering shared by `SubArea` (A3) and `HeatSubArea` (H5a). Both
/// enums are deliberately separate types; this is the ordinal they agree on.
constexpr int kSubIMPERV0 = 0;
constexpr int kSubIMPERV1 = 1;
constexpr int kSubPERV    = 2;

/**
 * @brief Net precipitation actually reaching one subarea this step [ft/s].
 *
 * @details **This is the mixing volume, and it is not always the rainfall.**
 *
 *          `ctx.subcatches.rainfall` is set once, at `Runoff.cpp:294-295`,
 *          to `rain + SNOWFALL`, and never updated. On a subcatchment with a
 *          snowpack the runoff solver does not use it: it uses
 *          `snow_net_imperv` for IMPERV0/IMPERV1 and `snow_net_perv` for
 *          PERV (`Runoff.cpp:548-552`), each built as
 *          `imelt + rainfall·(1 − asc)` — **melt, plus the rain falling on
 *          the snow-free fraction** (`SWMMEngine.cpp:1608-1616`).
 *
 *          Reading the rainfall field on such a deck is wrong twice over:
 *          - **snowfall is counted as arriving liquid**, so water being
 *            stored in the pack is mixed into the surface as though it had
 *            landed, and
 *          - **snowmelt is not counted at all**, so the water that genuinely
 *            arrives is invisible to the mixing volume and the surface ages
 *            as if nothing arrived.
 *
 *          The second of those is A3's own net-gain failure (lessons 64, 68)
 *          reappearing through a different field: the mixing volume is again
 *          not the arriving volume. They do not cancel — they are displaced
 *          by the whole residence time of the pack, which is precisely the
 *          quantity a snow-aware age model exists to measure.
 *
 * @par The `-1.0` sentinel is the whole guard
 *      `snow_net_*` initialise to **-1.0** (`SubcatchData.hpp:642`), and the
 *      solver's own test is `>= 0.0` (`Runoff.cpp:551-552`). A subcatchment
 *      with a pack but no melt this step publishes 0.0, which is a real
 *      rate; only the negative sentinel means "no pack, use the gage". This
 *      function mirrors that test exactly rather than inventing its own.
 *
 * @par IGNORE_SNOWMELT
 *      Falls back to the gage value, matching `Runoff.cpp:548`'s
 *      `!ctx.options.ignore_snow_melt` and legacy `subcatch.c:784`.
 *
 * @param subarea One of `kSubIMPERV0` / `kSubIMPERV1` / `kSubPERV`.
 */
double arrivingPrecipRate(const SimulationContext& ctx, std::size_t ui,
                          int subarea) noexcept;

}  // namespace openswmm::transport

#endif  // OPENSWMM_ENGINE_TRANSPORT_WATERSHED_COMMON_HPP
