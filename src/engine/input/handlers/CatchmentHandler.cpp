/**
 * @file CatchmentHandler.cpp
 * @brief Section handlers for [SUBCATCHMENTS], [SUBAREAS], and [RAINGAGES].
 *
 * ### [SUBCATCHMENTS] format
 * ```
 * ;; Name   Gage   Outlet   Area   %Imperv  Width  %Slope  CurbLen  SnowPack
 * S1        RG1    J1       10.0   50.0     100.0  0.5     0.0
 * ```
 *
 * ### [SUBAREAS] format
 * ```
 * ;; Subcatch  N-Imperv  N-Perv  S-Imperv  S-Perv  PctZero  RouteTo  PctRouted
 * S1           0.013     0.1     0.05      0.1     25       OUTLET
 * ```
 *
 * ### [RAINGAGES] format
 * ```
 * ;; Name   Format    Interval  SCF   Source
 * RG1       VOLUME    0:15      1.0   TIMESERIES RAIN1
 * RG2       VOLUME    0:15      1.0   FILE "rain.csv:EAST_GAGE"
 * ```
 *
 * @see Legacy reference: src/solver/input.c — readSubcatch(), readGage()
 * @ingroup engine_input
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#include "CatchmentHandler.hpp"

#include "../Tokenizer.hpp"
#include "../../core/SimulationContext.hpp"
#include "../../data/SubcatchData.hpp"
#include "../../data/GageData.hpp"

#include "../InputParseUtils.hpp"

#include <charconv>
#include <string>

namespace openswmm::input {

static void ensure_subcatch_capacity(SimulationContext& ctx, int idx) {
    ctx.subcatches.grow_to(idx + 1);
}

static void ensure_gage_capacity(SimulationContext& ctx, int idx) {
    const auto n = static_cast<std::size_t>(idx + 1);
    auto grow = [&](auto& vec, auto def) {
        if (vec.size() < n) vec.resize(n, def);
    };
    grow(ctx.gages.source,        RainSource::TIMESERIES);
    grow(ctx.gages.ts_index,      -1);
    grow(ctx.gages.ts_name,       std::string{});
    grow(ctx.gages.file_path,     std::string{});
    grow(ctx.gages.col_name,      std::string{});
    grow(ctx.gages.file_format,   RainFileFormat::UNKNOWN);
    grow(ctx.gages.interval_sec,  3600);
    grow(ctx.gages.snow_factor,   1.0);
    grow(ctx.gages.rain_type,     0);
    grow(ctx.gages.rainfall,      0.0);
    grow(ctx.gages.next_rainfall, 0.0);
    grow(ctx.gages.api_rainfall,  -1.0);  // -1 = no API override
    grow(ctx.gages.next_rain_date,0.0);
    grow(ctx.gages.is_raining,    false);

    // Past-rain tracking (used by updateAllGages for controls past-rain)
    grow(ctx.gages.past_rain_accum,   0.0);
    grow(ctx.gages.past_rain_time,    0.0);
    grow(ctx.gages.cumul_rain_accum,  0.0);
    grow(ctx.gages.co_gage_index,     -1);
    // past_rain is a flat 2-D array: [gage * MAXPASTRAIN + hour]
    {
        const auto nr = n * static_cast<std::size_t>(GageData::MAXPASTRAIN);
        if (ctx.gages.past_rain.size() < nr)
            ctx.gages.past_rain.resize(nr, 0.0);
    }
}

// ============================================================================
// handle_subcatchments()
// ============================================================================

void handle_subcatchments(SimulationContext& ctx, const std::vector<std::string>& lines) {
    for (const auto& line : lines) {
        auto tok = Tokenizer::tokenize(line);
        if (tok.size() < 7) continue;
        // Name  Gage  Outlet  Area  %Imperv  Width  %Slope  [CurbLen]

        const std::string& name = tok[0];
        int idx = ctx.subcatch_names.find(name);
        if (idx < 0) idx = ctx.subcatch_names.add(name);

        ensure_subcatch_capacity(ctx, idx);

        // Gage — resolve index, may be -1 if gage not yet parsed
        ctx.subcatches.gage[idx] = ctx.gage_names.find(tok[1]);

        // Outlet: could be a node or another subcatchment
        // Store name for deferred resolution in PostParseResolver
        ctx.subcatches.outlet_name[idx] = tok[2];
        int node_idx = ctx.node_names.find(tok[2]);
        if (node_idx >= 0) {
            ctx.subcatches.outlet_node[idx] = node_idx;
        } else {
            int sub_idx = ctx.subcatch_names.find(tok[2]);
            if (sub_idx >= 0) ctx.subcatches.outlet_subcatch[idx] = sub_idx;
        }

        ctx.subcatches.area[idx]       = to_double(tok[3]);
        ctx.subcatches.frac_imperv[idx]= to_double(tok[4]) / 100.0;  // % → fraction
        ctx.subcatches.width[idx]      = to_double(tok[5]);
        ctx.subcatches.slope[idx]      = to_double(tok[6]) / 100.0;  // % → fraction

        // Optional CurbLen (column 7)
        if (tok.size() > 7)
            ctx.subcatches.curb_length[idx] = to_double(tok[7]);
    }
}

// ============================================================================
// handle_subareas()
// ============================================================================

void handle_subareas(SimulationContext& ctx, const std::vector<std::string>& lines) {
    for (const auto& line : lines) {
        auto tok = Tokenizer::tokenize(line);
        if (tok.size() < 6) continue;
        // Subcatch  N-Imperv  N-Perv  S-Imperv  S-Perv  PctZero  [RouteTo]

        const int idx = ctx.subcatch_names.find(tok[0]);
        if (idx < 0) continue;

        ctx.subcatches.n_imperv[idx]  = to_double(tok[1]);
        ctx.subcatches.n_perv[idx]    = to_double(tok[2]);
        ctx.subcatches.ds_imperv[idx] = to_double(tok[3]);
        ctx.subcatches.ds_perv[idx]   = to_double(tok[4]);

        // PctZero → fraction of impervious with no depression storage
        ctx.subcatches.frac_imperv_no_store[idx] = to_double(tok[5]) / 100.0;

        // RouteTo: OUTLET (0), IMPERV (1), or PERV (2)
        if (tok.size() > 6) {
            std::string rt = Tokenizer::to_upper(tok[6]);
            if (rt == "IMPERV")       ctx.subcatches.subarea_routing[idx] = 1;
            else if (rt == "PERV")    ctx.subcatches.subarea_routing[idx] = 2;
            else                      ctx.subcatches.subarea_routing[idx] = 0; // OUTLET
        }

        // PctRouted (default 100%)
        if (tok.size() > 7) {
            ctx.subcatches.pct_routed[idx] = to_double(tok[7]) / 100.0;
        } else if (tok.size() > 6) {
            // If RouteTo specified but no PctRouted, default to 100%
            std::string rt = Tokenizer::to_upper(tok[6]);
            if (rt == "IMPERV" || rt == "PERV")
                ctx.subcatches.pct_routed[idx] = 1.0;
        }
    }
}

// ============================================================================
// handle_infiltration()
// ============================================================================

void handle_infiltration(SimulationContext& ctx, const std::vector<std::string>& lines) {
    // Determine infiltration model from OPTIONS enum
    int model = static_cast<int>(ctx.options.infiltration);

    for (const auto& line : lines) {
        auto tok = Tokenizer::tokenize(line);
        if (tok.size() < 4) continue;
        // Subcatch  Param1  Param2  Param3  [Param4]  [Param5]

        const int idx = ctx.subcatch_names.find(tok[0]);
        if (idx < 0) continue;

        ctx.subcatches.infil_model[idx] = model;
        ctx.subcatches.infil_p1[idx] = to_double(tok[1]);
        ctx.subcatches.infil_p2[idx] = to_double(tok[2]);
        ctx.subcatches.infil_p3[idx] = to_double(tok[3]);
        if (tok.size() > 4) ctx.subcatches.infil_p4[idx] = to_double(tok[4]);
        if (tok.size() > 5) ctx.subcatches.infil_p5[idx] = to_double(tok[5]);
    }
}

// ============================================================================
// handle_raingages()
// ============================================================================

void handle_raingages(SimulationContext& ctx, const std::vector<std::string>& lines) {
    for (const auto& line : lines) {
        auto tok = Tokenizer::tokenize(line);
        if (tok.size() < 5) continue;
        // Name  Format  Interval  SCF  Source  [SourceName]

        const std::string& name = tok[0];
        int idx = ctx.gage_names.find(name);
        if (idx < 0) idx = ctx.gage_names.add(name);

        ensure_gage_capacity(ctx, idx);

        // Format: VOLUME or INTENSITY → store in rain_type
        const std::string fmt = Tokenizer::to_upper(tok[1]);
        if (fmt == "VOLUME")          ctx.gages.rain_type[idx] = 1;
        else if (fmt == "CUMULATIVE") ctx.gages.rain_type[idx] = 2;
        else                          ctx.gages.rain_type[idx] = 0; // INTENSITY

        // Interval: HH:MM or seconds
        ctx.gages.interval_sec[idx] =
            static_cast<int>(parse_time_seconds(tok[2]));

        // Snow correction factor
        ctx.gages.snow_factor[idx] = to_double(tok[3], 1.0);

        // Source: TIMESERIES <name>  or  FILE "<path>"  or  FILE "<path:col>"
        const std::string src = Tokenizer::to_upper(tok[4]);

        if (src == "TIMESERIES" && tok.size() > 5) {
            ctx.gages.source[idx]   = RainSource::TIMESERIES;
            ctx.gages.ts_name[idx]  = tok[5]; // Store name for deferred resolution
            ctx.gages.ts_index[idx] = ctx.table_names.find(tok[5]);
            // ts_index may be -1 if TIMESERIES section appears after RAINGAGES
        } else if (src == "FILE" && tok.size() > 5) {
            ctx.gages.source[idx] = RainSource::FILE_RAIN;

            // tok[5] already has quotes stripped by the tokenizer
            // Check for "path:COLUMN" syntax (R08)
            const std::string& file_tok = tok[5];
            const auto colon = file_tok.rfind(':');

            // On Windows, drive letters look like "C:\path" — skip the first char
            const auto search_start = (file_tok.size() > 1 && file_tok[1] == ':') ? 2 : 0;
            const auto col_sep = file_tok.find(':', search_start);

            if (col_sep != std::string::npos) {
                ctx.gages.file_path[idx] = file_tok.substr(0, col_sep);
                ctx.gages.col_name[idx]  = file_tok.substr(col_sep + 1);
                ctx.gages.file_format[idx] = RainFileFormat::USER_CSV;
            } else {
                ctx.gages.file_path[idx]  = file_tok;
                ctx.gages.file_format[idx] = RainFileFormat::STAN_PRCP;
            }
            (void)colon; // suppress warning
        }
    }
}

} /* namespace openswmm::input */
