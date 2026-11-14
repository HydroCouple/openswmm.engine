// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
#include "PassiveStorageExchange.hpp"
#include "QualityRouting.hpp"
#include "../core/SimulationContext.hpp"
#include "../transport/components/ReactionModule/ReactionLegacyBinding.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>
#include <type_traits>

namespace openswmm::quality {
namespace {
void require(bool ok, const char* message) {
    if (!ok)
        throw std::invalid_argument(std::string("Passive storage exchange: ") + message);
}
bool zeroAt(const std::vector<double>& values, std::size_t i) {
    return values.empty() || (i < values.size() && values[i] == 0.0);
}
bool noneAt(const std::vector<ForcingMode>& values, std::size_t i) {
    return values.empty() || (i < values.size() && values[i] == ForcingMode::NONE);
}
bool pending(const PassiveStorageExchange::Row& r) {
    return r.drain_water != 0 || r.spill_water != 0 || r.drain_mass != 0 || r.spill_mass != 0 ||
        !r.drain_mass_parts.empty() || !r.spill_mass_parts.empty() ||
        !r.drain_water_parts.empty() || !r.spill_water_parts.empty();
}
bool pending(const std::vector<PassiveStorageExchange::Receipt>& receipts) {
    for (const auto& receipt : receipts)
        for (const auto& row : receipt.remaining)
            if (pending(row))
                return true;
    return false;
}
using ExactSum = NativeAmountSum;
void addWater(ExactSum& sum, const PassiveStorageExchange::Row& row) {
    sum.add(row.waterAmount(true));
    const auto spill = row.waterAmount(false);
    for (double part : spill.parts()) sum.add(-part);
}
double nativeConcentration(double volume, double concentration, double amount) {
    if (amount == 0)
        return concentration;
    const double before = volume * concentration;
    ExactSum stored;
    stored.add(before);
    stored.add(amount);
    const double target = stored.value();
    const double next = target / volume;
    const double actual = next * volume;
    const double bound = (2 * std::numeric_limits<double>::epsilon()) * std::abs(target);
    require(target >= 0 && std::isfinite(next) && std::isfinite(actual) &&
                next != concentration && actual != before && std::abs(actual - target) <= bound,
            "stored increment is not representable within native division/product roundoff");
    return next;
}
struct MassTotals {
    ExactSum drain, spill, net;
};
MassTotals totals(const std::vector<PassiveStorageExchange::Receipt>& receipts, int node) {
    MassTotals sum;
    for (const auto& receipt : receipts)
        for (const auto& row : receipt.remaining)
            if (row.node == node) {
                const auto drain = row.materialAmount(true), spill = row.materialAmount(false);
                sum.drain.add(drain); sum.spill.add(spill); sum.net.add(drain);
                for (double part : spill.parts()) sum.net.add(-part);
            }
    return sum;
}
} // namespace

NativeAmountSum PassiveStorageExchange::Row::materialAmount(bool drain) const {
    const double scalar = drain ? drain_mass : spill_mass;
    const auto& components = drain ? drain_mass_parts : spill_mass_parts;
    NativeAmountSum amount;
    require(std::isfinite(scalar) && scalar >= 0, "invalid scalar material amount");
    if (components.empty()) { amount.add(scalar); return amount; }
    require(scalar == 0 && components.size() >= 2 && components.size() <= NativeAmountSum::capacity,
            "invalid point material scalar/component extent");
    for (double part : components) {
        require(std::isfinite(part) && part != 0, "invalid point material component");
        amount.add(part);
    }
    require(!amount.negative() && amount.parts().size() == components.size() &&
                std::equal(components.begin(), components.end(), amount.parts().begin()),
            "noncanonical/negative point material amount");
    return amount;
}
void PassiveStorageExchange::Row::setMaterialAmount(bool drain, const NativeAmountSum& amount) {
    require(!amount.negative(), "negative point material amount");
    auto& scalar = drain ? drain_mass : spill_mass;
    auto& components = drain ? drain_mass_parts : spill_mass_parts;
    if (amount.parts().size() <= 1) { scalar = amount.value(); components.clear(); }
    else { components.assign(amount.parts().begin(), amount.parts().end()); scalar = 0; }
}

NativeAmountSum PassiveStorageExchange::Row::waterAmount(bool drain) const {
    const double scalar = drain ? drain_water : spill_water;
    const auto& components = drain ? drain_water_parts : spill_water_parts;
    NativeAmountSum amount;
    require(std::isfinite(scalar) && scalar >= 0, "invalid scalar water amount");
    if (components.empty()) { amount.add(scalar); return amount; }
    require(scalar == 0 && components.size() >= 2 && components.size() <= NativeAmountSum::capacity,
            "invalid point water scalar/component extent");
    for (double part : components) {
        require(std::isfinite(part) && part != 0, "invalid point water component");
        amount.add(part);
    }
    require(!amount.negative() && amount.parts().size() == components.size() &&
                std::equal(components.begin(), components.end(), amount.parts().begin()),
            "noncanonical/negative point water amount");
    return amount;
}
void PassiveStorageExchange::Row::setWaterAmount(bool drain, const NativeAmountSum& amount) {
    require(!amount.negative(), "negative point water amount");
    auto& scalar = drain ? drain_water : spill_water;
    auto& components = drain ? drain_water_parts : spill_water_parts;
    if (amount.parts().size() <= 1) { scalar = amount.value(); components.clear(); }
    else { components.assign(amount.parts().begin(), amount.parts().end()); scalar = 0; }
}

void PassiveStorageExchange::validate(const SimulationContext& ctx) const {
    require(!failed_, "receiving owner is terminal");
    require(water_arithmetic_ == PassiveStorageWaterArithmetic::EXACT ||
                (retainsWaterAggregates() && changingVolume() && (arithmetic_ == PassiveStorageArithmetic::RETAIN_EXACT_HALVES || exactSlices())),
            "retained water requires changing-volume authenticated exact-half arithmetic");
    require(!exactSlices() || (changingVolume() && retainsWaterAggregates()),
            "exact slices require changing-volume retained-water arithmetic");
    require(law_ == PassiveStorageLaw::FIXED_VOLUME ||
                (changingVolume() && (arithmetic_ == PassiveStorageArithmetic::RETAIN_EXACT_HALVES || exactSlices())),
            "changing-volume law requires authenticated exact-half arithmetic");
    if (changingVolume()) {
        require(!ctx.n_links() && !ctx.n_subcatches() && !ctx.n_gages() && ctx.events.empty() &&
                    !ctx.ext_inflows.count() && !ctx.dwf_inflows.count() && !ctx.control_rules.count() && !ctx.runtime_coupling_used,
                "changing-volume law excludes network links, sources, events and controls");
        const auto& storage = ctx.node_subtypes.storages;
        for (const auto* values : {&storage.evap_frac, &storage.evap_loss, &storage.exfil_loss, &storage.exfil_rate, &storage.exfil_ksat})
            for (double v : *values) require(v == 0, "changing-volume law excludes storage evaporation/exfiltration");
    }
    require(arithmetic_ == PassiveStorageArithmetic::EXACT || arithmetic_ == PassiveStorageArithmetic::RETAIN_ROUNDOFF ||
                retainsMaterialAggregates(arithmetic_),
            "unknown stored arithmetic policy");
    require(!owner_ || owner_ == &ctx, "wrong native context");
    require(ctx.n_pollutants() == 1 && ctx.pollutants.units.size() == 1 &&
                ctx.pollutants.k_decay.size() == 1 && ctx.pollutants.k_decay[0] == 0,
            "exactly one passive native pollutant is required");
    require(ctx.pollutants.unit_declaration_valid.size() == 1 && ctx.pollutants.unit_declaration_valid[0] == 1,
            "unrecognized pollutant unit declaration; use MG/L, UG/L or #/L");
    const auto units = ctx.pollutants.units[0];
    require(units == MassUnits::MG_PER_L || units == MassUnits::UG_PER_L ||
                units == MassUnits::COUNTS_PER_L, "unsupported native amount basis");
    require(!owner_ || (node_ids_ == ctx.node_names.names() &&
                pollutant_ids_ == ctx.pollutant_names.names() && units_ == units),
            "native identity, ordering or amount basis changed");
    require(ctx.options.routing_model == RoutingModel::DYNWAVE &&
                ctx.options.quality_solver == QualitySolverKind::LEGACY &&
                !ctx.options.ignore_quality && !ctx.options.water_age &&
                !ctx.options.heat_transport && ctx.reactions.n_species() == 0 &&
                !transport::legacyReactionsActive(ctx) && !ctx.treatment.hasAny(),
            "unsupported quality owner");
    for (const auto& state : ctx.node_subtypes.storages.lid_state)
        require(state.cells.empty(), "storage LID iteration is unsupported");
    require(!ctx.surface_outfall_quality.any && !ctx.link_gain_quality.any,
            "another quality receipt owner is active");
    require(std::isfinite(ctx.elapsed_ms) && ctx.elapsed_ms >= 0 &&
                std::isfinite(ctx.coupling_delivery_remaining) && ctx.coupling_delivery_remaining >= 0,
            "invalid native clock");
}

void PassiveStorageExchange::validatePool(const SimulationContext& ctx, int node,
                                         const Pool& pool, bool assembled) const {
    require(node >= 0 && node < ctx.n_nodes(), "unknown storage node");
    const auto i = static_cast<std::size_t>(node);
    const auto& n = ctx.nodes;
    require(i < n.type.size() && i < n.volume.size() && i < n.old_volume.size() &&
                i < n.depth.size() && i < n.old_depth.size() &&
                i < n.conc.size() && i < n.conc_old.size(), "incomplete native pool shape");
    require(n.type[i] == NodeType::STORAGE && std::isfinite(pool.volume) &&
                pool.volume > ZERO_VOLUME && std::isfinite(pool.depth) && pool.depth > ZERO_DEPTH &&
                std::isfinite(pool.concentration) && pool.concentration >= 0 &&
                std::isfinite(pool.concentration * pool.volume) &&
                (changingVolume() ?
                 (std::isfinite(n.volume[i]) && n.volume[i] > ZERO_VOLUME &&
                  std::isfinite(n.old_volume[i]) && n.old_volume[i] > ZERO_VOLUME &&
                  std::isfinite(n.depth[i]) && n.depth[i] > ZERO_DEPTH &&
                  std::isfinite(n.old_depth[i]) && n.old_depth[i] > ZERO_DEPTH &&
                  ((frozen_ && frozen_->observed) ?
                   (n.old_volume[i] == pool.volume && n.old_depth[i] == pool.depth) :
                   (n.volume[i] == pool.volume && n.depth[i] == pool.depth))) :
                 (n.volume[i] == pool.volume && n.old_volume[i] == pool.volume &&
                  n.depth[i] == pool.depth && n.old_depth[i] == pool.depth)) &&
                n.conc[i] == pool.concentration && (!assembled || n.conc_old[i] == pool.concentration),
            "pool is dry, changed, or not settled passive storage");
    for (int link = 0; link < ctx.n_links(); ++link)
        require(ctx.links.node1[link] != node && ctx.links.node2[link] != node,
                "incident links are unsupported");
    for (int sub = 0; sub < ctx.n_subcatches(); ++sub)
        require(ctx.subcatches.outlet_node[sub] != node, "ordinary watershed outlet is unsupported");
    const int row = ctx.node_subtypes.storage_row(node);
    require(row >= 0 && zeroAt(ctx.node_subtypes.storages.evap_loss, row) &&
                zeroAt(ctx.node_subtypes.storages.exfil_loss, row), "another storage water owner is active");
    if (!changingVolume())
        for (const auto* values : {&n.lat_flow, &n.old_lat_flow, &n.inflow, &n.outflow,
                                  &n.overflow, &n.coupling_inflow, &n.coupling_queue})
            require(zeroAt(*values, i), "ordinary or legacy coupling mobilization is unsupported");
    else
        require(std::isfinite(n.coupling_inflow.at(i)) && n.lat_flow.at(i) == n.coupling_inflow.at(i) && n.losses.at(i) == 0 &&
                    n.inflow.at(i) == std::max(0.0, n.coupling_inflow.at(i)) &&
                    n.outflow.at(i) == std::max(0.0, -n.coupling_inflow.at(i)) &&
                    std::isfinite(n.coupling_queue.at(i)), "inconsistent closed native water state");
    for (const auto* values : {&n.user_lat_flow, &n.ext_inflow, &n.dwf_inflow, &n.rdii_inflow, &n.iface_inflow,
             &n.runoff_inflow, &n.gw_inflow, &n.lid_drain_inflow,
             &n.coupling_volume, &n.coupling_qual_queue, &n.coupling_qual_inflow,
             &n.ext_qual_mass, &n.dwf_qual_mass, &n.iface_qual_mass, &n.user_conc_mass_flux,
             &n.lid_drain_qual_load, &n.lid_drain_qual_load_old})
        require(zeroAt(*values, i), "ordinary or legacy coupling mobilization is unsupported");
    require((n.aquifer2d_bed.empty() || (i < n.aquifer2d_bed.size() && !n.aquifer2d_bed[i])) &&
                noneAt(ctx.forcing.node_lat_inflow_mode, i) &&
                noneAt(ctx.forcing.node_quality_mode, i), "another native source/forcing owner is active");
    if (assembled)
        require((changingVolume() && frozen_ ?
                    n.qual_vol_in.at(i) == std::max(0.0, n.coupling_inflow.at(i)) * frozen_->dt :
                    zeroAt(n.qual_vol_in, i)) && zeroAt(n.qual_mass_in, i),
                "ordinary assembled quality loads are unsupported");
}

void PassiveStorageExchange::bindChangingVolumePools(const SimulationContext& ctx, const std::vector<int>& nodes) {
    validate(ctx);
    require(changingVolume() && !owner_ && nodes.size() == std::size_t(ctx.n_nodes()) && !nodes.empty() && ctx.elapsed_ms == 0,
            "changing-volume pools require an initial unbound owner");
    std::map<int, Pool> pools;
    for (int node : nodes) {
        require(node >= 0 && node < ctx.n_nodes(), "unknown initial pool");
        Pool sample{ctx.nodes.volume[node], ctx.nodes.depth[node], ctx.nodes.conc[node]};
        validatePool(ctx, node, sample, true);
        require(ctx.nodes.old_volume[node] == sample.volume && ctx.nodes.old_depth[node] == sample.depth &&
                    ctx.nodes.coupling_queue[node] == 0 && ctx.nodes.coupling_inflow[node] == 0 &&
                    ctx.nodes.old_net_inflow[node] == 0 && ctx.nodes.old_lat_flow[node] == 0 &&
                    ctx.nodes.old_inflow[node] == 0, "initial changing-volume water is not settled");
        require(pools.emplace(node, sample).second, "duplicate initial pool");
    }
    auto initial = pools;
    auto names = ctx.node_names.names(), pollutants = ctx.pollutant_names.names();
    pools_.swap(pools); initial_pools_.swap(initial);
    node_ids_.swap(names); pollutant_ids_.swap(pollutants);
    owner_ = &ctx; units_ = ctx.pollutants.units[0];
    next_start_ms_ = ctx.elapsed_ms; remaining_ = ctx.coupling_delivery_remaining;
}

PassiveStorageExchange::WaterState PassiveStorageExchange::waterState(const SimulationContext& ctx) const {
    WaterState s;
    for (const auto& [node, pool] : initial_pools_) {
        const auto& n = ctx.nodes;
#define COLUMN(field, native) s.field.push_back(n.native.at(node));
        COLUMN(volume, volume) COLUMN(old_volume, old_volume) COLUMN(depth, depth) COLUMN(old_depth, old_depth)
        COLUMN(inflow, inflow) COLUMN(outflow, outflow) COLUMN(old_net, old_net_inflow)
        COLUMN(old_lateral, old_lat_flow) COLUMN(old_inflow, old_inflow) COLUMN(rate, coupling_inflow) COLUMN(queue, coupling_queue)
#undef COLUMN
    }
    return s;
}
void PassiveStorageExchange::installWaterState(SimulationContext& ctx, const WaterState& s) const {
    const auto size = initial_pools_.size();
    for (const auto* c : {&s.volume, &s.old_volume, &s.depth, &s.old_depth, &s.inflow, &s.outflow,
                         &s.old_net, &s.old_lateral, &s.old_inflow, &s.rate, &s.queue}) {
        require(c->size() == size, "observed hydraulic column shape differs");
        for (double v : *c) require(std::isfinite(v), "nonfinite observed hydraulic state");
    }
    std::size_t k = 0;
    for (const auto& [node, pool] : initial_pools_) {
        auto& n = ctx.nodes;
#define COLUMN(field, native) n.native.at(node) = s.field[k];
        COLUMN(volume, volume) COLUMN(old_volume, old_volume) COLUMN(depth, depth) COLUMN(old_depth, old_depth)
        COLUMN(inflow, inflow) COLUMN(outflow, outflow) COLUMN(old_net, old_net_inflow)
        COLUMN(old_lateral, old_lat_flow) COLUMN(old_inflow, old_inflow) COLUMN(rate, coupling_inflow) COLUMN(queue, coupling_queue)
#undef COLUMN
        n.lat_flow[node] = s.rate[k++];
    }
}
void PassiveStorageExchange::validateWaterTransition(const SimulationContext& ctx) const {
    require(frozen_ && frozen_->observed, "changing-volume delivery needs native water observations");
    const auto& before = frozen_->before;
    std::size_t k = 0;
    for (const auto& [node, pool] : initial_pools_) {
        const auto& n = ctx.nodes;
        require(n.old_volume[node] == before.volume[k] && n.old_depth[node] == before.depth[k] &&
                    n.old_net_inflow[node] == before.inflow[k] - before.outflow[k] &&
                    n.old_inflow[node] == before.inflow[k] && n.old_lat_flow[node] == before.rate[k] &&
                    n.coupling_inflow[node] == frozen_->assembled.rate[k],
                "native hydraulic transition differs from frozen water memory");
        ++k;
    }
}
void PassiveStorageExchange::beginWaterPublication(const SimulationContext& ctx, const LiveProof& proof,
        Phase phase, double start, double end, double dt) {
    require(!changingVolume() || !exactSlices() || funding_, "native sliced water publication requires issued funding");
    require(changingVolume() && !pending_water_ && std::isfinite(dt) && dt > 0 &&
                std::isfinite(proof.si_to_native) && proof.si_to_native > 0 &&
                (phase == Phase::ADVANCE ? frozen_ && frozen_->observed : !frozen_) &&
                std::isfinite(start) && start >= 0 && std::isfinite(end) && end > start &&
                end == start + 1000.0 * dt, "invalid native water publication interval");
    require(phase == Phase::ADVANCE ?
                (start <= frozen_->start_ms && end == frozen_->end_ms && ctx.elapsed_ms == frozen_->start_ms) :
                ((phase == Phase::REPORT || phase == Phase::FINAL) && end == ctx.elapsed_ms),
            "native water publication phase/endpoint differs");
    require(start == (water_publications_.empty() ? 0 : water_publications_.back().publication.batch_end_ms),
            "native water publication windows are discontinuous");
    if (funding_) {
        if (!seal_) reconcileFunding(ctx, proof, phase, start, end);
        else require(&ctx == owner_ && funding_->reconciled && phase == funding_->phase && start == funding_->start_ms &&
                         end == funding_->end_ms && ctx.elapsed_ms == funding_->capture_ms,
                     "prepared material seal differs from its funding window");
        water_publications_.reserve(water_publications_.size() + 1);
    }
    std::set<std::uint64_t> points;
    for (const auto& point : proof.points)
        require(initial_pools_.count(point.node) && point.cell >= 0 && points.insert(point.point).second &&
                    std::isfinite(point.exchange_si) && (!proof.legs.empty() || point.exchange_si == 0),
                "native water publication topology/empty proof differs");
    WaterPublication next;
    next.publication = {phase, ctx.elapsed_ms, start, end,
        phase == Phase::ADVANCE ? frozen_->end_ms : ctx.elapsed_ms,
        ctx.coupling_delivery_remaining, deliveries_.size()};
    next.dt = dt; next.si_to_native = proof.si_to_native;
    next.points = proof.points; next.before = waterState(ctx).queue;
    next.after = next.before; next.remaining_after = ctx.coupling_delivery_remaining;
    if (!proof.legs.empty()) {
        const auto& receipts = seal_ ? seal_->receipts : receipts_;
        require(!receipts.empty() && receipts.back().proof == proof, "water publication has no matching material proof");
        next.receipt = receipts.back().id;
    }
    bool booked = false;
    std::size_t k = 0;
    for (const auto& [node, pool] : initial_pools_) {
        double actual = 0;
        NativeAmountSum gross;
        for (const auto& point : proof.points) if (point.node == node) actual += point.exchange_si * proof.si_to_native;
        for (const auto& leg : proof.legs) if (leg.node == node) gross.add(leg.drain ? -leg.water_native : leg.water_native);
        gross.add(actual); next.booking_roundoff.push_back(gross);
        require(std::isfinite(actual) && (next.receipt || actual == 0), "water publication lacks authenticated legs");
        next.after[k] += actual;
        require(std::isfinite(next.after[k++]), "nonfinite native water booking");
        booked |= actual != 0;
    }
    if (booked) next.remaining_after = std::max(next.remaining_after, dt);
    pending_water_ = std::move(next);
    if (funding_) funding_->reconciled = true;
}
void PassiveStorageExchange::finishWaterPublication(const SimulationContext& ctx) {
    require(!changingVolume() || !exactSlices() || funding_, "native sliced water confirmation requires issued funding");
    require(pending_water_.has_value() && &ctx == owner_, "missing native water publication or wrong owner");
    require(pending_water_->after.size() == initial_pools_.size() &&
                ctx.coupling_delivery_remaining == pending_water_->remaining_after, "actual native water span differs");
    std::size_t k = 0;
    for (const auto& [node, pool] : initial_pools_) {
        require(ctx.nodes.coupling_queue.at(node) == pending_water_->after[k++], "actual native water queue differs");
        require(ctx.nodes.coupling_volume.at(node) == 0, "native booking left an undelivered volume");
    }
    if (funding_)
        require(funding_->reconciled && funding_->ordinal == water_publications_.size() + 1 &&
                    ctx.elapsed_ms == funding_->capture_ms &&
                    pending_water_->publication == Publication{funding_->phase, funding_->capture_ms,
                        funding_->start_ms, funding_->end_ms,
                        funding_->phase == Phase::ADVANCE ? funding_->end_ms : funding_->capture_ms,
                        funding_->remaining, funding_->generation} && funding_->generation == deliveries_.size() &&
                    (funding_->phase == Phase::ADVANCE ? frozen_ && frozen_->observed && !prepared_ &&
                        frozen_->start_ms == funding_->capture_ms && frozen_->end_ms == funding_->end_ms : !frozen_) &&
                    (!pending_water_->receipt || pending_water_->receipt == funding_->receipt) &&
                    water_publications_.capacity() > water_publications_.size() &&
                    (pending_water_->receipt ? seal_.has_value() : !seal_),
                "native water confirmation has no matching prepared funding seal");
    // Capacity was reserved before native water effects. These owner commits do
    // not allocate; unused authority is released only after both books publish.
    water_publications_.push_back(std::move(*pending_water_)); pending_water_.reset();
    if (seal_) {
        receipts_.swap(seal_->receipts); pools_.swap(seal_->pools);
        node_ids_.swap(seal_->node_ids); pollutant_ids_.swap(seal_->pollutant_ids);
        last_id_ = seal_->last_id; next_start_ms_ = seal_->next_start_ms; remaining_ = seal_->remaining;
        seal_.reset();
    }
    if (!frozen_) remaining_ = ctx.coupling_delivery_remaining;
    peer_received_.reset();
    funding_.reset();
}

void PassiveStorageExchange::reserve(const SimulationContext& ctx, std::uint64_t id,
                                     std::vector<Row> rows) {
    Publication p{Phase::SETTLED, ctx.elapsed_ms, ctx.elapsed_ms, ctx.elapsed_ms,
                  ctx.elapsed_ms, ctx.coupling_delivery_remaining, deliveries_.size()};
    reserveImpl(ctx, id, std::move(rows), p);
}

void PassiveStorageExchange::reserveAtBoundary(const SimulationContext& ctx, std::uint64_t id,
        std::vector<Row> rows, Phase phase, double batch_start_ms, double batch_end_ms) {
    require(phase == Phase::REPORT || phase == Phase::FINAL, "unsupported boundary publication phase");
    require(std::isfinite(batch_start_ms) && std::isfinite(batch_end_ms) && batch_start_ms >= 0 &&
                batch_start_ms < batch_end_ms && batch_end_ms == ctx.elapsed_ms,
            "invalid physical boundary batch");
    Publication p{phase, ctx.elapsed_ms, batch_start_ms, batch_end_ms,
                  ctx.elapsed_ms, ctx.coupling_delivery_remaining, deliveries_.size()};
    reserveImpl(ctx, id, std::move(rows), p);
}

void PassiveStorageExchange::reserveNext(const SimulationContext& ctx, std::uint64_t id,
        std::vector<Row> rows, double batch_start_ms, double batch_end_ms) {
    require(frozen_ && frozen_->observed && !prepared_, "ADVANCE requires an observed delivery handoff");
    require(std::isfinite(batch_start_ms) && batch_start_ms >= 0 &&
                batch_start_ms <= frozen_->start_ms && batch_end_ms == frozen_->end_ms,
            "physical ADVANCE batch differs from its native endpoint");
    Publication p{Phase::ADVANCE, ctx.elapsed_ms, batch_start_ms, batch_end_ms,
                  frozen_->end_ms, ctx.coupling_delivery_remaining, deliveries_.size()};
    reserveImpl(ctx, id, std::move(rows), p);
}

void PassiveStorageExchange::reserveAuthenticated(const SimulationContext& ctx, std::uint64_t id,
        std::vector<Row> rows, Phase phase, double start, double end, const LiveProof& proof) {
    require(!changingVolume() || !exactSlices() || funding_, "native sliced material publication requires issued funding");
    require(std::isfinite(start) && std::isfinite(end) && start >= 0 && start < end,
            "invalid authenticated physical batch");
    Publication p{phase, ctx.elapsed_ms, start, end, ctx.elapsed_ms,
                  ctx.coupling_delivery_remaining, deliveries_.size()};
    if (phase == Phase::ADVANCE) {
        require(frozen_ && frozen_->observed && !prepared_ && start <= frozen_->start_ms &&
                    end == frozen_->end_ms, "authenticated ADVANCE differs from native endpoint");
        p.eligible_ms = frozen_->end_ms;
    } else {
        require((phase == Phase::REPORT || phase == Phase::FINAL) && end == ctx.elapsed_ms,
                "authenticated boundary differs from actual publication");
    }
    if (funding_) {
        require(id == funding_->receipt, "authenticated receipt differs from its funding identity");
        reconcileFunding(ctx, proof, phase, start, end);
    }
    reserveImpl(ctx, id, std::move(rows), p, &proof);
}

void PassiveStorageExchange::validateProof(const SimulationContext& ctx, const std::vector<Row>& rows,
        const Publication& p, const LiveProof& proof) const {
    require(std::isfinite(proof.si_to_native) && proof.si_to_native > 0 &&
                !proof.legs.empty() && proof.legs.size() <= 65536 &&
                !proof.points.empty() && proof.points.size() <= 4096, "invalid authenticated proof bounds");
    struct Gross { ExactSum dw, sw, dm, sm, signed_si; double actual_si = 0; };
    std::map<std::uint64_t, Gross> gross;
    std::map<std::uint64_t, HydraulicPoint> points;
    std::map<int, ExactSum> hydraulic;
    std::uint64_t last_point = 0;
    double last_offset = -1, last_duration = 0;
    for (const auto& point : proof.points) {
        require(point.cell >= 0 && point.node >= 0 && point.node < ctx.n_nodes() &&
                    std::isfinite(point.exchange_si) && points.emplace(point.point, point).second,
                "invalid authenticated point topology");
        hydraulic[point.node].add(point.exchange_si * proof.si_to_native);
    }
    for (const auto& l : proof.legs) {
        const auto it = points.find(l.point);
        require(it != points.end() && it->second.cell == l.cell && it->second.node == l.node &&
                    l.applied && l.phase == p.phase && l.batch_start_ms == p.batch_start_ms &&
                    l.batch_end_ms == p.batch_end_ms, "authenticated leg ownership/window differs");
        for (double value : {l.water_si, l.mass_si, l.water_native, l.mass_native, l.before, l.after,
                            l.sample, l.volume_before, l.volume_after, l.offset_s, l.duration_s, l.withdrawal_si})
            require(std::isfinite(value) && value >= 0, "invalid authenticated native operands");
        require(l.water_si > 0 && l.water_native > 0 && (!l.drain || l.volume_before > 0) && l.duration_s > 0 &&
                    l.offset_s + l.duration_s <= (p.batch_end_ms - p.batch_start_ms) / 1000 &&
                    (l.offset_s == last_offset ? (l.point > last_point && l.duration_s == last_duration)
                                               : l.offset_s >= last_offset + last_duration),
                "authenticated firing order/interval differs");
        require(l.sample == ctx.nodes.conc[l.node] && l.water_native == l.water_si * proof.si_to_native &&
                    l.mass_native == l.mass_si * proof.si_to_native &&
                    (l.mass_si == 0 || l.mass_native > 0), "authenticated sample/conversion differs");
        require(l.drain ? l.withdrawal_si == 0 :
                    (l.withdrawal_si / l.duration_s) * l.duration_s == l.water_si,
                "authenticated native water withdrawal differs");
        const double signed_water = l.drain ? l.water_si : -l.water_si;
        const double delta = l.drain ? -l.mass_si : l.mass_si;
        const double bound = 2 * std::numeric_limits<double>::epsilon() * std::abs(l.after);
        require(l.after == l.before + delta && l.volume_after == std::max(0.0, l.volume_before - signed_water) &&
                    (l.mass_si == 0 || (l.after != l.before && std::abs((l.after - l.before) - delta) <= bound)),
                "authenticated donor write differs");
        require(l.mass_si == (l.drain ? std::min(l.water_si * (l.before / l.volume_before), l.before)
                                     : l.water_si * l.sample), "authenticated material differs from native donor");
        auto& g = gross[l.point];
        g.signed_si.add(signed_water);
        g.actual_si += signed_water;
        if (l.drain) { g.dw.add(l.water_native); g.dm.add(l.mass_native); }
        else { g.sw.add(l.water_native); g.sm.add(l.mass_native); }
        last_point = l.point; last_offset = l.offset_s; last_duration = l.duration_s;
    }
    require(rows.size() == gross.size(), "authenticated row/proof extent differs");
    for (const auto& r : rows) {
        const auto it = gross.find(r.point);
        require(it != gross.end() && points.at(r.point).node == r.node, "authenticated row identity differs");
        const auto& g = it->second;
        require((retainsWaterAggregates() ?
                    (r.waterAmount(true) == g.dw && r.waterAmount(false) == g.sw) :
                    (r.drain_water_parts.empty() && r.spill_water_parts.empty() &&
                     r.drain_water == g.dw.value() && r.spill_water == g.sw.value())) &&
                    (retainsPointAggregates(arithmetic_) ?
                     (r.materialAmount(true) == g.dm && r.materialAmount(false) == g.sm) :
                     (r.drain_mass_parts.empty() && r.spill_mass_parts.empty() &&
                      r.drain_mass == g.dm.value() && r.spill_mass == g.sm.value())),
                "authenticated gross row differs from actual native legs");
    }
    for (const auto& [point, mapping] : points)
        require(gross.count(point) ? (changingVolume() ? gross.at(point).actual_si : gross.at(point).signed_si.value()) == mapping.exchange_si
                                  : mapping.exchange_si == 0, "authenticated hydraulic point differs");
    if (!changingVolume())
        for (const auto& [node, water] : hydraulic)
            require(water.zero(), "authenticated actual hydraulic booking is nonzero");
}

void PassiveStorageExchange::reserveImpl(const SimulationContext& ctx, std::uint64_t id,
                                       std::vector<Row> rows, Publication publication, const LiveProof* proof) {
    validate(ctx);
    require(!seal_ && (!funding_ || (proof && !funding_->reconciled && id == funding_->receipt &&
                publication.phase == funding_->phase && publication.batch_start_ms == funding_->start_ms &&
                publication.batch_end_ms == funding_->end_ms)), "exclusive funding blocks another material reservation");
    require(!changingVolume() || (owner_ && proof), "changing-volume input requires a bound authenticated owner");
    require(id > last_id_ && !rows.empty(), "replayed/empty receipt");
    if (publication.phase == Phase::ADVANCE) {
        require(frozen_ && frozen_->observed && ctx.elapsed_ms == frozen_->start_ms &&
                    ctx.coupling_delivery_remaining == frozen_->remaining_after,
                "ADVANCE changed the observed native delivery span or clock");
    } else {
        require(!frozen_ && !prepared_, "between-step receipt inside a native delivery handoff");
        require(!owner_ || (pending(receipts_) ? ctx.elapsed_ms == next_start_ms_ :
                    ctx.elapsed_ms >= next_start_ms_), "receipt is outside the next native interval");
        require(!pending(receipts_) || ctx.coupling_delivery_remaining == remaining_,
                "native delivery span changed while receipts were pending");
    }
    auto pools = pools_;
    for (const auto& [node, pool] : pools)
        validatePool(ctx, node, pool, false);
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.point < b.point; });
    if (proof)
        validateProof(ctx, rows, publication, *proof);
    std::map<int, ExactSum> water;
    std::set<std::uint64_t> points;
    for (const auto& r : rows) {
        require(points.insert(r.point).second, "duplicate point in receipt");
        require(r.node >= 0 && r.node < ctx.n_nodes(), "unknown storage node");
        require(r.pollutant == ctx.pollutant_names.names()[0] && r.units == ctx.pollutants.units[0],
                "pollutant identity or amount basis differs");
        for (double value : {r.drain_water, r.spill_water, r.drain_mass, r.spill_mass})
            require(std::isfinite(value) && value >= 0, "nonfinite/negative gross quantity");
        require((proof && retainsPointAggregates(arithmetic_)) ||
                    (r.drain_mass_parts.empty() && r.spill_mass_parts.empty()),
                "point material components require authenticated point policy");
        require((proof && retainsWaterAggregates()) ||
                    (r.drain_water_parts.empty() && r.spill_water_parts.empty()),
                "point water components require authenticated water policy");
        const auto drain = r.materialAmount(true), spill = r.materialAmount(false);
        require(pending(r) && (!r.waterAmount(true).zero() || drain.zero()) &&
                    (!r.waterAmount(false).zero() || spill.zero()), "material has no carrier");
        const auto ni = static_cast<std::size_t>(r.node);
        require(ni < ctx.nodes.conc.size() && ni < ctx.nodes.volume.size() && ni < ctx.nodes.depth.size(),
                "incomplete native pool shape");
        require(proof || r.spill_mass == r.spill_water * ctx.nodes.conc[r.node],
                "spill differs from its native published sample");
        for (const auto& receipt : receipts_)
            for (const auto& old : receipt.accepted) {
                require(old.point != r.point || old.node == r.node, "point changed native node identity");
                if (old.point == r.point && publication.phase != Phase::SETTLED &&
                    receipt.publication.phase != Phase::SETTLED)
                    require(publication.batch_start_ms >= receipt.publication.batch_end_ms ||
                                publication.batch_end_ms <= receipt.publication.batch_start_ms,
                            "physical point batch overlaps an accepted firing");
            }
        require(!changingVolume() || pools.count(r.node), "changing-volume receipt names an unbound pool");
        if (!pools.count(r.node))
            pools.emplace(r.node, Pool{ctx.nodes.volume[r.node], ctx.nodes.depth[r.node], ctx.nodes.conc[r.node]});
        validatePool(ctx, r.node, pools.at(r.node), false);
        addWater(water[r.node], r);
    }
    if (!changingVolume())
        for (const auto& [node, w] : water)
            require(w.zero(), "native node-summed water is not exactly zero net");
    auto receipts = receipts_;
    Receipt receipt{id, rows, rows, {}, publication};
    if (proof) { receipt.authenticated = true; receipt.proof = *proof; }
    if (exactSlices()) {
        require(proof, "exact slices require authenticated native legs");
        for (const auto& leg : proof->legs) {
            receipt.remaining_water_legs.push_back(leg.water_native);
            receipt.remaining_mass_legs.push_back(leg.mass_native);
        }
    }
    for (const auto& r : rows)
        receipt.samples.push_back(pools.at(r.node));
    receipts.push_back(std::move(receipt));
    for (const auto& [node, pool] : pools) {
        const auto sum = totals(receipts, node);
        const bool funded = retainsMaterialAggregates(arithmetic_) ?
            canFund(node, pool.volume * pool.concentration, sum.spill) :
            (std::isfinite(sum.drain.value()) && canFund(node, pool.volume * pool.concentration, sum.spill.value()));
        require(funded,
                "gross spill exceeds unencumbered native mass");
        auto roundoff = storedRoundoff(node);
        double mixer_amount;
        if (!changingVolume()) receivingConcentration(pool, sum.net, roundoff, mixer_amount);
    }
    auto node_ids = ctx.node_names.names();
    auto pollutant_ids = ctx.pollutant_names.names();
    if (funding_) {
        seal_ = PreparedSeal{std::move(receipts), std::move(pools), std::move(node_ids), std::move(pollutant_ids),
            id, frozen_ ? next_start_ms_ : ctx.elapsed_ms, frozen_ ? remaining_ : ctx.coupling_delivery_remaining};
        funding_->reconciled = true;
        return;
    }
    // All allocations and refusals precede this nonthrowing ownership commit.
    receipts_.swap(receipts);
    pools_.swap(pools);
    node_ids_.swap(node_ids);
    pollutant_ids_.swap(pollutant_ids);
    owner_ = &ctx;
    units_ = ctx.pollutants.units[0];
    last_id_ = id;
    if (!frozen_) {
        next_start_ms_ = ctx.elapsed_ms;
        remaining_ = ctx.coupling_delivery_remaining;
    }
}

