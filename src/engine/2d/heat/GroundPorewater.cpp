// SPDX-License-Identifier: Apache-2.0

/**
 * @file      GroundPorewater.cpp
 * @brief     Species inventories in fixed physical soil layers.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include "GroundPorewater.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <stdexcept>
namespace openswmm::twoD {
namespace {
/**
 * @brief Compute one physical soil layer's bulk volume.
 * @param[in] soil Prepared positive-area column with valid depth edges.
 * @param[in] j Layer index with j+1 < soil.depth_edges.size().
 * @return area times layer thickness in m3. No shape, sign or finiteness check occurs.
 */
double bulk(const PorousHeatColumn& soil, std::size_t j) {
    return soil.area * (soil.depth_edges[j + 1] - soil.depth_edges[j]);
}
/**
 * @brief Compute the fraction of physical pore capacity occupied by saturated overlap.
 * @param[in] soil Prepared physical column with valid area, depths and porosity.
 * @param[in] j Valid physical layer index.
 * @param[in] sat Saturated water volume m3 assigned to this layer.
 * @return sat/(porosity*bulk volume) clipped to [0,1], or zero when pore capacity is not
 * positive. No input shape or finite-value validation occurs; no state is changed.
 */
double saturatedFraction(const PorousHeatColumn& soil, std::size_t j, double sat) {
    const double pore = soil.material.porosity * bulk(soil, j);
    return pore > 0 ? std::clamp(sat / pore, 0., 1.) : 0;
}
}
/**
 * @brief Seed canonical physical-layer species inventories from saturated/unsaturated totals.
 * @param[in] soil Prepared thermal geometry, moisture and material; layer count is theta.size().
 * @param[in] saturated Finite nonnegative saturated water volume m3 per layer, no greater than
 * physical layer water except the 1e-12*max(1,water) roundoff allowance.
 * @param[in] sat_mass Nonempty species totals for the saturated zone in native inventory units.
 * @param[in] unsat_mass Same-size unsaturated species totals. Non-temperature rows must be
 * finite and nonnegative and nonzero inventory requires positive finite physical zone capacity.
 * @param[in] kd Finite nonnegative distribution coefficient m3/kg per species. Age ignores
 * retention; temperature rows are not represented in porewater mass and their totals are
 * ignored.
 * @param[in] age Optional age row index, -1 when absent; otherwise within species count.
 * @param[in] temperature Optional distinct temperature row index, -1 when absent.
 * @return Empty on success after replacing this object; diagnostic on validation refusal with
 * this unchanged. Initial masses are copied, contact/reservation/generated-age ledgers start
 * zero.
 * @note Species-major layout is row*layer_count+layer. Sorption capacity uses solid density and
 * solid volume; zone totals are distributed in proportion to water plus retained capacity. The
 * input soil arrays must have consistent extents. Allocation errors propagate before
 * publication.
 */
