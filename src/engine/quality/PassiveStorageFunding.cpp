// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
#include "PassiveStorageExchange.hpp"
#include "../core/SimulationContext.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace openswmm::quality {
namespace {
void require(bool ok, const char* message) {
    if (!ok) throw std::invalid_argument(message);
}
}
void PassiveStorageExchange::grantFunding(const SimulationContext& ctx, std::uint64_t receipt,
        Phase phase, double start, double end, std::size_t capacity) {
    validate(ctx);
    require(changingVolume() && exactSlices() && owner_ == &ctx && !funding_ && !seal_ &&
                !prepared_ && !pending_water_ && receipt > last_id_ && receipt == last_id_ + 1 &&
                water_publications_.size() < std::numeric_limits<std::uint64_t>::max() && capacity <= 65536,
            "Passive funding requires an exclusive native publication owner.");
    require(std::isfinite(start) && std::isfinite(end) && start >= 0 && end > start &&
                start == (water_publications_.empty() ? 0 : water_publications_.back().publication.batch_end_ms) &&
                (phase == Phase::ADVANCE ? frozen_ && frozen_->observed &&
                    start <= frozen_->start_ms && end == frozen_->end_ms && ctx.elapsed_ms == frozen_->start_ms :
                    (phase == Phase::REPORT || phase == Phase::FINAL) && !frozen_ && end == ctx.elapsed_ms),
            "Passive funding phase or physical window differs.");
    FundingAllowance next{std::uint64_t(water_publications_.size()) + 1, receipt, std::uint64_t(deliveries_.size()), phase,
        ctx.elapsed_ms, start, end, ctx.coupling_delivery_remaining,
        ctx.pollutant_names.names()[0], ctx.pollutants.units[0], capacity};
    next.spills.reserve(capacity); // every later spill admission is allocation-free
    if (peer_binding_) next.peer = *peer_binding_;
    for (const auto& [node, pool] : pools_) {
        validatePool(ctx, node, pool, false);
        FundingPool row{node, pool, ctx.nodes.conc.at(node), storedRoundoff(node), pendingSpillAmount(node)};
        const double physical = pool.volume * pool.concentration;
        require(canFund(node, physical, row.sealed), "Existing passive spill is no longer funded.");
        row.granted.add(physical);
        if (row.roundoff.negative()) row.granted.add(row.roundoff);
        for (double part : row.sealed.parts()) row.granted.add(-part);
        require(!row.granted.negative(), "Passive allowance has negative available inventory.");
        next.pools.push_back(std::move(row));
    }
    funding_ = std::move(next);
}

bool PassiveStorageExchange::spendFunding(const SimulationContext& ctx, const AppliedLeg& leg) noexcept {
    if (peer_binding_ || &ctx != owner_ || !funding_ || funding_->reconciled || seal_ || pending_water_ || leg.drain || leg.applied ||
        funding_->ordinal != water_publications_.size() + 1 || ctx.elapsed_ms != funding_->capture_ms ||
        leg.phase != funding_->phase || leg.batch_start_ms != funding_->start_ms || leg.batch_end_ms != funding_->end_ms ||
        !std::isfinite(leg.mass_native) || leg.mass_native < 0 ||
        funding_->spills.size() >= funding_->leg_limit) return false;
    auto& pools = funding_->pools;
    auto it = std::find_if(pools.begin(), pools.end(), [&](const FundingPool& p) { return p.node == leg.node; });
    if (it == pools.end() || leg.sample != it->published_concentration) return false;
    auto spent = it->spent;
    if (!spent.tryAdd(leg.mass_native) || !it->granted.atLeast(spent)) return false;
    funding_->spills.push_back(leg);
    it->spent = spent;
    return true;
}

void PassiveStorageExchange::reconcileFunding(const SimulationContext& ctx, const LiveProof& proof,
        Phase phase, double start, double end) const {
    require(funding_ && owner_ == &ctx && !funding_->reconciled && !seal_ && !pending_water_ &&
                funding_->ordinal == water_publications_.size() + 1 && ctx.elapsed_ms == funding_->capture_ms &&
                phase == funding_->phase && start == funding_->start_ms && end == funding_->end_ms &&
                funding_->pollutant == ctx.pollutant_names.names()[0] && funding_->units == ctx.pollutants.units[0],
            "Passive seal has no matching exclusive funding allowance.");
    if (peer_binding_)
        require(funding_->peer == *peer_binding_ && funding_->peer_reported && peer_received_ && proof == *peer_received_,
                "Peer funding requires its bound reported proof, not a local spending replay.");
    std::size_t index = 0;
    for (const auto& leg : proof.legs) if (!leg.drain && !peer_binding_) {
        require(index < funding_->spills.size() && leg.applied, "Passive seal has an unfunded spill leg.");
        auto expected = funding_->spills[index++]; expected.applied = true;
        require(leg == expected, "Passive seal spill operands or order differ from pre-credit spending.");
    }
    require(index == funding_->spills.size(), "Passive seal omitted a reserved spill leg.");
    for (const auto& row : funding_->pools) {
        NativeAmountSum actual;
        for (const auto& leg : proof.legs) if (!leg.drain && leg.node == row.node) actual.add(leg.mass_native);
        require(actual == row.spent && row.granted.atLeast(actual) &&
                    pendingSpillAmount(row.node) == row.sealed && storedRoundoff(row.node) == row.roundoff &&
                    pools_.at(row.node) == row.settled && ctx.nodes.conc.at(row.node) == row.published_concentration,
                "Passive seal funding inventory or gross spend differs.");
    }
}
}
