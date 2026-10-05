// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
#include "LidNode.hpp"
#include "../core/SimulationContext.hpp"
#include "../core/UnitConversion.hpp"
#include "../input/Tokenizer.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <set>

namespace openswmm::lidnode {
namespace {
bool number(const std::string& s, double& v) {
    char* end = nullptr; v = std::strtod(s.c_str(), &end);
    return end != s.c_str() && *end == '\0' && std::isfinite(v);
}
double rainDepth(const SimulationContext& ctx) {
    return ucf::Ucf[ucf::RAINDEPTH][ucf::getUnitSystem(static_cast<int>(ctx.options.flow_units))];
}
int outletNode(const SimulationContext& ctx, int link) {
    if (link < 0 || link >= ctx.n_links()) return -1;
    int found = -1;
    for (int node : {ctx.links.node1[link], ctx.links.node2[link]}) {
        const int r = ctx.node_subtypes.storage_row(node);
        if (r >= 0 && ctx.node_subtypes.storages.lid[r].control >= 0) {
            if (found >= 0) return -1; // three-column grammar is ambiguous for two LID ends
            found = node;
        }
    }
    return found;
}
double anchorHeight(const std::vector<LidNodeLayer>& stack, int layer, bool top) {
    int n = 0; double total = thickness(stack), down = 0.0;
    for (const auto& l : stack) {
        if (l.kind == LidNodeLayerKind::Bottom) continue;
        ++n;
        if (n == layer) return total - down - (top ? 0.0 : l.params[0]);
        down += l.params[0];
    }
    return -1.0;
}
}
std::vector<LidNodeLayer> layers(const SimulationContext& ctx, int control) {
    const auto& c = ctx.lid_controls;
    if (control < 0 || control >= c.count()) return {};
    const auto& type = c.lid_type[control];
    if (type == "NODE") return control < static_cast<int>(c.node_layers.size()) ? c.node_layers[control] : std::vector<LidNodeLayer>{};
    if (type != "BC" && type != "RG" && type != "IT" && type != "PP") return {};
    std::vector<LidNodeLayer> out;
    const auto& s = c.surface[control];
    if (s[0] > 0.0) out.push_back({LidNodeLayerKind::Surface, {s[0], s[1]}});
    // Pavement void ratio is converted once; impermeable paver fraction reduces
    // available pore space. Its hydraulic layer remains above the soil.
    const auto& p = c.pavement[control];
    if (type == "PP" && p[0] > 0.0)
        out.push_back({LidNodeLayerKind::Aggregate, {p[0], p[1] / (1.0 + p[1]) * (1.0 - p[2]), p[3]}});
    const auto& m = c.soil[control];
    if (m[0] > 0.0) out.push_back({LidNodeLayerKind::Media, m});
    const auto& g = c.storage[control];
    if (g[0] > 0.0) {
        // Standard storage K is native-soil seepage, not gravel conductivity.
        // Use effectively free drainage within the aggregate.
        out.push_back({LidNodeLayerKind::Aggregate, {g[0], g[1] / (1.0 + g[1]), 1.e12}});
        out.push_back({LidNodeLayerKind::Bottom, {g[2], g[3]}});
    }
    return out;
}
bool validLayer(const LidNodeLayer& l) {
    const auto& p = l.params;
    for (double v : p) if (!std::isfinite(v) || v < 0.0) return false;
    const int used = l.kind == LidNodeLayerKind::Media ? 7 : l.kind == LidNodeLayerKind::Aggregate ? 3 : 2;
    for (int i = used; i < 7; ++i) if (p[i] != 0.0) return false;
    switch (l.kind) {
    case LidNodeLayerKind::Surface: return p[0] > 0.0 && p[1] < 1.0;
    case LidNodeLayerKind::Media: return p[0] > 0.0 && p[1] > 0.0 && p[1] <= 1.0 && p[3] < p[2] && p[2] < p[1] && p[5] > 0.0;
    case LidNodeLayerKind::Aggregate: return p[0] > 0.0 && p[1] > 0.0 && p[1] <= 1.0;
    case LidNodeLayerKind::Bottom: return true;
    }
    return false;
}
std::string validateStack(const std::vector<LidNodeLayer>& stack) {
    bool porous = false;
    for (std::size_t i = 0; i < stack.size(); ++i) {
        const auto& l = stack[i];
        if (!validLayer(l)) return "invalid layer parameters at row " + std::to_string(i + 1);
        if (l.kind == LidNodeLayerKind::Surface && i != 0) return "SURFACE must occur once, first";
        if (l.kind == LidNodeLayerKind::Bottom && i + 1 != stack.size()) return "BOTTOM must occur once, last";
        porous |= l.kind == LidNodeLayerKind::Media || l.kind == LidNodeLayerKind::Aggregate;
    }
    return porous ? "" : "at least one MEDIA or AGGREGATE layer is required";
}
double thickness(const std::vector<LidNodeLayer>& stack) {
    double d = 0.0;
    for (const auto& l : stack) if (l.kind != LidNodeLayerKind::Bottom) d += l.params[0];
    return d;
}
void readNodes(SimulationContext& ctx, const std::vector<std::string>& lines) {
    for (const auto& line : lines) {
        auto t = input::Tokenizer::tokenize(line);
        if (t.empty()) continue;
        double sat = 0.0;
        if (t.size() != 3 || !number(t[2], sat) || sat < 0.0 || sat > 100.0) {
            ctx.errors.push_back("Invalid [LID_NODES] row: " + line); continue;
        }
        const int n = ctx.node_names.find(t[0]), c = ctx.lid_names.find(t[1]);
        if (n < 0 || c < 0) { ctx.deferred_section_rows.emplace_back("LID_NODES", line); continue; }
        const int r = ctx.node_subtypes.storage_row(n);
        if (r < 0) { ctx.errors.push_back("[LID_NODES] requires a storage node: " + t[0]); continue; }
        auto& config = ctx.node_subtypes.storages.lid[r];
        if (config.control >= 0) { ctx.errors.push_back("Duplicate [LID_NODES] assignment: " + t[0]); continue; }
        config = {c, sat};
    }
}
void readOutlets(SimulationContext& ctx, const std::vector<std::string>& lines) {
    for (const auto& line : lines) {
        auto t = input::Tokenizer::tokenize(line);
        if (t.empty()) continue;
        double layer = 0.0;
        const std::string pos = t.size() == 3 ? input::Tokenizer::to_upper(t[2]) : "";
        if (t.size() != 3 || !number(t[1], layer) || layer < 1.0 || layer > 2147483647.0 || layer != std::floor(layer) || (pos != "TOP" && pos != "BOTTOM")) {
            ctx.errors.push_back("Invalid [LID_NODE_OUTLETS] row: " + line); continue;
        }
        int link = ctx.link_names.find(t[0]);
        if (link < 0) { ctx.deferred_section_rows.emplace_back("LID_NODE_OUTLETS", line); continue; }
        auto& out = ctx.lid_node_outlets;
        if (std::any_of(out.begin(), out.end(), [link](const auto& a) { return a.link == link; })) {
            ctx.errors.push_back("Duplicate LID outlet anchor: " + t[0]); continue;
        }
        out.push_back({link, static_cast<int>(layer), pos == "TOP"});
    }
}
// Weirs and rating outlets keep their physical crest in subtype tables,
// not links.offset1. All LID coupling must use that actual port elevation.
double portOffset(const SimulationContext& ctx, int link, int node) {
    double offset=ctx.links.offset1[link];
    const int wr=ctx.link_subtypes.weir_row(link), out=ctx.link_subtypes.outlet_row(link);
    if(wr>=0) offset=ctx.link_subtypes.weirs.crest_height[wr];
    else if(out>=0) offset=ctx.link_subtypes.outlets.crest_height[out];
    else return ctx.links.node1[link]==node?offset:ctx.links.offset2[link];
    // Keep a crest authored relative to its own node exact. Adding its datum
    // before subtracting the same datum can move an interface by one ULP.
    return offset+(ctx.nodes.invert_elev[ctx.links.node1[link]]-ctx.nodes.invert_elev[node]);
}
void validate(SimulationContext& ctx) {
    for (int c = 0; c < ctx.lid_controls.count(); ++c) {
        if (ctx.lid_controls.lid_type[c] != "NODE") continue;
        for(const auto& layer:layers(ctx,c))for(const auto& rule:layer.treatment) {
            std::string diagnostic;
            if(!validTreatment(ctx,rule,diagnostic))ctx.errors.push_back("LID treatment: "+diagnostic);
        }
        auto error = validateStack(layers(ctx, c));
        if (!error.empty()) ctx.errors.push_back("LID control " + ctx.lid_controls.names[c] + ": " + error);
    }
    const auto& st = ctx.node_subtypes.storages;
    const bool has_lid = std::any_of(st.lid.begin(), st.lid.end(), [](const auto& c) { return c.control >= 0; });
    if (has_lid && ctx.options.routing_model != RoutingModel::DYNWAVE)
        ctx.errors.push_back("LID storage nodes require FLOW_ROUTING DYNWAVE");
    if (has_lid && ctx.n_pollutants() > 0 && ctx.options.quality_solver != QualitySolverKind::LEGACY)
        ctx.errors.push_back("LID layer pollutant routing requires QUALITY_SOLVER LEGACY");
    for (int r = 0; r < st.count(); ++r) {
        const auto& cfg = st.lid[r];
        if (cfg.control < 0) continue;
        const int n = st.node_idx[r];
        const auto stack = layers(ctx, cfg.control);
        const auto error = validateStack(stack);
        if (!error.empty()) { ctx.errors.push_back("LID node " + ctx.node_names.name_of(n) + ": " + error); continue; }
        const double d = thickness(stack) / rainDepth(ctx);
        if (std::abs(d - ctx.nodes.full_depth[n]) > 1.e-7 * std::max(1.0, d))
            ctx.errors.push_back("LID node " + ctx.node_names.name_of(n) + ": MaxDepth must equal the sum of layer thicknesses");
        if (ctx.lid_controls.lid_type[cfg.control] != "NODE") {
            const auto& surf = ctx.lid_controls.surface[cfg.control];
            const auto& drain = ctx.lid_controls.drain[cfg.control];
            if (surf[2] != 0.0 || surf[3] != 0.0 || surf[4] != 0.0 || std::any_of(drain.begin(), drain.end(), [](double v) { return v != 0.0; }))
                ctx.warnings.push_back("LID node " + ctx.node_names.name_of(n) + ": surface roughness/slopes and DRAIN parameters are ignored; use hydraulic links");
        }
    }
    for (const auto& anchor : ctx.lid_node_outlets) {
        const int n = outletNode(ctx, anchor.link);
        if (n < 0) { ctx.errors.push_back("LID outlet anchor requires exactly one LID endpoint"); continue; }
        const int r = ctx.node_subtypes.storage_row(n);
        const double h = anchorHeight(layers(ctx, st.lid[r].control), anchor.layer, anchor.top);
        if (h < 0.0) { ctx.errors.push_back("LID outlet anchor layer is outside the stack"); continue; }
        const double offset = portOffset(ctx,anchor.link,n);
        if (std::abs(offset - h / rainDepth(ctx)) > 1.e-7)
            ctx.warnings.push_back("LID outlet " + ctx.link_names.name_of(anchor.link) + ": offset does not match the anchored layer interface");
    }
}
void sync(SimulationContext& ctx, int control) {
    const auto stack = layers(ctx, control);
    if (!validateStack(stack).empty()) return;
    const double d = thickness(stack) / rainDepth(ctx);
    auto& st = ctx.node_subtypes.storages;
    for (int r = 0; r < st.count(); ++r) if (st.lid[r].control == control) {
        st.lid_state[r] = {};
        const int n = st.node_idx[r];
        ctx.nodes.full_depth[n] = d;
        ctx.nodes.full_volume[n] = 0.0; // derived geometry must be recomputed at initialize
    }
    for (const auto& a : ctx.lid_node_outlets) {
        const int n = outletNode(ctx, a.link), r = ctx.node_subtypes.storage_row(n);
        if (r < 0 || st.lid[r].control != control) continue;
        const double h = anchorHeight(stack, a.layer, a.top);
        if (h < 0.0) continue;
        const double elevation=ctx.nodes.invert_elev[n]+h/rainDepth(ctx);
        const int wr=ctx.link_subtypes.weir_row(a.link), out=ctx.link_subtypes.outlet_row(a.link);
        if(wr>=0) ctx.link_subtypes.weirs.crest_height[wr]=elevation-ctx.nodes.invert_elev[ctx.links.node1[a.link]];
        else if(out>=0) ctx.link_subtypes.outlets.crest_height[out]=elevation-ctx.nodes.invert_elev[ctx.links.node1[a.link]];
        else (ctx.links.node1[a.link] == n ? ctx.links.offset1[a.link] : ctx.links.offset2[a.link]) = h / rainDepth(ctx);
    }
}
} // namespace openswmm::lidnode