void PassiveStorageExchange::freezeDelivery(const SimulationContext& ctx, double dt) {
    require(!funding_ && !seal_, "unresolved funding blocks quality advancement");
    if (frozen_) {
        fail();
        require(false, "delivery cohort already frozen");
    }
    validate(ctx);
    require(!prepared_ && std::isfinite(dt) && dt > 0 &&
                std::isfinite(ctx.elapsed_ms + 1000.0 * dt) &&
                ctx.elapsed_ms + 1000.0 * dt > ctx.elapsed_ms, "invalid frozen native interval");
    require(!owner_ || (pending(receipts_) ? ctx.elapsed_ms == next_start_ms_ :
                ctx.elapsed_ms >= next_start_ms_), "frozen native cursor differs");
    require(!pending(receipts_) || ctx.coupling_delivery_remaining == remaining_,
            "frozen native delivery span differs");
    for (const auto& [node, pool] : pools_)
        validatePool(ctx, node, pool, false);
    auto nodes = ctx.node_names.names();
    auto pollutants = ctx.pollutant_names.names();
    node_ids_.swap(nodes); pollutant_ids_.swap(pollutants);
    owner_ = &ctx; units_ = ctx.pollutants.units[0];
    next_start_ms_ = ctx.elapsed_ms; remaining_ = ctx.coupling_delivery_remaining;
    frozen_ = Frozen{ctx.elapsed_ms, dt, ctx.elapsed_ms + 1000.0 * dt,
                      remaining_, 0, last_id_, false};
    if (changingVolume()) frozen_->before = waterState(ctx);
}

