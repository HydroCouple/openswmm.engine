// SPDX-License-Identifier: Apache-2.0

/**
 * @file      PorewaterTransport.cpp
 * @brief     Transport of species in fixed physical porewater layers.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include "PorewaterTransport.hpp"
#include "../subsurface/LayerTransport.hpp"
#include <algorithm>
#include <cmath>
namespace openswmm::twoD {
/**
 * @brief Compute explicit fixed-layer porewater transport from accepted physical water tangents.
 * @param[in] old_water Finite nonnegative beginning water m3 per layer.
 * @param[in] retention Finite nonnegative sorption capacity m3 per layer, zero for age.
 * @param[in] mass Finite nonnegative native_concentration*m3 inventory per layer; nonempty and
 * its length defines the common layer count n.
 * @param[in] change Finite signed accepted physical water increment m3 per layer, independent of
 * geometric hydraulic-grid remapping.
 * @param[in] external_water Finite signed net accepted external water m3 per layer.
 * @param[in] external_mass Finite signed paired external inventory per layer; withdrawal below
 * is excluded from it to avoid double booking.
 * @param[in] withdrawal Finite nonnegative accepted outgoing volume m3 per layer; exactly n
 * entries.
 * @param[in] reserved_water Optional n beginning water reservations m3, finite/nonnegative and
 * no greater than old_water. Empty means zero.
 * @param[in] reserved_mass Optional n canonical mass reservations, finite/nonnegative and no
 * greater than mass; empty means zero. Reserved packets cannot donate to another export.
 * @return Owned result with empty error on success; final n masses, donor/accepted-withdrawal
 * concentrations, n withdrawn masses, n-1 downward-positive interior water/mass receipts, net
 * external mass and bottom water residual m3. On refusal error is nonempty and no partial
 * solution is published. Inputs are always unchanged; allocation errors propagate.
 * @note Every required vector has n entries. Reconstructs interior flux by water balance with
 * 1e-10*absolute-volume-scale + 1e-12 m3 closure tolerance. Uses LayerTransportEuler donor
 * limiting; returned concentration reflects accepted withdrawal composition when that budget
 * binds.
 */
