// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "PorousHeat.hpp"
#include "../../data/HeatData.hpp"
#include "../../data/LidNodeData.hpp"
#include <cstddef>
#include <string>
#include <vector>

namespace openswmm::twoD {
// Experimental finite-volume thermal graph for the custom [LID_NODES]
// storage layers. It is intentionally not attached to a runtime owner yet:
// mesh-LID heat remains refused until accepted water, thermal, and restart
// receipts are integrated together.
struct LidNodeThermalGraph {
    std::vector<std::size_t> cells; // compact thermal-store index -> LID cell
    std::vector<ThermalStore> stores;
    std::vector<ThermalContact> contacts;
};

// Build capacities and vertical series-resistance contacts from one accepted
// LID water state. mobile_water_ft3 supplies the separate surface/mobile row;
// water_heat_capacity is volumetric J/m3/K. An absent energy vector is seeded
// at initial_celsius; a present vector is retained exactly.
std::string buildLidNodeThermalGraph(const LidNodeState&, double mobile_water_ft3,
    double water_heat_capacity, const ConductionConfig&, double initial_celsius,
    LidNodeThermalGraph&);

// Publish a successful graph solution back to its original physical-cell
// layout. Zero-capacity cells carry exactly zero stored energy.
std::string publishLidNodeThermalEnergy(LidNodeState&, const LidNodeThermalGraph&,
    const std::vector<double>& energy);
} // namespace openswmm::twoD