void PassiveStorageExchange::observeDeliveryDrain(const SimulationContext& ctx) {
    if (!failed_ && frozen_)
        attempt_ = {frozen_->start_ms, frozen_->dt, frozen_->remaining_start};
    try {
        validate(ctx);
        require(frozen_ && !frozen_->observed && !prepared_, "missing or duplicate native drain observation");
        require(ctx.elapsed_ms == frozen_->start_ms &&
                    ctx.coupling_delivery_remaining == std::max(0.0, frozen_->remaining_start - frozen_->dt),
                "observed native lateral drain differs");
        frozen_->remaining_after = ctx.coupling_delivery_remaining;
        if (changingVolume()) {
            auto expected = frozen_->before;
            for (std::size_t k = 0; k < expected.queue.size(); ++k) {
                const double rate = expected.queue[k] / (frozen_->remaining_start > frozen_->dt ? frozen_->remaining_start : frozen_->dt);
                expected.queue[k] = frozen_->remaining_start > frozen_->dt ? expected.queue[k] - rate * frozen_->dt : 0;
                expected.rate[k] = rate;
            }
            frozen_->assembled = waterState(ctx);
            require(frozen_->assembled == expected, "observed native queue assembly differs");
        }
        frozen_->observed = true;
    } catch (...) {
        fail();
        throw;
    }
}

