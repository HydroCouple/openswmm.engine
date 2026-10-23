// SPDX-License-Identifier: Apache-2.0

/**
 * @file      GroundHeat.hpp
 * @brief     Thermal state and exchange bookkeeping for fixed ground layers.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#pragma once
#include "PorousHeat.hpp"
#include "GroundPorewater.hpp"
#include <cstdint>
namespace openswmm {
struct SimulationContext;
}
namespace openswmm::surface {
class MeshSnow;
}
namespace openswmm::twoD {
struct MeshData;
struct GwTransportData;
struct SubsurfaceTransportState;
struct GroundThermalCell {
    PorousHeatColumn column;
    GroundPorewaterColumn porewater;
    std::vector<double> saturated_water;
    // Water already accepted out of this column but not yet applied to its
    // hydraulic moisture profile. Its sensible energy is debited at the
    // producing face; reserve its heat capacity from intervening contacts.
    std::vector<double> reserved_out_water;
    double initial_j = 0, advective_j = 0, source_j = 0, surface_j = 0, deep_j = 0, latent_j = 0,
           lateral_j = 0, water_contact_j = 0, channel_contact_j = 0, lid_contact_j = 0;
    double vertical_absolute_j = 0, geo_flux = .065;
    bool surface_owned = false;
    int failed = 0;
    std::string error;
};
struct GroundLidThermalOwner {
    int node = -1, row = -1;
    double area = 0;
};
struct GroundHeatState {
    SimulationContext* context = nullptr;
    surface::MeshSnow* snow_owner = nullptr; // borrowed mesh snow state for reciprocal UEB contact
    const GwTransportData* config = nullptr;
    std::vector<GroundThermalCell> cells;
    // Resolved Mesh Richards LID footprints by soil cell. Derived from the
    // hydraulic bed bindings at initialization; energy itself lives in the
    // persistent LidNodeState and is checkpointed with the 1D owner.
    std::vector<std::vector<GroundLidThermalOwner>> lid_owners;
    double top_now = 0, deep_now = 0;
    double density = 1000, water_capacity = 4184000;
    int top_series = -1, deep_series = -1;
    /**
     * @brief Test whether the owner has allocated any thermal cell slots.
     * @return True when cells is nonempty, including a mesh-sized array containing no active
     * columns. Does not inspect individual capacity, configuration or borrowed context validity.
     * No mutation.
     */
    bool active() const noexcept { return !cells.empty(); }
    /**
     * @brief Reset thermal cells, diagnostics, configuration and borrowed bindings to defaults.
     * @note Releases owned vectors and invalidates element views. Does not mutate external
     * context, mesh snow, hydraulic or transport owners. Caller serializes with all
     * column/contact consumers.
     */
    void clear() { *this = {}; }
    /**
     * - cell: Valid prepared cell index.
     * - layer: Valid physical layer index.
     * @return Remaining J/K after reserved outgoing water capacity, without positivity clamp or
     * mutation.
     */
    double activeCapacity(int cell, std::size_t layer) const noexcept;
    /**
     * - cell: Valid prepared cell index.
     * - layer: Valid physical layer index with positive finite active capacity.
     * @return Layer energy/active capacity, degC; unchecked and read-only.
     */
    double activeTemperature(int cell, std::size_t layer) const noexcept;
    /**
     * @brief Test reciprocal UEB snow/soil thermal contact availability.
     * @param[in] cell Cell index, called i in source; invalid returns false.
     * @return True for a nonempty soil column, live snow owner/config, enabled conduction and
     * matching snow-owner contact. Does not infer snow cover from soil alone or book energy;
     * dependencies must not throw under this noexcept query.
     */
    bool snowContactEnabled(int cell) const noexcept;
    /**
     * @brief Read the active snow-contact footprint fraction.
     * @param[in] cell Mesh cell index; invalid/disabled contact returns zero.
     * @return Snow-owner fraction clamped to [0,1] when enabled. No area, water or energy
     * receipt is changed; borrowed snow/config owners must remain stable and noexcept
     * dependencies must not throw.
     */
    double snowContactFraction(int cell) const noexcept;
    /**
     * - cell: Prepared snow/soil mesh cell.
     * - area: Finite positive contact area m2.
     * @return True after appending one local top-layer boundary to the borrowed vector argument,
     * false for disabled contact/invalid area. Invalid prepared resistance/temperature throws;
     * no energy booked.
     */
    bool appendSnowContact(int cell, double area, std::vector<ThermalBoundary>&) const;
    /**
     * @brief Publish reciprocal energy to the active snow owner after a soil contact solve.
     * @param[in] cell Matching prepared soil/snow cell.
     * @param[in] soil_j Signed energy J added to soil; the snow owner applies the matching
     * reciprocal receipt. Caller supplies a finite accepted solve result and serializes owner
     * mutation.
     * @return Empty on snow-owner acceptance, otherwise its diagnostic or a missing-ownership
     * diagnostic. Does not itself update the soil column energy; caller pairs both sides and
     * handles refusal.
     */
    std::string applySnowContactEnergy(int cell, double soil_j);
    std::string initialize(SimulationContext&, const MeshData&, const SubsurfaceState&,
                           const SubsurfaceTransportState&, const GwTransportData&,
                           const std::vector<uint8_t>& coverage);
    std::string initializeStandalone(SimulationContext&, const MeshData&, const GwTransportData&,
                                     const std::vector<unsigned char>& owned);
    /**
     * @brief Sample top and deep thermal forcing at context.current_date.
     * @throws std::runtime_error Missing temperature coverage or nonfinite sampled forcing.
     * @note Inactive returns without work. Writes top_now/deep_now and may update shared
     * time-series cursors; scalar parse exceptions propagate. Requires live context/config and
     * serial cursor access. A later boundary failure can leave top_now updated; no energy or
     * calendar advance occurs.
     */
    void prepare();
    // RK2 forcing reads project series without changing their shared cursors
    // or the engine calendar. Cuts apply to the entire coupled transaction.
    /**
     * - elapsed_seconds: Finite coupled elapsed time s from options.start_date; source calls
     * this time. Updates cached top/deep forcing only, with cursor-free series interpolation.
     * @throws std::runtime_error Invalid/uncovered temperature forcing; partially updated caches
     * may remain. Requires live prepared context/config and exclusive access to forcing cache.
     */
    void prepareAt(double elapsed_seconds);
    /**
     * - start: Finite elapsed interval start s.
     * - end: Finite elapsed end s, >= start.
     * @note Appends interior top/deep forcing times to the borrowed vector argument; retains
     * existing entries and can throw after partial append. Caller sorts/deduplicates and stages
     * its transaction. Temperature series must cover the full interval; flux series may have
     * finite support.
     */
    void appendRk2Cuts(double start, double end, std::vector<double>&) const;
    /**
     * @brief Publish canonical physical heat/porewater inventories into the derived groundwater
     * view.
     * - cell: Valid prepared cell. The transport-reference argument is a writable, matching
     * species layout; saturated/unsaturated/layer temperature-volume is degC*m3.
     * @note Inactive/no-temperature returns. Invalid porewater mapping throws and may leave
     * partial derived updates; physical energy/mass is unchanged. Requires exclusive
     * derived-view access.
     */
    void refreshView(int cell, SubsurfaceTransportState&);
    /**
     * - cell: Valid prepared cell, called i in the definition.
     * - saturated: True for saturated water, false for unsaturated water.
     * @return Physical-water-volume-weighted degC; empty zone falls back to its adjacent
     * bottom/top layer, empty column to zero. Requires consistent positive active capacities;
     * does not mutate state.
     */
    double zoneTemperature(int cell, bool saturated) const;
    // Append physical top/deep boundaries, returning the number of top
    // records. Exposed fraction excludes the finite water contact footprint.
    /**
     * - cell: Valid prepared column index.
     * - exposed_fraction: Top forcing area fraction in [0,1], unchecked.
     * @return Output vector size after top records and before appended deep records; preexisting
     * entries are included. Supply an empty vector to interpret this as a count. Disabled/empty
     * returns zero.
     * @note Appends to the borrowed boundary vector using cached forcing; no energy mutation
     * occurs.
     */
    std::size_t boundaries(int cell, double exposed_fraction, std::vector<ThermalBoundary>&) const;
    // Called after accepted water advection. A surface-owned column advances
    // its conduction with its finite water contact instead, never twice.
    /**
     * - cell: Valid prepared physical column.
     * - seconds: Finite positive solve duration s, called dt in source.
     * - midpoint: True selects the RK2 thermal contact solver.
     * @return Empty on success/skip, otherwise diagnostic. Allocates and may throw on boundary
     * setup; snow refusal may leave cumulative receipts changed. Surface-owned columns are
     * skipped to avoid advancing the same physical owner twice.
     */
    std::string conduct(int cell, double seconds, bool midpoint = false);
};
} // namespace openswmm::twoD
