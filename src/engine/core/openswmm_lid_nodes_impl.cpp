// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin

/**
 * @file      openswmm_lid_nodes_impl.cpp
 * @brief     Public C API implementation for LID node configuration.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include "openswmm_api_common.hpp"
#include "../../../include/openswmm/engine/openswmm_infrastructure.h"
#include "../hydrology/LidNode.hpp"
#include "../hydraulics/Node.hpp"
#include "UnitConversion.hpp"
#include <algorithm>
#include <cmath>

namespace {
/**
 * @brief Check the two authoring lifecycle states accepted by NODE LID edits.
 *
 * @param[in] c Borrowed context lifecycle state.
 * @return True only for BUILDING or OPENED.
 */
bool editable(const openswmm::SimulationContext& c) {
    return c.state == openswmm::EngineState::BUILDING || c.state == openswmm::EngineState::OPENED;
}
}
extern "C" {
SWMM_ENGINE_API int swmm_lid_node_layer_count(SWMM_Engine engine, int control) {
    if (!engine)
        return -1;
    const auto& c = to_engine(engine)->context();
    if (control < 0 || control >= c.lid_controls.count())
        return -1;
    return static_cast<int>(openswmm::lidnode::layers(c, control).size());
}
SWMM_ENGINE_API int swmm_lid_node_layer_get(SWMM_Engine engine, int control, int row,
                                            SWMM_LidNodeLayer* out) {
    CHECK_HANDLE(engine);
    if (!out)
        return SWMM_ERR_BADPARAM;
    const auto& c = to_engine(engine)->context();
    CHECK_INDEX(control >= 0 && control < c.lid_controls.count());
    const auto layers = openswmm::lidnode::layers(c, control);
    CHECK_INDEX(row >= 0 && row < static_cast<int>(layers.size()));
    out->kind = static_cast<int>(layers[row].kind);
    std::copy(layers[row].params.begin(), layers[row].params.end(), out->params);
    return SWMM_OK;
}
SWMM_ENGINE_API int swmm_lid_node_layers_set(SWMM_Engine engine, int control,
                                             const SWMM_LidNodeLayer* rows, int count) {
    CHECK_HANDLE(engine);
    auto& c = to_engine(engine)->context();
    if (!editable(c))
        return SWMM_ERR_LIFECYCLE;
    CHECK_INDEX(control >= 0 && control < c.lid_controls.count());
    if (!rows || count <= 0 || c.lid_controls.lid_type[control] != "NODE")
        return SWMM_ERR_BADPARAM;
    std::vector<openswmm::LidNodeLayer> layers;
    for (int i = 0; i < count; ++i) {
        if (rows[i].kind < 0 || rows[i].kind > 3)
            return SWMM_ERR_BADPARAM;
        openswmm::LidNodeLayer l;
        l.kind = static_cast<openswmm::LidNodeLayerKind>(rows[i].kind);
        std::copy(rows[i].params, rows[i].params + 7, l.params.begin());
        layers.push_back(l);
    }
    if (!openswmm::lidnode::validateStack(layers).empty())
        return SWMM_ERR_BADPARAM;
    const int numbered = count - (layers.back().kind == openswmm::LidNodeLayerKind::Bottom ? 1 : 0);
    // An edit cannot silently orphan an existing outlet anchor.
    for (const auto& a : c.lid_node_outlets) {
        if (a.layer <= numbered)
            continue;
        for (int n : {c.links.node1[a.link], c.links.node2[a.link]}) {
            int r = c.node_subtypes.storage_row(n);
            if (r >= 0 && c.node_subtypes.storages.lid[r].control == control)
                return SWMM_ERR_BADPARAM;
        }
    }
    c.lid_controls.node_layers.resize(c.lid_controls.count());
    const auto previous = openswmm::lidnode::layers(c, control);
    if (!previous.empty() && (previous.front().flow.enabled ||
                              std::any_of(previous.begin(), previous.end(),
                                          [](const auto& l) { return l.retention.alpha > 0; }))) {
        if (layers.size() != previous.size())
            return SWMM_ERR_BADPARAM;
        for (std::size_t i = 0; i < layers.size(); ++i) {
            if (layers[i].kind != previous[i].kind)
                return SWMM_ERR_BADPARAM;
            layers[i].retention = previous[i].retention;
        }
        layers.front().flow = previous.front().flow;
        if (!openswmm::lidnode::validateStack(layers).empty())
            return SWMM_ERR_BADPARAM;
    }
    for (std::size_t i = 0; i < previous.size(); ++i)
        if (!previous[i].treatment.empty()) {
            if (i >= layers.size() || layers[i].kind != previous[i].kind)
                return SWMM_ERR_BADPARAM;
            layers[i].treatment = previous[i].treatment;
        }
    c.lid_controls.node_layers[control] = std::move(layers);
    openswmm::lidnode::sync(c, control);
    return SWMM_OK;
}
SWMM_ENGINE_API int swmm_lid_node_treatment_count(SWMM_Engine engine, int control) {
    if (!engine)
        return -1;
    const auto& c = to_engine(engine)->context();
    if (control < 0 || control >= c.lid_controls.count())
        return -1;
    int count = 0;
    for (const auto& l : openswmm::lidnode::layers(c, control))
        count += l.treatment.size();
    return count;
}
SWMM_ENGINE_API int swmm_lid_node_treatment_get(SWMM_Engine engine, int control, int row,
                                                SWMM_LidLayerTreatment* out) {
    CHECK_HANDLE(engine);
    if (!out || row < 0)
        return SWMM_ERR_BADPARAM;
    const auto& c = to_engine(engine)->context();
    CHECK_INDEX(control >= 0 && control < static_cast<int>(c.lid_controls.node_layers.size()));
    int i = 0;
    for (const auto& l : c.lid_controls.node_layers[control]) {
        ++i;
        for (const auto& t : l.treatment) {
            if (row-- == 0) {
                *out = {i, c.pollutant_names.find(t.pollutant), t.removal * 100, t.decay,
                        t.expression.c_str()};
                return SWMM_OK;
            }
        }
    }
    return SWMM_ERR_BADPARAM;
}
SWMM_ENGINE_API int swmm_lid_node_configure(SWMM_Engine engine, int control,
                                            const SWMM_LidNodeLayer* rows, int count,
                                            const SWMM_LidLayerTreatment* treatments, int nt) {
    CHECK_HANDLE(engine);
    auto& c = to_engine(engine)->context();
    if (!editable(c))
        return SWMM_ERR_LIFECYCLE;
    CHECK_INDEX(control >= 0 && control < c.lid_controls.count());
    if (!rows || count < 1 || nt < 0 || (nt > 0 && !treatments))
        return SWMM_ERR_BADPARAM;
    std::vector<std::vector<openswmm::LidLayerTreatment>> rules(count);
    for (int i = 0; i < nt; ++i) {
        const auto& t = treatments[i];
        if (t.layer < 1 || t.layer > count || rows[t.layer - 1].kind == 3 || t.pollutant < 0 ||
            t.pollutant >= c.n_pollutants())
            return SWMM_ERR_BADPARAM;
        openswmm::LidLayerTreatment rule{c.pollutant_names.name_of(t.pollutant),
                                         t.removal_percent / 100, t.decay_per_day,
                                         t.expression ? t.expression : ""};
        std::string error;
        if (!openswmm::lidnode::validTreatment(c, rule, error))
            return SWMM_ERR_BADPARAM;
        auto& layer = rules[t.layer - 1];
        if (std::any_of(layer.begin(), layer.end(),
                        [&](const auto& other) { return other.pollutant == rule.pollutant; }))
            return SWMM_ERR_BADPARAM;
        layer.push_back(std::move(rule));
    }
    c.lid_controls.node_layers.resize(c.lid_controls.count());
    const auto previous = c.lid_controls.node_layers[control];
    for (auto& l : c.lid_controls.node_layers[control])
        l.treatment.clear();
    const int rc = swmm_lid_node_layers_set(engine, control, rows, count);
    if (rc != SWMM_OK) {
        c.lid_controls.node_layers[control] = previous;
        return rc;
    }
    for (int i = 0; i < count; ++i)
        c.lid_controls.node_layers[control][i].treatment = std::move(rules[i]);
    return SWMM_OK;
}
SWMM_ENGINE_API int swmm_node_get_lid(SWMM_Engine engine, int node, int* control,
                                      double* saturation) {
    CHECK_HANDLE(engine);
    if (!control || !saturation)
        return SWMM_ERR_BADPARAM;
    const auto& c = to_engine(engine)->context();
    CHECK_INDEX(node >= 0 && node < c.n_nodes());
    const int r = c.node_subtypes.storage_row(node);
    *control = r >= 0 ? c.node_subtypes.storages.lid[r].control : -1;
    *saturation = r >= 0 ? c.node_subtypes.storages.lid[r].initial_saturation : 0.0;
    return SWMM_OK;
}
SWMM_ENGINE_API int swmm_lid_richards_options_get(SWMM_Engine engine, int control,
                                                  SWMM_LidRichardsOptions* out) {
    CHECK_HANDLE(engine);
    if (!out)
        return SWMM_ERR_BADPARAM;
    const auto& c = to_engine(engine)->context();
    CHECK_INDEX(control >= 0 && control < c.lid_controls.count());
    const auto stack = openswmm::lidnode::layers(c, control);
    const auto o = stack.empty() ? openswmm::richards::Options{} : stack.front().flow;
    *out = {o.enabled ? 1 : 0, o.cells_per_layer, o.atol, o.rtol, o.max_step};
    return SWMM_OK;
}
SWMM_ENGINE_API int swmm_lid_richards_material_get(SWMM_Engine engine, int control, int row,
                                                   SWMM_LidRichardsMaterial* out) {
    CHECK_HANDLE(engine);
    if (!out)
        return SWMM_ERR_BADPARAM;
    const auto& c = to_engine(engine)->context();
    CHECK_INDEX(control >= 0 && control < c.lid_controls.count());
    const auto stack = openswmm::lidnode::layers(c, control);
    CHECK_INDEX(row >= 0 && row < static_cast<int>(stack.size()));
    const auto& p = stack[row].retention;
    *out = {p.theta_r, p.alpha, p.n, p.l, p.specific_storage};
    return SWMM_OK;
}
SWMM_ENGINE_API int swmm_lid_node_configure_flow(SWMM_Engine engine, int control,
                                                 const SWMM_LidNodeLayer* rows, int count,
                                                 const SWMM_LidLayerTreatment* treatment, int nt,
                                                 const SWMM_LidRichardsOptions* options,
                                                 const SWMM_LidRichardsMaterial* material) {
    CHECK_HANDLE(engine);
    auto& c = to_engine(engine)->context();
    if (!editable(c))
        return SWMM_ERR_LIFECYCLE;
    CHECK_INDEX(control >= 0 && control < c.lid_controls.count());
    if (!rows || count < 1 || !options || options->model < 0 || options->model > 1 ||
        (options->model == 1 && !material) || nt < 0 || (nt && !treatment) ||
        c.lid_controls.lid_type[control] != "NODE")
        return SWMM_ERR_BADPARAM;
    std::vector<openswmm::LidNodeLayer> stack(count);
    for (int i = 0; i < count; ++i) {
        if (rows[i].kind < 0 || rows[i].kind > 3)
            return SWMM_ERR_BADPARAM;
        stack[i].kind = static_cast<openswmm::LidNodeLayerKind>(rows[i].kind);
        std::copy(rows[i].params, rows[i].params + 7, stack[i].params.begin());
        if (material)
            stack[i].retention = {material[i].theta_r, material[i].alpha, material[i].n,
                                  material[i].l, material[i].specific_storage};
    }
    stack.front().flow = {options->model == 1, options->cells_per_layer, options->atol,
                          options->rtol, options->max_step};
    if (!openswmm::lidnode::validateStack(stack).empty())
        return SWMM_ERR_BADPARAM;
    for (int i = 0; i < nt; ++i) {
        const auto& t = treatment[i];
        if (t.layer < 1 || t.layer > count || rows[t.layer - 1].kind == 3 || t.pollutant < 0 ||
            t.pollutant >= c.n_pollutants())
            return SWMM_ERR_BADPARAM;
        openswmm::LidLayerTreatment rule{c.pollutant_names.name_of(t.pollutant),
                                         t.removal_percent / 100, t.decay_per_day,
                                         t.expression ? t.expression : ""};
        std::string error;
        if (!openswmm::lidnode::validTreatment(c, rule, error))
            return SWMM_ERR_BADPARAM;
        auto& rules = stack[t.layer - 1].treatment;
        if (std::any_of(rules.begin(), rules.end(),
                        [&](const auto& r) { return r.pollutant == rule.pollutant; }))
            return SWMM_ERR_BADPARAM;
        rules.push_back(std::move(rule));
    }
    const int numbered = count - (rows[count - 1].kind == 3 ? 1 : 0);
    for (const auto& a : c.lid_node_outlets)
        if (a.layer > numbered)
            for (int node : {c.links.node1[a.link], c.links.node2[a.link]}) {
                int r = c.node_subtypes.storage_row(node);
                if (r >= 0 && c.node_subtypes.storages.lid[r].control == control)
                    return SWMM_ERR_BADPARAM;
            }
    c.lid_controls.node_layers.resize(c.lid_controls.count());
    c.lid_controls.node_layers[control] = std::move(stack);
    openswmm::lidnode::sync(c, control);
    return SWMM_OK;
}
SWMM_ENGINE_API int swmm_storage_lid_get(SWMM_Engine engine, int node, SWMM_StorageLidState* out) {
    CHECK_HANDLE(engine);
    if (!out)
        return SWMM_ERR_BADPARAM;
    const auto& c = to_engine(engine)->context();
    CHECK_INDEX(node >= 0 && node < c.n_nodes());
    const int row = c.node_subtypes.storage_row(node);
    if (row < 0)
        return SWMM_ERR_BADPARAM;
    const auto& cfg = c.node_subtypes.storages.lid[row];
    *out = {cfg.control, cfg.initial_saturation, static_cast<int>(cfg.bottom),
            c.nodes.full_depth[node]};
    return SWMM_OK;
}
SWMM_ENGINE_API int swmm_storage_lid_anchor_get(SWMM_Engine engine, int link,
                                                SWMM_StorageLidAnchor* out) {
    CHECK_HANDLE(engine);
    if (!out)
        return SWMM_ERR_BADPARAM;
    const auto& c = to_engine(engine)->context();
    CHECK_INDEX(link >= 0 && link < c.n_links());
    int layer = 0, top = 0;
    swmm_lid_node_outlet_get(engine, link, &layer, &top);
    const int wr = c.link_subtypes.weir_row(link), orow = c.link_subtypes.outlet_row(link);
    const double crest = wr >= 0     ? c.link_subtypes.weirs.crest_height[wr]
                         : orow >= 0 ? c.link_subtypes.outlets.crest_height[orow]
                                     : 0;
    *out = {link, layer, top, c.links.offset1[link], c.links.offset2[link], crest};
    return SWMM_OK;
}
SWMM_ENGINE_API int swmm_storage_lid_validate(SWMM_Engine engine, int node,
                                              const SWMM_StorageLidState* state,
                                              const SWMM_StorageLidAnchor* anchors, int count) {
    CHECK_HANDLE(engine);
    const auto& c = to_engine(engine)->context();
    if (!editable(c))
        return SWMM_ERR_LIFECYCLE;
    CHECK_INDEX(node >= 0 && node < c.n_nodes());
    if (!state || count < 0 || (count && !anchors) || c.node_subtypes.storage_row(node) < 0 ||
        state->control < -1 || state->control >= c.lid_controls.count() ||
        !std::isfinite(state->saturation) || state->saturation < 0 || state->saturation > 100 ||
        !std::isfinite(state->max_depth_ft) || state->max_depth_ft < 0 || state->bottom < 0 ||
        state->bottom > 2)
        return SWMM_ERR_BADPARAM;
    int numbered = 0;
    if (state->control >= 0) {
        const auto stack = openswmm::lidnode::layers(c, state->control);
        if (!openswmm::lidnode::validateStack(stack).empty())
            return SWMM_ERR_BADPARAM;
        for (const auto& layer : stack)
            numbered += layer.kind != openswmm::LidNodeLayerKind::Bottom;
    } else if (count || state->saturation != 0 || state->bottom != 0)
        return SWMM_ERR_BADPARAM;
    if (state->control >= 0 &&
        c.node_subtypes.storages.lid[c.node_subtypes.storage_row(node)].control < 0)
        for (const auto& a : c.lid_node_outlets)
            if (c.links.node1[a.link] == node || c.links.node2[a.link] == node)
                return SWMM_ERR_BADPARAM; // Do not claim an existing other owner's anchor.
    std::vector<int> links;
    for (int i = 0; i < count; ++i) {
        const auto& a = anchors[i];
        if (a.link < 0 || a.link >= c.n_links() || a.layer < 1 || a.layer > numbered ||
            (a.top != 0 && a.top != 1) || !std::isfinite(a.offset_up_ft) ||
            !std::isfinite(a.offset_down_ft) || !std::isfinite(a.crest_ft) ||
            std::find(links.begin(), links.end(), a.link) != links.end())
            return SWMM_ERR_BADPARAM;
        links.push_back(a.link);
        const int n1 = c.links.node1[a.link], n2 = c.links.node2[a.link];
        if (n1 != node && n2 != node)
            return SWMM_ERR_BADPARAM;
        const int other = n1 == node ? n2 : n1;
        const int other_row = c.node_subtypes.storage_row(other);
        if (other_row >= 0 && c.node_subtypes.storages.lid[other_row].control >= 0)
            return SWMM_ERR_BADPARAM;
    }
    return SWMM_OK;
}
SWMM_ENGINE_API int swmm_storage_lid_prepare(SWMM_Engine engine, int node, int control,
                                             double saturation, int bottom,
                                             SWMM_StorageLidState* out,
                                             SWMM_StorageLidAnchor* anchors, int capacity,
                                             int* count) {
    CHECK_HANDLE(engine);
    if (!out || !count || capacity < 0 || (capacity && !anchors))
        return SWMM_ERR_BADPARAM;
    const auto& c = to_engine(engine)->context();
    if (!editable(c))
        return SWMM_ERR_LIFECYCLE;
    SWMM_StorageLidState before{};
    const int status = swmm_storage_lid_get(engine, node, &before);
    if (status != SWMM_OK)
        return status;
    if (control < -1 || control >= c.lid_controls.count() || !std::isfinite(saturation) ||
        saturation < 0 || saturation > 100 || bottom < 0 || bottom > 2)
        return SWMM_ERR_BADPARAM;
    SWMM_StorageLidState next{control, control < 0 ? 0 : saturation, control < 0 ? 0 : bottom,
                              before.max_depth_ft};
    std::vector<SWMM_StorageLidAnchor> proposed;
    const bool changed = control != before.control;
    const auto stack = control >= 0 ? openswmm::lidnode::layers(c, control)
                                    : std::vector<openswmm::LidNodeLayer>{};
    const double factor =
        openswmm::ucf::Ucf[openswmm::ucf::RAINDEPTH]
                          [openswmm::ucf::getUnitSystem(static_cast<int>(c.options.flow_units))];
    if (control >= 0) {
        if (!openswmm::lidnode::validateStack(stack).empty())
            return SWMM_ERR_BADPARAM;
        if (changed)
            next.max_depth_ft = openswmm::lidnode::thickness(stack) / factor;
        for (const auto& a : c.lid_node_outlets) {
            if (c.links.node1[a.link] != node && c.links.node2[a.link] != node)
                continue;
            SWMM_StorageLidAnchor row{};
            swmm_storage_lid_anchor_get(engine, a.link, &row);
            if (changed) {
                double down = 0;
                int numbered = 0;
                bool found = false;
                for (const auto& l : stack) {
                    if (l.kind == openswmm::LidNodeLayerKind::Bottom)
                        continue;
                    if (++numbered == a.layer) {
                        const double h = (openswmm::lidnode::thickness(stack) - down -
                                          (a.top ? 0 : l.params[0])) /
                                         factor;
                        const int wr = c.link_subtypes.weir_row(a.link),
                                  orow = c.link_subtypes.outlet_row(a.link);
                        if (wr >= 0 || orow >= 0)
                            row.crest_ft = c.nodes.invert_elev[node] + h -
                                           c.nodes.invert_elev[c.links.node1[a.link]];
                        else if (c.links.node1[a.link] == node)
                            row.offset_up_ft = h;
                        else
                            row.offset_down_ft = h;
                        found = true;
                        break;
                    }
                    down += l.params[0];
                }
                if (!found)
                    return SWMM_ERR_BADPARAM;
            }
            proposed.push_back(row);
        }
    }
    const int valid =
        swmm_storage_lid_validate(engine, node, &next, proposed.data(), proposed.size());
    if (valid != SWMM_OK)
        return valid;
    if (capacity < static_cast<int>(proposed.size()))
        return SWMM_ERR_BADPARAM;
    if (!proposed.empty())
        std::copy(proposed.begin(), proposed.end(), anchors);
    *out = next;
    *count = proposed.size();
    return SWMM_OK;
}
SWMM_ENGINE_API int swmm_storage_lid_replace(SWMM_Engine engine, int node,
                                             const SWMM_StorageLidState* state,
                                             const SWMM_StorageLidAnchor* anchors, int count) {
    const int status = swmm_storage_lid_validate(engine, node, state, anchors, count);
    if (status != SWMM_OK)
        return status;
    auto& c = to_engine(engine)->context();
    auto next = c.lid_node_outlets;
    const int previous_row = c.node_subtypes.storage_row(node);
    const bool previously_assigned = c.node_subtypes.storages.lid[previous_row].control >= 0;
    next.erase(std::remove_if(next.begin(), next.end(),
                              [&](const auto& a) {
                                  return previously_assigned && (c.links.node1[a.link] == node ||
                                                                 c.links.node2[a.link] == node);
                              }),
               next.end());
    for (int i = 0; i < count; ++i)
        next.push_back({anchors[i].link, anchors[i].layer, anchors[i].top != 0});
    const int row = c.node_subtypes.storage_row(node);
    c.node_subtypes.storages.lid[row] = {state->control, state->saturation,
                                         static_cast<openswmm::LidNodeBottom>(state->bottom)};
    c.node_subtypes.storages.lid_state[row] = {};
    c.nodes.full_depth[node] = state->max_depth_ft;
    c.nodes.full_volume[node] = 0;
    for (int i = 0; i < count; ++i) {
        const auto& a = anchors[i];
        c.links.offset1[a.link] = a.offset_up_ft;
        c.links.offset2[a.link] = a.offset_down_ft;
        const int wr = c.link_subtypes.weir_row(a.link), orow = c.link_subtypes.outlet_row(a.link);
        if (wr >= 0)
            c.link_subtypes.weirs.crest_height[wr] = a.crest_ft;
        else if (orow >= 0)
            c.link_subtypes.outlets.crest_height[orow] = a.crest_ft;
    }
    c.lid_node_outlets.swap(next);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_set_lid(SWMM_Engine engine, int node, int control,
                                      double saturation) {
    CHECK_HANDLE(engine);
    const auto& c = to_engine(engine)->context();
    if (!editable(c))
        return SWMM_ERR_LIFECYCLE;
    SWMM_StorageLidState before{}, after{};
    const int status = swmm_storage_lid_get(engine, node, &before);
    if (status != SWMM_OK)
        return status;
    std::vector<SWMM_StorageLidAnchor> anchors(c.lid_node_outlets.size());
    int count = 0;
    const int prepared = swmm_storage_lid_prepare(engine, node, control, saturation, before.bottom,
                                                  &after, anchors.data(), anchors.size(), &count);
    return prepared == SWMM_OK
               ? swmm_storage_lid_replace(engine, node, &after, anchors.data(), count)
               : prepared;
}
SWMM_ENGINE_API int swmm_node_get_lid_bottom(SWMM_Engine engine, int node, int* mode) {
    CHECK_HANDLE(engine);
    const auto& c = to_engine(engine)->context();
    if (!mode)
        return SWMM_ERR_BADPARAM;
    CHECK_INDEX(node >= 0 && node < c.n_nodes());
    *mode = static_cast<int>(openswmm::lidnode::bottomMode(c, node));
    return SWMM_OK;
}
SWMM_ENGINE_API int swmm_node_set_lid_bottom(SWMM_Engine engine, int node, int mode) {
    CHECK_HANDLE(engine);
    auto& c = to_engine(engine)->context();
    if (!editable(c))
        return SWMM_ERR_LIFECYCLE;
    CHECK_INDEX(node >= 0 && node < c.n_nodes());
    const int r = c.node_subtypes.storage_row(node);
    if (r < 0 || c.node_subtypes.storages.lid[r].control < 0 || mode < 0 || mode > 2)
        return SWMM_ERR_BADPARAM;
    c.node_subtypes.storages.lid[r].bottom = static_cast<openswmm::LidNodeBottom>(mode);
    return SWMM_OK;
}
SWMM_ENGINE_API int swmm_lid_node_base_area(SWMM_Engine engine, int node, double* area) {
    CHECK_HANDLE(engine);
    if (!area)
        return SWMM_ERR_BADPARAM;
    auto& c = to_engine(engine)->context();
    if (!editable(c))
        return SWMM_ERR_LIFECYCLE;
    CHECK_INDEX(node >= 0 && node < c.n_nodes());
    const int r = c.node_subtypes.storage_row(node);
    if (r < 0 || c.node_subtypes.storages.lid[r].control < 0 ||
        !c.node_subtypes.storages.lid_state[r].cells.empty())
        return SWMM_ERR_BADPARAM;
    const int us = openswmm::ucf::getUnitSystem(static_cast<int>(c.options.flow_units));
    const double length = openswmm::ucf::Ucf[openswmm::ucf::LENGTH][us];
    const double value =
        openswmm::node::getSurfArea(c.nodes, node, 0, &c.tables, us, &c.node_subtypes) * length *
        length;
    if (!std::isfinite(value) || value < 0)
        return SWMM_ERR_BADPARAM;
    *area = value;
    return SWMM_OK;
}
SWMM_ENGINE_API int swmm_lid_node_outlet_get(SWMM_Engine engine, int link, int* layer, int* top) {
    CHECK_HANDLE(engine);
    if (!layer || !top)
        return SWMM_ERR_BADPARAM;
    const auto& c = to_engine(engine)->context();
    CHECK_INDEX(link >= 0 && link < c.n_links());
    *layer = 0;
    *top = 0;
    for (const auto& a : c.lid_node_outlets)
        if (a.link == link) {
            *layer = a.layer;
            *top = a.top;
            break;
        }
    return SWMM_OK;
}
SWMM_ENGINE_API int swmm_lid_node_outlet_set(SWMM_Engine engine, int link, int layer, int top) {
    CHECK_HANDLE(engine);
    auto& c = to_engine(engine)->context();
    if (!editable(c))
        return SWMM_ERR_LIFECYCLE;
    CHECK_INDEX(link >= 0 && link < c.n_links());
    if (layer < 0 || (top != 0 && top != 1))
        return SWMM_ERR_BADPARAM;
    int control = -1, ends = 0;
    if (layer > 0) {
        for (int n : {c.links.node1[link], c.links.node2[link]}) {
            int r = c.node_subtypes.storage_row(n);
            if (r >= 0 && c.node_subtypes.storages.lid[r].control >= 0) {
                control = c.node_subtypes.storages.lid[r].control;
                ++ends;
            }
        }
        if (ends != 1)
            return SWMM_ERR_BADPARAM;
        auto stack = openswmm::lidnode::layers(c, control);
        int numbered = 0;
        for (const auto& l : stack)
            numbered += l.kind != openswmm::LidNodeLayerKind::Bottom;
        if (layer > numbered)
            return SWMM_ERR_BADPARAM;
    }
    auto& a = c.lid_node_outlets;
    a.erase(std::remove_if(a.begin(), a.end(), [link](const auto& v) { return v.link == link; }),
            a.end());
    if (layer > 0) {
        a.push_back({link, layer, top != 0});
        openswmm::lidnode::sync(c, control);
    }
    return SWMM_OK;
}
} // extern C

extern "C" {
SWMM_ENGINE_API int swmm_lid_node_state_count(SWMM_Engine engine, int node) {
    if (!engine)
        return -1;
    const auto& c = to_engine(engine)->context();
    if (node < 0 || node >= c.n_nodes())
        return -1;
    const int r = c.node_subtypes.storage_row(node);
    return r < 0 ? 0 : static_cast<int>(c.node_subtypes.storages.lid_state[r].cells.size());
}
SWMM_ENGINE_API int swmm_lid_node_state_get(SWMM_Engine engine, int node, int row, int* layer,
                                            double* bottom, double* top, double* moisture) {
    CHECK_HANDLE(engine);
    if (!layer || !bottom || !top || !moisture)
        return SWMM_ERR_BADPARAM;
    const auto& c = to_engine(engine)->context();
    CHECK_INDEX(node >= 0 && node < c.n_nodes());
    const int r = c.node_subtypes.storage_row(node);
    CHECK_INDEX(r >= 0 && row >= 0 &&
                row < static_cast<int>(c.node_subtypes.storages.lid_state[r].cells.size()));
    const auto& cell = c.node_subtypes.storages.lid_state[r].cells[row];
    const double u =
        openswmm::ucf::Ucf[openswmm::ucf::LENGTH]
                          [openswmm::ucf::getUnitSystem(static_cast<int>(c.options.flow_units))];
    *layer = cell.layer;
    *bottom = cell.bottom * u;
    *top = cell.top * u;
    *moisture = c.node_subtypes.storages.lid_state[r].richards && row > 0
                    ? cell.theta
                    : cell.theta + (cell.porosity - cell.theta) *
                                       std::clamp((c.nodes.depth[node] - cell.bottom) /
                                                      (cell.top - cell.bottom),
                                                  0.0, 1.0);
    return SWMM_OK;
}
SWMM_ENGINE_API int swmm_lid_richards_state_get(SWMM_Engine engine, int node, int row,
                                                double* pressure, double* head, double* water) {
    CHECK_HANDLE(engine);
    if (!pressure || !head || !water)
        return SWMM_ERR_BADPARAM;
    const auto& c = to_engine(engine)->context();
    CHECK_INDEX(node >= 0 && node < c.n_nodes());
    const int r = c.node_subtypes.storage_row(node);
    CHECK_INDEX(r >= 0 && row >= 0 &&
                row < static_cast<int>(c.node_subtypes.storages.lid_state[r].cells.size()));
    const auto& s = c.node_subtypes.storages.lid_state[r];
    if (!s.richards)
        return SWMM_ERR_BADPARAM;
    const auto& cell = s.cells[row];
    const int us = openswmm::ucf::getUnitSystem(static_cast<int>(c.options.flow_units));
    const double length = openswmm::ucf::Ucf[openswmm::ucf::LENGTH][us],
                 volume = openswmm::ucf::Ucf[openswmm::ucf::VOLUME][us];
    *pressure = (row ? s.richards_pressure[row] : c.nodes.depth[node] - cell.bottom) * length;
    *head =
        (c.nodes.invert_elev[node] + (row ? .5 * (cell.bottom + cell.top) : cell.bottom)) * length +
        *pressure;
    *water = (row ? s.richards_water[row] : c.nodes.volume[node]) * volume;
    return SWMM_OK;
}
SWMM_ENGINE_API int swmm_lid_richards_statistics_get(SWMM_Engine engine, int node,
                                                     SWMM_LidRichardsStatistics* out) {
    CHECK_HANDLE(engine);
    if (!out)
        return SWMM_ERR_BADPARAM;
    const auto& c = to_engine(engine)->context();
    CHECK_INDEX(node >= 0 && node < c.n_nodes());
    if (!openswmm::lidnode::richardsMode(c, node))
        return SWMM_ERR_BADPARAM;
    const auto& r =
        c.node_subtypes.storages.lid_state[c.node_subtypes.storage_row(node)].richards_report;
    *out = {r.accepted, r.rejected, r.rhs, r.newton, r.min_step, r.balance};
    return SWMM_OK;
}
}

SWMM_ENGINE_API int swmm_lid_node_bottom_state_get(SWMM_Engine engine, int node,
                                                   SWMM_LidBottomState* out) {
    CHECK_HANDLE(engine);
    if (!out)
        return SWMM_ERR_BADPARAM;
    const auto& c = to_engine(engine)->context();
    CHECK_INDEX(node >= 0 && node < c.n_nodes());
#ifdef OPENSWMM_HAS_2D
    const int sr = c.node_subtypes.storage_row(node);
    if (sr < 0 || c.node_subtypes.storages.lid[sr].bottom != openswmm::LidNodeBottom::MeshAquifer)
        return SWMM_ERR_BADPARAM;
    const auto& gw = to_engine(engine)->surfaceRouter2D().subsurface();
    if (!gw.lidReceiptsEnabled())
        return SWMM_ERR_BADPARAM;
    for (std::size_t b = 0; b < gw.nodeBeds().size(); ++b) {
        const auto& bed = gw.nodeBeds()[b];
        if (bed.node != node || !bed.exchange || bed.cell < 0)
            continue;
        const auto& d = gw.lidBedDiagnostics()[b];
        SWMM_LidBottomState v{};
        v.flow_model = c.node_subtypes.storages.lid_state[sr].richards ? 1 : 0;
        v.cell = bed.cell;
        v.valid_sample = d.end > d.start;
        v.contact_area_m2 = bed.area;
        v.surface_head_m = (c.nodes.invert_elev[node] + c.nodes.depth[node]) * .3048;
        v.aquifer_head_m = gw.state().z_bed[bed.cell] + gw.state().hg[bed.cell];
        v.aquifer_time_s = gw.cellCompletedTime(bed.cell);
        v.down_cumulative_m3 = gw.richardsBedDownCumulative()[b];
        v.up_cumulative_m3 = gw.richardsBedUpCumulative()[b];
        v.net_cumulative_m3 = v.down_cumulative_m3 - v.up_cumulative_m3;
        v.pending_down_m3 = gw.pendingBedDown(int(b));
        v.pending_up_m3 = gw.pendingBedUp(int(b));
        v.groundwater_residual_m3 = gw.state().continuityResidual();
        v.sample_start_s = d.start;
        v.sample_end_s = d.end;
        v.sampled_aquifer_time_s = d.groundwater_time;
        v.sampled_lid_head_m = d.lid_head;
        v.sampled_aquifer_head_m = d.groundwater_head;
        v.requested_down_m3 = d.requested_down;
        v.requested_up_m3 = d.requested_up;
        v.reserved_down_m3 = d.reserved_down;
        v.reserved_up_m3 = d.reserved_up;
        v.accepted_down_m3 = d.accepted_down;
        v.accepted_up_m3 = d.accepted_up;
        v.net_rate_m3s = v.valid_sample ? (d.accepted_down - d.accepted_up) / (d.end - d.start) : 0;
        *out = v;
        return SWMM_OK;
    }
#endif
    return SWMM_ERR_BADPARAM;
}
