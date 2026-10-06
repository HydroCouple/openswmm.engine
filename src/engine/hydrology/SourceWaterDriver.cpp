// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
#include "SourceWaterDriver.hpp"
#include "../core/SimulationContext.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace openswmm::runoff {
namespace {
double unitStorage(const lid::LIDGroupSoA& g, int u) {
    return (g.surf_depth[u] * g.surf_void_frac[u] + g.soil_moist[u] * g.soil_thick[u] +
            g.stor_depth[u] * g.stor_void[u] +
            g.pave_depth[u] * g.pave_void[u] * (1 - g.pave_imperv_frac[u])) * g.area[u];
}
double factor(const SimulationContext& c, int sc, const SourceForcingInterval& f) {
    if (static_cast<std::size_t>(sc) < c.subcatch_infil_pattern.size()) {
        const int p = c.subcatch_infil_pattern[sc];
        if (p >= 0 && static_cast<std::size_t>(p) < c.patterns.factors.size() &&
            static_cast<std::size_t>(f.month) < c.patterns.factors[p].size())
            return c.patterns.factors[p][f.month];
    }
    return f.infil_factor;
}
}
struct SourceWaterDriver::State {
    SimulationContext ctx;
    RunoffSolver runoff;
    lid::LIDSolver lids;
    std::vector<double> runon, pervious_return; // pending ft3, never held rates
    std::vector<SourceWaterLedger> ledger;
    std::vector<SourceWaterDelivery> deliveries;
    explicit State(const SimulationContext& c) : ctx(c) {}
    State(const State& s) : ctx(s.ctx), runoff(s.runoff), lids(s.lids.waterTrial()),
        runon(s.runon), pervious_return(s.pervious_return), ledger(s.ledger), deliveries(s.deliveries) {}
};
SourceWaterDriver::SourceWaterDriver() = default;
SourceWaterDriver::~SourceWaterDriver() = default;
SourceWaterDriver::SourceWaterDriver(SourceWaterDriver&&) noexcept = default;
SourceWaterDriver& SourceWaterDriver::operator=(SourceWaterDriver&&) noexcept = default;
SourceWaterDriver SourceWaterDriver::waterTrial() const {
    if (!state_ || trial_) throw std::logic_error("Source snapshot requires committed water state.");
    SourceWaterDriver copy; copy.state_ = std::make_unique<State>(*state_); copy.clocks_ = clocks_;
    return copy;
}
const SourceWaterDriver::State& SourceWaterDriver::view(bool trial) const {
    const auto* s = trial ? trial_.get() : state_.get();
    if (!s) throw std::logic_error("No source water state for this view.");
    return *s;
}
const SimulationContext& SourceWaterDriver::context(bool t) const { return view(t).ctx; }
const RunoffSolver& SourceWaterDriver::runoff(bool t) const { return view(t).runoff; }
const lid::LIDSolver& SourceWaterDriver::lids(bool t) const { return view(t).lids; }
const std::vector<SourceWaterLedger>& SourceWaterDriver::ledgers(bool t) const { return view(t).ledger; }
const std::vector<SourceWaterDelivery>& SourceWaterDriver::deliveries(bool t) const { return view(t).deliveries; }