void PassiveStorageExchange::begin(const SimulationContext& ctx, double dt) {
    require(!funding_ && !seal_, "unresolved funding blocks quality advancement");
    if (!failed_)
        attempt_ = {ctx.elapsed_ms, dt, remaining_};
    validate(ctx);
    require(owner_ && (pending(receipts_) || frozen_), "no undelivered receipt");
    require(std::isfinite(dt) && dt > 0 && ctx.elapsed_ms == next_start_ms_ &&
                std::isfinite(ctx.elapsed_ms + 1000.0 * dt) &&
                ctx.elapsed_ms + 1000.0 * dt > ctx.elapsed_ms, "replayed or invalid native interval");
    if (frozen_)
        require(frozen_->observed && dt == frozen_->dt && ctx.elapsed_ms == frozen_->start_ms &&
                    ctx.coupling_delivery_remaining == (changingVolume() && !water_publications_.empty() &&
                        water_publications_.back().publication.phase == Phase::ADVANCE &&
                        water_publications_.back().publication.generation == deliveries_.size() ?
                        water_publications_.back().remaining_after : frozen_->remaining_after) && !pending_water_,
                "frozen native delivery handoff differs");
    else
        require(ctx.coupling_delivery_remaining == std::max(0.0, remaining_ - dt),
                "native post-assembly delivery span differs");
    for (const auto& [node, pool] : pools_)
        validatePool(ctx, node, pool, false);
}