// Conservative operator split: retained moisture is stored once in cells;
// node.volume contains mobile, hydraulically connected water. The hydraulic
// capacity of a slice is its pore volume minus retained moisture. Internal
// transfers change these two stores by equal and opposite amounts.
#include "../hydraulics/Node.hpp"
namespace openswmm::lidnode {
bool active(const SimulationContext& ctx, int node) {
    int r = ctx.node_subtypes.storage_row(node);
    return r >= 0 && !ctx.node_subtypes.storages.lid_state[r].cells.empty();
}
double heldVolume(const SimulationContext& ctx, int node) {
    int r = ctx.node_subtypes.storage_row(node);
    return r >= 0 ? ctx.node_subtypes.storages.lid_state[r].held_volume : 0.0;
}
void initialize(SimulationContext& ctx) {
    auto& st = ctx.node_subtypes.storages;
    const int us = ucf::getUnitSystem(static_cast<int>(ctx.options.flow_units));
    const double rd = rainDepth(ctx), rf = ucf::Ucf[ucf::RAINFALL][us];
    for (int r = 0; r < st.count(); ++r) {
        st.lid_state[r] = {};
        if (st.lid[r].control < 0) continue;
        const int n = st.node_idx[r];
        ctx.nodes.full_volume[n] = 0.0;
        const auto stack = layers(ctx, st.lid[r].control);
        if (!validateStack(stack).empty()) continue;
        double top = thickness(stack) / rd;
        std::vector<LidNodeCell> cells;
        int layer = 0;
        for (const auto& l : stack) {
            if (l.kind == LidNodeLayerKind::Bottom) continue;
            ++layer;
            const int count = l.kind == LidNodeLayerKind::Media ? 5 : 1;
            const double dz = l.params[0] / rd / count;
            for (int i = 0; i < count; ++i) {
                LidNodeCell c;
                c.top = top; c.bottom = std::max(0.0, top - dz); c.kind = l.kind; c.layer = layer;
                // No runtime cells have been installed yet, so these calls
                // evaluate the authored geometric storage relation.
                const double vtop = node::getVolume(ctx.nodes, n, c.top, &ctx.tables, us, &ctx.node_subtypes);
                const double vbottom = node::getVolume(ctx.nodes, n, c.bottom, &ctx.tables, us, &ctx.node_subtypes);
                c.geometric_volume = std::max(0.0, vtop - vbottom);
                c.area = c.geometric_volume / dz;
                c.porosity = l.kind == LidNodeLayerKind::Surface ? 1.0 - l.params[1] : l.params[1];
                if (l.kind == LidNodeLayerKind::Media) {
                    c.field_capacity = l.params[2]; c.wilting_point = l.params[3];
                    c.conductivity = l.params[4] / rf; c.exponent = l.params[5]; c.suction = l.params[6] / rd;
                    c.theta = c.wilting_point + st.lid[r].initial_saturation / 100.0 * (c.porosity - c.wilting_point);
                } else if (l.kind == LidNodeLayerKind::Aggregate) {
                    c.conductivity = l.params[2] / rf;
                    c.theta = st.lid[r].initial_saturation / 100.0 * c.porosity;
                }
                if (c.area <= 0.0) ctx.errors.push_back("LID node " + ctx.node_names.name_of(n) + ": layers require positive geometric storage area");
                cells.push_back(c); top = c.bottom;
            }
        }
        auto& state = st.lid_state[r];
        state.cells = std::move(cells);
        state.port_delta.assign(state.cells.size(), 0.0);
        // Fully saturated cells connected to the base belong to the mobile
        // hydraulic store above field capacity, including InitSat=100%.
        double water_table = node::getDepth(ctx.nodes, n, 0.0, &ctx.tables, us, &ctx.node_subtypes);
        for (auto& c : state.cells) {
            if (c.top <= water_table && c.theta > c.field_capacity) {
                ctx.nodes.volume[n] += (c.theta - c.field_capacity) * c.geometric_volume;
                c.theta = c.field_capacity;
            }
            state.held_volume += c.theta * c.geometric_volume;
        }
        ctx.nodes.full_volume[n] = 0.0;
    }
}
void prepareStep(SimulationContext& ctx, double dt, double evaporation) {
    if (!(dt > 0.0)) return;
    auto& st = ctx.node_subtypes.storages;
    const int us = ucf::getUnitSystem(static_cast<int>(ctx.options.flow_units));
    for (int r = 0; r < st.count(); ++r) {
        auto& state = st.lid_state[r];
        if (state.cells.empty()) continue;
        const int n = st.node_idx[r];
        const double before = state.held_volume;
        state.quality_old_mobile = ctx.nodes.volume[n];
        state.quality_old_water.clear(); state.quality_transfers.clear();
        for (const auto& cell : state.cells) state.quality_old_water.push_back(cell.theta * cell.geometric_volume);
        state.captured_flow = 0.0; state.evap_volume = 0.0;
        for (auto& c : state.cells)
            // A submerged cell retains field-capacity water on recession.
            // The before/after store difference below transfers this water
            // from (or to) mobile storage without changing the total.
            if (c.top <= ctx.nodes.depth[n]) {
                const int i = static_cast<int>(&c - state.cells.data());
                const double v = (c.field_capacity - c.theta) * c.geometric_volume;
                if (v > 0) state.quality_transfers.push_back({-1, i, v});
                else if (v < 0) state.quality_transfers.push_back({i, -1, -v});
                c.theta = c.field_capacity;
            }

        // Lateral inflows enter the top of the column. Saturated capacity that
        // cannot be retained stays in the hydraulic inflow and raises the HGL.
        auto& first = state.cells.front();
        const double capacity = std::max(0.0, (first.porosity - first.theta) * first.geometric_volume);
        const double capture = std::min(std::max(0.0, ctx.nodes.lat_flow[n]) * dt, capacity);
        if (first.geometric_volume > 0.0) first.theta += capture / first.geometric_volume;
        state.captured_flow = capture / dt;
        if (capture > 0) state.quality_transfers.push_back({-2, 0, capture});
        state.treated_volume += capture;
        ctx.nodes.lat_flow[n] -= state.captured_flow;
        // Evaporation draws from top to bottom, never below wilting point.
        double evap = std::max(0.0, evaporation * st.evap_frac[r] * first.area * dt);
        for (auto& c : state.cells) {
            const double take = std::min(evap, std::max(0.0, (c.theta - c.wilting_point) * c.geometric_volume));
            if (c.geometric_volume > 0.0) c.theta -= take / c.geometric_volume;
            state.evap_volume += take; evap -= take;
            if (take > 0) state.quality_transfers.push_back({static_cast<int>(&c - state.cells.data()), -3, take});
        }
        // Bounded forward-Euler method of lines. A shared transfer is removed
        // from its source and added to its receiver exactly once. Substeps keep
        // travel times resolved; each transfer is additionally capacity limited.
        double remaining = dt;
        while (remaining > 0.0) {
            const double step = std::min(remaining, 1.0);
            std::vector<double> transfer(state.cells.size(), 0.0);
            for (std::size_t i = 0; i < state.cells.size(); ++i) {
                const auto& c = state.cells[i];
                // A moving water table replaces free drainage below itself.
                if (c.bottom < ctx.nodes.depth[n]) continue;
                double q = 0.0;
                if (c.kind == LidNodeLayerKind::Surface && i + 1 < state.cells.size()) {
                    const auto& below = state.cells[i + 1];
                    const double deficit = std::max(0.0, below.porosity - below.theta);
                    q = below.conductivity * (1.0 + below.suction * deficit / std::max(below.suction + c.theta * (c.top - c.bottom), 1.e-12)) * c.area;
                } else if (c.theta > c.field_capacity) {
                    const double ratio = std::clamp(c.theta / c.porosity, 0.0, 1.0);
                    q = c.conductivity * std::pow(ratio, c.exponent) * c.area;
                }
                double v = std::min(q * step, std::max(0.0, (c.theta - c.field_capacity) * c.geometric_volume));
                if (i + 1 < state.cells.size()) {
                    const auto& below = state.cells[i + 1];
                    if (below.bottom >= ctx.nodes.depth[n])
                        v = std::min(v, std::max(0.0, (below.porosity - below.theta) * below.geometric_volume));
                }
                transfer[i] = v;
            }
            for (std::size_t i = 0; i < state.cells.size(); ++i) {
                auto& c = state.cells[i];
                const int dest = i + 1 < state.cells.size() && state.cells[i + 1].bottom >= ctx.nodes.depth[n] ? static_cast<int>(i + 1) : -1;
                if (transfer[i] > 0) state.quality_transfers.push_back({static_cast<int>(i), dest, transfer[i]});
                if (c.geometric_volume > 0.0) c.theta -= transfer[i] / c.geometric_volume;
                if (i + 1 < state.cells.size()) {
                    auto& below = state.cells[i + 1];
                    if (below.bottom >= ctx.nodes.depth[n] && below.geometric_volume > 0.0)
                        below.theta += transfer[i] / below.geometric_volume;
                }
            }
            remaining -= step;
        }
        state.held_volume = 0.0;
        for (const auto& c : state.cells) state.held_volume += c.theta * c.geometric_volume;
        ctx.nodes.volume[n] += before + capture - state.evap_volume - state.held_volume;
        // This is an internal redistribution before the routing step, so both
        // storage capacity and the initial hydraulic head must be refreshed.
        ctx.nodes.full_volume[n] = 0.0;
        ctx.nodes.full_volume[n] = node::getVolume(ctx.nodes, n, ctx.nodes.full_depth[n], &ctx.tables, us, &ctx.node_subtypes);
        ctx.nodes.rpt_full_volume[n] = ctx.nodes.full_volume[n];
        ctx.nodes.depth[n] = node::getDepth(ctx.nodes, n, ctx.nodes.volume[n], &ctx.tables, us, &ctx.node_subtypes);
        if (ctx.options.allow_ponding && ctx.nodes.ponded_area[n] > 0.0 && ctx.nodes.volume[n] > ctx.nodes.full_volume[n])
            ctx.nodes.depth[n] = ctx.nodes.full_depth[n] + (ctx.nodes.volume[n] - ctx.nodes.full_volume[n]) / ctx.nodes.ponded_area[n];
        ctx.nodes.head[n] = ctx.nodes.invert_elev[n] + ctx.nodes.depth[n];
    }
}
void finishStep(SimulationContext& ctx) {
    auto& st = ctx.node_subtypes.storages;
    for (int r = 0; r < st.count(); ++r) {
        auto& state = st.lid_state[r];
        if (state.cells.empty()) continue;
        const int n = st.node_idx[r];
        for (std::size_t i = 0; i < state.cells.size(); ++i) {
            auto& cell = state.cells[i];
            if (cell.geometric_volume > 0.0) cell.theta += state.port_delta[i] / cell.geometric_volume;
        }
        state.held_volume = 0.0;
        for (const auto& cell : state.cells) state.held_volume += cell.theta * cell.geometric_volume;
        const int us = ucf::getUnitSystem(static_cast<int>(ctx.options.flow_units));
        ctx.nodes.full_volume[n] = 0.0;
        ctx.nodes.full_volume[n] = node::getVolume(ctx.nodes, n, ctx.nodes.full_depth[n], &ctx.tables, us, &ctx.node_subtypes);
        ctx.nodes.rpt_full_volume[n] = ctx.nodes.full_volume[n];
        ctx.nodes.depth[n] = node::getDepth(ctx.nodes, n, ctx.nodes.volume[n], &ctx.tables, us, &ctx.node_subtypes);
        if (ctx.options.allow_ponding && ctx.nodes.ponded_area[n] > 0.0 && ctx.nodes.volume[n] > ctx.nodes.full_volume[n])
            ctx.nodes.depth[n] = ctx.nodes.full_depth[n] + (ctx.nodes.volume[n] - ctx.nodes.full_volume[n]) / ctx.nodes.ponded_area[n];
        ctx.nodes.head[n] = ctx.nodes.invert_elev[n] + ctx.nodes.depth[n];
        ctx.nodes.lat_flow[st.node_idx[r]] += state.captured_flow;
        st.evap_loss[r] += state.evap_volume;
        state.captured_flow = 0.0;
    }
}
} // namespace openswmm::lidnode