std::string GroundPorewaterColumn::initialize(const PorousHeatColumn& soil,
                                              const std::vector<double>& saturated,
                                              const std::vector<double>& sat_mass,
                                              const std::vector<double>& unsat_mass,
                                              const std::vector<double>& kd, int age,
                                              int temperature) {
    const auto ns = sat_mass.size(), nl = soil.theta.size();
    if (!ns || !nl || saturated.size() != nl || unsat_mass.size() != ns || kd.size() != ns ||
        age < -1 || temperature < -1 || age >= int(ns) || temperature >= int(ns) ||
        (age >= 0 && age == temperature))
        return "Ground porewater seed shapes or row indices differ.";
    for (std::size_t j = 0; j < nl; ++j) {
        const double water = soil.theta[j] * bulk(soil, j);
        if (!std::isfinite(saturated[j]) || saturated[j] < 0 ||
            saturated[j] > water + 1e-12 * std::max(1., water))
            return "Ground porewater saturated overlap exceeds its physical layer water.";
    }
    GroundPorewaterColumn next;
    next.n_species = int(ns);
    next.age_row = age;
    next.temp_row = temperature;
    next.mass.assign(ns * nl, 0);
    next.retention = next.mass;
    next.contact_mass = next.mass;
    next.reserved_out = next.mass;
    next.generated_age.assign(nl, 0);
    for (std::size_t r = 0; r < ns; ++r) {
        if (!std::isfinite(kd[r]) || kd[r] < 0)
            return "Ground porewater Kd must be finite and nonnegative.";
        if (int(r) == temperature)
            continue;
        if (!std::isfinite(sat_mass[r]) || !std::isfinite(unsat_mass[r]) || sat_mass[r] < 0 ||
            unsat_mass[r] < 0)
            return "Ground porewater initial total masses must be finite and nonnegative.";
        std::vector<double> sat_capacity(nl), unsat_capacity(nl);
        double sat_total = 0, unsat_total = 0;
        for (std::size_t j = 0; j < nl; ++j) {
            const auto k = r * nl + j;
            const double retained = int(r) == age
                                        ? 0
                                        : (1 - soil.material.porosity) *
                                              soil.material.grain_density * kd[r] * bulk(soil, j);
            if (!std::isfinite(retained))
                return "Ground porewater retention capacity overflow.";
            next.retention[k] = retained;
            sat_capacity[j] = saturated[j] + retained * saturatedFraction(soil, j, saturated[j]);
            unsat_capacity[j] = std::max(0., soil.theta[j] * bulk(soil, j) - saturated[j]) +
                                retained * (1 - saturatedFraction(soil, j, saturated[j]));
            sat_total += sat_capacity[j];
            unsat_total += unsat_capacity[j];
        }
        if (!std::isfinite(sat_total) || !std::isfinite(unsat_total) ||
            (!(sat_total > 0) && sat_mass[r] > 0) || (!(unsat_total > 0) && unsat_mass[r] > 0))
            return "Nonzero ground porewater initial mass has no finite physical zone capacity.";
        for (std::size_t j = 0; j < nl; ++j) {
            next.mass[r * nl + j] =
                (sat_total > 0 ? sat_mass[r] * (sat_capacity[j] / sat_total) : 0) +
                (unsat_total > 0 ? unsat_mass[r] * (unsat_capacity[j] / unsat_total) : 0);
            if (!std::isfinite(next.mass[r * nl + j]))
                return "Ground porewater initial mass overflow.";
        }
    }
    next.initial_mass = next.mass;
    *this = std::move(next);
    return {};
}
/**
 * @brief Derive saturated and unsaturated totals without creating another inventory.
 * @param[in] soil Stable prepared physical layer geometry/moisture consistent with this column.
 * @param[in] saturated Saturated water volume m3 per physical layer.
 * @return Owned pair of species vectors (saturated,unsaturated), length n_species. The
 * temperature row contributes zero; mass is apportioned using water and retained capacity.
 * @throws std::invalid_argument On saturated, mass or retention shape mismatch.
 * @note Remaining soil geometry is assumed valid. Allocation failures propagate; no state
 * changes.
 */
std::pair<std::vector<double>, std::vector<double>>
GroundPorewaterColumn::zoneMasses(const PorousHeatColumn& soil,
                                  const std::vector<double>& saturated) const {
    const auto nl = soil.theta.size();
    if (saturated.size() != nl || mass.size() != std::size_t(n_species) * nl ||
        retention.size() != mass.size())
        throw std::invalid_argument("Ground porewater view shapes differ.");
    std::vector<double> sat(n_species, 0), unsat(n_species, 0);
    for (int r = 0; r < n_species; ++r) {
        const auto pair = zoneMass(soil, saturated, r);
        sat[r] = pair.first;
        unsat[r] = pair.second;
    }
    return {std::move(sat), std::move(unsat)};
}
/**
 * @brief Derive one species' saturated/unsaturated split from physical-layer mass.
 * @param[in] soil Stable prepared geometry/moisture; all depth and layer arrays must be valid.
 * @param[in] saturated Saturated water volume m3 per layer, with soil.theta.size() entries.
 * @param[in] r Species row in [0,n_species); temperature/invalid rows return zeros immediately.
 * @return Pair (saturated,unsaturated) in native species inventory units. Shares use water plus
 * retention and are clipped to [0,1], so the pair sums to canonical mass for the species.
 * @note noexcept does not validate array extents; mass/retention must each have n_species*layers
 * entries. No allocation, mutation or receipt consumption occurs.
 */
std::pair<double, double> GroundPorewaterColumn::zoneMass(const PorousHeatColumn& soil,
                                                          const std::vector<double>& saturated,
                                                          int r) const noexcept {
    double sat = 0, unsat = 0;
    const auto nl = soil.theta.size();
    if (r == temp_row || r < 0 || r >= n_species)
        return {0, 0};
    for (std::size_t j = 0; j < nl; ++j) {
        const auto k = std::size_t(r) * nl + j;
        const double water = soil.theta[j] * bulk(soil, j), capacity = water + retention[k],
                     fraction = saturatedFraction(soil, j, saturated[j]);
        const double share =
            capacity > 0 ? std::clamp((saturated[j] + retention[k] * fraction) / capacity, 0., 1.)
                         : fraction;
        const double carried = mass[k] * share;
        sat += carried;
        unsat += mass[k] - carried;
    }
    return {sat, unsat};
}
/**
 * @brief Add elapsed water age to every physical layer and its generation ledger.
 * @param[in] soil Prepared physical column, with stable moisture and geometry in SI units.
 * @param[in] dt Finite nonnegative interval s; ignored when age_row is absent.
 * @return True after adding dt*water volume (m3 s) to age inventory and generated_age, or true
 * without mutation when no age row exists. False on invalid interval/shapes or finite overflow,
 * with no updates. Prepared age_row must be within n_species and soil geometry consistent.
 */
