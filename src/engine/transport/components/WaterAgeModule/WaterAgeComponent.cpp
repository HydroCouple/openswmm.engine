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
 * @file WaterAgeComponent.cpp
 * @brief waterage component apply hook — phase A1a body.
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "WaterAgeComponent.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>
#include <vector>

#include "../../../core/SimulationContext.hpp"
#include "../../../plugins/ProcessComponentRegistry.hpp"

namespace openswmm::transport {

namespace {

constexpr const char* kAgeId = "org.hydrocouple.openswmm.waterage";

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

bool parse_hours(const std::string& tok, double& out) {
    // D-NS1 (X6): negative hours are now legal — a negative source age is
    // age-volume EXTRACTION (warned at the call site). The parse rejects
    // only non-numbers.
    char* end = nullptr;
    out = std::strtod(tok.c_str(), &end);
    return end != nullptr && *end == '\0' && end != tok.c_str();
}

int source_of(const std::string& u) {
    if (u == "RAINFALL")        return static_cast<int>(WaterAgeSource::RAINFALL);
    if (u == "DWF")             return static_cast<int>(WaterAgeSource::DWF);
    if (u == "GW")              return static_cast<int>(WaterAgeSource::GW);
    if (u == "RDII")            return static_cast<int>(WaterAgeSource::RDII);
    if (u == "EXTERNAL_INFLOW") return static_cast<int>(WaterAgeSource::EXTERNAL_INFLOW);
    if (u == "IFACE")           return static_cast<int>(WaterAgeSource::IFACE);
    if (u == "INITIAL_STATE")   return static_cast<int>(WaterAgeSource::INITIAL_STATE);
    return -1;
}

void applyWaterAgeSections(SimulationContext& ctx,
                           const components::ComponentConfigSections& config,
                           std::vector<std::string>& errors) {
    ctx.water_age_config = WaterAgeConfigData{};  // reopen hygiene
    const std::size_t errors_before = errors.size();

    for (const auto& sec : config.sections) {
        if (sec.first != "WATER_AGE_SOURCES") {
            errors.push_back(
                "model.age: unknown section [" + sec.first +
                "] (A1a recognizes: [WATER_AGE_SOURCES]).");
            continue;
        }
        for (const auto& line : sec.second) {
            const auto toks = tokenize(line);
            if (toks.size() < 3) {
                errors.push_back(
                    "[WATER_AGE_SOURCES] expects '<source> <scope> [name] "
                    "<hours>': '" + line + "'.");
                continue;
            }
            const int src = source_of(upper(toks[0]));
            if (src < 0) {
                errors.push_back(
                    "[WATER_AGE_SOURCES] unknown source '" + toks[0] +
                    "' (RAINFALL, DWF, GW, RDII, EXTERNAL_INFLOW, IFACE, "
                    "INITIAL_STATE).");
                continue;
            }
            const std::string scope = upper(toks[1]);

            // Deferral surface — precise phase names, never silence.
            if (scope == "SUBCATCH") {
                errors.push_back(
                    "[WATER_AGE_SOURCES] SUBCATCH scope arrives with plan "
                    "phase A3 (watershed age states).");
                continue;
            }
            if (scope == "EDGE_BC") {
                errors.push_back(
                    "[WATER_AGE_SOURCES] EDGE_BC scope arrives with phase "
                    "T6 (2D transport).");
                continue;
            }
            // The value token sits after the scope's optional name. Bind it
            // only once the row is known to HAVE that column: reading
            // toks[3] before the arity check was an out-of-bounds read on
            // any 3-token NODE row ('DWF NODE J0', an age omitted by hand).
            const bool has_name = (scope == "NODE");
            const std::size_t vpos = has_name ? 3u : 2u;
            if (toks.size() <= vpos) {
                errors.push_back(
                    "[WATER_AGE_SOURCES] malformed row: '" + line + "'.");
                continue;
            }
            const std::string& vtok = toks[vpos];
            // TIMESERIES is checked BEFORE the exact-arity test because its
            // documented spelling carries a series NAME after the keyword
            // ('EXTERNAL_INFLOW NODE N4 TIMESERIES age_ts', plan §2) and so
            // has one column more than a constant row. Testing arity first
            // reported "malformed row" for the very spelling the plan
            // documents, and the deferral this phase owes the user was
            // unreachable in the only form anyone would write.
            if (upper(vtok).rfind("TIMESERIES", 0) == 0) {
                // Z1 (amendment D-Y4) FLIPPED this from a deferral to a
                // redirect: time-varying ages ARE supported now, through
                // the pathway that already speaks timeseries.
                errors.push_back(
                    "[WATER_AGE_SOURCES] TIMESERIES ages are prescribed as "
                    "[INFLOWS] rows naming __WATER_AGE__ (amendment D-Y4 / "
                    "Z1) — this table takes constant hours.");
                continue;
            }
            if (toks.size() != vpos + 1) {
                errors.push_back(
                    "[WATER_AGE_SOURCES] malformed row: '" + line + "'.");
                continue;
            }
            double hours = 0.0;
            if (!parse_hours(vtok, hours)) {
                errors.push_back(
                    "[WATER_AGE_SOURCES] '" + toks[0] + "': '" + vtok +
                    "' is not an age in hours.");
                continue;
            }
            // D-NS1 (X6): a negative source age is age-volume EXTRACTION —
            // legal, warned at parse per the subplan §3.1 contract.
            if (hours < 0.0)
                ctx.warnings.push_back(
                    "[WATER_AGE_SOURCES] '" + toks[0] + "' is negative (" +
                    vtok + " h): the pathway EXTRACTS age-volume (water "
                    "reads younger). Extraction clamps so age never goes "
                    "below zero (D-NS1).");
            const double seconds = hours * 3600.0;

            if (scope == "GLOBAL") {
                ctx.water_age_config.global_age[src] = seconds;
            } else if (scope == "NODE") {
                if (src != static_cast<int>(WaterAgeSource::DWF) &&
                    src != static_cast<int>(WaterAgeSource::EXTERNAL_INFLOW)) {
                    errors.push_back(
                        "[WATER_AGE_SOURCES] NODE scope applies to DWF and "
                        "EXTERNAL_INFLOW in A1a; '" + toks[0] +
                        "' takes GLOBAL.");
                    continue;
                }
                const int nd = ctx.node_names.find(toks[2]);
                if (nd < 0) {
                    errors.push_back(
                        "[WATER_AGE_SOURCES] unknown node '" + toks[2] +
                        "'.");
                    continue;
                }
                bool dup = false;
                for (std::size_t i = 0;
                     i < ctx.water_age_config.node_over_source.size(); ++i)
                    if (ctx.water_age_config.node_over_source[i] == src &&
                        ctx.water_age_config.node_over_node[i] == nd) {
                        errors.push_back(
                            "[WATER_AGE_SOURCES] duplicate NODE row for '" +
                            toks[0] + "' at '" + toks[2] + "'.");
                        dup = true;
                        break;
                    }
                if (dup) continue;
                ctx.water_age_config.node_over_source.push_back(src);
                ctx.water_age_config.node_over_node.push_back(nd);
                ctx.water_age_config.node_over_age.push_back(seconds);
            } else {
                errors.push_back(
                    "[WATER_AGE_SOURCES] unknown scope '" + toks[1] +
                    "' (GLOBAL or NODE in A1a).");
            }
        }
    }

    if (errors.size() != errors_before) {
        ctx.water_age_config = WaterAgeConfigData{};  // never half-apply
        return;
    }
    ctx.water_age_config.configured = true;

    // Silent-bypass enumeration (lessons 10/20): every configuration in
    // which this table reaches nothing says so.
    // (The ON + IGNORE_QUALITY case warns engine-level in SWMMEngine::open
    // — it applies with or without this component.)
    if (!ctx.options.water_age) {
        ctx.warnings.push_back(
            "A waterage component is configured but [OPTIONS] WATER_AGE is "
            "OFF — no age is tracked this simulation. Set WATER_AGE ON.");
    }
}

}  // namespace

void registerWaterAgeComponent() {
    components::ProcessComponentRegistry::instance().register_component(
        kAgeId,
        "Water age tracking coordinator (A1a: [WATER_AGE_SOURCES] GLOBAL + "
        "NODE constant ages)",
        [](SimulationContext& ctx, const ProcessComponentSpec& /*spec*/,
           const components::ComponentConfigSections& config,
           std::vector<std::string>& errors) {
            applyWaterAgeSections(ctx, config, errors);
        });
}

}  // namespace openswmm::transport