namespace openswmm::lidnode {
double bottomConductivity(const SimulationContext& c, int r) {
    if (r < 0) return 0.0;
    const auto& st = c.node_subtypes.storages;
    if (st.lid[r].control >= 0) {
        const auto stack = layers(c, st.lid[r].control);
        if (!stack.empty() && stack.back().kind == LidNodeLayerKind::Bottom) return stack.back().params[0];
    }
    return st.exfil_ksat[r];
}
double bottomCloggingFactor(const SimulationContext& c, int r) {
    if (r < 0 || c.node_subtypes.storages.lid[r].control < 0) return 1.0;
    const auto& st = c.node_subtypes.storages;
    const auto stack = layers(c, st.lid[r].control);
    if (stack.empty() || stack.back().kind != LidNodeLayerKind::Bottom || stack.back().params[1] <= 0.0) return 1.0;
    double pores = 0.0;
    for (const auto& cell : st.lid_state[r].cells) if (cell.kind != LidNodeLayerKind::Surface) pores += cell.porosity * cell.geometric_volume;
    return pores > 0.0 ? std::max(0.0, 1.0 - st.lid_state[r].treated_volume / (stack.back().params[1] * pores)) : 1.0;
}
}

namespace openswmm::lidnode {
int portCell(const LidNodeState& state, double offset) {
    for (int i = 0; i < static_cast<int>(state.cells.size()); ++i) {
        const auto& cell=state.cells[i];
        // An interface belongs to the cell above it. Snap only roundoff-sized
        // differences, not physical offsets near an interface. The same rule
        // is used for hydraulic head, water withdrawals and outlet treatment.
        const double scale=std::max({1.0,std::abs(offset),std::abs(cell.bottom),std::abs(cell.top)});
        const double tolerance=32*std::numeric_limits<double>::epsilon()*scale;
        if (offset >= cell.bottom-tolerance && offset < cell.top-tolerance) return i;
    }
    return state.cells.empty() ? -1 : 0;
}
void resetPorts(SimulationContext& ctx) {
    for (auto& s : ctx.node_subtypes.storages.lid_state) {
        std::fill(s.port_delta.begin(), s.port_delta.end(), 0.0);
        s.mobile_delta = 0.0; s.quality_ports.clear();
    }
}
double portDepth(const SimulationContext& ctx, int node, double offset) {
    const double head = ctx.nodes.depth[node];
    const int r = ctx.node_subtypes.storage_row(node);
    if (r < 0) return head;
    const auto& state = ctx.node_subtypes.storages.lid_state[r];
    const int i = portCell(state, offset);
    if (i < 0 || state.cells[i].kind != LidNodeLayerKind::Surface || state.cells[i].theta <= 0.0) return head;
    const auto& c = state.cells[i];
    return std::max(head, c.bottom + c.theta / c.porosity * (c.top - c.bottom));
}
double exchangePorts(SimulationContext& ctx, int link, double flow, double dt) {
    if (!(dt > 0) || flow == 0) return flow;
    if (!active(ctx, ctx.links.node1[link]) && !active(ctx, ctx.links.node2[link])) return flow;
    const int source = flow > 0 ? ctx.links.node1[link] : ctx.links.node2[link];
    const int dest = flow > 0 ? ctx.links.node2[link] : ctx.links.node1[link];
    const double source_offset = portOffset(ctx,link,source);
    const double dest_offset = portOffset(ctx,link,dest);
    double volume = std::abs(flow) * dt;
    int sr = ctx.node_subtypes.storage_row(source);
    if (active(ctx, source) && source_offset < ctx.nodes.depth[source]) {
        auto& state = ctx.node_subtypes.storages.lid_state[sr];
        const double available = std::max(0.0, ctx.nodes.old_volume[source] +
            (ctx.nodes.lat_flow[source] - ctx.nodes.losses[source]) * dt + state.mobile_delta);
        volume = std::min(volume, available);
        state.mobile_delta -= volume;
    }

    if (sr >= 0 && source_offset >= ctx.nodes.depth[source]) {
        auto& s = ctx.node_subtypes.storages.lid_state[sr];
        int i = portCell(s, source_offset);
        if (i >= 0) {
            const auto& c = s.cells[i];
            const double available = std::max(0.0, (c.theta - c.wilting_point) * c.geometric_volume + s.port_delta[i]);
            const double throttle = c.kind == LidNodeLayerKind::Media ? std::clamp(c.theta / c.porosity, 0.0, 1.0) : 1.0;
            volume = std::min(volume * throttle, available);
            s.port_delta[i] -= volume;
            s.quality_ports.push_back({link, i, -volume});
            // The routing equation handles only mobile water. Credit the same
            // flux internally so this link draws retained water exactly once.
            ctx.nodes.inflow[source] += volume / dt;
        }
    }
    sr = ctx.node_subtypes.storage_row(dest);
    double mobile_received = volume;
    if (sr >= 0 && dest_offset >= ctx.nodes.depth[dest]) {
        auto& s = ctx.node_subtypes.storages.lid_state[sr];
        int i = portCell(s, dest_offset);
        if (i >= 0) {
            const auto& c = s.cells[i];
            const double capacity = std::max(0.0, (c.porosity - c.theta) * c.geometric_volume - s.port_delta[i]);
            const double held = std::min(volume, capacity);
            s.port_delta[i] += held;
            s.quality_ports.push_back({link, i, held});
            mobile_received -= held;
            ctx.nodes.outflow[dest] += held / dt;
        }
    }
    if (active(ctx, dest)) ctx.node_subtypes.storages.lid_state[sr].mobile_delta += mobile_received;
    return std::copysign(volume / dt, flow);
}
}