bool GroundPorewaterColumn::generateAge(const PorousHeatColumn& soil, double dt) noexcept {
    if (age_row < 0)
        return true;
    const auto nl = soil.theta.size(), base = std::size_t(age_row) * nl;
    if (!std::isfinite(dt) || dt < 0 || generated_age.size() != nl ||
        mass.size() != std::size_t(n_species) * nl)
        return false;
    for (std::size_t j = 0; j < nl; ++j) {
        const double generated = dt * soil.theta[j] * bulk(soil, j);
        if (!std::isfinite(generated) || !std::isfinite(mass[base + j] + generated) ||
            !std::isfinite(generated_age[j] + generated))
            return false;
    }
    for (std::size_t j = 0; j < nl; ++j) {
        const double generated = dt * soil.theta[j] * bulk(soil, j);
        mass[base + j] += generated;
        generated_age[j] += generated;
    }
    return true;
}
/**
 * @brief Read unreserved physical-layer inventory available to a finite contact.
 * @param[in] r Species index; temperature rows are unsupported.
 * @param[in] j Physical layer index, derived from retention.size()/n_species.
 * @return mass-reserved_out in native inventory units. Returns NaN for invalid indices/shapes,
 * temperature rows, nonfinite or negative inventory/reservation, or over-reservation.
 * @note Read-only, allocation-free and noexcept; canonical mass still includes queued
 * reservations.
 */
double GroundPorewaterColumn::contactAvailableMass(int r, std::size_t j) const noexcept {
    if (n_species <= 0 || r < 0 || r >= n_species ||
        retention.size() % std::size_t(n_species) != 0 || mass.size() != retention.size() ||
        reserved_out.size() != mass.size() || r == temp_row)
        return std::numeric_limits<double>::quiet_NaN();
    const auto nl = retention.size() / std::size_t(n_species);
    if (j >= nl)
        return std::numeric_limits<double>::quiet_NaN();
    const auto k = std::size_t(r) * nl + j;
    const double available = mass[k] - reserved_out[k];
    if (!std::isfinite(mass[k]) || !std::isfinite(reserved_out[k]) || mass[k] < 0 ||
        reserved_out[k] < 0 || available < 0)
        return std::numeric_limits<double>::quiet_NaN();
    return available;
}
/**
 * @brief Preflight an outgoing reservation against unreserved canonical mass.
 * @param[in] r Non-temperature species index.
 * @param[in] j Physical layer index.
 * @param[in] amount Finite nonnegative native inventory amount to reserve.
 * @return True when indices/shapes and available mass are valid, amount fits, and the
 * reservation sum remains finite. False leaves all state unchanged; no reservation is created.
 */
bool GroundPorewaterColumn::canReserveOutgoing(int r, std::size_t j, double amount) const noexcept {
    const double available = contactAvailableMass(r, j);
    const auto nl = retention.size() / std::size_t(std::max(1, n_species));
    if (!std::isfinite(available) || !std::isfinite(amount) || amount < 0 || r < 0 ||
        r >= n_species || j >= nl)
        return false;
    const auto k = std::size_t(r) * nl + j;
    return std::isfinite(reserved_out[k] + amount) && amount <= available;
}
/**
 * @brief Reserve accepted outgoing inventory without subtracting its canonical mass.
 * @param[in] r Non-temperature species index.
 * @param[in] j Physical layer index.
 * @param[in] amount Finite nonnegative amount in native inventory units.
 * @return True after increasing reserved_out; false with no mutation when canReserveOutgoing()
 * refuses. The existing gather, not this overlay, eventually removes canonical outgoing mass.
 */
bool GroundPorewaterColumn::reserveOutgoing(int r, std::size_t j, double amount) noexcept {
    if (!canReserveOutgoing(r, j, amount))
        return false;
    const auto nl = retention.size() / std::size_t(std::max(1, n_species)),
               k = std::size_t(r) * nl + j;
    reserved_out[k] += amount;
    return true;
}
/**
 * @brief Release a queued outgoing reservation after its owning gather has consumed the packet.
 * @param[in] r Species index in [0,n_species).
 * @param[in] j Physical layer index for a consistently prepared inventory layout.
 * @param[in] amount Finite nonnegative native amount, no greater than the existing reservation.
 * @return True after subtracting only reserved_out, setting exact complete settlement to zero;
 * false on invalid index/shape/value or insufficient reservation, with no mutation.
 * @note Does not subtract mass or update contact receipts; callers perform the matching
 * inventory gather separately. Prepared retention/mass extents must describe the same
 * physical-layer count.
 */
