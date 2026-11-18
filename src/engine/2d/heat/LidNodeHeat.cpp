// SPDX-License-Identifier: Apache-2.0

/**
 * @file      LidNodeHeat.cpp
 * @brief     Finite-volume thermal graphs for LID node storage layers.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include "LidNodeHeat.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace openswmm::twoD {
namespace {
constexpr double ft_to_m = .3048;
constexpr double ft2_to_m2 = .09290304;
constexpr double ft3_to_m3 = .028316846592;
/**
 * @brief Test a strictly positive finite thermal scalar.
 * @param[in] x Scalar in the caller's documented units.
 * @return x > 0 and std::isfinite(x); zero, negative values, infinity and NaN return false.
 */
bool positive(double x) {
    return x > 0 && std::isfinite(x);
}
/**
 * @brief Test a finite nonnegative thermal scalar.
 * @param[in] x Scalar in the caller's documented units.
 * @return x >= 0 and std::isfinite(x); infinity, NaN and negative values return false.
 */
bool nonnegative(double x) {
    return x >= 0 && std::isfinite(x);
}
}
/**
 * @brief Build compact active thermal stores and vertical contacts from accepted LID water
 * state.
 * @param[in] state Borrowed physical LID layers in top-to-bottom contiguous native ft/ft2/ft3
 * geometry. Richards water is native ft3 per physical cell; other subsurface water is
 * theta*geometric_volume. Existing thermal_energy and optional ground receipts must match
 * physical cells and be finite.
 * @param[in] mobile_water_ft3 Finite nonnegative water in the separate surface/mobile physical
 * cell, ft3; must not exceed its native geometric volume within tolerance.
 * @param[in] water_heat_capacity Finite positive volumetric water capacity J/(m3 K).
 * @param[in] material Finite positive water/sediment conductivity W/(m K), sediment density
 * kg/m3 and specific heat J/(kg K). Nonwater bulk supplies the sediment capacity/conductivity
 * mixture.
 * @param[in] initial_celsius Finite initial degC used only when state.thermal_energy is empty.
 * @param[out] output Replaced on success by SI physical arrays, positive-capacity compact stores
 * with J/K and J, and adjacent active physical-layer conductances W/K. Zero-capacity cells are
 * omitted; no contact bridges an omitted physical layer.
 * @return Empty on success; descriptive shape, geometry, water, material, energy or resistance
 * error.
 * @note Stages all output locally, preserving output/state on reported failure. Allocations can
 * throw. Existing layer energy is retained exactly; nonzero energy in a zero-capacity cell is
 * rejected. Does not book heat/water or modify borrowed hydraulic/thermal owners.
 */
