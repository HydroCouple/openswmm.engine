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

#include <cmath>
#include <cstring>
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
