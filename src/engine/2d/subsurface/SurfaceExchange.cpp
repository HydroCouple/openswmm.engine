// SPDX-License-Identifier: Apache-2.0

/**
 * @file      SurfaceExchange.cpp
 * @brief     Conservative surface exchange reservations and donor water limits.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include "SurfaceExchange.hpp"
#include "SubsurfaceSolver.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>

namespace openswmm::twoD {
namespace {
using Donor = std::tuple<SurfaceDonorKind, std::string, int>;
/**
 * @brief Form the exact identity shared by one source donor across receiving cells.
 * @param[in] r Borrowed intake request with source kind/name/unit.
 * @return Owned tuple (kind, source, unit); receiver cell is deliberately excluded. Copies
 * source text and may allocate; does not validate or mutate requests.
 */
Donor donor(const SurfaceIntakeRequest& r) {
    return {r.kind, r.source, r.unit};
}
/**
 * @brief Check an admissible nonnegative scalar for exchange planning/settlement.
 * @param[in] v Candidate scalar in the caller's SI units.
 * @return True only for finite v >= 0, including negative zero; false for NaN/infinity/negative.
 * Pure allocation-free test.
 */
bool nonnegative(double v) {
    return std::isfinite(v) && v >= 0;
}
}

/**
 * @brief Reserve a contiguous completed source interval against donor and receiver budgets.
 * @param[in] gw Borrowed active spatial aquifer with stable covered-cell
 * areas/capacity/headroom. Its identity is retained until commit/cancel, so it must remain alive
 * and unmoved.
 * @param[in] start Finite nonnegative elapsed start time, s; exactly completedEnd().
 * @param[in] end Finite elapsed endpoint, s, strictly greater than start and no later than
 * completed.
 * @param[in] completed Finite source-completion evidence through at least end, elapsed seconds;
 * canonical declaration calls this completed_through.
 * @param[in] requests Borrowed candidate transfers: nonempty source id, zero-based receiver
 * cell, contact area m2, pond head m, water_ceiling/candidate volumes m3. A donor's ceiling must
 * match across its cells; duplicate donor/cell pairs are refused.
 * @param[in] prescribed Optional borrowed synchronous capacity callback, invoked with request
 * and interval seconds and returning finite nonnegative capacity m3; NULL queries gw capacity.
 * Callback must be nonempty when supplied and not mutate/reenter this exchange or receiver.
 * @return Empty on a published plan, otherwise refusal text with no receiver water booked.
 * Existing plan is preserved if another plan is already pending.
 * @post Sorts awards by stable donor/cell identity, scales demands to each donor's available
 * water and each cell's receiver headroom, retains copied requests and receiver identity. Denied
 * capacity is not reassigned to another cell.
 * @note Requires serial exclusive exchange access and stable receiver reads. Local
 * allocation/callback exceptions propagate before the final plan publication; no
 * request/callback storage is retained, but the receiver pointer is borrowed until
 * settlement/cancel.
 */
