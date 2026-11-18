// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin

/**
 * @file      SigmaPorewaterMap.cpp
 * @brief     Derived overlap mapping for canonical sigma-layer porewater chemistry.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include "SigmaPorewaterMap.hpp"
#include "../subsurface/SubsurfaceTransportState.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace openswmm::twoD {
/**
 * @brief Construct a derived physical/SIGMA overlap view and seed it from canonical inventories.
 * @param[in] m Positive unsaturated reactor count; reactor m is the saturated zone.
 * @param[in] ns Positive species count.
 * @param[in] entries Owned overlap geometry, moved into the candidate. Each entry has a
 * nonnegative physical index, reactor in [0,m], finite nonnegative water m3 and finite positive
 * bulk volume m3. Physical upper bounds are the caller's responsibility because this map has no
 * physical-grid size.
 * @param[in] partition ns finite nonnegative retained-capacity factors: water-equivalent volume
 * per bulk volume. Overlap capacity is water + partition*bulk in m3.
 * @param[in] canonical Species-major native inventories with ns*(m+1) entries, unsaturated
 * reactors first and saturated last, all finite and nonnegative.
 * @param[in] held ns finite nonnegative saturated outgoing reservations, no greater than their
 * canonical inventory, in native species units.
 * @return Empty after replacing this map and zeroing pending receipts; diagnostic on invalid
 * layout, geometry, capacity or rebase refusal with this map unchanged. Allocation exceptions
 * propagate.
 * @note Empty entries are allowed; zero-capacity canonical dry mass remains solely with its
 * owner. Reservations require positive aqueous overlap capacity. Inputs must remain stable
 * during copying.
 */
std::string SigmaPorewaterMap::configure(int m, int ns, std::vector<Overlap> entries,
                                         const std::vector<double>& partition,
                                         const std::vector<double>& canonical,
                                         const std::vector<double>& held) {
    if (m <= 0 || ns <= 0 || int(partition.size()) != ns)
        return "Invalid SIGMA contact mapping layout.";
    // Keep the prior buffers for the next call, while invalid input still leaves this map intact.
    static thread_local SigmaPorewaterMap next;
    next.layers = m;
    next.species = ns;
    next.overlaps = std::move(entries);
    const auto ne = next.overlaps.size();
    next.capacity.resize(std::size_t(ns) * ne);
    next.mass.resize(next.capacity.size());
    next.reserved.resize(next.capacity.size());
    next.pending.assign(std::size_t(ns) * (m + 1), 0);
    for (int s = 0; s < ns; ++s)
        for (std::size_t e = 0; e < ne; ++e) {
            const auto& o = next.overlaps[e];
            if (o.physical < 0 || o.reactor < 0 || o.reactor > m || !std::isfinite(o.water) ||
                o.water < 0 || !std::isfinite(o.bulk) || o.bulk <= 0 ||
                !std::isfinite(partition[s]) || partition[s] < 0)
                return "Invalid SIGMA contact overlap geometry.";
            const double value = o.water + partition[s] * o.bulk;
            if (!std::isfinite(value))
                return "SIGMA contact capacity overflow.";
            next.capacity[std::size_t(s) * ne + e] = value;
        }
    if (auto error = next.rebase(canonical, held); !error.empty())
        return error;
    std::swap(*this, next);
    return {};
}
/**
 * @brief Redistribute canonical reactor mass and saturated claims over existing overlap
 * capacities.
 * @param[in] canonical Species-major native inventory, species*(layers+1) entries. Each amount
 * must be finite and nonnegative; the last reactor of each species is saturated.
 * @param[in] held One finite nonnegative saturated reservation per species, no greater than the
 * corresponding saturated mass; unsaturated reservation targets are zero.
 * @return Empty after replacing derived mass/reserved arrays; diagnostic on shape, invalid
 * inventory or unpartitionable reservation with those arrays unchanged. Allocation exceptions
 * propagate.
 * @note Existing geometry/capacity and storage sizes must be consistent. Largest positive
 * capacity receives the subtraction remainder. A zero-capacity reactor retains canonical mass
 * outside this view and gets zero overlap mass unless it has a reservation, which is refused.
 * Pending receipts are retained; callers must choose whether rebasing includes or excludes those
 * receipts.
 */