PorewaterAdvection advectPorewaterEuler(
    const std::vector<double>& old_water, const std::vector<double>& retention,
    const std::vector<double>& mass, const std::vector<double>& change,
    const std::vector<double>& external_water, const std::vector<double>& external_mass,
    const std::vector<double>& withdrawal, const std::vector<double>& reserved_water,
    const std::vector<double>& reserved_mass) {
    const auto refused = [](const char* message) {
        PorewaterAdvection out;
        out.error = message;
        return out;
    };
    const auto n = mass.size();
    if (!n || old_water.size() != n || retention.size() != n || change.size() != n ||
        external_water.size() != n || external_mass.size() != n || withdrawal.size() != n ||
        (!reserved_water.empty() && reserved_water.size() != n) ||
        (!reserved_mass.empty() && reserved_mass.size() != n))
        return refused("Euler porewater layer shapes differ.");
    std::vector<double> capacity(n), initial(n), rhs(n), water(n + 1, 0);
    double scale = 0;
    for (std::size_t j = 0; j < n; ++j) {
        const double held_water = reserved_water.empty() ? 0 : reserved_water[j],
                     held_mass = reserved_mass.empty() ? 0 : reserved_mass[j];
        for (double value :
             {old_water[j], retention[j], mass[j], withdrawal[j], held_water, held_mass})
            if (!std::isfinite(value) || value < 0)
                return refused(
                    "Euler porewater stores and reservations must be finite and nonnegative.");
        if (held_water > old_water[j] || held_mass > mass[j])
            return refused("Euler porewater reservation exceeds its physical donor.");
        if (!std::isfinite(change[j]) || !std::isfinite(external_water[j]) ||
            !std::isfinite(external_mass[j]))
            return refused("Nonfinite Euler porewater receipt.");
        capacity[j] = old_water[j] - held_water + retention[j];
        initial[j] = mass[j] - held_mass;
        rhs[j] = mass[j] + external_mass[j];
        water[j + 1] = water[j] + external_water[j] - change[j];
        scale += std::abs(external_water[j]) + std::abs(change[j]);
        if (!std::isfinite(water[j + 1]) || !std::isfinite(scale))
            return refused("Euler porewater water receipt overflow.");
    }
    if (std::abs(water.back()) > 1e-10 * scale + 1e-12)
        return refused("Euler porewater increments do not close the physical water balance.");
    std::vector<LayerTransportMove> moves;
    moves.reserve(2 * n - 1);
    for (std::size_t j = 1; j < n; ++j) {
        const int from = int(water[j] >= 0 ? j - 1 : j), to = int(water[j] >= 0 ? j : j - 1);
        moves.push_back({from, to, std::abs(water[j])});
    }
    for (std::size_t j = 0; j < n; ++j)
        moves.push_back({int(j), -1, withdrawal[j]});
    auto result = advanceLayerTransportEuler(capacity, initial, rhs, moves);
    if (!result.error.empty())
        return refused(result.error.c_str());
    PorewaterAdvection out;
    out.mass = std::move(result.mass);
    out.concentration = std::move(result.concentration);
    out.water_residual = water.back();
    out.vertical_water.assign(water.begin() + 1, water.end() - 1);
    for (std::size_t j = 1; j < n; ++j)
        out.vertical_mass.push_back((water[j] >= 0 ? 1 : -1) * result.transferred[j - 1]);
    for (std::size_t j = 0; j < n; ++j) {
        const double amount = result.transferred[n - 1 + j];
        out.withdrawn_mass.push_back(amount);
        if (withdrawal[j] > 0)
            out.concentration[j] = amount / withdrawal[j];
        out.external_mass += external_mass[j] - amount;
        if (!std::isfinite(out.concentration[j]) || !std::isfinite(out.external_mass))
            return refused("Euler porewater accepted receipt overflow.");
    }
    return out;
}
/**
 * @brief Solve conservative implicit upwind porewater transport on fixed physical layers.
 * @param[in] old_water Finite nonnegative beginning water m3 per layer.
 * @param[in] new_water Finite nonnegative endpoint water m3 per layer.
 * @param[in] retention Finite nonnegative equilibrium sorption capacity m3 per layer, zero for
 * age.
 * @param[in] mass Nonempty finite nonnegative beginning native_concentration*m3 inventory;
 * defines n.
 * @param[in] external_water Finite signed net accepted external water m3 per layer, including
 * actual physical transfers rather than pure moving-grid handover.
 * @param[in] external_mass Finite signed paired external inventory, excluding unknown implicit
 * withdrawals. mass+external_mass must be nonnegative for every layer.
 * @param[in] withdrawal Finite nonnegative implicit dissolved outgoing volumes m3, n entries.
 * @return Owned result with empty error on success: n final masses/equilibrium concentrations
 * and withdrawn masses, n-1 downward-positive water/mass transfers, net external inventory and
 * bottom water closure residual. Nonempty error reports refusal with no partial solution; inputs
 * remain unchanged. Allocation errors propagate.
 * @note All input vectors have n entries. Interior water balance must close within 1e-10
 * absolute transfer scale + 1e-12 m3. The tridiagonal M-matrix uses new water plus retention and
 * outgoing volumes. A zero-capacity dry layer retains immobile residue conservatively with zero
 * dissolved concentration; a throughflowing zero-end-water layer needs no artificial pore
 * volume.
 */