std::string buildLidNodeThermalGraph(const LidNodeState& state, double mobile_water_ft3,
                                     double water_heat_capacity, const ConductionConfig& material,
                                     double initial_celsius, LidNodeThermalGraph& output) {
    if (state.cells.empty() || !nonnegative(mobile_water_ft3) || !positive(water_heat_capacity) ||
        !positive(material.water_conductivity) || !positive(material.sed_conductivity) ||
        !positive(material.sed_density) || !positive(material.sed_specific_heat) ||
        !std::isfinite(initial_celsius))
        return "Invalid LID thermal geometry, phase properties or initial condition.";
    if (!state.richards && std::any_of(state.cells.begin(), state.cells.end(),
                                       [](const auto& c) { return !std::isfinite(c.theta); }))
        return "Nonfinite LID thermal moisture.";
    if (state.richards && state.richards_water.size() != state.cells.size())
        return "Richards LID thermal water shape differs from its physical layers.";
    if (state.richards && std::any_of(state.richards_water.begin(), state.richards_water.end(),
                                      [](double v) { return !nonnegative(v); }))
        return "Nonfinite or negative Richards LID thermal water.";
    if (!state.thermal_energy.empty() && state.thermal_energy.size() != state.cells.size())
        return "LID thermal energy shape differs from its physical layers.";
    if (!state.thermal_ground_contact_j.empty() &&
        state.thermal_ground_contact_j.size() != state.cells.size())
        return "LID ground-contact receipt shape differs from its physical layers.";
    if (std::any_of(state.thermal_ground_contact_j.begin(), state.thermal_ground_contact_j.end(),
                    [](double value) { return !std::isfinite(value); }))
        return "LID ground-contact receipt contains a nonfinite value.";

    LidNodeThermalGraph next;
    std::vector<double> thickness, area, conductivity;
    thickness.reserve(state.cells.size());
    area.reserve(state.cells.size());
    conductivity.reserve(state.cells.size());
    next.physical_thickness_m.resize(state.cells.size());
    next.physical_area_m2.resize(state.cells.size());
    next.physical_conductivity.resize(state.cells.size());
    next.physical_water_m3.resize(state.cells.size());
    next.physical_water_limit_m3.resize(state.cells.size());
    next.physical_matrix_bulk_m3.resize(state.cells.size());
    next.physical_matrix_capacity_density.resize(state.cells.size());
    for (std::size_t i = 0; i < state.cells.size(); ++i) {
        const auto& cell = state.cells[i];
        if (!std::isfinite(cell.top) || !std::isfinite(cell.bottom) || !(cell.top > cell.bottom) ||
            !positive(cell.area) || !positive(cell.geometric_volume) ||
            (cell.kind != LidNodeLayerKind::Surface &&
             (!std::isfinite(cell.porosity) || cell.porosity < 0 || cell.porosity > 1)))
            return "Invalid LID thermal cell geometry or porosity.";
        if (i > 0) {
            const double scale =
                std::max({1., std::abs(cell.top), std::abs(state.cells[i - 1].bottom)});
            if (std::abs(cell.top - state.cells[i - 1].bottom) > 1e-10 * scale)
                return "LID thermal cells must be ordered, contiguous physical layers.";
        }
        const double dz = (cell.top - cell.bottom) * ft_to_m;
        const double a = cell.area * ft2_to_m2;
        const double bulk = cell.geometric_volume * ft3_to_m3;
        double water_ft3 = 0;
        if (cell.kind == LidNodeLayerKind::Surface) {
            if (i != 0)
                return "LID thermal surface storage must be the first physical cell.";
            water_ft3 = mobile_water_ft3;
        } else if (state.richards)
            water_ft3 = state.richards_water[i];
        else
            water_ft3 = cell.theta * cell.geometric_volume;
        if (!nonnegative(water_ft3) || water_ft3 > cell.geometric_volume * (1 + 1e-10))
            return "LID thermal water exceeds its physical cell volume.";
        const double water = std::min(water_ft3, cell.geometric_volume) * ft3_to_m3;
        // Match the existing LID conduction mixture: water and the remaining
        // bulk layer share heat capacity and conductivity in proportion.
        const double solid =
            cell.kind == LidNodeLayerKind::Surface ? 0 : std::max(0., bulk - water);
        const double matrix_capacity_density =
            cell.kind == LidNodeLayerKind::Surface
                ? 0
                : material.sed_density * material.sed_specific_heat;
        const double capacity = water * water_heat_capacity + solid * matrix_capacity_density;
        const double saturation =
            cell.kind == LidNodeLayerKind::Surface ? 1 : std::clamp(water / bulk, 0., 1.);
        const double lambda =
            saturation * material.water_conductivity + (1 - saturation) * material.sed_conductivity;
        if (!std::isfinite(capacity) || capacity < 0 || !positive(lambda))
            return "Invalid LID thermal layer capacity or conductivity.";
        next.physical_thickness_m[i] = dz;
        next.physical_area_m2[i] = a;
        next.physical_conductivity[i] = lambda;
        next.physical_water_m3[i] = water;
        next.physical_water_limit_m3[i] = bulk;
        next.physical_matrix_bulk_m3[i] = cell.kind == LidNodeLayerKind::Surface ? 0 : bulk;
        next.physical_matrix_capacity_density[i] = matrix_capacity_density;
        thickness.push_back(dz);
        area.push_back(a);
        conductivity.push_back(lambda);
        if (capacity > 0) {
            const double energy =
                state.thermal_energy.empty() ? capacity * initial_celsius : state.thermal_energy[i];
            if (!std::isfinite(energy))
                return "Nonfinite LID thermal layer energy.";
            next.cells.push_back(i);
            next.stores.push_back({capacity, energy});
        } else if (!state.thermal_energy.empty() && state.thermal_energy[i] != 0) {
            // A water withdrawal must carry its producer-time sensible heat;
            // dropping a now-dry surface store here would silently lose it.
            return "LID thermal energy remains in a cell with no thermal capacity.";
        }
    }
    for (std::size_t j = 1; j < next.cells.size(); ++j) {
        const std::size_t left = next.cells[j - 1], right = next.cells[j];
        if (right != left + 1)
            continue; // no conductive path across an absent physical layer
        // Cell areas are secant areas from the native storage curve. Linear
        // interpolation between adjacent cell centres gives the interface.
        const double face_area = (area[left] * thickness[right] + area[right] * thickness[left]) /
                                 (thickness[left] + thickness[right]);
        const double resistance =
            .5 * thickness[left] / conductivity[left] + .5 * thickness[right] / conductivity[right];
        const double conductance = face_area / resistance;
        if (!positive(face_area) || !positive(resistance) || !positive(conductance))
            return "Invalid LID thermal contact resistance.";
        next.contacts.push_back({int(j - 1), int(j), conductance});
    }
    output = std::move(next);
    return {};
}
/**
 * @brief Connect the compact bottom LID store to an appended physical ground store.
 * @param[in] graph Prepared consistent LID graph whose last active store is the bottom physical
 * cell.
 * @param[in] combined_store_count Total stores after the ground graph is appended; must exceed
 * the LID store count and fit the int indexing used by the solve.
 * @param[in] ground_store Valid appended ground index, at least graph.stores.size().
 * @param[in] overlap_area Finite positive shared footprint m2, no larger than either owner area
 * within relative 1e-12 tolerance (header: overlap_area_m2).
 * @param[in] ground_area Finite positive ground footprint m2 (header: ground_area_m2).
 * @param[in] soil_thickness Finite positive ground layer thickness m (header:
 * ground_layer_thickness_m).
 * @param[in] soil_conductivity Finite positive ground conductivity W/(m K) (header:
 * ground_conductivity).
 * @param[in] interface_resistance Finite nonnegative added areal resistance m2 K/W (header:
 * interface_resistance_m2_k_w).
 * @param[out] output Replaced on success by bottom LID/ground store indices and conductance W/K.
 * @return Empty on success, otherwise invalid shape, geometry, owner index or resistance
 * diagnostic.
 * @note Uses both half-layer resistances plus the interface term. Does not append stores or
 * mutate owners. Preserves output on refusal; graph numeric physical arrays must already be
 * valid.
 */
