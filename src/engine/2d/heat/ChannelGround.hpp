// SPDX-License-Identifier: Apache-2.0

/**
 * @file      ChannelGround.hpp
 * @brief     Coupled finite thermal solves for channels and shared ground columns.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#pragma once
#include "MeshHeat.hpp"
#include <cstdint>
#include <utility>
namespace openswmm::detail {
class CompletedSourceArchive;
}
namespace openswmm::twoD {
struct ChannelGroundContact {
    int link = -1, cell = -1, layer = -1;
    double begin = 0, end = 0, share = 1, invert = 0;
};
struct SurfaceThermalSample {
    double mass = 0, volume = 0, evaporated = 0;
};
// All routed water owners sharing a soil column join one finite solve. The
// router owns this state; the CPU marcher invokes it on its source clock.
class ChannelGroundState {
public:
    /**
     * @brief Release this contact mapping, borrowed bindings and diagnostic storage.
     * @note Resets the object to its default state, invalidating all element views. Does not
     * clear external mesh-contact flags, ground-cell surface_owned flags or mutate the borrowed
     * owners. Call serially after consumers finish; initialize() also clears the context
     * mesh-contact mask.
     */
    void clear() { *this = {}; }
    /**
     * @brief Test whether any prepared channel/ground contact exists.
     * @return True when contacts_ is nonempty. Does not validate the borrowed owners, current
     * channel wetness or enabled transport rows; no allocation or state change occurs.
     */
    bool active() const noexcept { return !contacts_.empty(); }
    /**
     * @brief Test the resolved thermal contact ownership mask for one cell.
     * @param[in] i Zero-based cell index; negative/out-of-range returns false.
     * @return Stored nonzero ownership flag. Does not rebuild geometry, infer current wetness or
     * validate external-owner lifetime. Read-only and allocation-free.
     */
    bool ownsCell(int i) const noexcept {
        return i >= 0 && std::size_t(i) < cell_owned_.size() && cell_owned_[i];
    }
    /**
     * @brief Test the resolved thermal contact ownership mask for one link.
     * @param[in] i Zero-based link index; negative/out-of-range returns false.
     * @return Stored nonzero ownership flag. Does not rebuild geometry, infer current wetness or
     * validate external-owner lifetime. Read-only and allocation-free.
     */
    bool ownsLink(int i) const noexcept {
        return i >= 0 && std::size_t(i) < link_owned_.size() && link_owned_[i];
    }
    /**
     * @brief Borrow the prepared unique owned cells in increasing mesh-index order.
     * @return Solver-owned const vector; valid during stable object access. clear(),
     * initialize() or replacement invalidates element views. Contact lengths/elevations use SI
     * geometry and normalized along-link fractions. No copy, reservation, mutation or ownership
     * transfer occurs.
     */
    const std::vector<int>& cells() const noexcept { return cells_; }
    /**
     * @brief Borrow the prepared contact segments sorted by link, begin fraction and cell.
     * @return Solver-owned const vector; valid during stable object access. clear(),
     * initialize() or replacement invalidates element views. Contact lengths/elevations use SI
     * geometry and normalized along-link fractions. No copy, reservation, mutation or ownership
     * transfer occurs.
     */
    const std::vector<ChannelGroundContact>& contacts() const noexcept { return contacts_; }
    /**
     * @brief Read the number of mapped contact pieces for a routed link.
     * @param[in] link Zero-based link; invalid/unowned returns zero.
     * @return Difference of the link's prepared contact range endpoints, including multiple
     * cells and layers. Requires matching prepared ranges; no geometry rebuild or wetness test
     * occurs.
     */
    std::size_t contactCount(int link) const noexcept {
        return ownsLink(link) ? contact_ranges_[link].second - contact_ranges_[link].first : 0;
    }
    // Capacity-weighted temperature of the actual soil beneath this reach.
    // This is a view of existing soil energy, including while the reach is dry.
    /**
     * @brief Read the capacity-weighted temperature of physical soil beneath a channel reach.
     * @param[in] link Zero-based owned routed-link index.
     * @param[in] begin Finite normalized start fraction along the complete link, in [0,1).
     * @param[in] end Finite normalized end fraction, begin < end <= 1.
     * @return Capacity-weighted temperature degC of existing soil energy beneath the requested
     * bed reach, including dry channels. Quiet NaN for invalid/unowned reach or no positive
     * active capacity.
     * @note Requires stable initialized context, mesh, ground and material bindings. Uses
     * overlap of the full bed thickness with physical layers, excluding reserved capacity
     * through activeCapacity(). No channel/soil energy, receipt or clock is changed; noexcept
     * dependencies must not throw.
     */
    double bedTemperature(int link, double begin = 0, double end = 1) const noexcept;
    /**
     * @brief Resolve channel geometry onto shared porous thermal columns and bind borrowed
     * owners.
     * - coordinate_to_si: Finite positive XY-to-m conversion; passed as coordinate_scale in the
     * definition. Context, mesh, ground, transport and surface arguments are borrowed as
     * described in the definition and must remain stable/alive through all subsequent advances.
     * @return Empty string for success/no selected contacts, otherwise a configuration
     * diagnostic.
     * @note Resets this mapping and the context mesh-contact mask first. Successful
     * initialization marks participating ground cells surface-owned; errors do not restore prior
     * mapping/flags.
     */
    std::string initialize(SimulationContext&, const MeshData&, GroundHeatState&,
                           SubsurfaceTransportState&, SurfaceStateData&, double coordinate_to_si);
    // Samples are already mixed, before intensive sinks. Returned surface
    // temperature-volume values are published by the caller at that boundary.
    /**
     * @brief Advance the shared finite channel/soil solve and publish surface
     * temperature-volume.
     * - seconds: Finite positive duration s, called dt in the definition.
     * - surface_mass: Replaced on successful active solve with mesh-cell degC*m3 values;
     * retained on inactive return or prepublication refusal.
     * @return Empty on success/inactive state, otherwise detailed failure text. Mixed sample
     * layout, borrowed owner lifetime, callback publication and partial-update rules are
     * documented on the definition. This operation allocates and requires exclusive owner
     * access.
     */
    std::string advance(const SolverOptions2D&, const std::vector<SurfaceThermalSample>&,
                        double seconds, std::vector<double>& surface_mass);
    /**
     * @brief Borrow cumulative signed atmospheric energy added to routed channel water.
     * @return Solver-owned const vector in J per link, initialized to zero on successful
     * binding. Values change on successful advances/checkpoint restore. clear()/initialize()
     * invalidates views; requires stable read access. No copy, reset or receipt consumption
     * occurs.
     */
    const std::vector<double>& channelAtmosphere() const noexcept { return channel_atmospheric_j_; }
    /**
     * @brief Borrow cumulative signed channel-to-ground contact energy.
     * @return Solver-owned const vector in J per link, positive into ground, initialized to zero
     * on successful binding. Values change on successful advances/checkpoint restore.
     * clear()/initialize() invalidates views; requires stable read access. No copy, reset or
     * receipt consumption occurs.
     */
    const std::vector<double>& channelToGround() const noexcept { return channel_ground_j_; }
    /**
     * @brief Borrow cumulative signed species exchange into physical porewater.
     * @return Solver-owned const vector in native inventory in species-major [row*n_links+link]
     * order, initialized to zero on successful binding. Values change on successful
     * advances/checkpoint restore. clear()/initialize() invalidates views; requires stable read
     * access. No copy, reset or receipt consumption occurs.
     */
    const std::vector<double>& channelSpeciesToGround() const noexcept {
        return channel_species_to_ground_;
    }
    /**
     * @brief Borrow cumulative net finite thermal energy change of routed water.
     * @return Solver-owned const vector in J per link, initialized to zero on successful
     * binding. Values change on successful advances/checkpoint restore. clear()/initialize()
     * invalidates views; requires stable read access. No copy, reset or receipt consumption
     * occurs.
     */
    const std::vector<double>& channelThermalIncrement() const noexcept {
        return channel_increment_j_;
    }
    /**
     * @brief Serialize the prepared ownership, contact geometry and transport row identity.
     * @return Owned binary archive bytes for masks, ordered cells/ranges/contact fields and row
     * names. Excludes mutable thermal/species receipts and external owner state; not a portable
     * text ID/hash.
     * @note Allocates copies/serialized bytes and can throw. Requires stable prepared mapping
     * and live transport binding when present; no state or clock mutation occurs.
     */
    std::string modelIdentity() const;
    /**
     * @brief Read/write mutable contact receipts through the completed-source archive.
     * @note The archive parameter is caller-owned and borrowed during the call. Restored vector
     * sizes must match the initialized link/species identity and all entries must be finite.
     * Restore can throw after vector mutation; caller stages the owning checkpoint transaction.
     */
    void checkpoint(openswmm::detail::CompletedSourceArchive&);

private:
    SimulationContext* context_ = nullptr;
    const MeshData* mesh_ = nullptr;
    GroundHeatState* ground_ = nullptr;
    SubsurfaceTransportState* transport_ = nullptr;
    SurfaceStateData* surface_ = nullptr;
    std::vector<ChannelGroundContact> contacts_;
    std::vector<uint8_t> cell_owned_, link_owned_;
    std::vector<int> cells_;
    std::vector<std::pair<std::size_t, std::size_t>> contact_ranges_;
    std::vector<double> channel_atmospheric_j_, channel_ground_j_, channel_increment_j_,
        channel_species_to_ground_;
};
} // namespace openswmm::twoD
