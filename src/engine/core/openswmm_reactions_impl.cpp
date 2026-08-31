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
#include "../transport/components/ReactionModule/ReactionsComponent.hpp"
#include "../transport/components/ReactionModule/ReactionsWriter.hpp"
#include "../plugins/ProcessComponentRegistry.hpp"

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
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

/// True when any compiled expression pushes slot @p idx of kind @p op —
/// the authoritative referenced-check for remove refusals (D-RC3): the
/// compiled tokens are what evaluation actually reads, so a scan over the
/// pool cannot miss or over-count.
bool pool_references(const openswmm::ReactionData& rx, uint8_t op, int idx) {
    for (const auto& t : rx.token_pool)
        if (t.op == op && t.idx == idx) return true;
    return false;
}

/// Rebuild the species registry's MSX block from ctx.reactions after a
/// species add/remove (D-RC3): snapshot every row after the old MSX block
/// (the reserved species live there), truncate, re-add the MSX species in
/// ReactionData order, restore the tail. Legal in BUILDING/OPENED only —
/// nothing downstream has latched registry indexes before initialize().
void rebuild_msx_registry(openswmm::SimulationContext& ctx, int old_base,
                          int old_n_msx) {
    auto& reg = ctx.species_registry;
    auto& rx  = ctx.reactions;
    const int base = (old_base >= 0) ? old_base : reg.pollutant_count();
    struct Row { std::string name; openswmm::SpeciesKind kind;
                 std::string units; };
    std::vector<Row> tail;
    for (int i = base + ((old_base >= 0) ? old_n_msx : 0);
         i < reg.count(); ++i)
        tail.push_back({reg.name(i), reg.kind(i), reg.units(i)});

    reg.truncate_to(base);
    rx.registry_base = -1;
    for (int s = 0; s < rx.n_species(); ++s) {
        const auto us = static_cast<std::size_t>(s);
        const int idx = reg.add(rx.species_name[us],
                                rx.species_is_wall[us]
                                    ? openswmm::SpeciesKind::MSX_WALL
                                    : openswmm::SpeciesKind::MSX_BULK,
                                rx.species_units[us]);
        if (rx.registry_base < 0) rx.registry_base = idx;
    }
    for (auto& r : tail)
        reg.add(std::move(r.name), r.kind, std::move(r.units));
}

/// Recompile after a mutation; on failure restore the given backups and
/// return false (D-RC5: the engine never holds an uncompilable system).
bool recompile_or_rollback(openswmm::SimulationContext& ctx,
                           openswmm::ReactionData& rx_backup,
                           openswmm::SpeciesRegistry& reg_backup) {
    std::vector<std::string> errs;
    if (openswmm::transport::recompileReactionSystem(ctx, errs)) return true;
    ctx.reactions        = std::move(rx_backup);
    ctx.species_registry = std::move(reg_backup);
    return false;
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
    } else if (k == "TEMPERATURE") {
        v = std::to_string(rx.default_temp_c);
    } else {
        return SWMM_ERR_BADPARAM;
    }
    copy_to_buf(v, value, value_len);
    return SWMM_OK;
}

// ============================================================================
// CRUD (E-C2) — BUILDING/OPENED, eager validation, rollback on failure
// ============================================================================

