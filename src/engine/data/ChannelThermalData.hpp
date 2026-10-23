// SPDX-License-Identifier: Apache-2.0

/**
 * @file      ChannelThermalData.hpp
 * @brief     Thermal state and geometry associated with routed channels.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#pragma once
#include <string>
#include <vector>
namespace openswmm {
// Solver-neutral finite channel water. Region IDs belong to the actual
// transport engine; ARD regions are cells, LEGACY/LARD regions are links.
// Along-reach fractions map hydraulic discretization onto the full polyline.
struct ChannelThermalRegion {
    int link = -1, region = -1;
    double begin = 0, end = 1;
    double volume = 0, temperature = 0;                           // SI m3 and degC
    double length = 0, surface_area = 0, bed_area = 0, depth = 0; // SI m, m2, m2, m
};
struct ChannelThermalIncrement {
    int link = -1, region = -1;
    double celsius = 0;
};
// Finite channel region and signed concentration update for reciprocal 2D
// porewater contacts. Rows use the channel transport owner's pollutant, age,
// temperature ordering; temperature is carried separately by the heat API.
struct ChannelSpeciesRegion {
    int link = -1, region = -1;
    double begin = 0, end = 1, volume = 0;
    std::vector<std::string> row_names;
    std::vector<double> concentration;
};
struct ChannelSpeciesIncrement {
    int link = -1, region = -1;
    std::vector<double> concentration_delta;
};
} // namespace openswmm
