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
#include <filesystem>
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
                } else if (key == "SCALAR_SCHEME") {
                    // E5a: aliases of the internal FV transport config
                    // (plan §4). model.ard wins over [OPTIONS] FV_* — the
                    // component applies after the options parse.
                    if (toks.size() != 2) {
                        errors.push_back(
                            "[TRANSPORT_OPTIONS] SCALAR_SCHEME expects "
                            "UPWIND | MUSCL | QUICKEST_ULTIMATE.");
                        continue;
                    }
                    const std::string v = upper(toks[1]);
                    if (v == "UPWIND")
                        ctx.options.fv.scalar_scheme = fv::ScalarScheme::UPWIND;
                    else if (v == "MUSCL")
                        ctx.options.fv.scalar_scheme = fv::ScalarScheme::MUSCL;
                    else if (v == "QUICKEST_ULTIMATE")
                        ctx.options.fv.scalar_scheme =
                            fv::ScalarScheme::QUICKEST_ULTIMATE;
                    else
                        errors.push_back(
                            "[TRANSPORT_OPTIONS] SCALAR_SCHEME '" + toks[1] +
                            "' is not UPWIND, MUSCL, or QUICKEST_ULTIMATE.");
                } else if (key == "LIMITER") {
                    if (toks.size() != 2) {
                        errors.push_back(
                            "[TRANSPORT_OPTIONS] LIMITER expects MINMOD | "
                            "VANLEER | SUPERBEE.");
                        continue;
                    }
                    const std::string v = upper(toks[1]);
                    if (v == "MINMOD")
                        ctx.options.fv.limiter = fv::Limiter::MINMOD;
                    else if (v == "VANLEER")
                        ctx.options.fv.limiter = fv::Limiter::VANLEER;
                    else if (v == "SUPERBEE")
                        ctx.options.fv.limiter = fv::Limiter::SUPERBEE;
                    else
                        errors.push_back(
                            "[TRANSPORT_OPTIONS] LIMITER '" + toks[1] +
                            "' is not MINMOD, VANLEER, or SUPERBEE.");
                } else if (key == "TARGET_DX") {
                    // E5b: the plan §8 open item resolved — transport-mesh
                    // cell length under non-FV hydraulics (display units).
                    // Under FLOW_ROUTING FV the engine warns and ignores it
                    // (the solver mesh governs).
                    double v = 0.0;
                    if (toks.size() != 2 || !parse_value(toks[1], v) ||
                        v <= 0.0) {
                        errors.push_back(
                            "[TRANSPORT_OPTIONS] TARGET_DX expects a "
                            "positive cell length (display length units).");
                        continue;
                    }
                    ctx.ard_config.target_dx = v;
                } else if (key == "DETAILED_OUTPUT") {
                    if (toks.size() != 2) {
                        errors.push_back(
                            "[TRANSPORT_OPTIONS] DETAILED_OUTPUT expects "
                            "one path argument.");
                        continue;
                    }
                    // Relative paths resolve against the CONFIG file's own
                    // directory (component-file locality, like the config
                    // itself resolves against the .inp).
                    namespace fsys = std::filesystem;
                    fsys::path p(toks[1]);
                    if (p.is_relative()) {
                        const fsys::path base =
                            fsys::path(config.source_path).parent_path();
                        if (!base.empty()) p = base / p;
                    }
                    ctx.ard_config.detailed_output_path = p.string();
                } else {
                    errors.push_back(
                        "[TRANSPORT_OPTIONS] unknown key '" + toks[0] +
                        "' (E5b recognizes: DISPERSION, SCALAR_SCHEME, "
                        "LIMITER, TARGET_DX, DETAILED_OUTPUT).");
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
            // E5a. Rows are stored RAW — species are resolved after every
            // component has applied (order-independence w.r.t. the reactions
            // component; see resolveArdTransportRows). Syntax and numerics
            // are validated here, where the row text is at hand.
            const bool is_bc = (tag == "TRANSPORT_BOUNDARIES");
            for (const auto& line : sec.second) {
                const auto toks = tokenize(line);
                if (toks.size() != 4) {
                    errors.push_back(
                        "[" + tag + "] expects '<" +
                        std::string(is_bc ? "node" : "conduit") +
                        "> <species> VALUE <v> | TIMESERIES <name>': '" +
                        line + "'.");
                    continue;
                }
                ArdTransportRow row;
                row.element = toks[0];
                row.species = toks[1];
                const std::string mode = upper(toks[2]);
                if (mode == "VALUE") {
                    double v = 0.0;
                    if (!parse_value(toks[3], v) || v < 0.0) {
                        errors.push_back(
                            "[" + tag + "] '" + toks[0] + "': VALUE '" +
                            toks[3] + "' is not a non-negative number.");
                        continue;
                    }
                    row.value = v;
                } else if (mode == "TIMESERIES") {
                    row.is_ts   = true;
                    row.ts_name = toks[3];
                } else {
                    errors.push_back(
                        "[" + tag + "] '" + toks[0] + "': mode '" + toks[2] +
                        "' is not VALUE or TIMESERIES.");
                    continue;
                }
                (is_bc ? ctx.ard_config.boundary_rows
                       : ctx.ard_config.source_rows)
                    .push_back(std::move(row));
            }
        } else if (tag == "STORAGE_MIXING") {
            errors.push_back(
                "[STORAGE_MIXING] arrives with plan phase E2b (Eulerian ARD "
                "plan) — not yet implemented.");
        } else {
            errors.push_back(
                "model.ard: unknown section [" + tag +
                "] (E5a recognizes: [TRANSPORT_OPTIONS] "
                "[CONDUIT_DISPERSION] [TRANSPORT_BOUNDARIES] "
                "[TRANSPORT_SOURCES]).");
        }
    }

    if (errors.size() != errors_before) {
        ctx.ard_config = ArdConfigData{};  // never half-apply
        return;
    }
    ctx.ard_config.configured = true;

    // Silent-bypass enumeration (R4 validation lesson): every configuration
    // under which this file parses but its content reaches nothing warns at
    // open, naming the remedy. E5b: TARGET_DX and DETAILED_OUTPUT count as
    // engine content too (any_engine_content).
    if (ctx.ard_config.any_engine_content()) {
        if (ctx.options.ignore_quality) {
            ctx.warnings.push_back(
                "A transport.ard component is configured but IGNORE_QUALITY "
                "is YES — the quality stage does not run, so none of its "
                "dispersion/boundary/source configuration applies this "
                "simulation.");
        } else if (ctx.options.quality_solver !=
                   QualitySolverKind::EULERIAN_ARD) {
            ctx.warnings.push_back(
                "A transport.ard component is configured but QUALITY_SOLVER "
                "is not EULERIAN_ARD — its dispersion/boundary/source "
                "configuration is inert under the LEGACY engine. Set "
                "[OPTIONS] QUALITY_SOLVER EULERIAN_ARD to activate it.");
        } else if (ctx.n_pollutants() == 0 &&
                   ctx.ard_config.any_dispersion()) {
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

bool ardBoundariesNeedExternalVolumes(const SimulationContext& ctx) {
    return ctx.options.quality_solver == QualitySolverKind::EULERIAN_ARD &&
           !ctx.options.ignore_quality &&
           !ctx.ard_config.bc_node.empty();
}

void resolveArdTransportRows(SimulationContext& ctx,
                             std::vector<std::string>& errors) {
    auto& cfg = ctx.ard_config;
    cfg.bc_node.clear();  cfg.bc_msx.clear();
    cfg.bc_value.clear(); cfg.bc_ts.clear();
    cfg.src_link.clear(); cfg.src_msx.clear();
    cfg.src_value.clear(); cfg.src_ts.clear();
    cfg.transport_rows_resolved = false;
    if (!cfg.configured || !cfg.any_transport_rows()) {
        cfg.transport_rows_resolved = true;
        return;
    }

    // Species must be MSX: pollutants keep their full legacy loading
    // surface ([INFLOWS] CONCEN/MASS, DWF, RDII, washoff) and allowing them
    // here would create a silent double-count question E5a refuses to have.
    const auto resolve_species = [&](const ArdTransportRow& row,
                                     const char* sec) -> int {
        const int s = ctx.reactions.find_species(row.species);
        if (s >= 0) return s;
        if (ctx.species_registry.find(row.species) >= 0) {
            errors.push_back(
                std::string("[") + sec + "] '" + row.element + "': '" +
                row.species + "' is a pollutant — pollutant loading uses "
                "the legacy pathways ([INFLOWS], DWF, washoff); transport "
                "boundaries/sources apply to MSX species only (E5a).");
        } else {
            errors.push_back(
                std::string("[") + sec + "] '" + row.element +
                "': unknown species '" + row.species +
                "' — declare it in the reactions component's "
                "[REACTION_SPECIES].");
        }
        return -1;
    };
    const auto resolve_ts = [&](const ArdTransportRow& row,
                                const char* sec) -> int {
        const int t = ctx.tables.find_by_kind(row.ts_name, true);
        if (t < 0)
            errors.push_back(std::string("[") + sec + "] '" + row.element +
                             "': unknown timeseries '" + row.ts_name + "'.");
        return t;
    };

    for (const auto& row : cfg.boundary_rows) {
        const int nd = ctx.node_names.find(row.element);
        if (nd < 0) {
            errors.push_back("[TRANSPORT_BOUNDARIES] unknown node '" +
                             row.element + "'.");
            continue;
        }
        const int s = resolve_species(row, "TRANSPORT_BOUNDARIES");
        if (s < 0) continue;
        int ts = -1;
        if (row.is_ts &&
            (ts = resolve_ts(row, "TRANSPORT_BOUNDARIES")) < 0)
            continue;
        bool dup = false;
        for (std::size_t i = 0; i < cfg.bc_node.size(); ++i)
            if (cfg.bc_node[i] == nd && cfg.bc_msx[i] == s) {
                errors.push_back(
                    "[TRANSPORT_BOUNDARIES] duplicate row for node '" +
                    row.element + "' species '" + row.species + "'.");
                dup = true;
                break;
            }
        if (dup) continue;
        cfg.bc_node.push_back(nd);
        cfg.bc_msx.push_back(s);
        cfg.bc_value.push_back(row.value);
        cfg.bc_ts.push_back(row.is_ts ? ts : -1);
    }

    for (const auto& row : cfg.source_rows) {
        const int link = ctx.link_names.find(row.element);
        if (link < 0 || ctx.link_subtypes.conduit_row(link) < 0) {
            errors.push_back(
                "[TRANSPORT_SOURCES] '" + row.element +
                "' is not a conduit — distributed sources apply to "
                "conduits.");
            continue;
        }
        const int s = resolve_species(row, "TRANSPORT_SOURCES");
        if (s < 0) continue;
        int ts = -1;
        if (row.is_ts && (ts = resolve_ts(row, "TRANSPORT_SOURCES")) < 0)
            continue;
        bool dup = false;
        for (std::size_t i = 0; i < cfg.src_link.size(); ++i)
            if (cfg.src_link[i] == link && cfg.src_msx[i] == s) {
                errors.push_back(
                    "[TRANSPORT_SOURCES] duplicate row for conduit '" +
                    row.element + "' species '" + row.species + "'.");
                dup = true;
                break;
            }
        if (dup) continue;
        cfg.src_link.push_back(link);
        cfg.src_msx.push_back(s);
        // VALUE rates arrive in species mass units per second; internal MSX
        // store mass is conc(mass/L)·ft³, so divide by L/ft³ once here.
        // TIMESERIES values get the same conversion at evaluation time.
        cfg.src_value.push_back(row.value / kLitersPerFt3);
        cfg.src_ts.push_back(row.is_ts ? ts : -1);
    }

    // Resolution ran; fatality of any errors pushed above is the caller's
    // decision (the strict-vs-lenient open path).
    cfg.transport_rows_resolved = true;
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
