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
 * @file      OptionsHandler.cpp
 * @brief     [OPTIONS] section handler for the new engine.
 *
 * @details Parses key-value pairs from the [OPTIONS] section and populates
 *          `SimulationContext::options` (a SimulationOptions struct).
 *
 *          Unknown keys are stored in `options.ext_options` with a
 *          SWMM_WARN_UNKNOWN_OPTION warning (R05).
 *
 *          A value outside the keyword list of a legacy option (FLOW_UNITS,
 *          INFILTRATION, FLOW_ROUTING, LINK_OFFSETS, FORCE_MAIN_EQUATION,
 *          INERTIAL_DAMPING, NORMAL_FLOW_LIMITED, SURCHARGE_METHOD and the
 *          YES/NO flags) refuses the deck with ERR_KEYWORD (205), as legacy
 *          project_readOption does. This engine's own keys stay lenient.
 *
 *          The special key `CRS` is stored in both `options.crs` and
 *          `spatial.crs` (R06).
 *
 * Key mapping (standard SWMM 5.x keys only):
 *
 *  FLOW_UNITS           → options.flow_units
 *  INFILTRATION         → options.infiltration
 *  FLOW_ROUTING         → options.routing_model
 *  LINK_OFFSETS         → options.link_offsets (0 = DEPTH, 1 = ELEVATION)
 *  MIN_SLOPE            → options.min_slope
 *  ALLOW_PONDING        → options.allow_ponding
 *  SKIP_STEADY_STATE    → options.skip_steady_state
 *  START_DATE           → options.start_date (OADate (days since 12/30/1899))
 *  START_TIME           → combined with START_DATE
 *  END_DATE             → options.end_date
 *  END_TIME             → combined with END_DATE
 *  REPORT_START_DATE    → options.report_start
 *  REPORT_START_TIME    → combined with REPORT_START_DATE
 *  SWEEP_START          → options.sweep_start (MM/DD → day-of-year)
 *  SWEEP_END            → options.sweep_end (MM/DD → day-of-year)
 *  DRY_DAYS             → options.dry_days
 *  REPORT_STEP          → options.report_step (HH:MM:SS or seconds)
 *  WET_STEP             → options.wet_step
 *  DRY_STEP             → options.dry_step
 *  ROUTING_STEP         → options.routing_step
 *  RULE_STEP            → options.rule_step (seconds; 0 = use routing step)
 *  MAX_TRIALS           → options.max_trials
 *  HEAD_TOLERANCE       → options.head_tol
 *  SYS_FLOW_TOL         → options.sys_flow_tol
 *  LAT_FLOW_TOL         → options.lat_flow_tol
 *  VARIABLE_STEP        → options.variable_step (Courant fraction for variable timestep)
 *  MINIMUM_STEP         → options.min_routing_step
 *  THREADS              → options.num_threads (parallel solver thread count)
 *  IGNORE_RAINFALL      → options.ignore_rainfall
 *  IGNORE_SNOWMELT      → options.ignore_snow_melt
 *  IGNORE_GROUNDWATER   → options.ignore_groundwater
 *  IGNORE_RDII          → options.ignore_rdii
 *  IGNORE_ROUTING       → options.ignore_routing
 *  IGNORE_QUALITY       → options.ignore_quality
 *  CRS                  → options.crs + spatial.crs (R06)
 *
 * @see Legacy reference: src/solver/input.c — readOption()
 * @see SimulationOptions.hpp
 * @ingroup engine_input
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include "OptionsHandler.hpp"

#include "../Tokenizer.hpp"
#include "../../core/SimulationContext.hpp"
#include "../../core/DateTime.hpp"

#include "../InputParseUtils.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace openswmm::input {

// ============================================================================
// Helper: normalize uppercase key
// ============================================================================

/**
 * @brief Copy and uppercase an option token using tokenizer case rules.
 *
 * Uses Tokenizer::to_upper locale behavior; allocation may throw.
 *
 * @param[in] sv Borrowed bytes; may be empty.
 * @return Owned uppercase token, with no trimming, prefix matching or numeric parsing.
 */
static std::string norm(std::string_view sv) {
    return Tokenizer::to_upper(sv);
}

// legacy findmatch / match (input.c): a keyword matches when it is a
// case-insensitive PREFIX of the token — `STEADYFLOW` is STEADY, `DYNWAVE2`
// DYNWAVE. `tok` is already upper-cased by norm().
/**
 * @brief Test whether a normalized token begins with a legacy keyword.
 *
 * No case folding here; e.g. STEADYFLOW matches STEADY.
 *
 * @param[in] tok Borrowed already-uppercase token.
 * @param[in] keyword Non-NULL NUL-terminated uppercase keyword.
 * @return True when the entire keyword is a prefix of tok; suffixes are allowed, abbreviations are
 *     not.
 */
static bool legacyPrefix(const std::string& tok, const char* keyword) {
    const std::size_t n = std::strlen(keyword);
    return tok.size() >= n && tok.compare(0, n, keyword) == 0;
}

// legacy OptionWords (keywords.c): project_readOption warns only for a key
// none of these prefixes.
/**
 * @brief Test whether an uppercased option key has a legacy-recognized prefix.
 *
 * Controls compatibility warning policy, not whether this engine applies that option or accepts its
 * value.
 *
 * @param[in] key Already-normalized option key.
 * @return True for any configured legacy keyword prefix, false otherwise.
 */
