// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "PorousHeat.hpp"
#include "../../data/HeatData.hpp"
#include "../../data/LidNodeData.hpp"
#include <cstddef>
#include <string>
#include <vector>

namespace openswmm::twoD {
// Finite-volume thermal graph for the custom [LID_NODES] storage layers.
// Runtime owners advance it with accepted water/heat receipts and persist its
// energy through the hydraulic and thermal checkpoints.
struct LidNodeThermalGraph {
    std::vector<std::size_t> cells; // compact thermal-store index -> LID cell
    std::vector<ThermalStore> stores;
    std::vector<ThermalContact> contacts;
    std::vector<double> physical_thickness_m,physical_area_m2,physical_conductivity;
    // Water and matrix geometry are kept in physical-cell layout so accepted
    // packets can update donor capacity and temperature in producer order.
    std::vector<double> physical_water_m3,physical_water_limit_m3;
    std::vector<double> physical_matrix_bulk_m3,physical_matrix_capacity_density;
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

// Build the bottom LID-to-soil contact for a combined graph whose ground
// store has already been appended. Contact area is the actual mapped overlap,
// bounded by both owners; the two half-layer resistances remain distinct.
// interface_resistance_m2_k_w is an additional nonnegative interface term.
std::string buildLidNodeGroundThermalContact(const LidNodeThermalGraph&,
    std::size_t combined_store_count,int ground_store,double overlap_area_m2,double ground_area_m2,
    double ground_layer_thickness_m,double ground_conductivity,
    double interface_resistance_m2_k_w,ThermalContact&);

// Publish a successful graph solution back to its original physical-cell
// layout. Zero-capacity cells carry exactly zero stored energy.
std::string publishLidNodeThermalEnergy(LidNodeState&, const LidNodeThermalGraph&,
    const std::vector<double>& energy);

// Convert accepted water receipts to sensible energy at producer temperature.
// Internal donor temperature and capacity are updated after every ordered
// packet, so split transfers use the state at their accepted time. external_j
// is signed positive into the LID owner. This is a pure transaction: outputs
// are replaced only after every packet validates.
std::string transferLidNodeThermalEnergy(const LidNodeThermalGraph&,
    std::size_t physical_cell_count,double water_heat_capacity,
    const std::vector<LidThermalWaterTransfer>&,
    std::vector<double>& physical_cell_energy,double& external_j);
} // namespace openswmm::twoD
