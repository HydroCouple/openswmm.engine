// SPDX-License-Identifier: Apache-2.0

/**
 * @file      LayerTransport.cpp
 * @brief     Conservative species transfers between physical layers.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include "LayerTransport.hpp"
#include <cmath>
#include <algorithm>
#include <utility>
namespace openswmm::twoD {
LayerTransportResult solveLayerTransport(const std::vector<double>& capacity,
                                         const std::vector<double>& rhs,
                                         const std::vector<LayerTransportMove>& moves,
                                         bool signed_species) {
    LayerTransportScratch scratch;
    solveLayerTransport(capacity, rhs, moves, scratch, signed_species);
    return std::move(scratch.result);
}
const LayerTransportResult& solveLayerTransport(const std::vector<double>& capacity,
                                               const std::vector<double>& rhs,
                                               const std::vector<LayerTransportMove>& moves,
                                               LayerTransportScratch& scratch,
                                               bool signed_species) {
    auto& r = scratch.result;
    r.mass.clear();
    r.concentration.clear();
    r.transferred.clear();
    r.error.clear();
    const int n = int(capacity.size());
    if (n == 0 || rhs.size() != capacity.size()) {
        r.error = "Layer transport shape differs";
        return r;
    }
    auto& diagonal = scratch.diagonal;
    auto& upper = scratch.upper;
    auto& lower = scratch.lower;
    auto& b = scratch.b;
    auto& dry = scratch.dry;
    diagonal = capacity;
    upper.assign(n, 0);
    lower.assign(n, 0);
    b = rhs;
    dry.assign(n, 0);
    for (int i = 0; i < n; ++i)
        if (!std::isfinite(capacity[i]) || capacity[i] < 0 || !std::isfinite(rhs[i]) ||
            (!signed_species && rhs[i] < 0)) {
            r.error = "Layer transport has inadmissible storage or paired mass";
            return r;
        }
    for (const auto& move : moves) {
        if (move.from < 0 || move.from >= n || move.to < -1 || move.to >= n ||
            move.from == move.to || (move.to >= 0 && std::abs(move.to - move.from) != 1) ||
            !std::isfinite(move.capacity) || move.capacity < 0) {
            r.error = "Invalid layer transport transfer";
            return r;
        }
        diagonal[move.from] += move.capacity;
        if (move.to == move.from + 1)
            lower[move.to] -= move.capacity;
        // An external export from store zero also satisfies -1 == from-1.
        // It contributes only to the donor diagonal, never to upper[-1].
        else if (move.to >= 0 && move.to == move.from - 1)
            upper[move.to] -= move.capacity;
    }
    for (int i = 0; i < n; ++i) {
        if (diagonal[i] == 0) {
            if (lower[i] != 0 || upper[i] != 0) {
                r.error = "Layer transport has a zero-capacity receiving store";
                return r;
            }
            dry[i] = b[i];
            b[i] = 0;
            diagonal[i] = 1;
        }
        if (i) {
            const double factor = lower[i] / diagonal[i - 1];
            diagonal[i] -= factor * upper[i - 1];
            b[i] -= factor * b[i - 1];
        }
        if (!std::isfinite(diagonal[i]) || !(diagonal[i] > 0) || !std::isfinite(b[i])) {
            r.error = "Layer transport system is singular or nonfinite";
            return r;
        }
    }
    r.concentration.assign(n, 0);
    for (int i = n - 1; i >= 0; --i) {
        r.concentration[i] =
            (b[i] - (i + 1 < n ? upper[i] * r.concentration[i + 1] : 0)) / diagonal[i];
        if (!std::isfinite(r.concentration[i]) || (!signed_species && r.concentration[i] < 0)) {
            r.error = "Layer transport produced inadmissible concentration";
            return r;
        }
    }
    r.mass.resize(n);
    for (int i = 0; i < n; ++i)
        r.mass[i] = capacity[i] * r.concentration[i] + dry[i];
    r.transferred.reserve(moves.size());
    for (const auto& move : moves)
        r.transferred.push_back(move.capacity * r.concentration[move.from]);
    return r;
}
LayerTransportResult advanceLayerTransportEuler(const std::vector<double>& capacity,
                                                const std::vector<double>& initial,
                                                const std::vector<double>& rhs,
                                                const std::vector<LayerTransportMove>& moves,
                                                bool signed_species,
                                                const std::vector<double>* donor_available) {
    LayerTransportScratch scratch;
    advanceLayerTransportEuler(capacity, initial, rhs, moves, scratch, signed_species, donor_available);
    return std::move(scratch.result);
}
const LayerTransportResult& advanceLayerTransportEuler(
    const std::vector<double>& capacity, const std::vector<double>& initial,
    const std::vector<double>& rhs, const std::vector<LayerTransportMove>& moves,
    LayerTransportScratch& scratch, bool signed_species,
    const std::vector<double>* donor_available) {
    auto& r = scratch.result;
    r.mass.clear();
    r.concentration.clear();
    r.transferred.clear();
    r.error.clear();
    const int n = int(capacity.size());
    if (n == 0 || initial.size() != capacity.size() || rhs.size() != capacity.size() ||
        (donor_available && donor_available->size() != capacity.size())) {
        r.error = "Euler layer transport shape differs";
        return r;
    }
    r.mass = rhs;
    r.concentration.assign(n, 0);
    auto& requested = scratch.requested;
    auto& remaining = scratch.remaining;
    auto& scale = scratch.scale;
    requested.assign(n, 0);
    remaining.resize(n);
    scale.assign(n, 1);
    for (int i = 0; i < n; ++i) {
        if (!std::isfinite(capacity[i]) || capacity[i] < 0 || !std::isfinite(initial[i]) ||
            !std::isfinite(rhs[i]) || (!signed_species && (initial[i] < 0 || rhs[i] < 0))) {
            r.error = "Euler layer transport has inadmissible storage or mass";
            return r;
        }
        r.concentration[i] = capacity[i] > 0 ? initial[i] / capacity[i] : 0;
        remaining[i] = std::min(initial[i], rhs[i]);
        if (donor_available) {
            const double available = (*donor_available)[i];
            if (!std::isfinite(available) || (!signed_species && available < 0)) {
                r.error = "Invalid Euler donor reservation";
                return r;
            }
            remaining[i] = std::min(remaining[i], available);
        }
    }
    for (const auto& move : moves) {
        if (move.from < 0 || move.from >= n || move.to < -1 || move.to >= n ||
            move.from == move.to || !std::isfinite(move.capacity) || move.capacity < 0) {
            r.error = "Invalid Euler layer transfer";
            return r;
        }
        const double amount = move.capacity * r.concentration[move.from];
        if (!std::isfinite(amount)) {
            r.error = "Nonfinite Euler layer transfer";
            return r;
        }
        requested[move.from] += amount;
        if (!std::isfinite(requested[move.from])) {
            r.error = "Nonfinite Euler donor demand";
            return r;
        }
    }
    if (!signed_species)
        for (int i = 0; i < n; ++i)
            if (requested[i] > remaining[i])
                scale[i] = remaining[i] / requested[i];
    for (const auto& move : moves) {
        const int from = move.from;
        double amount = move.capacity * r.concentration[from] * scale[from];
        if (!signed_species)
            amount = std::min(amount, remaining[from]);
        remaining[from] -= amount;
        r.mass[from] -= amount;
        if (move.to >= 0)
            r.mass[move.to] += amount;
        r.transferred.push_back(amount);
    }
    for (double mass : r.mass)
        if (!std::isfinite(mass) || (!signed_species && mass < 0)) {
            r.error = "Euler layer transport produced inadmissible mass";
            return r;
        }
    return r;
}

}
