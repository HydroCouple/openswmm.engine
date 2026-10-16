// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
#include "SourceWaterDeliveryBridge.hpp"
#include "core/CompletedSourceArchive.hpp"
#include "core/SimulationContext.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace openswmm::runoff {
namespace {
using detail::CompletedSourceArchive;
std::string sourceModel(const SourceWaterDriver& source) {
    auto copy = source.waterTrial();
    CompletedSourceArchive a; copy.checkpoint(a, true); return a.bytes();
}
void waterRecord(CompletedSourceArchive& a, SourceWaterDelivery& d) {
    a(d.source, d.node, d.start, d.end, d.volume, d.drain);
}
uint64_t digest(const std::vector<SourceWaterDelivery>& records, uint64_t count) {
    uint64_t h = 14695981039346656037ULL;
    for (uint64_t i = 0; i < count; ++i) {
        auto d = records[i]; CompletedSourceArchive a; waterRecord(a, d);
        for (unsigned char byte : a.bytes()) { h ^= byte; h *= 1099511628211ULL; }
    }
    return h;
}
bool nonnegative(double v) { return std::isfinite(v) && v >= 0; }
// Batched surface and engine clocks add the same intervals in different orders.
// Admit only roundoff at their shared horizon, never a physical future interval.
bool past(double time,double horizon) {
    return time<=horizon || time-horizon<=1e-12*std::max(1.,std::max(time,horizon));
}
bool close(double a, double b) {
    return std::isfinite(a) && std::isfinite(b) && std::abs(a-b) <= 1e-11 * (1+std::max(a,b));
}
void add(SourceNodeDelivery& to, const SourceWaterDelivery& d) {
    (d.drain ? to.drain : to.runoff) += d.volume;
    if (!nonnegative(to.runoff) || !nonnegative(to.drain))
        throw std::runtime_error("Source delivery volume overflow.");
}
void nodeTotals(CompletedSourceArchive& a, std::vector<SourceNodeDelivery>& totals) {
    a.sequence(totals, [&](auto& d) { a(d.runoff, d.drain); }, true);
}
}
std::string SourceWaterDeliveryBridge::initialize(const SourceWaterDriver& source) {
    if (initialized_) return "Source delivery bridge is already attached.";
    try {
        const auto& c = source.context();
        if (c.n_pollutants() || !source.pollutantNames().empty())
            return "Source delivery bridge requires water-only sources.";
        SourceWaterDeliveryBridge prepared;
        prepared.source_groups_.resize(c.n_subcatches(), -1);
        prepared.state_.source_ends.resize(source.clocks().groups().size(), 0);
        for (const auto& g : source.clocks().groups())
            if (g.completed_end != 0 || !g.pending.empty())
                return "Source delivery ownership must bind before source advancement.";
        if (!source.deliveries().empty()) return "Source delivery ownership requires empty initial outlet history.";
        for (int sc = 0; sc < c.n_subcatches(); ++sc) {
            const int group = source.clocks().groupForSubcatch(sc);
            prepared.source_groups_[sc] = group;
            if (group < 0) continue;
            const int node = c.subcatches.outlet_node[sc], target = c.subcatches.outlet_subcatch[sc];
            if (node >= c.n_nodes() || (node < 0 && (target < 0 || target == sc)))
                return "Owned source has no resolved hydraulic or connected subcatchment outlet.";
        }
        for (int node = 0; node < c.n_nodes(); ++node) {
            auto name = c.node_names.name_of(node);
            if (name.empty()) return "Source delivery requires named hydraulic destinations.";
            prepared.node_names_.push_back(name);
        }
        prepared.state_.received.resize(c.n_nodes()); prepared.state_.delivered.resize(c.n_nodes()); prepared.state_.last.resize(c.n_nodes());
        prepared.source_model_ = sourceModel(source); prepared.initialized_ = true;
        prepared.validate(prepared.state_, source);
        *this = std::move(prepared); return {};
    } catch (const std::exception& e) { return e.what(); }
}
bool SourceWaterDeliveryBridge::ownsSubcatch(int sc) const {
    return initialized_ && sc >= 0 && static_cast<std::size_t>(sc) < source_groups_.size() && source_groups_[sc] >= 0;
}
std::string SourceWaterDeliveryBridge::validateReceiver(const SimulationContext& c) const {
    if (!initialized_ || static_cast<std::size_t>(c.n_nodes()) != node_names_.size())
        return "Source delivery hydraulic destination shape differs.";
    for (int n = 0; n < c.n_nodes(); ++n)
        if (c.node_names.name_of(n) != node_names_[n]) return "Source delivery hydraulic destination identity differs.";
    return {};
}
void SourceWaterDeliveryBridge::validate(const State& s, const SourceWaterDriver& source) const {
    if (!initialized_ || sourceModel(source) != source_model_ || !validateReceiver(source.context()).empty())
        throw std::runtime_error("Source delivery model or ownership differs.");
    const auto& groups = source.clocks().groups(); const auto& records = source.deliveries();
    if (!nonnegative(s.receiver_end) || s.source_ends.size() != groups.size() ||
        s.received.size() != node_names_.size() || s.delivered.size() != node_names_.size() ||
        s.count > records.size() || s.pending.size() > s.count || s.digest != digest(records, s.count))
        throw std::runtime_error("Source delivery cursor or receiving boundary differs.");
    for (std::size_t g = 0; g < groups.size(); ++g)
        if (!groups[g].pending.empty() || !nonnegative(s.source_ends[g]) ||
            s.source_ends[g] > groups[g].completed_end || !past(s.source_ends[g],s.receiver_end))
            throw std::runtime_error("Source delivery requires completed, past-only group clocks.");
    std::vector<SourceNodeDelivery> received(node_names_.size()), pending(node_names_.size());
    for (uint64_t i = 0; i < s.count; ++i) {
        const auto& d = records[i];
        if (!ownsSubcatch(d.source) || d.node < 0 || static_cast<std::size_t>(d.node) >= node_names_.size() ||
            !nonnegative(d.start) || !std::isfinite(d.end) || d.end <= d.start ||
            d.end > s.source_ends[source_groups_[d.source]] || !nonnegative(d.volume) || d.volume == 0 || !d.mass.empty())
            throw std::runtime_error("Invalid completed source outlet record.");
        add(received[d.node], d);
    }
    const uint64_t delivered_count = s.count-s.pending.size();
    if (s.last_begin > delivered_count || !nonnegative(s.last_start) || s.last_start > s.receiver_end ||
        (s.receiver_end > 0 && s.last_start == s.receiver_end) || s.last.size() != node_names_.size())
        throw std::runtime_error("Invalid last accepted source receiving interval.");
    std::vector<SourceNodeDelivery> last(node_names_.size());
    for (uint64_t i = s.last_begin; i < delivered_count; ++i) {
        if (!past(records[i].end,s.last_start)) throw std::runtime_error("Last source receipt contains future water.");
        add(last[records[i].node], records[i]);
    }
    for (std::size_t n = 0; n < last.size(); ++n) {
        const auto& d = s.last[n];
        const double rate = s.receiver_end > s.last_start ?
            (last[n].runoff+last[n].drain)/(s.receiver_end-s.last_start) : 0;
        if (!close(d.runoff,last[n].runoff) || !close(d.drain,last[n].drain) || !close(d.rate,rate) ||
            !nonnegative(d.runoff) || !nonnegative(d.drain) || !nonnegative(d.rate))
            throw std::runtime_error("Last source receipt differs from its completed outlet prefix.");
    }
    uint64_t ordinal = delivered_count;
    for (const auto& q : s.pending) {
        if (q.ordinal != ordinal++ || !nonnegative(q.available) || q.available != s.receiver_end)
            throw std::runtime_error("Invalid pending source delivery order or availability.");
        const auto& expected = records[q.ordinal]; CompletedSourceArchive a, b;
        auto water = q.water, original = expected; waterRecord(a, water); waterRecord(b, original);
        if (a.bytes() != b.bytes() || !water.mass.empty() || !past(water.end,q.available))
            throw std::runtime_error("Pending source delivery differs from its completed outlet.");
        add(pending[water.node], water);
    }
    for (std::size_t n = 0; n < node_names_.size(); ++n) {
        const auto& r = s.received[n]; const auto& d = s.delivered[n];
        if (!nonnegative(r.runoff) || !nonnegative(r.drain) || !nonnegative(d.runoff) || !nonnegative(d.drain) ||
            r.rate != 0 || d.rate != 0 || !close(r.runoff, received[n].runoff) || !close(r.drain, received[n].drain) ||
            !close(r.runoff, d.runoff+pending[n].runoff) || !close(r.drain, d.drain+pending[n].drain))
            throw std::runtime_error("Source delivery pending/received/delivered volumes do not close.");
    }
}
std::string SourceWaterDeliveryBridge::capture(const SourceWaterDriver& source, double end) {
    if (!initialized_ || trial_ || !std::isfinite(end) || end != state_.receiver_end)
        return "Source capture requires the accepted receiving boundary.";
    try {
        validate(state_, source); auto prepared = state_;
        const auto& groups = source.clocks().groups(); const auto& records = source.deliveries();
        for (std::size_t g = 0; g < groups.size(); ++g) {
            if (!past(groups[g].completed_end,end)) return "Source delivery cannot publish a future completed interval.";
            prepared.source_ends[g] = groups[g].completed_end;
        }
        for (uint64_t i = prepared.count; i < records.size(); ++i) {
            prepared.pending.push_back({i, end, records[i]});
        }
        prepared.count = records.size(); prepared.digest = digest(records, prepared.count);
        // Validate every new record before indexing its hydraulic destination.
        prepared.received.assign(node_names_.size(), {});
        for (const auto& q : records) {
            if (q.node < 0 || static_cast<std::size_t>(q.node) >= node_names_.size())
                return "Completed source outlet has no matching hydraulic destination.";
            add(prepared.received[q.node], q);
        }
        validate(prepared, source); state_ = std::move(prepared); return {};
    } catch (const std::exception& e) { return e.what(); }
}
std::string SourceWaterDeliveryBridge::stage(double start, double end) {
    if (!initialized_ || trial_ || !std::isfinite(start) || !std::isfinite(end) ||
        start != state_.receiver_end || end <= start || !std::isfinite(end-start))
        return "Hydraulic source delivery requires the next finite contiguous receiving interval.";
    try {
        Trial prepared; prepared.end = end; prepared.delivered = state_.delivered;
        prepared.receipts.resize(node_names_.size());
        for (const auto& q : state_.pending) {
            add(prepared.receipts[q.water.node], q.water); add(prepared.delivered[q.water.node], q.water);
        }
        for (auto& receipt : prepared.receipts) {
            receipt.rate = (receipt.runoff + receipt.drain) / (end-start);
            if (!nonnegative(receipt.rate)) return "Hydraulic source delivery rate overflow.";
        }
        trial_ = std::move(prepared); return {};
    } catch (const std::exception& e) { return e.what(); }
}
std::string SourceWaterDeliveryBridge::commit() {
    if (!trial_) return "No staged hydraulic source delivery to accept.";
    state_.last_begin = state_.count-state_.pending.size(); state_.last_start = state_.receiver_end;
    state_.last = std::move(trial_->receipts);
    state_.receiver_end = trial_->end; state_.delivered = std::move(trial_->delivered);
    state_.pending.clear(); trial_.reset(); return {};
}
const std::vector<SourceNodeDelivery>& SourceWaterDeliveryBridge::receipts() const {
    if (!trial_) throw std::logic_error("No staged hydraulic source receipts.");
    return trial_->receipts;
}
void SourceWaterDeliveryBridge::checkpoint(CompletedSourceArchive& a, bool model, const SourceWaterDriver& source) {
    if (!initialized_ || trial_) throw std::runtime_error("Source delivery checkpoint requires an accepted receiving boundary.");
    if (model) {
        if (a.reading()) throw std::runtime_error("Source delivery model identity is write-only.");
        validate(state_, source);
        uint32_t version = 2; a(version, source_model_, node_names_, source_groups_); return;
    }
    State prepared = state_; uint32_t version = 2; a(version);
    if (version != 2) throw std::runtime_error("Unsupported source delivery checkpoint version.");
    a(prepared.count, prepared.digest, prepared.receiver_end); a.fixed(prepared.source_ends);
    a.sequence(prepared.pending, [&](auto& q) { a(q.ordinal, q.available); waterRecord(a, q.water); });
    nodeTotals(a, prepared.received); nodeTotals(a, prepared.delivered);
    a(prepared.last_begin, prepared.last_start);
    a.sequence(prepared.last, [&](auto& d) { a(d.runoff,d.drain,d.rate); }, true);
    validate(prepared, source);
    if (prepared.count != source.deliveries().size())
        throw std::runtime_error("Source delivery checkpoint requires all completed outlets to be published.");
    for (std::size_t g = 0; g < prepared.source_ends.size(); ++g)
        if (prepared.source_ends[g] != source.clocks().groups()[g].completed_end)
            throw std::runtime_error("Source delivery checkpoint requires published group clocks.");
    if (a.reading()) state_ = std::move(prepared);
}
}