PorewaterAdvection
advectPorewater(const std::vector<double>& old_water, const std::vector<double>& new_water,
                const std::vector<double>& retention, const std::vector<double>& mass,
                const std::vector<double>& external_water, const std::vector<double>& external_mass,
                const std::vector<double>& withdrawal) {
    const auto refused = [](const char* message) {
        PorewaterAdvection out;
        out.error = message;
        return out;
    };
    const auto n = mass.size();
    if (!n || old_water.size() != n || new_water.size() != n || retention.size() != n ||
        external_water.size() != n || external_mass.size() != n || withdrawal.size() != n)
        return refused("Porewater transport layer shapes differ.");
    std::vector<double> capacity(n), diagonal(n), lower(n, 0), upper(n, 0), rhs(n), water(n + 1, 0);
    double volume_scale = 0;
    for (std::size_t j = 0; j < n; ++j) {
        for (double value : {old_water[j], new_water[j], retention[j], mass[j], withdrawal[j]})
            if (!std::isfinite(value) || value < 0)
                return refused(
                    "Porewater water, retention, mass and withdrawals must be finite and nonnegative.");
        if (!std::isfinite(external_water[j]) || !std::isfinite(external_mass[j]))
            return refused("Nonfinite accepted porewater receipt.");
        capacity[j] = new_water[j] + retention[j];
        diagonal[j] = capacity[j] + withdrawal[j];
        rhs[j] = mass[j] + external_mass[j];
        if (rhs[j] < 0)
            return refused("Accepted porewater export exceeds its physical layer donor mass.");
        const double delta = new_water[j] - old_water[j];
        water[j + 1] = water[j] + external_water[j] - delta;
        volume_scale += std::abs(external_water[j]) + std::abs(delta);
    }
    if (!std::isfinite(volume_scale) || !std::isfinite(water.back()) ||
        std::abs(water.back()) > 1e-10 * volume_scale + 1e-12)
        return refused("Accepted porewater transfers do not close the fixed-layer water balance.");
    for (std::size_t j = 1; j < n; ++j) {
        const double q = std::abs(water[j]);
        if (water[j] >= 0) {
            diagonal[j - 1] += q;
            lower[j] -= q;
        } else {
            diagonal[j] += q;
            upper[j - 1] -= q;
        }
    }
    std::vector<unsigned char> residue(n, 0);
    for (std::size_t j = 0; j < n; ++j) {
        if (!std::isfinite(diagonal[j]) || !std::isfinite(rhs[j]))
            return refused("Porewater transport system overflow.");
        if (diagonal[j] == 0) {
            // No water/sorption capacity and no outgoing dissolved packet.
            // Incoming solute accumulates as immobile dry residue. This row
            // cannot supply another row, so remove only its own incoming
            // equation; retain those receipts in the conservative reconstruction.
            residue[j] = 1;
            diagonal[j] = 1;
            rhs[j] = 0;
            lower[j] = upper[j] = 0;
        }
    }
    for (std::size_t j = 1; j < n; ++j) {
        if (!(diagonal[j - 1] > 0) || !std::isfinite(diagonal[j - 1]))
            return refused("Invalid implicit porewater donor capacity.");
        const double f = lower[j] / diagonal[j - 1];
        diagonal[j] -= f * upper[j - 1];
        rhs[j] -= f * rhs[j - 1];
    }
    std::vector<double> concentration(n), out_mass(n), withdrawn(n), vertical_mass(n - 1);
    for (std::size_t j = n; j-- > 0;) {
        if (!(diagonal[j] > 0) || !std::isfinite(diagonal[j]))
            return refused("Invalid implicit porewater donor capacity.");
        concentration[j] =
            (rhs[j] - (j + 1 < n ? upper[j] * concentration[j + 1] : 0)) / diagonal[j];
        if (!std::isfinite(concentration[j]) || concentration[j] < 0)
            return refused("Nonfinite or negative porewater transport solution.");
        withdrawn[j] = withdrawal[j] * concentration[j];
        // C*c is the canonical equilibrated mass. A zero final capacity
        // after throughflow holds exactly zero, without a fake pore volume.
        out_mass[j] = capacity[j] * concentration[j];
    }
    for (std::size_t j = 1; j < n; ++j)
        vertical_mass[j - 1] = water[j] * (water[j] >= 0 ? concentration[j - 1] : concentration[j]);
    double external_total = 0;
    for (std::size_t j = 0; j < n; ++j) {
        external_total += external_mass[j] - withdrawn[j];
        if (residue[j]) {
            long double retained =
                static_cast<long double>(mass[j]) + external_mass[j] - withdrawn[j];
            if (j > 0)
                retained += vertical_mass[j - 1];
            if (j + 1 < n)
                retained -= vertical_mass[j];
            out_mass[j] = double(retained);
        }
        if (!std::isfinite(out_mass[j]) || out_mass[j] < 0 || !std::isfinite(withdrawn[j]))
            return refused("Nonfinite or negative accepted porewater mass.");
    }
    if (!std::isfinite(external_total))
        return refused("Porewater transport receipt overflow.");
    PorewaterAdvection out;
    out.mass = std::move(out_mass);
    out.concentration = std::move(concentration);
    out.withdrawn_mass = std::move(withdrawn);
    out.vertical_mass = std::move(vertical_mass);
    out.vertical_water.assign(water.begin() + 1, water.end() - 1);
    out.external_mass = external_total;
    out.water_residual = water.back();
    return out;
}
} // namespace openswmm::twoD
