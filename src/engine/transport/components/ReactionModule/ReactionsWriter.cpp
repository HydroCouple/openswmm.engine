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
 * @file ReactionsWriter.cpp
 * @brief Canonical .rxn serializer (E-C3). @see ReactionsWriter.hpp
 */

#include "ReactionsWriter.hpp"
#include "../../MsxInitialQuality.hpp"   // U2

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "../../../core/SimulationContext.hpp"

namespace openswmm::transport {

namespace {

/// Shortest decimal form that parses back to exactly @p v — stable across
/// serialize -> parse -> serialize (the D-RC6 requirement).
std::string fmt_double(double v) {
    char buf[64];
    for (int prec = 15; prec <= 17; ++prec) {
        std::snprintf(buf, sizeof(buf), "%.*g", prec, v);
        if (std::strtod(buf, nullptr) == v) break;
    }
    return buf;
}

void row(std::string& out, const char* a, const std::string& b,
         const std::string& c = {}, const std::string& d = {},
         const std::string& e = {}) {
    char line[512];
    if (!e.empty())
        std::snprintf(line, sizeof(line), "%-10s %-16s %-16s %-12s %s\n",
                      a, b.c_str(), c.c_str(), d.c_str(), e.c_str());
    else if (!d.empty())
        std::snprintf(line, sizeof(line), "%-10s %-16s %-16s %s\n",
                      a, b.c_str(), c.c_str(), d.c_str());
    else if (!c.empty())
        std::snprintf(line, sizeof(line), "%-10s %-16s %s\n",
                      a, b.c_str(), c.c_str());
    else
        std::snprintf(line, sizeof(line), "%-10s %s\n", a, b.c_str());
    out += line;
}

const char* form_tok(ReactionExprForm f) {
    switch (f) {
        case ReactionExprForm::RATE:    return "RATE";
        case ReactionExprForm::EQUIL:   return "EQUIL";
        case ReactionExprForm::FORMULA: return "FORMULA";
        default:                        return nullptr;
    }
}

} // namespace

std::string serializeReactionSystem(const SimulationContext& ctx) {
    const auto& rx = ctx.reactions;
    if (!rx.configured && rx.n_species() == 0) return {};

    std::string out;
    out += ";; Reaction system configuration — written by OpenSWMM.\n";

    out += "\n[REACTION_OPTIONS]\n";
    {
        static const char* kSolver[] = {"EUL", "RK5", "ROS2", "BDF2"};
        static const char* kRate[]   = {"SEC", "MIN", "HR", "DAY"};
        static const char* kArea[]   = {"FT2", "M2", "CM2"};
        row(out, "SOLVER",     kSolver[static_cast<int>(rx.solver)]);
        row(out, "COUPLING",
            static_cast<int>(rx.coupling) == 1 ? "FULL" : "NONE");
        row(out, "RATE_UNITS", kRate[static_cast<int>(rx.rate_units)]);
        row(out, "AREA_UNITS", kArea[static_cast<int>(rx.area_units)]);
        row(out, "PATTERN_TIMESTEP", fmt_double(rx.pattern_timestep));
        row(out, "TIMESTEP",   fmt_double(rx.timestep));
        row(out, "ATOL",       fmt_double(rx.atol));
        row(out, "RTOL",       fmt_double(rx.rtol));
        row(out, "TEMPERATURE", fmt_double(rx.default_temp_c));
    }

    out += "\n[REACTION_SPECIES]\n";
    out += ";;Kind     Name             Units            [Atol]       [Rtol]\n";
    for (int s = 0; s < rx.n_species(); ++s) {
        const auto u = static_cast<std::size_t>(s);
        const bool tols = rx.species_atol[u] > 0.0 || rx.species_rtol[u] > 0.0;
        if (tols)
            row(out, rx.species_is_wall[u] ? "WALL" : "BULK",
                rx.species_name[u], rx.species_units[u],
                fmt_double(rx.species_atol[u]),
                fmt_double(rx.species_rtol[u]));
        else
            row(out, rx.species_is_wall[u] ? "WALL" : "BULK",
                rx.species_name[u], rx.species_units[u]);
    }

    if (!rx.coef_name.empty()) {
        out += "\n[REACTION_COEFFICIENTS]\n";
        out += ";;Kind     Name             Value\n";
        for (std::size_t i = 0; i < rx.coef_name.size(); ++i)
            row(out, rx.coef_is_param[i] ? "PARAMETER" : "CONSTANT",
                rx.coef_name[i], fmt_double(rx.coef_value[i]));
    }

    if (!rx.source_patterns.empty()) {
        out += "\n[REACTION_PATTERNS]\n";
        for (const auto& pattern : rx.source_patterns) {
            out += pattern.name;
            for (const auto value : pattern.factors) out += " " + fmt_double(value);
            out += "\n";
        }
    }
    if (!rx.sources.empty()) {
        out += "\n[REACTION_SOURCES]\n";
        for (const auto& r : rx.sources)
            row(out, r.mass ? "MASS" : "CONC", ctx.node_names.name_of(r.node),
                rx.species_name[r.species], fmt_double(r.value),
                r.pattern < 0 ? "*" : rx.source_patterns[r.pattern].name);
    }
    if (!rx.parameter_overrides.empty()) {
        out += "\n[REACTION_PARAMETERS]\n";
        for (const auto& r : rx.parameter_overrides)
            row(out, r.link ? "LINK" : "NODE",
                r.link ? ctx.link_names.name_of(r.element) : ctx.node_names.name_of(r.element),
                rx.coef_name[r.coefficient], fmt_double(r.value));
    }
    if (!rx.report_enabled.empty()) {
        out += "\n[REACTION_REPORT]\nSPECIES ALL YES\n";
        for (int m = 0; m < rx.n_species(); ++m)
            if (!rx.report_enabled[m]) row(out, "SPECIES", rx.species_name[m], "NO");
    }

    if (!rx.term_name.empty()) {
        out += "\n[REACTION_TERMS]\n";
        out += ";;Name     Expression\n";
        for (std::size_t i = 0; i < rx.term_name.size(); ++i)
            row(out, rx.term_name[i].c_str(), rx.term_expr_src[i]);
    }

    auto emit_scope = [&](const char* tag,
                          const std::vector<ReactionExprForm>& forms,
                          const std::vector<std::string>& srcs) {
        bool any = false;
        for (const auto f : forms)
            if (f != ReactionExprForm::NONE) any = true;
        if (!any) return;
        out += std::string("\n[") + tag + "]\n";
        out += ";;Form     Species          Expression\n";
        for (int s = 0; s < rx.n_species(); ++s) {
            const auto u = static_cast<std::size_t>(s);
            const char* tok = form_tok(forms[u]);
            if (!tok) continue;
            row(out, tok, rx.species_name[u], srcs[u]);
        }
    };
    emit_scope("REACTION_PIPES", rx.pipe_form, rx.pipe_expr_src);
    emit_scope("REACTION_TANKS", rx.tank_form, rx.tank_expr_src);
    emit_scope("REACTION_SUBCATCHMENTS", rx.subcatch_form, rx.subcatch_expr_src);

    if(!rx.subsurface_laws.empty()) {
        out+="\n[REACTION_SUBSURFACE]\n";
        for(const auto& law:rx.subsurface_laws) {
            std::string scope=law.scope==0?"*":law.scope==1?"TAG \""+law.tag+"\"":"CELL "+std::to_string(law.cell+1);
            const std::string zone=law.zone==0?"SAT":law.zone==1?"UNSAT":"LAYER "+std::to_string(law.layer+1);
            out+=scope+" "+zone+" "+rx.species_name[law.species]+" "+form_tok(law.form)+" "+law.expression+"\n";
        }
    }

    // Initial quality: nonzero GLOBAL rows, then the per-element rows.
    {
        bool any = false;
        for (const auto v : rx.init_global)
            if (v != 0.0) any = true;
        // U2: per-element rows mirrored from [INITIAL_QUALITY] are written
        // THERE by the InpWriter; only rows the .rxn itself owns (authored in
        // its NODE|LINK scopes or set through the reactions C API) stay here.
        bool any_own_elem = false;
        for (std::size_t k = 0; k < rx.init_elem_idx.size(); ++k) {
            if (!initialQualityHasMsxRow(ctx, rx.init_elem_is_link[k] != 0,
                                         rx.init_elem_idx[k], rx.init_elem_species[k])) {
                any_own_elem = true;
                break;
            }
        }
        // [REACTION_RAINFALL] (Multispecies Phase 2), only when a species rains in.
        {
            bool rain = false;
            for (const auto v : rx.rain_conc) rain = rain || v != 0.0;
            if (rain) {
                out += "\n[REACTION_RAINFALL]\n";
                out += ";;Species         Concentration\n";
                for (int s = 0; s < rx.n_species(); ++s) {
                    const auto u = static_cast<std::size_t>(s);
                    if (u < rx.rain_conc.size() && rx.rain_conc[u] != 0.0)
                        out += rx.species_name[u] + "  " + fmt_double(rx.rain_conc[u]) + "\n";
                }
            }
        }
        if (any || any_own_elem) {
            out += "\n[REACTION_QUALITY]\n";
            out += ";;Scope    [Element]        Species          Value\n";
            for (int s = 0; s < rx.n_species(); ++s) {
                const auto u = static_cast<std::size_t>(s);
                if (u < rx.init_global.size() && rx.init_global[u] != 0.0)
                    row(out, "GLOBAL", rx.species_name[u],
                        fmt_double(rx.init_global[u]));
            }
            for (std::size_t k = 0; k < rx.init_elem_idx.size(); ++k) {
                const bool link = rx.init_elem_is_link[k] != 0;
                const int ei = rx.init_elem_idx[k];
                const int si = rx.init_elem_species[k];
                if (si < 0 || si >= rx.n_species()) continue;
                if (initialQualityHasMsxRow(ctx, link, ei, si)) continue;   // U2
                const std::string en =
                    link ? ((ei >= 0 && ei < ctx.n_links())
                                ? ctx.link_names.name_of(ei)
                                : std::string("*"))
                         : ((ei >= 0 && ei < ctx.n_nodes())
                                ? ctx.node_names.name_of(ei)
                                : std::string("*"));
                row(out, link ? "LINK" : "NODE", en,
                    rx.species_name[static_cast<std::size_t>(si)],
                    fmt_double(rx.init_elem_value[k]));
            }
        }
    }

    return out;
}

}  // namespace openswmm::transport
