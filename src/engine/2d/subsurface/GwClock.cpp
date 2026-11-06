// SPDX-License-Identifier: Apache-2.0
#include "GwClock.hpp"
#include "../../core/CompletedSourceArchive.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

namespace openswmm::twoD {
namespace {
void uniqueIds(std::vector<int>& ids) {
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
}
bool positive(double x) {
    return x > 0 && std::isfinite(x);
}
constexpr double infinity = std::numeric_limits<double>::infinity();
// Independent floating-point expressions for the same absolute timestamp
// (e.g. i*0.05 and (i-1)*0.05+0.05) can differ by one representable time.
bool exceeds(double end, double limit) {
    return end > std::nextafter(limit, infinity);
}
}

double GwClock::margin() const noexcept {
    return options_.surface_max_step;
}
double GwClock::period(int k) const {
    return std::ldexp(options_.base_step, k);
}

double GwClock::producerStep() const {
    return std::min(options_.base_step, period(select(infinity, infinity)));
}

int GwClock::select(double bound, double accuracy) const {
    if (!(bound > 0))
        throw std::invalid_argument("GW clock: nonpositive or NaN stability bound");
    const auto quantize = [&](double limit) {
        int k = std::ilogb(limit) - std::ilogb(options_.base_step);
        if (period(k) > limit)
            --k;
        if (!positive(period(k)))
            throw std::range_error("GW clock: step is not representable");
        return k;
    };
    // The ceiling reserves its own bounded allowance. A physical/accuracy
    // limit looser than that ceiling cannot require a finer period: the hard
    // deadline already keeps the actual interval below the looser limit.
    const double cap = options_.max_step;
    const double cap_margin = std::min(margin(), cap / 4);
    const bool fine_cap = cap - cap_margin < options_.base_step;
    int k = quantize(fine_cap ? cap : cap - cap_margin);
    if (fine_cap)
        k = std::min(k, -1);
    if (bound < cap && bound - margin() < options_.base_step)
        k = std::min(k, std::min(quantize(bound), -1));
    else if (bound < cap && period(k) > bound - margin())
        k = quantize(bound - margin());
    // Accuracy alone never creates stiff numerical subcycles. Its finest
    // period is the authored base check; physical stability remains exact.
    if (!(accuracy > 0))
        throw std::invalid_argument("GW clock: invalid accuracy limit");
    const double limit_accuracy = std::max(options_.base_step, accuracy - margin());
    if (accuracy < cap && period(k) > limit_accuracy)
        k = std::min(k, std::max(0, quantize(limit_accuracy)));
    return k;
}

double GwClock::next(double after, int k) const {
    const double p = period(k);
    double n = std::floor((after - options_.epoch) / p);
    if (!std::isfinite(n) || n >= 0x1p53)
        throw std::range_error("GW clock: ladder tick is not representable");
    // Division can round a point immediately below a grid line onto the line.
    // Never skip that deadline; never drift the phase by adding dt to a check.
    double due = std::fma(n, p, options_.epoch);
    if (due <= after) {
        const double successor = std::fma(n + 1, p, options_.epoch);
        // Conversely division at a represented grid instant can round DOWN:
        // n+1 is then this same instant, and n+2 is the next one.
        if (successor == after && due < after && n + 1 < 0x1p53)
            due = std::fma(n + 2, p, options_.epoch);
        else
            due = successor;
    }
    if (!std::isfinite(due) || !(due > after))
        throw std::range_error("GW clock: absolute rung cannot advance");
    return due;
}

GwClock::GwClock(Options options, std::span<const double> bounds, std::vector<Face> faces,
                 std::span<const double> accuracy)
    : options_(options), time_(options.epoch), faces_(std::move(faces)), incident_(bounds.size()),
      face_last_(faces_.size(), time_), due_(bounds.size(), infinity),
      latest_(bounds.size(), infinity) {
    if (!std::isfinite(time_) || !positive(options.base_step) || !positive(options.max_step) ||
        !positive(options.surface_max_step) || !positive(margin()))
        throw std::invalid_argument("GW clock: invalid epoch or time-step options");
    if (!accuracy.empty() && accuracy.size() != bounds.size())
        throw std::invalid_argument("GW clock: accuracy shape differs");
    for (int e = 0; e < static_cast<int>(faces_.size()); ++e) {
        const auto [l, r] = faces_[e];
        if (l < 0 || r < 0 || std::size_t(l) >= bounds.size() || std::size_t(r) >= bounds.size() ||
            l == r)
            throw std::invalid_argument("GW clock: invalid face topology");
        incident_[l].push_back(e);
        incident_[r].push_back(e);
    }
    for (int i = 0; i < static_cast<int>(bounds.size()); ++i) {
        cells_.push_back(
            {time_, bounds[i], select(bounds[i], accuracy.empty() ? 1e30 : accuracy[i])});
        schedule(i, cells_.back().rung, time_);
    }
}

void GwClock::schedule(int id, int k, double after) {
    auto& cell = cells_[id];
    if (std::isfinite(due_[id]) && cell.rung == 0)
        --base_cells_;
    (cell.rung < 0 ? fine_ : coarse_).erase({due_[id], id});
    if (cell.rung >= 0)
        limits_.erase({latest_[id], id});
    cell.rung = k;
    if (k == 0)
        ++base_cells_;
    due_[id] = next(after, k);
    (k < 0 ? fine_ : coarse_).emplace(due_[id], id);
    latest_[id] = infinity;
    if (k >= 0) {
        latest_[id] = cell.last + std::min(options_.max_step, cell.committed_bound);
        limits_.emplace(latest_[id], id);
    }
}

void GwClock::event(double t, std::vector<int> ids, std::optional<double> external_through,
                    Owner& owner, std::set<int>& landing) {
    uniqueIds(ids);
    std::vector<int> edges, affected;
    std::vector<Interval> cell_intervals, face_intervals;
    for (int i : ids) {
        const auto& c = cells_[i];
        if (t == c.last)
            continue; // an explicit finish after a check at the same time
        if (!(t > c.last) || exceeds(t, c.last + std::min(options_.max_step, c.committed_bound)))
            throw std::logic_error("GW clock: accepted interval exceeds previous committed bound");
        cell_intervals.push_back({i, c.last, t});
        edges.insert(edges.end(), incident_[i].begin(), incident_[i].end());
        affected.push_back(i);
    }
    if (cell_intervals.empty())
        return;
    uniqueIds(edges);
    for (int e : edges) {
        if (face_last_[e] < t)
            face_intervals.push_back({e, face_last_[e], t});
        else if (face_last_[e] != t)
            throw std::logic_error("GW clock: face time went backwards");
        affected.push_back(faces_[e].first);
        affected.push_back(faces_[e].second);
    }
    owner.faces(face_intervals);
    owner.cells(cell_intervals, external_through);
    for (int e : edges)
        face_last_[e] = t;
    for (const auto& c : cell_intervals)
        cells_[c.id].last = t;

    // Observe all new heads before re-evaluating neighbours. A neighbour's
    // *next* rung may tighten, but its previous committed bound is historical.
    uniqueIds(affected);
    for (int i : affected) {
        auto& c = cells_[i];
        const double bound = owner.stability(i);
        int k = select(bound, owner.accuracy(i));
        if (c.last == t) {
            c.committed_bound = bound;
            // Loosen only when the nominal deadline is shared with the slower
            // rung. A delayed coupled observation need not itself be on-grid.
            if (k > c.rung &&
                (due_[i] > t || std::fma(std::round((due_[i] - options_.epoch) / period(k)),
                                         period(k), options_.epoch) != due_[i]))
                k = c.rung;
            schedule(i, k, t);
        } else if (k < c.rung) {
            // If the old coarse firing is already owed at the imminent check,
            // preserve it. Substituting a deadline later than the old deadline
            // can skip that firing even though the requested rung is finer.
            if (due_[i] > t)
                schedule(i, k, t);
            else if (k < 0) {
                landing.insert(i);
                schedule(i, k, t); // honour finer interior ticks and the owed endpoint
            }
        }
    }
}

void GwClock::check(double end, Owner& owner, bool gw_only, bool finish,
                    const std::function<void()>& publish) {
    if (!std::isfinite(end) || end < time_)
        throw std::invalid_argument("GW clock: invalid check time");
    if (!gw_only && exceeds(end, time_ + options_.surface_max_step))
        throw std::invalid_argument("GW clock: producer check exceeds the authored surface step");
    std::set<int> subcycled; // one endpoint landing per stiff owner, however many interior ticks
    const auto collect = [](const Queue& queue, double t, std::vector<int>& ids) {
        for (const auto& [due, id] : queue) {
            if (due > t)
                break;
            ids.push_back(id);
        }
    };
    for (;;) {
        const double coarse = coarse_.empty() ? infinity : coarse_.begin()->first;
        // A small authored ceiling can expire before the fixed surface-check
        // allowance. Guard every coarse owner, including owners whose nominal
        // due time is later than the queue's first entry. Visit nominal events
        // chronologically; only already completed producer packets are visible.
        const bool numerical_check = gw_only || exceeds(end, coarse + margin()) ||
                                     (!limits_.empty() && exceeds(end, limits_.begin()->first));
        const double t = std::min(fine_.empty() ? infinity : fine_.begin()->first,
                                  numerical_check ? coarse : infinity);
        if (t >= end)
            break;
        std::vector<int> ids;
        collect(fine_, t, ids);
        if (!gw_only)
            subcycled.insert(ids.begin(), ids.end());
        std::optional<double> through;
        if (numerical_check && coarse == t) {
            collect(coarse_, t, ids);
            if (!gw_only)
                through = time_;
        }
        event(t, std::move(ids), through, owner, subcycled);
    }
    // Stiff owners that subcycled land their last (possibly fractional)
    // interval at the producer check. Otherwise a non-dyadic check sequence
    // could withhold their external packets forever. An unfired slow cell
    // has no such landing. GW-only windows have no endpoint landings.
    if (publish)
        publish();
    std::vector<int> landing(subcycled.begin(), subcycled.end());
    collect(fine_, end, landing);
    collect(coarse_, end, landing);
    owner.early(end, landing);
    if (finish)
        for (int i = 0; i < static_cast<int>(cells_.size()); ++i)
            landing.push_back(i);
    event(end, std::move(landing), (gw_only && !publish) ? std::nullopt : std::optional{end}, owner,
          subcycled);
    time_ = end;
}

void GwClock::land(std::span<const int> cells, Owner& owner) {
    std::set<int> landing;
    event(time_, {cells.begin(), cells.end()}, time_, owner, landing);
}

void GwClock::refresh(Owner& owner) {
    for (int i = 0; i < static_cast<int>(cells_.size()); ++i) {
        const double bound = owner.stability(i);
        if (cells_[i].last == time_)
            cells_[i].committed_bound = bound;
        const int k = select(bound, owner.accuracy(i));
        // Rebuild the hard limit even when an edited bound leaves the rung
        // unchanged. Only tighten a held owner's selected period.
        schedule(i, std::min(k, cells_[i].rung), time_);
    }
}

void GwClock::checkpoint(detail::CompletedSourceArchive& ar) {
    // The clock record accompanies its owner's pending state.
    // Validate identity and all clock fields on a private copy before adoption.
    std::string tag = "Independent groundwater clock v2 separate ceiling";
    auto options = options_;
    ar(tag, options.epoch, options.base_step, options.max_step, options.surface_max_step);
    if (tag != "Independent groundwater clock v2 separate ceiling" ||
        options.epoch != options_.epoch || options.base_step != options_.base_step ||
        options.max_step != options_.max_step ||
        options.surface_max_step != options_.surface_max_step)
        throw std::runtime_error("GW clock: checkpoint options differ");
    auto topology = faces_;
    ar.fixed(topology);
    if (topology != faces_)
        throw std::runtime_error("GW clock: checkpoint topology differs");
    auto t = time_;
    auto cells = cells_;
    auto face_last = face_last_;
    auto count = static_cast<std::uint64_t>(cells.size());
    ar(t, count);
    if (count != cells_.size() || !std::isfinite(t) || t < options_.epoch)
        throw std::runtime_error("GW clock: invalid checkpoint time or shape");
    for (auto& c : cells)
        ar(c.last, c.committed_bound, c.rung);
    ar.fixed(face_last);
    if (!ar.reading())
        return;
    auto restored = *this;
    restored.time_ = t;
    restored.cells_ = std::move(cells);
    restored.face_last_ = std::move(face_last);
    restored.fine_.clear();
    restored.coarse_.clear();
    restored.limits_.clear();
    restored.base_cells_ = 0;
    std::fill(restored.due_.begin(), restored.due_.end(), infinity);
    for (int i = 0; i < static_cast<int>(restored.cells_.size()); ++i) {
        const auto& c = restored.cells_[i];
        if (!std::isfinite(c.last) || c.last < options_.epoch || c.last > t ||
            !(c.committed_bound > 0) || !positive(period(c.rung)) ||
            c.rung > select(c.committed_bound) ||
            exceeds(t, c.last + std::min(c.committed_bound, options_.max_step)))
            throw std::runtime_error("GW clock: invalid checkpoint cell");
        restored.schedule(i, c.rung, t);
    }
    for (std::size_t e = 0; e < faces_.size(); ++e) {
        const auto [l, r] = faces_[e];
        if (restored.face_last_[e] != std::max(restored.cells_[l].last, restored.cells_[r].last))
            throw std::runtime_error("GW clock: invalid checkpoint face interval");
    }
    *this = std::move(restored);
}

} // namespace openswmm::twoD
