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
 * @file HeatComponent.cpp
 * @brief heat component apply hook — phase H1 body.
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "HeatComponent.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>
#include <vector>

#include "../../../core/SimulationContext.hpp"
#include "../../../plugins/ProcessComponentRegistry.hpp"

namespace openswmm::transport {

namespace {

constexpr const char* kHeatId = "org.hydrocouple.openswmm.heat";

/// Physically motivated guard rails on an inlet temperature (°C). Liquid
/// water outside this band is not a modelling case H1 supports, and a
/// typo (a Fahrenheit value, a stray exponent) is far more likely than a
/// deck that means it.
constexpr double kMinTemp = -50.0;
constexpr double kMaxTemp = 100.0;

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

/// Unlike the age parser's `parse_hours`, this accepts NEGATIVE values —
/// sub-zero water temperatures are ordinary — so the range check is
/// explicit rather than riding on a `>= 0` test.
bool parse_celsius(const std::string& tok, double& out) {
    char* end = nullptr;
    out = std::strtod(tok.c_str(), &end);
    if (end == nullptr || *end != '\0' || end == tok.c_str()) return false;
    return out >= kMinTemp && out <= kMaxTemp;
}

int source_of(const std::string& u) {
    if (u == "RAINFALL")        return static_cast<int>(HeatSource::RAINFALL);
    if (u == "DWF")             return static_cast<int>(HeatSource::DWF);
    if (u == "GW")              return static_cast<int>(HeatSource::GW);
    if (u == "RDII")            return static_cast<int>(HeatSource::RDII);
    if (u == "EXTERNAL_INFLOW") return static_cast<int>(HeatSource::EXTERNAL_INFLOW);
    if (u == "IFACE")           return static_cast<int>(HeatSource::IFACE);
    if (u == "INITIAL_STATE")   return static_cast<int>(HeatSource::INITIAL_STATE);
    return -1;
}

void applyHeatSections(SimulationContext& ctx,
                       const components::ComponentConfigSections& config,
                       std::vector<std::string>& errors) {
    ctx.heat_config = HeatConfigData{};  // reopen hygiene
    const std::size_t errors_before = errors.size();

    for (const auto& sec : config.sections) {
        // H2: [HEAT_FLUXES] toggles the plan §2 modules, one row per module.
        // H3: [RADIATIVE_FLUXES] carries the plan §2.2 parameters. GLOBAL
        // scope only; RHE's per-element ranges are a later phase.
        if (sec.first == "RADIATIVE_FLUXES") {
            for (const auto& line : sec.second) {
                const auto toks = tokenize(line);
                if (toks.size() < 3 || upper(toks[1]) != "GLOBAL") {
                    errors.push_back(
                        "[RADIATIVE_FLUXES] expects '<param> GLOBAL <value>' "
                        "(per-element ranges arrive with a later heat "
                        "phase): '" + line + "'.");
                    continue;
                }
                double v = 0.0;
                char* end = nullptr;
                v = std::strtod(toks[2].c_str(), &end);
                if (end == nullptr || *end != '\0' ||
                    end == toks[2].c_str()) {
                    errors.push_back("[RADIATIVE_FLUXES] '" + toks[0] +
                                     "': '" + toks[2] + "' is not a number.");
                    continue;
                }
                auto& rc = ctx.heat_config.radiative;
                const std::string k = upper(toks[0]);
                // Fractions are refused outside [0,1] rather than clamped: a
                // 97 typed for an emissivity of 0.97 would otherwise scale
                // every longwave term by a hundred, silently.
                auto frac = [&](double& dst) {
                    if (v < 0.0 || v > 1.0)
                        errors.push_back("[RADIATIVE_FLUXES] '" + toks[0] +
                                         "' must be a fraction in [0,1], got "
                                         + toks[2] + ".");
                    else dst = v;
                };
                if      (k == "SHORTWAVE")         { if (v < 0.0)
                        errors.push_back("[RADIATIVE_FLUXES] SHORTWAVE must "
                                         "be >= 0 W/m2.");
                    else rc.shortwave_wm2 = v; }
                else if (k == "ALBEDO")            frac(rc.albedo);
                else if (k == "SHADE_FACTOR")      frac(rc.shade_factor);
                else if (k == "SKY_VIEW")          frac(rc.sky_view);
                else if (k == "EMISS_WATER")       frac(rc.emiss_water);
                else if (k == "EMISS_LANDCOVER")   frac(rc.emiss_landcover);
                else if (k == "ATM_EMISS_COEFF")   frac(rc.atm_emiss_coeff);
                else if (k == "ATM_LW_REFLECTION") frac(rc.lw_reflection);
                else
                    errors.push_back("[RADIATIVE_FLUXES] unknown parameter '" +
                                     toks[0] + "'.");
            }
            continue;
        }
        if (sec.first == "HEAT_FLUXES") {
            for (const auto& line : sec.second) {
                const auto toks = tokenize(line);
                if (toks.size() != 2) {
                    errors.push_back(
                        "[HEAT_FLUXES] expects '<module> ON|OFF': '" + line +
                        "'.");
                    continue;
                }
                const std::string mod = upper(toks[0]);
                const std::string val = upper(toks[1]);

                // H5a/D-H5c. Checked BEFORE the ON|OFF validation because it
                // is the one key in this section whose value is a named mode
                // rather than a toggle; leaving it below would have it
                // rejected as "not ON or OFF" before its own branch ran.
                // Ladder shape follows ArdConfig.cpp:116-137 (SCALAR_SCHEME).
                if (mod == "DRY_ELEMENT_TEMPERATURE") {
                    if (val == "HOLD")
                        ctx.heat_config.dry_temp_policy = DryTempPolicy::HOLD;
                    else if (val == "AIR")
                        ctx.heat_config.dry_temp_policy = DryTempPolicy::AIR;
                    else if (val == "DEFAULT")
                        ctx.heat_config.dry_temp_policy =
                            DryTempPolicy::DEFAULT;
                    else
                        errors.push_back(
                            "[HEAT_FLUXES] DRY_ELEMENT_TEMPERATURE '" +
                            toks[1] + "' is not HOLD, AIR, or DEFAULT.");
                    continue;
                }

                if (val != "ON" && val != "OFF" && val != "YES" &&
                    val != "NO") {
                    errors.push_back(
                        "[HEAT_FLUXES] '" + toks[0] + "': '" + toks[1] +
                        "' is not ON or OFF.");
                    continue;
                }
                const bool on = (val == "ON" || val == "YES");
                if (mod == "SURFACE_EXCHANGE") {
                    ctx.heat_config.surface_exchange = on;
                } else if (mod == "RADIATIVE_EXCHANGE") {
                    ctx.heat_config.radiative_exchange = on;
                } else if (mod == "SEDIMENT_EXCHANGE") {
                    errors.push_back(
                        "[HEAT_FLUXES] SEDIMENT_EXCHANGE arrives with plan "
                        "phase H6 (HTS two-layer storage). H4 is the ARD "
                        "mesh binding.");
                } else {
                    errors.push_back(
                        "[HEAT_FLUXES] unknown module '" + toks[0] +
                        "' (SURFACE_EXCHANGE in H2, RADIATIVE_EXCHANGE in "
                        "H3, DRY_ELEMENT_TEMPERATURE in H5a).");
                }
            }
            continue;
        }
        if (sec.first != "HEAT_SOURCES") {
            errors.push_back(
                "model.heat: unknown section [" + sec.first +
                "] (recognized: [HEAT_SOURCES], [HEAT_FLUXES], "
                "[RADIATIVE_FLUXES]). Sediment sections arrive with H4.");
            continue;
        }
        for (const auto& line : sec.second) {
            const auto toks = tokenize(line);
            if (toks.size() < 3) {
                errors.push_back(
                    "[HEAT_SOURCES] expects '<source> <scope> [name] "
                    "<degC>': '" + line + "'.");
                continue;
            }
            const int src = source_of(upper(toks[0]));
            if (src < 0) {
                errors.push_back(
                    "[HEAT_SOURCES] unknown source '" + toks[0] +
                    "' (RAINFALL, DWF, GW, RDII, EXTERNAL_INFLOW, IFACE, "
                    "INITIAL_STATE).");
                continue;
            }
            const std::string scope = upper(toks[1]);

            // Deferral surface — precise phase names, never silence.
            if (scope == "SUBCATCH") {
                errors.push_back(
                    "[HEAT_SOURCES] SUBCATCH scope arrives with plan phase "
                    "H5 (watershed temperature states).");
                continue;
            }
            if (scope == "EDGE_BC") {
                errors.push_back(
                    "[HEAT_SOURCES] EDGE_BC scope arrives with phase T6 "
                    "(2D transport).");
                continue;
            }
            // Bind the value token only once the row is known to HAVE that
            // column (A1a lesson: reading toks[3] before the arity check
            // was an out-of-bounds read on a 3-token NODE row).
            const bool has_name = (scope == "NODE");
            const std::size_t vpos = has_name ? 3u : 2u;
            if (toks.size() <= vpos) {
                errors.push_back(
                    "[HEAT_SOURCES] malformed row: '" + line + "'.");
                continue;
            }
            const std::string& vtok = toks[vpos];
            // TIMESERIES is checked BEFORE the exact-arity test: its
            // spelling carries a series NAME after the keyword, so it has
            // one column more than a constant row, and an arity-first test
            // would report "malformed" for the documented spelling —
            // making this deferral unreachable (the A1a defect).
            if (upper(vtok).rfind("TIMESERIES", 0) == 0) {
                errors.push_back(
                    "[HEAT_SOURCES] TIMESERIES temperatures arrive with a "
                    "later heat phase — H1 takes constant degC.");
                continue;
            }
            if (toks.size() != vpos + 1) {
                errors.push_back(
                    "[HEAT_SOURCES] malformed row: '" + line + "'.");
                continue;
            }
            double degc = 0.0;
            if (!parse_celsius(vtok, degc)) {
                errors.push_back(
                    "[HEAT_SOURCES] '" + toks[0] + "': '" + vtok +
                    "' is not a temperature in degC between -50 and 100.");
                continue;
            }

            if (scope == "GLOBAL") {
                ctx.heat_config.global_temp[src] = degc;
                ctx.heat_config.configured_source[src] = true;
            } else if (scope == "NODE") {
                if (src != static_cast<int>(HeatSource::DWF) &&
                    src != static_cast<int>(HeatSource::EXTERNAL_INFLOW)) {
                    errors.push_back(
                        "[HEAT_SOURCES] NODE scope applies to DWF and "
                        "EXTERNAL_INFLOW in H1; '" + toks[0] +
                        "' takes GLOBAL.");
                    continue;
                }
                const int nd = ctx.node_names.find(toks[2]);
                if (nd < 0) {
                    errors.push_back(
                        "[HEAT_SOURCES] unknown node '" + toks[2] + "'.");
                    continue;
                }
                bool dup = false;
                for (std::size_t i = 0;
                     i < ctx.heat_config.node_over_source.size(); ++i)
                    if (ctx.heat_config.node_over_source[i] == src &&
                        ctx.heat_config.node_over_node[i] == nd) {
                        errors.push_back(
                            "[HEAT_SOURCES] duplicate NODE row for '" +
                            toks[0] + "' at '" + toks[2] + "'.");
                        dup = true;
                        break;
                    }
                if (dup) continue;
                ctx.heat_config.node_over_source.push_back(src);
                ctx.heat_config.node_over_node.push_back(nd);
                ctx.heat_config.node_over_temp.push_back(degc);
            } else {
                errors.push_back(
                    "[HEAT_SOURCES] unknown scope '" + toks[1] +
                    "' (GLOBAL or NODE in H1).");
            }
        }
    }

    if (errors.size() != errors_before) {
        ctx.heat_config = HeatConfigData{};  // never half-apply
        return;
    }
    ctx.heat_config.configured = true;

    // Silent-bypass enumeration (lessons 10/20): every configuration in
    // which this table reaches nothing says so. (ON + IGNORE_QUALITY warns
    // engine-level in SWMMEngine::open — it applies with or without this
    // component.)
    if (!ctx.options.heat_transport) {
        ctx.warnings.push_back(
            "A heat component is configured but [OPTIONS] HEAT_TRANSPORT is "
            "OFF — no temperature is tracked this simulation. Set "
            "HEAT_TRANSPORT ON.");
    }
}

}  // namespace

void registerHeatComponent() {
    components::ProcessComponentRegistry::instance().register_component(
        kHeatId,
        "Heat transport coordinator (H1: [HEAT_SOURCES] GLOBAL + NODE "
        "constant inlet temperatures; flux modules arrive with H2-H4)",
        [](SimulationContext& ctx, const ProcessComponentSpec& /*spec*/,
           const components::ComponentConfigSections& config,
           std::vector<std::string>& errors) {
            applyHeatSections(ctx, config, errors);
        });
}

}  // namespace openswmm::transport