static bool legacy_knows_option(const std::string& key) {
    static const char* const kWords[] = {"FLOW_UNITS",
                                         "INFILTRATION",
                                         "FLOW_ROUTING",
                                         "START_DATE",
                                         "START_TIME",
                                         "END_DATE",
                                         "END_TIME",
                                         "REPORT_START_DATE",
                                         "REPORT_START_TIME",
                                         "SWEEP_START",
                                         "SWEEP_END",
                                         "DRY_DAYS",
                                         "WET_STEP",
                                         "DRY_STEP",
                                         "ROUTING_STEP",
                                         "RULE_STEP",
                                         "REPORT_STEP",
                                         "ALLOW_PONDING",
                                         "INERTIAL_DAMPING",
                                         "SLOPE_WEIGHTING",
                                         "VARIABLE_STEP",
                                         "NORMAL_FLOW_LIMITED",
                                         "LENGTHENING_STEP",
                                         "MIN_SURFAREA",
                                         "COMPATIBILITY",
                                         "SKIP_STEADY_STATE",
                                         "TEMPDIR",
                                         "IGNORE_RAINFALL",
                                         "FORCE_MAIN_EQUATION",
                                         "LINK_OFFSETS",
                                         "MIN_SLOPE",
                                         "IGNORE_SNOWMELT",
                                         "IGNORE_GROUNDWATER",
                                         "IGNORE_ROUTING",
                                         "IGNORE_QUALITY",
                                         "MAX_TRIALS",
                                         "HEAD_TOLERANCE",
                                         "SYS_FLOW_TOL",
                                         "LAT_FLOW_TOL",
                                         "IGNORE_RDII",
                                         "MINIMUM_STEP",
                                         "THREADS",
                                         "SURCHARGE_METHOD",
                                         "OUTFALL_BACKFLOW_QUALITY"};
    for (const char* w : kWords)
        if (legacyPrefix(key, w))
            return true;
    return false;
}

// legacy keywords.c word lists for the [OPTIONS] values project_readOption
// resolves with findmatch, refusing the deck with ERR_KEYWORD when none
// matches. Where no value table follows, the option stores the word's index.
static const char* const kFlowUnitWords[] = {"CFS", "GPM", "MGD", "CMS", "LPS", "MLD"};
static constexpr FlowUnits kFlowUnitValues[] = {FlowUnits::CFS, FlowUnits::GPM, FlowUnits::MGD,
                                                FlowUnits::CMS, FlowUnits::LPS, FlowUnits::MLD};
static const char* const kInfilModelWords[] = {"HORTON", "MODIFIED_HORTON", "GREEN_AMPT",
                                               "MODIFIED_GREEN_AMPT", "CURVE_NUMBER"};
static constexpr InfiltrationModel kInfilModelValues[] = {
    InfiltrationModel::HORTON, InfiltrationModel::MOD_HORTON, InfiltrationModel::GREEN_AMPT,
    InfiltrationModel::MOD_GREEN_AMPT, InfiltrationModel::CURVE_NUMBER};
static const char* const kLinkOffsetWords[] = {"DEPTH", "ELEVATION"};
static const char* const kForceMainEqnWords[] = {"H-W", "D-W"};
static const char* const kInertDampingWords[] = {"NONE", "PARTIAL", "FULL"};
// Index 3 is legacy NONE, which this engine calls NEITHER.
static const char* const kNormalFlowWords[] = {"SLOPE", "FROUDE", "BOTH", "NONE"};
static const char* const kSurchargeWords[] = {"EXTRAN", "SLOT"};
static const char* const kNoYesWords[] = {"NO", "YES"};

/**
 * @brief Find the first legacy keyword that is a prefix of a token (legacy findmatch).
 *
 * @param[in] tok Borrowed already-uppercase token.
 * @param[in] words Legacy word list, searched in order.
 * @return Index of the first word legacyPrefix() accepts, or -1 when none does (legacy then
 *     reports ERR_KEYWORD).
 */
template <std::size_t N>
static int legacyFindmatch(const std::string& tok, const char* const (&words)[N]) {
    for (std::size_t i = 0; i < N; ++i)
        if (legacyPrefix(tok, words[i]))
            return static_cast<int>(i);
    return -1;
}

/**
 * @brief Read a legacy YES/NO option value as legacy or this engine spells it.
 *
 * Accepts this engine's TRUE/FALSE/1/0 and legacy NoYesWords by prefix (YES, NO; NOPE reads as
 * NO, as legacy findmatch does).
 *
 * @param[in] val Borrowed option value, any case.
 * @param[out] out Set only when the value is recognised; otherwise left unchanged.
 * @return True when recognised; false when the caller must report ERR_KEYWORD.
 */
static bool parseLegacyYesNo(const std::string& val, bool& out) {
    const std::string v = norm(val);
    if (v == "TRUE" || v == "1") {
        out = true;
        return true;
    }
    if (v == "FALSE" || v == "0") {
        out = false;
        return true;
    }
    const int m = legacyFindmatch(v, kNoYesWords);
    if (m < 0)
        return false;
    out = (m == 1);
    return true;
}

/** @brief Match legacy getDouble: strtod must consume the complete value. */
static bool legacyNumber(const std::string& val, double& out) {
    char* end = nullptr;
    out = std::strtod(val.c_str(), &end);
    return *end == '\0';
}

/** @brief Apply a legacy numeric option's own bounds, preserving the value on refusal. */
static void readLegacyNumber(SimulationContext& ctx, const std::string& val, double& out,
                             double lower = -std::numeric_limits<double>::infinity(),
                             double upper = std::numeric_limits<double>::infinity(),
                             bool exclusive_upper = false) {
    double value = 0.0;
    if (!legacyNumber(val, value) || value < lower || value > upper ||
        (exclusive_upper && value == upper))
        ctx.errors.push_back(format_error(ERR_NUMBER, val));
    else
        out = value;
}

