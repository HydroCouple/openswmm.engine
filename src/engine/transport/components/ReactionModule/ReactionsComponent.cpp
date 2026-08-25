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
 * @file ReactionsComponent.cpp
 * @brief Reactions process component — phase R1 body.
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "ReactionsComponent.hpp"

#include <cstdlib>

#include "../../../core/SimulationContext.hpp"
#include "../../../input/Tokenizer.hpp"
#include "../../../plugins/ProcessComponentRegistry.hpp"
#include "ReactionExpression.hpp"

namespace openswmm::transport {

namespace {

using input::Tokenizer;

constexpr const char* kComponentId = "org.hydrocouple.openswmm.reactions";

/// Rejoin tokens [from, end) with single spaces — expression bodies span the
/// rest of their row.
std::string rejoin(const std::vector<std::string>& tok, std::size_t from) {
    std::string s;
    for (std::size_t i = from; i < tok.size(); ++i) {
        if (!s.empty()) s += ' ';
        s += tok[i];
    }
    return s;
}

bool to_num(const std::string& s, double& out) {
    char* end = nullptr;
    out = std::strtod(s.c_str(), &end);
    return end != nullptr && *end == '\0' && end != s.c_str();
}

void parseOptions(SimulationContext& ctx, const std::vector<std::string>& lines,
                  std::vector<std::string>& errors) {
    auto& rx = ctx.reactions;
    for (const auto& line : lines) {
        auto tok = Tokenizer::tokenize(line);
        if (tok.size() < 2) continue;
        const std::string key = Tokenizer::to_upper(tok[0]);
        const std::string val = Tokenizer::to_upper(tok[1]);
        double num = 0.0;
        if (key == "SOLVER") {
            if      (val == "EUL")  rx.solver = ReactionSolverKind::EUL;
            else if (val == "RK5")  rx.solver = ReactionSolverKind::RK5;
            else if (val == "ROS2") rx.solver = ReactionSolverKind::ROS2;
            else if (val == "BDF2") rx.solver = ReactionSolverKind::BDF2;
            else errors.push_back("[REACTION_OPTIONS] unknown SOLVER '" +
                                  tok[1] + "' (EUL|RK5|ROS2|BDF2).");
        } else if (key == "COUPLING") {
            if      (val == "NONE") rx.coupling = ReactionCoupling::NONE;
            else if (val == "FULL") rx.coupling = ReactionCoupling::FULL;
            else errors.push_back("[REACTION_OPTIONS] unknown COUPLING '" +
                                  tok[1] + "' (NONE|FULL).");
        } else if (key == "RATE_UNITS") {
            if      (val == "SEC") rx.rate_units = ReactionRateUnits::SEC;
            else if (val == "MIN") rx.rate_units = ReactionRateUnits::MIN;
            else if (val == "HR")  rx.rate_units = ReactionRateUnits::HR;
            else if (val == "DAY") rx.rate_units = ReactionRateUnits::DAY;
            else errors.push_back("[REACTION_OPTIONS] unknown RATE_UNITS '" +
                                  tok[1] + "' (SEC|MIN|HR|DAY).");
        } else if (key == "AREA_UNITS") {
            if      (val == "FT2") rx.area_units = ReactionAreaUnits::FT2;
            else if (val == "M2")  rx.area_units = ReactionAreaUnits::M2;
            else if (val == "CM2") rx.area_units = ReactionAreaUnits::CM2;
            else errors.push_back("[REACTION_OPTIONS] unknown AREA_UNITS '" +
                                  tok[1] + "' (FT2|M2|CM2).");
        } else if (key == "TIMESTEP") {
            if (to_num(tok[1], num) && num >= 0.0) rx.timestep = num;
            else errors.push_back("[REACTION_OPTIONS] bad TIMESTEP '" + tok[1] + "'.");
        } else if (key == "ATOL") {
            if (to_num(tok[1], num) && num > 0.0) rx.atol = num;
            else errors.push_back("[REACTION_OPTIONS] bad ATOL '" + tok[1] + "'.");
        } else if (key == "RTOL") {
            if (to_num(tok[1], num) && num > 0.0) rx.rtol = num;
            else errors.push_back("[REACTION_OPTIONS] bad RTOL '" + tok[1] + "'.");
        } else {
            errors.push_back("[REACTION_OPTIONS] unknown option '" + tok[0] + "'.");
        }
    }
}

void parseSpecies(SimulationContext& ctx, const std::vector<std::string>& lines,
                  std::vector<std::string>& errors) {
    auto& rx = ctx.reactions;
    for (const auto& line : lines) {
        auto tok = Tokenizer::tokenize(line);
        if (tok.empty()) continue;
        if (tok.size() < 3) {
            errors.push_back("[REACTION_SPECIES] row needs BULK|WALL name "
                             "units [atol rtol]: '" + line + "'.");
            continue;
        }
        const std::string kind = Tokenizer::to_upper(tok[0]);
        const bool is_wall = (kind == "WALL");
        if (!is_wall && kind != "BULK") {
            errors.push_back("[REACTION_SPECIES] kind must be BULK or WALL: '" +
                             tok[0] + "'.");
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

void parseCoefficients(SimulationContext& ctx,
                       const std::vector<std::string>& lines,
                       std::vector<std::string>& errors) {
    auto& rx = ctx.reactions;
    for (const auto& line : lines) {
        auto tok = Tokenizer::tokenize(line);
        if (tok.empty()) continue;
        if (tok.size() < 3) {
            errors.push_back("[REACTION_COEFFICIENTS] row needs "
                             "PARAMETER|CONSTANT name value: '" + line + "'.");
            continue;
        }
        const std::string kind = Tokenizer::to_upper(tok[0]);
        if (kind != "PARAMETER" && kind != "CONSTANT") {
            errors.push_back("[REACTION_COEFFICIENTS] kind must be PARAMETER "
                             "or CONSTANT: '" + tok[0] + "'.");
            continue;
        }
        if (rx.find_coef(tok[1]) >= 0 || rx.find_species(tok[1]) >= 0) {
            errors.push_back("[REACTION_COEFFICIENTS] name '" + tok[1] +
                             "' duplicates a coefficient or species.");
            continue;
        }
        double v = 0.0;
        if (!to_num(tok[2], v)) {
            errors.push_back("[REACTION_COEFFICIENTS] bad value for '" +
                             tok[1] + "'.");
            continue;
        }
        rx.coef_name.push_back(tok[1]);
        rx.coef_is_param.push_back(kind == "PARAMETER" ? 1 : 0);
        rx.coef_value.push_back(v);
    }
}

void parseTerms(SimulationContext& ctx, const std::vector<std::string>& lines,
                std::vector<std::string>& errors) {
    auto& rx = ctx.reactions;
    for (const auto& line : lines) {
        auto tok = Tokenizer::tokenize(line);
        if (tok.empty()) continue;
        if (tok.size() < 2) {
            errors.push_back("[REACTION_TERMS] row needs name expression: '" +
                             line + "'.");
            continue;
        }
        if (rx.find_term(tok[0]) >= 0 || rx.find_species(tok[0]) >= 0 ||
            rx.find_coef(tok[0]) >= 0) {
            errors.push_back("[REACTION_TERMS] name '" + tok[0] +
                             "' duplicates a term, species, or coefficient.");
            continue;
        }
        rx.term_name.push_back(tok[0]);
        rx.term_expr_src.push_back(rejoin(tok, 1));
    }
}

void parseExpressions(SimulationContext& ctx,
                      const std::vector<std::string>& lines, bool tanks,
                      std::vector<std::string>& errors) {
    auto& rx = ctx.reactions;
    const char* sec = tanks ? "[REACTION_TANKS]" : "[REACTION_PIPES]";
    auto& forms = tanks ? rx.tank_form : rx.pipe_form;
    auto& srcs  = tanks ? rx.tank_expr_src : rx.pipe_expr_src;
    forms.resize(static_cast<std::size_t>(rx.n_species()), ReactionExprForm::NONE);
    srcs.resize(static_cast<std::size_t>(rx.n_species()));
    for (const auto& line : lines) {
        auto tok = Tokenizer::tokenize(line);
        if (tok.empty()) continue;
        if (tok.size() < 3) {
            errors.push_back(std::string(sec) +
                             " row needs RATE|EQUIL|FORMULA species "
                             "expression: '" + line + "'.");
            continue;
        }
        const std::string form = Tokenizer::to_upper(tok[0]);
        ReactionExprForm f = ReactionExprForm::NONE;
        if      (form == "RATE")    f = ReactionExprForm::RATE;
        else if (form == "EQUIL")   f = ReactionExprForm::EQUIL;
        else if (form == "FORMULA") f = ReactionExprForm::FORMULA;
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
                errors.push_back(std::string(sec) + " undeclared species '" +
                                 tok[1] +
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
        srcs[us]  = rejoin(tok, 2);
    }
}

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
        if (tok.empty()) continue;
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
            const int ei = link ? ctx.link_names.find(tok[1])
                                : ctx.node_names.find(tok[1]);
            if (ei < 0) {
                errors.push_back(std::string("[REACTION_QUALITY] unknown ") +
                                 (link ? "link" : "node") + " '" + tok[1] +
                                 "'.");
                continue;
            }
            const int s = rx.find_species(tok[2]);
            if (s < 0) {
                errors.push_back("[REACTION_QUALITY] undeclared species '" +
                                 tok[2] + "'.");
                continue;
            }
            double v = 0.0;
            if (!to_num(tok[3], v) || v < 0.0) {
                errors.push_back("[REACTION_QUALITY] bad value for '" +
                                 tok[2] + "'.");
                continue;
            }
            bool dup = false;
            for (std::size_t k = 0; k < rx.init_elem_idx.size(); ++k) {
                if ((rx.init_elem_is_link[k] != 0) == link &&
                    rx.init_elem_idx[k] == ei &&
                    rx.init_elem_species[k] == s) {
                    dup = true;
                    break;
                }
            }
            if (dup) {
                errors.push_back("[REACTION_QUALITY] duplicate row for '" +
                                 tok[2] + "' at " +
                                 (link ? "link" : "node") + " '" + tok[1] +
                                 "'.");
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
                             "value: '" + line + "'.");
            continue;
        }
        const int s = rx.find_species(tok[1]);
        if (s < 0) {
            errors.push_back("[REACTION_QUALITY] undeclared species '" +
                             tok[1] + "'.");
            continue;
        }
        double v = 0.0;
        if (!to_num(tok[2], v) || v < 0.0) {
            errors.push_back("[REACTION_QUALITY] bad value for '" + tok[1] + "'.");
            continue;
        }
        rx.init_global[static_cast<std::size_t>(s)] = v;
    }
}

}  // namespace

const std::vector<std::string>& reactionSectionTags() {
    static const std::vector<std::string> tags = {
        "REACTION_OPTIONS",       "REACTION_SPECIES",
        "REACTION_COEFFICIENTS",  "REACTION_TERMS",
        "REACTION_PIPES",         "REACTION_TANKS",
        "REACTION_SOURCES",       "REACTION_QUALITY",
        "REACTION_PARAMETERS",    "REACTION_PATTERNS",
        "REACTION_REPORT",        "REACTION_SUBCATCHMENTS"};
    return tags;
}

void applyReactionSections(SimulationContext& ctx,
                           const components::ComponentConfigSections& config,
                           std::vector<std::string>& errors) {
    if (ctx.reactions.configured) {
        errors.push_back(
            "Reactions system configured twice — duplicate registration or "
            "embedded sections alongside an external component file.");
        return;
    }
    ctx.reactions.clear();

    // Order matters: species before coefficients/terms/expressions so
    // references validate. Sections may appear in any file order.
    const auto* s = config.find("REACTION_OPTIONS");
    if (s) parseOptions(ctx, *s, errors);
    s = config.find("REACTION_SPECIES");
    if (s) parseSpecies(ctx, *s, errors);
    if (ctx.reactions.n_species() == 0) {
        errors.push_back("Reactions config declares no [REACTION_SPECIES] — "
                         "at least one species is required.");
        return;
    }
    s = config.find("REACTION_COEFFICIENTS");
    if (s) parseCoefficients(ctx, *s, errors);
    s = config.find("REACTION_TERMS");
    if (s) parseTerms(ctx, *s, errors);
    s = config.find("REACTION_PIPES");
    if (s) parseExpressions(ctx, *s, /*tanks=*/false, errors);
    else {
        ctx.reactions.pipe_form.assign(
            static_cast<std::size_t>(ctx.reactions.n_species()),
            ReactionExprForm::NONE);
        ctx.reactions.pipe_expr_src.resize(
            static_cast<std::size_t>(ctx.reactions.n_species()));
    }
    s = config.find("REACTION_TANKS");
    if (s) parseExpressions(ctx, *s, /*tanks=*/true, errors);
    else {
        ctx.reactions.tank_form.assign(
            static_cast<std::size_t>(ctx.reactions.n_species()),
            ReactionExprForm::NONE);
        ctx.reactions.tank_expr_src.resize(
            static_cast<std::size_t>(ctx.reactions.n_species()));
    }
    s = config.find("REACTION_QUALITY");
    if (s) parseQuality(ctx, *s, errors);
    else
        ctx.reactions.init_global.assign(
            static_cast<std::size_t>(ctx.reactions.n_species()), 0.0);

    // Later-phase sections: defined behavior, never silent acceptance.
    const struct { const char* tag; const char* phase; } later[] = {
        {"REACTION_SOURCES",       "R-sources (post-R3)"},
        {"REACTION_PARAMETERS",    "R-parameters (post-R3)"},
        {"REACTION_PATTERNS",      "R-sources (post-R3)"},
        {"REACTION_REPORT",        "R5"},
        {"REACTION_SUBCATCHMENTS", "R6"},
    };
    for (const auto& l : later) {
        if (config.find(l.tag) != nullptr)
            errors.push_back(std::string("[") + l.tag +
                             "] is recognized but not yet supported — "
                             "arrives with plan phase " + l.phase + ".");
    }

    // Unknown REACTION_* sections in the file are typos worth failing on.
    for (const auto& sect : config.sections) {
        bool known = false;
        for (const auto& t : reactionSectionTags())
            if (sect.first == t) { known = true; break; }
        if (!known)
            errors.push_back("Unknown section [" + sect.first +
                             "] in reactions config '" + config.source_path +
                             "'.");
    }

    if (!errors.empty()) {
        // Transactional: a rejected config leaves NOTHING behind — neither
        // reaction state nor registry entries (nothing was committed yet).
        ctx.reactions.clear();
        return;
    }

    // ---- R2: compile every expression into the flat token pool. --------
    auto& rx2 = ctx.reactions;
    rx2.token_pool.clear();
    rx2.term_expr.assign(rx2.term_name.size(), RxExprSpan{});
    rx2.pipe_expr.assign(static_cast<std::size_t>(rx2.n_species()), RxExprSpan{});
    rx2.tank_expr.assign(static_cast<std::size_t>(rx2.n_species()), RxExprSpan{});

    // R4: pollutants are referencable (read-only) in expressions. Build the
    // name list in registry order (pollutants occupy the first registry
    // slots; PUSH_POLLUT idx == pollutant index).
    std::vector<std::string> pollutant_names;
    for (int p2 = 0; p2 < ctx.species_registry.pollutant_count(); ++p2)
        pollutant_names.push_back(ctx.species_registry.name(p2));

    RxSymbols sym;
    sym.species    = &rx2.species_name;
    sym.coefs      = &rx2.coef_name;
    sym.terms      = &rx2.term_name;
    sym.pollutants = &pollutant_names;

    auto compile_one = [&](const std::string& src_expr, RxExprSpan& span,
                           const std::string& where) {
        int col = 0;
        const std::string err = compileReactionExpression(
            src_expr, sym, rx2.token_pool, span, col);
        if (!err.empty())
            errors.push_back(where + " (col " + std::to_string(col) +
                             "): " + err + ".");
    };

    for (std::size_t i = 0; i < rx2.term_name.size(); ++i) {
        sym.max_term = static_cast<int>(i);   // forward-only rule
        compile_one(rx2.term_expr_src[i], rx2.term_expr[i],
                    "[REACTION_TERMS] '" + rx2.term_name[i] + "'");
    }
    sym.max_term = static_cast<int>(rx2.term_name.size());
    for (int sidx = 0; sidx < rx2.n_species(); ++sidx) {
        const auto us = static_cast<std::size_t>(sidx);
        if (rx2.pipe_form[us] != ReactionExprForm::NONE)
            compile_one(rx2.pipe_expr_src[us], rx2.pipe_expr[us],
                        "[REACTION_PIPES] '" + rx2.species_name[us] + "'");
        if (rx2.tank_form[us] != ReactionExprForm::NONE)
            compile_one(rx2.tank_expr_src[us], rx2.tank_expr[us],
                        "[REACTION_TANKS] '" + rx2.species_name[us] + "'");
    }
    if (!errors.empty()) {
        ctx.reactions.clear();
        return;
    }

    // ---- Commit: registry entries only now, after full success. --------
    for (int sidx = 0; sidx < rx2.n_species(); ++sidx) {
        const auto us = static_cast<std::size_t>(sidx);
        const int reg = ctx.species_registry.add(
            rx2.species_name[us],
            rx2.species_is_wall[us] ? SpeciesKind::MSX_WALL
                                    : SpeciesKind::MSX_BULK,
            rx2.species_units[us]);
        if (rx2.registry_base < 0) rx2.registry_base = reg;
    }
    rx2.compiled   = true;
    rx2.configured = true;
}

void applyEmbeddedReactionSections(SimulationContext& ctx,
                                   bool external_component_registered,
                                   std::vector<std::string>& errors) {
    if (ctx.embedded_component_sections.empty()) return;

    if (external_component_registered) {
        ctx.warnings.push_back(
            "Embedded [REACTION_*] sections in the .inp are IGNORED because "
            "an external reactions component is registered in "
            "[PROCESS_COMPONENTS] — the external config file wins "
            "(TRANSPORT_IO_PLUGIN_CONFIG_PLAN §3.2).");
        return;
    }

    ctx.warnings.push_back(
        "Embedded [REACTION_*] sections found in the .inp. This works, but "
        "the clean layout is an external component config file registered "
        "via [PROCESS_COMPONENTS] (config=\"model.rxn\") — see "
        "TRANSPORT_IO_PLUGIN_CONFIG_PLAN.md (style warning, D-UT8).");

    components::ComponentConfigSections synth;
    synth.source_path = "(embedded in .inp)";
    synth.sections    = ctx.embedded_component_sections;
    applyReactionSections(ctx, synth, errors);
}

void registerReactionsComponent() {
    components::ProcessComponentRegistry::instance().register_component(
        kComponentId,
        "Multispecies reaction system (EPANET-MSX conventions)",
        [](SimulationContext& ctx, const ProcessComponentSpec& /*spec*/,
           const components::ComponentConfigSections& config,
           std::vector<std::string>& errors) {
            applyReactionSections(ctx, config, errors);
        });
}

}  // namespace openswmm::transport
