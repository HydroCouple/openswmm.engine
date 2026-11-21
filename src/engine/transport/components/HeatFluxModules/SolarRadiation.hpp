// SPDX-License-Identifier: Apache-2.0

/**
 * @file      SolarRadiation.hpp
 * @brief     Solar radiation inputs and calculations for thermal components.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#pragma once
#include "../../../hydrology/Radiation.hpp"
// Compatibility names: heat and surface hydrology use the same solar kernel.
namespace openswmm::transport::heat {
using climate::SolarPosition;
using climate::kSolarConstant;
using climate::kStdPressureMb;
using climate::solarPosition;
using climate::airMass;
using climate::pressureFromElevation;
using climate::birdClearSkyGHI;
using climate::cloudShortwaveFactor;
using climate::cloudLongwaveFactor;
using climate::updateSolarForcing;
}
