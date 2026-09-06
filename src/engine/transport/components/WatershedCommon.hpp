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

/**
 * @brief Fraction of the arriving water that is MELTWATER, in [0, 1].
 *
 * @details Under a pack the arriving water is two different waters:
 *          meltwater, and rain that reached the ground through the snow-free
 *          fraction. `arrivingPrecipRate` returns their sum, and a sum
 *          cannot say what either is worth — this is the split.
 *
 *          Returns **0** wherever there is no pack, `IGNORE_SNOWMELT` is on,
 *          or nothing arrived, so a caller can blend unconditionally and a
 *          bare deck is bit-identical to its pre-S2 behaviour.
 */
double arrivingMeltFraction(const SimulationContext& ctx, std::size_t ui,
                            int subarea) noexcept;

/**
 * @brief Temperature of the water arriving at one subarea [°C] (S2).
 *
 * @details **Meltwater is at 0 °C essentially by definition** — that is what
 *          melting means — so the arriving temperature is the melt fraction
 *          blended against the configured `HeatSource::RAINFALL` value:
 *
 *          `T = (1 − f)·T_rain + f·0`
 *
 *          On a winter deck this is not a small approximation to make: it is
 *          the difference between a stream fed by snowmelt and one fed by
 *          rain. Unlike the age half, it needs **no pack state at all** —
 *          the freezing point is not something the pack has to remember.
 */
double arrivingPrecipTemperature(const SimulationContext& ctx, std::size_t ui,
                                 int subarea) noexcept;

/**
 * @brief Water age of the water arriving at one subarea [seconds] (S2b).
 *
 * @details The age analogue of `arrivingPrecipTemperature`, and **it uses
 *          the same `arrivingMeltFraction`** — not an equivalent expression,
 *          the same call. Under a pack the arriving water is meltwater
 *          carrying the pack's residence time plus rain that reached the
 *          ground through the snow-free fraction carrying the configured
 *          `WaterAgeSource::RAINFALL` age:
 *
 *          `a = (1 − f)·a_rain + f·a_pack`
 *
 *          If the two tracks ever computed `f` separately they could drift,
 *          and the drift would be invisible: both answers stay inside their
 *          brackets, and only a deck comparing arriving age against arriving
 *          temperature could see it. One call is what makes that
 *          unrepresentable.
 *
 * @par Where a_pack comes from
 *      `snow_melt_age_*`, published beside `snow_melt_*` under the identical
 *      area blend. **It is NOT the age of the water still in the pack** — a
 *      pack that empties this step publishes the age its water HAD, which
 *      the remaining-water age cannot express because there is none.
 *
 * @par The no-pack answer
 *      Returns the configured RAINFALL age wherever there is no pack,
 *      `IGNORE_SNOWMELT` is on, nothing arrived, or the pack published no
 *      melt — so a caller blends unconditionally and a bare deck is
 *      bit-identical to its pre-S2b behaviour.
 */
double arrivingPrecipAge(const SimulationContext& ctx, std::size_t ui,
                         int subarea) noexcept;

/// The temperature meltwater leaves a pack at, °C. Zero, and named rather
/// than written as a literal so a gate can assert against the constant the
/// code uses instead of against a number a reader hopes it uses.
inline constexpr double kMeltwaterTempC = 0.0;

}  // namespace openswmm::transport

#endif  // OPENSWMM_ENGINE_TRANSPORT_WATERSHED_COMMON_HPP
