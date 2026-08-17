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
 * ;; Name   Format    Interval  SCF   Source                 [ScaleFactor]
 * RG1       VOLUME    0:15      1.0   TIMESERIES RAIN1        2.0
 * RG2       VOLUME    0:15      1.0   FILE "rain.csv:EAST"    1.5
 * ```
 *
 * The trailing `ScaleFactor` token is optional; it multiplies the gage's
 * rainfall intensity after unit conversion (Build 5.3.0+, parity with
 * legacy `gage.c`).  When two gages share the same TIMESERIES (co-gages),
 * the secondary's rainfall is rescaled by the ratio
 * `scale_factor[secondary] / scale_factor[primary]`.
 *
 * @see Legacy reference: src/solver/input.c — readSubcatch(), readGage()
 * @ingroup engine_input
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "CatchmentHandler.hpp"

#include "../Tokenizer.hpp"
#include "../SectionParser.hpp"
#include "../MultiColumnSeriesFile.hpp"
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
    grow(ctx.gages.station_id,    std::string{});
    grow(ctx.gages.rain_units,    0);
    grow(ctx.gages.file_first_date,    0.0);
    grow(ctx.gages.file_last_date,     0.0);
    grow(ctx.gages.file_periods_precip, 0L);
    if (ctx.gages.rain_series.size() < n) ctx.gages.rain_series.resize(n);
    grow(ctx.gages.file_format,   RainFileFormat::UNKNOWN);
    grow(ctx.gages.interval_sec,  3600);
    grow(ctx.gages.snow_factor,   1.0);
    grow(ctx.gages.scale_factor,  1.0);
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
    if (ctx.gages.comments.size() < n) ctx.gages.comments.resize(n, std::string{});
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
    // Pre-reserve from the section's row count (an upper bound: some rows
    // are comments or duplicates). Capacity only — see reserve_to().
    ctx.subcatches.reserve_to(ctx.subcatches.count() + static_cast<int>(lines.size()));
    ctx.subcatch_names.reserve(static_cast<std::size_t>(ctx.subcatch_names.size()) + lines.size());
    for (const auto& pl : parse_section(lines)) {
        auto tok = Tokenizer::tokenize(pl.data);
        if (tok.size() < 7) continue;
        // Name  Gage  Outlet  Area  %Imperv  Width  %Slope  [CurbLen]
        //       [Snowpack] [RainScale] [SnowScale]
        //
        // Snowpack (tok 8) accepts "*" as a placeholder meaning "no snow pack",
        // which lets a model reach tokens 9/10 without assigning one. Same
        // convention as the omitted start date in the [RAINGAGES] FILE format.
        // RainScale (tok 9) and SnowScale (tok 10) default to 1.0.

        const std::string& name = tok[0];
        int idx = add_unique(ctx.subcatch_names, name, ctx.errors);
        if (idx < 0) continue;  // duplicate ID (ERR 207, legacy input.c parity)

        ensure_subcatch_capacity(ctx, idx);

        // Gage — resolve index, may be -1 if gage not yet parsed. Store the
        // name so PostParseResolver can re-resolve a forward reference and
        // raise ERR_NAME for a truly undefined gage (legacy fatal).
        ctx.subcatches.gage_name[idx] = tok[1];
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

        // Optional SnowPack (column 8) — store the name for deferred
        // resolution (SNOWPACKS is usually parsed after SUBCATCHMENTS) and
        // attempt an immediate resolve in case it was parsed first.
        // "*" is a positional placeholder, not a snow pack name.
        if (tok.size() > 8 && !tok[8].empty() && tok[8] != "*") {
            ctx.subcatches.snowpack_name[idx] = tok[8];
            ctx.subcatches.snowpack[idx] = ctx.snowpack_names.find(tok[8]);
        }

        // Optional rainfall / snowfall scale factors (columns 9, 10).
        // Both default to 1.0; non-positive values are ignored rather than
        // silently zeroing a subcatchment's precipitation.
        if (tok.size() > 9) {
            double rsf = to_double(tok[9]);
            if (rsf > 0.0) ctx.subcatches.rain_scale_factor[idx] = rsf;
        }
        if (tok.size() > 10) {
            double ssf = to_double(tok[10]);
            if (ssf > 0.0) ctx.subcatches.snow_scale_factor[idx] = ssf;
        }

        if (!pl.comment.empty())
            ctx.subcatches.comments[static_cast<std::size_t>(idx)] = pl.comment;
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
    for (const auto& pl : parse_section(lines)) {
        auto tok = Tokenizer::tokenize(pl.data);
        if (tok.size() < 5) continue;
        // Name  Format  Interval  SCF  Source  [SourceName]

        const std::string& name = tok[0];
        int idx = add_unique(ctx.gage_names, name, ctx.errors);
        if (idx < 0) continue;  // duplicate ID (ERR 207, legacy input.c parity)

        ensure_gage_capacity(ctx, idx);

        // Format: VOLUME or INTENSITY → store in rain_type
        const std::string fmt = Tokenizer::to_upper(tok[1]);
        if (fmt == "VOLUME")          ctx.gages.rain_type[idx] = 1;
        else if (fmt == "CUMULATIVE") ctx.gages.rain_type[idx] = 2;
        else                          ctx.gages.rain_type[idx] = 0; // INTENSITY

        // Recording interval. SWMM specifies the rain-gage interval in
        // DECIMAL HOURS or "H:MM[:SS]" format — NOT seconds. A bare decimal
        // such as "0.08333" means 0.08333 hours (= 300 s); the generic
        // parse_time_seconds() would mis-read it as 0.08333 s and truncate to
        // 0, which empties the rain window and yields ZERO precipitation on
        // every timeseries/file rain gage. Match legacy datetime_strToTime
        // (decimal hours → fraction of day → seconds).
        const std::string& itok = tok[2];
        double interval_secs =
            (itok.find(':') != std::string::npos)
                ? parse_time_seconds(itok)        // H:MM[:SS] clock duration
                : to_double(itok, 0.0) * 3600.0;  // decimal hours
        ctx.gages.interval_sec[idx] =
            static_cast<int>(interval_secs + 0.5);

        // Snow correction factor
        ctx.gages.snow_factor[idx] = to_double(tok[3], 1.0);

        // Source: TIMESERIES <name>  or  FILE "<path>"  or  FILE "<path:col>"
        const std::string src = Tokenizer::to_upper(tok[4]);

        if (src == "TIMESERIES" && tok.size() > 5) {
            ctx.gages.source[idx]   = RainSource::TIMESERIES;
            ctx.gages.ts_name[idx]  = tok[5]; // Store name for deferred resolution
            ctx.gages.ts_index[idx] = ctx.find_timeseries(tok[5]);
            // ts_index may be -1 if TIMESERIES section appears after RAINGAGES

            // Optional trailing rainfall scaling factor (legacy gage.c readGageSeriesFormat tok[6])
            if (tok.size() > 6) {
                double sf = to_double(tok[6], 1.0);
                if (sf > 0.0) ctx.gages.scale_factor[idx] = sf;
            }
        } else if (src == "FILE" && tok.size() > 5) {
            ctx.gages.source[idx] = RainSource::FILE_RAIN;

            // tok[5] already has quotes stripped by the tokenizer.
            // Check for "path:COLUMN" syntax (R08) using the SHARED split
            // rule (MultiColumnSeriesFile.hpp) — the timeseries loader used
            // to split differently, so a path containing a colon produced two
            // different cache keys for one file and defeated the single-read
            // guarantee.
            std::string file_only, file_col;
            if (split_series_file_token(tok[5], file_only, file_col)) {
                ctx.gages.file_path[idx]   = file_only;
                ctx.gages.col_name[idx]    = file_col;
                ctx.gages.file_format[idx] = RainFileFormat::USER_CSV;
            } else {
                ctx.gages.file_path[idx]   = file_only;
                ctx.gages.file_format[idx] = RainFileFormat::STAN_PRCP;
            }

            // Standard SWMM FILE grammar (legacy gage.c gage_readParams):
            //   Name Format Interval SCF FILE Fname Station Units [StartDate] [SCF]
            // tok[5]=Fname, tok[6]=Station, tok[7]=Units (IN|MM),
            // tok[8]=optional start date, tok[9]=optional rainfall scale factor.
            // Station + Units are required for STAN_PRCP; the compact `path:col`
            // (USER_CSV) form keeps its older behaviour (trailing scale factor).
            if (ctx.gages.file_format[idx] == RainFileFormat::STAN_PRCP) {
                // '*' is the writer's placeholder for "no station" — normalize
                // back to empty ("accept all rows") instead of filtering on a
                // literal '*' station that would zero out the rainfall.
                if (tok.size() > 6 && tok[6] != "*") ctx.gages.station_id[idx] = tok[6];
                if (tok.size() > 7) {
                    const std::string u = Tokenizer::to_upper(tok[7]);
                    ctx.gages.rain_units[idx] = (u == "MM") ? 1 : 0; // default IN
                }
                // The trailing scale factor is honoured only when present as a
                // positive number (a start date token would not parse as one).
                if (tok.size() > 9) {
                    double sf = to_double(tok[9], 1.0);
                    if (sf > 0.0) ctx.gages.scale_factor[idx] = sf;
                }
            } else {
                if (tok.size() > 6) {
                    double sf = to_double(tok[6], 1.0);
                    if (sf > 0.0) ctx.gages.scale_factor[idx] = sf;
                }
            }
        }
        if (!pl.comment.empty())
            ctx.gages.comments[static_cast<std::size_t>(idx)] = pl.comment;
    }
}

} /* namespace openswmm::input */