double SourceWaterDriver::storage(const State& s, int sc) const {
    const auto& r = s.runoff.soa();
    double v = r.area[sc] * (r.depth_imperv0[sc] * r.frac_imperv0[sc] +
        r.depth_imperv1[sc] * r.frac_imperv1[sc] + r.depth_perv[sc] * (1 - r.imperv_pct[sc]));
    for (const auto& [t, u] : s.lids.usageOrder()) {
        const auto& g = s.lids.group(t);
        if (g.subcatch_idx[u] == sc) v += unitStorage(g, u);
    }
    return v;
}
double SourceWaterDriver::pendingVolume(int group, bool trial) const {
    const auto& s = view(trial); double v = 0;
    for (int sc : clocks_.groups().at(group).subcatches)
        v += s.runon[sc] + s.pervious_return[sc] + s.runoff.pendingRoutingVolume(s.ctx, sc);
    return v;
}
double SourceWaterDriver::balanceResidual(int group, bool trial) const {
    const auto& s = view(trial); double v = -pendingVolume(group, trial);
    for (int sc : clocks_.groups().at(group).subcatches) {
        const auto& b = s.ledger[sc];
        v += b.initial_storage + b.rain - b.evaporation - b.infiltration - b.outlet - storage(s, sc);
    }
    return v;
}
std::string SourceWaterDriver::initialize(const SimulationContext& c, const std::vector<int>& seeds,
                                         const std::vector<std::pair<int, double>>& areas,
                                         const lid::LIDSolver* initial_lids) {
    if (state_) return "Source water driver already initialized; restart/import is not qualified.";
    if (c.current_time != 0.0 || c.elapsed_ms != 0.0)
        return "Source water driver requires simulation-start state; hot starts are not qualified.";
    if ((!c.options.ignore_quality && c.n_pollutants() > 0) || c.options.water_age ||
        c.options.heat_transport || c.reactions.n_species() > 0)
        return "Source water driver does not yet pair quality, age, heat or MSX.";
    SourceClockGroups clocks; auto error = clocks.initialize(c, seeds);
    if (!error.empty()) return error;
    auto s = std::make_unique<State>(c);
    try {
        s->lids.init(s->ctx);
        if (initial_lids) {
            if (initial_lids->numGroups() != s->lids.numGroups() ||
                initial_lids->usageOrder() != s->lids.usageOrder())
                return "Initial LID water snapshot does not match source model.";
            for (int t = 0; t < s->lids.numGroups(); ++t) {
                const auto& a = initial_lids->group(t); const auto& b = s->lids.group(t);
                if (a.area != b.area || a.subcatch_idx != b.subcatch_idx || a.control_idx != b.control_idx)
                    return "Initial LID water snapshot does not match source model.";
            }
            s->lids = initial_lids->waterTrial();
        }
        // The ordinary LID initializer snaps near-full coverage to a rounded
        // LANDAREA denominator. Completed physical-area budgets use the actual
        // unit footprints, together with the reviewed non-LID area.
        std::fill(s->ctx.subcatches.total_lid_area_ft2.begin(), s->ctx.subcatches.total_lid_area_ft2.end(), 0);
        for (const auto& [t, u] : s->lids.usageOrder()) {
            const auto& g = s->lids.group(t);
            s->ctx.subcatches.total_lid_area_ft2[g.subcatch_idx[u]] += g.area[u];
        }
        s->runoff.init(s->ctx, areas);
        const auto n = static_cast<std::size_t>(c.n_subcatches());
        s->runon.assign(n, 0); s->pervious_return.assign(n, 0); s->ledger.resize(n);
        for (const auto& group : clocks.groups()) {
            for (int sc : group.subcatches) {
                if (c.subcatches.gw_aquifer[sc] >= 0)
                    return "Source water driver does not yet advance lumped groundwater.";
                if (c.subcatches.runon_rate[sc] != 0 || c.subcatches.lid_return_to_perv_cfs[sc] != 0)
                    return "Source water driver requires empty initial transfer histories.";
                s->ledger[sc].initial_storage = storage(*s, sc);
            }
            for (const auto& [t, u] : s->lids.usageOrder()) {
                const auto& g = s->lids.group(t); const int sc = g.subcatch_idx[u];
                if (clocks.groupForSubcatch(sc) < 0) continue;
                if (g.type != lid::LIDType::INFIL_TRENCH && g.type != lid::LIDType::RAIN_BARREL)
                    return "Source water driver currently qualifies storage trenches and rain barrels only.";
                const double perv = s->runoff.soa().area[sc] * (1 - c.subcatches.frac_imperv[sc]);
                if (g.type == lid::LIDType::RAIN_BARREL && g.stor_covered[u] && perv <= 0)
                    return "Covered barrel rainfall requires a non-LID pervious return area.";
            }
        }
    } catch (const std::exception& e) { return e.what(); }
    state_ = std::move(s); clocks_ = std::move(clocks); return {};
}