std::string SurfaceExchange::plan(const SubsurfaceSolver& gw, double start, double end,
                                  double completed,
                                  const std::vector<SurfaceIntakeRequest>& requests,
                                  const Capacity* prescribed) {
    if (planned_)
        return "Settle or cancel the previous source interval first.";
    if (!nonnegative(start) || !std::isfinite(end) || !std::isfinite(completed) ||
        start != completed_end_ || end <= start || end > completed)
        return "Source interval must be contiguous and completed; look-ahead/replay is unavailable.";
    if (!gw.active())
        return "A spatial receiving aquifer is required.";

    std::vector<SurfaceIntakeAward> proposed;
    std::map<int, long double> contact;
    for (const auto& r : requests) {
        if (r.source.empty() || r.cell < 0 || r.cell >= gw.state().n_cells ||
            !nonnegative(r.area) || !nonnegative(r.pond) || !nonnegative(r.water_ceiling) ||
            !nonnegative(r.candidate) || !(gw.state().area[r.cell] > 0) ||
            !std::isfinite(gw.state().area[r.cell]))
            return "Invalid source identity, receiver, contact area, head or water request.";
        const double volume = prescribed
                                  ? (*prescribed)(r, end - start)
                                  : gw.sourceInfiltrationCapacity(r.cell, r.pond, end - start) *
                                        r.area * (end - start);
        if (!nonnegative(volume))
            return "Invalid source capacity volume.";
        proposed.push_back({r, std::min(r.candidate, volume), 0});
        contact[r.cell] += r.area;
    }
    for (const auto& [cell, area] : contact)
        if (area > gw.state().area[cell] * (1 + 1e-12))
            return "Source contact areas overbook the receiving cell.";
    // Stable IDs make accumulation and roundoff independent of caller ordering.
    std::sort(proposed.begin(), proposed.end(), [](const auto& a, const auto& b) {
        return std::tuple(donor(a.request), a.request.cell) <
               std::tuple(donor(b.request), b.request.cell);
    });
    for (std::size_t a = 0; a < proposed.size();) {
        std::size_t b = a;
        long double total = 0;
        const double ceiling = proposed[a].request.water_ceiling;
        while (b < proposed.size() && donor(proposed[b].request) == donor(proposed[a].request)) {
            if (proposed[b].request.water_ceiling != ceiling)
                return "A donor spanning cells must have one water ceiling.";
            if (b > a && proposed[b].request.cell == proposed[b - 1].request.cell)
                return "Duplicate donor/receiver request.";
            total += proposed[b++].demand;
        }
        const long double scale = total > ceiling ? ceiling / total : 1;
        long double remaining = ceiling;
        for (auto j = a; j < b; ++j) {
            double take = static_cast<double>(std::min(remaining, proposed[j].demand * scale));
            if (take > remaining)
                take = std::nextafter(take, 0.0);
            proposed[j].demand = take;
            remaining -= take;
        }
        a = b;
    }
    std::map<int, long double> demand;
    for (const auto& a : proposed)
        demand[a.request.cell] += a.demand;
    std::map<int, double> headroom;
    std::map<int, long double> remaining, allocated;
    for (const auto& [cell, sum] : demand)
        remaining[cell] = headroom[cell] = gw.infiltrationHeadroom(cell);
    for (auto& a : proposed) {
        const int c = a.request.cell;
        const long double scale = demand[c] > headroom[c] ? headroom[c] / demand[c] : 1;
        a.maximum = static_cast<double>(std::min(remaining[c], a.demand * scale));
        if (a.maximum > remaining[c])
            a.maximum = std::nextafter(a.maximum, 0.0);
        // Use the same ascending sum as commit, including platforms where
        // long double is double. Repeated remainder subtraction can award
        // one ulp more than the shared headroom at a nearly saturated cell.
        while (allocated[c] + a.maximum > headroom[c])
            a.maximum = std::nextafter(a.maximum, 0.0);
        allocated[c] += a.maximum;
        remaining[c] = headroom[c] - allocated[c];
    }
    awards_ = std::move(proposed);
    start_ = start;
    end_ = end;
    planned_ = true;
    receiver_ = &gw;
    return {};
}

