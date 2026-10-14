// SPDX-License-Identifier: Apache-2.0
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
