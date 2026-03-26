/**
 * @file OptionsHandler.cpp
 * @brief [OPTIONS] section handler for the new engine.
 *
 * @details Parses key-value pairs from the [OPTIONS] section and populates
 *          `SimulationContext::options` (a SimulationOptions struct).
 *
 *          Unknown keys are stored in `options.ext_options` with a
 *          SWMM_WARN_UNKNOWN_OPTION warning (R05).
 *
 *          The special key `CRS` is stored in both `options.crs` and
 *          `spatial.crs` (R06).
 *
 * Key mapping (standard SWMM 5.x keys only):
 *
 *  FLOW_UNITS           → options.flow_units
 *  INFILTRATION         → options.infiltration
 *  FLOW_ROUTING         → options.routing_model
 *  LINK_OFFSETS         → (ignored — legacy compatibility)
 *  MIN_SLOPE            → (stored in ext_options — not used by new solver yet)
 *  ALLOW_PONDING        → options.allow_ponding
 *  SKIP_STEADY_STATE    → (ignored)
 *  START_DATE           → options.start_date (Julian date)
 *  START_TIME           → combined with START_DATE
 *  END_DATE             → options.end_date
 *  END_TIME             → combined with END_DATE
 *  REPORT_START_DATE    → options.report_start
 *  REPORT_START_TIME    → combined with REPORT_START_DATE
 *  SWEEP_START          → (ext_options)
 *  SWEEP_END            → (ext_options)
 *  DRY_DAYS             → (ext_options)
 *  REPORT_STEP          → options.report_step (HH:MM:SS or seconds)
 *  WET_STEP             → options.wet_step
 *  DRY_STEP             → options.dry_step
 *  ROUTING_STEP         → options.routing_step
 *  RULE_STEP            → options.dt_controls_remaining (set to first rule step)
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
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#include "OptionsHandler.hpp"

#include "../Tokenizer.hpp"
#include "../../core/SimulationContext.hpp"
#include "../../core/DateTime.hpp"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace openswmm::input {

// ============================================================================
// Helper: parse HH:MM:SS or decimal seconds → seconds
// ============================================================================

static double parse_time_hhmmss(std::string_view sv) {
    // Accepts:
    //   HH:MM:SS   → hours*3600 + min*60 + sec
    //   MM:SS      → min*60 + sec
    //   N          → N (seconds, floating point)

    double val = 0.0;
    auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), val);
    if (ec == std::errc{} && ptr == sv.data() + sv.size()) {
        return val;  // plain number = seconds
    }

    // Try HH:MM:SS or MM:SS
    unsigned h = 0, m = 0;
    double s = 0.0;
    const char* p = sv.data();
    const char* end = sv.data() + sv.size();

    auto read_uint = [&](unsigned& out) -> bool {
        auto [np, nec] = std::from_chars(p, end, out);
        if (nec != std::errc{}) return false;
        p = np;
        return true;
    };
    auto read_double = [&](double& out) -> bool {
        auto [np, nec] = std::from_chars(p, end, out);
        if (nec != std::errc{}) return false;
        p = np;
        return true;
    };

    if (!read_uint(h)) return 0.0;
    if (p < end && *p == ':') {
        ++p;
        if (!read_uint(m)) return static_cast<double>(h);
        if (p < end && *p == ':') {
            ++p;
            if (!read_double(s)) return h * 3600.0 + m * 60.0;
        }
    }
    return h * 3600.0 + m * 60.0 + s;
}

// ============================================================================
// Helper: parse date string MM/DD/YYYY → Julian date (decimal days)
// ============================================================================

static double parse_date(std::string_view sv) {
    // Accepts MM/DD/YYYY
    unsigned m = 0, d = 0, y = 0;
    const char* p = sv.data();
    const char* end = sv.data() + sv.size();

    auto read_uint = [&](unsigned& out) -> bool {
        auto [np, ec] = std::from_chars(p, end, out);
        if (ec != std::errc{}) return false;
        p = np;
        return true;
    };

    if (!read_uint(m)) return 0.0;
    if (p < end && *p == '/') ++p; else return 0.0;
    if (!read_uint(d)) return 0.0;
    if (p < end && *p == '/') ++p; else return 0.0;
    if (!read_uint(y)) return 0.0;

    return datetime::encodeDate(static_cast<int>(y),
                                static_cast<int>(m),
                                static_cast<int>(d));
}

// ============================================================================
// Helper: normalize uppercase key
// ============================================================================

static std::string norm(std::string_view sv) {
    return Tokenizer::to_upper(sv);
}

// ============================================================================
// handle_options() — registered as built-in handler for "OPTIONS"
// ============================================================================

void handle_options(SimulationContext& ctx, const std::vector<std::string>& lines) {
    SimulationOptions& opt = ctx.options;

    // We need to accumulate START_DATE/START_TIME because they may appear
    // in any order relative to each other.
    double start_date_part = 0.0, start_time_part = 0.0;
    double end_date_part   = 0.0, end_time_part   = 0.0;
    double rpt_date_part   = 0.0, rpt_time_part   = 0.0;
    bool got_start_date = false, got_start_time = false;
    bool got_end_date   = false, got_end_time   = false;
    bool got_rpt_date   = false, got_rpt_time   = false;

    for (const auto& raw : lines) {
        auto tokens = Tokenizer::tokenize(raw);
        if (tokens.size() < 2) continue;

        const std::string key = norm(tokens[0]);
        // Value may be a single token or a multi-token (e.g., quoted CRS string).
        // Reconstruct value as original suffix for tokens[1..end]
        const std::string& val = tokens[1];

        // -----------------------------------------------------------------
        // Flow / routing units
        // -----------------------------------------------------------------
        if (key == "FLOW_UNITS") {
            const std::string uv = norm(val);
            if      (uv == "CFS") opt.flow_units = FlowUnits::CFS;
            else if (uv == "GPM") opt.flow_units = FlowUnits::GPM;
            else if (uv == "MGD") opt.flow_units = FlowUnits::MGD;
            else if (uv == "CMS") opt.flow_units = FlowUnits::CMS;
            else if (uv == "LPS") opt.flow_units = FlowUnits::LPS;
            else if (uv == "MLD") opt.flow_units = FlowUnits::MLD;
            else opt.ext_options[key] = val;

        } else if (key == "INFILTRATION") {
            const std::string iv = norm(val);
            if      (iv == "HORTON"         || iv == "MODIFIED_HORTON")
                opt.infiltration = InfiltrationModel::HORTON;
            else if (iv == "MOD_HORTON")
                opt.infiltration = InfiltrationModel::MOD_HORTON;
            else if (iv == "GREEN_AMPT"     || iv == "MODIFIED_GREEN_AMPT")
                opt.infiltration = InfiltrationModel::GREEN_AMPT;
            else if (iv == "MOD_GREEN_AMPT")
                opt.infiltration = InfiltrationModel::MOD_GREEN_AMPT;
            else if (iv == "CURVE_NUMBER")
                opt.infiltration = InfiltrationModel::CURVE_NUMBER;
            else opt.ext_options[key] = val;

        } else if (key == "FLOW_ROUTING") {
            const std::string rv = norm(val);
            if      (rv == "STEADY")   opt.routing_model = RoutingModel::STEADY;
            else if (rv == "KINWAVE"   || rv == "KINEMATIC_WAVE")
                opt.routing_model = RoutingModel::KINWAVE;
            else if (rv == "DYNWAVE"   || rv == "DYNAMIC_WAVE")
                opt.routing_model = RoutingModel::DYNWAVE;
            else opt.ext_options[key] = val;

        // -----------------------------------------------------------------
        // Timesteps
        // -----------------------------------------------------------------
        } else if (key == "ROUTING_STEP") {
            opt.routing_step = parse_time_hhmmss(val);
        } else if (key == "MINIMUM_STEP") {
            opt.min_routing_step = parse_time_hhmmss(val);
        } else if (key == "DRY_DAYS") {
            std::from_chars(val.data(), val.data() + val.size(), opt.dry_days);
        } else if (key == "DRY_STEP") {
            opt.dry_step = parse_time_hhmmss(val);
        } else if (key == "WET_STEP") {
            opt.wet_step = parse_time_hhmmss(val);
        } else if (key == "REPORT_STEP") {
            opt.report_step = parse_time_hhmmss(val);

        // -----------------------------------------------------------------
        // Simulation dates / times
        // -----------------------------------------------------------------
        } else if (key == "START_DATE") {
            start_date_part = parse_date(val);
            got_start_date  = true;
        } else if (key == "START_TIME") {
            start_time_part = parse_time_hhmmss(val) / datetime::SecsPerDay;
            got_start_time  = true;
        } else if (key == "END_DATE") {
            end_date_part = parse_date(val);
            got_end_date  = true;
        } else if (key == "END_TIME") {
            end_time_part = parse_time_hhmmss(val) / datetime::SecsPerDay;
            got_end_time  = true;
        } else if (key == "REPORT_START_DATE") {
            rpt_date_part = parse_date(val);
            got_rpt_date  = true;
        } else if (key == "REPORT_START_TIME") {
            rpt_time_part = parse_time_hhmmss(val) / datetime::SecsPerDay;
            got_rpt_time  = true;

        // -----------------------------------------------------------------
        // Solver settings
        // -----------------------------------------------------------------
        } else if (key == "MAX_TRIALS") {
            double d = 0.0;
            std::from_chars(val.data(), val.data() + val.size(), d);
            opt.max_trials = static_cast<int>(d);
        } else if (key == "HEAD_TOLERANCE") {
            std::from_chars(val.data(), val.data() + val.size(), opt.head_tol);
        } else if (key == "SYS_FLOW_TOL") {
            std::from_chars(val.data(), val.data() + val.size(), opt.sys_flow_tol);
        } else if (key == "LAT_FLOW_TOL") {
            std::from_chars(val.data(), val.data() + val.size(), opt.lat_flow_tol);

        } else if (key == "VARIABLE_STEP") {
            std::from_chars(val.data(), val.data() + val.size(), opt.variable_step);

        } else if (key == "THREADS") {
            double d = 0.0;
            std::from_chars(val.data(), val.data() + val.size(), d);
            opt.num_threads = static_cast<int>(d);

        // -----------------------------------------------------------------
        // Boolean flags
        // -----------------------------------------------------------------
        } else if (key == "ALLOW_PONDING") {
            opt.allow_ponding = Tokenizer::parse_boolean(val);
        } else if (key == "IGNORE_RAINFALL") {
            opt.ignore_rainfall = Tokenizer::parse_boolean(val);
        } else if (key == "IGNORE_SNOWMELT") {
            opt.ignore_snow_melt = Tokenizer::parse_boolean(val);
        } else if (key == "IGNORE_GROUNDWATER") {
            opt.ignore_groundwater = Tokenizer::parse_boolean(val);
        } else if (key == "IGNORE_RDII") {
            opt.ignore_rdii = Tokenizer::parse_boolean(val);
        } else if (key == "IGNORE_ROUTING") {
            opt.ignore_routing = Tokenizer::parse_boolean(val);
        } else if (key == "IGNORE_QUALITY") {
            opt.ignore_quality = Tokenizer::parse_boolean(val);

        // -----------------------------------------------------------------
        // CRS (R06)
        // -----------------------------------------------------------------
        } else if (key == "CRS") {
            // val may be a quoted string parsed as one token, e.g. "EPSG:4326"
            // or the raw token EPSG:4326 without quotes
            opt.crs         = val;
            ctx.spatial.crs = val;
            // Heuristic: geographic if EPSG:4326 or contains "latlong"/"geographic"
            std::string lv = norm(val);
            ctx.spatial.is_geographic =
                lv == "EPSG:4326" ||
                lv.find("LATLONG") != std::string::npos ||
                lv.find("GEOGRAPHIC") != std::string::npos;

        // -----------------------------------------------------------------
        // Legacy-compatibility keys that we silently accept but don't use
        // -----------------------------------------------------------------
        } else if (key == "SWEEP_START") {
            // MM/DD → day-of-year
            unsigned sm = 1, sd = 1;
            { const char* sp = val.data(); const char* se = sp + val.size();
              std::from_chars(sp, se, sm);
              while (sp < se && *sp != '/') ++sp;
              if (sp < se) ++sp;
              std::from_chars(sp, se, sd);
            }
            opt.sweep_start = datetime::dayOfYear(
                datetime::encodeDate(2000, static_cast<int>(sm), static_cast<int>(sd)));
        } else if (key == "SWEEP_END") {
            unsigned sm = 12, sd = 31;
            { const char* sp = val.data(); const char* se = sp + val.size();
              std::from_chars(sp, se, sm);
              while (sp < se && *sp != '/') ++sp;
              if (sp < se) ++sp;
              std::from_chars(sp, se, sd);
            }
            opt.sweep_end = datetime::dayOfYear(
                datetime::encodeDate(2000, static_cast<int>(sm), static_cast<int>(sd)));

        } else if (key == "LINK_OFFSETS"    ||
                   key == "SKIP_STEADY_STATE"||
                   key == "COMPATIBILITY") {
            // no-op; recognized but unused in new engine

        // -----------------------------------------------------------------
        // Unknown key → ext_options (R05)
        // -----------------------------------------------------------------
        } else {
            opt.ext_options[key] = val;
            // Record a warning (non-fatal)
            if (ctx.warning_code == 0) {
                ctx.warning_code = 101;  // SWMM_WARN_UNKNOWN_OPTION
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
    if (got_rpt_date || got_rpt_time) {
        opt.report_start = rpt_date_part + rpt_time_part;
    } else {
        opt.report_start = opt.start_date;
    }
}

} /* namespace openswmm::input */
