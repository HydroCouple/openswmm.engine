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
 * @file      ReactionsComponent.cpp
 * @brief     Reactions process component — phase R1 body.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include "ReactionsComponent.hpp"
#include "../../MsxInitialQuality.hpp" // U2
#include "ReactionsWriter.hpp"         // IO3a save hook

#include <cstdlib>
#include <algorithm>
#include <functional>
#include <cmath>

#include "../../../core/SimulationContext.hpp"
#include "../../../input/Tokenizer.hpp"
#include "../../../plugins/ProcessComponentRegistry.hpp"
#include "ReactionExpression.hpp"

namespace openswmm::transport {

namespace {

using input::Tokenizer;

constexpr const char* kComponentId = "org.hydrocouple.openswmm.reactions";

/**
 * @brief The raw remainder of @p line after its first @p n_lead fields — expression bodies span
 * the rest of their row VERBATIM, so an unquoted comma (`MIN(A, 2)`) survives where the old
 * tokenize-then-rejoin capture ate it as a delimiter (E-D1). A fully double-quoted remainder is
 * unwrapped, keeping the pre-E-D1 quoted workaround working.
 *
 * @details Preserve the expression tail after a fixed number of quoted/unquoted leading fields.
 * @param[in] line Stable row text with space/tab/comma field delimiters.
 * @param[in] n_lead Number of fields to skip; exhausted input yields empty tail.
 * @return Owning remaining text with leading delimiters/trailing space-tab-CR removed and one
 * enclosing quote pair stripped. No escape/nested-quote interpretation or syntax validation;
 * allocation can throw, no compiled/model/clock mutation.
 */
std::string expr_payload(const std::string& line, std::size_t n_lead) {
    const auto is_delim = [](char c) { return c == ' ' || c == '\t' || c == ','; };
    std::size_t i = 0;
    for (std::size_t f = 0; f < n_lead; ++f) {
        while (i < line.size() && is_delim(line[i]))
            ++i;
        if (i < line.size() && line[i] == '"') { // quoted field
            ++i;
            while (i < line.size() && line[i] != '"')
                ++i;
            if (i < line.size())
                ++i;
        } else {
            while (i < line.size() && !is_delim(line[i]))
                ++i;
        }
    }
    while (i < line.size() && is_delim(line[i]))
        ++i;
    std::size_t e = line.size();
    while (e > i && (line[e - 1] == ' ' || line[e - 1] == '\t' || line[e - 1] == '\r'))
        --e;
    std::string s = line.substr(i, e - i);
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
        s = s.substr(1, s.size() - 2);
    return s;
}

/**
 * @brief Parse a full finite reaction configuration scalar.
 * @param[in] s Nonempty numeric token, strtod full consumption required.
 * @param[out] out Parsed double even on false, no unit conversion.
 * @return True only completely parsed finite value; no physical-range or errno test. No
 * allocation/model mutation.
 */
bool to_num(const std::string& s, double& out) {
    char* end = nullptr;
    out = std::strtod(s.c_str(), &end);
    return end != nullptr && *end == '\0' && end != s.c_str() && std::isfinite(out);
}

/**
 * @brief Parse and install reaction integration/options rows.
 * @param[in,out] ctx Setup-only reaction metadata and relevant final model name/registry tables;
 * receives parsed rows/options and occasional warnings, no hydraulic water or engine clock
 * advance.
 * @param[in] lines Stable tokenizable source rows, input retained.
 * @param[in,out] errors Appends syntax/name/range diagnostics while continuing later rows;
 * preserves prior errors.
 * @details Selects solver/coupling/rate/area/wall geometry and finite temperature, positive
 * global tolerances and pattern timestep seconds. TIMESTEP permits0, stored but warns when
 * positive because quality step owns integration duration. Fewer than2 tokens silently skip;
 * extra fields ignored; repeated options last-win.
 * @note Allocates strings/vectors, may leave partial rows on ordinary errors or exceptions. No
 * rollback inside this helper; applyReactionSections owns whole-configuration error policy.
 * Exclusive setup access.
 */
void parseOptions(SimulationContext& ctx, const std::vector<std::string>& lines,
                  std::vector<std::string>& errors) {
    auto& rx = ctx.reactions;
    for (const auto& line : lines) {
        auto tok = Tokenizer::tokenize(line);
        if (tok.size() < 2)
            continue;
        const std::string key = Tokenizer::to_upper(tok[0]);
        const std::string val = Tokenizer::to_upper(tok[1]);
        double num = 0.0;
        if (key == "SOLVER") {
            if (val == "EUL")
                rx.solver = ReactionSolverKind::EUL;
            else if (val == "RK5")
                rx.solver = ReactionSolverKind::RK5;
            else if (val == "ROS2")
                rx.solver = ReactionSolverKind::ROS2;
            else if (val == "BDF2")
                rx.solver = ReactionSolverKind::BDF2;
            else
                errors.push_back("[REACTION_OPTIONS] unknown SOLVER '" + tok[1] +
                                 "' (EUL|RK5|ROS2|BDF2).");
        } else if (key == "COUPLING") {
            if (val == "NONE")
                rx.coupling = ReactionCoupling::NONE;
            else if (val == "FULL")
                rx.coupling = ReactionCoupling::FULL;
            else
                errors.push_back("[REACTION_OPTIONS] unknown COUPLING '" + tok[1] +
                                 "' (NONE|FULL).");
        } else if (key == "RATE_UNITS") {
            if (val == "SEC")
                rx.rate_units = ReactionRateUnits::SEC;
            else if (val == "MIN")
                rx.rate_units = ReactionRateUnits::MIN;
            else if (val == "HR")
                rx.rate_units = ReactionRateUnits::HR;
            else if (val == "DAY")
                rx.rate_units = ReactionRateUnits::DAY;
            else
                errors.push_back("[REACTION_OPTIONS] unknown RATE_UNITS '" + tok[1] +
                                 "' (SEC|MIN|HR|DAY).");
        } else if (key == "WALL_GEOMETRY") {
            if (val == "YES")
                rx.wall_geometry = true;
            else if (val == "NO")
                rx.wall_geometry = false;
            else
                errors.push_back("[REACTION_OPTIONS] WALL_GEOMETRY must be YES or NO.");
        } else if (key == "AREA_UNITS") {
            if (val == "FT2")
                rx.area_units = ReactionAreaUnits::FT2;
            else if (val == "M2")
                rx.area_units = ReactionAreaUnits::M2;
            else if (val == "CM2")
                rx.area_units = ReactionAreaUnits::CM2;
            else
                errors.push_back("[REACTION_OPTIONS] unknown AREA_UNITS '" + tok[1] +
                                 "' (FT2|M2|CM2).");
        } else if (key == "TIMESTEP") {
            if (to_num(tok[1], num) && num >= 0.0)
                rx.timestep = num;
            else
                errors.push_back("[REACTION_OPTIONS] bad TIMESTEP '" + tok[1] + "'.");
            // Physics review A3: kept for round trip, read by no binding.
            if (rx.timestep > 0.0)
                ctx.warnings.push_back("[REACTION_OPTIONS] TIMESTEP " + tok[1] +
                                       " is not used: reactions advance with the quality step and "
                                       "choose their own substeps within it.");
        } else if (key == "PATTERN_TIMESTEP") {
            if (to_num(tok[1], num) && num > 0.0)
                rx.pattern_timestep = num;
            else
                errors.push_back("[REACTION_OPTIONS] bad PATTERN_TIMESTEP '" + tok[1] + "'.");
        } else if (key == "ATOL") {
            if (to_num(tok[1], num) && num > 0.0)
                rx.atol = num;
            else
                errors.push_back("[REACTION_OPTIONS] bad ATOL '" + tok[1] + "'.");
        } else if (key == "RTOL") {
            if (to_num(tok[1], num) && num > 0.0)
                rx.rtol = num;
            else
                errors.push_back("[REACTION_OPTIONS] bad RTOL '" + tok[1] + "'.");
        } else if (key == "TEMPERATURE") {
            // TEMP's fallback (degC) for elements without a heat-transport
            // temperature. Any finite value is a temperature; no range gate.
            if (to_num(tok[1], num))
                rx.default_temp_c = num;
            else
                errors.push_back("[REACTION_OPTIONS] bad TEMPERATURE '" + tok[1] + "'.");
        } else {
            errors.push_back("[REACTION_OPTIONS] unknown option '" + tok[0] + "'.");
        }
    }
}

/**
 * @brief Append declared BULK/WALL species and optional per-row tolerances.
 * @param[in,out] ctx Setup-only reaction metadata and relevant final model name/registry tables;
 * receives parsed rows/options and occasional warnings, no hydraulic water or engine clock
 * advance.
 * @param[in] lines Stable tokenizable source rows, input retained.
 * @param[in,out] errors Appends syntax/name/range diagnostics while continuing later rows;
 * preserves prior errors.
 * @details Requires kind/name/units, rejects duplicate species and existing registry name
 * collision. Units copied verbatim, no conversion/validation. Optional finite tolerances can be
 * nonpositive (later fall back to global); bad tolerance appends error after species row
 * insertion. Requires final pollutant registry.
 * @note Allocates strings/vectors, may leave partial rows on ordinary errors or exceptions. No
 * rollback inside this helper; applyReactionSections owns whole-configuration error policy.
 * Exclusive setup access.
 */
void parseSpecies(SimulationContext& ctx, const std::vector<std::string>& lines,
                  std::vector<std::string>& errors) {
    auto& rx = ctx.reactions;
    for (const auto& line : lines) {
        auto tok = Tokenizer::tokenize(line);
        if (tok.empty())
            continue;
        if (tok.size() < 3) {
            errors.push_back("[REACTION_SPECIES] row needs BULK|WALL name "
                             "units [atol rtol]: '" +
                             line + "'.");
            continue;
        }
        const std::string kind = Tokenizer::to_upper(tok[0]);
        const bool is_wall = (kind == "WALL");
        if (!is_wall && kind != "BULK") {
            errors.push_back("[REACTION_SPECIES] kind must be BULK or WALL: '" + tok[0] + "'.");
            continue;
        }
        const std::string& name = tok[1];
        if (rx.find_species(name) >= 0) {
            errors.push_back("[REACTION_SPECIES] duplicate species '" + name + "'.");
            continue;
        }
        // Collision with pollutant (or reserved) names is refused — species
        // names are globally unique (master plan §4.1). STAGED ONLY: the
        // registry commit happens after the whole config validates and
        // compiles (R2 transactional rule — a rejected file must leave no
        // registry entries behind, R1 validation finding).
        if (ctx.species_registry.find(name) >= 0) {
            errors.push_back("[REACTION_SPECIES] '" + name +
                             "' collides with an existing pollutant or "
                             "species name.");
            continue;
        }
        rx.species_name.push_back(name);
        rx.species_is_wall.push_back(is_wall ? 1 : 0);
        rx.species_units.push_back(tok[2]);
        double atol = 0.0, rtol = 0.0;
        if (tok.size() > 3 && !to_num(tok[3], atol))
            errors.push_back("[REACTION_SPECIES] bad atol for '" + name + "'.");
        if (tok.size() > 4 && !to_num(tok[4], rtol))
            errors.push_back("[REACTION_SPECIES] bad rtol for '" + name + "'.");
        rx.species_atol.push_back(atol);
        rx.species_rtol.push_back(rtol);
    }
}

/**
 * @brief Append finite CONSTANT/PARAMETER coefficient declarations.
 * @param[in,out] ctx Setup-only reaction metadata and relevant final model name/registry tables;
 * receives parsed rows/options and occasional warnings, no hydraulic water or engine clock
 * advance.
 * @param[in] lines Stable tokenizable source rows, input retained.
 * @param[in,out] errors Appends syntax/name/range diagnostics while continuing later rows;
 * preserves prior errors.
 * @details Requires kind/name/value, refuses duplicate coefficient or species name. Values
 * retain expression-defined units and may be signed; extra row fields ignored. Terms are parsed
 * later for collision validation.
 * @note Allocates strings/vectors, may leave partial rows on ordinary errors or exceptions. No
 * rollback inside this helper; applyReactionSections owns whole-configuration error policy.
 * Exclusive setup access.
 */
void parseCoefficients(SimulationContext& ctx, const std::vector<std::string>& lines,
                       std::vector<std::string>& errors) {
    auto& rx = ctx.reactions;
    for (const auto& line : lines) {
        auto tok = Tokenizer::tokenize(line);
        if (tok.empty())
            continue;
        if (tok.size() < 3) {
            errors.push_back("[REACTION_COEFFICIENTS] row needs "
                             "PARAMETER|CONSTANT name value: '" +
                             line + "'.");
            continue;
        }
        const std::string kind = Tokenizer::to_upper(tok[0]);
        if (kind != "PARAMETER" && kind != "CONSTANT") {
            errors.push_back("[REACTION_COEFFICIENTS] kind must be PARAMETER "
                             "or CONSTANT: '" +
                             tok[0] + "'.");
            continue;
        }
        if (rx.find_coef(tok[1]) >= 0 || rx.find_species(tok[1]) >= 0) {
            errors.push_back("[REACTION_COEFFICIENTS] name '" + tok[1] +
                             "' duplicates a coefficient or species.");
            continue;
        }
        double v = 0.0;
        if (!to_num(tok[2], v)) {
            errors.push_back("[REACTION_COEFFICIENTS] bad value for '" + tok[1] + "'.");
            continue;
        }
        rx.coef_name.push_back(tok[1]);
        rx.coef_is_param.push_back(kind == "PARAMETER" ? 1 : 0);
        rx.coef_value.push_back(v);
    }
}

/**
 * @brief Append named expression source tails without compiling them.
 * @param[in,out] ctx Setup-only reaction metadata and relevant final model name/registry tables;
 * receives parsed rows/options and occasional warnings, no hydraulic water or engine clock
 * advance.
 * @param[in] lines Stable tokenizable source rows, input retained.
 * @param[in,out] errors Appends syntax/name/range diagnostics while continuing later rows;
 * preserves prior errors.
 * @details Refuses duplicate term/species/coefficient name and missing expression field. Stores
 * original expression payload after name; forward dependency/syntax checks occur during
 * recompile, not here.
 * @note Allocates strings/vectors, may leave partial rows on ordinary errors or exceptions. No
 * rollback inside this helper; applyReactionSections owns whole-configuration error policy.
 * Exclusive setup access.
 */
void parseTerms(SimulationContext& ctx, const std::vector<std::string>& lines,
                std::vector<std::string>& errors) {
    auto& rx = ctx.reactions;
    for (const auto& line : lines) {
        auto tok = Tokenizer::tokenize(line);
        if (tok.empty())
            continue;
        if (tok.size() < 2) {
            errors.push_back("[REACTION_TERMS] row needs name expression: '" + line + "'.");
            continue;
        }
        if (rx.find_term(tok[0]) >= 0 || rx.find_species(tok[0]) >= 0 ||
            rx.find_coef(tok[0]) >= 0) {
            errors.push_back("[REACTION_TERMS] name '" + tok[0] +
                             "' duplicates a term, species, or coefficient.");
            continue;
        }
        rx.term_name.push_back(tok[0]);
        rx.term_expr_src.push_back(expr_payload(line, 1));
    }
}

/**
 * @brief Append deferred scoped groundwater reaction laws.
 * @param[in,out] ctx Setup-only reaction metadata and relevant final model name/registry tables;
 * receives parsed rows/options and occasional warnings, no hydraulic water or engine clock
 * advance.
 * @param[in] lines Stable tokenizable source rows, input retained.
 * @param[in,out] errors Appends syntax/name/range diagnostics while continuing later rows;
 * preserves prior errors.
 * @details Accepts wildcard/TAG/CELL and SAT/UNSAT/LAYER, stores positive integral
 * CELL/LAYER1-based inputs as zero-based indices (up to signed-int limit), resolves BULK species
 * and RATE/EQUIL/FORMULA. Leaves actual tag/cell/layer geometry qualification and compilation
 * for later owner setup; no duplicate-law check.
 * @note Allocates strings/vectors, may leave partial rows on ordinary errors or exceptions. No
 * rollback inside this helper; applyReactionSections owns whole-configuration error policy.
 * Exclusive setup access.
 */
void parseSubsurface(SimulationContext& ctx, const std::vector<std::string>& lines,
                     std::vector<std::string>& errors) {
    auto& rx = ctx.reactions;
    for (const auto& line : lines) {
        const auto tokens = Tokenizer::tokenize(line);
        if (tokens.empty())
            continue;
        const auto fail = [&](const std::string& why) {
            errors.push_back("[REACTION_SUBSURFACE] " + why + " — " + line);
        };
        std::size_t at = 0;
        ReactionData::SubsurfaceLaw law;
        const auto number = [&](int& value) {
            if (at >= tokens.size())
                return false;
            double n = 0;
            if (!to_num(tokens[at++], n) || !std::isfinite(n) || n < 1 || n > 2147483647 ||
                n != std::floor(n))
                return false;
            value = int(n) - 1;
            return true;
        };
        if (tokens[at] == "*")
            ++at;
        else if (Tokenizer::to_upper(tokens[at]) == "TAG") {
            law.scope = 1;
            if (++at >= tokens.size()) {
                fail("TAG needs a name");
                continue;
            }
            law.tag = tokens[at++];
        } else if (Tokenizer::to_upper(tokens[at]) == "CELL") {
            law.scope = 2;
            ++at;
            if (!number(law.cell)) {
                fail("CELL needs a positive index");
                continue;
            }
        } else {
            fail("scope must be *, TAG or CELL");
            continue;
        }
        if (at >= tokens.size()) {
            fail("missing SAT, UNSAT or LAYER");
            continue;
        }
        const auto zone = Tokenizer::to_upper(tokens[at++]);
        if (zone == "SAT")
            law.zone = 0;
        else if (zone == "UNSAT")
            law.zone = 1;
        else if (zone == "LAYER") {
            law.zone = 2;
            if (!number(law.layer)) {
                fail("LAYER needs a positive index");
                continue;
            }
        } else {
            fail("zone must be SAT, UNSAT or LAYER");
            continue;
        }
        if (tokens.size() < at + 3) {
            fail("needs SPECIES RATE|EQUIL|FORMULA EXPRESSION");
            continue;
        }
        law.species = rx.find_species(tokens[at++]);
        if (law.species < 0) {
            fail("species must be a declared MSX BULK species");
            continue;
        }
        if (rx.species_is_wall[law.species]) {
            fail("groundwater WALL species are unsupported");
            continue;
        }
        const auto form = Tokenizer::to_upper(tokens[at++]);
        if (form == "RATE")
            law.form = ReactionExprForm::RATE;
        else if (form == "EQUIL")
            law.form = ReactionExprForm::EQUIL;
        else if (form == "FORMULA")
            law.form = ReactionExprForm::FORMULA;
        else {
            fail("unknown reaction form");
            continue;
        }
        law.expression = expr_payload(line, at);
        rx.subsurface_laws.push_back(std::move(law));
    }
}

/**
 * @brief Populate one reactor's per-species form and retained expression source arrays.
 * @param[in,out] ctx Setup-only reaction metadata and relevant final model name/registry tables;
 * receives parsed rows/options and occasional warnings, no hydraulic water or engine clock
 * advance.
 * @param[in] lines Stable tokenizable source rows, input retained.
 * @param[in,out] errors Appends syntax/name/range diagnostics while continuing later rows;
 * preserves prior errors.
 * @param[in] scope 2 selects subcatchment,1 tank, other values pipe.
 * @details Resizes without clearing already sized entries; duplicate non-NONE target refused.
 * Resolves native species only, diagnoses pollutant targets read-only; stores expression tail
 * after form/species. Syntax, wall/context availability and finite behavior are checked during
 * compilation/integration.
 * @note Allocates strings/vectors, may leave partial rows on ordinary errors or exceptions. No
 * rollback inside this helper; applyReactionSections owns whole-configuration error policy.
 * Exclusive setup access.
 */
void parseExpressions(SimulationContext& ctx, const std::vector<std::string>& lines, int scope,
                      std::vector<std::string>& errors) {
    auto& rx = ctx.reactions;
    const char* sec = scope == 2   ? "[REACTION_SUBCATCHMENTS]"
                      : scope == 1 ? "[REACTION_TANKS]"
                                   : "[REACTION_PIPES]";
    auto& forms = scope == 2 ? rx.subcatch_form : scope == 1 ? rx.tank_form : rx.pipe_form;
    auto& srcs = scope == 2   ? rx.subcatch_expr_src
                 : scope == 1 ? rx.tank_expr_src
                              : rx.pipe_expr_src;
    forms.resize(static_cast<std::size_t>(rx.n_species()), ReactionExprForm::NONE);
    srcs.resize(static_cast<std::size_t>(rx.n_species()));
    for (const auto& line : lines) {
        auto tok = Tokenizer::tokenize(line);
        if (tok.empty())
            continue;
        if (tok.size() < 3) {
            errors.push_back(std::string(sec) +
                             " row needs RATE|EQUIL|FORMULA species "
                             "expression: '" +
                             line + "'.");
            continue;
        }
        const std::string form = Tokenizer::to_upper(tok[0]);
        ReactionExprForm f = ReactionExprForm::NONE;
        if (form == "RATE")
            f = ReactionExprForm::RATE;
        else if (form == "EQUIL")
            f = ReactionExprForm::EQUIL;
        else if (form == "FORMULA")
            f = ReactionExprForm::FORMULA;
        else {
            errors.push_back(std::string(sec) + " unknown form '" + tok[0] +
                             "' (RATE|EQUIL|FORMULA).");
            continue;
        }
        const int s = rx.find_species(tok[1]);
        if (s < 0) {
            if (ctx.species_registry.find(tok[1]) >= 0) {
                errors.push_back(std::string(sec) + " '" + tok[1] +
                                 "' is a pollutant — pollutant kinetics "
                                 "(RATE/EQUIL/FORMULA on a pollutant) arrive "
                                 "with plan phase R4b; pollutants may be "
                                 "REFERENCED read-only in MSX expressions "
                                 "today, and kdecay applies under every "
                                 "engine.");
            } else {
                errors.push_back(std::string(sec) + " undeclared species '" + tok[1] +
                                 "' — declare it in [REACTION_SPECIES].");
            }
            continue;
        }
        const auto us = static_cast<std::size_t>(s);
        if (forms[us] != ReactionExprForm::NONE) {
            errors.push_back(std::string(sec) + " species '" + tok[1] +
                             "' already has an expression in this scope.");
            continue;
        }
        forms[us] = f;
        srcs[us] = expr_payload(line, 2);
    }
}

/**
 * @brief Replace native global/element initial species seeds and mirror owned INITIAL_QUALITY
 * rows.
 * @param[in,out] ctx Setup-only reaction metadata and relevant final model name/registry tables;
 * receives parsed rows/options and occasional warnings, no hydraulic water or engine clock
 * advance.
 * @param[in] lines Stable tokenizable source rows, input retained.
 * @param[in,out] errors Appends syntax/name/range diagnostics while continuing later rows;
 * preserves prior errors.
 * @details Resets all global seeds0 and clears element rows. Accepts finite nonnegative
 * GLOBAL/NODE/LINK values in species declared units; global duplicate last-win, element/species
 * duplicate error. Resolves native names, then mirrors INITIAL_QUALITY species rows with
 * conflict checks. Does not seed evolved runtime arrays.
 * @note Allocates strings/vectors, may leave partial rows on ordinary errors or exceptions. No
 * rollback inside this helper; applyReactionSections owns whole-configuration error policy.
 * Exclusive setup access.
 */
void parseQuality(SimulationContext& ctx, const std::vector<std::string>& lines,
                  std::vector<std::string>& errors) {
    auto& rx = ctx.reactions;
    rx.init_global.assign(static_cast<std::size_t>(rx.n_species()), 0.0);
    rx.init_elem_is_link.clear();
    rx.init_elem_idx.clear();
    rx.init_elem_species.clear();
    rx.init_elem_value.clear();
    for (const auto& line : lines) {
        auto tok = Tokenizer::tokenize(line);
        if (tok.empty())
            continue;
        const std::string scope = Tokenizer::to_upper(tok[0]);

        // E-B1: NODE/LINK per-element rows. Element resolution happens here
        // (D-RQ1): components apply at open, AFTER the full .inp parse, so
        // ctx.node_names/link_names are complete.
        if (scope == "NODE" || scope == "LINK") {
            const bool link = (scope == "LINK");
            if (tok.size() < 4) {
                errors.push_back("[REACTION_QUALITY] row needs " + scope +
                                 " element species value: '" + line + "'.");
                continue;
            }
            const int ei = link ? ctx.link_names.find(tok[1]) : ctx.node_names.find(tok[1]);
            if (ei < 0) {
                errors.push_back(std::string("[REACTION_QUALITY] unknown ") +
                                 (link ? "link" : "node") + " '" + tok[1] + "'.");
                continue;
            }
            const int s = rx.find_species(tok[2]);
            if (s < 0) {
                errors.push_back("[REACTION_QUALITY] undeclared species '" + tok[2] + "'.");
                continue;
            }
            double v = 0.0;
            if (!to_num(tok[3], v) || v < 0.0) {
                errors.push_back("[REACTION_QUALITY] bad value for '" + tok[2] + "'.");
                continue;
            }
            bool dup = false;
            for (std::size_t k = 0; k < rx.init_elem_idx.size(); ++k) {
                if ((rx.init_elem_is_link[k] != 0) == link && rx.init_elem_idx[k] == ei &&
                    rx.init_elem_species[k] == s) {
                    dup = true;
                    break;
                }
            }
            if (dup) {
                errors.push_back("[REACTION_QUALITY] duplicate row for '" + tok[2] + "' at " +
                                 (link ? "link" : "node") + " '" + tok[1] + "'.");
                continue;
            }
            rx.init_elem_is_link.push_back(link ? 1 : 0);
            rx.init_elem_idx.push_back(ei);
            rx.init_elem_species.push_back(s);
            rx.init_elem_value.push_back(v);
            continue;
        }

        if (scope != "GLOBAL") {
            errors.push_back("[REACTION_QUALITY] scope '" + tok[0] +
                             "' is not GLOBAL, NODE, or LINK.");
            continue;
        }
        if (tok.size() < 3) {
            errors.push_back("[REACTION_QUALITY] row needs GLOBAL species "
                             "value: '" +
                             line + "'.");
            continue;
        }
        const int s = rx.find_species(tok[1]);
        if (s < 0) {
            errors.push_back("[REACTION_QUALITY] undeclared species '" + tok[1] + "'.");
            continue;
        }
        double v = 0.0;
        if (!to_num(tok[2], v) || v < 0.0) {
            errors.push_back("[REACTION_QUALITY] bad value for '" + tok[1] + "'.");
            continue;
        }
        rx.init_global[static_cast<std::size_t>(s)] = v;
    }
    // U2: [INITIAL_QUALITY] MSX rows live in ctx.initial_quality (already
    // classified when this is a re-apply; PostParseResolver mirrors them at
    // open). The block reset above emptied init_elem_*, so bring them back.
    for (const auto& e : mirrorInitialQualityMsxRows(ctx))
        errors.push_back(e);
}

/**
 * @brief Append or extend named periodic reaction source multipliers.
 * @param[in,out] ctx Setup-only reaction metadata and relevant final model name/registry tables;
 * receives parsed rows/options and occasional warnings, no hydraulic water or engine clock
 * advance.
 * @param[in] lines Stable tokenizable source rows, input retained.
 * @param[in,out] errors Appends syntax/name/range diagnostics while continuing later rows;
 * preserves prior errors.
 * @details Each repeated pattern name continues its factor list. Appends only finite nonnegative
 * factors, but new pattern may remain empty when all values invalid; errors recorded for each
 * rejected token. Pattern timestep stored separately in seconds. Runtime caller must use
 * successful validated configuration.
 * @note Allocates strings/vectors, may leave partial rows on ordinary errors or exceptions. No
 * rollback inside this helper; applyReactionSections owns whole-configuration error policy.
 * Exclusive setup access.
 */
void parsePatterns(SimulationContext& ctx, const std::vector<std::string>& lines,
                   std::vector<std::string>& errors) {
    auto& rx = ctx.reactions;
    for (const auto& line : lines) {
        const auto tok = Tokenizer::tokenize(line);
        if (tok.empty())
            continue;
        if (tok.size() < 2) {
            errors.push_back("[REACTION_PATTERNS] row needs name factors: '" + line + "'.");
            continue;
        }
        int index = -1;
        for (int i = 0; i < static_cast<int>(rx.source_patterns.size()); ++i)
            if (rx.source_patterns[i].name == tok[0])
                index = i;
        if (index < 0) {
            index = static_cast<int>(rx.source_patterns.size());
            rx.source_patterns.push_back({tok[0], {}});
        }
        for (std::size_t i = 1; i < tok.size(); ++i) {
            double value;
            if (!to_num(tok[i], value) || value < 0.0)
                errors.push_back("[REACTION_PATTERNS] factor must be finite and nonnegative: '" +
                                 tok[i] + "'.");
            else
                rx.source_patterns[index].factors.push_back(value);
        }
    }
}

/**
 * @brief Append resolved native BULK external/booster source definitions.
 * @param[in,out] ctx Setup-only reaction metadata and relevant final model name/registry tables;
 * receives parsed rows/options and occasional warnings, no hydraulic water or engine clock
 * advance.
 * @param[in] lines Stable tokenizable source rows, input retained.
 * @param[in,out] errors Appends syntax/name/range diagnostics while continuing later rows;
 * preserves prior errors.
 * @details Requires exact CONC/MASS/FLOWPACED/SETPOINT node species value [pattern], finite
 * nonnegative strength, known optional pattern (wildcard none), unique node/species; WALL
 * inflows and boosters on virtual splice nodes refused. Concentration kinds retain species
 * units; MASS is source-rate convention, no conversion here. Source evaluation/booster ledger
 * booking occurs in routing, not configuration.
 * @note Allocates strings/vectors, may leave partial rows on ordinary errors or exceptions. No
 * rollback inside this helper; applyReactionSections owns whole-configuration error policy.
 * Exclusive setup access.
 */
void parseSources(SimulationContext& ctx, const std::vector<std::string>& lines,
                  std::vector<std::string>& errors) {
    auto& rx = ctx.reactions;
    for (const auto& line : lines) {
        const auto tok = Tokenizer::tokenize(line);
        if (tok.empty())
            continue;
        const auto kind = Tokenizer::to_upper(tok[0]);
        if ((tok.size() != 4 && tok.size() != 5) ||
            (kind != "CONC" && kind != "MASS" && kind != "FLOWPACED" && kind != "SETPOINT")) {
            errors.push_back(
                "[REACTION_SOURCES] row needs CONC|MASS|FLOWPACED|SETPOINT node species value [pattern]: '" +
                line + "'.");
            continue;
        }
        const int node = ctx.node_names.find(tok[1]), species = rx.find_species(tok[2]);
        double value;
        if (node < 0 || species < 0 || !to_num(tok[3], value) || !std::isfinite(value) ||
            value < 0.0) {
            errors.push_back("[REACTION_SOURCES] unknown node/species or invalid value: '" + line +
                             "'.");
            continue;
        }
        if (rx.species_is_wall[species]) {
            errors.push_back("[REACTION_SOURCES] WALL species cannot enter with inflow water.");
            continue;
        }
        int pattern = -1;
        if (tok.size() == 5 && tok[4] != "*") {
            for (int i = 0; i < static_cast<int>(rx.source_patterns.size()); ++i)
                if (rx.source_patterns[i].name == tok[4])
                    pattern = i;
            if (pattern < 0) {
                errors.push_back("[REACTION_SOURCES] unknown pattern '" + tok[4] + "'.");
                continue;
            }
        }
        bool duplicate = false;
        for (const auto& row : rx.sources)
            duplicate |= row.node == node && row.species == species;
        if (duplicate) {
            errors.push_back("[REACTION_SOURCES] duplicate node/species: '" + line + "'.");
            continue;
        }
        if ((kind == "FLOWPACED" || kind == "SETPOINT") &&
            node < int(ctx.nodes.is_virtual.size()) && ctx.nodes.is_virtual[node]) {
            errors.push_back(
                "[REACTION_SOURCES] boosters require a physical mixing node, not a virtual splice.");
            continue;
        }
        rx.sources.push_back({kind == "MASS", node, species, pattern, value,
                              kind == "FLOWPACED"  ? 1
                              : kind == "SETPOINT" ? 2
                                                   : 0});
    }
}

/**
 * @brief Append unique element-specific PARAMETER coefficient overrides.
 * @param[in,out] ctx Setup-only reaction metadata and relevant final model name/registry tables;
 * receives parsed rows/options and occasional warnings, no hydraulic water or engine clock
 * advance.
 * @param[in] lines Stable tokenizable source rows, input retained.
 * @param[in,out] errors Appends syntax/name/range diagnostics while continuing later rows;
 * preserves prior errors.
 * @details Exact NODE/LINK element parameter finite-value rows, resolves element/coefficient and
 * rejects CONSTANT or duplicate element/parameter. Values retain expression-defined units,
 * signed permitted; no runtime state changes or dense cache resolution here.
 * @note Allocates strings/vectors, may leave partial rows on ordinary errors or exceptions. No
 * rollback inside this helper; applyReactionSections owns whole-configuration error policy.
 * Exclusive setup access.
 */
void parseParameters(SimulationContext& ctx, const std::vector<std::string>& lines,
                     std::vector<std::string>& errors) {
    auto& rx = ctx.reactions;
    for (const auto& line : lines) {
        const auto tok = Tokenizer::tokenize(line);
        if (tok.empty())
            continue;
        const auto scope = Tokenizer::to_upper(tok[0]);
        if (tok.size() != 4 || (scope != "NODE" && scope != "LINK")) {
            errors.push_back(
                "[REACTION_PARAMETERS] row needs NODE|LINK element parameter value: '" + line +
                "'.");
            continue;
        }
        const bool link = scope == "LINK";
        const int element = link ? ctx.link_names.find(tok[1]) : ctx.node_names.find(tok[1]);
        const int coefficient = rx.find_coef(tok[2]);
        double value = 0.0;
        if (element < 0 || coefficient < 0 || !to_num(tok[3], value)) {
            errors.push_back(
                "[REACTION_PARAMETERS] unknown element/parameter or non-finite value: '" + line +
                "'.");
            continue;
        }
        if (!rx.coef_is_param[coefficient]) {
            errors.push_back("[REACTION_PARAMETERS] '" + tok[2] +
                             "' is a CONSTANT, not a PARAMETER.");
            continue;
        }
        bool duplicate = false;
        for (const auto& row : rx.parameter_overrides)
            duplicate |=
                row.link == link && row.element == element && row.coefficient == coefficient;
        if (duplicate) {
            errors.push_back("[REACTION_PARAMETERS] duplicate element/parameter: '" + line + "'.");
            continue;
        }
        rx.parameter_overrides.push_back({link, element, coefficient, value});
    }
}

/**
 * @brief Reset reporting mask to enabled and apply ALL/species YES/NO rows.
 * @param[in,out] ctx Setup-only reaction metadata and relevant final model name/registry tables;
 * receives parsed rows/options and occasional warnings, no hydraulic water or engine clock
 * advance.
 * @param[in] lines Stable tokenizable source rows, input retained.
 * @param[in,out] errors Appends syntax/name/range diagnostics while continuing later rows;
 * preserves prior errors.
 * @details Exact3-token rows, ALL must precede named species; duplicates append errors. Even
 * invalid repeated/late ALL still fills mask before outer error policy clears configuration.
 * Does not control physical transport.
 * @note Allocates strings/vectors, may leave partial rows on ordinary errors or exceptions. No
 * rollback inside this helper; applyReactionSections owns whole-configuration error policy.
 * Exclusive setup access.
 */
void parseReport(SimulationContext& ctx, const std::vector<std::string>& lines,
                 std::vector<std::string>& errors) {
    auto& rx = ctx.reactions;
    rx.report_enabled.assign(static_cast<std::size_t>(rx.n_species()), 1);
    std::vector<bool> seen(rx.report_enabled.size(), false);
    bool all_seen = false;
    for (const auto& line : lines) {
        const auto tok = Tokenizer::tokenize(line);
        if (tok.empty())
            continue;
        if (tok.size() != 3 || Tokenizer::to_upper(tok[0]) != "SPECIES" ||
            (Tokenizer::to_upper(tok[2]) != "YES" && Tokenizer::to_upper(tok[2]) != "NO")) {
            errors.push_back("[REACTION_REPORT] row needs SPECIES ALL|species YES|NO: '" + line +
                             "'.");
            continue;
        }
        const bool value = Tokenizer::to_upper(tok[2]) == "YES";
        if (Tokenizer::to_upper(tok[1]) == "ALL") {
            if (all_seen)
                errors.push_back("[REACTION_REPORT] duplicate ALL row.");
            // ALL is a baseline; require it before named overrides.
            bool named = false;
            for (const auto v : seen)
                named |= v;
            if (named)
                errors.push_back("[REACTION_REPORT] ALL must precede named species rows.");
            std::fill(rx.report_enabled.begin(), rx.report_enabled.end(), value);
            all_seen = true;
        } else {
            const int m = rx.find_species(tok[1]);
            if (m < 0)
                errors.push_back("[REACTION_REPORT] undeclared species '" + tok[1] + "'.");
            else if (seen[m])
                errors.push_back("[REACTION_REPORT] duplicate species '" + tok[1] + "'.");
            else {
                rx.report_enabled[m] = value;
                seen[m] = true;
            }
        }
    }
}

/**
 * @brief [REACTION_RAINFALL] (Multispecies Phase 2): `species concentration`, the species'
 * counterpart of the [POLLUTANTS] Crain column.
 *
 * @details Install finite nonnegative rainfall species concentrations into pre-sized rows.
 * @param[in,out] ctx Setup-only reaction metadata and relevant final model name/registry tables;
 * receives parsed rows/options and occasional warnings, no hydraulic water or engine clock
 * advance.
 * @param[in] lines Stable tokenizable source rows, input retained.
 * @param[in,out] errors Appends syntax/name/range diagnostics while continuing later rows;
 * preserves prior errors.
 * @details Known native species, at most one row per species within this call; extra fields
 * ignored. Does not reject WALL at this parse step or clear prior rain values, so caller
 * prepares zeroed n_species array. Units remain declared concentration units; wet deposition and
 * store activation occur later.
 * @note Allocates strings/vectors, may leave partial rows on ordinary errors or exceptions. No
 * rollback inside this helper; applyReactionSections owns whole-configuration error policy.
 * Exclusive setup access.
 */
void parseRainfall(SimulationContext& ctx, const std::vector<std::string>& lines,
                   std::vector<std::string>& errors) {
    auto& rx = ctx.reactions;
    std::vector<bool> seen(static_cast<std::size_t>(rx.n_species()), false);
    for (const auto& line : lines) {
        auto tok = Tokenizer::tokenize(line);
        if (tok.empty())
            continue;
        if (tok.size() < 2) {
            errors.push_back("[REACTION_RAINFALL] row needs species concentration: '" + line +
                             "'.");
            continue;
        }
        const int m = rx.find_species(tok[0]);
        if (m < 0) {
            errors.push_back("[REACTION_RAINFALL] undeclared species '" + tok[0] + "'.");
            continue;
        }
        double v = 0.0;
        if (!to_num(tok[1], v) || v < 0.0) {
            errors.push_back("[REACTION_RAINFALL] bad concentration '" + tok[1] + "' for '" +
                             tok[0] + "'.");
            continue;
        }
        if (seen[static_cast<std::size_t>(m)]) {
            errors.push_back("[REACTION_RAINFALL] duplicate row for '" + tok[0] + "'.");
            continue;
        }
        seen[static_cast<std::size_t>(m)] = true;
        rx.rain_conc[static_cast<std::size_t>(m)] = v;
    }
}

} // namespace

/**
 * @brief Borrow the complete fixed set of supported embedded/external reaction section names.
 * @return Const static owning-vector reference, process lifetime; caller must not mutate/retain
 * mutable aliases. First initialization allocates and may throw; subsequent access no
 * model/clock change.
 */
const std::vector<std::string>& reactionSectionTags() {
    static const std::vector<std::string> tags = {
        "REACTION_OPTIONS",  "REACTION_SPECIES",    "REACTION_COEFFICIENTS",
        "REACTION_TERMS",    "REACTION_PIPES",      "REACTION_TANKS",
        "REACTION_SOURCES",  "REACTION_QUALITY",    "REACTION_PARAMETERS",
        "REACTION_PATTERNS", "REACTION_REPORT",     "REACTION_SUBCATCHMENTS",
        "REACTION_RAINFALL", "REACTION_SUBSURFACE", "REACTION_WALLS",
        "REACTION_LIDS",     "REACTION_BUILDUP"};
    return tags;
}

/**
 * @brief Parse, validate, compile and register one native multispecies reaction system.
 * @param[in,out] ctx Final native node/link/subcatch/landuse/LID names and pollutant registry;
 * replaces reaction metadata, installs compiled tokens/spans and successful native registry
 * species.
 * @param[in] config Supported REACTION_* sections and source path used in diagnostics; read
 * synchronously.
 * @param[in,out] errors Appends parse/profile/compile diagnostics; any nonempty vector at final
 * parse check (including preexisting caller errors) clears reaction metadata and prevents
 * registration.
 * @note Already configured refuses duplicate without clearing existing state. Otherwise clears
 * at entry, parses declarations before scoped laws/seeds/patterns/sources/walls/LIDs, then
 * rejects LAGRANGIAN WALL profile. Requires at least one species; zero-species early return can
 * retain partially parsed options. Final parse/compile refusal clears reaction metadata;
 * warnings remain. Successful registry insertion can allocate/throw after earlier species
 * registrations, no transactional rollback. Does not seed evolved runtime species arrays,
 * resolve hydraulic ownership or advance physical water/clock. Exclusive startup, matching name
 * tables and correctly built parallel model arrays required.
 */
void applyReactionSections(SimulationContext& ctx,
                           const components::ComponentConfigSections& config,
                           std::vector<std::string>& errors) {
    if (ctx.reactions.configured) {
        errors.push_back("Reactions system configured twice — duplicate registration or "
                         "embedded sections alongside an external component file.");
        return;
    }
    ctx.reactions.clear();

    // Order matters: species before coefficients/terms/expressions so
    // references validate. Sections may appear in any file order.
    const auto* s = config.find("REACTION_OPTIONS");
    if (s)
        parseOptions(ctx, *s, errors);
    s = config.find("REACTION_SPECIES");
    if (s)
        parseSpecies(ctx, *s, errors);
    if (ctx.reactions.n_species() == 0) {
        errors.push_back("Reactions config declares no [REACTION_SPECIES] — "
                         "at least one species is required.");
        return;
    }
    s = config.find("REACTION_COEFFICIENTS");
    if (s)
        parseCoefficients(ctx, *s, errors);
    s = config.find("REACTION_TERMS");
    if (s)
        parseTerms(ctx, *s, errors);
    s = config.find("REACTION_PIPES");
    if (s)
        parseExpressions(ctx, *s, /*pipe scope=*/0, errors);
    else {
        ctx.reactions.pipe_form.assign(static_cast<std::size_t>(ctx.reactions.n_species()),
                                       ReactionExprForm::NONE);
        ctx.reactions.pipe_expr_src.resize(static_cast<std::size_t>(ctx.reactions.n_species()));
    }
    s = config.find("REACTION_TANKS");
    if (s)
        parseExpressions(ctx, *s, /*tank scope=*/1, errors);
    else {
        ctx.reactions.tank_form.assign(static_cast<std::size_t>(ctx.reactions.n_species()),
                                       ReactionExprForm::NONE);
        ctx.reactions.tank_expr_src.resize(static_cast<std::size_t>(ctx.reactions.n_species()));
    }
    ctx.reactions.subcatch_form.assign(static_cast<std::size_t>(ctx.reactions.n_species()),
                                       ReactionExprForm::NONE);
    ctx.reactions.subcatch_expr_src.resize(static_cast<std::size_t>(ctx.reactions.n_species()));
    s = config.find("REACTION_SUBCATCHMENTS");
    if (s) {
        std::vector<std::string> global;
        for (const auto& line : *s) {
            auto t = Tokenizer::tokenize(line);
            if (t.empty())
                continue;
            if (Tokenizer::to_upper(t[0]) != "SUBCATCHMENT") {
                global.push_back(line);
                continue;
            }
            if (t.size() < 5) {
                errors.push_back(
                    "[REACTION_SUBCATCHMENTS] needs SUBCATCHMENT name form species expression.");
                continue;
            }
            ReactionData::SurfaceLaw law;
            law.subcatchment = ctx.subcatch_names.find(t[1]);
            law.species = ctx.reactions.find_species(t[3]);
            auto form = Tokenizer::to_upper(t[2]);
            law.form = form == "RATE"      ? ReactionExprForm::RATE
                       : form == "EQUIL"   ? ReactionExprForm::EQUIL
                       : form == "FORMULA" ? ReactionExprForm::FORMULA
                                           : ReactionExprForm::NONE;
            if (law.subcatchment < 0 || law.species < 0 || law.form == ReactionExprForm::NONE) {
                errors.push_back("[REACTION_SUBCATCHMENTS] invalid selector, species or form: " +
                                 line);
                continue;
            }
            if (std::any_of(ctx.reactions.wet_laws.begin(), ctx.reactions.wet_laws.end(),
                            [&](const auto& l) {
                                return l.subcatchment == law.subcatchment &&
                                       l.species == law.species;
                            })) {
                errors.push_back("[REACTION_SUBCATCHMENTS] duplicate selected law.");
                continue;
            }
            law.expression = expr_payload(line, 4);
            ctx.reactions.wet_laws.push_back(std::move(law));
        }
        parseExpressions(ctx, global, 2, errors);
    }
    s = config.find("REACTION_BUILDUP");
    if (s)
        for (const auto& line : *s) {
            auto t = Tokenizer::tokenize(line);
            if (t.empty())
                continue;
            ReactionData::SurfaceLaw law;
            std::size_t pos = 0;
            auto fail = [&] {
                errors.push_back(
                    "[REACTION_BUILDUP] needs [SUBCATCHMENT name] LANDUSE name RATE|EQUIL|FORMULA BULK-species expression: " +
                    line);
            };
            if (Tokenizer::to_upper(t[0]) == "SUBCATCHMENT") {
                if (t.size() < 7 || (law.subcatchment = ctx.subcatch_names.find(t[1])) < 0) {
                    fail();
                    continue;
                }
                pos = 2;
            }
            if (t.size() < pos + 5 || Tokenizer::to_upper(t[pos]) != "LANDUSE" ||
                (law.landuse = ctx.landuse_names.find(t[pos + 1])) < 0) {
                fail();
                continue;
            }
            auto form = Tokenizer::to_upper(t[pos + 2]);
            law.form = form == "RATE"      ? ReactionExprForm::RATE
                       : form == "EQUIL"   ? ReactionExprForm::EQUIL
                       : form == "FORMULA" ? ReactionExprForm::FORMULA
                                           : ReactionExprForm::NONE;
            law.species = ctx.reactions.find_species(t[pos + 3]);
            if (law.species < 0 || law.form == ReactionExprForm::NONE) {
                fail();
                continue;
            }
            if (std::any_of(ctx.reactions.buildup_laws.begin(), ctx.reactions.buildup_laws.end(),
                            [&](const auto& l) {
                                return l.subcatchment == law.subcatchment &&
                                       l.landuse == law.landuse && l.species == law.species;
                            })) {
                errors.push_back("[REACTION_BUILDUP] duplicate selected law.");
                continue;
            }
            law.expression = expr_payload(line, int(pos + 4));
            ctx.reactions.buildup_laws.push_back(std::move(law));
        }
    s = config.find("REACTION_SUBSURFACE");
    if (s)
        parseSubsurface(ctx, *s, errors);
    s = config.find("REACTION_QUALITY");
    if (s)
        parseQuality(ctx, *s, errors);
    else
        ctx.reactions.init_global.assign(static_cast<std::size_t>(ctx.reactions.n_species()), 0.0);
    ctx.reactions.rain_conc.assign(static_cast<std::size_t>(ctx.reactions.n_species()), 0.0);
    s = config.find("REACTION_RAINFALL");
    if (s)
        parseRainfall(ctx, *s, errors);

    s = config.find("REACTION_PATTERNS");
    if (s)
        parsePatterns(ctx, *s, errors);
    s = config.find("REACTION_SOURCES");
    if (s)
        parseSources(ctx, *s, errors);
    s = config.find("REACTION_PARAMETERS");
    if (s)
        parseParameters(ctx, *s, errors);
    s = config.find("REACTION_REPORT");
    if (s)
        parseReport(ctx, *s, errors);
    s = config.find("REACTION_WALLS");
    if (s)
        for (const auto& line : *s) {
            const auto tok = Tokenizer::tokenize(line);
            if (tok.empty())
                continue;
            double area = 0;
            const bool link = Tokenizer::to_upper(tok[0]) == "LINK";
            const int e = tok.size() == 3
                              ? (link ? ctx.link_names.find(tok[1]) : ctx.node_names.find(tok[1]))
                              : -1;
            if (tok.size() != 3 || (!link && Tokenizer::to_upper(tok[0]) != "NODE") || e < 0 ||
                !to_num(tok[2], area) || !std::isfinite(area) || area < 0) {
                errors.push_back("[REACTION_WALLS] needs NODE|LINK element nonnegative-area: '" +
                                 line + "'.");
                continue;
            }
            auto& rows = ctx.reactions.wall_areas;
            if (std::any_of(rows.begin(), rows.end(),
                            [&](const auto& r) { return r.link == link && r.element == e; }))
                errors.push_back("[REACTION_WALLS] duplicate contact area: '" + line + "'.");
            else
                rows.push_back({link, e, area});
        }

    s = config.find("REACTION_LIDS");
    if (s)
        for (const auto& line : *s) {
            const auto t = Tokenizer::tokenize(line);
            if (t.empty())
                continue;
            auto fail = [&](const std::string& why) {
                errors.push_back("[REACTION_LIDS] " + why + ": '" + line + "'.");
            };
            if (t.size() < 3 || Tokenizer::to_upper(t[0]) != "CONTROL") {
                fail(
                    "needs CONTROL name TRANSPORT or CONTROL name LAYER number RATE|EQUIL|FORMULA species expression");
                continue;
            }
            const int control = ctx.lid_names.find(t[1]);
            if (control < 0) {
                fail("unknown LID control");
                continue;
            }
            if (!ctx.reactions.lid_enabled(control))
                ctx.reactions.lid_controls.push_back(control);
            if (t.size() == 3 && Tokenizer::to_upper(t[2]) == "TRANSPORT")
                continue;
            double number = 0;
            if (t.size() < 7 || Tokenizer::to_upper(t[2]) != "LAYER" || !to_num(t[3], number) ||
                number < 0 || number > 10000 || number != std::floor(number)) {
                fail("invalid layer (0 means every layer)");
                continue;
            }
            ReactionData::LidLaw law;
            law.control = control;
            law.layer = int(number);
            law.species = ctx.reactions.find_species(t[5]);
            const auto f = Tokenizer::to_upper(t[4]);
            if (f == "RATE")
                law.form = ReactionExprForm::RATE;
            else if (f == "EQUIL")
                law.form = ReactionExprForm::EQUIL;
            else if (f == "FORMULA")
                law.form = ReactionExprForm::FORMULA;
            else {
                fail("unknown form");
                continue;
            }
            if (law.species < 0 || ctx.reactions.species_is_wall[law.species]) {
                fail("target must be BULK");
                continue;
            }
            if (std::any_of(ctx.reactions.lid_laws.begin(), ctx.reactions.lid_laws.end(),
                            [&](const auto& r) {
                                return r.control == control && r.layer == law.layer &&
                                       r.species == law.species;
                            })) {
                fail("duplicate layer/species law");
                continue;
            }
            law.expression = expr_payload(line, 6);
            ctx.reactions.lid_laws.push_back(std::move(law));
        }
    if (!ctx.reactions.wall_geometry && ctx.reactions.buildup_laws.empty() &&
        ctx.reactions.area_units != ReactionAreaUnits::FT2)
        ctx.warnings.push_back(std::string("[REACTION_OPTIONS] AREA_UNITS ") +
                               (ctx.reactions.area_units == ReactionAreaUnits::M2 ? "M2" : "CM2") +
                               " has no effect unless WALL_GEOMETRY YES is selected.");

    // Unknown REACTION_* sections in the file are typos worth failing on.
    for (const auto& sect : config.sections) {
        bool known = false;
        for (const auto& t : reactionSectionTags())
            if (sect.first == t) {
                known = true;
                break;
            }
        if (!known)
            errors.push_back("Unknown section [" + sect.first + "] in reactions config '" +
                             config.source_path + "'.");
    }

    if (ctx.options.quality_solver == QualitySolverKind::LAGRANGIAN &&
        std::any_of(ctx.reactions.species_is_wall.begin(), ctx.reactions.species_is_wall.end(),
                    [](uint8_t wall) { return wall != 0; }))
        errors.push_back(
            "LAGRANGIAN reaction transport does not support attached WALL species; use QUALITY_SOLVER LEGACY.");

    if (!errors.empty()) {
        // Transactional: a rejected config leaves NOTHING behind — neither
        // reaction state nor registry entries (nothing was committed yet).
        ctx.reactions.clear();
        return;
    }

    // ---- R2: compile every expression into the flat token pool. --------
    if (!recompileReactionSystem(ctx, errors)) {
        ctx.reactions.clear();
        return;
    }
    auto& rx2 = ctx.reactions;

    // ---- Commit: registry entries only now, after full success. --------
    for (int sidx = 0; sidx < rx2.n_species(); ++sidx) {
        const auto us = static_cast<std::size_t>(sidx);
        const int reg = ctx.species_registry.add(rx2.species_name[us],
                                                 rx2.species_is_wall[us] ? SpeciesKind::MSX_WALL
                                                                         : SpeciesKind::MSX_BULK,
                                                 rx2.species_units[us]);
        if (rx2.registry_base < 0)
            rx2.registry_base = reg;
    }
    rx2.compiled = true;
    rx2.configured = true;
}

/**
 * @brief Apply retained INP reaction sections unless an external component has precedence.
 * @param[in,out] ctx Embedded component rows and final reaction/name state, warnings/config
 * updated.
 * @param[in] external_component_registered True warns/ignores embedded sections; external owner
 * wins.
 * @param[in,out] errors Passed to applyReactionSections, append-only except underlying reaction
 * metadata reset.
 * @note No embedded rows returns. Otherwise appends style/precedence warning each invocation;
 * not once-latched. Builds copied synthetic section set then parses/compiles. Allocation can
 * leave warnings or partial metadata; no water/clock advance, exclusive initial configuration
 * chronology.
 */
void applyEmbeddedReactionSections(SimulationContext& ctx, bool external_component_registered,
                                   std::vector<std::string>& errors) {
    if (ctx.embedded_component_sections.empty())
        return;

    if (external_component_registered) {
        ctx.warnings.push_back("Embedded [REACTION_*] sections in the .inp are IGNORED because "
                               "an external reactions component is registered in "
                               "[PROCESS_COMPONENTS] — the external config file wins "
                               "(TRANSPORT_IO_PLUGIN_CONFIG_PLAN §3.2).");
        return;
    }

    ctx.warnings.push_back("Embedded [REACTION_*] sections found in the .inp. This works, but "
                           "the clean layout is an external component config file registered "
                           "via [PROCESS_COMPONENTS] (config=\"model.rxn\") — see "
                           "TRANSPORT_IO_PLUGIN_CONFIG_PLAN.md (style warning, D-UT8).");

    components::ComponentConfigSections synth;
    synth.source_path = "(embedded in .inp)";
    synth.sections = ctx.embedded_component_sections;
    applyReactionSections(ctx, synth, errors);
}

/**
 * @brief Register native reaction parser/serializer callbacks under the built-in component ID.
 * @note Installs org.hydrocouple.openswmm.reactions in process singleton; duplicate policy is
 * registry-owned. Does not instantiate a reaction system or advance clock. Registration may
 * allocate/throw and must be serialized during startup before process component
 * lookup/application.
 */
void registerReactionsComponent() {
    components::ProcessComponentRegistry::instance().register_component(
        kComponentId, "Multispecies reaction system (EPANET-MSX conventions)",
        [](SimulationContext& ctx, const ProcessComponentSpec& /*spec*/,
           const components::ComponentConfigSections& config,
           std::vector<std::string>& errors) { applyReactionSections(ctx, config, errors); },
        // IO3a: the canonical .rxn serializer already existed (E-C3,
        // ReactionsWriter) and was reachable only through the C API's
        // get-text call. Nothing ever asked it to write the file on save, so
        // reaction edits made through the API or the GUI were lost exactly
        // like heat's. The capability was present; the CALL SITE was missing.
        [](const SimulationContext& ctx, const ProcessComponentSpec& /*spec*/) -> std::string {
            return transport::serializeReactionSystem(ctx);
        });
}

// ============================================================================
// recompileReactionSystem() — the one compile path (E-C2 / D-RC4)
// ============================================================================

/**
 * @brief Rebuild all compiled reaction expressions and validate scoped availability/profile
 * constraints.
 * @param[in,out] ctx Internally consistent species/coefficient/term/source form arrays and final
 * native geometry/control/name tables; clears compiled flag/token pool and replaces spans before
 * compilation.
 * @param[in,out] errors Appends qualified source/column diagnostics; prior errors alone do not
 * make this routine fail (success tests only newly appended count).
 * @return True and compiled=true when no new errors; false with partial rebuilt token pool/spans
 * and compiled=false. Does not restore previous compiled state on refusal.
 * @note Compiles terms in declaration order allowing earlier terms only; law contexts enforce
 * readable hydraulic/pollutant/BULK references. Checks LID layer count, wall geometry+FULL
 * coupling, required storage-node wall areas and RATE/NONE WALL forms. Trusts prepared parallel
 * source/form extents; not a general geometry/index validator. Does not reseed inventories,
 * refresh existing workspaces, book physical mass or advance clock. Token relocation invalidates
 * prior spans/views/callers; exclusive configuration barrier required. Allocation/diagnostic
 * errors propagate with partial metadata retained.
 */
bool recompileReactionSystem(SimulationContext& ctx, std::vector<std::string>& errors) {
    auto& rx = ctx.reactions;
    rx.compiled = false;
    rx.token_pool.clear();
    rx.term_expr.assign(rx.term_name.size(), RxExprSpan{});
    rx.pipe_expr.assign(static_cast<std::size_t>(rx.n_species()), RxExprSpan{});
    rx.tank_expr.assign(static_cast<std::size_t>(rx.n_species()), RxExprSpan{});

    rx.subcatch_form.resize(static_cast<std::size_t>(rx.n_species()), ReactionExprForm::NONE);
    rx.subcatch_expr_src.resize(static_cast<std::size_t>(rx.n_species()));
    rx.subcatch_expr.assign(static_cast<std::size_t>(rx.n_species()), RxExprSpan{});
    // R4: pollutants are referencable (read-only) in expressions. Build the
    // name list in registry order (pollutants occupy the first registry
    // slots; PUSH_POLLUT idx == pollutant index).
    std::vector<std::string> pollutant_names;
    for (int p2 = 0; p2 < ctx.species_registry.pollutant_count(); ++p2)
        pollutant_names.push_back(ctx.species_registry.name(p2));

    RxSymbols sym;
    sym.species = &rx.species_name;
    sym.coefs = &rx.coef_name;
    sym.terms = &rx.term_name;
    sym.pollutants = &pollutant_names;

    const std::size_t n_errors_in = errors.size();
    auto compile_one = [&](const std::string& src_expr, RxExprSpan& span, const std::string& where,
                           int context = 0) {
        int col = 0;
        std::string err = compileReactionExpression(src_expr, sym, rx.token_pool, span, col);
        // Physics review A1: refuse what would silently read zero.
        if (err.empty())
            err = reactionContextDiagnostic(rx, rx.token_pool, span, context);
        if (!err.empty())
            errors.push_back(where + " (col " + std::to_string(col) + "): " + err + ".");
    };

    for (std::size_t i = 0; i < rx.term_name.size(); ++i) {
        sym.max_term = static_cast<int>(i); // forward-only rule
        compile_one(rx.term_expr_src[i], rx.term_expr[i],
                    "[REACTION_TERMS] '" + rx.term_name[i] + "'", 1);
    }
    sym.max_term = static_cast<int>(rx.term_name.size());
    for (int sidx = 0; sidx < rx.n_species(); ++sidx) {
        const auto us = static_cast<std::size_t>(sidx);
        if (rx.pipe_form[us] != ReactionExprForm::NONE)
            compile_one(rx.pipe_expr_src[us], rx.pipe_expr[us],
                        "[REACTION_PIPES] '" + rx.species_name[us] + "'");
        if (rx.tank_form[us] != ReactionExprForm::NONE)
            compile_one(rx.tank_expr_src[us], rx.tank_expr[us],
                        "[REACTION_TANKS] '" + rx.species_name[us] + "'");
        if (rx.subcatch_form[us] != ReactionExprForm::NONE) {
            if (rx.species_is_wall[us])
                errors.push_back("[REACTION_SUBCATCHMENTS] requires a BULK species.");
            else
                compile_one(rx.subcatch_expr_src[us], rx.subcatch_expr[us],
                            "[REACTION_SUBCATCHMENTS] '" + rx.species_name[us] + "'", 3);
        }
    }
    for (auto& law : rx.subsurface_laws) {
        if (law.species < 0 || law.species >= rx.n_species() || rx.species_is_wall[law.species]) {
            errors.push_back("[REACTION_SUBSURFACE] requires a valid BULK species");
            continue;
        }
        compile_one(law.expression, law.compiled,
                    "[REACTION_SUBSURFACE] '" + rx.species_name[law.species] + "'", 2);
    }

    for (auto* laws : {&rx.wet_laws, &rx.buildup_laws})
        for (auto& law : *laws) {
            if (law.species < 0 || law.species >= rx.n_species() ||
                rx.species_is_wall[law.species]) {
                errors.push_back("Surface reactions require a BULK species.");
                continue;
            }
            compile_one(law.expression, law.compiled,
                        "Surface reaction '" + rx.species_name[law.species] + "'", 3);
        }
    for (auto& law : rx.lid_laws) {
        if (law.species < 0 || law.species >= rx.n_species() || rx.species_is_wall[law.species]) {
            errors.push_back("[REACTION_LIDS] target must be BULK.");
            continue;
        }
        compile_one(law.expression, law.compiled,
                    "[REACTION_LIDS] '" + rx.species_name[law.species] + "'", 4);
        const bool node = law.control >= 0 &&
                          law.control < int(ctx.lid_controls.node_layers.size()) &&
                          !ctx.lid_controls.node_layers[law.control].empty();
        const int count = node ? int(ctx.lid_controls.node_layers[law.control].size()) : 4;
        if (law.layer > count)
            errors.push_back("[REACTION_LIDS] layer exceeds the control's stack.");
    }
    if (!rx.wall_geometry && !rx.wall_areas.empty())
        errors.push_back("[REACTION_WALLS] requires WALL_GEOMETRY YES.");
    if (rx.wall_geometry)
        for (int s = 0; s < rx.n_species(); ++s)
            if (rx.species_is_wall[s]) {
                if (rx.coupling != ReactionCoupling::FULL)
                    errors.push_back("WALL_GEOMETRY requires COUPLING FULL.");
                if (rx.tank_form[s] != ReactionExprForm::NONE)
                    for (int node = 0; node < ctx.n_nodes(); ++node)
                        if (ctx.nodes.type[node] == NodeType::STORAGE &&
                            std::none_of(
                                rx.wall_areas.begin(), rx.wall_areas.end(),
                                [&](const auto& r) { return !r.link && r.element == node; }))
                            errors.push_back(
                                "WALL_GEOMETRY tank reactions require an explicit [REACTION_WALLS] NODE area for '" +
                                ctx.node_names.name_of(node) + "'.");
                for (auto form : {rx.pipe_form[s], rx.tank_form[s]})
                    if (form == ReactionExprForm::EQUIL || form == ReactionExprForm::FORMULA)
                        errors.push_back(
                            "WALL_GEOMETRY supports RATE or NONE for WALL species; algebraic WALL projections have no conservative wet/dry transfer.");
            }
    if (errors.size() != n_errors_in)
        return false;
    rx.compiled = true;
    return true;
}

} // namespace openswmm::transport
