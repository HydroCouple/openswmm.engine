// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
#include "SourceWaterDriver.hpp"
#include "../core/CompletedSourceArchive.hpp"
#include "../core/SimulationContext.hpp"
#include "../core/UnitConversion.hpp"
#include "surface/UebSubcatchments.hpp"
#include "../transport/components/LidLayerCommon.hpp"
#include <algorithm>
#include <cmath>
#include <sstream>
#include <iomanip>
#include <stdexcept>

namespace openswmm::runoff {
namespace {
constexpr double ft3 = .3048 * .3048 * .3048;
SourcePollutantLedger pollutantLedger(int source, int type, int unit, int np) {
    SourcePollutantLedger b; b.source = source; b.type = type; b.unit = unit;
    b.layers.resize(np); b.rain.assign(np, 0); b.spatial_infiltration.assign(np, 0);
    b.outside_infiltration.assign(np, 0); b.outlet.assign(np, 0); b.removed.assign(np, 0);
    b.surface_mass.assign(np, 0); b.drain_mass.assign(np, 0);
    b.impervious_mass.assign(np, 0); b.pervious_mass.assign(np, 0);
    b.aquifer_return.assign(np,0);b.pending_routing.assign(np, 0); b.incoming.assign(np, 0); return b;
}

void advanceNonLidPollutants(SourcePollutantLedger& b, const std::array<CompletedSubareaWater, 3>& flux,
                            const SimulationContext& c, double full,
                            const std::vector<double>& runon, const std::vector<double>& pervious_return,
                            const std::vector<std::array<double,3>>& aquifer_return) {
    for (const auto& w : flux) {
        for (double v : {w.opening, w.rain, w.runon, w.evaporation, w.infiltration,
                         w.spatial_infiltration, w.outflow, w.outlet, w.closing})
            if (!std::isfinite(v) || v < 0) throw std::runtime_error("Invalid completed non-LID pollutant water budget: " + c.subcatch_names.name_of(b.source));
        if (w.spatial_infiltration > w.infiltration || w.outlet > w.outflow ||
            std::abs(w.opening + w.rain + w.runon - w.evaporation - w.infiltration - w.outflow - w.closing) >
                1e-10 * std::max(1.0, w.opening + w.rain + w.runon))
            throw std::runtime_error("Completed non-LID pollutant water budget does not close: " + c.subcatch_names.name_of(b.source));
    }
    const int mode = c.subcatches.subarea_routing[b.source];
    const int destination = mode == 2 ? 2 : mode == 1 ? 1 : -1;
    for (std::size_t p = 0; p < b.layers.size(); ++p) {
        const double routed = b.pending_routing[p]; b.pending_routing[p] = 0;
        b.impervious_mass[p] = b.pervious_mass[p] = 0;
        for (int k = 0; k < 3; ++k) {
            const auto& w = flux[k];
            const double rain = w.rain * ft3 * c.pollutants.c_rain[p];
            const double received = runon[p] * (w.area / full) + (k == 2 ? pervious_return[p] : 0) + aquifer_return[p][k];
            const double available = b.layers[p][k] + rain + received + (k == destination ? routed : 0);
            const double water = w.closing + w.outflow + w.infiltration;
            const auto take = [&](double v) { return water > 0 ? available * (v / water) : 0; };
            b.rain[p] += rain; b.incoming[p] += received;
            b.spatial_infiltration[p] += take(w.spatial_infiltration);
            b.outside_infiltration[p] += take(w.infiltration - w.spatial_infiltration);
            b.pending_routing[p] += take(w.outflow - w.outlet);
            if (k == 2) b.pervious_mass[p] += take(w.outlet);
            else b.impervious_mass[p] += take(w.outlet);
            b.layers[p][k] = water > 0 ? take(w.closing) : available;
            if (!std::isfinite(available) || !std::isfinite(b.layers[p][k]))
                throw std::runtime_error("Non-finite completed non-LID pollutant mixture: " + c.subcatch_names.name_of(b.source));
        }
        b.surface_mass[p] = b.impervious_mass[p] + b.pervious_mass[p];
    }
}
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
                         double dt, const std::string& source_name, double rain_volume,
                         const std::vector<double>& other_mass) {
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
        const double rain = rain_volume * p.c_rain[j];
        b.rain[j] += rain;
        b.incoming[j] += other_mass[j];
        const double external_mass = other_mass[j] +
            (g.type == lid::LIDType::RAIN_BARREL && g.stor_covered[u] ? 0 : rain);
        for (int k = 0; k < 4; ++k) {
            const double arriving = donor[k] == transport::kExternal ? external_mass :
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
    surface::SourceSnow snow;
    bool partitioned=false;
    bool hydraulic_returns=false;
    double hydraulic_return_end=0;
    std::vector<double> inside;
    std::vector<double> runon, pervious_return; // pending ft3, never held rates
    std::vector<SourceSurfaceReturn> aquifer_returns;
    std::vector<SourceOutfallReturn> outfall_returns;
    std::vector<SourceWaterLedger> ledger;
    std::vector<SourceWaterDelivery> deliveries;
    std::vector<SourceAtmosphericLedger> atmosphere;
    std::vector<std::string> pollutant_names;
    std::vector<SourcePollutantLedger> pollutants;
    std::vector<std::vector<double>> runon_mass, return_mass; // pending paired transfers, conc*m3
    explicit State(const SimulationContext& c) : ctx(c) {}
    State(const State& s) : ctx(s.ctx), runoff(s.runoff), lids(s.lids.waterTrial()), snow(s.snow),
        partitioned(s.partitioned),hydraulic_returns(s.hydraulic_returns),hydraulic_return_end(s.hydraulic_return_end),inside(s.inside),runon(s.runon), pervious_return(s.pervious_return), aquifer_returns(s.aquifer_returns),outfall_returns(s.outfall_returns), ledger(s.ledger), deliveries(s.deliveries), atmosphere(s.atmosphere),
        pollutant_names(s.pollutant_names), pollutants(s.pollutants), runon_mass(s.runon_mass), return_mass(s.return_mass) {}
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
const surface::SourceSnow& SourceWaterDriver::snow(bool trial)const{return view(trial).snow;}
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
std::string SourceWaterDriver::creditSurfaceReturns(double start,const std::vector<SourceSurfaceReturn>& returns) {
    if(!state_ || trial_ || !std::isfinite(start))return "Surface return requires committed source state.";
    const auto& s=*state_;const int np=int(s.pollutant_names.size());
    for(const auto& r:returns) {
        const int group=clocks_.groupForSubcatch(r.source);
        if(group<0 || clocks_.groups()[group].completed_end!=start || !clocks_.groups()[group].pending.empty() ||
           !std::isfinite(r.volume) || r.volume<0 || int(r.mass.size())!=np)
            return "Invalid surface return source, interval or paired volume.";
        bool found=false;
        if(r.type==-1 && r.unit==-1 && r.pool>=0 && r.pool<3) {
            const auto& a=s.runoff.soa();
            const double fraction=r.pool==0 ? a.frac_imperv0[r.source] : r.pool==1 ? a.frac_imperv1[r.source] : 1-a.imperv_pct[r.source];
            found=a.area[r.source]*fraction>0;
        } else for(const auto& [t,u]:s.lids.usageOrder())
            if(t==r.type && u==r.unit && s.lids.group(t).subcatch_idx[u]==r.source && s.lids.group(t).area[u]>0)found=true;
        if(!found)return "Surface return has no originating physical pool.";
        for(double mass:r.mass)if(!std::isfinite(mass) || mass<0 || (r.volume==0 && mass!=0))
            return "Invalid paired surface return mass.";
    }
    for(const auto& r:returns)if(r.volume>0) {
        state_->aquifer_returns.push_back(r);state_->ledger[r.source].aquifer_return+=r.volume;
        for(auto& b:state_->pollutants)if(b.source==r.source && b.type==r.type && b.unit==r.unit)
            for(int p=0;p<np;++p)b.aquifer_return[p]+=r.mass[p];
    }
    return {};
}
bool SourceWaterDriver::acceptsOutfallReturns() const { return view(false).hydraulic_returns; }
double SourceWaterDriver::outfallReturnEnd() const { return view(false).hydraulic_return_end; }
const std::vector<SourceOutfallReturn>& SourceWaterDriver::pendingOutfallReturns() const { return view(false).outfall_returns; }
std::string SourceWaterDriver::creditOutfallReturns(double start, double end, const std::vector<double>& volumes) {
    if (!state_ || trial_ || !state_->hydraulic_returns || !std::isfinite(start) || !std::isfinite(end) ||
        start != state_->hydraulic_return_end || end <= start || !std::isfinite(end-start) ||
        volumes.size() != state_->runon.size() ||
        end-state_->ctx.options.totalDurationMs()/1000.>1e-12*std::max(1.,end))
        return "Outfall return requires the next finite accepted hydraulic interval and explicit water-only ownership.";
    const auto& s=*state_;
    for (const auto& g:clocks_.groups())
        if (!g.pending.empty() || g.completed_end-end>1e-12*std::max(1.,end))
            return "Outfall return cannot credit an unsettled or future source interval.";
    for (std::size_t sc=0;sc<volumes.size();++sc) {
        const double v=volumes[sc];
        if (!std::isfinite(v) || v<0 || (v>0 && (clocks_.groupForSubcatch(int(sc))<0 ||
            s.runoff.soa().area[sc]+s.ctx.subcatches.total_lid_area_ft2[sc]<=0)) ||
            !std::isfinite(s.runon[sc]+v) || !std::isfinite(s.ledger[sc].outfall_return+v))
            return "Invalid outfall return volume or physical receiver.";
    }
    auto pending=s.outfall_returns;
    for (std::size_t sc=0;sc<volumes.size();++sc)if(volumes[sc]>0)
        pending.push_back({int(sc),end,volumes[sc]});
    // Prepare the whole queue before publishing any ledger or cursor change.
    state_->outfall_returns=std::move(pending);
    for (std::size_t sc=0;sc<volumes.size();++sc) {
        state_->ledger[sc].outfall_return+=volumes[sc];
    }
    state_->hydraulic_return_end=end;
    return {};
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
    double residual = -pendingPollutantMass(group, pollutant, t);
    for (const auto& b : s.pollutants) if (clocks_.groupForSubcatch(b.source) == group) {
        residual += b.rain[pollutant] + b.aquifer_return[pollutant] - b.spatial_infiltration[pollutant] - b.outside_infiltration[pollutant] -
            b.outlet[pollutant] - b.removed[pollutant];
        for (double mass : b.layers[pollutant]) residual -= mass;
    }
    return residual;
}
double SourceWaterDriver::pendingPollutantMass(int group, int pollutant, bool t) const {
    const auto& s = view(t); (void)s.pollutant_names.at(pollutant); double mass = 0;
    for (int sc : clocks_.groups().at(group).subcatches) mass += s.runon_mass[sc][pollutant] + s.return_mass[sc][pollutant];
    for (const auto& b : s.pollutants) if (clocks_.groupForSubcatch(b.source) == group) mass += b.pending_routing[pollutant];
    for(const auto& r:s.aquifer_returns)if(clocks_.groupForSubcatch(r.source)==group)mass+=r.mass[pollutant];
    return mass;
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
    for(const auto& r:s.aquifer_returns)if(clocks_.groupForSubcatch(r.source)==group)v+=r.volume;
    for(const auto& r:s.outfall_returns)if(clocks_.groupForSubcatch(r.source)==group)v+=r.volume;
    return v;
}
double SourceWaterDriver::storedVolume(int source) const {
    (void)view(false).ledger.at(source);
    return storage(view(false),source);
}
double SourceWaterDriver::balanceResidual(int group, bool trial) const {
    const auto& s = view(trial); double v = -pendingVolume(group, trial);
    for (int sc : clocks_.groups().at(group).subcatches) {
        const auto& b = s.ledger[sc];
        v += b.initial_storage + b.rain + b.aquifer_return + b.outfall_return - b.evaporation - b.infiltration - b.outlet - storage(s, sc);
    }
    return v;
}
std::string SourceWaterDriver::initialize(const SimulationContext& c, const std::vector<int>& seeds,
                                         const std::vector<std::pair<int, double>>& areas,
                                         const lid::LIDSolver* initial_lids, bool hydraulic_returns) {
    if (state_) return "Source water driver already initialized; restart/import is not qualified.";
    if (c.current_time != 0.0 || c.elapsed_ms != 0.0)
        return "Source water driver requires simulation-start state; hot starts are not qualified.";
    if (c.options.water_age ||
        c.options.heat_transport || c.reactions.n_species() > 0)
        return "Source water driver does not yet pair age, heat or MSX.";
    const int np = c.options.ignore_quality ? 0 : c.n_pollutants();
    if (hydraulic_returns && np>0) return "Hydraulic source returns require water-only sources.";
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
    SourceClockGroups clocks; auto error = clocks.initialize(c, seeds, hydraulic_returns);
    if (!error.empty()) return error;
    auto s = std::make_unique<State>(c);
    s->hydraulic_returns=hydraulic_returns;
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
        std::vector<std::vector<int>> snow_groups;
        bool snow_active=false;
        for(const auto& g:clocks.groups()) {
            snow_groups.push_back(g.subcatches);
            for(int sc:g.subcatches)if(!c.options.ignore_snow_melt&&c.subcatches.snowpack[sc]>=0)snow_active=true;
        }
        if(snow_active)for(const auto& g:clocks.groups())for(int sc:g.subcatches)
            s->ctx.subcatches.area[sc]=(s->runoff.soa().area[sc]+s->ctx.subcatches.total_lid_area_ft2[sc])*ucf::UCF(ucf::LANDAREA,c.options);
        if(auto error=s->snow.initialize(s->ctx,snow_groups,s->runoff.soa().area);!error.empty())return error;

        const auto n = static_cast<std::size_t>(c.n_subcatches());
        s->inside.assign(n,1);s->runon.assign(n, 0); s->pervious_return.assign(n, 0); s->ledger.resize(n);
        s->runon_mass.assign(n, std::vector<double>(np, 0)); s->return_mass = s->runon_mass;
        for (const auto& group : clocks.groups()) {
            for (int sc : group.subcatches) {
                if (c.subcatches.gw_aquifer[sc] >= 0)
                    return "Source water driver does not yet advance lumped groundwater.";
                if (c.subcatches.runon_rate[sc] != 0 || c.subcatches.lid_return_to_perv_cfs[sc] != 0)
                    return "Source water driver requires empty initial transfer histories.";
                if (hydraulic_returns && c.subcatches.outfall_runon_vol[sc]!=0)
                    return "Hydraulic source returns require empty initial outfall receipts.";
                if (hydraulic_returns && s->runoff.soa().area[sc]+s->ctx.subcatches.total_lid_area_ft2[sc]<=0 &&
                    std::find(c.node_subtypes.outfalls.route_to.begin(),c.node_subtypes.outfalls.route_to.end(),sc)!=c.node_subtypes.outfalls.route_to.end())
                    return "Hydraulic source return destination requires positive physical area.";
                s->ledger[sc].initial_storage = storage(*s, sc);
                const auto& r = s->runoff.soa();
                if (np > 0 && r.area[sc] > 0) s->pollutants.push_back(pollutantLedger(sc, -1, -1, np));
                for (auto kind : {SourceEtKind::PERVIOUS, SourceEtKind::IMPERVIOUS}) {
                    const bool pervious = kind == SourceEtKind::PERVIOUS;
                    const double area = r.area[sc] * (pervious ? 1 - r.imperv_pct[sc] : r.imperv_pct[sc]);
                    if (area > 0) s->atmosphere.push_back({kind, sc, -1, -1, area, pervious});
                }
            }
            for (const auto& [t, u] : s->lids.usageOrder()) {
                const auto& g = s->lids.group(t); const int sc = g.subcatch_idx[u];
                if (clocks.groupForSubcatch(sc) < 0) continue;
                if(g.surf_void_frac[u]<=0) return "Completed LID surface storage requires a positive void fraction.";
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
                if (g.n_pollutants != np || g.drain_rmvl.size() != std::size_t(g.count * np))
                    return "Completed LID pollutant drain removal layout is invalid.";
                for (int j = 0; j < np; ++j) {
                    const double f = g.drain_rmvl[u * np + j];
                    if (!std::isfinite(f) || f < 0 || f > 1) return "Invalid completed LID pollutant drain removal fraction.";
                }
                s->pollutants.push_back(pollutantLedger(sc, t, u, np));
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
    auto weather_input=f.sources;
    if(s.snow.active())s.snow.advance(c,clocks_.groupForSubcatch(weather_input.front().subcatch),f.start,f.end,weather_input);

    if (s.partitioned && !ceiling) {
        for (const auto& [t, u] : s.lids.usageOrder()) {
            const auto& g = s.lids.group(t);
            if (s.inside[g.subcatch_idx[u]] > 0 && g.area[u] > 0 &&
                lid::hasNativeBottom(g.type, g.stor_thick[u], g.stor_ksat[u]) &&
                std::any_of(weather_input.begin(), weather_input.end(), [&](const auto& w) {
                    return w.subcatch == g.subcatch_idx[u];
                }))
                throw std::runtime_error("Completed inside native LID area requires a spatial bottom ceiling.");
        }
    }
    std::vector<const RunoffSourceForcing*> weather_at(c.n_subcatches(), nullptr);
    for (const auto& weather : weather_input) weather_at[weather.subcatch] = &weather;
    const auto selected = [&](int sc) { return sc >= 0 && sc < c.n_subcatches() && weather_at[sc]; };
    const auto available = [&](const auto& q) {
        return selected(q.source) && q.available-f.start<=1e-12*std::max(1.,std::max(q.available,f.start));
    };
    for(const auto& q:s.outfall_returns)if(available(q))s.runon[q.source]+=q.volume;
    std::erase_if(s.outfall_returns,available);
    const int np = int(s.pollutant_names.size());
    auto forcing=weather_input;
    std::vector<SourceSurfaceReturn> incoming_aquifer;
    std::vector<std::vector<std::array<double,3>>> nonlid_return(c.n_subcatches(),std::vector<std::array<double,3>>(np));
    for(const auto& q:s.aquifer_returns)if(selected(q.source)) {
        incoming_aquifer.push_back(q);
        if(q.type<0) {
            const auto& a=s.runoff.soa();
            const double fraction=q.pool==0 ? a.frac_imperv0[q.source] : q.pool==1 ? a.frac_imperv1[q.source] : 1-a.imperv_pct[q.source];
            for(auto& w:forcing)if(w.subcatch==q.source)w.surface_return[q.pool]+=q.volume/dt/a.area[q.source]/fraction;
            for(int p=0;p<np;++p)nonlid_return[q.source][p][q.pool]+=q.mass[p];
        }
    }
    std::erase_if(s.aquifer_returns,[&](const auto& q){return selected(q.source);});
    const auto incoming_runon = s.runon_mass, incoming_return = s.return_mass;
    std::vector<std::vector<double>> outlet_mass(c.n_subcatches(), std::vector<double>(np, 0));
    // Consume each pending volume once. Runon is distributed across the full
    // non-LID + LID footprint, so partial LID coverage cannot multiply it.
    for (const auto& weather : weather_input) {
        const int sc = weather.subcatch;
        const double full = s.runoff.soa().area[sc] + c.subcatches.total_lid_area_ft2[sc];
        if (s.runon[sc] > 0 && full <= 0) throw std::runtime_error("Runon receiver has no surface area.");
        c.subcatches.runon_rate[sc] = full > 0 ? s.runon[sc] / dt / full : 0;
        c.subcatches.lid_return_to_perv_cfs[sc] = s.pervious_return[sc] / dt;
        s.ledger[sc].runon += s.runon[sc]; s.ledger[sc].pervious_return += s.pervious_return[sc];
        s.runon[sc] = 0; s.pervious_return[sc] = 0;
        std::fill(s.runon_mass[sc].begin(), s.runon_mass[sc].end(), 0);
        std::fill(s.return_mass[sc].begin(), s.return_mass[sc].end(), 0);
        double arrival=weather.rain*full;
        if(s.snow.active()&&c.subcatches.snowpack[sc]>=0) {
            const double fi=s.runoff.soa().imperv_pct[sc];
            arrival=weather.rain*c.subcatches.total_lid_area_ft2[sc]+s.runoff.soa().area[sc]*
                (fi*c.subcatches.snow_net_imperv[sc]+(1-fi)*c.subcatches.snow_net_perv[sc]);
        }
        s.ledger[sc].rain += arrival * dt;
    }
    s.runoff.execute(c, dt, 0, f.infil_factor, f.recovery_factor, f.month, boundary, &forcing,s.partitioned ? &s.inside : nullptr);
    const auto& r = s.runoff.soa();
    for (auto& b : s.pollutants) if (b.type < 0 && selected(b.source)) {
        const int sc = b.source;
        advanceNonLidPollutants(b, r.completed[sc], c, r.area[sc] + c.subcatches.total_lid_area_ft2[sc],
            incoming_runon[sc], incoming_return[sc],nonlid_return[sc]);
        outlet_mass[sc] = b.surface_mass;
    }
    std::vector<double> outlet(c.n_subcatches(), 0), native(c.n_subcatches(), 0);
    for (const auto& weather : weather_input) {
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
    std::vector<std::vector<double>> lid_incoming(s.pollutants.size(), std::vector<double>(np, 0));
    std::vector<double> capture_impervious(c.n_subcatches(), 0), capture_pervious(c.n_subcatches(), 0);
    for (const auto& [t, u] : s.lids.usageOrder()) {
        const auto& g = s.lids.group(t); const int sc = g.subcatch_idx[u];
        if (!selected(sc) || g.area[u] <= 0) continue;
        const auto* weather = weather_at[sc];
        const double captured = (r.imperv_runoff_cfs[sc] * g.from_imperv[u] +
                                 r.perv_runoff_cfs[sc] * g.from_perv[u]) * dt;
        capture_impervious[sc] += g.from_imperv[u]; capture_pervious[sc] += g.from_perv[u];
        outlet[sc] -= captured; s.ledger[sc].captured += captured;
        for (std::size_t j = 0; j < s.pollutants.size(); ++j) {
            const auto& b = s.pollutants[j];
            if (b.type != t || b.unit != u) continue;
            for (int p = 0; p < np; ++p) {
                double mass = 0;
                for (const auto& nonlid : s.pollutants) if (nonlid.source == sc && nonlid.type < 0)
                    mass = nonlid.impervious_mass[p] * g.from_imperv[u] + nonlid.pervious_mass[p] * g.from_perv[u];
                lid_incoming[j][p] = mass + incoming_runon[sc][p] *
                    (g.area[u] / (r.area[sc] + c.subcatches.total_lid_area_ft2[sc]));
            }
        }
        double returned=0;
        for(const auto& q:incoming_aquifer)if(q.type==t && q.unit==u) {
            returned+=q.volume;
            for(std::size_t j=0;j<s.pollutants.size();++j)if(s.pollutants[j].type==t && s.pollutants[j].unit==u)
                for(int p=0;p<np;++p)lid_incoming[j][p]+=q.mass[p];
        }
        double rain = weather->rain;
        if (g.type == lid::LIDType::RAIN_BARREL && g.stor_covered[u]) {
            for (int p = 0; p < np; ++p) s.return_mass[sc][p] += rain * g.area[u] * dt * ft3 * c.pollutants.c_rain[p];
            s.pervious_return[sc] += rain * g.area[u] * dt; rain = 0;
        }
        const double bottom=ceiling ? (*ceiling)(t,u,f.start,f.end) : 1.0e10;
        inputs.push_back({t, u, (captured+returned) / dt / g.area[u] + rain + c.subcatches.runon_rate[sc],
            weather->rain, weather->pet, native[sc], s.partitioned ? 1.0e10 : bottom,
            factor(c, sc, f),s.partitioned ? s.inside[sc] : -1.0,bottom});
    }
    std::sort(inputs.begin(), inputs.end(), [](const auto& a, const auto& b) {
        return std::pair{a.type, a.unit} < std::pair{b.type, b.unit};
    });
    for (const auto& weather : weather_input)
        if (outlet[weather.subcatch] < -1e-12 ||
            capture_impervious[weather.subcatch] * r.imperv_runoff_cfs[weather.subcatch] * dt > r.imperv_runoff_cfs[weather.subcatch] * dt + 1e-12 ||
            capture_pervious[weather.subcatch] * r.perv_runoff_cfs[weather.subcatch] * dt > r.perv_runoff_cfs[weather.subcatch] * dt + 1e-12)
            throw std::runtime_error("LID capture exceeds the available non-LID outlet volume.");
    for (auto& b : s.pollutants) if (b.type < 0 && selected(b.source)) {
        // Form the uncaptured fraction once. Repeated subtraction of nearly
        // equal mass shares can leave signed residue after a 100% capture.
        for (int p = 0; p < np; ++p) b.surface_mass[p] =
            b.impervious_mass[p] * std::max(0.0, 1 - capture_impervious[b.source]) +
            b.pervious_mass[p] * std::max(0.0, 1 - capture_pervious[b.source]);
        outlet_mass[b.source] = b.surface_mass;
    }
    std::vector<std::array<double, 4>> old_layers;
    for (const auto& b : s.pollutants) old_layers.push_back(b.type >= 0 ? layerWater(s.lids.group(b.type), b.unit) : std::array<double, 4>{});
    s.lids.executeCompleted(dt, f.start, f.recovery_factor, inputs);
    for (std::size_t j = 0; j < s.pollutants.size(); ++j) {
        auto& b = s.pollutants[j];
        if (b.type >= 0 && selected(b.source)) advanceLidPollutants(b, s.lids.group(b.type), c.pollutants, old_layers[j], dt,
            c.subcatch_names.name_of(b.source), weather_at[b.source]->rain * s.lids.group(b.type).area[b.unit] * dt * ft3,
            lid_incoming[j]);
    }
    for (auto& b : s.atmosphere) {
        const auto* weather = weather_at[b.source]; if (!weather) continue;
        double pet=weather->pet;
        if(b.kind==SourceEtKind::PERVIOUS&&weather->pet_perv>=0)pet=weather->pet_perv;
        if(b.kind==SourceEtKind::IMPERVIOUS&&weather->pet_imperv>=0)pet=weather->pet_imperv;
        const double potential = pet * dt * b.area;
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
    const auto route = [&](int sc, int node, int target, double volume, bool drain, const std::vector<double>& mass) {
        if (!std::isfinite(volume) || volume < -1e-12)
            throw std::runtime_error("Source capture/output produced invalid water volume.");
        volume = std::max(volume, 0.0);
        for (double m : mass) if (!std::isfinite(m) || m < -1e-12 || (volume == 0 && m != 0))
            throw std::runtime_error("Source capture/output produced invalid paired pollutant mass.");
        if (node < 0 && target >= 0 && target != sc) {
            if (!selected(target)) throw std::runtime_error("Transfer escaped its source clock group.");
            s.runon[target] += volume;
            for (int p = 0; p < np; ++p) s.runon_mass[target][p] += std::max(0.0, mass[p]);
            return false;
        } else if (volume > 0) {
            s.deliveries.push_back({sc, node, f.start, f.end, volume, drain, mass}); s.ledger[sc].outlet += volume;
        }
        return true;
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
        SourcePollutantLedger* quality = nullptr;
        for (auto& b : s.pollutants) if (b.type == t && b.unit == u) quality = &b;
        std::vector<double> drain_mass = quality ? quality->drain_mass : std::vector<double>{};
        const bool can_return = g.to_perv[u] && r.area[sc] * (1 - r.imperv_pct[sc]) > 0;
        const bool to_outlet = (g.drain_node[u] < 0 && g.drain_subcatch[u] < 0) ||
            (g.drain_node[u] == c.subcatches.outlet_node[sc] && g.drain_subcatch[u] == c.subcatches.outlet_subcatch[sc]);
        if (can_return) {
            s.pervious_return[sc] += surface;
            if (quality) for (int p = 0; p < np; ++p) s.return_mass[sc][p] += quality->surface_mass[p];
            if (to_outlet) {
                s.pervious_return[sc] += drain; drain = 0; g.drain_flow[u] = 0;
                if (quality) for (int p = 0; p < np; ++p) {
                    s.return_mass[sc][p] += drain_mass[p]; drain_mass[p] = 0;
                }
            }
        } else {
            outlet[sc] += surface;
            if (quality) for (int p = 0; p < np; ++p) outlet_mass[sc][p] += quality->surface_mass[p];
        }
        int node = g.drain_node[u], target = g.drain_subcatch[u];
        if (node < 0 && (target < 0 || target == sc)) {
            node = c.subcatches.outlet_node[sc]; target = c.subcatches.outlet_subcatch[sc];
        }
        if (route(sc, node, target, drain, true, drain_mass) && quality)
            for (int p = 0; p < np; ++p) quality->outlet[p] += drain_mass[p];
        g.drain_open[u] = g.drain_flow[u] > 0; g.old_drain_flow[u] = g.drain_flow[u];
    }
    for (const auto& weather : weather_input) {
        const int sc = weather.subcatch;
        if (route(sc, c.subcatches.outlet_node[sc], c.subcatches.outlet_subcatch[sc], outlet[sc], false, outlet_mass[sc]))
            for (auto& b : s.pollutants) if (b.source == sc) {
                const bool returned = b.type >= 0 && s.lids.group(b.type).to_perv[b.unit] && r.area[sc] * (1 - r.imperv_pct[sc]) > 0;
                if (!returned) for (int p = 0; p < np; ++p) b.outlet[p] += b.surface_mass[p];
            }
        c.subcatches.runoff[sc] = std::max(outlet[sc], 0.0) / dt;
        c.subcatches.runon_rate[sc] = 0;
    }
}

std::string SourceWaterDriver::stage(int group, double end, double completed_through,
                                    const RunoffSolver::InfiltrationBoundary* boundary,
                                    const BottomCeiling* ceiling,const ForcingTransform* forcing) {
    if (!state_) return "Source water driver is not initialized.";
    if (trial_) return "Source water driver already has an unsettled trial.";
    auto error = clocks_.stage(group, end, completed_through); if (!error.empty()) return error;
    trial_ = std::make_unique<State>(*state_); pending_group_ = group;
    try {
        for (const auto& f : clocks_.groups()[group].pending) {
            std::vector<double> ends{f.end};
            for(const auto& q:trial_->outfall_returns)if(clocks_.groupForSubcatch(q.source)==group &&
                q.available-f.start>1e-12*std::max(1.,q.available) && q.available<f.end)ends.push_back(q.available);
            std::sort(ends.begin(),ends.end());ends.erase(std::unique(ends.begin(),ends.end()),ends.end());
            if(trial_->snow.active()) {
                // UEB held windows stop at the same clock boundaries as native
                // snow; weather intervals may also end sooner at gage changes.
                double at=f.start;
                while(at<f.end) {
                    const double span=surface::ueb::nextSnowBoundary(trial_->ctx,at,f.end-at);
                    const double next=std::min(f.end,at+span);
                    if(!(next>at))throw std::runtime_error("Snow source boundary failed to advance.");
                    if(next<f.end)ends.push_back(next);at=next;
                }
                std::sort(ends.begin(),ends.end());ends.erase(std::unique(ends.begin(),ends.end()),ends.end());
            }
            double start=f.start;
            for(double end:ends) {
                auto segment=f;segment.start=start;segment.end=end;
                if(forcing)(*forcing)(segment);
                advance(*trial_,segment,boundary,ceiling);start=end;
            }
        }
        double scale = 1;
        for (int sc : clocks_.groups()[group].subcatches)
            scale += trial_->ledger[sc].initial_storage + trial_->ledger[sc].rain + trial_->ledger[sc].outfall_return;
        const double residual = balanceResidual(group, true);
        if (!std::isfinite(residual) || std::abs(residual) > 1e-10 * scale) {
            std::ostringstream message;message<<std::setprecision(17)
                <<"Completed source water trial failed storage/transfer conservation: group "<<group
                <<", end "<<end<<" s, residual "<<residual<<" ft3, tolerance "<<1e-10*scale<<" ft3.";
            throw std::runtime_error(message.str());
        }
        for (std::size_t j = 0; j < trial_->pollutant_names.size(); ++j) {
            double mass_scale = 1;
            for (const auto& b : trial_->pollutants)
                if (clocks_.groupForSubcatch(b.source) == group) mass_scale += b.rain[j];
            const double mass_residual = pollutantBalanceResidual(group, int(j), true);
            if (!std::isfinite(mass_residual) || std::abs(mass_residual) > 1e-10 * mass_scale)
                throw std::runtime_error("Completed source pollutant trial failed mass conservation: " + trial_->pollutant_names[j]);
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
void SourceWaterDriver::checkpoint(detail::CompletedSourceArchive& a,bool model) {
    if(!state_ || trial_)throw std::runtime_error("Source checkpoint requires committed water state.");
    auto& s=*state_;auto& c=s.ctx;auto& sub=c.subcatches;
    clocks_.checkpoint(a,model);
    s.runoff.checkpoint(a,model);s.lids.checkpoint(a,model);
    s.snow.checkpoint(a,model,c);
    if(!model&&s.snow.active())for(std::size_t g=0;g<clocks_.groups().size();++g)
        if(s.snow.ledgers()[g].end!=clocks_.groups()[g].completed_end)throw std::runtime_error("Snow source restart clocks differ.");
    if(model) {
        a(s.partitioned,s.inside,s.pollutant_names,c.options.flow_units,c.options.ignore_quality,
            c.options.ignore_snow_melt,c.options.water_age,c.options.heat_transport);
        for(int sc=0;sc<c.n_subcatches();++sc){auto name=c.subcatch_names.name_of(sc);a(name);}
        a(sub.outlet_node,sub.outlet_subcatch,sub.gage,sub.area,sub.width,sub.slope);
        a(sub.curb_length,sub.rain_scale_factor,sub.snow_scale_factor,sub.frac_imperv,sub.frac_imperv_no_store,sub.pct_zero);
        a(sub.n_imperv,sub.n_perv,sub.ds_imperv,sub.ds_perv,sub.subarea_routing,sub.pct_routed);
        a(sub.infil_model,sub.infil_p1,sub.infil_p2,sub.infil_p3,sub.infil_p4,sub.infil_p5);
        a(sub.total_lid_area_ft2,sub.gw_aquifer,sub.gw_node,sub.snowpack);
        a(c.subcatch_infil_pattern,c.patterns.factors,c.pollutants.units,c.pollutants.c_rain,
            c.pollutants.k_decay,c.pollutants.co_pollut,c.pollutants.co_frac,c.pollutants.snow_only);
        a.sequence(s.atmosphere,[&](auto& b){a(b.kind,b.source,b.type,b.unit,b.area,b.soil_eligible);},true);
        a.sequence(s.pollutants,[&](auto& b){a(b.source,b.type,b.unit);},true);
        if (s.hydraulic_returns) {
            uint32_t profile=1;a(profile,c.node_subtypes.outfalls.route_to);
        }
        return;
    }
        a.fixed(sub.runoff,sub.rainfall,sub.evap_loss,sub.infil_loss,sub.ponded_depth,sub.old_ponded_depth);
        a.fixed(sub.washoff_vout1,sub.gw_flow,sub.snow_depth,sub.lid_drain_flow,sub.old_runoff,sub.old_gw_flow);
        a.fixed(sub.old_snow_depth,sub.old_lid_drain_flow,sub.runon_inflow,sub.old_runon_inflow,sub.runon_rate,sub.outfall_runon_vol);
        a.fixed(sub.subarea_runoff_rate,sub.lid_return_to_perv_cfs,sub.lid_drain_runon_cfs,sub.stat_precip_vol,sub.stat_evap_vol,sub.stat_infil_vol);
        a.fixed(sub.stat_imperv_vol,sub.stat_perv_vol,sub.stat_runoff_vol,sub.stat_max_runoff);
    a.fixed(s.runon,s.pervious_return);
    a.sequence(s.runon_mass,[&](auto& mass){a.fixed(mass);},true);
    a.sequence(s.return_mass,[&](auto& mass){a.fixed(mass);},true);
    a.sequence(s.ledger,[&](auto& b){a(b.initial_storage,b.rain,b.evaporation,b.infiltration,
        b.spatial_infiltration,b.outside_infiltration,b.aquifer_return,b.outlet,b.captured,b.runon,b.pervious_return);},true);
    a.sequence(s.atmosphere,[&](auto& b){a(b.potential,b.evaporation);},true);
    a.sequence(s.pollutants,[&](auto& b){a.fixed(b.layers,b.rain,b.spatial_infiltration,b.outside_infiltration,
        b.outlet,b.removed,b.surface_mass,b.drain_mass,b.impervious_mass,b.pervious_mass,b.aquifer_return,b.pending_routing,b.incoming);},true);
    a.sequence(s.deliveries,[&](auto& d){a(d.source,d.node,d.start,d.end,d.volume,d.drain,d.mass);});
    a.sequence(s.aquifer_returns,[&](auto& q){a(q.source,q.type,q.unit,q.pool,q.volume,q.mass);});
    // Conditional extension: ordinary completed-source model/runtime archives
    // keep their existing byte layout. Model identity binds the receipt profile.
    if (s.hydraulic_returns) {
        uint32_t profile=1;a(profile,s.hydraulic_return_end);
        if (profile!=1 || !std::isfinite(s.hydraulic_return_end) || s.hydraulic_return_end<0 ||
            s.hydraulic_return_end-c.options.totalDurationMs()/1000.>1e-12*std::max(1.,s.hydraulic_return_end))
            throw std::runtime_error("Invalid hydraulic source return checkpoint profile or clock.");
        for (std::size_t sc=0;sc<s.ledger.size();++sc) {
            auto& v=s.ledger[sc].outfall_return;a(v);
            if (!std::isfinite(v) || v<0 || (v>0 && (clocks_.groupForSubcatch(int(sc))<0 || s.hydraulic_return_end==0)))
                throw std::runtime_error("Invalid hydraulic source return checkpoint ledger.");
        }
        a.sequence(s.outfall_returns,[&](auto& q){a(q.source,q.available,q.volume);});
        std::vector<double> pending(s.ledger.size(),0);
        double previous=0;
        for(const auto& q:s.outfall_returns) {
            const int group=clocks_.groupForSubcatch(q.source);
            if(group<0 || !std::isfinite(q.available) || q.available<=0 || q.available<previous ||
                q.available>s.hydraulic_return_end || !std::isfinite(q.volume) || q.volume<=0 ||
                clocks_.groups()[group].completed_end-q.available>1e-12*std::max(1.,q.available) ||
                s.runoff.soa().area[q.source]+s.ctx.subcatches.total_lid_area_ft2[q.source]<=0)
                throw std::runtime_error("Invalid pending hydraulic source return checkpoint.");
            previous=q.available;pending[q.source]+=q.volume;
        }
        for(std::size_t sc=0;sc<pending.size();++sc)if(!std::isfinite(pending[sc]) ||
            pending[sc]>s.ledger[sc].outfall_return+1e-11*std::max(1.,s.ledger[sc].outfall_return))
            throw std::runtime_error("Pending hydraulic source return exceeds credited water.");
    }
    if(a.reading()) {
        const int np=int(s.pollutant_names.size());
        const auto paired=[&](double volume,const auto& mass) {
            if(!std::isfinite(volume) || volume<0 || int(mass.size())!=np)throw std::runtime_error("Invalid source restart tuple.");
            for(double m:mass)if(!std::isfinite(m) || m<0 || (volume==0 && m!=0))throw std::runtime_error("Invalid source restart mass.");
        };
        for(std::size_t sc=0;sc<s.runon.size();++sc){paired(s.runon[sc],s.runon_mass[sc]);paired(s.pervious_return[sc],s.return_mass[sc]);}
        for(const auto& b:s.pollutants) {
            for(const auto& layer:b.layers)for(double mass:layer)if(!std::isfinite(mass)||mass<0)
                throw std::runtime_error("Invalid source checkpoint pool mass.");
        }
        for(const auto& b:s.atmosphere)if(!std::isfinite(b.potential)||!std::isfinite(b.evaporation)||b.potential<0||
            b.evaporation<0||b.evaporation>b.potential+1e-10*std::max(1.,b.potential))
                throw std::runtime_error("Invalid source checkpoint atmospheric budget.");
        for(const auto& d:s.deliveries) {
            if(clocks_.groupForSubcatch(d.source)<0 || !std::isfinite(d.start) || !std::isfinite(d.end) || d.start<0 || d.end<=d.start ||
                d.end>clocks_.groups()[clocks_.groupForSubcatch(d.source)].completed_end)
                throw std::runtime_error("Invalid source restart delivery interval.");
            paired(d.volume,d.mass);
        }
        // Reuse the normal credit validator without crediting or replaying it.
        auto probe=waterTrial();probe.state_->aquifer_returns.clear();
        for(const auto& q:s.aquifer_returns) {
            paired(q.volume,q.mass);const int group=clocks_.groupForSubcatch(q.source);
            if(group<0 || !probe.creditSurfaceReturns(clocks_.groups()[group].completed_end,{q}).empty())
                throw std::runtime_error("Invalid originating source restart pool.");
        }
        for(std::size_t g=0;g<clocks_.groups().size();++g) {
            const double scale=std::max(1.0,ledgers()[clocks_.groups()[g].subcatches[0]].rain);
            if(!std::isfinite(balanceResidual(int(g))) || std::abs(balanceResidual(int(g)))>1e-8*scale)
                throw std::runtime_error("Source restart water balance differs.");
            for(int p=0;p<np;++p)if(!std::isfinite(pollutantBalanceResidual(int(g),p)) || std::abs(pollutantBalanceResidual(int(g),p))>1e-8*scale)
                throw std::runtime_error("Source restart pollutant balance differs.");
        }
    }
}

}