SWMM_ENGINE_API int swmm_reaction_species_add(SWMM_Engine engine,
        const char* name, int is_wall, const char* units, double atol,
        double rtol) {
    CHECK_HANDLE(engine);
    if (!name || !*name || !units || atol < 0.0 || rtol < 0.0)
        return SWMM_ERR_BADPARAM;
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    // Names are unique across ALL kinds (an MSX species may not shadow a
    // pollutant or reserved species) — the parser's collision rule — and a
    // species named like a coefficient or term would silently SHADOW it in
    // expressions (resolution order: species first), so those collide too.
    if (ctx.species_registry.find(name) >= 0 ||
        ctx.reactions.find_coef(name) >= 0 ||
        ctx.reactions.find_term(name) >= 0)
        return SWMM_ERR_BADPARAM;

    auto rx_backup  = ctx.reactions;
    auto reg_backup = ctx.species_registry;
    auto& rx = ctx.reactions;
    const int old_base = rx.registry_base;
    const int old_n    = rx.n_species();
    rx.species_name.push_back(name);
    rx.species_is_wall.push_back(is_wall ? 1 : 0);
    rx.species_units.push_back(units);
    rx.species_atol.push_back(atol);
    rx.species_rtol.push_back(rtol);
    rx.pipe_form.push_back(openswmm::ReactionExprForm::NONE);
    rx.pipe_expr_src.emplace_back();
    rx.tank_form.push_back(openswmm::ReactionExprForm::NONE);
    rx.tank_expr_src.emplace_back();
    rx.init_global.push_back(0.0);
    rebuild_msx_registry(ctx, old_base, old_n);
    if (!recompile_or_rollback(ctx, rx_backup, reg_backup))
        return SWMM_ERR_BADPARAM;
    rx.configured = true;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_reaction_species_remove(SWMM_Engine engine,
        int idx) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    auto& rx = ctx.reactions;
    CHECK_INDEX(idx >= 0 && idx < rx.n_species());
    // D-RC3: refuse while any compiled expression references the species.
    if (pool_references(rx, openswmm::RxToken::PUSH_SPECIES, idx))
        return SWMM_ERR_BADPARAM;

    auto rx_backup  = ctx.reactions;
    auto reg_backup = ctx.species_registry;
    const int old_base = rx.registry_base;
    const int old_n    = rx.n_species();
    const auto u = static_cast<std::size_t>(idx);
    // Erase the row from EVERY index-aligned vector (the D-RC3 hazard).
    rx.species_name.erase(rx.species_name.begin() + u);
    rx.species_is_wall.erase(rx.species_is_wall.begin() + u);
    rx.species_units.erase(rx.species_units.begin() + u);
    rx.species_atol.erase(rx.species_atol.begin() + u);
    rx.species_rtol.erase(rx.species_rtol.begin() + u);
    rx.pipe_form.erase(rx.pipe_form.begin() + u);
    rx.pipe_expr_src.erase(rx.pipe_expr_src.begin() + u);
    rx.tank_form.erase(rx.tank_form.begin() + u);
    rx.tank_expr_src.erase(rx.tank_expr_src.begin() + u);
    rx.init_global.erase(rx.init_global.begin() + u);
    // Per-element initial rows for the species vanish with it; higher
    // species indexes shift down.
    for (int r = static_cast<int>(rx.init_elem_idx.size()) - 1; r >= 0; --r) {
        const auto ur = static_cast<std::size_t>(r);
        if (rx.init_elem_species[ur] == idx) {
            rx.init_elem_is_link.erase(rx.init_elem_is_link.begin() + ur);
            rx.init_elem_idx.erase(rx.init_elem_idx.begin() + ur);
            rx.init_elem_species.erase(rx.init_elem_species.begin() + ur);
            rx.init_elem_value.erase(rx.init_elem_value.begin() + ur);
        } else if (rx.init_elem_species[ur] > idx) {
            --rx.init_elem_species[ur];
        }
    }
    rebuild_msx_registry(ctx, old_base, old_n);
    if (!recompile_or_rollback(ctx, rx_backup, reg_backup))
        return SWMM_ERR_BADPARAM;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_reaction_coeff_add(SWMM_Engine engine,
        const char* name, int is_param, double value) {
    CHECK_HANDLE(engine);
    if (!name || !*name) return SWMM_ERR_BADPARAM;
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    auto& rx = ctx.reactions;
    // The parser's duplicate rule: a coefficient may not duplicate a
    // coefficient, species, or term name.
    if (rx.find_coef(name) >= 0 || rx.find_species(name) >= 0 ||
        rx.find_term(name) >= 0)
        return SWMM_ERR_BADPARAM;
    rx.coef_name.push_back(name);
    rx.coef_is_param.push_back(is_param ? 1 : 0);
    rx.coef_value.push_back(value);
    // Appending shifts no compiled index — no recompile needed.
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_reaction_coeff_set_value(SWMM_Engine engine,
        int idx, double value) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    auto& rx = ctx.reactions;
    CHECK_INDEX(idx >= 0 && idx < static_cast<int>(rx.coef_name.size()));
    rx.coef_value[static_cast<std::size_t>(idx)] = value;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_reaction_coeff_remove(SWMM_Engine engine, int idx) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    auto& rx = ctx.reactions;
    CHECK_INDEX(idx >= 0 && idx < static_cast<int>(rx.coef_name.size()));
    if (pool_references(rx, openswmm::RxToken::PUSH_COEF, idx))
        return SWMM_ERR_BADPARAM;
    auto rx_backup  = ctx.reactions;
    auto reg_backup = ctx.species_registry;
    const auto u = static_cast<std::size_t>(idx);
    rx.coef_name.erase(rx.coef_name.begin() + u);
    rx.coef_is_param.erase(rx.coef_is_param.begin() + u);
    rx.coef_value.erase(rx.coef_value.begin() + u);
    // Higher coefficient indexes shift — recompile re-derives them from
    // the sources.
    if (!recompile_or_rollback(ctx, rx_backup, reg_backup))
        return SWMM_ERR_BADPARAM;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_reaction_term_add(SWMM_Engine engine,
        const char* name, const char* expr) {
    CHECK_HANDLE(engine);
    if (!name || !*name || !expr) return SWMM_ERR_BADPARAM;
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    auto& rx = ctx.reactions;
    if (rx.find_term(name) >= 0 || rx.find_species(name) >= 0 ||
        rx.find_coef(name) >= 0)
        return SWMM_ERR_BADPARAM;
    auto rx_backup  = ctx.reactions;
    auto reg_backup = ctx.species_registry;
    rx.term_name.push_back(name);
    rx.term_expr_src.push_back(expr);
    if (!recompile_or_rollback(ctx, rx_backup, reg_backup))
        return SWMM_ERR_BADPARAM;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_reaction_term_set_expr(SWMM_Engine engine, int idx,
        const char* expr) {
    CHECK_HANDLE(engine);
    if (!expr) return SWMM_ERR_BADPARAM;
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    auto& rx = ctx.reactions;
    CHECK_INDEX(idx >= 0 && idx < static_cast<int>(rx.term_name.size()));
    auto rx_backup  = ctx.reactions;
    auto reg_backup = ctx.species_registry;
    rx.term_expr_src[static_cast<std::size_t>(idx)] = expr;
    if (!recompile_or_rollback(ctx, rx_backup, reg_backup))
        return SWMM_ERR_BADPARAM;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_reaction_term_remove(SWMM_Engine engine, int idx) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    auto& rx = ctx.reactions;
    CHECK_INDEX(idx >= 0 && idx < static_cast<int>(rx.term_name.size()));
    if (pool_references(rx, openswmm::RxToken::PUSH_TERM, idx))
        return SWMM_ERR_BADPARAM;
    auto rx_backup  = ctx.reactions;
    auto reg_backup = ctx.species_registry;
    const auto u = static_cast<std::size_t>(idx);
    rx.term_name.erase(rx.term_name.begin() + u);
    rx.term_expr_src.erase(rx.term_expr_src.begin() + u);
    if (!recompile_or_rollback(ctx, rx_backup, reg_backup))
        return SWMM_ERR_BADPARAM;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_reaction_expr_set(SWMM_Engine engine, int scope,
        int species_idx, int form, const char* expr) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    auto& rx = ctx.reactions;
    CHECK_INDEX(species_idx >= 0 && species_idx < rx.n_species());
    if (scope != SWMM_RXN_SCOPE_PIPE && scope != SWMM_RXN_SCOPE_TANK)
        return SWMM_ERR_BADPARAM;
    if (form < SWMM_RXN_FORM_NONE || form > SWMM_RXN_FORM_FORMULA)
        return SWMM_ERR_BADPARAM;
    if (form != SWMM_RXN_FORM_NONE && (!expr || !*expr))
        return SWMM_ERR_BADPARAM;

    auto rx_backup  = ctx.reactions;
    auto reg_backup = ctx.species_registry;
    const auto u = static_cast<std::size_t>(species_idx);
    auto& forms = (scope == SWMM_RXN_SCOPE_PIPE) ? rx.pipe_form
                                                 : rx.tank_form;
    auto& srcs  = (scope == SWMM_RXN_SCOPE_PIPE) ? rx.pipe_expr_src
                                                 : rx.tank_expr_src;
    forms[u] = static_cast<openswmm::ReactionExprForm>(form);
    srcs[u]  = (form == SWMM_RXN_FORM_NONE) ? std::string() : expr;
    if (!recompile_or_rollback(ctx, rx_backup, reg_backup))
        return SWMM_ERR_BADPARAM;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_reaction_option_set(SWMM_Engine engine,
        const char* key, const char* value) {
    CHECK_HANDLE(engine);
    if (!key || !value) return SWMM_ERR_BADPARAM;
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    auto& rx = ctx.reactions;

    auto up = [](const char* s) {
        std::string r(s);
        for (auto& ch : r)
            ch = static_cast<char>(
                std::toupper(static_cast<unsigned char>(ch)));
        return r;
    };
    const std::string k = up(key);
    const std::string v = up(value);

    if (k == "SOLVER") {
        if      (v == "EUL")  rx.solver = openswmm::ReactionSolverKind::EUL;
        else if (v == "RK5")  rx.solver = openswmm::ReactionSolverKind::RK5;
        else if (v == "ROS2") rx.solver = openswmm::ReactionSolverKind::ROS2;
        else if (v == "BDF2") rx.solver = openswmm::ReactionSolverKind::BDF2;
        else return SWMM_ERR_BADPARAM;
    } else if (k == "COUPLING") {
        if      (v == "NONE") rx.coupling = openswmm::ReactionCoupling::NONE;
        else if (v == "FULL") rx.coupling = openswmm::ReactionCoupling::FULL;
        else return SWMM_ERR_BADPARAM;
    } else if (k == "RATE_UNITS") {
        if      (v == "SEC") rx.rate_units = openswmm::ReactionRateUnits::SEC;
        else if (v == "MIN") rx.rate_units = openswmm::ReactionRateUnits::MIN;
        else if (v == "HR")  rx.rate_units = openswmm::ReactionRateUnits::HR;
        else if (v == "DAY") rx.rate_units = openswmm::ReactionRateUnits::DAY;
        else return SWMM_ERR_BADPARAM;
    } else if (k == "AREA_UNITS") {
        if      (v == "FT2") rx.area_units = openswmm::ReactionAreaUnits::FT2;
        else if (v == "M2")  rx.area_units = openswmm::ReactionAreaUnits::M2;
        else if (v == "CM2") rx.area_units = openswmm::ReactionAreaUnits::CM2;
        else return SWMM_ERR_BADPARAM;
    } else if (k == "TIMESTEP") {
        char* end = nullptr;
        const double d = std::strtod(value, &end);
        if (!end || *end != '\0' || end == value || d < 0.0)
            return SWMM_ERR_BADPARAM;
        rx.timestep = d;
    } else if (k == "ATOL" || k == "RTOL") {
        char* end = nullptr;
        const double d = std::strtod(value, &end);
        if (!end || *end != '\0' || end == value || d <= 0.0)
            return SWMM_ERR_BADPARAM;
        (k == "ATOL" ? rx.atol : rx.rtol) = d;
    } else if (k == "TEMPERATURE") {
        // Any finite value is a temperature (degC); no range gate.
        char* end = nullptr;
        const double d = std::strtod(value, &end);
        if (!end || *end != '\0' || end == value || !std::isfinite(d))
            return SWMM_ERR_BADPARAM;
        rx.default_temp_c = d;
    } else {
        return SWMM_ERR_BADPARAM;
    }
    return SWMM_OK;
}

// ============================================================================
// Initial quality (GLOBAL + the E-B NODE/LINK rows)
// ============================================================================

SWMM_ENGINE_API int swmm_reaction_init_global_get(SWMM_Engine engine,
        int species_idx, double* value) {
    CHECK_HANDLE(engine);
    const auto& rx = to_engine(engine)->context().reactions;
    CHECK_INDEX(species_idx >= 0 && species_idx < rx.n_species());
    if (!value) return SWMM_ERR_BADPARAM;
    const auto u = static_cast<std::size_t>(species_idx);
    *value = (u < rx.init_global.size()) ? rx.init_global[u] : 0.0;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_reaction_init_global_set(SWMM_Engine engine,
        int species_idx, double value) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    auto& rx = ctx.reactions;
    CHECK_INDEX(species_idx >= 0 && species_idx < rx.n_species());
    if (value < 0.0) return SWMM_ERR_BADPARAM;
    if (rx.init_global.size() < static_cast<std::size_t>(rx.n_species()))
        rx.init_global.resize(static_cast<std::size_t>(rx.n_species()), 0.0);
    rx.init_global[static_cast<std::size_t>(species_idx)] = value;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_reaction_init_elem_count(SWMM_Engine engine) {
    if (!engine) return -1;
    return static_cast<int>(
        to_engine(engine)->context().reactions.init_elem_idx.size());
}

SWMM_ENGINE_API int swmm_reaction_init_elem_get(SWMM_Engine engine,
        int entry_idx, int* is_link, int* elem_idx, int* species_idx,
        double* value) {
    CHECK_HANDLE(engine);
    const auto& rx = to_engine(engine)->context().reactions;
    CHECK_INDEX(entry_idx >= 0 &&
                entry_idx < static_cast<int>(rx.init_elem_idx.size()));
    if (!is_link || !elem_idx || !species_idx || !value)
        return SWMM_ERR_BADPARAM;
    const auto u = static_cast<std::size_t>(entry_idx);
    *is_link     = rx.init_elem_is_link[u] ? 1 : 0;
    *elem_idx    = rx.init_elem_idx[u];
    *species_idx = rx.init_elem_species[u];
    *value       = rx.init_elem_value[u];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_reaction_init_elem_set(SWMM_Engine engine,
        int is_link, int elem_idx, int species_idx, double value) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    auto& rx = ctx.reactions;
    CHECK_INDEX(species_idx >= 0 && species_idx < rx.n_species());
    const int n_elems = is_link ? ctx.n_links() : ctx.n_nodes();
    CHECK_INDEX(elem_idx >= 0 && elem_idx < n_elems);
    if (value < 0.0) return SWMM_ERR_BADPARAM;
    for (std::size_t k = 0; k < rx.init_elem_idx.size(); ++k) {
        if ((rx.init_elem_is_link[k] != 0) == (is_link != 0) &&
            rx.init_elem_idx[k] == elem_idx &&
            rx.init_elem_species[k] == species_idx) {
            rx.init_elem_value[k] = value;                    // upsert
            return SWMM_OK;
        }
    }
    rx.init_elem_is_link.push_back(is_link ? 1 : 0);
    rx.init_elem_idx.push_back(elem_idx);
    rx.init_elem_species.push_back(species_idx);
    rx.init_elem_value.push_back(value);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_reaction_init_elem_remove(SWMM_Engine engine,
        int entry_idx) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    auto& rx = ctx.reactions;
    CHECK_INDEX(entry_idx >= 0 &&
                entry_idx < static_cast<int>(rx.init_elem_idx.size()));
    const auto u = static_cast<std::size_t>(entry_idx);
    rx.init_elem_is_link.erase(rx.init_elem_is_link.begin() + u);
    rx.init_elem_idx.erase(rx.init_elem_idx.begin() + u);
    rx.init_elem_species.erase(rx.init_elem_species.begin() + u);
    rx.init_elem_value.erase(rx.init_elem_value.begin() + u);
    return SWMM_OK;
}

// ============================================================================
// Whole-file text surface (E-C3)
// ============================================================================

SWMM_ENGINE_API int swmm_reactions_serialize(SWMM_Engine engine, char* buf,
        int buflen, int* needed_len) {
    CHECK_HANDLE(engine);
    if (!needed_len) return SWMM_ERR_BADPARAM;
    const std::string text = openswmm::transport::serializeReactionSystem(
        to_engine(engine)->context());
    *needed_len = static_cast<int>(text.size()) + 1;
    if (!buf) return SWMM_OK;
    if (buflen < *needed_len) return SWMM_ERR_BADPARAM;
    std::memcpy(buf, text.c_str(), text.size() + 1);
    return SWMM_OK;
}

namespace {

/// Shared body of check_text (keep=false) and apply_text (keep=true):
/// parse, strip the current MSX system (preserving the reserved-species
/// tail so the re-applied block lands back in canonical position), apply,
/// and either keep or restore. On ANY failure both ctx.reactions and the
/// registry are restored byte-identical (the staged-swap contract).
int apply_or_check_text(openswmm::SimulationContext& ctx, const char* text,
                        char* errbuf, int buflen, bool keep) {
    if (errbuf && buflen > 0) errbuf[0] = '\0';
    if (!text || !*text) {
        if (errbuf && buflen > 0)
            copy_to_buf("empty reactions text", errbuf, buflen);
        return SWMM_ERR_BADPARAM;
    }

    openswmm::components::ComponentConfigSections sections;
    const std::string perr = openswmm::components::parse_component_config_text(
        text, "<text>", sections);
    if (!perr.empty()) {
        if (errbuf && buflen > 0) copy_to_buf(perr, errbuf, buflen);
        return SWMM_ERR_BADPARAM;
    }

    auto rx_backup  = ctx.reactions;
    auto reg_backup = ctx.species_registry;

    // Strip: remove the current MSX block but SNAPSHOT everything after it
    // (the reserved species), so the fresh apply's registry adds land in
    // the canonical pollutants < MSX < reserved order.
    auto& reg = ctx.species_registry;
    auto& rx  = ctx.reactions;
    const int base = (rx.registry_base >= 0) ? rx.registry_base
                                             : reg.pollutant_count();
    const int old_msx = (rx.registry_base >= 0) ? rx.n_species() : 0;
    struct Row { std::string name; openswmm::SpeciesKind kind;
                 std::string units; };
    std::vector<Row> tail;
    for (int i = base + old_msx; i < reg.count(); ++i)
        tail.push_back({reg.name(i), reg.kind(i), reg.units(i)});
    reg.truncate_to(base);
    rx.clear();

    std::vector<std::string> errs;
    openswmm::transport::applyReactionSections(ctx, sections, errs);
    if (!errs.empty()) {
        ctx.reactions        = std::move(rx_backup);
        ctx.species_registry = std::move(reg_backup);
        if (errbuf && buflen > 0) copy_to_buf(errs.front(), errbuf, buflen);
        return SWMM_ERR_BADPARAM;
    }
    for (auto& r : tail)
        reg.add(std::move(r.name), r.kind, std::move(r.units));

    if (!keep) {
        ctx.reactions        = std::move(rx_backup);
        ctx.species_registry = std::move(reg_backup);
    }
    return SWMM_OK;
}

} // namespace

SWMM_ENGINE_API int swmm_reactions_check_text(SWMM_Engine engine,
        const char* text, char* errbuf, int buflen) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    // The dry-run stages through the live context (restore-always), so it
    // shares apply's editable-states guard rather than racing a running
    // solver with a transient swap.
    CHECK_GEOMETRY(ctx);
    return apply_or_check_text(ctx, text, errbuf, buflen, /*keep=*/false);
}

SWMM_ENGINE_API int swmm_reactions_apply_text(SWMM_Engine engine,
        const char* text, char* errbuf, int buflen) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    return apply_or_check_text(ctx, text, errbuf, buflen, /*keep=*/true);
}

SWMM_ENGINE_API int swmm_reactions_save(SWMM_Engine engine,
        const char* path_or_null) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();

    std::string path = path_or_null ? path_or_null : "";
    if (path.empty()) {
        // The bound reactions component's config path — resolved when the
        // model was opened from a file, verbatim otherwise.
        for (const auto& spec : ctx.process_component_specs) {
            if (spec.id != "org.hydrocouple.openswmm.reactions") continue;
            path = !spec.resolved_config_path.empty()
                       ? spec.resolved_config_path
                       : spec.config_path;
            break;
        }
    }
    if (path.empty()) return SWMM_ERR_BADPARAM;

    const std::string text =
        openswmm::transport::serializeReactionSystem(ctx);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f.is_open()) return SWMM_ERR_BADPARAM;
    f << text;
    return f.good() ? SWMM_OK : SWMM_ERR_BADPARAM;
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