std::string buildLidNodeGroundThermalContact(const LidNodeThermalGraph& graph,
                                             std::size_t combined_store_count, int ground_store,
                                             double overlap_area, double ground_area,
                                             double soil_thickness, double soil_conductivity,
                                             double interface_resistance, ThermalContact& output) {
    const auto n = graph.physical_thickness_m.size();
    if (!n || graph.cells.empty() || graph.cells.size() != graph.stores.size() ||
        graph.physical_area_m2.size() != n || graph.physical_conductivity.size() != n ||
        graph.cells.back() != n - 1 || ground_store < int(graph.stores.size()) ||
        ground_store < 0 || std::size_t(ground_store) >= combined_store_count ||
        combined_store_count <= graph.stores.size() || !positive(overlap_area) ||
        !positive(ground_area) || overlap_area > ground_area * (1 + 1e-12) ||
        overlap_area > graph.physical_area_m2.back() * (1 + 1e-12) || !positive(soil_thickness) ||
        !positive(soil_conductivity) || !nonnegative(interface_resistance))
        return "Invalid LID-to-ground thermal contact geometry or owner index.";
    const double resistance =
        .5 * graph.physical_thickness_m.back() / graph.physical_conductivity.back() +
        interface_resistance + .5 * soil_thickness / soil_conductivity;
    const double conductance = overlap_area / resistance;
    if (!positive(resistance) || !positive(conductance))
        return "Invalid LID-to-ground thermal contact resistance.";
    output = {int(graph.stores.size() - 1), ground_store, conductance};
    return {};
}
/**
 * @brief Replace persisted physical-cell energy with a successful compact graph solution.
 * @param[in,out] state LID owner whose cell count determines the output shape; only
 * thermal_energy is replaced, with omitted zero-capacity physical cells receiving zero J.
 * @param[in] graph Prepared compact graph mapping strictly increasing unique physical cell
 * indices to stores; store/cell counts must match energy.
 * @param[in] energy Finite signed J per compact thermal store. Temperature below zero degC may
 * yield negative sensible energy; values are not required nonnegative.
 * @return Empty after swapping the staged physical energy, otherwise shape/index/order/finite
 * error.
 * @note Refusal preserves owner energy. Allocates a physical-cell vector and can throw; does not
 * publish water, capacity, ground-contact receipts or recompute hydraulic state.
 */