std::vector<PassiveStorageExchange::Row> PassiveStorageExchange::exactHalfRows(
        const Receipt& receipt, double span, double dt) const {
    require(receipt.authenticated && receipt.half_state == 0 && std::isfinite(2*dt) && span == 2*dt &&
                receipt.remaining == receipt.accepted, "exact half requires an unsliced authenticated receipt and half span");
    const auto half = [&](double original) {
        const double slice = (original / span) * dt;
        const double remainder = original - slice;
        ExactSum partition; partition.add(slice); partition.add(remainder); partition.add(-original);
        require(std::isfinite(slice) && slice >= 0 && std::isfinite(remainder) && remainder >= 0 &&
                    slice == remainder && partition.zero(), "native leg does not partition into exact equal halves");
        return slice;
    };
    struct Gross { ExactSum dw, sw, dm, sm; };
    std::map<std::uint64_t, Gross> gross;
    for (const auto& leg : receipt.proof.legs) {
        const double water = half(leg.water_native), mass = half(leg.mass_native);
        auto& g = gross[leg.point];
        if (leg.drain) { g.dw.add(water); g.dm.add(mass); }
        else { g.sw.add(water); g.sm.add(mass); }
    }
    auto rows = receipt.accepted;
    for (auto& row : rows) {
        const auto& g = gross.at(row.point);
        for (bool drain : {true, false}) {
            const auto& amount = drain ? g.dw : g.sw;
            const auto& parts = drain ? row.drain_water_parts : row.spill_water_parts;
            if (parts.empty()) {
                require(amount.value() == half(drain ? row.drain_water : row.spill_water),
                        "native leg half water differs from the scalar point schedule");
            } else {
                auto partition = amount; partition.add(amount);
                const auto original = row.waterAmount(drain);
                for (double part : original.parts()) partition.add(-part);
                require(retainsWaterAggregates() && partition.zero(),
                        "native leg halves do not partition the exact point water");
            }
            row.setWaterAmount(drain, amount);
        }
        row.setMaterialAmount(true, g.dm); row.setMaterialAmount(false, g.sm);
    }
    return rows;
}