void SourceWaterDriver::advance(State& s, const SourceForcingInterval& f,
                               const RunoffSolver::InfiltrationBoundary* boundary,
                               const BottomCeiling* ceiling) {
    const double dt = f.end - f.start; auto& c = s.ctx;
    std::vector<const RunoffSourceForcing*> weather_at(c.n_subcatches(), nullptr);
    for (const auto& weather : f.sources) weather_at[weather.subcatch] = &weather;
    const auto selected = [&](int sc) { return sc >= 0 && sc < c.n_subcatches() && weather_at[sc]; };
    // Consume each pending volume once. Runon is distributed across the full
    // non-LID + LID footprint, so partial LID coverage cannot multiply it.
    for (const auto& weather : f.sources) {
        const int sc = weather.subcatch;
        const double full = s.runoff.soa().area[sc] + c.subcatches.total_lid_area_ft2[sc];
        if (s.runon[sc] > 0 && full <= 0) throw std::runtime_error("Runon receiver has no surface area.");
        c.subcatches.runon_rate[sc] = full > 0 ? s.runon[sc] / dt / full : 0;
        c.subcatches.lid_return_to_perv_cfs[sc] = s.pervious_return[sc] / dt;
        s.ledger[sc].runon += s.runon[sc]; s.ledger[sc].pervious_return += s.pervious_return[sc];
        s.runon[sc] = 0; s.pervious_return[sc] = 0;
        s.ledger[sc].rain += weather.rain * dt * full;
    }
    s.runoff.execute(c, dt, 0, f.infil_factor, f.recovery_factor, f.month, boundary, &f.sources);
    const auto& r = s.runoff.soa();
    std::vector<double> outlet(c.n_subcatches(), 0), native(c.n_subcatches(), 0);
    for (const auto& weather : f.sources) {
        const int sc = weather.subcatch; outlet[sc] = r.outflow_vol[sc];
        s.ledger[sc].evaporation += c.subcatches.evap_loss[sc] * r.area[sc] * dt;
        s.ledger[sc].infiltration += r.infil_vol[sc];
        if (c.subcatches.total_lid_area_ft2[sc] > 0) {
            if (r.area[sc] > 0 && r.imperv_pct[sc] < 1)
                native[sc] = r.infil_vol[sc] / r.area[sc] / dt;
            else native[sc] = s.runoff.nativeInfilFullLid(c, sc, dt, f.recovery_factor);
        }
    }
    std::vector<lid::LIDSolver::CompletedUnitInput> inputs;
    for (const auto& [t, u] : s.lids.usageOrder()) {
        const auto& g = s.lids.group(t); const int sc = g.subcatch_idx[u];
        if (!selected(sc) || g.area[u] <= 0) continue;
        const auto* weather = weather_at[sc];
        const double captured = (r.imperv_runoff_cfs[sc] * g.from_imperv[u] +
                                 r.perv_runoff_cfs[sc] * g.from_perv[u]) * dt;
        outlet[sc] -= captured; s.ledger[sc].captured += captured;
        double rain = weather->rain;
        if (g.type == lid::LIDType::RAIN_BARREL && g.stor_covered[u]) {
            s.pervious_return[sc] += rain * g.area[u] * dt; rain = 0;
        }
        inputs.push_back({t, u, captured / dt / g.area[u] + rain + c.subcatches.runon_rate[sc],
            weather->rain, weather->pet, native[sc], ceiling ? (*ceiling)(t, u, f.start, f.end) : 1.0e10,
            factor(c, sc, f)});
    }
    std::sort(inputs.begin(), inputs.end(), [](const auto& a, const auto& b) {
        return std::pair{a.type, a.unit} < std::pair{b.type, b.unit};
    });
    for (const auto& weather : f.sources)
        if (outlet[weather.subcatch] < -1e-12)
            throw std::runtime_error("LID capture exceeds the available non-LID outlet volume.");
    s.lids.executeCompleted(dt, f.start, f.recovery_factor, inputs);
    const auto route = [&](int sc, int node, int target, double volume, bool drain) {
        if (!std::isfinite(volume) || volume < -1e-12)
            throw std::runtime_error("Source capture/output produced invalid water volume.");
        volume = std::max(volume, 0.0);
        if (node < 0 && target >= 0 && target != sc) {
            if (!selected(target)) throw std::runtime_error("Transfer escaped its source clock group.");
            s.runon[target] += volume;
        } else if (volume > 0) {
            s.deliveries.push_back({sc, node, f.start, f.end, volume, drain}); s.ledger[sc].outlet += volume;
        }
    };
    // Preserve parse order for accumulations. A pervious return is a pending
    // volume, not rainfall, and drains retain the legacy target precedence.
    for (const auto& [t, u] : s.lids.usageOrder()) {
        auto& g = s.lids.group(t); const int sc = g.subcatch_idx[u];
        if (!selected(sc) || g.area[u] <= 0) continue;
        s.ledger[sc].evaporation += g.evap_loss[u] * g.area[u];
        s.ledger[sc].infiltration += g.infil_loss[u] * g.area[u];
        double surface = g.surface_runoff[u] * g.area[u] * dt;
        double drain = g.drain_flow[u] * g.area[u] * dt;
        const bool can_return = g.to_perv[u] && r.area[sc] * (1 - r.imperv_pct[sc]) > 0;
        const bool to_outlet = (g.drain_node[u] < 0 && g.drain_subcatch[u] < 0) ||
            (g.drain_node[u] == c.subcatches.outlet_node[sc] && g.drain_subcatch[u] == c.subcatches.outlet_subcatch[sc]);
        if (can_return) {
            s.pervious_return[sc] += surface;
            if (to_outlet) { s.pervious_return[sc] += drain; drain = 0; g.drain_flow[u] = 0; }
        } else outlet[sc] += surface;
        int node = g.drain_node[u], target = g.drain_subcatch[u];
        if (node < 0 && (target < 0 || target == sc)) {
            node = c.subcatches.outlet_node[sc]; target = c.subcatches.outlet_subcatch[sc];
        }
        route(sc, node, target, drain, true);
        g.drain_open[u] = g.drain_flow[u] > 0; g.old_drain_flow[u] = g.drain_flow[u];
    }
    for (const auto& weather : f.sources) {
        const int sc = weather.subcatch;
        route(sc, c.subcatches.outlet_node[sc], c.subcatches.outlet_subcatch[sc], outlet[sc], false);
        c.subcatches.runoff[sc] = std::max(outlet[sc], 0.0) / dt;
        c.subcatches.runon_rate[sc] = 0;
    }
}