bool GroundPorewaterColumn::settleOutgoingReservation(int r, std::size_t j,
                                                      double amount) noexcept {
    const auto nl = retention.size() / std::size_t(std::max(1, n_species));
    if (r < 0 || r >= n_species || j >= nl || reserved_out.size() != mass.size() ||
        !std::isfinite(amount) || amount < 0)
        return false;
    const auto k = std::size_t(r) * nl + j;
    if (!std::isfinite(reserved_out[k]) || amount > reserved_out[k])
        return false;
    reserved_out[k] = (amount == reserved_out[k]) ? 0 : reserved_out[k] - amount;
    return true;
}
/**
 * @brief Preflight a signed contact receipt without spending reserved outgoing inventory.
 * @param[in] r Valid non-temperature species row.
 * @param[in] j Physical layer index.
 * @param[in] increment Finite signed native inventory change; positive enters the layer.
 * @return True when mass, reservation and contact ledger shapes match, resulting mass remains at
 * least the nonnegative reservation, and both mass and receipt sums stay finite. No state
 * changes; this check alone does not preflight SIGMA overlap distribution.
 */
bool GroundPorewaterColumn::canApplyContactMassIncrement(int r, std::size_t j,
                                                         double increment) const noexcept {
    const auto nl = retention.size() / std::size_t(std::max(1, n_species));
    if (r < 0 || r >= n_species || j >= nl || mass.size() != retention.size() ||
        reserved_out.size() != mass.size() || contact_mass.size() != mass.size() || r == temp_row ||
        !std::isfinite(increment))
        return false;
    const auto k = std::size_t(r) * nl + j;
    const double next = mass[k] + increment, receipt = contact_mass[k] + increment;
    return std::isfinite(next) && std::isfinite(reserved_out[k]) && next >= reserved_out[k] &&
           reserved_out[k] >= 0 && std::isfinite(receipt);
}
/**
 * @brief Apply a signed finite-contact receipt to the physical layer and optional SIGMA view.
 * @param[in] r Valid non-temperature species row.
 * @param[in] j Physical layer index.
 * @param[in] increment Finite signed native inventory change, positive into this layer.
 * @return True after increasing mass/contact_mass and, when active, distributing the same
 * receipt through sigma.apply(). False leaves state unchanged on shape/value/budget or SIGMA
 * refusal.
 * @note Does not consume outgoing reservation overlays. Requires exclusive access to this
 * column.
 */
bool GroundPorewaterColumn::applyContactMassIncrement(int r, std::size_t j,
                                                      double increment) noexcept {
    if (!canApplyContactMassIncrement(r, j, increment))
        return false;
    const auto nl = retention.size() / std::size_t(std::max(1, n_species)),
               k = std::size_t(r) * nl + j;
    if (sigma.active() && !sigma.apply(r, int(j), increment))
        return false;
    mass[k] += increment;
    contact_mass[k] += increment;
    return true;
}
/**
 * @brief Rebuild physical-layer mass and reservation views from an active SIGMA overlap map.
 * @note Does nothing without an active map. Otherwise zeros mass/reserved_out and gathers every
 * species/overlap into its physical layer. Prepared n_species, inventory sizes and overlap
 * indices must be consistent; no validation or rollback occurs. Contact/initial/age ledgers are
 * retained. Canonical SIGMA inventories and pending receipts are unchanged; no allocation
 * occurs.
 */
void GroundPorewaterColumn::refreshSigmaView() {
    if (!sigma.active())
        return;
    std::fill(mass.begin(), mass.end(), 0);
    std::fill(reserved_out.begin(), reserved_out.end(), 0);
    const auto nl = retention.size() / std::size_t(n_species), ne = sigma.overlaps.size();
    for (int s = 0; s < n_species; ++s)
        for (std::size_t e = 0; e < ne; ++e) {
            const auto k = std::size_t(s) * nl + sigma.overlaps[e].physical,
                       q = std::size_t(s) * ne + e;
            mass[k] += sigma.mass[q];
            reserved_out[k] += sigma.reserved[q];
        }
}
/**
 * @brief Publish pending SIGMA contact receipts and refresh the derived physical-layer views.
 * @param[in,out] tr Canonical transport owner with matching SIGMA species/layers and cell
 * extent.
 * @param[in] cell Target cell index in that owner.
 * @return Empty when inactive or successfully published/refreshed; otherwise sigma.publish()'s
 * diagnostic, without refreshing the derived views. The map handles pending receipt validation.
 * @note This method borrows the owner only for the call; publication requires exclusive access
 * to both objects and must occur at the phase where pending contact receipts become canonical.
 */
std::string GroundPorewaterColumn::publishSigma(SubsurfaceTransportState& tr, int cell) {
    if (!sigma.active())
        return {};
    if (auto error = sigma.publish(tr, cell); !error.empty())
        return error;
    refreshSigmaView();
    return {};
}

} // namespace openswmm::twoD
