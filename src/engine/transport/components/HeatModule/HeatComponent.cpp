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
#include <cmath>
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

/// Strict finite number. `std::strtod` ACCEPTS "nan" and "inf", and every
/// range test in this file is written as `v < lo || v > hi` — both of which
/// are FALSE for NaN, so a NaN sails through every guard and lands in the
/// config looking like a value. H6a found this on the new [SOLAR_RADIATION]
/// parser; it is checked here so the new sections cannot reintroduce it.
///
/// @note `parse_celsius` never had the hole — its range test is the
///       conjunctive ACCEPT form (`v >= lo && v <= hi`), which NaN fails.
///       The `[RADIATIVE_FLUXES]` ladder did, via its reject-form `frac()`
///       guard; its parse now comes through here (step 3 validation round).
bool parse_finite(const std::string& tok, double& out) {
    char* end = nullptr;
    out = std::strtod(tok.c_str(), &end);
    if (end == nullptr || *end != '\0' || end == tok.c_str()) return false;
    return std::isfinite(out);
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

    // H6a (D-H6a-3). The three SHORTWAVE spellings are MUTUALLY EXCLUSIVE,
    // so the parser has to remember whether it has seen one. A precedence
    // ladder was the alternative and was rejected: it makes a deck that
    // configures two sources run plausibly while silently discarding one.
    bool shortwave_seen = false;

    // Was an actual FRACTION row given, as opposed to coefficients alone?
    // A [CLOUD_COVER] holding only k/n tunes a cloud fraction of zero,
    // which is no cloud at all — worth saying so.
    bool cloud_fraction_seen = false;

    // Warnings are collected LOCALLY and merged into ctx only if the parse
    // succeeds. Pushing them straight to `ctx.warnings` meant that an error
    // anywhere in model.heat discarded the config via the reset below while
    // the warnings survived — so on a lenient open the user was warned
    // about a configuration that never took effect. Worse, a malformed
    // COMPUTED row left `sw_mode` at CONSTANT and produced the advice
    // "Set SHORTWAVE GLOBAL COMPUTED" to a user who had written exactly
    // that.
    std::vector<std::string> pending_warnings;

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
                auto& rc = ctx.heat_config.radiative;
                const std::string k = upper(toks[0]);

                // H6a. SHORTWAVE is handled BEFORE the numeric parse for the
                // same structural reason [HEAT_FLUXES] checks
                // DRY_ELEMENT_TEMPERATURE before its ON|OFF test: two of its
                // three spellings are keywords, not numbers, and a
                // number-first test would reject them as "not a number"
                // before their own branch could run — making the feature
                // unreachable rather than merely unimplemented. That exact
                // shape was the A1a defect.
                if (k == "SHORTWAVE") {
                    if (shortwave_seen) {
                        errors.push_back(
                            "[RADIATIVE_FLUXES] SHORTWAVE is given more than "
                            "once. The constant, TIMESERIES and COMPUTED "
                            "spellings are mutually exclusive — pick one "
                            "(plan D-H6a-3).");
                        continue;
                    }
                    const std::string spelling = upper(toks[2]);

                    if (spelling == "COMPUTED") {
                        if (toks.size() != 3) {
                            errors.push_back(
                                "[RADIATIVE_FLUXES] SHORTWAVE GLOBAL COMPUTED "
                                "takes no further tokens: '" + line + "'.");
                            continue;
                        }
                        rc.sw_mode = ShortwaveMode::COMPUTED;
                        shortwave_seen = true;
                        continue;
                    }

                    if (spelling == "TIMESERIES") {
                        if (toks.size() != 4) {
                            errors.push_back(
                                "[RADIATIVE_FLUXES] SHORTWAVE GLOBAL "
                                "TIMESERIES takes a series name: '" + line +
                                "'.");
                            continue;
                        }
                        const int ts = ctx.find_timeseries(toks[3]);
                        if (ts < 0) {
                            errors.push_back(
                                "[RADIATIVE_FLUXES] SHORTWAVE TIMESERIES '" +
                                toks[3] + "' is not a [TIMESERIES] in the "
                                "model.");
                            continue;
                        }
                        rc.sw_mode      = ShortwaveMode::TIMESERIES;
                        rc.sw_ts_index  = ts;
                        shortwave_seen  = true;
                        continue;
                    }

                    // Constant — the H3 spelling, still the default.
                    if (toks.size() != 3) {
                        errors.push_back(
                            "[RADIATIVE_FLUXES] malformed SHORTWAVE row: '" +
                            line + "'.");
                        continue;
                    }
                    double sw = 0.0;
                    if (!parse_finite(toks[2], sw)) {
                        errors.push_back(
                            "[RADIATIVE_FLUXES] SHORTWAVE '" + toks[2] +
                            "' is not a finite number, TIMESERIES, or "
                            "COMPUTED.");
                        continue;
                    }
                    if (sw < 0.0) {
                        errors.push_back("[RADIATIVE_FLUXES] SHORTWAVE must "
                                         "be >= 0 W/m2.");
                        continue;
                    }
                    rc.shortwave_wm2 = sw;
                    rc.sw_mode       = ShortwaveMode::CONSTANT;
                    shortwave_seen   = true;
                    continue;
                }

                double v = 0.0;
                if (!parse_finite(toks[2], v)) {
                    errors.push_back("[RADIATIVE_FLUXES] '" + toks[0] +
                                     "': '" + toks[2] +
                                     "' is not a finite number.");
                    continue;
                }
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
                // SHORTWAVE is not in this ladder — it returned above, where
                // its keyword spellings are reachable.
                if      (k == "ALBEDO")            frac(rc.albedo);
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
        // H6a. Site geometry and Bird atmosphere, consulted only under
        // SHORTWAVE ... COMPUTED. Same '<param> GLOBAL <value>' shape as
        // [RADIATIVE_FLUXES] so the two sections read alike.
        if (sec.first == "SOLAR_RADIATION") {
            for (const auto& line : sec.second) {
                const auto toks = tokenize(line);
                if (toks.size() != 3 || upper(toks[1]) != "GLOBAL") {
                    errors.push_back(
                        "[SOLAR_RADIATION] expects '<param> GLOBAL <value>': '"
                        + line + "'.");
                    continue;
                }
                double v = 0.0;
                if (!parse_finite(toks[2], v)) {
                    errors.push_back("[SOLAR_RADIATION] '" + toks[0] + "': '" +
                                     toks[2] + "' is not a finite number.");
                    continue;
                }
                auto& sc = ctx.heat_config.solar;
                const std::string k = upper(toks[0]);

                // Ranges are REFUSED, not clamped — the [RADIATIVE_FLUXES]
                // precedent. A latitude of 100 is a typo, and silently
                // saturating it to 90 would put the model at the pole.
                //
                // `ranged` RETURNS whether it assigned, and the "was this
                // provided" flags are set from that return value. Setting
                // them from a second copy of the range predicate instead
                // was the first spelling, and it disagreed with the
                // assignment for exactly one input class — NaN, which
                // passes `!(v < lo || v > hi)` and so got assigned while
                // the flag read false.
                auto ranged = [&](double lo, double hi, double& dst,
                                  const char* units) -> bool {
                    if (v < lo || v > hi) {
                        errors.push_back("[SOLAR_RADIATION] '" + toks[0] +
                                         "' must be in [" + std::to_string(lo) +
                                         ", " + std::to_string(hi) + "] " +
                                         units + ", got " + toks[2] + ".");
                        return false;
                    }
                    dst = v;
                    return true;
                };
                auto nonneg = [&](double& dst) {
                    if (v < 0.0)
                        errors.push_back("[SOLAR_RADIATION] '" + toks[0] +
                                         "' must be >= 0, got " + toks[2] +
                                         ".");
                    else dst = v;
                };

                if (k == "LATITUDE") {
                    if (ranged(-90.0, 90.0, sc.latitude_deg, "degrees"))
                        sc.has_latitude = true;
                } else if (k == "LONGITUDE") {
                    if (ranged(-180.0, 180.0, sc.longitude_deg, "degrees"))
                        sc.has_longitude = true;
                } else if (k == "TIMEZONE") {
                    // ±14 covers every real offset (Kiribati is +14).
                    if (ranged(-14.0, 14.0, sc.timezone_hours,
                               "hours from UTC"))
                        sc.has_timezone = true;
                } else if (k == "ELEVATION") {
                    // Metres. Below sea level is legal — and the flag, not
                    // the sign, is what marks it as provided, so a negative
                    // elevation is no longer indistinguishable from "unset".
                    if (ranged(-500.0, 9000.0, sc.elevation_m, "metres"))
                        sc.has_elevation = true;
                } else if (k == "TURBIDITY_380")  { nonneg(sc.aod380);
                } else if (k == "TURBIDITY_500")  { nonneg(sc.aod500);
                } else if (k == "PRECIP_WATER")   { nonneg(sc.precip_water_cm);
                } else if (k == "OZONE")          { nonneg(sc.ozone_cm);
                } else if (k == "GROUND_ALBEDO")  {
                    if (v < 0.0 || v > 1.0)
                        errors.push_back("[SOLAR_RADIATION] GROUND_ALBEDO must "
                                         "be a fraction in [0,1], got " +
                                         toks[2] + ".");
                    else sc.ground_albedo = v;
                } else {
                    errors.push_back(
                        "[SOLAR_RADIATION] unknown parameter '" + toks[0] +
                        "' (LATITUDE, LONGITUDE, TIMEZONE, ELEVATION, "
                        "TURBIDITY_380, TURBIDITY_500, PRECIP_WATER, OZONE, "
                        "GROUND_ALBEDO).");
                }
            }
            continue;
        }

        // H6a (D-H6a-2). ONE fraction, TWO modules — shortwave attenuation
        // and the longwave emissivity correction both read it.
        if (sec.first == "CLOUD_COVER") {
            for (const auto& line : sec.second) {
                const auto toks = tokenize(line);
                if (toks.size() < 3 || upper(toks[1]) != "GLOBAL") {
                    errors.push_back(
                        "[CLOUD_COVER] expects '<param> GLOBAL <value>': '" +
                        line + "'.");
                    continue;
                }
                auto& cc = ctx.heat_config.cloud;
                const std::string k = upper(toks[0]);

                if (k == "FRACTION") {
                    if (upper(toks[2]) == "TIMESERIES") {
                        if (toks.size() != 4) {
                            errors.push_back(
                                "[CLOUD_COVER] FRACTION GLOBAL TIMESERIES "
                                "takes a series name: '" + line + "'.");
                            continue;
                        }
                        const int ts = ctx.find_timeseries(toks[3]);
                        if (ts < 0) {
                            errors.push_back(
                                "[CLOUD_COVER] TIMESERIES '" + toks[3] +
                                "' is not a [TIMESERIES] in the model.");
                            continue;
                        }
                        cc.use_timeseries = true;
                        cc.ts_index       = ts;
                        cc.configured     = true;
                        cloud_fraction_seen = true;
                        continue;
                    }
                    if (toks.size() != 3) {
                        errors.push_back("[CLOUD_COVER] malformed FRACTION "
                                         "row: '" + line + "'.");
                        continue;
                    }
                    double c = 0.0;
                    if (!parse_finite(toks[2], c)) {
                        errors.push_back("[CLOUD_COVER] FRACTION '" + toks[2] +
                                         "' is not a finite number or "
                                         "TIMESERIES.");
                        continue;
                    }
                    // Refused, not clamped — a 75 meant as 75% would
                    // otherwise blacken the sky and look deliberate. (The
                    // TIMESERIES path DOES clamp, at runtime: a series value
                    // arrives mid-run where there is no user to ask.)
                    if (c < 0.0 || c > 1.0) {
                        errors.push_back(
                            "[CLOUD_COVER] FRACTION must be in [0,1], got " +
                            toks[2] + " (it is a fraction, not a percent).");
                        continue;
                    }
                    cc.use_timeseries = false;
                    cc.fraction       = c;
                    cc.configured     = true;
                    cloud_fraction_seen = true;
                    continue;
                }

                // Unknown key is checked BEFORE the numeric parse, so a
                // misspelled parameter reports itself rather than reporting
                // its value as "not a number" — the report should name the
                // thing the user can act on.
                if (k != "SW_ATTEN_K" && k != "SW_ATTEN_N" &&
                    k != "LW_CLOUD_K") {
                    errors.push_back(
                        "[CLOUD_COVER] unknown parameter '" + toks[0] +
                        "' (FRACTION, SW_ATTEN_K, SW_ATTEN_N, LW_CLOUD_K).");
                    continue;
                }
                if (toks.size() != 3) {
                    errors.push_back("[CLOUD_COVER] expects '<param> GLOBAL "
                                     "<value>': '" + line + "'.");
                    continue;
                }
                double v = 0.0;
                if (!parse_finite(toks[2], v)) {
                    errors.push_back("[CLOUD_COVER] '" + toks[0] + "': '" +
                                     toks[2] + "' is not a finite number.");
                    continue;
                }
                if (v < 0.0) {
                    errors.push_back("[CLOUD_COVER] '" + toks[0] +
                                     "' must be >= 0, got " + toks[2] + ".");
                    continue;
                }
                if      (k == "SW_ATTEN_K") cc.sw_atten_k = v;
                else if (k == "SW_ATTEN_N") cc.sw_atten_n = v;
                else                        cc.lw_cloud_k = v;
                // Marked configured on a COEFFICIENT row too, matching
                // `swmm_heat_set_cloud`. Without this a section holding only
                // coefficients left `configured` false, `updateSolarForcing`
                // skipped the whole cloud read, and the section did nothing
                // at all — with no error and no warning. The
                // no-FRACTION-given case is warned about below; it is a
                // different complaint from "the section was ignored".
                cc.configured = true;
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
                } else if (mod == "LAYER_CONDUCTION") {
                    ctx.heat_config.layer_conduction = on;
                } else if (mod == "SEDIMENT_EXCHANGE") {
                    errors.push_back(
                        "[HEAT_FLUXES] SEDIMENT_EXCHANGE arrives with plan "
                        "phase H6 (HTS two-layer storage). H4 is the ARD "
                        "mesh binding.");
                } else {
                    errors.push_back(
                        "[HEAT_FLUXES] unknown module '" + toks[0] +
                        "' (SURFACE_EXCHANGE in H2, RADIATIVE_EXCHANGE in "
                        "H3, DRY_ELEMENT_TEMPERATURE in H5a, "
                        "LAYER_CONDUCTION in H5b).");
                }
            }
            continue;
        }
        if (sec.first != "HEAT_SOURCES") {
            errors.push_back(
                "model.heat: unknown section [" + sec.first +
                "] (recognized: [HEAT_SOURCES], [HEAT_FLUXES], "
                "[RADIATIVE_FLUXES], [SOLAR_RADIATION], [CLOUD_COVER]). "
                "Sediment sections arrive with H6b.");
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

    // ---- H6a cross-section checks. These need every section parsed, so
    //      they cannot live inside the loop above.
    {
        const auto& rc = ctx.heat_config.radiative;
        const auto& sc = ctx.heat_config.solar;

        // D-H6a / plan §2.5 trap 1. COMPUTED without coordinates must be an
        // ERROR, never a default. `ClimateState::latitude` is the
        // [TEMPERATURE] SNOWMELT field: it defaults to 0 and is written only
        // by decks carrying that line, so borrowing it would silently model
        // equatorial noon — a plausible wrong answer, which is the only kind
        // that survives review.
        if (rc.sw_mode == ShortwaveMode::COMPUTED) {
            if (!sc.has_latitude || !sc.has_longitude)
                errors.push_back(
                    "[RADIATIVE_FLUXES] SHORTWAVE GLOBAL COMPUTED requires "
                    "[SOLAR_RADIATION] LATITUDE and LONGITUDE. They are NOT "
                    "taken from the [TEMPERATURE] SNOWMELT line: that "
                    "latitude defaults to 0 and its longitude field is a "
                    "solar-time correction in minutes, not a longitude "
                    "(plan §2.5).");
        } else if (ctx.heat_config.solar.has_latitude ||
                   ctx.heat_config.solar.has_longitude) {
            // Not an error — the section is harmless — but silence here is
            // exactly the bypass lessons 10/20 are about: the user wrote
            // coordinates and nothing reads them.
            pending_warnings.push_back(
                "[SOLAR_RADIATION] is configured but [RADIATIVE_FLUXES] "
                "SHORTWAVE is not COMPUTED, so the site coordinates are "
                "unused. Set SHORTWAVE GLOBAL COMPUTED to use them.");
        }

        // Double-counting: a measured pyranometer series already contains
        // the clouds that were over it. Attenuating it again is a real
        // modelling error, but it is not unambiguously one — a user may be
        // scaling a clear-sky-corrected record on purpose — so it warns.
        if (rc.sw_mode == ShortwaveMode::TIMESERIES &&
            ctx.heat_config.cloud.configured) {
            pending_warnings.push_back(
                "[CLOUD_COVER] is applied on top of a measured SHORTWAVE "
                "TIMESERIES. A measured record already contains its clouds, "
                "so this attenuates them twice. Drop [CLOUD_COVER] unless the "
                "series is explicitly clear-sky. (The longwave cloud "
                "correction still applies either way.)");
        }

        // Coefficients with nothing to scale. Both cloud factors are
        // identities at C = 0, so this section changes nothing.
        if (ctx.heat_config.cloud.configured && !cloud_fraction_seen) {
            pending_warnings.push_back(
                "[CLOUD_COVER] sets coefficients but no FRACTION, so the "
                "cloud fraction is 0 and both cloud corrections are the "
                "identity — the section has no effect. Add 'FRACTION GLOBAL "
                "<C>' or 'FRACTION GLOBAL TIMESERIES <name>'.");
        }

        // An omitted TIMEZONE is a legal 0 (UTC), and it is also the single
        // largest error available in this module: it shifts the whole
        // diurnal curve by up to 12 h, dwarfing the 0.1° position accuracy
        // D-H6a-4 spends its argument on. Not an error — a deck really may
        // mean UTC — but never silent.
        if (rc.sw_mode == ShortwaveMode::COMPUTED && !sc.has_timezone) {
            pending_warnings.push_back(
                "[SOLAR_RADIATION] has no TIMEZONE, so local time is taken "
                "as UTC. If the deck's clock is local, the computed solar "
                "day is shifted by the site's offset — up to 12 hours, "
                "which is larger than every other error in this module. "
                "Add 'TIMEZONE GLOBAL <hours from UTC>'.");
        }
    }

    if (errors.size() != errors_before) {
        ctx.heat_config = HeatConfigData{};  // never half-apply
        return;                              // ...and never half-warn either:
                                             // pending_warnings dies here.
    }
    ctx.heat_config.configured = true;

    // The parse stood, so its warnings are about a configuration that
    // actually took effect.
    for (auto& w : pending_warnings)
        ctx.warnings.push_back(std::move(w));

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
        "Heat transport coordinator ([HEAT_SOURCES] inlet temperatures, "
        "[HEAT_FLUXES] module toggles, [RADIATIVE_FLUXES] parameters, and "
        "H6a's [SOLAR_RADIATION] + [CLOUD_COVER] shortwave forcing)",
        [](SimulationContext& ctx, const ProcessComponentSpec& /*spec*/,
           const components::ComponentConfigSections& config,
           std::vector<std::string>& errors) {
            applyHeatSections(ctx, config, errors);
        });
}

}  // namespace openswmm::transport