std::string SourceWaterDriver::stage(int group, double end, double completed_through,
                                    const RunoffSolver::InfiltrationBoundary* boundary,
                                    const BottomCeiling* ceiling) {
    if (!state_) return "Source water driver is not initialized.";
    if (trial_) return "Source water driver already has an unsettled trial.";
    auto error = clocks_.stage(group, end, completed_through); if (!error.empty()) return error;
    trial_ = std::make_unique<State>(*state_); pending_group_ = group;
    try {
        for (const auto& f : clocks_.groups()[group].pending) advance(*trial_, f, boundary, ceiling);
        double scale = 1;
        for (int sc : clocks_.groups()[group].subcatches)
            scale += trial_->ledger[sc].initial_storage + trial_->ledger[sc].rain;
        const double residual = balanceResidual(group, true);
        if (!std::isfinite(residual) || std::abs(residual) > 1e-10 * scale)
            throw std::runtime_error("Completed source water trial failed storage/transfer conservation.");
    } catch (const std::exception& e) { error = e.what(); cancel(); return error; }
    return {};
}
std::string SourceWaterDriver::commit() {
    if (!trial_) return "No completed source water trial to commit.";
    auto error = clocks_.commit(pending_group_); if (!error.empty()) return error;
    state_.swap(trial_); trial_.reset(); pending_group_ = -1; return {};
}
void SourceWaterDriver::cancel() {
    if (pending_group_ >= 0) clocks_.cancel(pending_group_);
    trial_.reset(); pending_group_ = -1;
}
}