std::vector<PassiveStorageExchange::Row> PassiveStorageExchange::exactSliceRows(
        Receipt& receipt, double span, double dt) const {
    require(receipt.authenticated && receipt.half_state == 0 &&
                receipt.remaining_water_legs.size() == receipt.proof.legs.size() &&
                receipt.remaining_mass_legs.size() == receipt.proof.legs.size(),
            "exact slice leg ownership differs");
    const auto take = [&](double& before) {
        require(std::isfinite(before) && before >= 0, "invalid exact slice remainder");
        const double slice = span > dt ? (before / span) * dt : before;
        const double after = before - slice;
        ExactSum partition; partition.add(slice); partition.add(after); partition.add(-before);
        require(std::isfinite(slice) && slice >= 0 && slice <= before && partition.zero() &&
                    (!(span > dt && before > 0) || (slice > 0 && after > 0)),
                "native leg slice/remainder is not an exact nonvanishing partition");
        before = after; return slice;
    };
    struct Gross { ExactSum dw, sw, dm, sm; };
    std::map<std::uint64_t, Gross> delivered, remaining;
    for (std::size_t k = 0; k < receipt.proof.legs.size(); ++k) {
        const auto& leg = receipt.proof.legs[k];
        const double water = take(receipt.remaining_water_legs[k]);
        const double mass = take(receipt.remaining_mass_legs[k]);
        auto& d = delivered[leg.point]; auto& r = remaining[leg.point];
        if (leg.drain) {
            d.dw.add(water); d.dm.add(mass);
            r.dw.add(receipt.remaining_water_legs[k]); r.dm.add(receipt.remaining_mass_legs[k]);
        } else {
            d.sw.add(water); d.sm.add(mass);
            r.sw.add(receipt.remaining_water_legs[k]); r.sm.add(receipt.remaining_mass_legs[k]);
        }
    }
    auto rows = receipt.remaining;
    for (std::size_t k = 0; k < rows.size(); ++k) {
        auto& row = rows[k]; auto& rest = receipt.remaining[k];
        const auto& d = delivered.at(row.point); const auto& r = remaining.at(row.point);
        row.setWaterAmount(true, d.dw); row.setWaterAmount(false, d.sw);
        row.setMaterialAmount(true, d.dm); row.setMaterialAmount(false, d.sm);
        rest.setWaterAmount(true, r.dw); rest.setWaterAmount(false, r.sw);
        rest.setMaterialAmount(true, r.dm); rest.setMaterialAmount(false, r.sm);
    }
    return rows;
}

