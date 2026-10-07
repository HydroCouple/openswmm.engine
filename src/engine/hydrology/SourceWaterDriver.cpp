// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
#include "SourceWaterDriver.hpp"
#include "../core/SimulationContext.hpp"
#include "../transport/components/LidLayerCommon.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace openswmm::runoff {
namespace {
constexpr double ft3 = .3048 * .3048 * .3048;
std::array<double, 4> layerWater(const lid::LIDGroupSoA& g, int u) {
    double v[transport::kNL]; transport::layerVolumes(g, u, v);
    std::array<double, 4> result;
    for (int k = 0; k < 4; ++k) result[k] = v[k] * g.area[u] * ft3;
    if (g.type == lid::LIDType::VEG_SWALE)
        result[transport::kSurf] = lid::completedUnitStorage(g, u) * ft3;
    return result;
}

// One finite interval of complete mixing per physical layer. Evaporation
// removes no solute. Internal transfers use the donor's mixture, and each
// transfer enters the next tank exactly once. This is a transport split over
// the completed water solve, not the ordinary outlet-concentration adapter.
void advanceLidPollutants(SourcePollutantLedger& b, const lid::LIDGroupSoA& g,
                         const PollutantData& p, const std::array<double, 4>& old,
                         double dt, const std::string& source_name) {
    const int u = b.unit;
    auto d = transport::donorsFor(g.type, g.soil_thick[u] > 0);
    // The completed barrel kernel retains overflow on its surface and sends
    // only StorageInflow to storage. The ordinary age map omits that buffer.
    if (g.type == lid::LIDType::RAIN_BARREL) d = {transport::kExternal, transport::kAbsent, transport::kAbsent, transport::kSurf};
    if (g.soil_thick[u] == 0) d.soil = transport::kAbsent;
    if (g.stor_thick[u] == 0) d.storage = transport::kAbsent;
    const int donor[4] = {d.surface, d.pavement, d.soil, d.storage};
    const double scale = g.area[u] * ft3 * dt;
    const double in[4] = {g.in_surf[u] * scale, d.pavement == transport::kAbsent ? 0 : g.in_pave[u] * scale,
        d.soil == transport::kAbsent ? 0 : g.in_soil[u] * scale,
        d.storage == transport::kAbsent ? 0 : g.in_stor[u] * scale};
    const double evap[4] = {g.evap_surf[u] * scale, g.evap_pave[u] * scale,
        g.evap_soil[u] * scale, g.evap_stor[u] * scale};
    const auto closing = layerWater(g, u);
    const double surface = g.surface_runoff[u] * scale, drain = g.drain_flow[u] * scale;
    const double infiltration = g.infil_loss[u] * g.area[u] * ft3;
    const double spatial = g.spatial_infil_loss[u] * g.area[u] * ft3;
    const int bottom = g.type == lid::LIDType::VEG_SWALE ? transport::kSurf :
        (d.storage == transport::kAbsent ? transport::kSoil : transport::kStor);
    const int drain_layer = g.type == lid::LIDType::ROOF_DISCON ? transport::kSurf : transport::kStor;
    double out[4] = {surface, 0, 0, 0};
    out[bottom] += infiltration; out[drain_layer] += drain;
    for (int k = 0; k < 4; ++k) if (donor[k] >= 0) out[donor[k]] += in[k];
    for (int k = 0; k < 4; ++k) {
        constexpr const char* layer_name[] = {"surface", "pavement", "soil", "storage"};
        const double tolerance = 1e-10 * std::max(1.0, old[k] + in[k]);
        if (!std::isfinite(old[k]) || !std::isfinite(in[k]) || !std::isfinite(evap[k]) ||
            !std::isfinite(out[k]) || !std::isfinite(closing[k]) ||
            old[k] < 0 || in[k] < 0 || evap[k] < 0 || out[k] < 0 || closing[k] < 0 ||
            std::abs(old[k] + in[k] - evap[k] - out[k] - closing[k]) > tolerance)
            throw std::runtime_error("Completed LID pollutant layer water budget does not close: source " +
                source_name + ", unit " + std::to_string(u) + ", layer " + layer_name[k]);
    }
    for (std::size_t j = 0; j < b.layers.size(); ++j) {
        std::array<double, 4> mixture{}, water{};
        // Transfer bounded mass fractions rather than forming a concentration
        // over a nearly dry tank; tiny roundoff water must not overflow it.
        const auto leaving = [&](int layer, double volume) {
            return water[layer] > 0 ? mixture[layer] * (volume / water[layer]) : 0;
        };
        const double rain = in[transport::kSurf] * p.c_rain[j];
        b.rain[j] += rain;
        for (int k = 0; k < 4; ++k) {
            const double arriving = donor[k] == transport::kExternal ? rain :
                donor[k] >= 0 ? leaving(donor[k], in[k]) : 0;
            const double available = b.layers[j][k] + arriving;
            water[k] = closing[k] + out[k]; mixture[k] = available;
            // A fully evaporated layer keeps dry pollutant mass for rewetting.
            b.layers[j][k] = water[k] > 0 ? available * (closing[k] / water[k]) : available;
            if (!std::isfinite(available) || !std::isfinite(b.layers[j][k]))
                throw std::runtime_error("Completed LID pollutant mixture is non-finite: source " + source_name);
        }
        b.spatial_infiltration[j] += leaving(bottom, spatial);
        b.outside_infiltration[j] += leaving(bottom, infiltration - spatial);
        const double drain_mass = leaving(drain_layer, drain);
        const double removal = g.drain_rmvl[u * g.n_pollutants + j];
        b.removed[j] += drain_mass * removal;
        b.surface_mass[j] = leaving(transport::kSurf, surface);
        b.drain_mass[j] = drain_mass * (1 - removal);
        b.outlet[j] += b.surface_mass[j] + b.drain_mass[j];
    }
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
    bool partitioned=false;
    std::vector<double> inside;
    std::vector<double> runon, pervious_return; // pending ft3, never held rates
    std::vector<SourceWaterLedger> ledger;
    std::vector<SourceWaterDelivery> deliveries;
    std::vector<SourceAtmosphericLedger> atmosphere;
    std::vector<std::string> pollutant_names;
    std::vector<SourcePollutantLedger> pollutants;
    explicit State(const SimulationContext& c) : ctx(c) {}
    State(const State& s) : ctx(s.ctx), runoff(s.runoff), lids(s.lids.waterTrial()),
        partitioned(s.partitioned),inside(s.inside),runon(s.runon), pervious_return(s.pervious_return), ledger(s.ledger), deliveries(s.deliveries), atmosphere(s.atmosphere),
        pollutant_names(s.pollutant_names), pollutants(s.pollutants) {}
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
const std::vector<double>& SourceWaterDriver::spatialFractions() const { return view(false).inside; }
std::string SourceWaterDriver::configureSpatialCoverage(const std::vector<double>& fractions) {
    if(!state_ || trial_ || state_->partitioned || fractions.size()!=static_cast<std::size_t>(state_->ctx.n_subcatches()))
        return "Invalid or already configured completed spatial footprint partition.";
    for(const auto& g:clocks_.groups())if(g.completed_end!=0 || !g.pending.empty())
        return "Spatial footprints must bind before any completed source interval.";
    for(double f:fractions)if(!std::isfinite(f)||f<0||f>1)
        return "Invalid completed spatial footprint fraction.";
    state_->inside=fractions;state_->partitioned=true;return {};
}
const SimulationContext& SourceWaterDriver::context(bool t) const { return view(t).ctx; }
const RunoffSolver& SourceWaterDriver::runoff(bool t) const { return view(t).runoff; }
const lid::LIDSolver& SourceWaterDriver::lids(bool t) const { return view(t).lids; }
const std::vector<SourceWaterLedger>& SourceWaterDriver::ledgers(bool t) const { return view(t).ledger; }
const std::vector<SourceWaterDelivery>& SourceWaterDriver::deliveries(bool t) const { return view(t).deliveries; }
const std::vector<SourceAtmosphericLedger>& SourceWaterDriver::atmosphere(bool t) const { return view(t).atmosphere; }
const std::vector<std::string>& SourceWaterDriver::pollutantNames() const { return view(false).pollutant_names; }
const std::vector<SourcePollutantLedger>& SourceWaterDriver::pollutants(bool t) const { return view(t).pollutants; }
double SourceWaterDriver::pollutantBalanceResidual(int group, int pollutant, bool t) const {
    const auto& s = view(t); (void)s.pollutant_names.at(pollutant);
    double residual = 0;
    for (const auto& b : s.pollutants) if (clocks_.groupForSubcatch(b.source) == group) {
        residual += b.rain[pollutant] - b.spatial_infiltration[pollutant] - b.outside_infiltration[pollutant] -
            b.outlet[pollutant] - b.removed[pollutant];
        for (double mass : b.layers[pollutant]) residual -= mass;
    }
    return residual;
}

double SourceWaterDriver::storage(const State& s, int sc) const {
    const auto& r = s.runoff.soa();
    double v = r.area[sc] * (r.depth_imperv0[sc] * r.frac_imperv0[sc] +
        r.depth_imperv1[sc] * r.frac_imperv1[sc] + r.depth_perv[sc] * (1 - r.imperv_pct[sc]));
    for (const auto& [t, u] : s.lids.usageOrder()) {
        const auto& g = s.lids.group(t);
        if (g.subcatch_idx[u] == sc) v += lid::completedUnitStorage(g,u);
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
    if (c.options.water_age ||
        c.options.heat_transport || c.reactions.n_species() > 0)
        return "Source water driver does not yet pair age, heat or MSX.";
    const int np = c.options.ignore_quality ? 0 : c.n_pollutants();
    if (np > 0) {
        if (c.n_landuses() > 0) return "Completed source pollutants do not yet support land-use buildup/washoff.";
        const auto& p = c.pollutants;
        if (p.n_pollutants() != np || p.c_rain.size() != np || p.k_decay.size() != np ||
            p.co_pollut.size() != np || p.co_frac.size() != np || p.snow_only.size() != np)
            return "Completed source pollutant definitions do not match their IDs.";
        for (int j = 0; j < np; ++j) {
            if (!std::isfinite(p.c_rain[j]) || p.c_rain[j] < 0)
                return "Invalid completed source rainfall concentration: " + c.pollutant_names.name_of(j);
            if (p.k_decay[j] != 0 || p.co_pollut[j] >= 0 || p.co_frac[j] != 0 || p.snow_only[j])
                return "Completed source pollutants require conservative rain species: " + c.pollutant_names.name_of(j);
        }
    }
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
        s->inside.assign(n,1);s->runon.assign(n, 0); s->pervious_return.assign(n, 0); s->ledger.resize(n);
        for (const auto& group : clocks.groups()) {
            for (int sc : group.subcatches) {
                if (c.subcatches.gw_aquifer[sc] >= 0)
                    return "Source water driver does not yet advance lumped groundwater.";
                if (c.subcatches.runon_rate[sc] != 0 || c.subcatches.lid_return_to_perv_cfs[sc] != 0)
                    return "Source water driver requires empty initial transfer histories.";
                s->ledger[sc].initial_storage = storage(*s, sc);
                const auto& r = s->runoff.soa();
                if (np > 0 && (r.area[sc] > 0 || c.subcatches.outlet_subcatch[sc] >= 0))
                    return "Completed pollutants require LID-only sources without inter-subcatchment outlets: " + c.subcatch_names.name_of(sc);
                for (auto kind : {SourceEtKind::PERVIOUS, SourceEtKind::IMPERVIOUS}) {
                    const bool pervious = kind == SourceEtKind::PERVIOUS;
                    const double area = r.area[sc] * (pervious ? 1 - r.imperv_pct[sc] : r.imperv_pct[sc]);
                    if (area > 0) s->atmosphere.push_back({kind, sc, -1, -1, area, pervious});
                }
            }
            for (const auto& [t, u] : s->lids.usageOrder()) {
                const auto& g = s->lids.group(t); const int sc = g.subcatch_idx[u];
                if (clocks.groupForSubcatch(sc) < 0) continue;
                if(g.type==lid::LIDType::VEG_SWALE && (g.surf_store[u]<=0 || g.surf_void_frac[u]<=0 || g.unit_area[u]<=0))
                    return "Completed swale requires positive surface height, void fraction and unit area.";
                const double perv = s->runoff.soa().area[sc] * (1 - c.subcatches.frac_imperv[sc]);
                if (g.type == lid::LIDType::RAIN_BARREL && g.stor_covered[u] && perv <= 0)
                    return "Covered barrel rainfall requires a non-LID pervious return area.";
            }
        }
        for (const auto& [t, u] : s->lids.usageOrder()) {
            const auto& g = s->lids.group(t); const int sc = g.subcatch_idx[u];
            if (clocks.groupForSubcatch(sc) >= 0 && g.area[u] > 0)
                s->atmosphere.push_back({SourceEtKind::LID, sc, t, u, g.area[u],
                    lid::hasNativeBottom(g.type,g.stor_thick[u],g.stor_ksat[u])});
            if (np > 0 && clocks.groupForSubcatch(sc) >= 0 && g.area[u] > 0) {
                if (g.from_imperv[u] != 0 || g.from_perv[u] != 0 || g.to_perv[u] ||
                    g.drain_subcatch[u] >= 0 || g.stor_covered[u])
                    return "Completed LID pollutant capture/returns/covered storage are not qualified: " + c.subcatch_names.name_of(sc);
                if (g.n_pollutants != np || g.drain_rmvl.size() != std::size_t(g.count * np))
                    return "Completed LID pollutant drain removal layout is invalid.";
                for (int j = 0; j < np; ++j) {
                    const double f = g.drain_rmvl[u * np + j];
                    if (!std::isfinite(f) || f < 0 || f > 1) return "Invalid completed LID pollutant drain removal fraction.";
                }
                SourcePollutantLedger b; b.source = sc; b.type = t; b.unit = u;
                b.layers.resize(np); b.rain.assign(np, 0); b.spatial_infiltration.assign(np, 0);
                b.outside_infiltration.assign(np, 0); b.outlet.assign(np, 0); b.removed.assign(np, 0);
                b.surface_mass.assign(np, 0); b.drain_mass.assign(np, 0); s->pollutants.push_back(std::move(b));
            }
        }
        for (int j = 0; j < np; ++j) s->pollutant_names.push_back(c.pollutant_names.name_of(j));
    } catch (const std::exception& e) { return e.what(); }
    state_ = std::move(s); clocks_ = std::move(clocks); return {};
}

void SourceWaterDriver::advance(State& s, const SourceForcingInterval& f,
                               const RunoffSolver::InfiltrationBoundary* boundary,
                               const BottomCeiling* ceiling) {
    const double dt = f.end - f.start; auto& c = s.ctx;
    if (s.partitioned && !ceiling) {
        for (const auto& [t, u] : s.lids.usageOrder()) {
            const auto& g = s.lids.group(t);
            if (s.inside[g.subcatch_idx[u]] > 0 && g.area[u] > 0 &&
                lid::hasNativeBottom(g.type, g.stor_thick[u], g.stor_ksat[u]) &&
                std::any_of(f.sources.begin(), f.sources.end(), [&](const auto& w) {
                    return w.subcatch == g.subcatch_idx[u];
                }))
                throw std::runtime_error("Completed inside native LID area requires a spatial bottom ceiling.");
        }
    }
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
    s.runoff.execute(c, dt, 0, f.infil_factor, f.recovery_factor, f.month, boundary, &f.sources,s.partitioned ? &s.inside : nullptr);
    const auto& r = s.runoff.soa();
    std::vector<double> outlet(c.n_subcatches(), 0), native(c.n_subcatches(), 0);
    for (const auto& weather : f.sources) {
        const int sc = weather.subcatch; outlet[sc] = r.outflow_vol[sc];
        s.ledger[sc].evaporation += c.subcatches.evap_loss[sc] * r.area[sc] * dt;
        s.ledger[sc].infiltration += r.infil_vol[sc];
        s.ledger[sc].spatial_infiltration+=r.spatial_infil_vol[sc];
        s.ledger[sc].outside_infiltration+=r.infil_vol[sc]-r.spatial_infil_vol[sc];
        if (c.subcatches.total_lid_area_ft2[sc] > 0) {
            if (s.partitioned) {
                if(s.inside[sc]<1) {
                    if(r.area[sc]>0 && r.imperv_pct[sc]<1)
                        native[sc]=r.native_infil_rate[sc]*(1-r.imperv_pct[sc]);
                    else native[sc]=s.runoff.nativeInfilFullLid(c,sc,dt,f.recovery_factor);
                }
            } else if (r.area[sc] > 0 && r.imperv_pct[sc] < 1)
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
        const double bottom=ceiling ? (*ceiling)(t,u,f.start,f.end) : 1.0e10;
        inputs.push_back({t, u, captured / dt / g.area[u] + rain + c.subcatches.runon_rate[sc],
            weather->rain, weather->pet, native[sc], s.partitioned ? 1.0e10 : bottom,
            factor(c, sc, f),s.partitioned ? s.inside[sc] : -1.0,bottom});
    }
    std::sort(inputs.begin(), inputs.end(), [](const auto& a, const auto& b) {
        return std::pair{a.type, a.unit} < std::pair{b.type, b.unit};
    });
    for (const auto& weather : f.sources)
        if (outlet[weather.subcatch] < -1e-12)
            throw std::runtime_error("LID capture exceeds the available non-LID outlet volume.");
    std::vector<std::array<double, 4>> old_layers;
    for (const auto& b : s.pollutants) old_layers.push_back(layerWater(s.lids.group(b.type), b.unit));
    s.lids.executeCompleted(dt, f.start, f.recovery_factor, inputs);
    for (std::size_t j = 0; j < s.pollutants.size(); ++j) {
        auto& b = s.pollutants[j];
        if (selected(b.source)) advanceLidPollutants(b, s.lids.group(b.type), c.pollutants, old_layers[j], dt,
            c.subcatch_names.name_of(b.source));
    }
    for (auto& b : s.atmosphere) {
        const auto* weather = weather_at[b.source]; if (!weather) continue;
        const double potential = weather->pet * dt * b.area;
        double evaporation = r.actual_perv_evap_vol[b.source];
        if (b.kind == SourceEtKind::IMPERVIOUS)
            evaporation = c.subcatches.evap_loss[b.source] * r.area[b.source] * dt - evaporation;
        else if (b.kind == SourceEtKind::LID)
            evaporation = s.lids.group(b.type).evap_loss[b.unit] * b.area;
        const double tolerance = 1e-12 * std::max(1.0, potential);
        if (!std::isfinite(potential) || !std::isfinite(evaporation) || potential < 0 ||
            evaporation < -tolerance || evaporation > potential + tolerance)
            throw std::runtime_error("Completed source component exceeds its atmospheric footprint budget.");
        b.potential += potential; b.evaporation += std::clamp(evaporation, 0.0, potential);
    }
    std::vector<std::vector<double>> outlet_mass(c.n_subcatches(), std::vector<double>(s.pollutant_names.size(), 0));
    const auto route = [&](int sc, int node, int target, double volume, bool drain, const std::vector<double>& mass) {
        if (!std::isfinite(volume) || volume < -1e-12)
            throw std::runtime_error("Source capture/output produced invalid water volume.");
        volume = std::max(volume, 0.0);
        if (node < 0 && target >= 0 && target != sc) {
            if (!selected(target)) throw std::runtime_error("Transfer escaped its source clock group.");
            s.runon[target] += volume;
        } else if (volume > 0) {
            s.deliveries.push_back({sc, node, f.start, f.end, volume, drain, mass}); s.ledger[sc].outlet += volume;
        }
    };
    // Preserve parse order for accumulations. A pervious return is a pending
    // volume, not rainfall, and drains retain the legacy target precedence.
    for (const auto& [t, u] : s.lids.usageOrder()) {
        auto& g = s.lids.group(t); const int sc = g.subcatch_idx[u];
        if (!selected(sc) || g.area[u] <= 0) continue;
        s.ledger[sc].evaporation += g.evap_loss[u] * g.area[u];
        s.ledger[sc].infiltration += g.infil_loss[u] * g.area[u];
        s.ledger[sc].spatial_infiltration+=g.spatial_infil_loss[u]*g.area[u];
        s.ledger[sc].outside_infiltration+=(g.infil_loss[u]-g.spatial_infil_loss[u])*g.area[u];
        double surface = g.surface_runoff[u] * g.area[u] * dt;
        double drain = g.drain_flow[u] * g.area[u] * dt;
        std::vector<double> drain_mass;
        for (const auto& b : s.pollutants) if (b.type == t && b.unit == u) {
            drain_mass = b.drain_mass;
            for (std::size_t j = 0; j < b.layers.size(); ++j) outlet_mass[sc][j] += b.surface_mass[j];
        }
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
        route(sc, node, target, drain, true, drain_mass);
        g.drain_open[u] = g.drain_flow[u] > 0; g.old_drain_flow[u] = g.drain_flow[u];
    }
    for (const auto& weather : f.sources) {
        const int sc = weather.subcatch;
        route(sc, c.subcatches.outlet_node[sc], c.subcatches.outlet_subcatch[sc], outlet[sc], false, outlet_mass[sc]);
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
        for (std::size_t j = 0; j < trial_->pollutant_names.size(); ++j) {
            double mass_scale = 1;
            for (const auto& b : trial_->pollutants)
                if (clocks_.groupForSubcatch(b.source) == group) mass_scale += b.rain[j];
            const double mass_residual = pollutantBalanceResidual(group, int(j), true);
            if (!std::isfinite(mass_residual) || std::abs(mass_residual) > 1e-10 * mass_scale)
                throw std::runtime_error("Completed LID pollutant trial failed mass conservation: " + trial_->pollutant_names[j]);
        }
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
