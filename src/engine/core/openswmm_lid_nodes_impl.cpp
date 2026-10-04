// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
#include "openswmm_api_common.hpp"
#include "../../../include/openswmm/engine/openswmm_infrastructure.h"
#include "../hydrology/LidNode.hpp"
#include "UnitConversion.hpp"
#include <algorithm>
#include <cmath>

namespace {
bool editable(const openswmm::SimulationContext& c) {
    return c.state == openswmm::EngineState::BUILDING || c.state == openswmm::EngineState::OPENED;
}
}
extern "C" {
SWMM_ENGINE_API int swmm_lid_node_layer_count(SWMM_Engine engine, int control) {
    if (!engine) return -1;
    const auto& c = to_engine(engine)->context();
    if (control < 0 || control >= c.lid_controls.count()) return -1;
    return static_cast<int>(openswmm::lidnode::layers(c, control).size());
}
SWMM_ENGINE_API int swmm_lid_node_layer_get(SWMM_Engine engine, int control, int row, SWMM_LidNodeLayer* out) {
    CHECK_HANDLE(engine);
    if (!out) return SWMM_ERR_BADPARAM;
    const auto& c = to_engine(engine)->context();
    CHECK_INDEX(control >= 0 && control < c.lid_controls.count());
    const auto layers = openswmm::lidnode::layers(c, control);
    CHECK_INDEX(row >= 0 && row < static_cast<int>(layers.size()));
    out->kind = static_cast<int>(layers[row].kind);
    std::copy(layers[row].params.begin(), layers[row].params.end(), out->params);
    return SWMM_OK;
}
SWMM_ENGINE_API int swmm_lid_node_layers_set(SWMM_Engine engine, int control, const SWMM_LidNodeLayer* rows, int count) {
    CHECK_HANDLE(engine);
    auto& c = to_engine(engine)->context();
    if (!editable(c)) return SWMM_ERR_LIFECYCLE;
    CHECK_INDEX(control >= 0 && control < c.lid_controls.count());
    if (!rows || count <= 0 || c.lid_controls.lid_type[control] != "NODE") return SWMM_ERR_BADPARAM;
    std::vector<openswmm::LidNodeLayer> layers;
    for (int i = 0; i < count; ++i) {
        if (rows[i].kind < 0 || rows[i].kind > 3) return SWMM_ERR_BADPARAM;
        openswmm::LidNodeLayer l;
        l.kind = static_cast<openswmm::LidNodeLayerKind>(rows[i].kind);
        std::copy(rows[i].params, rows[i].params + 7, l.params.begin());
        layers.push_back(l);
    }
    if (!openswmm::lidnode::validateStack(layers).empty()) return SWMM_ERR_BADPARAM;
    const int numbered = count - (layers.back().kind == openswmm::LidNodeLayerKind::Bottom ? 1 : 0);
    // An edit cannot silently orphan an existing outlet anchor.
    for (const auto& a : c.lid_node_outlets) {
        if (a.layer <= numbered) continue;
        for (int n : {c.links.node1[a.link], c.links.node2[a.link]}) {
            int r = c.node_subtypes.storage_row(n);
            if (r >= 0 && c.node_subtypes.storages.lid[r].control == control) return SWMM_ERR_BADPARAM;
        }
    }
    c.lid_controls.node_layers.resize(c.lid_controls.count());
    const auto previous = openswmm::lidnode::layers(c, control);
    for (std::size_t i=0;i<previous.size();++i) if (!previous[i].treatment.empty()) {
        if (i >= layers.size() || layers[i].kind != previous[i].kind) return SWMM_ERR_BADPARAM;
        layers[i].treatment = previous[i].treatment;
    }
    c.lid_controls.node_layers[control] = std::move(layers);
    openswmm::lidnode::sync(c, control);
    return SWMM_OK;
}
SWMM_ENGINE_API int swmm_lid_node_treatment_count(SWMM_Engine engine, int control) {
    if (!engine) return -1;
    const auto& c=to_engine(engine)->context();
    if(control<0||control>=c.lid_controls.count())return -1;
    int count=0; for(const auto& l:openswmm::lidnode::layers(c,control))count+=l.treatment.size();
    return count;
}
SWMM_ENGINE_API int swmm_lid_node_treatment_get(SWMM_Engine engine,int control,int row,SWMM_LidLayerTreatment* out) {
    CHECK_HANDLE(engine); if(!out||row<0)return SWMM_ERR_BADPARAM;
    const auto& c=to_engine(engine)->context();
    CHECK_INDEX(control>=0&&control<static_cast<int>(c.lid_controls.node_layers.size()));
    int i=0; for(const auto& l:c.lid_controls.node_layers[control]) { ++i; for(const auto& t:l.treatment) {
        if(row--==0) { *out={i,c.pollutant_names.find(t.pollutant),t.removal*100,t.decay,t.expression.c_str()};return SWMM_OK; }
    }}
    return SWMM_ERR_BADPARAM;
}
SWMM_ENGINE_API int swmm_lid_node_configure(SWMM_Engine engine,int control,const SWMM_LidNodeLayer* rows,int count,const SWMM_LidLayerTreatment* treatments,int nt) {
    CHECK_HANDLE(engine);auto& c=to_engine(engine)->context();
    if(!editable(c))return SWMM_ERR_LIFECYCLE;
    CHECK_INDEX(control>=0&&control<c.lid_controls.count());
    if(!rows||count<1||nt<0||(nt>0&&!treatments))return SWMM_ERR_BADPARAM;
    std::vector<std::vector<openswmm::LidLayerTreatment>> rules(count);
    for(int i=0;i<nt;++i) {
        const auto& t=treatments[i];
        if(t.layer<1||t.layer>count||rows[t.layer-1].kind==3||t.pollutant<0||t.pollutant>=c.n_pollutants())return SWMM_ERR_BADPARAM;
        openswmm::LidLayerTreatment rule{c.pollutant_names.name_of(t.pollutant),t.removal_percent/100,t.decay_per_day,t.expression?t.expression:""};
        std::string error;if(!openswmm::lidnode::validTreatment(c,rule,error))return SWMM_ERR_BADPARAM;
        auto& layer=rules[t.layer-1];
        if(std::any_of(layer.begin(),layer.end(),[&](const auto& other){return other.pollutant==rule.pollutant;}))return SWMM_ERR_BADPARAM;
        layer.push_back(std::move(rule));
    }
    c.lid_controls.node_layers.resize(c.lid_controls.count());
    const auto previous=c.lid_controls.node_layers[control];
    for(auto& l:c.lid_controls.node_layers[control])l.treatment.clear();
    const int rc=swmm_lid_node_layers_set(engine,control,rows,count);
    if(rc!=SWMM_OK) { c.lid_controls.node_layers[control]=previous;return rc; }
    for(int i=0;i<count;++i)c.lid_controls.node_layers[control][i].treatment=std::move(rules[i]);
    return SWMM_OK;
}
SWMM_ENGINE_API int swmm_node_get_lid(SWMM_Engine engine, int node, int* control, double* saturation) {
    CHECK_HANDLE(engine);
    if (!control || !saturation) return SWMM_ERR_BADPARAM;
    const auto& c = to_engine(engine)->context();
    CHECK_INDEX(node >= 0 && node < c.n_nodes());
    const int r = c.node_subtypes.storage_row(node);
    *control = r >= 0 ? c.node_subtypes.storages.lid[r].control : -1;
    *saturation = r >= 0 ? c.node_subtypes.storages.lid[r].initial_saturation : 0.0;
    return SWMM_OK;
}
SWMM_ENGINE_API int swmm_node_set_lid(SWMM_Engine engine, int node, int control, double saturation) {
    CHECK_HANDLE(engine);
    auto& c = to_engine(engine)->context();
    if (!editable(c)) return SWMM_ERR_LIFECYCLE;
    CHECK_INDEX(node >= 0 && node < c.n_nodes());
    const int r = c.node_subtypes.storage_row(node);
    if (r < 0 || control < -1 || control >= c.lid_controls.count() || !std::isfinite(saturation) || saturation < 0.0 || saturation > 100.0) return SWMM_ERR_BADPARAM;
    if (control >= 0 && !openswmm::lidnode::validateStack(openswmm::lidnode::layers(c, control)).empty()) return SWMM_ERR_BADPARAM;
    if (control >= 0) {
        int numbered = 0;
        for (const auto& l : openswmm::lidnode::layers(c, control)) numbered += l.kind != openswmm::LidNodeLayerKind::Bottom;
        for (const auto& a : c.lid_node_outlets) {
            if (c.links.node1[a.link] != node && c.links.node2[a.link] != node) continue;
            const int other = c.links.node1[a.link] == node ? c.links.node2[a.link] : c.links.node1[a.link];
            const int other_row = c.node_subtypes.storage_row(other);
            if (a.layer > numbered || (other_row >= 0 && c.node_subtypes.storages.lid[other_row].control >= 0)) return SWMM_ERR_BADPARAM;
        }
    }
    c.node_subtypes.storages.lid_state[r] = {};
    c.node_subtypes.storages.lid[r] = {control, control < 0 ? 0.0 : saturation};
    if (control >= 0) openswmm::lidnode::sync(c, control);
    else {
        auto& a = c.lid_node_outlets;
        a.erase(std::remove_if(a.begin(), a.end(), [&](const auto& v) { return c.links.node1[v.link] == node || c.links.node2[v.link] == node; }), a.end());
    }
    c.nodes.full_volume[node] = 0.0;
    return SWMM_OK;
}
SWMM_ENGINE_API int swmm_lid_node_outlet_get(SWMM_Engine engine, int link, int* layer, int* top) {
    CHECK_HANDLE(engine);
    if (!layer || !top) return SWMM_ERR_BADPARAM;
    const auto& c = to_engine(engine)->context();
    CHECK_INDEX(link >= 0 && link < c.n_links());
    *layer = 0; *top = 0;
    for (const auto& a : c.lid_node_outlets) if (a.link == link) { *layer = a.layer; *top = a.top; break; }
    return SWMM_OK;
}
SWMM_ENGINE_API int swmm_lid_node_outlet_set(SWMM_Engine engine, int link, int layer, int top) {
    CHECK_HANDLE(engine);
    auto& c = to_engine(engine)->context();
    if (!editable(c)) return SWMM_ERR_LIFECYCLE;
    CHECK_INDEX(link >= 0 && link < c.n_links());
    if (layer < 0 || (top != 0 && top != 1)) return SWMM_ERR_BADPARAM;
    int control = -1, ends = 0;
    if (layer > 0) {
        for (int n : {c.links.node1[link], c.links.node2[link]}) {
            int r = c.node_subtypes.storage_row(n);
            if (r >= 0 && c.node_subtypes.storages.lid[r].control >= 0) { control = c.node_subtypes.storages.lid[r].control; ++ends; }
        }
        if (ends != 1) return SWMM_ERR_BADPARAM;
        auto stack = openswmm::lidnode::layers(c, control);
        int numbered = 0;
        for (const auto& l : stack) numbered += l.kind != openswmm::LidNodeLayerKind::Bottom;
        if (layer > numbered) return SWMM_ERR_BADPARAM;
    }
    auto& a = c.lid_node_outlets;
    a.erase(std::remove_if(a.begin(), a.end(), [link](const auto& v) { return v.link == link; }), a.end());
    if (layer > 0) { a.push_back({link, layer, top != 0}); openswmm::lidnode::sync(c, control); }
    return SWMM_OK;
}
} // extern C

