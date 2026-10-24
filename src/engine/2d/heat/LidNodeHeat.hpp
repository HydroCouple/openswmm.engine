// SPDX-License-Identifier: Apache-2.0

/**
 * @file      LidNodeHeat.hpp
 * @brief     Finite-volume thermal graphs for LID node storage layers.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

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
    std::vector<double> physical_thickness_m, physical_area_m2, physical_conductivity;
    // Water and matrix geometry are kept in physical-cell layout so accepted
    // packets can update donor capacity and temperature in producer order.
    std::vector<double> physical_water_m3, physical_water_limit_m3;
    std::vector<double> physical_matrix_bulk_m3, physical_matrix_capacity_density;
};

// Accepted hydraulic transfer. Cell indices are physical top-to-bottom LID
// cells; -1 denotes the external water owner. External inflows use the
// producer's Celsius temperature. Internal transfers always use the donor
// cell's pre-transfer temperature, so split packets cannot change donors.
struct LidThermalWaterTransfer {
    int from_cell = -1, to_cell = -1;
    double volume_m3 = 0;
    double source_temperature_c = 0;
};

// Build capacities and vertical series-resistance contacts from one accepted
// LID water state. mobile_water_ft3 supplies the separate surface/mobile row;
// water_heat_capacity is volumetric J/m3/K. An absent energy vector is seeded
// at initial_celsius; a present vector is retained exactly.
/**
 * @brief Build an SI finite thermal graph from a borrowed accepted physical LID state.
 * - mobile_water_ft3: Nonnegative native surface/mobile water ft3, bounded by its cell.
 * - water_heat_capacity: Finite positive volumetric water capacity J/(m3 K).
 * - initial_celsius: Finite initial temperature degC, used if no persisted physical energy.
 * @return Empty on success, otherwise validation text; output graph argument is replaced only on
 * success. Physical geometry/material and persistent-energy requirements are detailed in the
 * definition. State is read only; temporary allocations may throw.
 */
std::string buildLidNodeThermalGraph(const LidNodeState&, double mobile_water_ft3,
                                     double water_heat_capacity, const ConductionConfig&,
                                     double initial_celsius, LidNodeThermalGraph&);

// Build the bottom LID-to-soil contact for a combined graph whose ground
// store has already been appended. Contact area is the actual mapped overlap,
// bounded by both owners; the two half-layer resistances remain distinct.
// interface_resistance_m2_k_w is an additional nonnegative interface term.
/**
 * @brief Form the bottom-LID to appended-ground finite conductive contact.
 * - combined_store_count: Total store count, larger than LID stores and containing ground_store.
 * - ground_store: Valid appended ground store index, outside the LID store range.
 * - overlap_area_m2: Finite positive overlap m2, bounded by both physical owner footprints.
 * - ground_area_m2: Finite positive ground footprint m2.
 * - ground_layer_thickness_m: Finite positive receiving layer thickness m.
 * - ground_conductivity: Finite positive ground conductivity W/(m K).
 * - interface_resistance_m2_k_w: Finite nonnegative additional areal resistance m2 K/W.
 * @return Empty after replacing the output contact argument, otherwise validation text with
 * output unchanged. Borrowed graph must end at its physical bottom layer and have consistent SI
 * arrays.
 */
std::string buildLidNodeGroundThermalContact(const LidNodeThermalGraph&,
                                             std::size_t combined_store_count, int ground_store,
                                             double overlap_area_m2, double ground_area_m2,
                                             double ground_layer_thickness_m,
                                             double ground_conductivity,
                                             double interface_resistance_m2_k_w, ThermalContact&);

// Publish a successful graph solution back to its original physical-cell
// layout. Zero-capacity cells carry exactly zero stored energy.
/**
 * @brief Publish a compact thermal solve into its borrowed LID owner's physical energy layout.
 * - energy: Finite signed J per compact store, in exact graph order.
 * @return Empty after replacing owner thermal_energy, otherwise diagnostic with owner unchanged.
 * Omitted physical cells receive zero energy. Allocates; no water or contact receipts are
 * changed.
 */
std::string publishLidNodeThermalEnergy(LidNodeState&, const LidNodeThermalGraph&,
                                        const std::vector<double>& energy);

// Convert accepted water receipts to sensible energy at producer temperature.
// Internal donor temperature and capacity are updated after every ordered
// packet, so split transfers use the state at their accepted time. external_j
// is signed positive into the LID owner. This is a pure transaction: outputs
// are replaced only after every packet validates.
/**
 * @brief Convert accepted ordered water packets into a staged physical-layer energy result.
 * - physical_cell_count: Physical owner cell count matching graph arrays.
 * - water_heat_capacity: Finite positive volumetric water capacity J/(m3 K).
 * - physical_cell_energy: Replaced with signed J per physical cell on success.
 * - external_j: Replaced with net external J into the LID, rather than accumulated.
 * - transfer_j: Optional per-packet signed carried J vector, replaced on success.
 * @return Empty on success; otherwise descriptive refusal with all outputs unchanged. Requires
 * prepared graph and valid ordered accepted transfers as detailed in the definition. Allocates
 * and may throw; does not modify the graph, water state or canonical LID energy itself.
 */
std::string transferLidNodeThermalEnergy(
    const LidNodeThermalGraph&, std::size_t physical_cell_count, double water_heat_capacity,
    const std::vector<LidThermalWaterTransfer>&, std::vector<double>& physical_cell_energy,
    double& external_j, std::vector<double>* transfer_j = nullptr);
} // namespace openswmm::twoD