std::string publishLidNodeThermalEnergy(LidNodeState& state, const LidNodeThermalGraph& graph,
                                        const std::vector<double>& energy) {
    if (graph.cells.size() != graph.stores.size() || graph.cells.size() != energy.size())
        return "LID thermal solution shape differs from its owner graph.";
    std::vector<double> next(state.cells.size(), 0);
    for (std::size_t i = 0; i < graph.cells.size(); ++i) {
        const auto cell = graph.cells[i];
        if (cell >= next.size() || !std::isfinite(energy[i]))
            return "Invalid LID thermal solution energy.";
        if (i > 0 && graph.cells[i] <= graph.cells[i - 1])
            return "LID thermal graph cells are duplicated or out of order.";
        next[cell] = energy[i];
    }
    state.thermal_energy.swap(next);
    return {};
}
/**
 * @brief Stage producer-ordered sensible energy transfers through a fixed physical LID graph.
 * @param[in] graph Prepared beginning-state stores plus complete SI physical water/matrix
 * arrays. Stored capacities must match the derived beginning capacities; active stores must not
 * be omitted.
 * @param[in] cell_count Physical LID cell count (header: physical_cell_count), matching every
 * graph physical-array extent. Compact cell indices must be strictly increasing and valid.
 * @param[in] water_heat_capacity Finite positive volumetric water capacity J/(m3 K).
 * @param[in] transfers Ordered accepted water packets: nonnegative finite m3, physical cell
 * indices or -1 for the external owner. A packet must change owner and have at least one
 * physical endpoint. External inflows use source_temperature_c; internal donors use updated
 * pre-packet energy/capacity.
 * @param[out] physical_energy Replaced by signed J per physical cell only after every packet
 * validates (header: physical_cell_energy); incoming contents are not used.
 * @param[out] external_j Replaced by net J added to the LID from external transfers, signed
 * positive inward. Incoming scalar is not accumulated.
 * @param[out] transfer_j Optional vector replaced by signed carried sensible J per packet in
 * original order, including zeros. The sign follows donor temperature, not transfer direction.
 * @return Empty on success; diagnostic for layout/capacity, donor availability, receiver
 * overflow, invalid packet, nonfinite energy or energy remaining without capacity.
 * @note Keeps graph/hydraulic owners unchanged and stages outputs until validation completes.
 * Tracks water/capacity after every packet, including matrix replacement by water. Outputs must
 * be distinct buffers and access serialized; temporary allocations can throw. Caller separately
 * publishes accepted water and returned physical energy to matching canonical owners.
 */
