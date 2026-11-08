// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
#include "PassiveStorageExchange.hpp"
#include "QualityRouting.hpp"
#include "../core/CompletedSourceArchive.hpp"
#include "../core/SimulationContext.hpp"
#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

namespace openswmm::quality {
namespace {
using Archive = detail::CompletedSourceArchive;
constexpr std::size_t maxBytes = 64 * 1024 * 1024, maxRecords = 4096, maxReplayWork = 1000000;
const std::string magic = "OpenSWMM passive storage receipt owner";
void check(bool ok, const char* text) {
    if (!ok)
        throw std::invalid_argument(std::string("Passive receipt checkpoint: ") + text);
}
std::uint32_t crc(const std::string& bytes) {
    std::uint32_t value = ~std::uint32_t(0);
    for (unsigned char c : bytes) {
        value ^= c;
        for (int bit = 0; bit < 8; ++bit)
            value = (value >> 1) ^ ((value & 1) ? 0xedb88320u : 0u);
    }
    return ~value;
}
void row(Archive& a, PassiveStorageExchange::Row& r, std::uint32_t version) {
    a(r.point, r.node, r.pollutant, r.units, r.drain_water, r.spill_water, r.drain_mass, r.spill_mass);
    if (version >= 5) {
        for (bool drain : {true, false}) {
            auto& parts = drain ? r.drain_mass_parts : r.spill_mass_parts;
            std::uint64_t count = parts.size(); a(count);
            check(count == 0 || (count >= 2 && count <= NativeAmountSum::capacity), "point material component count differs");
            if (a.reading()) parts.resize(count);
            for (double& part : parts) a(part);
            if (!a.reading()) r.materialAmount(drain); // decoded components are validated after work preflight
        }
    } else check(r.drain_mass_parts.empty() && r.spill_mass_parts.empty(), "earlier format cannot omit point components");
    if (version >= 8) {
        for (bool drain : {true, false}) {
            auto& parts = drain ? r.drain_water_parts : r.spill_water_parts;
            std::uint64_t count = parts.size(); a(count);
            check(count == 0 || (count >= 2 && count <= NativeAmountSum::capacity), "point water component count differs");
            if (a.reading()) parts.resize(count);
            for (double& part : parts) a(part);
            if (!a.reading()) r.waterAmount(drain); // validate decoded components after work preflight
        }
    } else check(r.drain_water_parts.empty() && r.spill_water_parts.empty(), "earlier format cannot omit water components");
}
void proof(Archive& a, PassiveStorageExchange::LiveProof& p, std::size_t& total) {
    a(p.si_to_native);
    std::uint64_t count = p.points.size(); a(count);
    check(count > 0 && count <= maxRecords && count <= 65536 - total, "proof point count exceeds profile");
    total += count;
    if (a.reading()) p.points.resize(count);
    for (auto& point : p.points) a(point.point, point.cell, point.node, point.exchange_si);
    count = p.legs.size(); a(count);
    check(count > 0 && count <= 65536 - total, "proof leg count exceeds profile");
    total += count;
    if (a.reading()) p.legs.resize(count);
    for (auto& l : p.legs)
        a(l.point, l.cell, l.node, l.drain, l.applied, l.water_si, l.mass_si, l.water_native,
          l.mass_native, l.before, l.after, l.sample, l.batch_start_ms, l.batch_end_ms, l.phase,
          l.volume_before, l.volume_after, l.offset_s, l.duration_s, l.withdrawal_si);
}
void receipt(Archive& a, PassiveStorageExchange::Receipt& r, std::size_t& total,
             std::uint32_t version, std::size_t& proof_total) {
    auto& p = r.publication;
    a(r.id, p.phase, p.capture_ms, p.batch_start_ms, p.batch_end_ms, p.eligible_ms, p.remaining, p.generation);
    std::uint64_t count = r.accepted.size();
    a(count);
    check(count > 0 && count <= maxRecords - total, "receipt row count exceeds profile");
    total += count;
    if (a.reading()) {
        r.accepted.resize(count); r.remaining.resize(count); r.samples.resize(count);
    }
    check(r.remaining.size() == count && r.samples.size() == count, "receipt column shapes differ");
    for (std::size_t i = 0; i < count; ++i) {
        row(a, r.accepted[i], version); row(a, r.remaining[i], version);
        auto& s = r.samples[i];
        a(s.volume, s.depth, s.concentration);
    }
    if (version >= 2) {
        a(r.authenticated);
        if (r.authenticated) proof(a, r.proof, proof_total);
    } else check(!r.authenticated, "legacy owner cannot omit live proof");
    if (version >= 6) {
        a(r.half_state);
        check(r.half_state <= 1 && (r.half_state == 0 || r.authenticated), "invalid exact-half receipt progress");
    } else check(r.half_state == 0, "earlier format cannot omit half progress");
    if (version >= 9) {
        check(r.authenticated && r.half_state == 0, "exact slice authentication/progress differs");
        for (auto* column : {&r.remaining_water_legs, &r.remaining_mass_legs}) {
            std::uint64_t size = column->size(); a(size);
            check(size == r.proof.legs.size(), "exact slice leg column extent differs");
            if (a.reading()) column->resize(size);
            for (double& value : *column) {
                a(value); check(std::isfinite(value) && value >= 0, "invalid exact slice leg remainder");
            }
        }
    } else check(r.remaining_water_legs.empty() && r.remaining_mass_legs.empty(), "earlier format cannot omit leg remainders");
}
bool pendingRows(const std::vector<PassiveStorageExchange::Receipt>& receipts) {
    for (const auto& r : receipts)
        for (const auto& q : r.remaining)
            if (q.drain_water != 0 || q.spill_water != 0 || q.drain_mass != 0 || q.spill_mass != 0 ||
                !q.drain_mass_parts.empty() || !q.spill_mass_parts.empty() ||
                !q.drain_water_parts.empty() || !q.spill_water_parts.empty())
                return true;
    return false;
}
void archiveWaterState(Archive& a, auto& s, std::size_t nodes) {
    for (auto* values : {&s.volume, &s.old_volume, &s.depth, &s.old_depth, &s.inflow, &s.outflow,
                        &s.old_net, &s.old_lateral, &s.old_inflow, &s.rate, &s.queue}) {
        std::uint64_t count = values->size(); a(count);
        check(count == nodes && count <= maxRecords, "water observation column shape differs");
        if (a.reading()) values->resize(count);
        for (double& v : *values) { a(v); check(std::isfinite(v), "nonfinite water observation"); }
    }
}
void waterPublication(Archive& a, auto& w, std::size_t nodes, std::size_t& point_total) {
    auto& p = w.publication;
    a(p.phase, p.capture_ms, p.batch_start_ms, p.batch_end_ms, p.eligible_ms, p.remaining, p.generation,
      w.receipt, w.dt, w.remaining_after, w.si_to_native);
    std::uint64_t count = w.points.size(); a(count);
    check(count > 0 && count <= maxRecords && count <= 65536 - point_total, "water publication point count exceeds profile");
    point_total += count;
    if (a.reading()) w.points.resize(count);
    for (auto& point : w.points) a(point.point, point.cell, point.node, point.exchange_si);
    for (auto* values : {&w.before, &w.after}) {
        count = values->size(); a(count); check(count == nodes, "water publication pool extent differs");
        if (a.reading()) values->resize(count);
        for (double& v : *values) { a(v); check(std::isfinite(v), "nonfinite water publication"); }
    }
    count = w.booking_roundoff.size(); a(count); check(count == nodes, "water booking roundoff extent differs");
    if (a.reading()) w.booking_roundoff.resize(count);
    for (auto& sum : w.booking_roundoff) {
        std::uint64_t size = sum.parts().size(); a(size);
        check(size <= NativeAmountSum::capacity, "water booking roundoff expansion exceeds profile");
        std::vector<double> parts;
        if (a.reading()) {
            parts.resize(size);
            for (double& part : parts) { a(part); check(std::isfinite(part) && part != 0, "invalid booking roundoff part"); sum.add(part); }
            check(sum.parts().size() == parts.size() && std::equal(parts.begin(), parts.end(), sum.parts().begin()),
                  "noncanonical booking roundoff");
        } else for (double part : sum.parts()) a(part);
    }
}
} // namespace

PassiveStorageExchange::ReplaySize PassiveStorageExchange::replaySize(std::size_t deliveries) const {
    ReplaySize size; size.deliveries = deliveries; size.receipts = receipts_.size();
    std::set<int> nodes;
    for (const auto& receipt : receipts_) {
        for (const auto& r : receipt.accepted) { ++size.rows; nodes.insert(r.node); }
        size.proof_items += receipt.proof.points.size() + receipt.proof.legs.size();
        size.proof_legs += receipt.proof.legs.size();
        for (const auto* column : {&receipt.accepted, &receipt.remaining})
            for (const auto& r : *column) {
                size.material_parts += r.drain_mass_parts.size() + r.spill_mass_parts.size();
                size.water_parts += r.drain_water_parts.size() + r.spill_water_parts.size();
            }
    }
    if (arithmetic_ == PassiveStorageArithmetic::RETAIN_EXACT_HALVES || exactSlices()) {
        std::size_t bound = 0, water_bound = 0;
        for (const auto& receipt : receipts_) {
            std::map<std::pair<std::uint64_t, bool>, std::size_t> counts;
            for (const auto& row : receipt.accepted) {
                counts.emplace(std::make_pair(row.point, true), 0);
                counts.emplace(std::make_pair(row.point, false), 0);
            }
            for (const auto& leg : receipt.proof.legs) {
                const auto it = counts.find({leg.point, leg.drain});
                check(it != counts.end(), "half proof point differs from receipt rows");
                ++it->second;
            }
            for (const auto& row : receipt.accepted) {
                bound += row.drain_mass_parts.size() + row.spill_mass_parts.size();
                water_bound += row.drain_water_parts.size() + row.spill_water_parts.size();
            }
            for (const auto& row : receipt.remaining) {
                for (bool drain : {true, false}) {
                    const auto& parts = drain ? row.drain_mass_parts : row.spill_mass_parts;
                    const double scalar = drain ? row.drain_mass : row.spill_mass;
                    const double water = drain ? row.drain_water : row.spill_water;
                    const auto& water_parts = drain ? row.drain_water_parts : row.spill_water_parts;
                    if (water != 0 || scalar != 0 || !parts.empty() || !water_parts.empty()) {
                        const auto it = counts.find({row.point, drain});
                        check(it != counts.end(), "half remaining point differs from accepted rows");
                        const auto count = it->second;
                        if (count > 1) bound += std::min(count, NativeAmountSum::capacity);
                        if (retainsWaterAggregates() && count > 1) water_bound += std::min(count, NativeAmountSum::capacity);
                    }
                }
            }
        }
        // Cover both decoded canonical validation and any later per-leg regrouping.
        size.material_parts = std::max(size.material_parts, bound);
        size.water_parts = std::max(size.water_parts, water_bound);
    }
    size.nodes = changingVolume() ? initial_pools_.size() : nodes.size();
    size.water_publications = water_publications_.size();
    for (const auto& p : water_publications_) size.water_points += p.points.size();
    return size;
}
void PassiveStorageExchange::checkReplaySize(const SimulationContext& ctx, const ReplaySize& size,
                                            PassiveStorageArithmetic arithmetic) {
    const auto nn = std::size_t(ctx.n_nodes());
    check(nn <= maxRecords && size.deliveries <= maxRecords && size.receipts <= maxRecords && size.rows <= maxRecords,
          "detached validation model/history exceeds profile");
    check(size.proof_items <= 65536, "proof point/leg count exceeds profile");
    check(size.water_publications <= maxRecords && size.water_points <= 65536, "water history exceeds profile");
    std::size_t budget = maxReplayWork;
    auto charge = [&](std::initializer_list<std::size_t> factors) {
        std::size_t cost = 1;
        for (const auto value : factors) {
            if (value == 0) return;
            check(value <= budget / cost, "detached validation work exceeds staged archive profile");
            cost *= value;
        }
        budget -= cost;
    };
    charge({size.deliveries, nn + 1});
    charge({size.water_publications, size.nodes, 20});
    charge({size.water_points, size.nodes, 4});
    // Bound canonical expansion decoding before reading any water history.
    charge({size.water_publications, size.nodes, NativeAmountSum::capacity, NativeAmountSum::capacity, 8});
    if (size.water_publications) charge({size.proof_legs, size.nodes, NativeAmountSum::capacity, 4});
    charge({size.rows, size.rows});
    charge({size.receipts, size.rows, size.nodes});
    charge({size.deliveries, size.rows, size.nodes, 4});
    charge({size.proof_items, 16});
    charge({size.receipts, size.proof_items});
    if (arithmetic != PassiveStorageArithmetic::EXACT)
        charge({size.deliveries + size.receipts, size.nodes, NativeAmountSum::capacity, 16});
    if (retainsMaterialAggregates(arithmetic))
        charge({size.deliveries + size.receipts, size.nodes, NativeAmountSum::capacity, NativeAmountSum::capacity, 8});
    if (retainsPointAggregates(arithmetic))
        charge({size.deliveries + size.receipts + 1, size.material_parts, NativeAmountSum::capacity, 4});
    charge({size.deliveries + size.receipts + 1, size.water_parts, NativeAmountSum::capacity, 4});
    if (arithmetic == PassiveStorageArithmetic::RETAIN_EXACT_SLICES)
        charge({size.deliveries + size.receipts + 1, size.proof_legs, NativeAmountSum::capacity, 8});
    if (arithmetic == PassiveStorageArithmetic::RETAIN_EXACT_HALVES || arithmetic == PassiveStorageArithmetic::RETAIN_EXACT_SLICES)
        charge({size.deliveries + size.receipts + 1, size.proof_legs, NativeAmountSum::capacity, 8});
    const auto& n = ctx.nodes;
    check(n.volume.size() >= nn && n.old_volume.size() >= nn && n.depth.size() >= nn &&
              n.old_depth.size() >= nn && n.conc.size() >= nn && n.conc_old.size() >= nn,
          "incoming native context shape differs");
}

std::string PassiveStorageExchange::checkpoint(const SimulationContext& ctx) const {
    check(!peer_binding_, "peer funding requires a qualified paired recovery format");
    return checkpointSealed(ctx);
}
std::string PassiveStorageExchange::checkpointSealed(const SimulationContext& ctx) const {
    check(!failed_ && !frozen_ && !prepared_ && !pending_water_ && !funding_ && !seal_, "only sealed accepted owner state can be saved");
    if (!owner_) {
        check(!changingVolume(), "changing-volume checkpoint requires bound initial pools");
        return {};
    }
    validate(ctx);
    check(pendingRows(receipts_) ? ctx.elapsed_ms == next_start_ms_ : ctx.elapsed_ms >= next_start_ms_,
          "native accepted cursor differs");
    check(!pendingRows(receipts_) || ctx.coupling_delivery_remaining == remaining_,
          "native accepted delivery span differs");
    for (const auto& [node, pool] : pools_)
        validatePool(ctx, node, pool, false);
    checkReplaySize(ctx, replaySize(deliveries_.size()), arithmetic_);
    check(receipts_.size() <= maxRecords && pools_.size() <= maxRecords, "owner size exceeds profile");
    Archive a;
    std::uint32_t version = changingVolume() ? (exactSlices() ? 9 : retainsWaterAggregates() ? 8 : 7) : arithmetic_ == PassiveStorageArithmetic::RETAIN_EXACT_HALVES ? 6 :
        arithmetic_ == PassiveStorageArithmetic::RETAIN_POINT_AGGREGATES ? 5 :
        arithmetic_ == PassiveStorageArithmetic::RETAIN_AGGREGATES ? 4 :
        arithmetic_ == PassiveStorageArithmetic::RETAIN_ROUNDOFF ? 3 :
        (std::any_of(receipts_.begin(), receipts_.end(), [](const auto& r) { return r.authenticated; }) ? 2 : 1);
    auto nodes = node_ids_, pollutants = pollutant_ids_;
    auto units = units_;
    auto id = last_id_;
    double next = next_start_ms_, span = remaining_, actual_time = ctx.elapsed_ms;
    double actual_span = ctx.coupling_delivery_remaining;
    auto attempt = attempt_;
    a(nodes, pollutants, units, id, next, span, actual_time, actual_span,
      attempt.start_ms, attempt.dt, attempt.remaining_start);
    if (changingVolume()) {
        std::uint64_t n = initial_pools_.size(); a(n);
        for (const auto& [node, sample] : initial_pools_) { auto key = node; auto pool = sample; a(key, pool.volume, pool.depth, pool.concentration); }
    }
    std::uint64_t count = receipts_.size(); a(count);
    std::size_t total = 0, proof_total = 0;
    for (auto r : receipts_)
        receipt(a, r, total, version, proof_total);
    count = deliveries_.size(); a(count);
    for (auto d : deliveries_) {
        a(d.start_ms, d.dt, d.remaining_start, d.remaining_after, d.cutoff, d.frozen);
        if (changingVolume()) {
            a(d.drained_span);
            archiveWaterState(a, d.before, initial_pools_.size()); archiveWaterState(a, d.assembled, initial_pools_.size());
            archiveWaterState(a, d.received, initial_pools_.size());
        }
    }
    if (changingVolume()) {
        count = water_publications_.size(); a(count);
        std::size_t points = 0;
        for (auto w : water_publications_) waterPublication(a, w, initial_pools_.size(), points);
    }
    count = pools_.size(); a(count);
    for (const auto& [node, pool] : pools_) {
        auto n = node; auto p = pool;
        a(n, p.volume, p.depth, p.concentration);
    }
    if (version >= 3) {
        count = roundoff_.size(); a(count);
        check(count <= maxRecords, "retained roundoff pool count exceeds profile");
        for (const auto& [node, amount] : roundoff_) {
            check(pools_.count(node) && !amount.zero(), "retained roundoff has no bound native pool");
            auto n = node; std::uint64_t parts = amount.parts().size(); a(n, parts);
            for (double part : amount.parts()) a(part);
        }
    }
    std::string payload = a.bytes(), name = changingVolume() ? "OpenSWMM changing-volume storage receipt owner" : magic;
    std::uint32_t endian = 0x01020304, sum = crc(payload);
    Archive out;
    out(name, version, endian, sum, payload);
    check(out.bytes().size() <= maxBytes, "owner archive exceeds byte limit");
    return out.bytes();
}

void PassiveStorageExchange::validateChangingReplay(const SimulationContext& ctx) const {
    check(!initial_pools_.empty() && pools_.size() == initial_pools_.size(), "changing-volume pool ownership differs");
    SimulationContext trial;
    for (const auto& id : node_ids_) trial.node_names.add(id);
    for (const auto& id : pollutant_ids_) trial.pollutant_names.add(id);
    trial.allocate_objects(); trial.options = ctx.options; trial.pollutants = ctx.pollutants;
    std::vector<int> nodes;
    for (const auto& [node, sample] : initial_pools_) {
        check(node >= 0 && node < trial.n_nodes(), "unknown initial changing-volume pool");
        trial.node_subtypes.set_node_type(trial.nodes, node, NodeType::STORAGE);
        trial.nodes.volume[node] = trial.nodes.old_volume[node] = sample.volume;
        trial.nodes.depth[node] = trial.nodes.old_depth[node] = sample.depth;
        trial.nodes.conc[node] = trial.nodes.conc_old[node] = sample.concentration;
        nodes.push_back(node);
    }
    PassiveStorageExchange replay(arithmetic_, law_, water_arithmetic_);
    replay.bindChangingVolumePools(trial, nodes);
    QualitySolver solver; solver.init(trial.n_nodes(), 0, 1);
    std::size_t receipt_index = 0, publication_index = 0;
    const auto publish = [&](const WaterPublication& w) {
        const auto& p = w.publication;
        check(p.generation == replay.deliveries_.size() && p.capture_ms == trial.elapsed_ms &&
                  p.remaining == trial.coupling_delivery_remaining && w.before == replay.waterState(trial).queue,
              "historical water publication cursor/queue differs");
        LiveProof proof; proof.si_to_native = w.si_to_native; proof.points = w.points;
        if (w.receipt) {
            check(receipt_index < receipts_.size(), "water publication has no material receipt");
            const auto& r = receipts_[receipt_index++];
            check(r.id == w.receipt && r.authenticated && r.publication == p &&
                      r.proof.points == w.points && r.proof.si_to_native == w.si_to_native,
                  "water/material publication provenance differs");
            proof = r.proof;
            if (exactSlices()) {
                replay.grantFunding(trial, r.id, p.phase, p.batch_start_ms, p.batch_end_ms, proof.legs.size());
                for (auto leg : proof.legs) if (!leg.drain) {
                    leg.applied = false;
                    check(replay.spendFunding(trial, leg), "historical spill exceeds its pre-credit allowance");
                }
            }
            replay.reserveAuthenticated(trial, r.id, r.accepted, p.phase, p.batch_start_ms, p.batch_end_ms, proof);
            const auto& accepted = replay.seal_ ? replay.seal_->receipts.back() : replay.receipts_.back();
            check(accepted.samples == r.samples && accepted.accepted == r.accepted && accepted.publication == r.publication,
                  "changing-volume settled donor/publication sample differs");
        } else {
            for (const auto& point : w.points)
                check(initial_pools_.count(point.node) && point.exchange_si == 0, "unproved water-only publication");
            if (exactSlices())
                replay.grantFunding(trial, replay.last_id_ + 1, p.phase, p.batch_start_ms, p.batch_end_ms, 0);
        }
        replay.beginWaterPublication(trial, proof, p.phase, p.batch_start_ms, p.batch_end_ms, w.dt);
        for (std::size_t k = 0; k < nodes.size(); ++k) trial.nodes.coupling_queue[nodes[k]] = w.after.at(k);
        trial.coupling_delivery_remaining = w.remaining_after;
        replay.finishWaterPublication(trial);
        check(replay.water_publications_.back() == w, "native water publication/booking roundoff differs");
    };
    for (std::size_t step = 0; step <= deliveries_.size(); ++step) {
        while (publication_index < water_publications_.size() &&
               water_publications_[publication_index].publication.generation == step &&
               water_publications_[publication_index].publication.phase != Phase::ADVANCE)
            publish(water_publications_[publication_index++]);
        if (step == deliveries_.size()) break;
        const auto& d = deliveries_[step];
        check(d.frozen && d.start_ms == trial.elapsed_ms && d.remaining_start == trial.coupling_delivery_remaining,
              "changing-volume delivery cursor/span differs");
        trial.nodes.save_lat_qual_state();
        check(replay.waterState(trial) == d.before, "changing-volume water history is discontinuous");
        replay.freezeDelivery(trial, d.dt);
        replay.installWaterState(trial, d.assembled);
        trial.coupling_delivery_remaining = d.drained_span;
        replay.observeDeliveryDrain(trial);
        // The observed native hydraulic transition is an input to material
        // replay. Check its saved memory and settled anchor; do not solve water
        // a second time or replace its volume by a delivery-queue difference.
        auto routed = d.received; routed.queue = d.assembled.queue;
        replay.installWaterState(trial, routed);
        replay.validateWaterTransition(trial);
        while (publication_index < water_publications_.size() &&
               water_publications_[publication_index].publication.generation == step) {
            check(water_publications_[publication_index].publication.phase == Phase::ADVANCE,
                  "historical water publication phase differs");
            publish(water_publications_[publication_index++]);
        }
        check(replay.waterState(trial) == d.received, "post-producer native water differs");
        solver.execute(trial, d.dt, replay);
        check(replay.deliveries_.back() == d, "changing-volume delivery observation differs");
        trial.elapsed_ms = d.start_ms + 1000.0 * d.dt;
    }
    check(receipt_index == receipts_.size() && publication_index == water_publications_.size() &&
              replay.receipts_ == receipts_ && replay.pools_ == pools_ && replay.roundoff_ == roundoff_ &&
              replay.last_id_ == last_id_ && replay.remaining_ == remaining_ && replay.next_start_ms_ == next_start_ms_ &&
              replay.attempt_.start_ms == attempt_.start_ms && replay.attempt_.dt == attempt_.dt &&
              replay.attempt_.remaining_start == attempt_.remaining_start && ctx.elapsed_ms == trial.elapsed_ms &&
              ctx.coupling_delivery_remaining == trial.coupling_delivery_remaining && waterState(ctx) == replay.waterState(trial),
          "changing-volume owner or incoming hydraulic history differs from replay");
    for (int node : nodes)
        check(ctx.nodes.conc[node] == trial.nodes.conc[node] && ctx.nodes.conc_old[node] == trial.nodes.conc_old[node],
              "changing-volume incoming current/old concentration differs from replay");
}

void PassiveStorageExchange::restore(const SimulationContext& ctx, const std::string& bytes) {
    restoreImpl(ctx, bytes, nullptr);
}
void PassiveStorageExchange::restoreImpl(const SimulationContext& ctx, const std::string& bytes,
                                        const std::map<int, Sample>* closed_initial) {
    check(!peer_binding_, "peer funding requires a qualified paired recovery format");
    restoreSealed(ctx, bytes, closed_initial);
}
void PassiveStorageExchange::restoreSealed(const SimulationContext& ctx, const std::string& bytes,
                                          const std::map<int, Sample>* closed_initial) {
    check((!owner_ || changingVolume()) && !failed_ && !frozen_ && !prepared_ && !pending_water_ && !funding_ && !seal_ &&
              receipts_.empty() && deliveries_.empty() && water_publications_.empty(),
          "restore requires a pristine owner");
    if (bytes.empty()) {
        check(!changingVolume(), "changing-volume owner cannot omit its initial anchors");
        if (closed_initial)
            for (const auto& [node, sample] : *closed_initial) {
                validatePool(ctx, node, sample, false);
                check(ctx.nodes.conc_old[node] == sample.concentration, "closed live initial old concentration differs");
            }
        return;
    }
    check(bytes.size() <= maxBytes, "owner archive exceeds byte limit");
    Archive envelope(bytes);
    std::string name, payload;
    std::uint32_t version, endian, sum;
    envelope(name, version, endian, sum, payload); envelope.finish();
    check((changingVolume() ? name == "OpenSWMM changing-volume storage receipt owner" && version == (exactSlices() ? 9u : retainsWaterAggregates() ? 8u : 7u) :
          name == magic && ((version <= 2 && version >= 1 && arithmetic_ == PassiveStorageArithmetic::EXACT) ||
              (version == 3 && arithmetic_ == PassiveStorageArithmetic::RETAIN_ROUNDOFF) ||
              (version == 4 && arithmetic_ == PassiveStorageArithmetic::RETAIN_AGGREGATES) ||
              (version == 5 && arithmetic_ == PassiveStorageArithmetic::RETAIN_POINT_AGGREGATES) ||
              (version == 6 && arithmetic_ == PassiveStorageArithmetic::RETAIN_EXACT_HALVES))) &&
              endian == 0x01020304 && sum == crc(payload),
          "owner format/version/checksum differs");
    PassiveStorageExchange saved(arithmetic_, law_, water_arithmetic_);
    Archive a(std::move(payload));
    double actual_time, actual_span;
    a(saved.node_ids_, saved.pollutant_ids_, saved.units_, saved.last_id_, saved.next_start_ms_,
      saved.remaining_, actual_time, actual_span,
      saved.attempt_.start_ms, saved.attempt_.dt, saved.attempt_.remaining_start);
    check(saved.node_ids_ == ctx.node_names.names() && saved.pollutant_ids_ == ctx.pollutant_names.names() &&
              ctx.pollutants.units.size() == 1 && saved.units_ == ctx.pollutants.units[0],
          "native identity/ordering/basis differs");
    check(std::isfinite(actual_time) && std::isfinite(actual_span) &&
              actual_time == ctx.elapsed_ms && actual_span == ctx.coupling_delivery_remaining,
          "incoming native accepted clock/span differs");
    std::uint64_t count;
    if (changingVolume()) {
        a(count); check(count > 0 && count <= maxRecords && count <= std::size_t(ctx.n_nodes()), "initial changing-volume pool extent differs");
        for (std::uint64_t k = 0; k < count; ++k) {
            int node; Pool pool; a(node, pool.volume, pool.depth, pool.concentration);
            check(saved.initial_pools_.emplace(node, pool).second, "duplicate initial changing-volume pool");
        }
        check(!closed_initial || saved.initial_pools_ == *closed_initial, "changing-volume initial model anchors differ");
        check(initial_pools_.empty() || saved.initial_pools_ == initial_pools_, "changing-volume target initial anchors differ");
    }
    a(count); check(count <= maxRecords, "receipt count exceeds profile");
    saved.receipts_.resize(count);
    std::size_t total = 0, proof_total = 0;
    for (auto& r : saved.receipts_)
        receipt(a, r, total, version, proof_total);
    a(count); check(count <= maxRecords, "delivery count exceeds profile");
    checkReplaySize(ctx, saved.replaySize(count), arithmetic_);
    for (const auto& receipt : saved.receipts_)
        for (const auto* rows : {&receipt.accepted, &receipt.remaining})
            for (const auto& r : *rows) {
                r.materialAmount(true); r.materialAmount(false);
                r.waterAmount(true); r.waterAmount(false);
            }
    saved.deliveries_.resize(count);
    for (auto& d : saved.deliveries_) {
        a(d.start_ms, d.dt, d.remaining_start, d.remaining_after, d.cutoff, d.frozen);
        if (changingVolume()) {
            a(d.drained_span);
            archiveWaterState(a, d.before, saved.initial_pools_.size()); archiveWaterState(a, d.assembled, saved.initial_pools_.size());
            archiveWaterState(a, d.received, saved.initial_pools_.size());
        }
    }
    if (changingVolume()) {
        a(count); auto size = saved.replaySize(saved.deliveries_.size()); size.water_publications = count;
        checkReplaySize(ctx, size, arithmetic_);
        saved.water_publications_.resize(count);
        std::size_t points = 0;
        for (auto& w : saved.water_publications_) waterPublication(a, w, saved.initial_pools_.size(), points);
        checkReplaySize(ctx, saved.replaySize(saved.deliveries_.size()), arithmetic_);
    }
    a(count); check(count <= maxRecords, "active pool count exceeds profile");
    for (std::uint64_t i = 0; i < count; ++i) {
        int node; Pool pool;
        a(node, pool.volume, pool.depth, pool.concentration);
        check(saved.pools_.emplace(node, pool).second, "duplicate active pool");
    }
    if (version >= 3) {
        a(count); check(count <= maxRecords, "retained roundoff pool count exceeds profile");
        for (std::uint64_t i = 0; i < count; ++i) {
            int node; std::uint64_t parts; a(node, parts);
            check(saved.pools_.count(node) && parts > 0 && parts <= NativeAmountSum::capacity,
                  "retained roundoff binding/component count differs");
            std::vector<double> components(parts);
            NativeAmountSum amount;
            for (double& part : components) {
                a(part); check(std::isfinite(part) && part != 0, "nonfinite/zero roundoff component");
                amount.add(part);
            }
            check(amount.parts().size() == components.size() &&
                      std::equal(components.begin(), components.end(), amount.parts().begin()),
                  "noncanonical retained roundoff");
            check(saved.roundoff_.emplace(node, amount).second, "duplicate retained roundoff pool");
        }
    }
    a.finish();
    saved.owner_ = &ctx;
    saved.validate(ctx);
    for (const auto& [node, pool] : saved.pools_)
        saved.validatePool(ctx, node, pool, false);

    // Reconstruct the historically eligible isolated input, not today's
    // retired-pool forcing/topology. The same native receiver validates the
    // journal. Actual active incoming pools were checked above and are never
    // modified here; this is not a second numerical mixing implementation.
    if (changingVolume()) saved.validateChangingReplay(ctx);
    else {
    SimulationContext trial;
    for (const auto& id : ctx.node_names.names()) trial.node_names.add(id);
    for (const auto& id : ctx.pollutant_names.names()) trial.pollutant_names.add(id);
    trial.allocate_objects();
    trial.options = ctx.options; trial.pollutants = ctx.pollutants;
    if (closed_initial)
        for (const auto& [node, sample] : *closed_initial) {
            check(node >= 0 && node < trial.n_nodes(), "unknown closed live initial pool");
            trial.node_subtypes.set_node_type(trial.nodes, node, NodeType::STORAGE);
            trial.nodes.volume[node] = trial.nodes.old_volume[node] = sample.volume;
            trial.nodes.depth[node] = trial.nodes.old_depth[node] = sample.depth;
            trial.nodes.conc[node] = trial.nodes.conc_old[node] = sample.concentration;
        }
    for (const auto& receipt : saved.receipts_)
        for (const auto& r : receipt.accepted) {
            check(r.node >= 0 && r.node < trial.n_nodes(), "unknown archived pool");
            trial.node_subtypes.set_node_type(trial.nodes, r.node, NodeType::STORAGE);
        }
    PassiveStorageExchange replay(arithmetic_);
    QualitySolver solver; solver.init(trial.n_nodes(), trial.n_links(), 1);
    std::size_t index = 0;
    auto accept = [&](const Receipt& r, bool advance) {
        check(r.publication.generation == replay.deliveries_.size(), "receipt generation differs");
        for (std::size_t i = 0; i < r.accepted.size(); ++i) {
            const int node = r.accepted[i].node;
            check(node >= 0 && node < trial.n_nodes(), "unknown archived pool");
            if (replay.pools_.count(node)) {
                check(replay.pools_.at(node) == r.samples[i], "historical encumbered sample differs");
            } else if (closed_initial) {
                const Sample expected{trial.nodes.volume[node], trial.nodes.depth[node], trial.nodes.conc[node]};
                check(closed_initial->count(node) && expected == r.samples[i],
                      "closed live publication differs from prior native inventory");
            } else {
                const auto& sample = r.samples[i];
                trial.nodes.volume[node] = trial.nodes.old_volume[node] = sample.volume;
                trial.nodes.depth[node] = trial.nodes.old_depth[node] = sample.depth;
                trial.nodes.conc[node] = trial.nodes.conc_old[node] = sample.concentration;
            }
        }
        if (!advance) {
            trial.elapsed_ms = r.publication.capture_ms;
            trial.coupling_delivery_remaining = r.publication.remaining;
        }
        if (r.authenticated) {
            check((r.publication.phase == Phase::ADVANCE) == advance, "authenticated publication order differs");
            replay.reserveAuthenticated(trial, r.id, r.accepted, r.publication.phase,
                r.publication.batch_start_ms, r.publication.batch_end_ms, r.proof);
        } else if (advance) {
            check(r.publication.phase == Phase::ADVANCE, "publication order differs");
            replay.reserveNext(trial, r.id, r.accepted,
                               r.publication.batch_start_ms, r.publication.batch_end_ms);
        } else {
            if (r.publication.phase == Phase::SETTLED)
                replay.reserve(trial, r.id, r.accepted);
            else {
                check(r.publication.phase == Phase::REPORT || r.publication.phase == Phase::FINAL,
                      "unsupported publication phase");
                replay.reserveAtBoundary(trial, r.id, r.accepted, r.publication.phase,
                    r.publication.batch_start_ms, r.publication.batch_end_ms);
            }
        }
        const auto& accepted = replay.receipts_.back();
        check(accepted.accepted == r.accepted && accepted.samples == r.samples &&
                  accepted.publication == r.publication, "historical publication sample/interval differs");
    };
    for (std::size_t step = 0; step <= saved.deliveries_.size(); ++step) {
        while (index < saved.receipts_.size() && saved.receipts_[index].publication.generation == step &&
               saved.receipts_[index].publication.phase != Phase::ADVANCE)
            accept(saved.receipts_[index++], false);
        if (step == saved.deliveries_.size())
            break;
        const auto& d = saved.deliveries_[step];
        trial.elapsed_ms = d.start_ms;
        trial.coupling_delivery_remaining = d.remaining_start;
        trial.nodes.conc_old = trial.nodes.conc;
        if (d.frozen)
            replay.freezeDelivery(trial, d.dt);
        check(d.remaining_after == std::max(0.0, d.remaining_start - d.dt), "native drain history differs");
        trial.coupling_delivery_remaining = d.remaining_after;
        if (d.frozen)
            replay.observeDeliveryDrain(trial);
        while (index < saved.receipts_.size() && saved.receipts_[index].publication.generation == step) {
            check(d.frozen, "ADVANCE has no frozen native handoff");
            accept(saved.receipts_[index++], true);
        }
        solver.execute(trial, d.dt, replay);
        check(replay.deliveries_.back() == d, "native delivery cohort/history differs");
        trial.elapsed_ms = d.start_ms + 1000.0 * d.dt;
    }
    check(index == saved.receipts_.size() && replay.receipts_ == saved.receipts_ &&
              replay.pools_ == saved.pools_ && replay.roundoff_ == saved.roundoff_ && replay.last_id_ == saved.last_id_ &&
              replay.next_start_ms_ == saved.next_start_ms_ && replay.remaining_ == saved.remaining_ &&
              replay.attempt_.start_ms == saved.attempt_.start_ms && replay.attempt_.dt == saved.attempt_.dt &&
              replay.attempt_.remaining_start == saved.attempt_.remaining_start,
          "owner journal/pending state differs from native history");
    check(pendingRows(saved.receipts_) ? actual_time == saved.next_start_ms_ : actual_time >= saved.next_start_ms_,
          "accepted cursor differs from pending owner");
    check(!pendingRows(saved.receipts_) || actual_span == saved.remaining_, "pending native span differs");
    if (closed_initial)
        for (const auto& [node, initial] : *closed_initial) {
            check(ctx.nodes.volume[node] == trial.nodes.volume[node] &&
                      ctx.nodes.depth[node] == trial.nodes.depth[node] &&
                      ctx.nodes.conc[node] == trial.nodes.conc[node] &&
                      ctx.nodes.conc_old[node] == trial.nodes.conc_old[node],
                  "closed live native inventory differs from replay");
            saved.validatePool(ctx, node, {trial.nodes.volume[node], trial.nodes.depth[node], trial.nodes.conc[node]}, false);
        }

    }
    // Nonthrowing installation; no native water, quality or clock is changed.
    node_ids_.swap(saved.node_ids_); pollutant_ids_.swap(saved.pollutant_ids_);
    receipts_.swap(saved.receipts_); pools_.swap(saved.pools_); deliveries_.swap(saved.deliveries_);
    roundoff_.swap(saved.roundoff_);
    initial_pools_.swap(saved.initial_pools_); water_publications_.swap(saved.water_publications_);
    owner_ = &ctx; units_ = saved.units_; last_id_ = saved.last_id_;
    next_start_ms_ = saved.next_start_ms_; remaining_ = saved.remaining_; attempt_ = saved.attempt_;
}
} // namespace openswmm::quality