std::string SigmaPorewaterMap::rebase(const std::vector<double>& canonical,
                                      const std::vector<double>& held) {
    const auto ne = overlaps.size();
    const int nr = layers + 1;
    if (canonical.size() != std::size_t(species) * nr || held.size() != std::size_t(species))
        return "SIGMA contact canonical shape differs.";
    // Every overlap is written by exactly one reactor below. Staging preserves refusal
    // atomicity without copying the old derived values or reallocating on every rebase.
    static thread_local std::vector<double> values, claims;
    values.resize(mass.size());
    claims.resize(reserved.size());
    for (int s = 0; s < species; ++s)
        for (int r = 0; r < nr; ++r) {
            const double total = canonical[std::size_t(s) * nr + r],
                         promised = r == layers ? held[s] : 0;
            if (!std::isfinite(total) || total < 0 || !std::isfinite(promised) || promised < 0 ||
                promised > total)
                return "SIGMA contact inventory or reservation is invalid.";
            double sum = 0, largest = 0;
            std::size_t anchor = ne;
            for (std::size_t e = 0; e < ne; ++e)
                if (overlaps[e].reactor == r) {
                    const double c = capacity[std::size_t(s) * ne + e];
                    sum += c;
                    if (c > largest) {
                        largest = c;
                        anchor = e;
                    }
                }
            if (!(sum > 0)) { // Zero-capacity dry inventory remains with its canonical owner.
                if (promised > 0)
                    return "SIGMA contact reservation has no aqueous donor.";
                for (std::size_t e = 0; e < ne; ++e)
                    if (overlaps[e].reactor == r)
                        values[std::size_t(s) * ne + e] = claims[std::size_t(s) * ne + e] = 0;
                continue;
            }
            double assigned = 0, reservation = 0;
            for (std::size_t e = 0; e < ne; ++e)
                if (overlaps[e].reactor == r && e != anchor) {
                    const auto k = std::size_t(s) * ne + e;
                    const double fraction = capacity[k] / sum;
                    values[k] = total * fraction;
                    claims[k] = promised * fraction;
                    assigned += values[k];
                    reservation += claims[k];
                }
            const auto k = std::size_t(s) * ne + anchor;
            values[k] = total - assigned;
            claims[k] = promised - reservation;
            if (values[k] < claims[k] || claims[k] < 0)
                return "SIGMA contact reservation cannot be partitioned.";
        }
    mass.swap(values);
    reserved.swap(claims);
    return {};
}
/**
 * @brief Distribute a physical-layer contact receipt into overlap views and pending reactor
 * receipts.
 * @param[in] row Species index in [0,species).
 * @param[in] physical Physical layer index matched against overlap geometry. For a nonzero
 * receipt at least one matching overlap must have positive capacity/available inventory.
 * @param[in] increment Finite signed native inventory amount, positive into the physical layer.
 * @return True after publication, or for a zero increment after checking matching overlap
 * weights. False leaves state unchanged for invalid indices/weights, lack of capacity, overdraw,
 * overflow, or any caught exception including allocation failure.
 * @note Positive receipts partition by capacity, negative receipts by mass-reserved; the largest
 * weight gets the remainder. Canonical owner inventories are unchanged until publish(). Existing
 * overlap indices/array extents must be consistent. Requires exclusive access despite noexcept.
 */
