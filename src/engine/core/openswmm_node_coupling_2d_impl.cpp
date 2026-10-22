// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
//
// C API for the per-node 1D↔2D coupling type ([2D_NODE_COUPLING]). Node data:
// built with or without the 2D module, readable with or without a mesh.

/**
 * @file      openswmm_node_coupling_2d_impl.cpp
 * @brief     Public C API implementation for node-to-surface coupling.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include "openswmm_api_common.hpp"
#include "NodeCoupling2D.hpp"
#include "../../../include/openswmm/engine/openswmm_2d.h"

#include <cmath>
#include <cstring>
#include <string>

namespace nc = openswmm::nodecoupling2d;

extern "C" {

SWMM_ENGINE_API int swmm_node_get_2d_coupling_type(SWMM_Engine engine, int node_idx, int* type,
                                                   int* authored) {
    CHECK_HANDLE(engine);
    if (!type)
        return SWMM_ERR_BADPARAM;
    const auto& c = to_engine(engine)->context();
    CHECK_READABLE(c);
    CHECK_INDEX(node_idx >= 0 && node_idx < c.n_nodes());
    *type = static_cast<int>(nc::effectiveType(c, node_idx));
    if (authored)
        *authored = c.node_coupling_2d.row_of(node_idx) >= 0 ? 1 : 0;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_get_2d_coupling_default_type(SWMM_Engine engine, int node_idx,
                                                           int* type) {
    CHECK_HANDLE(engine);
    if (!type)
        return SWMM_ERR_BADPARAM;
    const auto& c = to_engine(engine)->context();
    CHECK_READABLE(c);
    CHECK_INDEX(node_idx >= 0 && node_idx < c.n_nodes());
    *type = static_cast<int>(nc::defaultType(c, node_idx));
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_set_2d_coupling_type(SWMM_Engine engine, int node_idx, int type) {
    CHECK_HANDLE(engine);
    auto& c = to_engine(engine)->context();
    CHECK_EDITABLE(c);
    CHECK_INDEX(node_idx >= 0 && node_idx < c.n_nodes());
    if (type < SWMM_2D_COUPLING_NONE || type > SWMM_2D_COUPLING_OUTFALL)
        return SWMM_ERR_BADPARAM;
    const auto t = static_cast<openswmm::NodeCoupling2DType>(type);
    if (!nc::checkType(c, node_idx, t).empty())
        return SWMM_ERR_BADPARAM;

    auto& rows = c.node_coupling_2d;
    const auto r = static_cast<std::size_t>(rows.ensure_row(node_idx, t));
    rows.type[r] = t;
    for (int k = 0; k < openswmm::kNodeCoupling2DKeys; ++k)
        if (!nc::keyAllowed(t, static_cast<openswmm::NodeCoupling2DKey>(k)))
            rows.params[r][static_cast<std::size_t>(k)] = std::nan("");
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_reset_2d_coupling(SWMM_Engine engine, int node_idx) {
    CHECK_HANDLE(engine);
    auto& c = to_engine(engine)->context();
    CHECK_EDITABLE(c);
    CHECK_INDEX(node_idx >= 0 && node_idx < c.n_nodes());
    const int r = c.node_coupling_2d.row_of(node_idx);
    if (r >= 0)
        c.node_coupling_2d.erase_row(r);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_get_2d_coupling_param(SWMM_Engine engine, int node_idx,
                                                    const char* key, char* value, int value_len) {
    CHECK_HANDLE(engine);
    if (!key || !value || value_len <= 0)
        return SWMM_ERR_BADPARAM;
    const auto& c = to_engine(engine)->context();
    CHECK_READABLE(c);
    CHECK_INDEX(node_idx >= 0 && node_idx < c.n_nodes());
    openswmm::NodeCoupling2DKey k{};
    if (!nc::parseKey(key, k))
        return SWMM_ERR_BADPARAM;

    std::string text;
    const int r = c.node_coupling_2d.row_of(node_idx);
    if (r >= 0) {
        const double v =
            c.node_coupling_2d.params[static_cast<std::size_t>(r)][static_cast<std::size_t>(k)];
        if (!std::isnan(v))
            text = nc::formatValue(k, v);
    }
    const auto n = std::min(text.size(), static_cast<std::size_t>(value_len - 1));
    std::memcpy(value, text.data(), n);
    value[n] = '\0';
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_set_2d_coupling_param(SWMM_Engine engine, int node_idx,
                                                    const char* key, const char* value) {
    CHECK_HANDLE(engine);
    if (!key)
        return SWMM_ERR_BADPARAM;
    auto& c = to_engine(engine)->context();
    CHECK_EDITABLE(c);
    CHECK_INDEX(node_idx >= 0 && node_idx < c.n_nodes());
    openswmm::NodeCoupling2DKey k{};
    if (!nc::parseKey(key, k))
        return SWMM_ERR_BADPARAM;

    auto& rows = c.node_coupling_2d;
    const auto slot = static_cast<std::size_t>(k);
    if (!value || value[0] == '\0') { // clear
        const int r = rows.row_of(node_idx);
        if (r >= 0)
            rows.params[static_cast<std::size_t>(r)][slot] = std::nan("");
        return SWMM_OK;
    }
    const auto t = nc::effectiveType(c, node_idx);
    if (!nc::keyAllowed(t, k))
        return SWMM_ERR_BADPARAM;
    double v = 0.0;
    if (!nc::parseValue(k, value, v).empty())
        return SWMM_ERR_BADPARAM;
    const auto r = static_cast<std::size_t>(rows.ensure_row(node_idx, t));
    rows.params[r][slot] = v;
    return SWMM_OK;
}

} // extern "C"