/**
 * @brief Validate actual donor withdrawals and book their paired aquifer receipts.
 * @param[in,out] gw Same live receiver instance retained by the pending plan; serial access
 * prevents intervening capacity/owner mutations during settlement.
 * @param[in] actual Borrowed vector exactly matching awards() ORDER/count. Each volume is finite
 * nonnegative m3 no greater than its maximum. Each mass tuple matches the receiver's species
 * order, native concentration*m3 (age s*m3, temperature degrees C*m3); zero water requires zero
 * tuple, only temperature may be negative.
 * @param[in] prescribed Optional synchronous capacity callback with the same units/semantics
 * used during plan; it is reevaluated to reject stale trials.
 * @return Empty after successful booking, otherwise validation/refusal text before receiver
 * mutation. A refused plan remains available for cancel/retrial.
 * @post Successful commit appends retained provenance where configured, books pending
 * water/species, advances wetting-front/remaining allowance, replaces receipts(), advances
 * completedEnd(), and clears the plan. It does not install the caller's donor trial state.
 * @note Preflight validation/local receipt allocation precedes booking. Receiver provenance
 * vectors may still allocate during retainSourceReceipt(); an allocation exception can interrupt
 * the booking loop after earlier receipts were applied. The caller needs an enclosing owner
 * transaction when exception rollback is required. No input/callback pointers retained after
 * success; copied receipts remain object-owned.
 */
std::string SurfaceExchange::commit(SubsurfaceSolver& gw,
                                    const std::vector<SurfaceIntakeActual>& actual,
                                    const Capacity* prescribed) {
    if (!planned_ || receiver_ != &gw || actual.size() != awards_.size())
        return "No matching source interval to settle.";
    const auto& transport = gw.transport();
    const int ns = transport.active() ? transport.n_species : 0;
    std::map<int, long double> totals;
    std::vector<SurfaceIntakeReceipt> receipts;
    for (std::size_t j = 0; j < actual.size(); ++j) {
        const auto& x = actual[j];
        const auto& a = awards_[j];
        if (!nonnegative(x.volume) || x.volume > a.maximum || int(x.mass.size()) != ns)
            return "Actual source transfer exceeds its award or has incompatible species.";
        for (int s = 0; s < ns; ++s) {
            const double mass = x.mass[static_cast<std::size_t>(s)];
            if (!std::isfinite(mass) || (s != transport.temp_row && mass < 0) ||
                (x.volume == 0 && mass != 0))
                return "Source water and constituent mass must be paired.";
        }
        const double capacity_volume =
            prescribed
                ? (*prescribed)(a.request, end_ - start_)
                : gw.sourceInfiltrationCapacity(a.request.cell, a.request.pond, end_ - start_) *
                      a.request.area * (end_ - start_);
        if (!nonnegative(capacity_volume) || x.volume > capacity_volume)
            return "Receiver capacity changed; cancel and recompute source trials before withdrawal.";
        totals[a.request.cell] += x.volume;
        receipts.push_back({a.request, start_, end_, x.volume, x.mass});
    }
    for (const auto& [cell, sum] : totals)
        if (sum > gw.infiltrationHeadroom(cell))
            return "Receiver headroom changed; cancel and recompute source trials before withdrawal.";

    // Validation and local preflight allocation precede receiver mutations; receipt
    // retention can still allocate during booking. No unused award
    // is booked, and capacity refresh cannot restore an accepted pending receipt.
    for (const auto& r : receipts) {
        gw.retainSourceReceipt(r);
        gw.bookInfiltrationFromSurface(r.request.cell, r.volume);
        gw.state().wetting_front[r.request.cell] += r.volume / gw.state().area[r.request.cell];
        gw.state().infil_remaining[r.request.cell] = gw.infiltrationHeadroom(r.request.cell);
        for (int s = 0; s < ns; ++s)
            gw.bookInfiltrationMass(r.request.cell, s, r.mass[s]);
    }
    receipts_ = std::move(receipts);
    completed_end_ = end_;
    planned_ = false;
    receiver_ = nullptr;
    return {};
}

/**
 * @brief Discard pending intake awards without undoing committed receipts.
 * @post Clears planned flag, borrowed receiver pointer and awards. completedEnd() and the last
 * committed receipts remain unchanged. No receiver water is booked or restored; no
 * allocation/exceptions. Requires exclusive exchange access.
 */
void SurfaceExchange::cancel() noexcept {
    planned_ = false;
    receiver_ = nullptr;
    awards_.clear();
}
}
