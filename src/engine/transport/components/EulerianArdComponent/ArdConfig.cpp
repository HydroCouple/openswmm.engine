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
 * @file ArdConfig.cpp
 * @brief transport.ard component apply hook — phase E3 body (dispersion
 *        subset of model.ard).
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "ArdConfig.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>
#include <vector>

#include "../../../core/SimulationContext.hpp"
#include "../../../plugins/ProcessComponentRegistry.hpp"

namespace openswmm::transport {

namespace {

constexpr const char* kArdId = "org.hydrocouple.openswmm.transport.ard";

std::vector<std::string> tokenize(const std::string& line) {
    std::vector<std::string> toks;
    std::string cur;
    for (const char ch : line) {
        if (ch == ' ' || ch == '\t') {
            if (!cur.empty()) { toks.push_back(cur); cur.clear(); }
        } else {
            cur.push_back(ch);
        }
    }
    if (!cur.empty()) toks.push_back(cur);
    return toks;
}

std::string upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return s;
}

/// Parse a non-negative double; false if the token is not fully numeric.
bool parse_value(const std::string& tok, double& out) {
    char* end = nullptr;
    out = std::strtod(tok.c_str(), &end);
    return end != nullptr && *end == '\0' && end != tok.c_str();
}

void applyArdSections(SimulationContext& ctx,
                      const components::ComponentConfigSections& config,
                      std::vector<std::string>& errors) {
    // Reset wholesale so a reopened model never inherits stale rows.
    ctx.ard_config = ArdConfigData{};

    const std::size_t errors_before = errors.size();

    for (const auto& sec : config.sections) {
        const std::string& tag = sec.first;

        if (tag == "TRANSPORT_OPTIONS") {
            for (const auto& line : sec.second) {
                const auto toks = tokenize(line);
                if (toks.empty()) continue;
                const std::string key = upper(toks[0]);
                if (key == "DISPERSION") {
                    if (toks.size() != 2) {
                        errors.push_back(
                            "[TRANSPORT_OPTIONS] DISPERSION expects exactly "
                            "one argument (OFF | FISCHER | value): '" + line +
                            "'.");
                        continue;
                    }
                    const std::string val = upper(toks[1]);
                    if (val == "OFF") {
                        ctx.ard_config.dispersion_mode = ArdDispersionMode::OFF;
                    } else if (val == "FISCHER") {
                        ctx.ard_config.dispersion_mode =
                            ArdDispersionMode::FISCHER;
                    } else {
                        double d = 0.0;
                        if (!parse_value(toks[1], d) || d < 0.0) {
                            errors.push_back(
                                "[TRANSPORT_OPTIONS] DISPERSION '" + toks[1] +
                                "' is not OFF, FISCHER, or a non-negative "
                                "coefficient (display units, len²/s).");
                            continue;
                        }
                        ctx.ard_config.dispersion_mode =
                            ArdDispersionMode::VALUE;
                        ctx.ard_config.dispersion_value = d;
                    }
                } else if (key == "SCALAR_SCHEME" || key == "LIMITER" ||
                           key == "TARGET_DX") {
                    errors.push_back(
                        "[TRANSPORT_OPTIONS] " + key +
                        " in model.ard arrives with plan phase E5 — until "
                        "then the [OPTIONS] FV_SCALAR_SCHEME / FV_LIMITER "
                        "keys configure the ARD engine's scheme.");
                } else {
                    errors.push_back(
                        "[TRANSPORT_OPTIONS] unknown key '" + toks[0] +
                        "' (E3 recognizes: DISPERSION).");
                }
            }
        } else if (tag == "CONDUIT_DISPERSION") {
            for (const auto& line : sec.second) {
                const auto toks = tokenize(line);
                if (toks.size() != 2) {
                    errors.push_back(
                        "[CONDUIT_DISPERSION] expects '<conduit_name> "
                        "<value>': '" + line + "'.");
                    continue;
                }
                const int link = ctx.link_names.find(toks[0]);
                if (link < 0) {
                    errors.push_back(
                        "[CONDUIT_DISPERSION] unknown conduit '" + toks[0] +
                        "'.");
                    continue;
                }
                if (ctx.link_subtypes.conduit_row(link) < 0) {
                    errors.push_back(
                        "[CONDUIT_DISPERSION] link '" + toks[0] +
                        "' is not a conduit — dispersion applies to conduits "
                        "only (structures are zero-volume passthrough).");
                    continue;
                }
                double d = 0.0;
                if (!parse_value(toks[1], d) || d < 0.0) {
                    errors.push_back(
                        "[CONDUIT_DISPERSION] '" + toks[0] + "': value '" +
                        toks[1] + "' is not a non-negative coefficient "
                        "(display units, len²/s).");
                    continue;
                }
                bool dup = false;
                for (const int prev : ctx.ard_config.conduit_disp_link)
                    if (prev == link) { dup = true; break; }
                if (dup) {
                    errors.push_back(
                        "[CONDUIT_DISPERSION] duplicate row for conduit '" +
                        toks[0] + "'.");
                    continue;
                }
                ctx.ard_config.conduit_disp_link.push_back(link);
                ctx.ard_config.conduit_disp_value.push_back(d);
            }
        } else if (tag == "TRANSPORT_BOUNDARIES" ||
                   tag == "TRANSPORT_SOURCES") {
            errors.push_back(
                "[" + tag + "] arrives with plan phase E5 (Eulerian ARD "
                "plan) — not yet implemented.");
        } else if (tag == "STORAGE_MIXING") {
            errors.push_back(
                "[STORAGE_MIXING] arrives with plan phase E2b (Eulerian ARD "
                "plan) — not yet implemented.");
        } else {
            errors.push_back(
                "model.ard: unknown section [" + tag +
                "] (E3 recognizes: [TRANSPORT_OPTIONS] "
                "[CONDUIT_DISPERSION]).");
        }
    }

    if (errors.size() != errors_before) {
        ctx.ard_config = ArdConfigData{};  // never half-apply
        return;
    }
    ctx.ard_config.configured = true;

    // Silent-bypass enumeration (R4 validation lesson): every configuration
    // under which this file parses but its dispersion reaches nothing warns
    // at open, naming the remedy.
    if (ctx.ard_config.any_dispersion()) {
        if (ctx.options.ignore_quality) {
            ctx.warnings.push_back(
                "A transport.ard component is configured but IGNORE_QUALITY "
                "is YES — the quality stage does not run, so no dispersion "
                "applies this simulation.");
        } else if (ctx.options.quality_solver !=
                   QualitySolverKind::EULERIAN_ARD) {
            ctx.warnings.push_back(
                "A transport.ard component is configured but QUALITY_SOLVER "
                "is not EULERIAN_ARD — its dispersion configuration is inert "
                "under the LEGACY engine. Set [OPTIONS] QUALITY_SOLVER "
                "EULERIAN_ARD to activate it.");
        } else if (ctx.n_pollutants() == 0) {
            // E4/R6: MSX species now transport (and disperse) under the ARD
            // engine, so an MSX-only model is no longer a bypass. The
            // reactions component may apply before OR after this hook, so
            // test the [PROCESS_COMPONENTS] registrations, which are
            // order-independent, rather than ctx.reactions.
            bool has_reactions_component = false;
            for (const auto& spec : ctx.process_component_specs)
                if (spec.id == "org.hydrocouple.openswmm.reactions")
                    has_reactions_component = true;
            if (!has_reactions_component)
                ctx.warnings.push_back(
                    "A transport.ard component configures dispersion but the "
                    "model has no [POLLUTANTS] and no reactions component — "
                    "there are no transported species, so nothing disperses "
                    "this simulation.");
        }
    }
}

}  // namespace

void warnIfFvDispersionKeyIgnored(SimulationContext& ctx) {
    if (ctx.options.fv.dispersion == 0.0) return;
    if (ctx.options.quality_solver != QualitySolverKind::EULERIAN_ARD) return;
    ctx.warnings.push_back(
        "[OPTIONS] FV_DISPERSION is set but does not configure the Eulerian "
        "ARD engine — it reads dispersion from the transport.ard component "
        "(model.ard: [TRANSPORT_OPTIONS] DISPERSION OFF|FISCHER|<value> and "
        "[CONDUIT_DISPERSION] rows). No dispersion is applied from this key. "
        "Unifying the FV_*/ARD_* option surface arrives with plan phase E5.");
}

void registerArdComponent() {
    components::ProcessComponentRegistry::instance().register_component(
        kArdId,
        "Eulerian ARD transport engine configuration (E3: dispersion; "
        "boundaries/sources arrive with E5)",
        [](SimulationContext& ctx, const ProcessComponentSpec& /*spec*/,
           const components::ComponentConfigSections& config,
           std::vector<std::string>& errors) {
            applyArdSections(ctx, config, errors);
        });
}

}  // namespace openswmm::transport
