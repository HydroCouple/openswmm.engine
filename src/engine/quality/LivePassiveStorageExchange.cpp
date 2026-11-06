// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
#include "LivePassiveStorageExchange.hpp"
#ifdef OPENSWMM_HAS_2D
#include "QualityRouting.hpp"
#include "../core/SimulationContext.hpp"
#include "../2d/coupling/NodeCoupling.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <span>
#include <stdexcept>
#include <set>

namespace openswmm::quality {
namespace {
void require(bool ok, const char* message) {
    if (!ok) throw std::invalid_argument(message);
}
std::size_t journalCapacity(std::size_t capacity) {
    require(capacity > 0 && capacity <= 65536, "Invalid live passive exchange journal capacity.");
    return capacity;
}
void noHeadOverride(const SimulationContext& ctx, int node) {
    const auto& modes = ctx.forcing.node_head_boundary_mode;
    require(modes.empty() || (std::size_t(node) < modes.size() && modes[node] == ForcingMode::NONE),
            "Live passive exchange excludes mapped node head overrides.");
}
// Exact two-sum check. This bounded profile refuses an unrepresentable aggregate.
bool add(double a, double b, double& out) noexcept {
    if (std::abs(a) < std::abs(b)) std::swap(a, b);
    out = a + b;
    return std::isfinite(out) && b - (out - a) == 0;
}
double sum(double a, double b) {
    double out;
    require(add(a, b, out), "Live passive exchange aggregate is not representable.");
    return out;
}
}
LivePassiveStorageExchange::LivePassiveStorageExchange(const SimulationContext& ctx,
        const twoD::SurfaceStateData& state, const std::vector<twoD::CouplingPoint>& points, double factor,
        std::size_t capacity, PassiveStorageArithmetic arithmetic, bool preflight_history, PassiveStorageLaw law)
    : context_(&ctx), legs_(journalCapacity(capacity)), factor_(factor), book_(arithmetic, law), preflight_history_(preflight_history) {
    require(!preflight_history || retainsPointAggregates(arithmetic),
            "History preflight requires exact authenticated point arithmetic.");
    require(capacity > 0 && capacity <= 65536 && !points.empty() && points.size() <= 4096 &&
                std::isfinite(factor) && factor > 0, "Invalid live passive exchange bounds.");
    book_.validate(ctx);
    require(state.transport.n_species == 1 && state.transport.lost_coupling.size() == 1 &&
                state.transport.gained_coupling.size() == 1, "Incomplete live surface ledger.");
    initial_loss_ = state.transport.lost_coupling[0]; initial_gain_ = state.transport.gained_coupling[0];
    initial_total_ = state.transport.totalIncludingLedgers(0);
    attached_ms_ = ctx.elapsed_ms;
    require(std::isfinite(initial_loss_) && initial_loss_ >= 0 && std::isfinite(initial_gain_) && initial_gain_ >= 0,
            "Invalid live attachment ledger.");
    for (const auto& cp : points) {
        require(!cp.is_outfall && cp.cell_idx >= 0 && cp.node_idx >= 0 && cp.node_idx < ctx.n_nodes(),
                "Live passive exchange requires mapped passive storage points.");
        points_.push_back({cp.cell_idx, cp.node_idx});
        if (std::none_of(budgets_.begin(), budgets_.end(), [&](const Budget& b) { return b.node == cp.node_idx; })) {
            const int n = cp.node_idx;
            noHeadOverride(ctx, n);
            book_.validatePool(ctx, n, {ctx.nodes.volume[n], ctx.nodes.depth[n], ctx.nodes.conc[n]}, false);
            budgets_.push_back({n, ctx.nodes.volume[n], ctx.nodes.depth[n], ctx.nodes.conc[n], {}, {}});
            initial_pools_.emplace(n, PassiveStorageExchange::Sample{ctx.nodes.volume[n], ctx.nodes.depth[n], ctx.nodes.conc[n]});
        }
    }
    if (book_.changingVolume()) {
        std::vector<int> nodes;
        for (const auto& b : budgets_) nodes.push_back(b.node);
        book_.bindChangingVolumePools(ctx, nodes);
    }
}
void LivePassiveStorageExchange::requireReady() const {
    if (failed()) throw std::runtime_error(error());
}
void LivePassiveStorageExchange::fail(const char* reason) noexcept {
    if (!failure_) failure_ = reason;
}
bool LivePassiveStorageExchange::preflightHistory(const SimulationContext& ctx, bool next_delivery) noexcept {
    if (!preflight_history_) return true;
    try {
        requireReady();
        require(&ctx == context_ && !open_ && !reserved_ && !book_.frozen_ && !book_.prepared_ &&
                    !book_.pending_water_ && count_ <= legs_.size(),
                "History preflight requires an accepted live boundary.");
        auto size = book_.replaySize(book_.deliveries_.size() + std::size_t(next_delivery));
        if (book_.changingVolume()) {
            // The closed engine path publishes at most one window per step or
            // pending flush. Empty windows retain water observations even when
            // all material has retired or the leg journal has no capacity left.
            ++size.water_publications;
            size.water_points += points_.size();
        }
        const auto remaining = legs_.size() - count_;
        if (remaining != 0) {
            ++size.receipts;
            size.rows += std::min(points_.size(), remaining);
            size.proof_items += points_.size() + remaining;
            size.proof_legs += remaining;
            // A successful expansion has at most one component per actual
            // contributing leg. Accepted/remaining hold at most two copies.
            if (remaining > 1) size.material_parts += 2 * remaining;
        }
        std::set<int> nodes;
        for (const auto& receipt : book_.receipts_)
            for (const auto& r : receipt.accepted) nodes.insert(r.node);
        for (const auto& b : budgets_) nodes.insert(b.node);
        size.nodes = nodes.size();
        PassiveStorageExchange::checkReplaySize(ctx, size, book_.arithmetic());
        history_error_.clear();
        return true;
    } catch (const std::exception& error) {
        try { history_error_ = error.what(); } catch (...) { history_error_.clear(); }
        return false;
    } catch (...) { history_error_.clear(); return false; }
}
void LivePassiveStorageExchange::freeze(const SimulationContext& ctx, double dt) {
    requireReady();
    try {
        require(!open_ && &ctx == context_, "Invalid live delivery owner.");
        for (const auto& b : budgets_)
            book_.validatePool(ctx, b.node, {b.volume, b.depth, b.concentration}, false);
        book_.freezeDelivery(ctx, dt);
    }
    catch (...) { fail("Live passive exchange delivery freeze failed."); throw; }
}
void LivePassiveStorageExchange::observeDrain(const SimulationContext& ctx) {
    requireReady();
    try { require(&ctx == context_, "Wrong live delivery context."); book_.observeDeliveryDrain(ctx); }
    catch (...) { fail("Live passive exchange delivery drain failed."); throw; }
}
void LivePassiveStorageExchange::receive(SimulationContext& ctx, QualitySolver& solver, double dt) {
    requireReady();
    try {
        require(&ctx == context_ && !open_, "Live exchange window remains unsealed or has wrong context.");
        std::vector<bool> active;
        for (const auto& b : budgets_) active.push_back(book_.pools_.count(b.node) != 0);
        solver.execute(ctx, dt, book_);
        for (std::size_t i = 0; i < budgets_.size(); ++i) {
            auto& b = budgets_[i];
            require(active[i] || ctx.nodes.conc[b.node] == b.concentration,
                    "Unencumbered live pool changed outside its native receipt owner.");
            if (book_.changingVolume()) { b.volume = ctx.nodes.volume[b.node]; b.depth = ctx.nodes.depth[b.node]; }
            book_.validatePool(ctx, b.node, {b.volume, b.depth, ctx.nodes.conc[b.node]}, false);
            b.concentration = ctx.nodes.conc[b.node];
        }
    }
    catch (...) { fail("Live passive exchange native receiving failed."); throw; }
}
void LivePassiveStorageExchange::reserveInitial(const SimulationContext& ctx,
        std::vector<PassiveStorageExchange::Row> rows) {
    requireReady();
    require(!ever_opened_ && &ctx == context_, "Initial reservation follows live admission.");
    book_.reserve(ctx, next_id_, std::move(rows));
    ++next_id_;
}
void LivePassiveStorageExchange::beginWindow(const SimulationContext& ctx,
        const twoD::SurfaceStateData& state, double start, double end, Phase phase) {
    requireReady();
    try {
        require(!open_ && &ctx == context_ && std::isfinite(start) && std::isfinite(end) &&
                    start >= 0 && end > start && (phase == Phase::ADVANCE || phase == Phase::REPORT || phase == Phase::FINAL),
                "Invalid live passive exchange window.");
        book_.validate(ctx);
        require(state.transport.n_species == 1 && state.transport.n_pollut == 1 && state.node_row_conc &&
                    state.node_row_conc->size() == std::size_t(ctx.n_nodes()), "Invalid live pollutant publication.");
        for (const auto& [node, pool] : book_.pools_)
            book_.validatePool(ctx, node, pool, false);
        require(state.nodes_1d == &ctx.nodes && state.node_coupling && state.node_coupling->size() == points_.size(),
                "Live hydraulic point owner differs.");
        for (std::size_t k = 0; k < points_.size(); ++k)
            require((*state.node_coupling)[k].cell_idx == points_[k].cell &&
                        (*state.node_coupling)[k].node_idx == points_[k].node && !(*state.node_coupling)[k].is_outfall,
                    "Live hydraulic point mapping changed.");
        for (auto& b : budgets_) {
            const int n = b.node;
            noHeadOverride(ctx, n);
            book_.validatePool(ctx, n, {b.volume, b.depth, b.concentration}, false);
            require((*state.node_row_conc)[n] == ctx.nodes.conc[n], "Live node publication differs from actual sample.");
            if (!book_.changingVolume()) { b.volume = ctx.nodes.volume[n]; b.depth = ctx.nodes.depth[n]; }
            b.concentration = ctx.nodes.conc[n];
            b.sealed = {}; b.open = {};
            if (retainsMaterialAggregates(book_.arithmetic()))
                b.sealed = book_.pendingSpillAmount(n);
            else b.sealed.add(book_.pendingSpill(n));
            require(book_.canFund(n, b.volume * b.concentration, b.sealed), "Existing live spill is no longer funded.");
        }
        for (const auto& p : points_)
            require(std::size_t(p.cell) < state.volume.size() && std::size_t(p.cell) < state.transport.cell_mass.size(),
                    "Live mapped donor is unavailable.");
        start_ms_ = start; end_ms_ = end; phase_ = phase; first_ = count_;
        ever_opened_ = open_ = true;
    } catch (...) { fail("Live passive exchange publication failed."); throw; }
}
bool LivePassiveStorageExchange::admit(std::size_t point, int cell, int node, double water,
                                      double mass, double before, double volume_before, double offset_s, double duration_s, double withdrawal_si) noexcept {
    if (failed()) return false;
    attempted_ = {point, cell, node, water > 0, false, std::abs(water), mass,
                  std::abs(water) * factor_, mass * factor_, before,
                  water > 0 ? before - mass : before + mass, 0, start_ms_, end_ms_, phase_};
    attempted_.volume_before = volume_before;
    attempted_.volume_after = std::max(0.0, volume_before - water);
    attempted_.offset_s = offset_s; attempted_.duration_s = duration_s; attempted_.withdrawal_si = withdrawal_si;
    auto reject = [&](const char* reason) { fail(reason); return false; };
    if (!open_ || reserved_ || point >= points_.size() || points_[point].cell != cell || points_[point].node != node)
        return reject("Live passive exchange leg has no matching open owner.");
    auto it = std::find_if(budgets_.begin(), budgets_.end(), [&](const Budget& b) { return b.node == node; });
    if (it == budgets_.end()) return reject("Live passive exchange donor is unowned.");
    attempted_.sample = it->concentration;
    if (count_ == legs_.size()) return reject("Live passive exchange journal capacity exhausted.");
    if (!std::isfinite(water) || water == 0 || !std::isfinite(mass) || mass < 0 ||
        !std::isfinite(before) || before < 0 || !std::isfinite(attempted_.after) || attempted_.after < 0 ||
        !std::isfinite(attempted_.water_native) || attempted_.water_native == 0 ||
        !std::isfinite(attempted_.mass_native) || (mass != 0 && attempted_.mass_native == 0))
        return reject("Live passive exchange leg is nonfinite or has no representable carrier.");
    if (!std::isfinite(volume_before) || volume_before < 0 || (water > 0 && volume_before == 0) ||
        !std::isfinite(attempted_.volume_after) || !std::isfinite(offset_s) || offset_s < 0 ||
        !std::isfinite(duration_s) || duration_s <= 0 ||
        offset_s + duration_s > (end_ms_ - start_ms_) / 1000)
        return reject("Live passive exchange donor water or firing interval differs.");
    if (water > 0 && mass != std::min(water * (before / volume_before), before))
        return reject("Live passive exchange drain differs from its actual donor operands.");
    if (!std::isfinite(withdrawal_si) || withdrawal_si < 0 ||
        (water > 0 ? withdrawal_si != 0 : (withdrawal_si / duration_s) * duration_s != -water))
        return reject("Live passive exchange water withdrawal differs from native operands.");
    const double delta = water > 0 ? -mass : mass;
    const double bound = 2 * std::numeric_limits<double>::epsilon() * std::abs(attempted_.after);
    if (mass != 0 && (attempted_.after == before || std::abs((attempted_.after - before) - delta) > bound))
        return reject("Live passive exchange source increment is not representable.");
    if (water < 0) {
        if (mass != attempted_.water_si * attempted_.sample)
            return reject("Live passive exchange spill differs from its actual SI sample.");
        if (retainsMaterialAggregates(book_.arithmetic())) {
            auto proposed = it->open, total = it->sealed;
            if (!proposed.tryAdd(attempted_.mass_native) || !total.tryAdd(proposed))
                return reject("Live passive exchange exact spill reservation exceeds arithmetic capacity.");
            if (!book_.canFund(node, it->volume * it->concentration, total))
                return reject("Live passive exchange spill exceeds unencumbered native mass.");
            it->open = proposed;
        } else {
            double proposed, total;
            if (!add(it->open.value(), attempted_.mass_native, proposed) || !add(it->sealed.value(), proposed, total))
                return reject("Live passive exchange spill reservation is not exactly representable.");
            if (!book_.canFund(node, it->volume * it->concentration, total))
                return reject("Live passive exchange spill exceeds unencumbered native mass.");
            it->open = {}; it->open.tryAdd(proposed);
        }
    }
    legs_[count_++] = attempted_;
    reserved_ = true;
    return true;
}
void LivePassiveStorageExchange::applied(double actual_after, double volume_after) noexcept {
    if (!reserved_ || count_ == 0) { fail("Live passive exchange has no reserved leg to apply."); return; }
    auto& l = legs_[count_ - 1];
    l.applied = true;
    reserved_ = false;
    if (l.after != actual_after || l.volume_after != volume_after) fail("Live passive exchange native write differs from admission.");
}
void LivePassiveStorageExchange::seal(const SimulationContext& ctx, const std::vector<double>& exchange) {
    requireReady();
    try {
        require(open_ && !reserved_ && &ctx == context_ && exchange.size() == points_.size(),
                "Live passive exchange has no complete hydraulic handoff.");
        std::vector<PassiveStorageExchange::Row> rows(points_.size());
        std::vector<double> hydraulic(budgets_.size(), 0), gross(budgets_.size(), 0);
        // Index only this window. Prepending in reverse keeps each point's
        // firing order and leaves the chronological proof journal intact.
        std::array<std::size_t, 256> local_indices;
        std::span<std::size_t> indices = local_indices;
        std::vector<std::size_t> overflow;
        const auto index_count = points_.size() + (count_ - first_);
        if (index_count > indices.size()) {
            overflow.resize(index_count);
            indices = overflow;
        }
        auto first_leg = indices.first(points_.size());
        auto next_leg = indices.subspan(points_.size(), count_ - first_);
        std::fill(first_leg.begin(), first_leg.end(), count_);
        for (std::size_t i = count_; i > first_;) {
            --i;
            auto& head = first_leg[legs_[i].point];
            next_leg[i - first_] = head;
            head = i;
        }
        for (std::size_t k = 0; k < points_.size(); ++k) {
            rows[k] = {k, points_[k].node, ctx.pollutant_names.names()[0], ctx.pollutants.units[0], 0, 0, 0, 0};
            const auto b = std::find_if(budgets_.begin(), budgets_.end(), [&](const Budget& b) { return b.node == rows[k].node; });
            const auto j = std::size_t(b - budgets_.begin());
            hydraulic[j] = book_.changingVolume() ? hydraulic[j] + exchange[k] * factor_ : sum(hydraulic[j], exchange[k] * factor_);
            double actual = 0;
            NativeAmountSum drain, spill;
            for (std::size_t i = first_leg[k]; i != count_; i = next_leg[i - first_]) {
                const auto& l = legs_[i];
                require(l.applied, "Live passive exchange contains an unapplied reservation.");
                actual = book_.changingVolume() ? actual + (l.drain ? l.water_si : -l.water_si) : sum(actual, l.drain ? l.water_si : -l.water_si);
                if (l.drain) rows[k].drain_water = sum(rows[k].drain_water, l.water_native);
                else rows[k].spill_water = sum(rows[k].spill_water, l.water_native);
                if (retainsPointAggregates(book_.arithmetic())) {
                    if (l.drain) drain.add(l.mass_native); else spill.add(l.mass_native);
                } else if (l.drain) rows[k].drain_mass = sum(rows[k].drain_mass, l.mass_native);
                else rows[k].spill_mass = sum(rows[k].spill_mass, l.mass_native);
                if (!book_.changingVolume()) gross[j] = sum(gross[j], l.drain ? l.water_native : -l.water_native);
            }
            if (retainsPointAggregates(book_.arithmetic())) {
                rows[k].setMaterialAmount(true, drain); rows[k].setMaterialAmount(false, spill);
            }
            require(actual == exchange[k], "Live gross legs differ from native hydraulic booking.");
        }
        if (!book_.changingVolume())
            for (std::size_t j = 0; j < budgets_.size(); ++j)
                require(hydraulic[j] == 0 && gross[j] == 0, "Live passive exchange requires exactly zero net native water.");
        std::erase_if(rows, [](const auto& r) { return r.drain_water == 0 && r.spill_water == 0; });
        PassiveStorageExchange::LiveProof proof;
        proof.si_to_native = factor_;
        proof.legs.assign(legs_.begin() + first_, legs_.begin() + count_);
        for (std::size_t k = 0; k < points_.size(); ++k)
            proof.points.push_back({k, points_[k].cell, points_[k].node, exchange[k]});
        if (!rows.empty()) {
            book_.reserveAuthenticated(ctx, next_id_, std::move(rows), phase_, start_ms_, end_ms_, proof);
            ++next_id_;
        }
        if (book_.changingVolume())
            book_.beginWaterPublication(ctx, proof, phase_, start_ms_, end_ms_, (end_ms_ - start_ms_) / 1000.0);
        open_ = false;
    } catch (...) { fail("Live passive exchange window cannot be sealed; applied legs retained."); throw; }
}
void LivePassiveStorageExchange::observeWaterBooking(const SimulationContext& ctx) {
    if (!book_.changingVolume()) return;
    requireReady();
    try { book_.finishWaterPublication(ctx); }
    catch (...) { fail("Live changing-volume water publication failed."); throw; }
}
}
#endif