/** @brief Match datetime_strToDate in the legacy input reader's M/D/Y format. */
static bool legacyDate(const std::string& val, double& out) {
    if (val.find_first_of("/-") == std::string::npos)
        return false;
    int year = 0, month = 0, day = 0;
    char sep1 = 0, sep2 = 0, month_text[4] = {};
    if (std::sscanf(val.c_str(), "%d%c%d%c%d", &month, &sep1, &day, &sep2, &year) < 3) {
        month = 0;
        if (std::sscanf(val.c_str(), "%3s%c%d%c%d", month_text, &sep1, &day, &sep2, &year) < 3)
            return false;
    }
    if (month == 0) {
        static const char* const months[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                             "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
        for (int i = 0; i < 12; ++i)
            if (norm(month_text) == months[i]) {
                month = i + 1;
                break;
            }
    }
    const double date = datetime::encodeDate(year, month, day);
    if (date == -datetime::DateDelta)
        return false;
    out = date;
    return true;
}

/** @brief Match legacy datetime_strToTime: decimal hours or permissive H[:M[:S]]. */
static bool legacyTime(const std::string& val, double& days) {
    double hours = 0.0;
    if (legacyNumber(val, hours)) {
        days = hours / 24.0;
        return true;
    }
    int h = 0, m = 0, s = 0;
    if (std::sscanf(val.c_str(), "%d:%d:%d", &h, &m, &s) < 1 || h < 0 || m < 0 || s < 0)
        return false;
    days = (3600.0 * h + 60.0 * m + s) / datetime::SecsPerDay;
    return true;
}

/** @brief Decode a legacy time step to whole seconds, including multi-day durations. */
static double legacyStepSeconds(double days) {
    int h = 0, m = 0, s = 0;
    datetime::decodeTime(days, h, m, s);
    return s + 60.0 * m + 3600.0 * h + 86400.0 * std::trunc(days);
}

// ============================================================================
// handle_options() — registered as built-in handler for "OPTIONS"
// ============================================================================

void handle_options(SimulationContext& ctx, const std::vector<std::string>& lines) {
    SimulationOptions& opt = ctx.options;

    // We need to accumulate START_DATE/START_TIME because they may appear
    // in any order relative to each other.
    double start_date_part = 0.0, start_time_part = 0.0;
    double end_date_part = 0.0, end_time_part = 0.0;
    double rpt_date_part = 0.0, rpt_time_part = 0.0;
    bool got_start_date = false, got_start_time = false;
    bool got_end_date = false, got_end_time = false;
    bool got_rpt_date = false, got_rpt_time = false;

    for (const auto& raw : lines) {
        auto tokens = Tokenizer::tokenize(raw);
        if (tokens.size() < 2)
            continue;

        const std::string key = norm(tokens[0]);
        // Value may be a single token or a multi-token (e.g., quoted CRS string).
        // Reconstruct value as original suffix for tokens[1..end]
        const std::string& val = tokens[1];

        // -----------------------------------------------------------------
        // Flow / routing units
        // -----------------------------------------------------------------
        if (key == "FLOW_UNITS") {
            const std::string uv = norm(val);
            if (uv == "CFS")
                opt.flow_units = FlowUnits::CFS;
            else if (uv == "GPM")
                opt.flow_units = FlowUnits::GPM;
            else if (uv == "MGD")
                opt.flow_units = FlowUnits::MGD;
            else if (uv == "CMS")
                opt.flow_units = FlowUnits::CMS;
            else if (uv == "LPS")
                opt.flow_units = FlowUnits::LPS;
            else if (uv == "MLD")
                opt.flow_units = FlowUnits::MLD;
            // Legacy findmatch also takes a keyword followed by anything (LPSX
            // reads as LPS) and refuses the rest. An unknown unit used to run
            // silently as the CFS default (FLOW_UNITS FURLONGS opened).
            else if (const int m = legacyFindmatch(uv, kFlowUnitWords); m >= 0)
                opt.flow_units = kFlowUnitValues[m];
            else
                ctx.errors.push_back(format_error(ERR_KEYWORD, val));

        } else if (key == "INFILTRATION") {
            const std::string iv = norm(val);
            if (iv == "HORTON")
                opt.infiltration = InfiltrationModel::HORTON;
            else if (iv == "MOD_HORTON" || iv == "MODIFIED_HORTON")
                opt.infiltration = InfiltrationModel::MOD_HORTON;
            else if (iv == "GREEN_AMPT")
                opt.infiltration = InfiltrationModel::GREEN_AMPT;
            // Legacy InfilModelWords spells it MODIFIED_GREEN_AMPT (text.h
            // w_MOD_GREEN_AMPT); MOD_GREEN_AMPT is this engine's own alias.
            // The deck keyword was mapped to plain GREEN_AMPT — 11 corpus
            // decks ran the wrong model (F reset between events).
            else if (iv == "MOD_GREEN_AMPT" || iv == "MODIFIED_GREEN_AMPT")
                opt.infiltration = InfiltrationModel::MOD_GREEN_AMPT;
            else if (iv == "CURVE_NUMBER")
                opt.infiltration = InfiltrationModel::CURVE_NUMBER;
            else if (const int m = legacyFindmatch(iv, kInfilModelWords); m >= 0)
                opt.infiltration = kInfilModelValues[m];
            else
                ctx.errors.push_back(format_error(ERR_KEYWORD, val));

        } else if (key == "FLOW_ROUTING") {
            const std::string rv = norm(val);
            // legacy project_readOption ROUTE_MODEL: findmatch over
            // RouteModelWords {NONE, STEADY, KINWAVE, XKINWAVE, DYNWAVE}, then
            // OldRouteModelWords {NONE, NF, KW, EKW, DW}, each a prefix test
            // in that order (routing-steadyflow writes `STEADYFLOW`, which
            // ran as the dynamic wave here). XKINWAVE / EKW are the kinematic
            // wave; NONE ignores routing.
            if (legacyPrefix(rv, "NONE"))
                opt.ignore_routing = true;
            else if (legacyPrefix(rv, "STEADY"))
                opt.routing_model = RoutingModel::STEADY;
            else if (legacyPrefix(rv, "KINWAVE"))
                opt.routing_model = RoutingModel::KINWAVE;
            else if (legacyPrefix(rv, "XKINWAVE"))
                opt.routing_model = RoutingModel::KINWAVE;
            else if (legacyPrefix(rv, "DYNWAVE"))
                opt.routing_model = RoutingModel::DYNWAVE;
            else if (legacyPrefix(rv, "NF"))
                opt.routing_model = RoutingModel::STEADY;
            else if (legacyPrefix(rv, "KW"))
                opt.routing_model = RoutingModel::KINWAVE;
            else if (legacyPrefix(rv, "EKW"))
                opt.routing_model = RoutingModel::KINWAVE;
            else if (legacyPrefix(rv, "DW"))
                opt.routing_model = RoutingModel::DYNWAVE;
            // This engine's own spellings.
            else if (rv == "KINEMATIC_WAVE")
                opt.routing_model = RoutingModel::KINWAVE;
            else if (rv == "DYNAMIC_WAVE")
                opt.routing_model = RoutingModel::DYNWAVE;
            else if (rv == "FV" || rv == "FINITE_VOLUME")
                opt.routing_model = RoutingModel::FV;
            else if (rv == "NO_ROUTING")
                opt.ignore_routing = true;
            else
                ctx.errors.push_back(format_error(ERR_KEYWORD, val));

        } else if (key == "QUALITY_SOLVER") {
            // Unified Transport suite, master plan D-UT6. LAGRANGIAN selects
            // the LARD dispatch (X1 wiring — a warned no-op until X2 lands
            // transport; LARD_AGE_EXPEDITE_SUBPLAN_2026-08-23.md). Unknown
            // values still fall to ext_options for diagnostics.
            const std::string qv = norm(val);
            if (qv == "LEGACY")
                opt.quality_solver = QualitySolverKind::LEGACY;
            else if (qv == "EULERIAN_ARD" || qv == "ARD")
                opt.quality_solver = QualitySolverKind::EULERIAN_ARD;
            else if (qv == "LAGRANGIAN" || qv == "LARD")
                opt.quality_solver = QualitySolverKind::LAGRANGIAN;
            else
                opt.ext_options[key] = val;

        } else if (key == "WATER_AGE") {
            // Water age plan §1 (phase A1a): transported age tracking.
            opt.water_age = (norm(val) == "ON" || norm(val) == "YES");

        } else if (key == "OUTFALL_BACKFLOW_QUALITY") {
            // Boundary re-entry quality at outfalls. LAST = legacy held
            // state (the default); ZERO = fresh boundary — a supplying
            // outfall's held state reads zero for pollutants and
            // __WATER_AGE__ alike. Match the legacy keyword prefixes.
            const std::string bv = norm(val);
            if (legacyPrefix(bv, "LAST"))
                opt.outfall_backflow_zero = false;
            else if (legacyPrefix(bv, "ZERO"))
                opt.outfall_backflow_zero = true;
            else
                ctx.errors.push_back(format_error(ERR_KEYWORD, val));

        } else if (key == "HEAT_TRANSPORT") {
            // Heat plan §1 (phase H1): transported temperature.
            opt.heat_transport = (norm(val) == "ON" || norm(val) == "YES");

            // Heat plan §1/§2.1 (phase H2) — energy bookkeeping and the
            // SurfaceExchange coefficients, CSH Table 4.1 names and defaults.
        } else if (key == "WATER_DENSITY") {
            openswmm::from_chars_double(val.data(), val.data() + val.size(), opt.water_density);
        } else if (key == "WATER_SPECIFIC_HEAT_CAPACITY") {
            openswmm::from_chars_double(val.data(), val.data() + val.size(),
                                        opt.water_specific_heat);
        } else if (key == "WIND_FUNC_COEFF_A") {
            openswmm::from_chars_double(val.data(), val.data() + val.size(), opt.wind_func_coeff_a);
        } else if (key == "WIND_FUNC_COEFF_B") {
            openswmm::from_chars_double(val.data(), val.data() + val.size(), opt.wind_func_coeff_b);
        } else if (key == "PRESSURE_RATIO") {
            openswmm::from_chars_double(val.data(), val.data() + val.size(), opt.pressure_ratio);

            // -----------------------------------------------------------------
            // Timesteps
            // -----------------------------------------------------------------
        } else if (key == "ROUTING_STEP") {
            double step = 0.0, days = 0.0;
            if (!legacyNumber(val, step)) {
                if (!legacyTime(val, days) || !std::isfinite(days) ||
                    std::abs(days) > std::numeric_limits<int>::max() / datetime::SecsPerDay) {
                    ctx.errors.push_back(format_error(ERR_NUMBER, val));
                    continue;
                }
                step = legacyStepSeconds(days);
            }
            if (step <= 0.0)
                ctx.errors.push_back(format_error(ERR_NUMBER, val));
            else
                opt.routing_step = step;
        } else if (key == "MINIMUM_STEP") {
            readLegacyNumber(ctx, val, opt.min_routing_step, 0.0);
        } else if (key == "DRY_DAYS") {
            // project_readOption deliberately uses atof, accepting numeric prefixes and junk as 0.
            const double value = std::atof(val.c_str());
            if (value < 0.0)
                ctx.errors.push_back(format_error(ERR_NUMBER, val));
            else
                opt.dry_days = value;
        } else if (key == "DRY_STEP" || key == "WET_STEP" || key == "REPORT_STEP" ||
                   key == "RULE_STEP") {
            double days = 0.0;
            if (!legacyTime(val, days) || !std::isfinite(days) ||
                std::abs(days) > std::numeric_limits<int>::max() / datetime::SecsPerDay) {
                ctx.errors.push_back(format_error(ERR_DATETIME, val));
                continue;
            }
            const double step = legacyStepSeconds(days);
            if (key == "RULE_STEP" ? step < 0.0 : step <= 0.0) {
                ctx.errors.push_back(format_error(ERR_NUMBER, val));
                continue;
            }
            if (key == "DRY_STEP") opt.dry_step = step;
            else if (key == "WET_STEP") opt.wet_step = step;
            else if (key == "REPORT_STEP") opt.report_step = step;
            else opt.rule_step = step;
        } else if (key == "QUALITY_STEP") {
            // X3a: LARD transport substep (strategy §4.2). Same time
            // spelling as the other *_STEP keys.
            opt.quality_step = parse_time_seconds(val);
        } else if (key == "MAX_SEGMENTS_PER_LINK") {
            // X3a: LARD slab capacity (strategy §4.1, EPANET MAXSEGS).
            double d = 0.0;
            openswmm::from_chars_double(val.data(), val.data() + val.size(), d);
            opt.max_segments_per_link = static_cast<int>(d);
        } else if (key == "DISPERSION") {
            // X3b: LARD RWPT toggle (GUI-plan Lagrangian-group key).
            // Unknown values fall to ext_options for diagnostics.
            const std::string dv = norm(val);
            if (dv == "RWPT")
                opt.lard_rwpt = true;
            else if (dv == "OFF" || dv == "NONE")
                opt.lard_rwpt = false;
            else
                opt.ext_options[key] = val;
        } else if (key == "RWPT_SEED") {
            // X3b: D-L6 deterministic counter-RNG seed.
            double d = 0.0;
            openswmm::from_chars_double(val.data(), val.data() + val.size(), d);
            opt.rwpt_seed = static_cast<int>(d);

            // -----------------------------------------------------------------
            // Simulation dates / times
            // -----------------------------------------------------------------
        } else if (key == "START_DATE") {
            if (legacyDate(val, start_date_part))
                got_start_date = true;
            else
                ctx.errors.push_back(format_error(ERR_DATETIME, val));
        } else if (key == "START_TIME") {
            if (legacyTime(val, start_time_part))
                got_start_time = true;
            else
                ctx.errors.push_back(format_error(ERR_DATETIME, val));
        } else if (key == "END_DATE") {
            if (legacyDate(val, end_date_part))
                got_end_date = true;
            else
                ctx.errors.push_back(format_error(ERR_DATETIME, val));
        } else if (key == "END_TIME") {
            if (legacyTime(val, end_time_part))
                got_end_time = true;
            else
                ctx.errors.push_back(format_error(ERR_DATETIME, val));
        } else if (key == "REPORT_START_DATE") {
            if (legacyDate(val, rpt_date_part))
                got_rpt_date = true;
            else
                ctx.errors.push_back(format_error(ERR_DATETIME, val));
        } else if (key == "REPORT_START_TIME") {
            if (legacyTime(val, rpt_time_part))
                got_rpt_time = true;
            else
                ctx.errors.push_back(format_error(ERR_DATETIME, val));

            // -----------------------------------------------------------------
            // Solver settings
            // -----------------------------------------------------------------
        } else if (key == "MAX_TRIALS") {
            // Legacy atoi accepts a numeric prefix (and maps nonnumeric text to zero).
            const int value = std::atoi(val.c_str());
            if (value < 0)
                ctx.errors.push_back(format_error(ERR_NUMBER, val));
            else
                opt.max_trials = value;
        } else if (key == "HEAD_TOLERANCE") {
            readLegacyNumber(ctx, val, opt.head_tol);
        } else if (key == "SYS_FLOW_TOL") {
            double pct = 0.0;
            if (!legacyNumber(val, pct))
                ctx.errors.push_back(format_error(ERR_NUMBER, val));
            else
                opt.sys_flow_tol = pct / 100.0; // input is percent, store as fraction
        } else if (key == "LAT_FLOW_TOL") {
            double pct = 0.0;
            if (!legacyNumber(val, pct))
                ctx.errors.push_back(format_error(ERR_NUMBER, val));
            else
                opt.lat_flow_tol = pct / 100.0; // input is percent, store as fraction

        } else if (key == "VARIABLE_STEP") {
            readLegacyNumber(ctx, val, opt.variable_step, 0.0, 2.0);

        } else if (key == "THREADS") {
            // Legacy atoi accepts a numeric prefix (and maps nonnumeric text to zero).
            const int value = std::atoi(val.c_str());
            if (value < 0)
                ctx.errors.push_back(format_error(ERR_NUMBER, val));
            else
                opt.num_threads = value;

            // -----------------------------------------------------------------
            // Boolean flags
            // -----------------------------------------------------------------
        } else if (key == "ALLOW_PONDING") {
            if (!parseLegacyYesNo(val, opt.allow_ponding))
                ctx.errors.push_back(format_error(ERR_KEYWORD, val));
        } else if (key == "IGNORE_RAINFALL") {
            if (!parseLegacyYesNo(val, opt.ignore_rainfall))
                ctx.errors.push_back(format_error(ERR_KEYWORD, val));
        } else if (key == "IGNORE_SNOWMELT") {
            if (!parseLegacyYesNo(val, opt.ignore_snow_melt))
                ctx.errors.push_back(format_error(ERR_KEYWORD, val));
        } else if (key == "IGNORE_GROUNDWATER") {
            if (!parseLegacyYesNo(val, opt.ignore_groundwater))
                ctx.errors.push_back(format_error(ERR_KEYWORD, val));
        } else if (key == "IGNORE_RDII") {
            if (!parseLegacyYesNo(val, opt.ignore_rdii))
                ctx.errors.push_back(format_error(ERR_KEYWORD, val));
        } else if (key == "IGNORE_ROUTING") {
            if (!parseLegacyYesNo(val, opt.ignore_routing))
                ctx.errors.push_back(format_error(ERR_KEYWORD, val));
        } else if (key == "IGNORE_QUALITY") {
            if (!parseLegacyYesNo(val, opt.ignore_quality))
                ctx.errors.push_back(format_error(ERR_KEYWORD, val));
        } else if (key == "SLOPE_WEIGHTING") {
            // Legacy validates this retired option even though it has no solver effect.
            bool value = false;
            if (!parseLegacyYesNo(val, value))
                ctx.errors.push_back(format_error(ERR_KEYWORD, val));
            else
                opt.ext_options[key] = val;
        } else if (key == "IGNORE_2D") {
            opt.ignore_2d = Tokenizer::parse_boolean(val);

            // -----------------------------------------------------------------
            // CRS (R06)
            // -----------------------------------------------------------------
        } else if (key == "CRS") {
            // val may be a quoted string parsed as one token, e.g. "EPSG:4326"
            // or the raw token EPSG:4326 without quotes
            opt.crs = val;
            ctx.spatial.crs = val;
            // Heuristic: geographic if EPSG:4326 or contains "latlong"/"geographic"
            std::string lv = norm(val);
            ctx.spatial.is_geographic = lv == "EPSG:4326" ||
                                        lv.find("LATLONG") != std::string::npos ||
                                        lv.find("GEOGRAPHIC") != std::string::npos;

            // -----------------------------------------------------------------
            // Legacy-compatibility keys that we silently accept but don't use
            // -----------------------------------------------------------------
        } else if (key == "SWEEP_START" || key == "SWEEP_END") {
            // Same non-leap anchor and 24-character input limit as legacy project_readOption.
            double date = 0.0;
            if (!legacyDate(val.substr(0, 24) + "/1947", date))
                ctx.errors.push_back(format_error(ERR_DATETIME, val));
            else if (key == "SWEEP_START")
                opt.sweep_start = datetime::dayOfYear(date);
            else
                opt.sweep_end = datetime::dayOfYear(date);

        } else if (key == "NORMAL_FLOW_LIMITED") {
            const std::string nv = norm(val);
            if (nv == "SLOPE")
                opt.normal_flow_ltd = 0;
            else if (nv == "FROUDE")
                opt.normal_flow_ltd = 1;
            else if (nv == "BOTH")
                opt.normal_flow_ltd = 2;
            else if (nv == "NEITHER")
                opt.normal_flow_ltd = 3;
            // Legacy prefix forms, and legacy NONE (index 3, NEITHER here),
            // which used to be ignored and run as BOTH.
            else if (const int m = legacyFindmatch(nv, kNormalFlowWords); m >= 0)
                opt.normal_flow_ltd = m;
            else
                ctx.errors.push_back(format_error(ERR_KEYWORD, val));

        } else if (key == "FORCE_MAIN_EQUATION") {
            const std::string fv = norm(val);
            if (fv == "H-W" || fv == "HW" || fv == "HAZEN-WILLIAMS")
                opt.force_main_eqn = 0;
            else if (fv == "D-W" || fv == "DW" || fv == "DARCY-WEISBACH")
                opt.force_main_eqn = 1;
            else if (const int m = legacyFindmatch(fv, kForceMainEqnWords); m >= 0)
                opt.force_main_eqn = m;
            else
                ctx.errors.push_back(format_error(ERR_KEYWORD, val));

        } else if (key == "INERTIAL_DAMPING") {
            const std::string iv = norm(val);
            if (iv == "NONE")
                opt.inertial_damping = 0;
            else if (iv == "PARTIAL")
                opt.inertial_damping = 1;
            else if (iv == "FULL")
                opt.inertial_damping = 2;
            else if (const int m = legacyFindmatch(iv, kInertDampingWords); m >= 0)
                opt.inertial_damping = m;
            else
                ctx.errors.push_back(format_error(ERR_KEYWORD, val));

        } else if (key == "SURCHARGE_METHOD") {
            const std::string sv = norm(val);
            if (sv == "EXTRAN")
                opt.surcharge_method = 0;
            else if (sv == "SLOT")
                opt.surcharge_method = 1;
            else if (sv == "DYNAMIC_SLOT")
                opt.surcharge_method = 2;
            else if (sv == "TPA")
                opt.surcharge_method = 3; // issue #156
            else if (const int m = legacyFindmatch(sv, kSurchargeWords); m >= 0)
                opt.surcharge_method = m;
            else
                ctx.errors.push_back(format_error(ERR_KEYWORD, val));

        } else if (key == "TPA_CELERITY") { // issue #156
            opt.tpa_celerity = to_double(val);

        } else if (key == "DPS_CELERITY") {
            opt.dps_target_celerity = to_double(val);

        } else if (key == "DPS_ALPHA") {
            opt.dps_alpha = to_double(val);

        } else if (key == "DPS_DECAY_TIME") {
            opt.dps_decay_time = to_double(val);

            // Unsteady friction (issue #156). Accepted-and-INERT, the same
            // posture as the FV block below: nothing downstream reads either key
            // yet (the Vitkovsky source term is Phase 2), so parsing them only
            // stops a deck that names them from drawing an unknown-option warning
            // and lets the writer carry them across a save.
        } else if (key == "UNSTEADY_FRICTION") {
            const std::string uv = norm(val);
            if (uv == "NONE")
                opt.unsteady_friction = 0;
            else if (uv == "VITKOVSKY")
                opt.unsteady_friction = 1;

        } else if (key == "UF_K3") {
            opt.uf_k3 = to_double(val);

        } else if (key == "REPORT_SIGNED_HEADS") { // issue #156 O-6
            const std::string sv2 = norm(val);
            opt.report_signed_heads =
                (sv2 == "YES" || sv2 == "TRUE" || sv2 == "ON" || sv2 == "1") ? 1 : 0;

            // -----------------------------------------------------------------
            // Explicit finite-volume solver (FLOW_ROUTING FV).
            //
            // First-class [OPTIONS] keys rather than a separate section (plan
            // §4.2). They are ACCEPTED AND INERT under any other routing model —
            // no warning, just unused — so switching FLOW_ROUTING back and forth
            // never invalidates a file. Length-dimensioned values stay in project
            // display units here; Router::init converts, exactly as it does for
            // HEAD_TOLERANCE.
            // -----------------------------------------------------------------
        } else if (key == "FV_CELL_LENGTH") {
            opt.fv.cell_length = to_double(val);

        } else if (key == "FV_MIN_CELLS") {
            opt.fv.min_cells = std::max(1, static_cast<int>(to_double(val)));

        } else if (key == "FV_CFL") {
            opt.fv.cfl = to_double(val);

        } else if (key == "FV_RIEMANN") {
            const std::string rv2 = norm(val);
            if (rv2 == "HLL")
                opt.fv.riemann = fv::RiemannSolver::HLL;
            else if (rv2 == "HLLC")
                opt.fv.riemann = fv::RiemannSolver::HLLC;

        } else if (key == "FV_ORDER") {
            opt.fv.order = static_cast<int>(to_double(val));

        } else if (key == "FV_LIMITER") {
            const std::string lv = norm(val);
            if (lv == "MINMOD")
                opt.fv.limiter = fv::Limiter::MINMOD;
            else if (lv == "VANLEER")
                opt.fv.limiter = fv::Limiter::VANLEER;
            else if (lv == "SUPERBEE")
                opt.fv.limiter = fv::Limiter::SUPERBEE;

        } else if (key == "FV_SCALAR_SCHEME") {
            const std::string sv2 = norm(val);
            if (sv2 == "UPWIND")
                opt.fv.scalar_scheme = fv::ScalarScheme::UPWIND;
            else if (sv2 == "MUSCL")
                opt.fv.scalar_scheme = fv::ScalarScheme::MUSCL;
            else if (sv2 == "QUICKEST_ULTIMATE" || sv2 == "QUICKEST")
                opt.fv.scalar_scheme = fv::ScalarScheme::QUICKEST_ULTIMATE;

        } else if (key == "FV_TIME_INTEGRATION") {
            const std::string tv = norm(val);
            if (tv == "EULER")
                opt.fv.time_integration = fv::TimeIntegration::EULER;
            else if (tv == "RK2")
                opt.fv.time_integration = fv::TimeIntegration::RK2;

        } else if (key == "FV_SLOT_CELERITY") {
            opt.fv.slot_celerity = to_double(val);

        } else if (key == "FV_PRESSURIZED_IMPLICIT") {
            const std::string pv = norm(val);
            opt.fv.pressurized_implicit = (pv == "YES" || pv == "TRUE" || pv == "ON" || pv == "1");

        } else if (key == "FV_PRESSURE_CLOSURE") { // issue #156 Phase 4
            const std::string pc = norm(val);
            if (pc == "SLOT")
                opt.fv.pressure_closure = 0;
            else if (pc == "TPA")
                opt.fv.pressure_closure = 1;

        } else if (key == "FV_DISPERSION") {
            opt.fv.dispersion = to_double(val);

        } else if (key == "FV_STRUCTURE_COUPLING") {
            const std::string cv = norm(val);
            if (cv == "SUBSTEP")
                opt.fv.structure_coupling = fv::StructureCoupling::SUBSTEP;
            else if (cv == "ROUTING_STEP")
                opt.fv.structure_coupling = fv::StructureCoupling::ROUTING_STEP;

        } else if (key == "FV_NODE_COUPLING") {
            // RETIRED 2026-08-29 with FV_NODE_DT and FV_NODE_PICARD below.
            // Two retirement tiers, on purpose: these three had an effect and
            // warn when a deck asks for the behaviour that no longer exists
            // (a calibrated model would otherwise change answers silently);
            // FV_NODE_CELL_COUPLING / FV_JUNCTION_MODEL further down were
            // already no-ops and stay silent. Spelling out the former default
            // is not warned -- it is what every deck gets.
            if (norm(val) == "EXPLICIT") {
                ctx.warnings.push_back(
                    "WARNING: FV_NODE_COUPLING EXPLICIT is retired and will be "
                    "treated as SEMI_IMPLICIT - storage nodes are always coupled "
                    "semi-implicitly (plain junctions are algebraic interfaces "
                    "under either).");
                if (ctx.warning_code == 0)
                    ctx.warning_code = 101;
            }

        } else if (key == "FV_COMPACTION") {
            const std::string bv = norm(val);
            opt.fv.compaction = !(bv == "NO" || bv == "FALSE" || bv == "0" || bv == "OFF");

        } else if (key == "FV_BACKEND") {
            const std::string bv = norm(val);
            if (bv == "CPU")
                opt.fv.backend = fv::Backend::CPU;
            else if (bv == "AUTO")
                opt.fv.backend = fv::Backend::AUTO;
            else if (bv == "OMP")
                opt.fv.backend = fv::Backend::OMP;
            else if (bv == "CUDA")
                opt.fv.backend = fv::Backend::CUDA;
            else if (bv == "HIP")
                opt.fv.backend = fv::Backend::HIP;
            else if (bv == "SYCL")
                opt.fv.backend = fv::Backend::SYCL;

        } else if (key == "FV_MIN_PARALLEL_CELLS") {
            opt.fv.min_parallel_cells = static_cast<long>(to_double(val));

        } else if (key == "FV_LTS") {
            const std::string bv = norm(val);
            opt.fv.lts = !(bv == "NO" || bv == "FALSE" || bv == "0" || bv == "OFF");

        } else if (key == "FV_NODE_FEEDBACK_DT") {
            const std::string bv = norm(val);
            opt.fv.node_feedback_dt = !(bv == "NO" || bv == "FALSE" || bv == "0" || bv == "OFF");

        } else if (key == "FV_LTS_MAX_TIERS") {
            opt.fv.lts_max_tiers = std::max(1, static_cast<int>(to_double(val)));

        } else if (key == "FV_CFL_CENSUS_INTERVAL") {
            opt.fv.cfl_census_interval = std::max(1, static_cast<int>(to_double(val)));

        } else if (key == "FV_NODE_DT") {
            // RETIRED 2026-08-29 -- see FV_NODE_COUPLING above.
            if (norm(val) == "NONE") {
                ctx.warnings.push_back("WARNING: FV_NODE_DT NONE is retired and will be treated as "
                                       "STABILITY - the node accuracy bound is always armed (it "
                                       "binds storage nodes only; algebraic junctions and outfalls "
                                       "are exempt).");
                if (ctx.warning_code == 0)
                    ctx.warning_code = 101;
            }

        } else if (key == "FV_NODE_PICARD") {
            // RETIRED 2026-08-29 -- see FV_NODE_COUPLING above.
            if (std::max(1, static_cast<int>(to_double(val))) > 1) {
                ctx.warnings.push_back("WARNING: FV_NODE_PICARD " + val +
                                       " is retired and will be "
                                       "treated as 1 - the semi-implicit node correction is always "
                                       "a single sweep.");
                if (ctx.warning_code == 0)
                    ctx.warning_code = 101;
            }

        } else if (key == "FV_NODE_CELL_COUPLING" || key == "FV_JUNCTION_MODEL") {
            // Retired options, accepted and ignored so existing project files
            // still parse: junctions are always algebraic interfaces now (the
            // BUCKET model and the coupled-star correction were superseded by
            // the pass-through interface treatment).

        } else if (key == "NODE_CONTINUITY") {
            const std::string nc = norm(val);
            if (nc == "EXPLICIT")
                opt.node_continuity = NodeContinuity::EXPLICIT;
            else if (nc == "SEMI_IMPLICIT")
                opt.node_continuity = NodeContinuity::SEMI_IMPLICIT;

        } else if (key == "VIRTUAL_JUNCTION_MOMENTUM") {
            // RETIRED 2026-08-14. FULL is accepted and warned for one release,
            // then the keyword goes the way of FV_NODE_CELL_COUPLING above.
            // It was measured to be defective, not merely inaccurate: its
            // cross-junction term dq4j is sign-inverted with respect to the
            // per-link convective term it supplements (at constant Q,
            // Δ(v²A) = −v²ΔA), and it is applied to BOTH adjacent links on top
            // of each link's own full-length dq4. On SWASHES macdonald-periodic
            // it destroyed 224-325 % of the routed volume; negating the term
            // restores mass conservation but still leaves l1 5.24 % against
            // BASIC's 0.163 % and plain DW's 0.141 %, so there is no
            // term-level correction worth keeping. Evidence:
            // epaswmm5_qa suites/swashes plans/VJ_MOMENTUM_SCOPE.md Phase 3.
            const std::string vm = norm(val);
            opt.virtual_junction_momentum = 0;
            if (vm == "FULL") {
                ctx.warnings.push_back("WARNING: VIRTUAL_JUNCTION_MOMENTUM FULL is retired and "
                                       "will be treated as BASIC - the cross-junction momentum "
                                       "term was not mass conserving.");
                if (ctx.warning_code == 0) {
                    ctx.warning_code = 101; // SWMM_WARN_UNKNOWN_OPTION
                }
            }

        } else if (key == "ANDERSON_ACCEL") {
            const std::string av = norm(val);
            opt.anderson_accel = (av == "YES" || av == "TRUE" || av == "1");

        } else if (key == "LINK_OFFSETS") {
            const std::string lv = norm(val);
            if (lv == "DEPTH")
                opt.link_offsets = 0;
            else if (lv == "ELEVATION")
                opt.link_offsets = 1;
            else if (const int m = legacyFindmatch(lv, kLinkOffsetWords); m >= 0)
                opt.link_offsets = m;
            else
                ctx.errors.push_back(format_error(ERR_KEYWORD, val));

        } else if (key == "MIN_SLOPE") {
            readLegacyNumber(ctx, val, opt.min_slope, 0.0, 100.0, true);

        } else if (key == "MIN_SURFAREA") {
            readLegacyNumber(ctx, val, opt.min_surf_area, 0.0);

        } else if (key == "LENGTHENING_STEP") {
            double step = 0.0, days = 0.0;
            if (!legacyNumber(val, step)) {
                if (!legacyTime(val, days) || !std::isfinite(days) ||
                    std::abs(days) > std::numeric_limits<int>::max() / datetime::SecsPerDay) {
                    ctx.errors.push_back(format_error(ERR_NUMBER, val));
                    continue;
                }
                step = legacyStepSeconds(days);
            }
            opt.lengthening_step = std::max(0.0, step);

        } else if (key == "SKIP_STEADY_STATE") {
            if (!parseLegacyYesNo(val, opt.skip_steady_state))
                ctx.errors.push_back(format_error(ERR_KEYWORD, val));

        } else if (key == "WRITE_ABSOLUTE_PATHS") {
            // Slice IO-4 portability opt-out — see SimulationOptions.hpp.
            opt.write_absolute_paths = Tokenizer::parse_boolean(val);

        } else if (key.starts_with("IO_")) {
            opt.ext_options[key] = val;
        } else if (key == "COMPATIBILITY") {
            // Not acted on by the v6 engine; kept verbatim so the round trip
            // and swmm_options_get/set("COMPATIBILITY") see it.
            if (val == "3" || val == "4" || val == "5")
                opt.ext_options[key] = val;
            else
                ctx.errors.push_back(format_error(ERR_KEYWORD, val));

            // -----------------------------------------------------------------
            // Unknown key → ext_options (R05)
            // -----------------------------------------------------------------
        } else if (legacy_knows_option(key)) {
            // A legacy keyword v6 does not act on (e.g. TEMPDIR): kept for the
            // round trip, and no warning, as legacy accepts it.
            opt.ext_options[key] = val;
        } else {
            opt.ext_options[key] = val;
            // Record a warning (non-fatal). Push to ctx.warnings so it reaches
            // the .rpt (legacy project.c warns per unknown keyword); keep the
            // legacy wording, and retain warning_code for the C API.
            // Leading "\n  " as legacy writes it (report_writeLine): a blank
            // line first, and the report prints it before the title.
            ctx.warnings.push_back("\n  WARNING: Unknown option keyword '" + tokens[0] +
                                   "' in [OPTIONS] section - option will be ignored.");
            if (ctx.warning_code == 0) {
                ctx.warning_code = 101; // SWMM_WARN_UNKNOWN_OPTION
            }
        }
    }

    // Combine date + time parts
    if (got_start_date || got_start_time) {
        opt.start_date = start_date_part + start_time_part;
    }
    if (got_end_date || got_end_time) {
        opt.end_date = end_date_part + end_time_part;
    }
    // PARITY swmm5.c:3198-3200: legacy forms TotalDuration from the SEPARATE
    // date and time parts — floor((EndDate-StartDate)*86400 +
    // (EndTime-StartTime)*86400) × 1000 — which is exact whole-second ms.
    // The combined serials (end_date - start_date)*86400000 round differently
    // (e.g. 172679999.99999997 vs the legacy-exact 172680000.0), perturbing
    // the final clipped runoff step near simulation end. Only computable here
    // when both dates were parsed from their parts; anything else falls back
    // to SimulationOptions::totalDurationMs().
    if (got_start_date && got_end_date) {
        opt.total_duration_ms = std::floor((end_date_part - start_date_part) * 86400.0 +
                                           (end_time_part - start_time_part) * 86400.0) *
                                1000.0;
    }
    if (got_rpt_date || got_rpt_time) {
        opt.report_start = rpt_date_part + rpt_time_part;
    } else {
        opt.report_start = opt.start_date;
    }
}

} /* namespace openswmm::input */
