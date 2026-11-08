// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
#include "LivePassiveStorageExchange.hpp"
#ifdef OPENSWMM_HAS_2D
#include "../core/CompletedSourceArchive.hpp"
#include "../core/SimulationContext.hpp"
#include "../2d/coupling/NodeCoupling.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace openswmm::quality {
namespace {
using Archive = detail::CompletedSourceArchive;
constexpr std::size_t maxBytes = 64 * 1024 * 1024;
void check(bool ok, const char* message) {
    if (!ok) throw std::invalid_argument(message);
}
std::uint32_t crc(const std::string& bytes) {
    std::uint32_t out = ~std::uint32_t(0);
    for (unsigned char c : bytes) {
        out ^= c;
        for (int bit = 0; bit < 8; ++bit) out = (out >> 1) ^ ((out & 1) ? 0xedb88320u : 0u);
    }
    return ~out;
}
}
void LivePassiveStorageExchange::validateSurface(const twoD::SurfaceStateData& surface) const {
    check(!open_ && !reserved_ && !surface.live_passive_exchange && first_ <= count_ && count_ <= legs_.size(),
          "Live checkpoint requires a sealed accepted owner.");
    const auto& tr = surface.transport;
    check(tr.n_species == 1 && tr.lost_coupling.size() == 1 && tr.gained_coupling.size() == 1 &&
              tr.exch_mass.size() == points_.size() && tr.exch_spill.size() == points_.size(),
          "Live checkpoint surface receipt shape differs.");
    const double total = tr.totalIncludingLedgers(0);
    check(std::isfinite(total) && std::isfinite(initial_total_) &&
              std::abs(total - initial_total_) <= 1e-12 * std::max({1.0, std::abs(total), std::abs(initial_total_)}),
          "Live checkpoint surface pollutant inventory differs from its attachment.");
    std::size_t index = 0;
    for (const auto& receipt : book_.receipts()) {
        check(receipt.authenticated && receipt.proof.si_to_native == factor_,
              "Live checkpoint cannot omit authenticated ownership.");
        check(receipt.proof.points.size() == points_.size(), "Live checkpoint point extent differs.");
        for (std::size_t k = 0; k < points_.size(); ++k) {
            const auto& p = receipt.proof.points[k];
            check(p.point == k && p.cell == points_[k].cell && p.node == points_[k].node,
                  "Live checkpoint point mapping differs.");
        }
        for (const auto& l : receipt.proof.legs) {
            check(index < count_ && legs_[index] == l, "Live checkpoint journal differs from sealed proof.");
            ++index;
        }
    }
    check(index == count_ && next_id_ == book_.last_id_ + 1, "Live checkpoint receipt sequence differs.");
    if (book_.changingVolume()) {
        for (const auto& w : book_.water_publications_) {
            check(w.si_to_native == factor_ && w.points.size() == points_.size(), "Live water publication identity differs.");
            for (std::size_t k = 0; k < points_.size(); ++k)
                check(w.points[k].point == k && w.points[k].cell == points_[k].cell && w.points[k].node == points_[k].node,
                      "Live water publication topology differs.");
        }
    }
    double loss = initial_loss_, gain = initial_gain_, group_loss = 0;
    const Leg* previous = nullptr;
    std::vector<double> drain(points_.size(), 0), spill(points_.size(), 0);
    for (std::size_t i = 0; i < count_; ++i) {
        const auto& l = legs_[i];
        if (previous && (l.batch_start_ms != previous->batch_start_ms || l.batch_end_ms != previous->batch_end_ms ||
                         l.phase != previous->phase || l.offset_s != previous->offset_s)) {
            loss += group_loss; group_loss = 0;
        }
        if (l.drain) group_loss += l.mass_si;
        else gain += l.mass_si;
        if (i >= first_) {
            check(l.batch_start_ms == start_ms_ && l.batch_end_ms == end_ms_ && l.phase == phase_,
                  "Live checkpoint latest physical window differs.");
            if (l.drain) drain.at(l.point) += l.mass_si;
            else spill.at(l.point) += l.water_si;
        }
        previous = &l;
    }
    loss += group_loss;
    check(std::isfinite(loss) && std::isfinite(gain) && loss == tr.lost_coupling[0] &&
              gain == tr.gained_coupling[0] && drain == tr.exch_mass && spill == tr.exch_spill,
          "Live checkpoint proof differs from actual surface ledgers.");
    if (first_ < count_) {
        check(!book_.receipts().empty() && book_.receipts().back().proof.legs.size() == count_ - first_,
              "Live checkpoint latest receipt boundary differs.");
    } else if (!book_.receipts().empty()) {
        check(book_.receipts().back().publication.batch_end_ms <= start_ms_,
              "Live checkpoint empty window overlaps its last receipt.");
    }
}
std::string LivePassiveStorageExchange::checkpoint(const SimulationContext& ctx,
        const twoD::SurfaceStateData& surface) const {
    requireReady();
    check(&ctx == context_ && attached_ms_ == 0, "Live restart requires its initial native attachment.");
    validateSurface(surface);
    for (const auto& b : budgets_)
        book_.validatePool(ctx, b.node, {b.volume, b.depth, b.concentration}, false);
    auto book = book_.checkpoint(ctx);
    Archive state;
    auto factor = factor_, start = start_ms_, end = end_ms_, loss = initial_loss_, gain = initial_gain_;
    auto initial = initial_total_;
    auto phase = phase_; auto opened = ever_opened_; auto id = next_id_; auto attached = attached_ms_;
    std::uint64_t capacity = legs_.size(), count = count_, first = first_, n = points_.size();
    state(factor, capacity, count, first, start, end, phase, opened, id, loss, gain, initial, attached, n);
    for (auto point : points_) state(point.cell, point.node);
    state(book);
    auto payload = state.bytes();
    std::string magic = "OpenSWMM live passive owner";
    std::uint32_t version = book_.exactSlices() ? (preflight_history_ ? 13 : 12) : book_.retainsWaterAggregates() ? (preflight_history_ ? 11 : 10) :
        book_.changingVolume() ? (preflight_history_ ? 9 : 8) : book_.arithmetic() == PassiveStorageArithmetic::RETAIN_EXACT_HALVES ? (preflight_history_ ? 7 : 6) :
        preflight_history_ ? 5 :
        book_.arithmetic() == PassiveStorageArithmetic::RETAIN_POINT_AGGREGATES ? 4 :
        book_.arithmetic() == PassiveStorageArithmetic::RETAIN_AGGREGATES ? 3 :
        book_.arithmetic() == PassiveStorageArithmetic::RETAIN_ROUNDOFF ? 2 : 1;
    std::uint32_t endian = 0x01020304, sum = crc(payload);
    Archive file; file(magic, version, endian, sum, payload);
    check(file.bytes().size() <= maxBytes, "Live owner checkpoint exceeds its byte limit.");
    return file.bytes();
}
std::unique_ptr<LivePassiveStorageExchange> LivePassiveStorageExchange::prepareRestore(
        const SimulationContext& ctx, const twoD::SurfaceStateData& surface, const std::string& bytes) const {
    requireReady();
    check(!ever_opened_ && !open_ && !reserved_ && !book_.deliveryFrozen() && book_.deliveries_.empty() &&
              count_ == 0 && book_.receipts().empty() && attached_ms_ == 0,
          "Live restore requires a pristine initial owner.");
    check(!bytes.empty() && bytes.size() <= maxBytes, "Live owner checkpoint exceeds its byte limit.");
    Archive envelope(bytes);
    std::string magic, payload;
    std::uint32_t version, endian, sum;
    envelope(magic, version, endian, sum, payload); envelope.finish();
    const std::uint32_t expected_version = book_.exactSlices() ? (preflight_history_ ? 13 : 12) : book_.retainsWaterAggregates() ? (preflight_history_ ? 11 : 10) :
        book_.changingVolume() ? (preflight_history_ ? 9 : 8) : book_.arithmetic() == PassiveStorageArithmetic::RETAIN_EXACT_HALVES ? (preflight_history_ ? 7 : 6) :
        preflight_history_ ? 5 :
        book_.arithmetic() == PassiveStorageArithmetic::RETAIN_POINT_AGGREGATES ? 4 :
        book_.arithmetic() == PassiveStorageArithmetic::RETAIN_AGGREGATES ? 3 :
        book_.arithmetic() == PassiveStorageArithmetic::RETAIN_ROUNDOFF ? 2 : 1;
    check(magic == "OpenSWMM live passive owner" && version == expected_version && endian == 0x01020304 && sum == crc(payload),
          "Live owner checkpoint format/checksum differs.");
    Archive state(std::move(payload));
    double factor, start, end, loss, gain, initial, attached;
    Phase phase; bool opened;
    std::uint64_t capacity, count, first, id, n;
    state(factor, capacity, count, first, start, end, phase, opened, id, loss, gain, initial, attached, n);
    check(factor == factor_ && capacity == legs_.size() && count <= capacity && first <= count &&
              n == points_.size() && attached == attached_ms_ && loss == initial_loss_ && gain == initial_gain_ && initial == initial_total_,
          "Live owner checkpoint identity/capacity/baseline differs.");
    check(std::isfinite(start) && std::isfinite(end) && start >= 0 && end >= start && end <= ctx.elapsed_ms &&
              (phase == Phase::ADVANCE || phase == Phase::REPORT || phase == Phase::FINAL) &&
              (opened ? end > start : (start == 0 && end == 0 && count == 0)),
          "Live owner checkpoint physical clock differs.");
    for (const auto& expected : points_) {
        Point point; state(point.cell, point.node);
        check(point.cell == expected.cell && point.node == expected.node, "Live owner checkpoint topology differs.");
    }
    std::string book; state(book); state.finish();
    check(surface.node_coupling != nullptr, "Live restore has no incoming point topology.");
    auto candidate = std::make_unique<LivePassiveStorageExchange>(book_.changingVolume() ? *context_ : ctx,
        surface, *surface.node_coupling, factor_, capacity, book_.arithmetic(), preflight_history_, book_.law(), book_.waterArithmetic());
    candidate->book_.restoreImpl(ctx, book, &initial_pools_);
    candidate->rebind(ctx);
    if (book_.changingVolume())
        for (auto& b : candidate->budgets_) {
            const auto& settled = candidate->book_.pools_.at(b.node);
            b.volume = settled.volume; b.depth = settled.depth; b.concentration = settled.concentration;
        }
    candidate->initial_pools_ = initial_pools_;
    candidate->initial_loss_ = loss; candidate->initial_gain_ = gain; candidate->initial_total_ = initial; candidate->attached_ms_ = attached;
    candidate->start_ms_ = start; candidate->end_ms_ = end; candidate->phase_ = phase;
    candidate->ever_opened_ = opened; candidate->next_id_ = id; candidate->first_ = first;
    for (const auto& receipt : candidate->book_.receipts())
        for (const auto& leg : receipt.proof.legs) {
            check(candidate->count_ < count, "Live owner checkpoint journal capacity differs.");
            candidate->legs_[candidate->count_++] = leg;
        }
    check(candidate->count_ == count, "Live owner checkpoint lost an authenticated leg.");
    candidate->validateSurface(surface);
    return candidate;
}
void LivePassiveStorageExchange::installFrom(LivePassiveStorageExchange& c, const SimulationContext& ctx) noexcept {
    points_.swap(c.points_); budgets_.swap(c.budgets_); legs_.swap(c.legs_); initial_pools_.swap(c.initial_pools_);
    count_ = c.count_; first_ = c.first_; factor_ = c.factor_; start_ms_ = c.start_ms_; end_ms_ = c.end_ms_;
    phase_ = c.phase_; ever_opened_ = c.ever_opened_; next_id_ = c.next_id_;
    initial_loss_ = c.initial_loss_; initial_gain_ = c.initial_gain_; initial_total_ = c.initial_total_; attached_ms_ = c.attached_ms_;
    open_ = reserved_ = false; failure_ = nullptr; attempted_ = {}; history_error_.clear();
    auto& b = book_; auto& s = c.book_;
    b.node_ids_.swap(s.node_ids_); b.pollutant_ids_.swap(s.pollutant_ids_);
    b.receipts_.swap(s.receipts_); b.pools_.swap(s.pools_); b.deliveries_.swap(s.deliveries_);
    b.roundoff_.swap(s.roundoff_);
    b.initial_pools_.swap(s.initial_pools_); b.water_publications_.swap(s.water_publications_);
    b.owner_ = s.owner_ ? &ctx : nullptr;
    b.units_ = s.units_; b.last_id_ = s.last_id_; b.next_start_ms_ = s.next_start_ms_;
    b.remaining_ = s.remaining_; b.attempt_ = s.attempt_; b.failed_ = false;
    b.prepared_.reset(); b.frozen_.reset(); b.pending_water_.reset(); b.funding_.reset(); b.seal_.reset();
    rebind(ctx);
}
}
#endif