std::string transferLidNodeThermalEnergy(const LidNodeThermalGraph& graph, std::size_t cell_count,
                                         double water_heat_capacity,
                                         const std::vector<LidThermalWaterTransfer>& transfers,
                                         std::vector<double>& physical_energy, double& external_j,
                                         std::vector<double>* transfer_j) {
    if (graph.cells.size() != graph.stores.size() || !positive(water_heat_capacity) ||
        graph.physical_water_m3.size() != cell_count ||
        graph.physical_water_limit_m3.size() != cell_count ||
        graph.physical_matrix_bulk_m3.size() != cell_count ||
        graph.physical_matrix_capacity_density.size() != cell_count)
        return "Invalid LID thermal transfer graph or water heat capacity.";
    std::vector<std::size_t> store_of(cell_count, graph.cells.size());
    std::vector<double> next(cell_count, 0), water = graph.physical_water_m3;
    for (std::size_t i = 0; i < graph.cells.size(); ++i) {
        const auto cell = graph.cells[i];
        if (cell >= cell_count || (i > 0 && cell <= graph.cells[i - 1]) ||
            !positive(graph.stores[i].capacity) || !std::isfinite(graph.stores[i].energy))
            return "Invalid LID thermal transfer store layout.";
        store_of[cell] = i;
        next[cell] = graph.stores[i].energy;
    }
    const auto capacity = [&](std::size_t i) {
        return water[i] * water_heat_capacity +
               std::max(0.0, graph.physical_matrix_bulk_m3[i] - water[i]) *
                   graph.physical_matrix_capacity_density[i];
    };
    for (std::size_t i = 0; i < cell_count; ++i) {
        if (!nonnegative(water[i]) || !nonnegative(graph.physical_water_limit_m3[i]) ||
            water[i] > graph.physical_water_limit_m3[i] * (1 + 1e-10) ||
            !nonnegative(graph.physical_matrix_bulk_m3[i]) ||
            !nonnegative(graph.physical_matrix_capacity_density[i]) ||
            graph.physical_matrix_bulk_m3[i] > graph.physical_water_limit_m3[i] * (1 + 1e-12))
            return "Invalid LID thermal transfer water capacity layout.";
        const double initial_capacity = capacity(i);
        if (!nonnegative(initial_capacity))
            return "Invalid LID thermal transfer capacity.";
        if (store_of[i] < graph.cells.size()) {
            const double stored_capacity = graph.stores[store_of[i]].capacity;
            if (!(initial_capacity > 0) || !std::isfinite(stored_capacity) ||
                std::abs(initial_capacity - stored_capacity) >
                    1e-12 * std::max(1.0, initial_capacity))
                return "LID thermal transfer store capacity does not match its physical water state.";
        } else if (initial_capacity > 0)
            return "LID thermal transfer graph omits an active physical heat store.";
    }
    double external = 0;
    std::vector<double> packet_energy(transfers.size(), 0);
    for (std::size_t packet = 0; packet < transfers.size(); ++packet) {
        const auto& transfer = transfers[packet];
        if (!nonnegative(transfer.volume_m3) || transfer.from_cell < -1 || transfer.to_cell < -1 ||
            (transfer.from_cell < 0 && transfer.to_cell < 0) ||
            transfer.from_cell == transfer.to_cell ||
            (transfer.from_cell >= 0 &&
             static_cast<std::size_t>(transfer.from_cell) >= cell_count) ||
            (transfer.to_cell >= 0 && static_cast<std::size_t>(transfer.to_cell) >= cell_count))
            return "Invalid accepted LID thermal water transfer.";
        if (transfer.volume_m3 == 0)
            continue;
        double temperature = transfer.source_temperature_c;
        if (transfer.from_cell >= 0) {
            const auto from = static_cast<std::size_t>(transfer.from_cell);
            const double donor_capacity = from < cell_count ? capacity(from) : 0.0;
            const double water_tolerance =
                64 * std::numeric_limits<double>::epsilon() *
                std::max({1.0, from < cell_count ? water[from] : 0.0, transfer.volume_m3});
            if (from >= cell_count || store_of[from] >= graph.cells.size() ||
                !(donor_capacity > 0) || transfer.volume_m3 > water[from] + water_tolerance)
                return "LID thermal donor has no active heat capacity.";
            temperature = next[from] / donor_capacity;
        } else if (!std::isfinite(temperature))
            return "Nonfinite external LID water temperature.";
        const double joules = transfer.volume_m3 * water_heat_capacity * temperature;
        if (!std::isfinite(joules))
            return "LID thermal water receipt overflow.";
        packet_energy[packet] = joules;
        if (transfer.from_cell >= 0) {
            const auto from = static_cast<std::size_t>(transfer.from_cell);
            next[from] -= joules;
            water[from] -= transfer.volume_m3;
            if (water[from] < 0 && water[from] >= -64 * std::numeric_limits<double>::epsilon())
                water[from] = 0;
        } else
            external += joules;
        if (transfer.to_cell >= 0) {
            const auto to = static_cast<std::size_t>(transfer.to_cell);
            const double receiver_water = water[to] + transfer.volume_m3;
            const double water_tolerance =
                64 * std::numeric_limits<double>::epsilon() *
                std::max({1.0, water[to], transfer.volume_m3, graph.physical_water_limit_m3[to]});
            if (receiver_water > graph.physical_water_limit_m3[to] + water_tolerance)
                return "LID thermal receiver exceeds its physical water capacity.";
            water[to] = receiver_water;
            next[to] += joules;
        } else
            external -= joules;
        if (!std::isfinite(external))
            return "LID external thermal receipt overflow.";
        if ((transfer.from_cell >= 0 &&
             !std::isfinite(next[static_cast<std::size_t>(transfer.from_cell)])) ||
            (transfer.to_cell >= 0 &&
             !std::isfinite(next[static_cast<std::size_t>(transfer.to_cell)])))
            return "LID thermal transfer energy overflow.";
    }
    for (std::size_t i = 0; i < next.size(); ++i) {
        if (!std::isfinite(next[i]))
            return "LID thermal transfer energy overflow.";
        if (!(capacity(i) > 0) && next[i] != 0)
            return "LID thermal energy remains without water or matrix capacity.";
    }
    physical_energy.swap(next);
    external_j = external;
    if (transfer_j)
        transfer_j->swap(packet_energy);
    return {};
}
} // namespace openswmm::twoD