void PassiveStorageExchange::prepare(const SimulationContext& ctx, double dt) {
    if (changingVolume()) validateWaterTransition(ctx);
    for (const auto& [node, pool] : pools_)
        validatePool(ctx, node, pool, true);
    Prepared next;
    next.receipts = receipts_;
    next.roundoff = roundoff_;
    next.amount.assign(ctx.n_nodes(), 0.0);
    next.concentration.assign(ctx.n_nodes(), 0.0);
    next.affected.assign(ctx.n_nodes(), 0);
    if (changingVolume()) { next.volume = ctx.nodes.volume; next.depth = ctx.nodes.depth; }
    const auto cutoff = frozen_ ? frozen_->cutoff : last_id_;
    const double span = frozen_ ? frozen_->remaining_start : remaining_;
    std::map<int, ExactSum> amount, water;
    for (auto& receipt : next.receipts) {
        if (receipt.id > cutoff)
            continue;
        require(receipt.publication.eligible_ms <= ctx.elapsed_ms, "receipt is not yet eligible for assembly");
        if (exactSlices() && std::any_of(receipt.remaining.begin(), receipt.remaining.end(),
                                       [](const Row& r) { return pending(r); })) {
            const auto slice = exactSliceRows(receipt, span, dt);
            for (const auto& row : slice) {
                next.affected[row.node] = 1; addWater(water[row.node], row);
                amount[row.node].add(row.materialAmount(true));
                const auto spill = row.materialAmount(false);
                for (double part : spill.parts()) amount[row.node].add(-part);
            }
            continue;
        }
        if (arithmetic_ == PassiveStorageArithmetic::RETAIN_EXACT_HALVES && span > dt &&
            std::any_of(receipt.remaining.begin(), receipt.remaining.end(), [](const Row& r) { return pending(r); })) {
            auto half = exactHalfRows(receipt, span, dt);
            for (const auto& row : half) {
                next.affected[row.node] = 1;
                addWater(water[row.node], row);
                amount[row.node].add(row.materialAmount(true));
                const auto spill = row.materialAmount(false);
                for (double part : spill.parts()) amount[row.node].add(-part);
            }
            receipt.remaining.swap(half);
            receipt.half_state = 1;
            continue;
        }
        for (auto& r : receipt.remaining) {
            if (!pending(r))
                continue;
            next.affected[r.node] = 1;
            auto take = [&](double& amount) {
                const double before = amount;
                const double slice = span > dt ? (amount / span) * dt : amount;
                require(std::isfinite(slice) && slice >= 0 && slice <= amount,
                        "unrepresentable native slice");
                amount -= slice;
                ExactSum partition;
                partition.add(slice);
                partition.add(amount);
                partition.add(-before);
                require(partition.zero(), "native gross slice partition is not exact");
                return slice;
            };
            auto takeWater = [&](bool drain) {
                auto exact = r.waterAmount(drain);
                if (exact.parts().size() > 1) {
                    require(retainsWaterAggregates() && span <= dt, "multipart point water requires a whole native flush");
                    r.setWaterAmount(drain, NativeAmountSum{});
                    return exact;
                }
                auto& scalar = drain ? r.drain_water : r.spill_water;
                NativeAmountSum slice; slice.add(take(scalar)); return slice;
            };
            water[r.node].add(takeWater(true));
            const auto water_spill = takeWater(false);
            for (double part : water_spill.parts()) water[r.node].add(-part);
            auto takeMaterial = [&](bool drain) {
                auto exact = r.materialAmount(drain);
                if (exact.parts().size() > 1) {
                    require(span <= dt, "multipart point material requires a whole native flush");
                    r.setMaterialAmount(drain, NativeAmountSum{});
                    return exact;
                }
                auto& scalar = drain ? r.drain_mass : r.spill_mass;
                NativeAmountSum slice; slice.add(take(scalar)); return slice;
            };
            amount[r.node].add(takeMaterial(true));
            const auto spill = takeMaterial(false);
            for (double part : spill.parts()) amount[r.node].add(-part);
        }
    }
    for (const auto& [node, pool] : pools_) {
        require(changingVolume() || water[node].zero(), "native water slice is not exactly zero net");
        ExactSum remaining_water;
        for (const auto& receipt : next.receipts)
            for (const auto& r : receipt.remaining)
                if (r.node == node) {
                    addWater(remaining_water, r);
                }
        require(changingVolume() || remaining_water.zero(), "remaining gross water is not exactly zero net");
        auto roundoff = storedRoundoff(node);
        const double volume = changingVolume() ? ctx.nodes.volume[node] : pool.volume;
        if (changingVolume()) next.affected[node] = 1;
        const double concentration = changingVolume() ? changingConcentration(pool, volume, amount[node], roundoff, next.amount[node]) :
            receivingConcentration(pool, amount[node], roundoff, next.amount[node]);
        if (roundoff.zero()) next.roundoff.erase(node);
        else next.roundoff[node] = roundoff;
        const auto outstanding = totals(next.receipts, node);
        auto accountable = roundoff;
        accountable.add(concentration * volume);
        ExactSum physical; physical.add(concentration * volume);
        const bool funded = retainsMaterialAggregates(arithmetic_) ?
            (physical.atLeast(outstanding.spill) && accountable.atLeast(outstanding.spill)) :
            (std::isfinite(outstanding.drain.value()) && std::isfinite(outstanding.net.value()) &&
             outstanding.spill.value() <= concentration * volume && accountable.atLeast(outstanding.spill.value()));
        require(std::isfinite(concentration) && concentration >= 0 &&
                    std::isfinite(concentration * volume) &&
                    funded,
                "native slice cannot preserve funded pending inventory");
        next.concentration[node] = concentration;
    }
    next.end_ms = ctx.elapsed_ms + 1000.0 * dt;
    next.remaining = ctx.coupling_delivery_remaining;
    next.delivery = {ctx.elapsed_ms, dt, span, next.remaining, cutoff, frozen_.has_value()};
    if (changingVolume()) {
        next.delivery.drained_span = frozen_->remaining_after;
        next.delivery.before = frozen_->before; next.delivery.assembled = frozen_->assembled;
        next.delivery.received = waterState(ctx);
    }
    if (deliveries_.size() == deliveries_.capacity())
        deliveries_.reserve(std::max<std::size_t>(1, deliveries_.capacity() * 2));
    prepared_ = std::move(next);
}