bool SigmaPorewaterMap::apply(int row, int physical, double increment) noexcept {
    try {
        if (row < 0 || row >= species || !std::isfinite(increment))
            return false;
        const auto ne = overlaps.size();
        double total = 0, largest = 0;
        std::size_t anchor = ne;
        for (std::size_t e = 0; e < ne; ++e)
            if (overlaps[e].physical == physical) {
                const auto k = std::size_t(row) * ne + e;
                const double weight = increment >= 0 ? capacity[k] : mass[k] - reserved[k];
                if (!std::isfinite(weight) || weight < 0)
                    return false;
                total += weight;
                if (weight > largest) {
                    largest = weight;
                    anchor = e;
                }
            }
        if (increment == 0)
            return true;
        if (!(total > 0) || !std::isfinite(total) || anchor == ne)
            return false;
        auto next_mass = mass, next_pending = pending;
        double assigned = 0;
        const auto add = [&](std::size_t e, double delta) {
            const auto k = std::size_t(row) * ne + e,
                       q = std::size_t(row) * (layers + 1) + overlaps[e].reactor;
            next_mass[k] += delta;
            next_pending[q] += delta;
            return std::isfinite(next_mass[k]) && next_mass[k] >= reserved[k] &&
                   std::isfinite(next_pending[q]);
        };
        for (std::size_t e = 0; e < ne; ++e)
            if (overlaps[e].physical == physical && e != anchor) {
                const auto k = std::size_t(row) * ne + e;
                const double weight = increment >= 0 ? capacity[k] : mass[k] - reserved[k];
                const double delta = increment * (weight / total);
                if (!add(e, delta))
                    return false;
                assigned += delta;
            }
        if (!add(anchor, increment - assigned))
            return false;
        mass = std::move(next_mass);
        pending = std::move(next_pending);
        return true;
    } catch (...) {
        return false;
    }
}
/**
 * @brief Publish pending overlap receipts into canonical hydraulic reactors and refresh this
 * view.
 * @param[in,out] tr Canonical owner with matching layers/species and valid per-cell saturated
 * and layer inventories. Temperature rows are excluded; other row inventories receive pending
 * amounts.
 * @param[in] cell Valid owner cell index; this method does not perform a cell-bounds check.
 * @return Empty on success after canonical publication, unsaturated-view refresh, overlap rebase
 * and clearing pending receipts. Diagnostic on owner layout or candidate rebase refusal before
 * canonical writes; allocation exceptions also occur before publication.
 * @note Rebase preserves current summed overlap reservations as saturated claims. Prepared
 * indices, extents and owner invariants are required. Both objects need exclusive access for the
 * call.
 */
std::string SigmaPorewaterMap::publish(SubsurfaceTransportState& tr, int cell) {
    if (layers != tr.n_layers || species != tr.n_species)
        return "SIGMA contact owner layout differs.";
    const int nr = layers + 1;
    std::vector<double> canonical(std::size_t(species) * nr), held(species, 0);
    for (int s = 0; s < species; ++s) {
        if (s == tr.temp_row)
            continue;
        for (int r = 0; r < nr; ++r) {
            const auto q = std::size_t(s) * nr + r;
            canonical[q] = (r == layers ? tr.sat_mass[tr.idx(s, cell)]
                                        : tr.layer_mass[tr.layerIdx(s, r, cell)]) +
                           pending[q];
        }
        for (std::size_t e = 0; e < overlaps.size(); ++e)
            held[s] += reserved[std::size_t(s) * overlaps.size() + e];
    }
    if (auto error = rebase(canonical, held); !error.empty())
        return error;
    for (int s = 0; s < species; ++s)
        if (s != tr.temp_row) {
            for (int r = 0; r < nr; ++r) {
                const auto q = std::size_t(s) * nr + r;
                if (r == layers)
                    tr.sat_mass[tr.idx(s, cell)] = canonical[q];
                else
                    tr.layer_mass[tr.layerIdx(s, r, cell)] = canonical[q];
            }
            tr.refreshUnsat(s, cell);
        }
    std::fill(pending.begin(), pending.end(), 0);
    return {};
}
} // namespace openswmm::twoD