extern "C" {
SWMM_ENGINE_API int swmm_lid_node_state_count(SWMM_Engine engine, int node) {
    if (!engine) return -1;
    const auto& c = to_engine(engine)->context();
    if (node < 0 || node >= c.n_nodes()) return -1;
    const int r = c.node_subtypes.storage_row(node);
    return r < 0 ? 0 : static_cast<int>(c.node_subtypes.storages.lid_state[r].cells.size());
}
SWMM_ENGINE_API int swmm_lid_node_state_get(SWMM_Engine engine, int node, int row, int* layer, double* bottom, double* top, double* moisture) {
    CHECK_HANDLE(engine);
    if (!layer || !bottom || !top || !moisture) return SWMM_ERR_BADPARAM;
    const auto& c = to_engine(engine)->context();
    CHECK_INDEX(node >= 0 && node < c.n_nodes());
    const int r = c.node_subtypes.storage_row(node);
    CHECK_INDEX(r >= 0 && row >= 0 && row < static_cast<int>(c.node_subtypes.storages.lid_state[r].cells.size()));
    const auto& cell = c.node_subtypes.storages.lid_state[r].cells[row];
    const double u = openswmm::ucf::Ucf[openswmm::ucf::LENGTH][openswmm::ucf::getUnitSystem(static_cast<int>(c.options.flow_units))];
    *layer = cell.layer; *bottom = cell.bottom * u; *top = cell.top * u;
    *moisture = cell.theta + (cell.porosity - cell.theta) * std::clamp((c.nodes.depth[node] - cell.bottom) / (cell.top - cell.bottom), 0.0, 1.0);
    return SWMM_OK;
}
}