void PassiveStorageExchange::commit(const SimulationContext& ctx) {
    require(prepared_.has_value(), "missing prepared native slice");
    for (const auto& [node, pool] : pools_)
        require(ctx.nodes.conc[node] == prepared_->concentration[node] &&
                    ctx.nodes.volume[node] == (changingVolume() ? prepared_->volume[node] : pool.volume) &&
                    ctx.nodes.depth[node] == (changingVolume() ? prepared_->depth[node] : pool.depth),
                "native consumer did not retain the prepared inventory");
    require(ctx.elapsed_ms == attempt_.start_ms && ctx.coupling_delivery_remaining == prepared_->remaining,
            "native consumer changed delivery clocks");
    for (auto it = pools_.begin(); it != pools_.end();) {
        const int node = it->first;
        const bool active = std::any_of(prepared_->receipts.begin(), prepared_->receipts.end(),
            [node](const Receipt& r) { return std::any_of(r.remaining.begin(), r.remaining.end(),
                [node](const Row& row) { return row.node == node && pending(row); }); });
        if (changingVolume() || active || prepared_->roundoff.count(node)) {
            it->second.concentration = ctx.nodes.conc[node];
            if (changingVolume()) { it->second.volume = ctx.nodes.volume[node]; it->second.depth = ctx.nodes.depth[node]; }
            ++it;
        } else {
            it = pools_.erase(it);
        }
    }
    receipts_.swap(prepared_->receipts);
    roundoff_.swap(prepared_->roundoff);
    next_start_ms_ = prepared_->end_ms;
    remaining_ = prepared_->remaining;
    static_assert(std::is_nothrow_move_constructible_v<Delivery>);
    deliveries_.push_back(std::move(prepared_->delivery));
    prepared_.reset();
    frozen_.reset();
}
double PassiveStorageExchange::changingConcentration(const Pool& pool, double volume,
        const NativeAmountSum& amount, NativeAmountSum& roundoff, double& mixer_amount) const {
    require(std::isfinite(volume) && volume > ZERO_VOLUME, "changing-volume receiver is dry");
    mixer_amount = amount.rounded();
    const double before = pool.volume * pool.concentration;
    const double next = (before + mixer_amount) / volume;
    const double actual = next * volume;
    auto target = amount; target.add(before);
    const double reference = target.rounded();
    const double unchanged = before / volume;
    require(!target.negative() && std::isfinite(next) && next >= 0 && std::isfinite(actual) && actual >= 0 &&
                (amount.zero() || (mixer_amount != 0 && next != unchanged && actual != unchanged * volume)),
            "changing-volume material increment disappeared or exhausted its donor");
    auto error = target; error.add(-actual);
    const double bound = (2 * std::numeric_limits<double>::epsilon()) * std::abs(reference);
    require(error.magnitudeAtMost(bound), "changing-volume local stored roundoff exceeds two-epsilon bound");
    roundoff.add(error);
    auto accountable = roundoff; accountable.add(actual);
    require(!accountable.negative(), "changing-volume accountable inventory is negative");
    return next;
}

double PassiveStorageExchange::pendingDrain(int node) const {
    return totals(receipts_, node).drain.value();
}
double PassiveStorageExchange::pendingSpill(int node) const {
    return totals(receipts_, node).spill.value();
}
NativeAmountSum PassiveStorageExchange::pendingDrainAmount(int node) const { return totals(receipts_, node).drain; }
NativeAmountSum PassiveStorageExchange::pendingSpillAmount(int node) const { return totals(receipts_, node).spill; }

const NativeAmountSum& PassiveStorageExchange::storedRoundoff(int node) const noexcept {
    static const NativeAmountSum zero;
    const auto it = roundoff_.find(node);
    return it == roundoff_.end() ? zero : it->second;
}
bool PassiveStorageExchange::canFund(int node, double physical, double spill) const noexcept {
    if (!std::isfinite(physical) || !std::isfinite(spill) || spill < 0 || spill > physical) return false;
    auto accountable = storedRoundoff(node);
    return accountable.tryAdd(physical) && accountable.atLeast(spill);
}
bool PassiveStorageExchange::canFund(int node, double physical, const NativeAmountSum& spill) const noexcept {
    if (!std::isfinite(physical) || physical < 0 || spill.negative()) return false;
    NativeAmountSum stock; stock.tryAdd(physical);
    auto accountable = storedRoundoff(node);
    return stock.atLeast(spill) && accountable.tryAdd(physical) && accountable.atLeast(spill);
}
double PassiveStorageExchange::receivingConcentration(const Pool& pool, double amount,
        NativeAmountSum& roundoff) const {
    if (arithmetic_ == PassiveStorageArithmetic::EXACT)
        return nativeConcentration(pool.volume, pool.concentration, amount);
    if (amount == 0) return pool.concentration;
    const double before = pool.volume * pool.concentration;
    // Same existing native operands. R is accounting only and never joins amount.
    const double target = before + amount;
    const double next = target / pool.volume;
    const double actual = next * pool.volume;
    require(target >= 0 && std::isfinite(target) && std::isfinite(next) && std::isfinite(actual) &&
                next != pool.concentration && actual != before,
            "stored increment is not representable within native division/product roundoff");
    NativeAmountSum error;
    error.add(before); error.add(amount); error.add(-actual);
    const double bound = (2 * std::numeric_limits<double>::epsilon()) * std::abs(target);
    require(error.magnitudeAtMost(bound), "native local stored roundoff exceeds two-epsilon bound");
    roundoff.add(before); roundoff.add(amount); roundoff.add(-actual);
    auto accountable = roundoff; accountable.add(actual);
    require(!accountable.negative(), "native accountable inventory is negative");
    return next;
}
double PassiveStorageExchange::receivingConcentration(const Pool& pool, const NativeAmountSum& amount,
        NativeAmountSum& roundoff, double& mixer_amount) const {
    if (!retainsMaterialAggregates(arithmetic_)) {
        mixer_amount = amount.value();
        return receivingConcentration(pool, mixer_amount, roundoff);
    }
    mixer_amount = amount.rounded();
    if (amount.zero()) return pool.concentration;
    const double before = pool.volume * pool.concentration;
    const double next = (before + mixer_amount) / pool.volume;
    const double actual = next * pool.volume;
    auto target = amount; target.add(before);
    const double reference = target.rounded();
    require(!target.negative() && reference >= 0 && next >= 0 && actual >= 0 && std::isfinite(next) && std::isfinite(actual) &&
                mixer_amount != 0 && next != pool.concentration && actual != before,
            "aggregate stored increment is not representable within native division/product roundoff");
    auto error = target; error.add(-actual);
    const double bound = (2 * std::numeric_limits<double>::epsilon()) * std::abs(reference);
    require(error.magnitudeAtMost(bound), "native exact aggregate stored roundoff exceeds two-epsilon bound");
    roundoff.add(error);
    auto accountable = roundoff; accountable.add(actual);
    require(!accountable.negative(), "native accountable inventory is negative");
    return next;
}
} // namespace openswmm::quality
