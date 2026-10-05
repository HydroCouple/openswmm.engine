// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
#include "LidNode.hpp"
#include "../core/SimulationContext.hpp"
#include "../core/UnitConversion.hpp"
#include "../quality/Treatment.hpp"
#include <algorithm>
#include <cmath>
#include <sstream>
#include <limits>

namespace openswmm::lidnode {
bool validTreatment(const SimulationContext& ctx, const LidLayerTreatment& rule, std::string& error) {
    if (ctx.pollutant_names.find(rule.pollutant) < 0) { error = "Unknown pollutant: " + rule.pollutant; return false; }
    if (!std::isfinite(rule.removal) || rule.removal < 0 || rule.removal > 1 || !std::isfinite(rule.decay) || rule.decay < 0) {
        error = "Removal must be between 0 and 100 percent; decay must be finite and nonnegative"; return false;
    }
    if (!rule.expression.empty()) {
        treatment::TreatExpr expr; int col = -1;
        if (treatment::validate(rule.expression, error, col) != 0 || treatment::parse(rule.expression, expr, ctx.pollutant_names.names()) != 0) {
            if (error.empty()) error = "Invalid treatment expression";
            return false;
        }
    }
    return true;
}
void readTreatment(SimulationContext& ctx, const std::vector<std::string>& lines) {
    for (const auto& line : lines) {
        std::istringstream stream(line); std::string control, pollutant, expression; int layer = 0; double removal = 0, decay = 0;
        if (!(stream >> control >> layer >> pollutant >> removal >> decay)) { ctx.errors.push_back("Invalid [LID_LAYER_TREATMENT] row: " + line); continue; }
        std::getline(stream >> std::ws, expression);
        const int c = ctx.lid_names.find(control);
        if(c < 0 || c >= static_cast<int>(ctx.lid_controls.node_layers.size()) || ctx.lid_controls.node_layers[c].empty() || ctx.pollutant_names.find(pollutant)<0) {
            ctx.deferred_section_rows.emplace_back("LID_LAYER_TREATMENT",line); continue;
        }
        if (c < 0 || c >= static_cast<int>(ctx.lid_controls.node_layers.size()) || layer < 1 || layer > static_cast<int>(ctx.lid_controls.node_layers[c].size()) || ctx.lid_controls.node_layers[c][layer-1].kind == LidNodeLayerKind::Bottom) {
            ctx.errors.push_back("Invalid LID treatment control/layer: " + line); continue;
        }
        auto& rules = ctx.lid_controls.node_layers[c][layer-1].treatment;
        LidLayerTreatment rule{pollutant, removal / 100.0, decay, expression == "-" ? "" : expression};
        std::string error;
        if (!validTreatment(ctx, rule, error)) { ctx.errors.push_back("LID treatment: " + error); continue; }
        if (std::any_of(rules.begin(), rules.end(), [&](const auto& r) { return ctx.pollutant_names.find(r.pollutant) == ctx.pollutant_names.find(pollutant); })) {
            ctx.errors.push_back("Duplicate LID layer treatment: " + line); continue;
        }
        rules.push_back(std::move(rule));
    }
}
namespace {
double layerVolume(const LidNodeState& state, const std::vector<double>& water, int cell) {
    double total=0;
    for (std::size_t i=0;i<water.size();++i)
        if (state.cells[i].layer==state.cells[cell].layer) total+=water[i];
    return total;
}
// Fixed removal and R=/C= expressions act on water leaving an authored layer,
// never at the five internal MEDIA subcell interfaces. Decay acts on resident
// mass once per routing step. Saturated mobile water remains the shared node
// reactor; retained cells each carry their own pollutant mass.
double treatedConcentration(const SimulationContext& ctx, const LidNodeCell& cell,
                            const LidNodeLayer& layer, int p, double concentration,
                            const std::vector<double>& concentrations, double volume, double flow, double dt) {
    const int np = ctx.n_pollutants();
    std::vector<double> removals(np, -1.0);
    auto evaluate = [&](int pollutant, auto& self) -> double {
        if (removals[pollutant] >= 0) return removals[pollutant] <= 1 ? removals[pollutant] : 0;
        removals[pollutant] = 2; // break cyclic co-treatment references
        const auto it = std::find_if(layer.treatment.begin(), layer.treatment.end(), [&](const auto& rule) { return ctx.pollutant_names.find(rule.pollutant) == pollutant; });
        if (it == layer.treatment.end()) return removals[pollutant] = 0;
        double r = it->removal;
        if (!it->expression.empty()) {
            treatment::TreatExpr expr;
            if (treatment::parse(it->expression, expr, ctx.pollutant_names.names()) == 0) {
                for (const auto& token : expr.tokens)
                    if (token.var == treatment::TreatVar::R_POLLUT && token.pollut_ref >= 0 && token.pollut_ref < np) self(token.pollut_ref, self);
                auto safe = removals;
                for (auto& value : safe) if (value < 0 || value > 1) value = 0;
                const double c = concentrations[pollutant] * (1-r);
                const double length = ucf::UCF(ucf::LENGTH, ctx.options);
                double result = treatment::evaluate(expr, c, dt, flow > 0 ? volume / flow / 3600.0 : dt / 3600.0,
                    flow * ucf::UCF(ucf::FLOW, ctx.options), volume, layer.params[0] / ucf::UCF(ucf::RAINDEPTH, ctx.options) * length,
                    concentrations.data(), safe.data(), np, cell.area*length*length, concentrations.data());
                if (std::isfinite(result)) {
                    const double out = expr.is_removal ? c * (1-std::clamp(result, 0.0, 1.0)) : std::clamp(result, 0.0, c);
                    r = concentrations[pollutant] > 0 ? 1-out/concentrations[pollutant] : 0;
                }
            }
        }
        return removals[pollutant] = r;
    };
    return concentration * (1-evaluate(p, evaluate));
}
}
void prepareQuality(SimulationContext& ctx, double dt) {
    const int np = ctx.n_pollutants(); if (np == 0 || dt <= 0) return;
    auto& st = ctx.node_subtypes.storages;
    for (int r = 0; r < st.count(); ++r) {
        auto& state = st.lid_state[r]; if (state.cells.empty() || state.quality_old_water.size() != state.cells.size()) continue;
        const int node = st.node_idx[r]; const int base = node*np;
        auto water = state.quality_old_water; double mobile = state.quality_old_mobile;
        std::vector<double> mobile_mass(np), lateral_c(np);
        if (state.quality_mass.size() != water.size()*np) {
            state.quality_mass.resize(water.size()*np);
            for (std::size_t i=0;i<water.size();++i) for (int p=0;p<np;++p) state.quality_mass[i*np+p]=water[i]*ctx.nodes.conc_old[base+p];
        }
        for (int p=0;p<np;++p) {
            mobile_mass[p] = ctx.nodes.conc_old[base+p]*mobile;
            if (state.richards) {
                mobile_mass[p] += state.quality_mass[p]; // dry pond solute redissolves on supply
                state.quality_mass[p] = 0;
            }
            lateral_c[p] = ctx.nodes.qual_vol_in[node] > 0 ? std::max(0.0, ctx.nodes.qual_mass_in[base+p]*dt/ctx.nodes.qual_vol_in[node]) : 0;
        }
        const auto stack = layers(ctx, st.lid[r].control);
        for (const auto& transfer : state.quality_transfers) {
            const int from = transfer.from, to = transfer.to; const double v = transfer.volume;
            if (to == -3) {
                if (from < 0) mobile = std::max(0., mobile - v);
                else water[from] = std::max(0., water[from] - v);
                continue; // evaporation leaves solute
            }
            const double available = from == -2 ? v : from < 0 ? mobile : water[from];
            std::vector<double> c(np);
            for (int p=0;p<np;++p) c[p] = from == -2 ? lateral_c[p] : available > 0 ? (from < 0 ? mobile_mass[p] : state.quality_mass[from*np+p])/available : 0;
            const bool exits = from >= 0 && (to < 0 || state.cells[from].layer != state.cells[to].layer);
            for (int p=0;p<np;++p) {
                const double resident = from == -2 ? std::max(0.0, v*c[p]) : std::max(0.0, from < 0 ? mobile_mass[p] : state.quality_mass[from*np+p]);
                const double mass = state.richards ? std::clamp(std::min(v,available)*c[p], 0.0, resident) : std::min(v,available)*c[p];
                const double removal = state.richards && exits && c[p] > 0 ? std::clamp(treatedConcentration(ctx,state.cells[from],stack[state.cells[from].layer-1],p,c[p],c,layerVolume(state,water,from),v/dt,dt)/c[p],0.0,1.0) : 1.0;
                const double out = state.richards ? mass*removal : exits ? std::min(v,available)*treatedConcentration(ctx,state.cells[from],stack[state.cells[from].layer-1],p,c[p],c,layerVolume(state,water,from),v/dt,dt) : mass;
                if (from == -2) {
                    auto& incoming=ctx.nodes.qual_mass_in[base+p];
                    const double prior=incoming;
                    incoming-=mass/dt;
                    // A fully captured lateral load can cancel to a tiny
                    // negative rate. It is roundoff, not an extraction source.
                    if(incoming<0 && -incoming<=64*std::numeric_limits<double>::epsilon()*std::max(1.0,std::abs(prior))) incoming=0;
                }
                else if (from < 0) mobile_mass[p] -= mass;
                else state.quality_mass[from*np+p] -= mass;
                if (to == -4) ctx.mass_balance.qual_routing_seep[p] += out;
                else if (to < 0) mobile_mass[p] += out; else state.quality_mass[to*np+p] += out;
                ctx.mass_balance.qual_routing_reacted[p] += mass-out;
            }
            if (from == -2) ctx.nodes.qual_vol_in[node] = std::max(0.0,ctx.nodes.qual_vol_in[node]-v);
            else if (from < 0) mobile=std::max(0.0,mobile-v); else water[from]=std::max(0.0,water[from]-v);
            if (to != -4) { if (to < 0) mobile += v; else water[to] += v; }
        }
        state.quality_outlet_conc.assign(ctx.n_links()*np,std::numeric_limits<double>::quiet_NaN());
        for (const auto& port : state.quality_ports) if (port.volume < 0) {
            const int i=port.cell; const double v=-port.volume;
            std::vector<double> c(np);
            for(int p=0;p<np;++p) c[p]=water[i]>0?state.quality_mass[i*np+p]/water[i]:0;
            for(int p=0;p<np;++p) {
                const double out=treatedConcentration(ctx,state.cells[i],stack[state.cells[i].layer-1],p,c[p],c,layerVolume(state,water,i),v/dt,dt);
                const double mass=state.richards ? std::clamp(std::min(v,water[i])*c[p],0.0,std::max(0.0,state.quality_mass[i*np+p])) : std::min(v,water[i])*c[p];
                const double accepted_out=state.richards ? c[p]>0 ? mass*std::clamp(out/c[p],0.0,1.0) : 0.0 : std::min(v,water[i])*out;
                state.quality_mass[i*np+p]-=mass;
                state.quality_outlet_conc[port.link*np+p]=v>0?accepted_out/v:0;
                ctx.mass_balance.qual_routing_reacted[p]+=mass-accepted_out;
            }
            water[i]=std::max(0.0,water[i]-v);
        }
        for (std::size_t i=0;i<water.size();++i) for(int p=0;p<np;++p) {
            const auto& rules=stack[state.cells[i].layer-1].treatment;
            const auto rule=std::find_if(rules.begin(),rules.end(),[&](const auto& value) {return ctx.pollutant_names.find(value.pollutant)==p;});
            // Layer decay supplements the pollutant's background decay.
            const double rate=ctx.pollutants.k_decay[p] + (rule!=rules.end() ? rule->decay/86400.0 : 0.0);
            auto& mass=state.quality_mass[i*np+p];
            const double after=mass*std::exp(-rate*dt);
            ctx.mass_balance.qual_routing_reacted[p]+=mass-after; mass=after;
        }
        // Saturated water shares the storage-node reactor. Weight each layer's
        // decay by its share of that mobile water, so saturated aggregate and
        // media remain active treatment volumes as the water table rises.
        const double mobile_volume = ctx.nodes.old_volume[node];
        std::vector<double> mobile_rates(np, 0.0);
        for (const auto& cell : state.cells) {
            if (state.richards && cell.kind != LidNodeLayerKind::Surface) continue;
            const double fraction = std::clamp((ctx.nodes.old_depth[node] - cell.bottom) / (cell.top-cell.bottom), 0.0, 1.0);
            const double v = cell.geometric_volume * std::max(0.0, cell.porosity-cell.theta) * fraction;
            for (const auto& rule : stack[cell.layer-1].treatment) {
                const int p = ctx.pollutant_names.find(rule.pollutant);
                if (p >= 0 && mobile_volume > 0) mobile_rates[p] += rule.decay * std::min(1.0, v/mobile_volume);
            }
        }
        for (int p=0;p<np;++p) {
            const double after = mobile_mass[p] * std::exp(-mobile_rates[p]*dt/86400.0);
            ctx.mass_balance.qual_routing_reacted[p] += mobile_mass[p]-after;
            mobile_mass[p] = after;
            if (mobile_volume > (state.richards ? 1.e-12 : 0.0)) ctx.nodes.conc_old[base+p] = after/mobile_volume;
            else {
                // Preserve residual solute when the last mobile water moves
                // into retained pores or evaporates; it redissolves there.
                state.quality_mass[p] += after;
                ctx.nodes.conc_old[base+p] = 0;
            }
        }
    }
}
// Refresh only mobile outlets against the mixture used by the coupled node
// solve. Retained outlet mass was already debited in prepareQuality; its
// concentration must stay fixed while the mobile mixtures converge.
void prepareOutletQuality(SimulationContext& ctx, double dt, bool book_reaction) {
    const int np = ctx.n_pollutants();
    if (np <= 0) return;
    auto& st = ctx.node_subtypes.storages;
    for (int r=0; r<st.count(); ++r) {
        auto& state=st.lid_state[r];
        if(state.cells.empty()) continue;
        const int node=st.node_idx[r], base=node*np;
        const auto stack=layers(ctx,st.lid[r].control);
        std::vector<double> mobile_layers(stack.size(),0.0);
        for(const auto& cell:state.cells) {
            const double fraction=std::clamp((ctx.nodes.old_depth[node]-cell.bottom)/(cell.top-cell.bottom),0.0,1.0);
            mobile_layers[cell.layer-1]+=cell.geometric_volume*std::max(0.0,cell.porosity-cell.theta)*fraction;
        }
        std::vector<double> c(np);
        for(int p=0;p<np;++p) c[p]=ctx.nodes.conc[base+p];
        for(int link=0;link<ctx.n_links();++link) {
            const double q=ctx.links.flow[link];
            const int source=q>=0?ctx.links.node1[link]:ctx.links.node2[link];
            if(source!=node || q==0) continue;
            if(std::any_of(state.quality_ports.begin(),state.quality_ports.end(),[&](const auto& port){return port.link==link && port.volume<0;})) continue;
            const double offset=portOffset(ctx,link,node);
            const int cell_index=portCell(state,offset);
            if(cell_index<0) continue;
            const auto* cell=&state.cells[cell_index];
            if(stack[cell->layer-1].treatment.empty()) continue;
            for(int p=0;p<np;++p) {
                const double out=treatedConcentration(ctx,*cell,stack[cell->layer-1],p,c[p],c,mobile_layers[cell->layer-1],std::abs(q),dt);
                state.quality_outlet_conc[link*np+p]=out;
                if(book_reaction) ctx.mass_balance.qual_routing_reacted[p]+=(c[p]-out)*std::abs(q)*dt;
            }
        }
    }
}

bool receiveQuality(SimulationContext& ctx,int node,int link,double volume,int p,double mass,double dt) {
    const int r=ctx.node_subtypes.storage_row(node); if(r<0||volume<=0)return false;
    auto& state=ctx.node_subtypes.storages.lid_state[r];
    for(const auto& port:state.quality_ports) if(port.link==link&&port.volume>0) {
        const double held=mass*std::clamp(port.volume/volume,0.0,1.0);
        if(state.quality_mass.size()>static_cast<std::size_t>(port.cell*ctx.n_pollutants()+p)) state.quality_mass[port.cell*ctx.n_pollutants()+p]+=held*dt;
        ctx.nodes.qual_mass_in[node*ctx.n_pollutants()+p]-=held;
        return true;
    }
    return false;
}
double outletQuality(const SimulationContext& ctx,int node,int link,int p,double fallback) {
    const int r=ctx.node_subtypes.storage_row(node);if(r<0)return fallback;
    const auto& values=ctx.node_subtypes.storages.lid_state[r].quality_outlet_conc;
    const int i=link*ctx.n_pollutants()+p;
    return i>=0&&i<static_cast<int>(values.size())&&std::isfinite(values[i])?values[i]:fallback;
}
double heldMass(const SimulationContext& ctx,int node,int p) {
    const int r=ctx.node_subtypes.storage_row(node);if(r<0)return 0;
    const auto& state=ctx.node_subtypes.storages.lid_state[r];
    double mass=0;const int np=ctx.n_pollutants();
    if (np <= 0 || p < 0 || p >= np) return 0;
    for(std::size_t i=p;i<state.quality_mass.size();i+=np)mass+=state.quality_mass[i];
    if(state.quality_mass.empty() && np>0 && node*np+p<static_cast<int>(ctx.nodes.conc.size())) mass=heldVolume(ctx,node)*ctx.nodes.conc[node*np+p];
    return mass;
}
}
