// SPDX-License-Identifier: Apache-2.0
#include "SurfaceExchange.hpp"
#include "SubsurfaceSolver.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>

namespace openswmm::twoD {
namespace {
using Donor = std::tuple<SurfaceDonorKind, std::string, int>;
Donor donor(const SurfaceIntakeRequest& r) { return {r.kind, r.source, r.unit}; }
bool nonnegative(double v) { return std::isfinite(v) && v >= 0; }
}

std::string SurfaceExchange::plan(const SubsurfaceSolver& gw, double start, double end,
                                 double completed, const std::vector<SurfaceIntakeRequest>& requests) {
    if (planned_) return "Settle or cancel the previous source interval first.";
    if (!nonnegative(start) || !std::isfinite(end) || !std::isfinite(completed) ||
        start != completed_end_ || end <= start || end > completed)
        return "Source interval must be contiguous and completed; look-ahead/replay is unavailable.";
    if (!gw.active()) return "A spatial receiving aquifer is required.";

    std::vector<SurfaceIntakeAward> proposed;
    std::map<int, long double> contact;
    for (const auto& r : requests) {
        if (r.source.empty() || r.cell < 0 || r.cell >= gw.state().n_cells ||
            !nonnegative(r.area) || !nonnegative(r.pond) ||
            !nonnegative(r.water_ceiling) || !nonnegative(r.candidate) ||
            !(gw.state().area[r.cell] > 0) || !std::isfinite(gw.state().area[r.cell]))
            return "Invalid source identity, receiver, contact area, head or water request.";
        const double capacity = gw.sourceInfiltrationCapacity(r.cell, r.pond, end - start);
        const double volume = capacity * r.area * (end - start);
        if (!std::isfinite(volume)) return "Source capacity volume overflow.";
        proposed.push_back({r, std::min(r.candidate, volume), 0});
        contact[r.cell] += r.area;
    }
    for (const auto& [cell, area] : contact)
        if (area > gw.state().area[cell] * (1 + 1e-12))
            return "Source contact areas overbook the receiving cell.";
    // Stable IDs make accumulation and roundoff independent of caller ordering.
    std::sort(proposed.begin(), proposed.end(), [](const auto& a, const auto& b) {
        return std::tuple(donor(a.request), a.request.cell) < std::tuple(donor(b.request), b.request.cell);
    });
    for (std::size_t a = 0; a < proposed.size();) {
        std::size_t b = a; long double total = 0;
        const double ceiling = proposed[a].request.water_ceiling;
        while (b < proposed.size() && donor(proposed[b].request) == donor(proposed[a].request)) {
            if (proposed[b].request.water_ceiling != ceiling)
                return "A donor spanning cells must have one water ceiling.";
            if (b > a && proposed[b].request.cell == proposed[b-1].request.cell)
                return "Duplicate donor/receiver request.";
            total += proposed[b++].demand;
        }
        const long double scale = total > ceiling ? ceiling / total : 1;
        long double remaining = ceiling;
        for (auto j = a; j < b; ++j) {
            double take = static_cast<double>(std::min(remaining, proposed[j].demand * scale));
            if (take > remaining) take = std::nextafter(take, 0.0);
            proposed[j].demand = take; remaining -= take;
        }
        a = b;
    }
    std::map<int, long double> demand;
    for (const auto& a : proposed) demand[a.request.cell] += a.demand;
    std::map<int, double> headroom;
    std::map<int, long double> remaining;
    for (const auto& [cell, sum] : demand) remaining[cell] = headroom[cell] = gw.infiltrationHeadroom(cell);
    for (auto& a : proposed) {
        const int c = a.request.cell;
        const long double scale = demand[c] > headroom[c] ? headroom[c] / demand[c] : 1;
        a.maximum = static_cast<double>(std::min(remaining[c], a.demand * scale));
        if (a.maximum > remaining[c]) a.maximum = std::nextafter(a.maximum, 0.0);
        remaining[c] -= a.maximum;
    }
    awards_ = std::move(proposed); start_ = start; end_ = end; planned_ = true; receiver_ = &gw;
    return {};
}

std::string SurfaceExchange::commit(SubsurfaceSolver& gw, const std::vector<SurfaceIntakeActual>& actual) {
    if (!planned_ || receiver_ != &gw || actual.size() != awards_.size()) return "No matching source interval to settle.";
    const int ns = gw.transport().active() ? gw.transport().n_species : 0;
    std::map<int, long double> totals;
    std::vector<SurfaceIntakeReceipt> receipts;
    for (std::size_t j = 0; j < actual.size(); ++j) {
        const auto& x = actual[j]; const auto& a = awards_[j];
        if (!nonnegative(x.volume) || x.volume > a.maximum || int(x.mass.size()) != ns)
            return "Actual source transfer exceeds its award or has incompatible species.";
        for (const double m : x.mass) if (!nonnegative(m) || (x.volume == 0 && m != 0))
            return "Source water and constituent mass must be paired.";
        const double capacity_volume = gw.sourceInfiltrationCapacity(a.request.cell, a.request.pond, end_-start_) * a.request.area * (end_-start_);
        if (x.volume > capacity_volume)
            return "Receiver capacity changed; cancel and recompute source trials before withdrawal.";
        totals[a.request.cell] += x.volume;
        receipts.push_back({a.request, start_, end_, x.volume, x.mass});
    }
    for (const auto& [cell, sum] : totals)
        if (sum > gw.infiltrationHeadroom(cell))
            return "Receiver headroom changed; cancel and recompute source trials before withdrawal.";

    // Validation/allocation finish before any receiver mutation. No unused award
    // is booked, and capacity refresh cannot restore an accepted pending receipt.
    for (const auto& r : receipts) {
        gw.bookInfiltrationFromSurface(r.request.cell, r.volume);
        gw.state().wetting_front[r.request.cell] += r.volume / gw.state().area[r.request.cell];
        gw.state().infil_remaining[r.request.cell] = gw.infiltrationHeadroom(r.request.cell);
        for (int s = 0; s < ns; ++s) gw.bookInfiltrationMass(r.request.cell, s, r.mass[s]);
    }
    receipts_ = std::move(receipts); completed_end_ = end_; planned_ = false; receiver_ = nullptr;
    return {};
}

void SurfaceExchange::cancel() noexcept { planned_ = false; receiver_ = nullptr; awards_.clear(); }
}
