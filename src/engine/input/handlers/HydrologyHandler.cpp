/**
 * @file HydrologyHandler.cpp
 * @brief Section handlers for [EVAPORATION], [TEMPERATURE], [SNOWPACKS],
 *        [AQUIFERS], [GROUNDWATER], [GWF], [LID_CONTROLS], and [LID_USAGE].
 *
 * ### [EVAPORATION] format
 * ```
 * CONSTANT  0.0
 * MONTHLY   0.1  0.1  0.2  0.3  0.4  0.5  0.5  0.4  0.3  0.2  0.1  0.1
 * TIMESERIES  EvapTS
 * TEMPERATURE
 * FILE
 * RECOVERY  patternName
 * DRY_ONLY  NO
 * ```
 *
 * ### [TEMPERATURE] format
 * ```
 * TIMESERIES  TempTS
 * FILE  "filename"  StartDate
 * WINDSPEED  MONTHLY  1.0 1.0 ... (12 values)
 * WINDSPEED  FILE
 * SNOWMELT  divT  ATIwt  nrgRatio  lat  minMelt  maxMelt
 * ADC  IMPERVIOUS  frac1  frac2  ... frac10
 * ADC  PERVIOUS    frac1  frac2  ... frac10
 * ```
 *
 * ### [SNOWPACKS] format
 * ```
 * SP1  PLOWABLE    cmin cmax tbase fwfrac sd0 fw0 snn0
 * SP1  IMPERVIOUS  cmin cmax tbase fwfrac sd0 fw0 snn0
 * SP1  PERVIOUS    cmin cmax tbase fwfrac sd0 fw0 snn0
 * SP1  REMOVAL     dSnow fOut fImp fPerv fImelt fToSubcatch SubcatchName
 * ```
 *
 * ### [AQUIFERS] format
 * ```
 * Name  Por WP FC Ksat Kslope Tslope ETu ETs Seep Ebot Egw Umc [ETupat]
 * ```
 *
 * ### [GROUNDWATER] format
 * ```
 * Subcatch  Aquifer  Node  SurfEl  A1  B1  A2  B2  A3  Twgr  Hstar
 * ```
 *
 * ### [LID_CONTROLS] format (multi-line per LID)
 * ```
 * LID1  BC
 * LID1  SURFACE   ...
 * LID1  SOIL      ...
 * LID1  STORAGE   ...
 * LID1  DRAIN     ...
 * ```
 *
 * ### [LID_USAGE] format
 * ```
 * Subcatch LID Number Area Width InitSat FromImp ToPerv [RptFile] [DrainTo] [FromPerv]
 * ```
 *
 * @see Legacy reference: src/solver/input.c
 * @ingroup engine_input
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#include "HydrologyHandler.hpp"

#include "../Tokenizer.hpp"
#include "../../core/SimulationContext.hpp"
#include "../../data/SubcatchData.hpp"
#include "../../data/HydrologyData.hpp"

#include "../../core/charconv_compat.hpp"

#include <charconv>
#include <string>
#include <algorithm>
#include <array>

namespace openswmm::input {

// ============================================================================
// Helpers
// ============================================================================

static double to_double(std::string_view sv, double def = 0.0) noexcept {
    double v = def;
    openswmm::from_chars_double(sv.data(), sv.data() + sv.size(), v);
    return v;
}

static int to_int(std::string_view sv, int def = 0) noexcept {
    int v = def;
    std::from_chars(sv.data(), sv.data() + sv.size(), v);
    return v;
}

static void ensure_snowpack_capacity(SnowpackStore& sp, int idx) {
    const auto n = static_cast<std::size_t>(idx + 1);
    if (sp.plowable.size() < n)   sp.plowable.resize(n, {0,0,0,0,0,0,0});
    if (sp.impervious.size() < n) sp.impervious.resize(n, {0,0,0,0,0,0,0});
    if (sp.pervious.size() < n)   sp.pervious.resize(n, {0,0,0,0,0,0,0});
    if (sp.removal.size() < n)    sp.removal.resize(n, {0,0,0,0,0,0});
    if (sp.removal_subcatch.size() < n) sp.removal_subcatch.resize(n, std::string{});
}

static void ensure_aquifer_capacity(AquiferStore& aq, int idx) {
    const auto n = static_cast<std::size_t>(idx + 1);
    auto grow = [&](auto& vec, auto def) {
        if (vec.size() < n) vec.resize(n, def);
    };
    grow(aq.porosity,         0.5);
    grow(aq.wilting_point,    0.15);
    grow(aq.field_capacity,   0.30);
    grow(aq.conductivity,     0.1);
    grow(aq.conduct_slope,    10.0);
    grow(aq.tension_slope,    15.0);
    grow(aq.upper_evap,       0.35);
    grow(aq.lower_evap,       14.0);
    grow(aq.lower_loss,       0.002);
    grow(aq.bottom_elev,      0.0);
    grow(aq.water_table_elev, 0.0);
    grow(aq.upper_moist,      0.30);
    grow(aq.upper_evap_pat,   std::string{});
}

static void ensure_lid_capacity(LidControlStore& lc, int idx) {
    const auto n = static_cast<std::size_t>(idx + 1);
    if (lc.lid_type.size() < n)  lc.lid_type.resize(n, std::string{});
    if (lc.surface.size() < n)   lc.surface.resize(n, {0,0,0,0,0});
    if (lc.soil.size() < n)     lc.soil.resize(n, {0,0,0,0,0,0,0});
    if (lc.pavement.size() < n) lc.pavement.resize(n, {0,0,0,0,0,0});
    if (lc.storage.size() < n)  lc.storage.resize(n, {0,0,0,0});
    if (lc.drain.size() < n)    lc.drain.resize(n, {0,0,0,0,0,0});
    if (lc.drainmat.size() < n) lc.drainmat.resize(n, {0,0,0});
}

static void ensure_subcatch_gw_capacity(SimulationContext& ctx, int idx) {
    const auto n = static_cast<std::size_t>(idx + 1);
    auto grow = [&](auto& vec, auto def) {
        if (vec.size() < n) vec.resize(n, def);
    };
    grow(ctx.subcatches.gw_aquifer,   -1);
    grow(ctx.subcatches.gw_node,      -1);
    grow(ctx.subcatches.gw_surf_elev, 0.0);
    grow(ctx.subcatches.gw_a1,        0.0);
    grow(ctx.subcatches.gw_b1,        0.0);
    grow(ctx.subcatches.gw_a2,        0.0);
    grow(ctx.subcatches.gw_b2,        0.0);
    grow(ctx.subcatches.gw_a3,        0.0);
    grow(ctx.subcatches.gw_tw,        0.0);
    grow(ctx.subcatches.gw_hstar,     0.0);
}

// ============================================================================
// handle_evaporation()
// ============================================================================

void handle_evaporation(SimulationContext& ctx, const std::vector<std::string>& lines) {
    for (const auto& line : lines) {
        auto tok = Tokenizer::tokenize(line);
        if (tok.empty()) continue;

        const std::string key = Tokenizer::to_upper(tok[0]);

        if (key == "CONSTANT" && tok.size() >= 2) {
            ctx.options.evap_type = 0;
            double val = to_double(tok[1]);
            for (int i = 0; i < 12; ++i)
                ctx.options.evap_values[i] = val;
        }
        else if (key == "MONTHLY" && tok.size() >= 13) {
            ctx.options.evap_type = 1;
            for (int i = 0; i < 12; ++i)
                ctx.options.evap_values[i] = to_double(tok[1 + i]);
        }
        else if (key == "TIMESERIES" && tok.size() >= 2) {
            ctx.options.evap_type = 2;
            ctx.options.evap_ts_name = tok[1];
        }
        else if (key == "TEMPERATURE") {
            ctx.options.evap_type = 3;
        }
        else if (key == "FILE") {
            ctx.options.evap_type = 4;
        }
        else if (key == "RECOVERY" && tok.size() >= 2) {
            ctx.options.evap_recovery_pat = tok[1];
        }
        else if (key == "DRY_ONLY" && tok.size() >= 2) {
            ctx.options.evap_dry_only = Tokenizer::parse_boolean(tok[1]);
        }
    }
}

// ============================================================================
// handle_temperature()
// ============================================================================

void handle_temperature(SimulationContext& ctx, const std::vector<std::string>& lines) {
    for (const auto& line : lines) {
        auto tok = Tokenizer::tokenize(line);
        if (tok.empty()) continue;

        const std::string key = Tokenizer::to_upper(tok[0]);

        if (key == "TIMESERIES" && tok.size() >= 2) {
            ctx.options.temp_source = 1;
            ctx.options.temp_ts_name = tok[1];
        }
        else if (key == "FILE" && tok.size() >= 2) {
            ctx.options.temp_source = 2;
            ctx.options.temp_file = tok[1];
            if (tok.size() >= 3)
                ctx.options.temp_file_start = to_double(tok[2]);
        }
        else if (key == "WINDSPEED" && tok.size() >= 2) {
            const std::string wtype = Tokenizer::to_upper(tok[1]);
            if (wtype == "MONTHLY" && tok.size() >= 14) {
                ctx.options.wind_type = 0;
                for (int i = 0; i < 12; ++i)
                    ctx.options.wind_speed[i] = to_double(tok[2 + i]);
            }
            else if (wtype == "FILE") {
                ctx.options.wind_type = 1;
            }
        }
        else if (key == "SNOWMELT" && tok.size() >= 7) {
            ctx.options.snow_divt      = to_double(tok[1]);
            ctx.options.snow_ati_wt    = to_double(tok[2]);
            ctx.options.snow_nrg_ratio = to_double(tok[3]);
            ctx.options.snow_lat       = to_double(tok[4]);
            ctx.options.snow_min_melt  = to_double(tok[5]);
            ctx.options.snow_max_melt  = to_double(tok[6]);
        }
        else if (key == "ADC" && tok.size() >= 12) {
            const std::string surface = Tokenizer::to_upper(tok[1]);
            if (surface == "IMPERVIOUS") {
                for (int i = 0; i < 10; ++i)
                    ctx.options.adc_imperv[i] = to_double(tok[2 + i]);
            }
            else if (surface == "PERVIOUS") {
                for (int i = 0; i < 10; ++i)
                    ctx.options.adc_perv[i] = to_double(tok[2 + i]);
            }
        }
    }
}

// ============================================================================
// handle_snowpacks()
// ============================================================================

void handle_snowpacks(SimulationContext& ctx, const std::vector<std::string>& lines) {
    for (const auto& line : lines) {
        auto tok = Tokenizer::tokenize(line);
        if (tok.size() < 2) continue;

        const std::string& name = tok[0];
        int idx = ctx.snowpack_names.find(name);
        if (idx < 0) {
            idx = ctx.snowpack_names.add(name);
            ctx.snowpacks.names.push_back(name);
        }

        ensure_snowpack_capacity(ctx.snowpacks, idx);

        const std::string surface = Tokenizer::to_upper(tok[1]);

        if (surface == "PLOWABLE" && tok.size() >= 9) {
            for (int i = 0; i < 7; ++i)
                ctx.snowpacks.plowable[idx][i] = to_double(tok[2 + i]);
        }
        else if (surface == "IMPERVIOUS" && tok.size() >= 9) {
            for (int i = 0; i < 7; ++i)
                ctx.snowpacks.impervious[idx][i] = to_double(tok[2 + i]);
        }
        else if (surface == "PERVIOUS" && tok.size() >= 9) {
            for (int i = 0; i < 7; ++i)
                ctx.snowpacks.pervious[idx][i] = to_double(tok[2 + i]);
        }
        else if (surface == "REMOVAL" && tok.size() >= 8) {
            for (int i = 0; i < 6; ++i)
                ctx.snowpacks.removal[idx][i] = to_double(tok[2 + i]);
            if (tok.size() >= 9)
                ctx.snowpacks.removal_subcatch[idx] = tok[8];
        }
    }
}

// ============================================================================
// handle_aquifers()
// ============================================================================

void handle_aquifers(SimulationContext& ctx, const std::vector<std::string>& lines) {
    for (const auto& line : lines) {
        auto tok = Tokenizer::tokenize(line);
        if (tok.size() < 13) continue;
        // Name Por WP FC Ksat Kslope Tslope ETu ETs Seep Ebot Egw Umc [ETupat]

        const std::string& name = tok[0];
        int idx = ctx.aquifer_names.find(name);
        if (idx < 0) {
            idx = ctx.aquifer_names.add(name);
            ctx.aquifers.names.push_back(name);
        }

        ensure_aquifer_capacity(ctx.aquifers, idx);

        ctx.aquifers.porosity[idx]         = to_double(tok[1]);
        ctx.aquifers.wilting_point[idx]     = to_double(tok[2]);
        ctx.aquifers.field_capacity[idx]    = to_double(tok[3]);
        ctx.aquifers.conductivity[idx]      = to_double(tok[4]);
        ctx.aquifers.conduct_slope[idx]     = to_double(tok[5]);
        ctx.aquifers.tension_slope[idx]     = to_double(tok[6]);
        ctx.aquifers.upper_evap[idx]        = to_double(tok[7]);
        ctx.aquifers.lower_evap[idx]        = to_double(tok[8]);
        ctx.aquifers.lower_loss[idx]        = to_double(tok[9]);
        ctx.aquifers.bottom_elev[idx]       = to_double(tok[10]);
        ctx.aquifers.water_table_elev[idx]  = to_double(tok[11]);
        ctx.aquifers.upper_moist[idx]       = to_double(tok[12]);

        if (tok.size() >= 14)
            ctx.aquifers.upper_evap_pat[idx] = tok[13];
    }
}

// ============================================================================
// handle_groundwater()
// ============================================================================

void handle_groundwater(SimulationContext& ctx, const std::vector<std::string>& lines) {
    for (const auto& line : lines) {
        auto tok = Tokenizer::tokenize(line);
        if (tok.size() < 11) continue;
        // Subcatch  Aquifer  Node  SurfEl  A1  B1  A2  B2  A3  Twgr  Hstar

        const int idx = ctx.subcatch_names.find(tok[0]);
        if (idx < 0) continue;

        ensure_subcatch_gw_capacity(ctx, idx);

        ctx.subcatches.gw_aquifer[idx]   = ctx.aquifer_names.find(tok[1]);
        ctx.subcatches.gw_node[idx]      = ctx.node_names.find(tok[2]);
        ctx.subcatches.gw_surf_elev[idx] = to_double(tok[3]);
        ctx.subcatches.gw_a1[idx]        = to_double(tok[4]);
        ctx.subcatches.gw_b1[idx]        = to_double(tok[5]);
        ctx.subcatches.gw_a2[idx]        = to_double(tok[6]);
        ctx.subcatches.gw_b2[idx]        = to_double(tok[7]);
        ctx.subcatches.gw_a3[idx]        = to_double(tok[8]);
        ctx.subcatches.gw_tw[idx]        = to_double(tok[9]);
        ctx.subcatches.gw_hstar[idx]     = to_double(tok[10]);
    }
}

// ============================================================================
// handle_gwf()
// ============================================================================

void handle_gwf(SimulationContext& ctx, const std::vector<std::string>& lines) {
    // [GWF] contains custom groundwater flow expressions.
    // Format: Subcatch  LATERAL/DEEP  expression...
    // For now, store as extension options keyed by "GWF:<subcatch>:<type>".
    for (const auto& line : lines) {
        auto tok = Tokenizer::tokenize(line);
        if (tok.size() < 3) continue;

        const std::string& subcatch = tok[0];
        const std::string type = Tokenizer::to_upper(tok[1]);

        // Reconstruct expression from remaining tokens
        std::string expr;
        for (std::size_t i = 2; i < tok.size(); ++i) {
            if (!expr.empty()) expr += ' ';
            expr += tok[i];
        }

        std::string key = "GWF:" + subcatch + ":" + type;
        ctx.options.ext_options[key] = expr;
    }
}

// ============================================================================
// handle_lid_controls()
// ============================================================================

void handle_lid_controls(SimulationContext& ctx, const std::vector<std::string>& lines) {
    for (const auto& line : lines) {
        auto tok = Tokenizer::tokenize(line);
        if (tok.size() < 2) continue;

        const std::string& name = tok[0];
        int idx = ctx.lid_names.find(name);
        if (idx < 0) {
            idx = ctx.lid_names.add(name);
            ctx.lid_controls.names.push_back(name);
        }

        ensure_lid_capacity(ctx.lid_controls, idx);

        const std::string layer = Tokenizer::to_upper(tok[1]);

        // First line for a LID sets its type code (e.g., BC, RG, GR, etc.)
        if (tok.size() == 2) {
            ctx.lid_controls.lid_type[idx] = layer;
            continue;
        }

        // Subsequent lines define layer parameters
        if (layer == "SURFACE") {
            for (std::size_t i = 0; i < 5 && (i + 2) < tok.size(); ++i)
                ctx.lid_controls.surface[idx][i] = to_double(tok[2 + i]);
        }
        else if (layer == "SOIL") {
            for (std::size_t i = 0; i < 7 && (i + 2) < tok.size(); ++i)
                ctx.lid_controls.soil[idx][i] = to_double(tok[2 + i]);
        }
        else if (layer == "PAVEMENT") {
            for (std::size_t i = 0; i < 6 && (i + 2) < tok.size(); ++i)
                ctx.lid_controls.pavement[idx][i] = to_double(tok[2 + i]);
        }
        else if (layer == "STORAGE") {
            for (std::size_t i = 0; i < 4 && (i + 2) < tok.size(); ++i)
                ctx.lid_controls.storage[idx][i] = to_double(tok[2 + i]);
        }
        else if (layer == "DRAIN") {
            for (std::size_t i = 0; i < 6 && (i + 2) < tok.size(); ++i)
                ctx.lid_controls.drain[idx][i] = to_double(tok[2 + i]);
        }
        else if (layer == "DRAINMAT") {
            for (std::size_t i = 0; i < 3 && (i + 2) < tok.size(); ++i)
                ctx.lid_controls.drainmat[idx][i] = to_double(tok[2 + i]);
        }
        else {
            // Unknown layer or it's actually the type code with params
            // (e.g., "LID1 BC" when BC is the type)
            ctx.lid_controls.lid_type[idx] = layer;
        }
    }
}

// ============================================================================
// handle_lid_usage()
// ============================================================================

void handle_lid_usage(SimulationContext& ctx, const std::vector<std::string>& lines) {
    for (const auto& line : lines) {
        auto tok = Tokenizer::tokenize(line);
        if (tok.size() < 8) continue;
        // Subcatch LID Number Area Width InitSat FromImp ToPerv [RptFile] [DrainTo] [FromPerv]

        const int sc_idx  = ctx.subcatch_names.find(tok[0]);
        const int lid_idx = ctx.lid_names.find(tok[1]);
        if (sc_idx < 0 || lid_idx < 0) continue;

        ctx.lid_usage.subcatch_index.push_back(sc_idx);
        ctx.lid_usage.lid_index.push_back(lid_idx);
        ctx.lid_usage.number.push_back(to_int(tok[2], 1));
        ctx.lid_usage.area.push_back(to_double(tok[3]));
        ctx.lid_usage.width.push_back(to_double(tok[4]));
        ctx.lid_usage.init_sat.push_back(to_double(tok[5]));
        ctx.lid_usage.from_imperv.push_back(to_double(tok[6]));
        ctx.lid_usage.to_perv.push_back(to_int(tok[7]));

        ctx.lid_usage.rpt_file.push_back(tok.size() > 8 ? tok[8] : std::string{});
        ctx.lid_usage.drain_to.push_back(tok.size() > 9 ? tok[9] : std::string{});
        ctx.lid_usage.from_perv.push_back(tok.size() > 10 ? to_double(tok[10]) : 0.0);
    }
}

} /* namespace openswmm::input */
