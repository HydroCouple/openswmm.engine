// SPDX-License-Identifier: Apache-2.0

/**
 * @file      GroundPorewater.hpp
 * @brief     Species inventories in fixed physical soil layers.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#pragma once
#include "PorousHeat.hpp"
#include "SigmaPorewaterMap.hpp"
#include <utility>
namespace openswmm::twoD {
// Species-major canonical mass in the existing fixed physical soil layers.
// Water and geometry are borrowed from the thermal/hydraulic owner. Bulk
// SAT/UNSAT masses become derived views, never an additional reservoir.
struct GroundPorewaterColumn {
    int n_species = 0, age_row = -1, temp_row = -1;
    // `reserved_out` is a non-inventory overlay for accepted face packets
    // that have not yet reached this cell's gather. Finite contacts may use
    // only mass - reserved_out; the canonical mass still owns the queued
    // packet until its existing gather consumes it.
    std::vector<double> mass, retention, initial_mass, contact_mass, reserved_out, generated_age;
    SigmaPorewaterMap sigma;
    /**
     * @brief Publish pending SIGMA contact receipts and refresh the derived physical-layer
     * views.
     *
     * @par Declaration arguments
     * - unnamed transport owner (in,out): Canonical transport owner with matching SIGMA
     * species/layers and cell extent.
     * - cell (in): Target cell index in that owner.
     * @return Empty when inactive or successfully published/refreshed; otherwise
     * sigma.publish()'s diagnostic, without refreshing the derived views. The map handles
     * pending receipt validation.
     * @note This method borrows the owner only for the call; publication requires exclusive
     * access to both objects and must occur at the phase where pending contact receipts become
     * canonical.
     */
    std::string publishSigma(SubsurfaceTransportState&, int cell);
    /**
     * @brief Rebuild physical-layer mass and reservation views from an active SIGMA overlap map.
     * @note Does nothing without an active map. Otherwise zeros mass/reserved_out and gathers
     * every species/overlap into its physical layer. Prepared n_species, inventory sizes and
     * overlap indices must be consistent; no validation or rollback occurs. Contact/initial/age
     * ledgers are retained. Canonical SIGMA inventories and pending receipts are unchanged; no
     * allocation occurs.
     */
    void refreshSigmaView();
    /**
     * @brief Test whether at least one species row is configured.
     * @return n_species > 0; does not validate inventory shapes or SIGMA mapping state.
     */
    bool active() const noexcept { return n_species > 0; }
    /**
     * @brief Seed canonical physical-layer species inventories from saturated/unsaturated
     * totals.
     *
     * @par Declaration arguments
     * - unnamed soil column (in): Prepared thermal geometry, moisture and material; layer count
     * is theta.size().
     * - saturated_water (in): Finite nonnegative saturated water volume m3 per layer, no greater
     * than physical layer water except the 1e-12*max(1,water) roundoff allowance.
     * - saturated_mass (in): Nonempty species totals for the saturated zone in native inventory
     * units.
     * - unsaturated_mass (in): Same-size unsaturated species totals. Non-temperature rows must
     * be finite and nonnegative and nonzero inventory requires positive finite physical zone
     * capacity.
     * - kd (in): Finite nonnegative distribution coefficient m3/kg per species. Age ignores
     * retention; temperature rows are not represented in porewater mass and their totals are
     * ignored.
     * - age (in): Optional age row index, -1 when absent; otherwise within species count.
     * - temperature (in): Optional distinct temperature row index, -1 when absent.
     * @return Empty on success after replacing this object; diagnostic on validation refusal
     * with this unchanged. Initial masses are copied, contact/reservation/generated-age ledgers
     * start zero.
     * @note Species-major layout is row*layer_count+layer. Sorption capacity uses solid density
     * and solid volume; zone totals are distributed in proportion to water plus retained
     * capacity. The input soil arrays must have consistent extents. Allocation errors propagate
     * before publication.
     */
    std::string initialize(const PorousHeatColumn&, const std::vector<double>& saturated_water,
                           const std::vector<double>& saturated_mass,
                           const std::vector<double>& unsaturated_mass,
                           const std::vector<double>& kd, int age, int temperature);
    /**
     * @brief Derive saturated and unsaturated totals without creating another inventory.
     *
     * @par Declaration arguments
     * - unnamed soil column (in): Stable prepared physical layer geometry/moisture consistent
     * with this column.
     * - saturated_water (in): Saturated water volume m3 per physical layer.
     * @return Owned pair of species vectors (saturated,unsaturated), length n_species. The
     * temperature row contributes zero; mass is apportioned using water and retained capacity.
     * @throws std::invalid_argument On saturated, mass or retention shape mismatch.
     * @note Remaining soil geometry is assumed valid. Allocation failures propagate; no state
     * changes.
     */
    std::pair<std::vector<double>, std::vector<double>>
    zoneMasses(const PorousHeatColumn&, const std::vector<double>& saturated_water) const;
    /**
     * @brief Derive one species' saturated/unsaturated split from physical-layer mass.
     *
     * @par Declaration arguments
     * - unnamed soil column (in): Stable prepared geometry/moisture; all depth and layer arrays
     * must be valid.
     * - saturated_water (in): Saturated water volume m3 per layer, with soil.theta.size()
     * entries.
     * - species (in): Species row in [0,n_species); temperature/invalid rows return zeros
     * immediately.
     * @return Pair (saturated,unsaturated) in native species inventory units. Shares use water
     * plus retention and are clipped to [0,1], so the pair sums to canonical mass for the
     * species.
     * @note noexcept does not validate array extents; mass/retention must each have
     * n_species*layers entries. No allocation, mutation or receipt consumption occurs.
     */
    std::pair<double, double> zoneMass(const PorousHeatColumn&,
                                       const std::vector<double>& saturated_water,
                                       int species) const noexcept;
    /**
     * @brief Add elapsed water age to every physical layer and its generation ledger.
     *
     * @par Declaration arguments
     * - unnamed soil column (in): Prepared physical column, with stable moisture and geometry in
     * SI units.
     * - seconds (in): Finite nonnegative interval s; ignored when age_row is absent.
     * @return True after adding dt*water volume (m3 s) to age inventory and generated_age, or
     * true without mutation when no age row exists. False on invalid interval/shapes or finite
     * overflow, with no updates. Prepared age_row must be within n_species and soil geometry
     * consistent.
     */
    bool generateAge(const PorousHeatColumn&, double seconds) noexcept;
    /**
     * @brief Read unreserved physical-layer inventory available to a finite contact.
     *
     * @par Declaration arguments
     * - species (in): Species index; temperature rows are unsupported.
     * - layer (in): Physical layer index, derived from retention.size()/n_species.
     * @return mass-reserved_out in native inventory units. Returns NaN for invalid
     * indices/shapes, temperature rows, nonfinite or negative inventory/reservation, or
     * over-reservation.
     * @note Read-only, allocation-free and noexcept; canonical mass still includes queued
     * reservations.
     */
    double contactAvailableMass(int species, std::size_t layer) const noexcept;
    /**
     * @brief Preflight an outgoing reservation against unreserved canonical mass.
     *
     * @par Declaration arguments
     * - species (in): Non-temperature species index.
     * - layer (in): Physical layer index.
     * - amount (in): Finite nonnegative native inventory amount to reserve.
     * @return True when indices/shapes and available mass are valid, amount fits, and the
     * reservation sum remains finite. False leaves all state unchanged; no reservation is
     * created.
     */
    bool canReserveOutgoing(int species, std::size_t layer, double amount) const noexcept;
    /**
     * @brief Reserve accepted outgoing inventory without subtracting its canonical mass.
     *
     * @par Declaration arguments
     * - species (in): Non-temperature species index.
     * - layer (in): Physical layer index.
     * - amount (in): Finite nonnegative amount in native inventory units.
     * @return True after increasing reserved_out; false with no mutation when
     * canReserveOutgoing() refuses. The existing gather, not this overlay, eventually removes
     * canonical outgoing mass.
     */
    bool reserveOutgoing(int species, std::size_t layer, double amount) noexcept;
    /**
     * @brief Release a queued outgoing reservation after its owning gather has consumed the
     * packet.
     *
     * @par Declaration arguments
     * - species (in): Species index in [0,n_species).
     * - layer (in): Physical layer index for a consistently prepared inventory layout.
     * - amount (in): Finite nonnegative native amount, no greater than the existing reservation.
     * @return True after subtracting only reserved_out, setting exact complete settlement to
     * zero; false on invalid index/shape/value or insufficient reservation, with no mutation.
     * @note Does not subtract mass or update contact receipts; callers perform the matching
     * inventory gather separately. Prepared retention/mass extents must describe the same
     * physical-layer count.
     */
    bool settleOutgoingReservation(int species, std::size_t layer, double amount) noexcept;
    /**
     * @brief Preflight a signed contact receipt without spending reserved outgoing inventory.
     *
     * @par Declaration arguments
     * - species (in): Valid non-temperature species row.
     * - layer (in): Physical layer index.
     * - increment (in): Finite signed native inventory change; positive enters the layer.
     * @return True when mass, reservation and contact ledger shapes match, resulting mass
     * remains at least the nonnegative reservation, and both mass and receipt sums stay finite.
     * No state changes; this check alone does not preflight SIGMA overlap distribution.
     */
    bool canApplyContactMassIncrement(int species, std::size_t layer,
                                      double increment) const noexcept;
    /**
     * @brief Apply a signed finite-contact receipt to the physical layer and optional SIGMA
     * view.
     *
     * @par Declaration arguments
     * - species (in): Valid non-temperature species row.
     * - layer (in): Physical layer index.
     * - increment (in): Finite signed native inventory change, positive into this layer.
     * @return True after increasing mass/contact_mass and, when active, distributing the same
     * receipt through sigma.apply(). False leaves state unchanged on shape/value/budget or SIGMA
     * refusal.
     * @note Does not consume outgoing reservation overlays. Requires exclusive access to this
     * column.
     */
    bool applyContactMassIncrement(int species, std::size_t layer, double increment) noexcept;
};
} // namespace openswmm::twoD
