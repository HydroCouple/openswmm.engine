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

// Accepted hydraulic transfer. Cell indices are physical top-to-bottom LID
// cells; -1 denotes the external water owner. External inflows use the
// producer's Celsius temperature. Internal transfers always use the donor
// cell's pre-transfer temperature, so split packets cannot change donors.
struct LidThermalWaterTransfer {
    int from_cell=-1,to_cell=-1;
    double volume_m3=0;
    double source_temperature_c=0;
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

// Convert accepted water receipts to sensible energy at producer temperature.
// external_j is signed positive into the LID owner. This is a pure transaction:
// outputs are replaced only after every packet validates.
std::string transferLidNodeThermalEnergy(const LidNodeThermalGraph&,
    std::size_t physical_cell_count,double water_heat_capacity,
    const std::vector<LidThermalWaterTransfer>&,
    std::vector<double>& physical_cell_energy,double& external_j);
} // namespace openswmm::twoD
