// SPDX-License-Identifier: Apache-2.0
//
// Copyright 2026 Caleb Buahin
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/**
 * @file openswmm_transport_impl.cpp
 * @brief E6 — ARD transport configuration surface. See the header for the
 *        read-only-rows decision and its reason.
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "openswmm_api_common.hpp"
#include "../../../include/openswmm/engine/openswmm_transport.h"
#include "../transport/components/EulerianArdComponent/ArdConfig.hpp"

#include <cmath>
#include <cstring>
#include <utility>
#include <string>
#include <vector>

namespace {

/// NUL-safe copy into a caller buffer; a NULL/zero-length buffer skips the
/// field rather than failing the call, so a reader can take only the
/// columns it wants (the pollutant-id reader's convention).
void put_str(const std::string& s, char* buf, int len) {
    if (buf == nullptr || len <= 0) return;
    const auto n = std::min(static_cast<std::size_t>(len - 1), s.size());
    std::memcpy(buf, s.data(), n);
    buf[n] = '\0';
}

int get_row(const std::vector<openswmm::ArdTransportRow>& rows, int index,
            char* element, int elem_len, char* species, int spec_len,
            int* is_ts, double* value, char* ts_name, int ts_len) {
    if (index < 0 || index >= static_cast<int>(rows.size()))
        return SWMM_ERR_BADINDEX;
    const auto& r = rows[static_cast<std::size_t>(index)];
    put_str(r.element, element, elem_len);
    put_str(r.species, species, spec_len);
    if (is_ts) *is_ts = r.is_ts ? 1 : 0;
    if (value) *value = r.value;
    put_str(r.ts_name, ts_name, ts_len);
    return SWMM_OK;
}

}  // namespace

SWMM_ENGINE_API int swmm_transport_get_configured(SWMM_Engine engine,
                                                  int* configured) {
    CHECK_HANDLE(engine);
    if (configured == nullptr) return SWMM_ERR_BADPARAM;
    *configured =
        to_engine(engine)->context().ard_config.configured ? 1 : 0;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_transport_get_dispersion_mode(SWMM_Engine engine,
                                                       int* mode) {
    CHECK_HANDLE(engine);
    if (mode == nullptr) return SWMM_ERR_BADPARAM;
    *mode = static_cast<int>(
        to_engine(engine)->context().ard_config.dispersion_mode);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_transport_set_dispersion_mode(SWMM_Engine engine,
                                                       int mode) {
    CHECK_HANDLE(engine);
    if (mode < static_cast<int>(openswmm::ArdDispersionMode::OFF) ||
        mode > static_cast<int>(openswmm::ArdDispersionMode::VALUE))
        return SWMM_ERR_BADPARAM;
    auto& cfg = to_engine(engine)->context().ard_config;
    cfg.dispersion_mode = static_cast<openswmm::ArdDispersionMode>(mode);
    cfg.configured = true;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_transport_get_dispersion_value(SWMM_Engine engine,
                                                        double* value) {
    CHECK_HANDLE(engine);
    if (value == nullptr) return SWMM_ERR_BADPARAM;
    *value = to_engine(engine)->context().ard_config.dispersion_value;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_transport_set_dispersion_value(SWMM_Engine engine,
                                                        double value) {
    CHECK_HANDLE(engine);
    // Refused, not clamped — the [RADIATIVE_FLUXES] convention: a negative
    // dispersion coefficient is anti-diffusion, which the implicit solve
    // would happily integrate into growing oscillations that look like a
    // model result.
    if (!(value >= 0.0) || !std::isfinite(value)) return SWMM_ERR_BADPARAM;
    auto& cfg = to_engine(engine)->context().ard_config;
    cfg.dispersion_value = value;
    cfg.configured = true;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_transport_get_target_dx(SWMM_Engine engine,
                                                 double* dx) {
    CHECK_HANDLE(engine);
    if (dx == nullptr) return SWMM_ERR_BADPARAM;
    *dx = to_engine(engine)->context().ard_config.target_dx;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_transport_set_target_dx(SWMM_Engine engine,
                                                 double dx) {
    CHECK_HANDLE(engine);
    if (!(dx >= 0.0) || !std::isfinite(dx)) return SWMM_ERR_BADPARAM;
    auto& cfg = to_engine(engine)->context().ard_config;
    cfg.target_dx = dx;
    cfg.configured = true;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_transport_conduit_disp_count(SWMM_Engine engine,
                                                      int* count) {
    CHECK_HANDLE(engine);
    if (count == nullptr) return SWMM_ERR_BADPARAM;
    *count = static_cast<int>(
        to_engine(engine)->context().ard_config.conduit_disp_link.size());
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_transport_get_conduit_disp(SWMM_Engine engine,
                                                    int index,
                                                    int* link_index,
                                                    double* value) {
    CHECK_HANDLE(engine);
    const auto& cfg = to_engine(engine)->context().ard_config;
    CHECK_INDEX(index >= 0 &&
                index < static_cast<int>(cfg.conduit_disp_link.size()));
    const auto ui = static_cast<std::size_t>(index);
    if (link_index) *link_index = cfg.conduit_disp_link[ui];
    if (value) *value = cfg.conduit_disp_value[ui];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_transport_boundary_count(SWMM_Engine engine,
                                                  int* count) {
    CHECK_HANDLE(engine);
    if (count == nullptr) return SWMM_ERR_BADPARAM;
    *count = static_cast<int>(
        to_engine(engine)->context().ard_config.boundary_rows.size());
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_transport_get_boundary(SWMM_Engine engine, int index,
                                                char* element, int elem_len,
                                                char* species, int spec_len,
                                                int* is_ts, double* value,
                                                char* ts_name, int ts_len) {
    CHECK_HANDLE(engine);
    return get_row(to_engine(engine)->context().ard_config.boundary_rows,
                   index, element, elem_len, species, spec_len, is_ts,
                   value, ts_name, ts_len);
}

SWMM_ENGINE_API int swmm_transport_source_count(SWMM_Engine engine,
                                                int* count) {
    CHECK_HANDLE(engine);
    if (count == nullptr) return SWMM_ERR_BADPARAM;
    *count = static_cast<int>(
        to_engine(engine)->context().ard_config.source_rows.size());
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_transport_get_source(SWMM_Engine engine, int index,
                                              char* element, int elem_len,
                                              char* species, int spec_len,
                                              int* is_ts, double* value,
                                              char* ts_name, int ts_len) {
    CHECK_HANDLE(engine);
    return get_row(to_engine(engine)->context().ard_config.source_rows,
                   index, element, elem_len, species, spec_len, is_ts,
                   value, ts_name, ts_len);
}

// ---------------------------------------------------------------------------
// Authoring
// ---------------------------------------------------------------------------

namespace {

/// Re-run the row resolution that open() performs. On failure the raw rows
/// are restored and resolved again, so a rejected edit leaves no trace.
/// While a model is being BUILT the rows stay raw, as the parser stores
/// them: species and series may be declared later, and resolution is
/// order-independent by design.
int resolve_or_revert(openswmm::SimulationContext& ctx,
                      openswmm::ArdConfigData before) {
    if (ctx.state == openswmm::EngineState::BUILDING) {
        ctx.ard_config.transport_rows_resolved = false;
        return SWMM_OK;
    }
    std::vector<std::string> errors;
    openswmm::transport::resolveArdTransportRows(ctx, errors);
    if (errors.empty()) return SWMM_OK;
    ctx.ard_config = std::move(before);
    std::vector<std::string> ignored;
    openswmm::transport::resolveArdTransportRows(ctx, ignored);
    return SWMM_ERR_BADPARAM;
}

int set_row(SWMM_Engine engine, bool is_bc, const char* element,
            const char* species, int is_ts, double value, const char* ts_name) {
    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);
    if (!element || !*element || !species || !*species) return SWMM_ERR_BADPARAM;
    if (is_ts != 0 && is_ts != 1) return SWMM_ERR_BADPARAM;
    if (is_ts && (!ts_name || !*ts_name)) return SWMM_ERR_BADPARAM;
    // Same numeric rules as the parser: a boundary concentration is >= 0, a
    // source rate may be negative (extraction).
    if (!is_ts && (!std::isfinite(value) || (is_bc && value < 0.0)))
        return SWMM_ERR_BADPARAM;

    auto before = ctx.ard_config;
    auto& rows = is_bc ? ctx.ard_config.boundary_rows : ctx.ard_config.source_rows;
    openswmm::ArdTransportRow row;
    row.element = element;
    row.species = species;
    row.is_ts   = is_ts != 0;
    row.value   = is_ts ? 0.0 : value;
    row.ts_name = is_ts ? ts_name : "";
    bool replaced = false;
    for (auto& r : rows)
        if (r.element == row.element && r.species == row.species) {
            r = row;
            replaced = true;
            break;
        }
    if (!replaced) rows.push_back(std::move(row));
    ctx.ard_config.configured = true;
    return resolve_or_revert(ctx, std::move(before));
}

int remove_row(SWMM_Engine engine, bool is_bc, int index) {
    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);
    auto& rows = is_bc ? ctx.ard_config.boundary_rows : ctx.ard_config.source_rows;
    if (index < 0 || index >= static_cast<int>(rows.size())) return SWMM_ERR_BADINDEX;
    auto before = ctx.ard_config;
    rows.erase(rows.begin() + index);
    return resolve_or_revert(ctx, std::move(before));
}

}  // namespace

SWMM_ENGINE_API int swmm_transport_get_detailed_output(SWMM_Engine engine,
                                                       char* buf, int buflen) {
    CHECK_HANDLE(engine);
    if (buf == nullptr || buflen <= 0) return SWMM_ERR_BADPARAM;
    put_str(to_engine(engine)->context().ard_config.detailed_output_path, buf, buflen);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_transport_set_detailed_output(SWMM_Engine engine,
                                                       const char* path) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);
    ctx.ard_config.detailed_output_path = path ? path : "";
    ctx.ard_config.configured = true;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_transport_set_conduit_disp(SWMM_Engine engine,
                                                    int link_index,
                                                    double value) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);
    CHECK_INDEX(link_index >= 0 && link_index < ctx.n_links());
    // Conduits only: structures are zero-volume passthrough (parser rule).
    if (ctx.link_subtypes.conduit_row(link_index) < 0) return SWMM_ERR_BADPARAM;
    if (!(value >= 0.0) || !std::isfinite(value)) return SWMM_ERR_BADPARAM;
    auto& cfg = ctx.ard_config;
    for (std::size_t i = 0; i < cfg.conduit_disp_link.size(); ++i)
        if (cfg.conduit_disp_link[i] == link_index) {
            cfg.conduit_disp_value[i] = value;
            cfg.configured = true;
            return SWMM_OK;
        }
    cfg.conduit_disp_link.push_back(link_index);
    cfg.conduit_disp_value.push_back(value);
    cfg.configured = true;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_transport_remove_conduit_disp(SWMM_Engine engine,
                                                       int index) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);
    auto& cfg = ctx.ard_config;
    CHECK_INDEX(index >= 0 && index < static_cast<int>(cfg.conduit_disp_link.size()));
    cfg.conduit_disp_link.erase(cfg.conduit_disp_link.begin() + index);
    cfg.conduit_disp_value.erase(cfg.conduit_disp_value.begin() + index);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_transport_set_boundary(SWMM_Engine engine,
                                                const char* element,
                                                const char* species,
                                                int is_ts, double value,
                                                const char* ts_name) {
    CHECK_HANDLE(engine);
    return set_row(engine, true, element, species, is_ts, value, ts_name);
}

SWMM_ENGINE_API int swmm_transport_remove_boundary(SWMM_Engine engine,
                                                   int index) {
    CHECK_HANDLE(engine);
    return remove_row(engine, true, index);
}

SWMM_ENGINE_API int swmm_transport_set_source(SWMM_Engine engine,
                                              const char* element,
                                              const char* species,
                                              int is_ts, double value,
                                              const char* ts_name) {
    CHECK_HANDLE(engine);
    return set_row(engine, false, element, species, is_ts, value, ts_name);
}

SWMM_ENGINE_API int swmm_transport_remove_source(SWMM_Engine engine,
                                                 int index) {
    CHECK_HANDLE(engine);
    return remove_row(engine, false, index);
}
