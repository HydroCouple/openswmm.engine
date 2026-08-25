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
 * @file openswmm_reactions_impl.cpp
 * @brief C API implementation — reaction-system validation + discovery
 *        (E-C1).
 *
 * @see include/openswmm/engine/openswmm_reactions.h
 * @ingroup engine_api
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "openswmm_api_common.hpp"
#include "../../../include/openswmm/engine/openswmm_reactions.h"

#include "../transport/components/ReactionModule/ReactionExpression.hpp"

#include <cstring>
#include <string>
#include <vector>

namespace {

/// Copy a std::string into a NUL-terminated caller buffer, truncating safely.
inline void copy_to_buf(const std::string& src, char* buf, int buflen) {
    if (!buf || buflen <= 0) return;
    const int copy_len = std::min(static_cast<int>(src.size()), buflen - 1);
    std::memcpy(buf, src.c_str(), static_cast<std::size_t>(copy_len));
    buf[copy_len] = '\0';
}

} // namespace

extern "C" {

SWMM_ENGINE_API int swmm_reaction_validate_expression(SWMM_Engine engine,
        int scope, const char* expr, char* errbuf, int buflen, int* col_out) {
    CHECK_HANDLE(engine);
    if (errbuf && buflen > 0) errbuf[0] = '\0';
    if (col_out) *col_out = -1;
    if (!expr) return SWMM_ERR_BADPARAM;
    if (scope != SWMM_RXN_SCOPE_TERM && scope != SWMM_RXN_SCOPE_PIPE &&
        scope != SWMM_RXN_SCOPE_TANK)
        return SWMM_ERR_BADPARAM;

    const auto& ctx = to_engine(engine)->context();
    const auto& rx  = ctx.reactions;

    openswmm::transport::RxSymbols sym;
    sym.species    = &rx.species_name;
    sym.coefs      = &rx.coef_name;
    sym.terms      = &rx.term_name;
    sym.pollutants = &ctx.pollutant_names.names();
    // D-RC1: validation accepts references to ALL terms — the forward-only
    // rule is a file-ordering property enforced at apply time.
    sym.max_term   = static_cast<int>(rx.term_name.size());

    std::vector<openswmm::transport::RxToken> pool;   // scratch, discarded
    openswmm::transport::RxExprSpan span;
    int col = -1;
    const std::string err = openswmm::transport::compileReactionExpression(
        expr, sym, pool, span, col);
    if (err.empty()) return SWMM_OK;

    if (errbuf && buflen > 0) copy_to_buf(err, errbuf, buflen);
    if (col_out) *col_out = col;
    return SWMM_ERR_BADPARAM;
}

// ============================================================================
// Model discovery
// ============================================================================

SWMM_ENGINE_API int swmm_reaction_species_count(SWMM_Engine engine) {
    if (!engine) return -1;
    return to_engine(engine)->context().reactions.n_species();
}

SWMM_ENGINE_API int swmm_reaction_species_get(SWMM_Engine engine, int idx,
        char* name, int name_len, int* is_wall,
        char* units, int units_len, double* atol, double* rtol) {
    CHECK_HANDLE(engine);
    const auto& rx = to_engine(engine)->context().reactions;
    CHECK_INDEX(idx >= 0 && idx < rx.n_species());
    if (!name || name_len <= 0 || !is_wall || !units || units_len <= 0 ||
        !atol || !rtol)
        return SWMM_ERR_BADPARAM;
    const auto u = static_cast<std::size_t>(idx);
    copy_to_buf(rx.species_name[u], name, name_len);
    *is_wall = rx.species_is_wall[u] ? 1 : 0;
    copy_to_buf(rx.species_units[u], units, units_len);
    *atol = rx.species_atol[u];
    *rtol = rx.species_rtol[u];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_reaction_coeff_count(SWMM_Engine engine) {
    if (!engine) return -1;
    return static_cast<int>(
        to_engine(engine)->context().reactions.coef_name.size());
}

SWMM_ENGINE_API int swmm_reaction_coeff_get(SWMM_Engine engine, int idx,
        char* name, int name_len, int* is_param, double* value) {
    CHECK_HANDLE(engine);
    const auto& rx = to_engine(engine)->context().reactions;
    CHECK_INDEX(idx >= 0 &&
                idx < static_cast<int>(rx.coef_name.size()));
    if (!name || name_len <= 0 || !is_param || !value)
        return SWMM_ERR_BADPARAM;
    const auto u = static_cast<std::size_t>(idx);
    copy_to_buf(rx.coef_name[u], name, name_len);
    *is_param = rx.coef_is_param[u] ? 1 : 0;
    *value    = rx.coef_value[u];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_reaction_term_count(SWMM_Engine engine) {
    if (!engine) return -1;
    return static_cast<int>(
        to_engine(engine)->context().reactions.term_name.size());
}

SWMM_ENGINE_API int swmm_reaction_term_get(SWMM_Engine engine, int idx,
        char* name, int name_len, char* expr, int expr_len) {
    CHECK_HANDLE(engine);
    const auto& rx = to_engine(engine)->context().reactions;
    CHECK_INDEX(idx >= 0 &&
                idx < static_cast<int>(rx.term_name.size()));
    if (!name || name_len <= 0 || !expr || expr_len <= 0)
        return SWMM_ERR_BADPARAM;
    const auto u = static_cast<std::size_t>(idx);
    copy_to_buf(rx.term_name[u], name, name_len);
    copy_to_buf(rx.term_expr_src[u], expr, expr_len);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_reaction_expr_get(SWMM_Engine engine, int scope,
        int species_idx, int* form, char* expr, int expr_len) {
    CHECK_HANDLE(engine);
    const auto& rx = to_engine(engine)->context().reactions;
    CHECK_INDEX(species_idx >= 0 && species_idx < rx.n_species());
    if (!form || !expr || expr_len <= 0) return SWMM_ERR_BADPARAM;
    if (scope != SWMM_RXN_SCOPE_PIPE && scope != SWMM_RXN_SCOPE_TANK)
        return SWMM_ERR_BADPARAM;
    const auto u = static_cast<std::size_t>(species_idx);
    if (scope == SWMM_RXN_SCOPE_PIPE) {
        *form = static_cast<int>(rx.pipe_form[u]);
        copy_to_buf(rx.pipe_expr_src[u], expr, expr_len);
    } else {
        *form = static_cast<int>(rx.tank_form[u]);
        copy_to_buf(rx.tank_expr_src[u], expr, expr_len);
    }
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_reaction_option_get(SWMM_Engine engine,
        const char* key, char* value, int value_len) {
    CHECK_HANDLE(engine);
    if (!key || !value || value_len <= 0) return SWMM_ERR_BADPARAM;
    const auto& rx = to_engine(engine)->context().reactions;

    std::string k(key);
    for (auto& ch : k)
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));

    std::string v;
    if (k == "SOLVER") {
        static const char* kTok[] = {"EUL", "RK5", "ROS2", "BDF2"};
        v = kTok[static_cast<int>(rx.solver)];
    } else if (k == "COUPLING") {
        v = (static_cast<int>(rx.coupling) == 1) ? "FULL" : "NONE";
    } else if (k == "RATE_UNITS") {
        static const char* kTok[] = {"SEC", "MIN", "HR", "DAY"};
        v = kTok[static_cast<int>(rx.rate_units)];
    } else if (k == "AREA_UNITS") {
        static const char* kTok[] = {"FT2", "M2", "CM2"};
        v = kTok[static_cast<int>(rx.area_units)];
    } else if (k == "TIMESTEP") {
        v = std::to_string(rx.timestep);
    } else if (k == "ATOL") {
        v = std::to_string(rx.atol);
    } else if (k == "RTOL") {
        v = std::to_string(rx.rtol);
    } else {
        return SWMM_ERR_BADPARAM;
    }
    copy_to_buf(v, value, value_len);
    return SWMM_OK;
}

// ============================================================================
// Static vocabulary — served by the expression compiler's own tables so the
// completer/highlighter cannot drift from what actually compiles.
// ============================================================================

SWMM_ENGINE_API int swmm_reaction_hydvar_count(void) {
    return openswmm::transport::reactionHydVarCount();
}

SWMM_ENGINE_API int swmm_reaction_hydvar_get(int idx, char* name,
        int name_len, char* description, int desc_len) {
    const char* n = nullptr;
    const char* d = nullptr;
    if (!openswmm::transport::reactionHydVarInfo(idx, &n, &d))
        return SWMM_ERR_BADINDEX;
    if (!name || name_len <= 0) return SWMM_ERR_BADPARAM;
    copy_to_buf(n, name, name_len);
    if (description && desc_len > 0) copy_to_buf(d, description, desc_len);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_reaction_function_count(void) {
    return openswmm::transport::reactionFunctionCount();
}

SWMM_ENGINE_API int swmm_reaction_function_get(int idx, char* name,
        int name_len, int* arity) {
    const char* n = nullptr;
    int a = 0;
    if (!openswmm::transport::reactionFunctionInfo(idx, &n, &a))
        return SWMM_ERR_BADINDEX;
    if (!name || name_len <= 0 || !arity) return SWMM_ERR_BADPARAM;
    copy_to_buf(n, name, name_len);
    *arity = a;
    return SWMM_OK;
}

} // extern "C"
