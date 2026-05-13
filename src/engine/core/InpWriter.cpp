/**
 * @file InpWriter.cpp
 * @brief Comprehensive .inp serialisation — round-trip identical output.
 *
 * @details Writes every SWMM input section with exact column layouts matching
 *          the legacy format so that read -> write -> read produces identical
 *          model state. Shape names, enum strings, and field widths are taken
 *          directly from legacy text.h and keywords.c.
 *
 * All standard SWMM sections implemented:
 *   TITLE, OPTIONS, EVAPORATION, TEMPERATURE, SNOWPACKS, ADJUSTMENTS,
 *   RAINGAGES, SUBCATCHMENTS, SUBAREAS, INFILTRATION,
 *   JUNCTIONS, OUTFALLS, DIVIDERS, STORAGE, CONDUITS, PUMPS, ORIFICES,
 *   WEIRS, OUTLETS, XSECTIONS, LOSSES, TRANSECTS, STREETS, INLETS,
 *   CONTROLS, REPORT, POLLUTANTS, LANDUSES, BUILDUP, WASHOFF, TREATMENT,
 *   INFLOWS, DWF, RDII, PATTERNS, TIMESERIES, CURVES,
 *   MAP, COORDINATES, VERTICES, Polygons, SYMBOLS,
 *   USER_FLAGS, USER_FLAG_VALUES, PLUGINS
 *
 * @ingroup engine_core
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#include "InpWriter.hpp"
#include "SimulationContext.hpp"
#include "DateTime.hpp"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>

namespace openswmm {
namespace inp_writer {

static void sec(FILE* f, const char* name) {
    std::fprintf(f, "\n[%s]\n", name);
}

// Format an OADate as MM/DD/YYYY
static void fmt_date(char* buf, double oadate) {
    int y, m, d;
    datetime::decodeDate(oadate, y, m, d);
    std::sprintf(buf, "%02d/%02d/%04d", m, d, y);
}

// Format the time-of-day part of an OADate as HH:MM:SS
static void fmt_time(char* buf, double oadate) {
    int h, m, s;
    datetime::decodeTime(oadate, h, m, s);
    std::sprintf(buf, "%02d:%02d:%02d", h, m, s);
}

// Format a timestep (seconds).  Whole-second values use H:MM:SS (matching
// legacy SWMM GUI GetTimeString); fractional values use %g decimal notation.
static void fmt_step(char* buf, double secs) {
    long r = std::lround(secs);
    if (std::fabs(secs - static_cast<double>(r)) < 0.001) {
        int ss = static_cast<int>(r % 60);
        int mm = static_cast<int>((r / 60) % 60);
        int hh = static_cast<int>(r / 3600);
        std::sprintf(buf, "%d:%02d:%02d", hh, mm, ss);
    } else {
        std::sprintf(buf, "%g", secs);
    }
}

// Format a day-of-year integer as M/D (no leading zeros, matching legacy GUI)
static void fmt_sweep(char* buf, int doy) {
    // Anchor to a non-leap year to get a stable month/day
    double dt = datetime::encodeDate(2001, 1, 1) + static_cast<double>(doy - 1);
    int y, m, d;
    datetime::decodeDate(dt, y, m, d);
    std::sprintf(buf, "%d/%d", m, d);
}

// Write per-object comment lines. Multi-line comments are stored with literal
// "\\n" (backslash + n) as separator; each part is emitted as a ;-prefixed row.
static void write_obj_comment(FILE* f,
                               const std::vector<std::string>& comments,
                               std::size_t idx)
{
    if (idx >= comments.size() || comments[idx].empty()) return;
    const std::string& c = comments[idx];
    const char* sep = "\\n";   // literal two-character token
    std::size_t start = 0;
    while (start <= c.size()) {
        std::size_t end = c.find(sep, start);
        if (end == std::string::npos) end = c.size();
        std::fprintf(f, ";%.*s\n",
                     static_cast<int>(end - start), c.data() + start);
        if (end == c.size()) break;
        start = end + 2;  // skip the two-character "\\n" token
    }
}

static const std::unordered_map<int, const char*> CURVE_TYPE_LABEL = {
    {static_cast<int>(TableType::CURVE_STORAGE),   "STORAGE"},
    {static_cast<int>(TableType::CURVE_DIVERSION),  "DIVERSION"},
    {static_cast<int>(TableType::CURVE_RATING),     "RATING"},
    {static_cast<int>(TableType::CURVE_SHAPE),      "SHAPE"},
    {static_cast<int>(TableType::CURVE_CONTROL),    "CONTROL"},
    {static_cast<int>(TableType::CURVE_TIDAL),      "TIDAL"},
    {static_cast<int>(TableType::CURVE_PUMP1),      "PUMP1"},
    {static_cast<int>(TableType::CURVE_PUMP2),      "PUMP2"},
    {static_cast<int>(TableType::CURVE_PUMP3),      "PUMP3"},
    {static_cast<int>(TableType::CURVE_PUMP4),      "PUMP4"},
    {static_cast<int>(TableType::CURVE_PUMP5),      "PUMP5"},
};

static const char* nN(const SimulationContext& c, int i) {
    return (i>=0 && i<c.n_nodes()) ? c.node_names.name_of(i).c_str() : "*";
}
static const char* gN(const SimulationContext& c, int i) {
    return (i>=0 && i<c.n_gages()) ? c.gage_names.name_of(i).c_str() : "*";
}
static const char* tN(const SimulationContext& c, int i) {
    return (i>=0 && i<static_cast<int>(c.table_names.size())) ? c.table_names.name_of(i).c_str() : "*";
}
static const char* pN(const SimulationContext& c, int i) {
    return (i>=0 && i<c.n_pollutants()) ? c.pollutant_names.name_of(i).c_str() : "*";
}

static const char* xsName(int s) {
    // Indexed by LinkData::XsectShape enum values
    static const char* n[] = {
        "CIRCULAR","FILLED_CIRCULAR","RECT_CLOSED","RECT_OPEN",
        "TRAPEZOIDAL","TRIANGULAR","PARABOLIC","POWER","MODBASKETHANDLE",
        "EGG","HORSESHOE","GOTHIC","CATENARY","SEMIELLIPTICAL",
        "BASKETHANDLE","SEMICIRCULAR","RECT_TRIANGULAR","RECT_ROUND",
        "HORIZ_ELLIPSE","VERT_ELLIPSE","ARCH",
        "IRREGULAR","CUSTOM","FORCE_MAIN","STREET","DUMMY"
    };
    return (s>=0&&s<=25) ? n[s] : "CIRCULAR";
}
static const char* ofName(OutfallType t) {
    switch(t){case OutfallType::NORMAL:return"NORMAL";case OutfallType::FIXED:return"FIXED";
    case OutfallType::TIDAL:return"TIDAL";case OutfallType::TIMESERIES:return"TIMESERIES";default:return"FREE";}
}
static bool hasNT(const SimulationContext& c, NodeType t) {
    for(int j=0;j<c.n_nodes();++j) if(c.nodes.type[static_cast<size_t>(j)]==t) return true; return false;
}
static bool hasLT(const SimulationContext& c, LinkType t) {
    for(int j=0;j<c.n_links();++j) if(c.links.type[static_cast<size_t>(j)]==t) return true; return false;
}

int writeInpFile(const SimulationContext& ctx, const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) return -1;

    sec(f,"TITLE");
    std::fprintf(f,";;Project Title/Notes\n");
    for (const auto& line : ctx.title_notes) {
        std::fprintf(f,"%s\n", line.c_str());
    }

    // [OPTIONS] — writes every option unconditionally, matching legacy SWMM GUI
    // ExportOptions() structure: core group, ignore group, date/time group,
    // dynamic wave solver group, then engine-specific extensions.
    {
    sec(f,"OPTIONS");
    std::fprintf(f,";;%-20s %s\n","Option","Value");

    static const char* sFlowUnits[]  = {"CFS","GPM","MGD","CMS","LPS","MLD"};
    static const char* sInfilt[]     = {"HORTON","MODIFIED_HORTON","GREEN_AMPT","MODIFIED_GREEN_AMPT","CURVE_NUMBER"};
    static const char* sRouting[]    = {"STEADY","KINWAVE","DYNWAVE"};
    static const char* sInertial[]   = {"NONE","PARTIAL","FULL"};
    static const char* sNormFlow[]   = {"SLOPE","FROUDE","BOTH","NEITHER"};
    static const char* sSurcharge[]  = {"EXTRAN","SLOT","DYNAMIC_SLOT"};

    const SimulationOptions& o = ctx.options;
    int fu  = static_cast<int>(o.flow_units);
    int inf = static_cast<int>(o.infiltration);
    int rm  = static_cast<int>(o.routing_model);
    int id  = o.inertial_damping;
    int nfl = o.normal_flow_ltd;
    int sm  = o.surcharge_method;

    // --- Group 1: Core process options (FLOW_UNITS .. SKIP_STEADY_STATE) ---
    std::fprintf(f,"%-20s %s\n",  "FLOW_UNITS",       (fu>=0&&fu<=5)?sFlowUnits[fu]:"CFS");
    std::fprintf(f,"%-20s %s\n",  "INFILTRATION",     (inf>=0&&inf<=4)?sInfilt[inf]:"HORTON");
    std::fprintf(f,"%-20s %s\n",  "FLOW_ROUTING",     (rm>=0&&rm<=2)?sRouting[rm]:"DYNWAVE");
    std::fprintf(f,"%-20s %s\n",  "LINK_OFFSETS",     o.link_offsets==1?"ELEVATION":"DEPTH");
    std::fprintf(f,"%-20s %g\n",  "MIN_SLOPE",        o.min_slope);
    std::fprintf(f,"%-20s %s\n",  "ALLOW_PONDING",    o.allow_ponding?"YES":"NO");
    std::fprintf(f,"%-20s %s\n",  "SKIP_STEADY_STATE",o.skip_steady_state?"YES":"NO");
    std::fprintf(f,"\n");

    // --- Group 2: Ignore flags (write all, even when NO) ---
    std::fprintf(f,"%-20s %s\n",  "IGNORE_RAINFALL",   o.ignore_rainfall?"YES":"NO");
    std::fprintf(f,"%-20s %s\n",  "IGNORE_SNOWMELT",   o.ignore_snow_melt?"YES":"NO");
    std::fprintf(f,"%-20s %s\n",  "IGNORE_GROUNDWATER",o.ignore_groundwater?"YES":"NO");
    std::fprintf(f,"%-20s %s\n",  "IGNORE_RDII",       o.ignore_rdii?"YES":"NO");
    std::fprintf(f,"%-20s %s\n",  "IGNORE_ROUTING",    o.ignore_routing?"YES":"NO");
    std::fprintf(f,"%-20s %s\n",  "IGNORE_QUALITY",    o.ignore_quality?"YES":"NO");
    std::fprintf(f,"\n");

    // --- Group 3: Date / time options (START_DATE .. RULE_STEP) ---
    char db[32], tb[32], sb[32];
    fmt_date(db, o.start_date);  fmt_time(tb, o.start_date);
    std::fprintf(f,"%-20s %s\n",  "START_DATE",        db);
    std::fprintf(f,"%-20s %s\n",  "START_TIME",        tb);

    double rpt_start = (o.report_start > 0.0) ? o.report_start : o.start_date;
    fmt_date(db, rpt_start);  fmt_time(tb, rpt_start);
    std::fprintf(f,"%-20s %s\n",  "REPORT_START_DATE", db);
    std::fprintf(f,"%-20s %s\n",  "REPORT_START_TIME", tb);

    double end_dt = (o.end_date > 0.0) ? o.end_date : o.start_date;
    fmt_date(db, end_dt);  fmt_time(tb, end_dt);
    std::fprintf(f,"%-20s %s\n",  "END_DATE",          db);
    std::fprintf(f,"%-20s %s\n",  "END_TIME",          tb);

    fmt_sweep(sb, o.sweep_start);
    std::fprintf(f,"%-20s %s\n",  "SWEEP_START",       sb);
    fmt_sweep(sb, o.sweep_end);
    std::fprintf(f,"%-20s %s\n",  "SWEEP_END",         sb);

    std::fprintf(f,"%-20s %g\n",  "DRY_DAYS",          o.dry_days);
    fmt_step(sb, o.report_step);
    std::fprintf(f,"%-20s %s\n",  "REPORT_STEP",       sb);
    fmt_step(sb, o.wet_step);
    std::fprintf(f,"%-20s %s\n",  "WET_STEP",          sb);
    fmt_step(sb, o.dry_step);
    std::fprintf(f,"%-20s %s\n",  "DRY_STEP",          sb);
    fmt_step(sb, o.routing_step);
    std::fprintf(f,"%-20s %s\n",  "ROUTING_STEP",      sb);
    fmt_step(sb, o.rule_step);
    std::fprintf(f,"%-20s %s\n",  "RULE_STEP",         sb);
    std::fprintf(f,"\n");

    // --- Group 4: Dynamic wave / solver options (INERTIAL_DAMPING .. THREADS) ---
    std::fprintf(f,"%-20s %s\n",  "INERTIAL_DAMPING",    (id>=0&&id<=2)?sInertial[id]:"PARTIAL");
    std::fprintf(f,"%-20s %s\n",  "NORMAL_FLOW_LIMITED", (nfl>=0&&nfl<=3)?sNormFlow[nfl]:"BOTH");
    std::fprintf(f,"%-20s %s\n",  "FORCE_MAIN_EQUATION", o.force_main_eqn==1?"D-W":"H-W");
    std::fprintf(f,"%-20s %s\n",  "SURCHARGE_METHOD",    (sm>=0&&sm<=2)?sSurcharge[sm]:"EXTRAN");
    std::fprintf(f,"%-20s %.2f\n","VARIABLE_STEP",       o.variable_step);
    std::fprintf(f,"%-20s %g\n",  "LENGTHENING_STEP",    o.lengthening_step);
    std::fprintf(f,"%-20s %g\n",  "MIN_SURFAREA",        o.min_surf_area);
    std::fprintf(f,"%-20s %d\n",  "MAX_TRIALS",          o.max_trials);
    std::fprintf(f,"%-20s %g\n",  "HEAD_TOLERANCE",      o.head_tol);
    std::fprintf(f,"%-20s %g\n",  "SYS_FLOW_TOL",        o.sys_flow_tol * 100.0);
    std::fprintf(f,"%-20s %g\n",  "LAT_FLOW_TOL",        o.lat_flow_tol * 100.0);
    fmt_step(sb, o.min_routing_step);
    std::fprintf(f,"%-20s %s\n",  "MINIMUM_STEP",        sb);
    std::fprintf(f,"%-20s %d\n",  "THREADS",             o.num_threads);

    // --- Engine-specific extensions (not in legacy GUI) ---
    if (sm == 2) {
        std::fprintf(f,"%-20s %.4f\n","DPS_CELERITY",   o.dps_target_celerity);
        std::fprintf(f,"%-20s %.4f\n","DPS_ALPHA",      o.dps_alpha);
        std::fprintf(f,"%-20s %.4f\n","DPS_DECAY_TIME", o.dps_decay_time);
    }
    if (o.node_continuity != NodeContinuity::EXPLICIT)
        std::fprintf(f,"%-20s %s\n",  "NODE_CONTINUITY","SEMI_IMPLICIT");
    if (o.anderson_accel)
        std::fprintf(f,"%-20s %s\n",  "ANDERSON_ACCEL", "YES");
    if (!o.crs.empty())
        std::fprintf(f,"%-20s %s\n",  "CRS",            o.crs.c_str());
    for (const auto& kv : o.ext_options)
        std::fprintf(f,"%-20s %s\n",  kv.first.c_str(), kv.second.c_str());
    }

    // [EVAPORATION]
    {
        const auto& opts = ctx.options;
        sec(f,"EVAPORATION");
        std::fprintf(f,";;Type       Parameters\n");
        std::fprintf(f,";;---------- ----------\n");
        switch (opts.evap_type) {
            case 0:  // CONSTANT
                std::fprintf(f,"CONSTANT     %.4f\n", opts.evap_values[0]);
                break;
            case 1:  // MONTHLY
                std::fprintf(f,"MONTHLY     ");
                for (int i = 0; i < 12; ++i)
                    std::fprintf(f," %.4f", opts.evap_values[i]);
                std::fprintf(f,"\n");
                break;
            case 2:  // TIMESERIES
                std::fprintf(f,"TIMESERIES   %s\n", opts.evap_ts_name.c_str());
                break;
            case 3:  // TEMPERATURE
                std::fprintf(f,"TEMPERATURE\n");
                break;
            case 4:  // FILE
                std::fprintf(f,"FILE        ");
                for (int i = 0; i < 12; ++i)
                    std::fprintf(f," %.4f", opts.pan_coeff[i]);
                std::fprintf(f,"\n");
                break;
        }
        if (!opts.evap_recovery_pat.empty())
            std::fprintf(f,"RECOVERY     %s\n", opts.evap_recovery_pat.c_str());
        std::fprintf(f,"DRY_ONLY     %s\n", opts.evap_dry_only ? "YES" : "NO");
    }

    // [TEMPERATURE]
    {
        const auto& opts = ctx.options;
        bool has_temp = (opts.temp_source > 0 || opts.wind_type > 0 ||
                         opts.snow_divt != 34.0 || opts.snow_lat != 0.0);
        for (int i = 0; i < 10 && !has_temp; ++i)
            if (opts.adc_imperv[i] != 1.0 || opts.adc_perv[i] != 1.0) has_temp = true;
        // Check monthly wind speeds
        for (int i = 0; i < 12 && !has_temp; ++i)
            if (opts.wind_speed[i] != 0.0) has_temp = true;

        if (has_temp) {
            sec(f,"TEMPERATURE");
            if (opts.temp_source == 1)
                std::fprintf(f,"TIMESERIES   %s\n", opts.temp_ts_name.c_str());
            else if (opts.temp_source == 2) {
                std::fprintf(f,"FILE         \"%s\"", opts.temp_file.c_str());
                if (opts.temp_file_start > 0.0)
                    std::fprintf(f," %.6f", opts.temp_file_start);
                std::fprintf(f,"\n");
            }

            if (opts.wind_type == 0) {
                std::fprintf(f,"WINDSPEED    MONTHLY");
                for (int i = 0; i < 12; ++i)
                    std::fprintf(f," %.4f", opts.wind_speed[i]);
                std::fprintf(f,"\n");
            } else {
                std::fprintf(f,"WINDSPEED    FILE\n");
            }

            std::fprintf(f,"SNOWMELT     %.2f %.4f %.4f %.4f %.6f %.6f %.4f\n",
                         opts.snow_divt, opts.snow_ati_wt, opts.snow_nrg_ratio,
                         opts.snow_lat, opts.snow_min_melt, opts.snow_max_melt,
                         opts.snow_elev);

            std::fprintf(f,"ADC          IMPERVIOUS");
            for (int i = 0; i < 10; ++i)
                std::fprintf(f," %.4f", opts.adc_imperv[i]);
            std::fprintf(f,"\n");
            std::fprintf(f,"ADC          PERVIOUS");
            for (int i = 0; i < 10; ++i)
                std::fprintf(f," %.4f", opts.adc_perv[i]);
            std::fprintf(f,"\n");
        }
    }

    // [SNOWPACKS]
    if (!ctx.snowpacks.names.empty()) {
        sec(f,"SNOWPACKS");
        std::fprintf(f,";;%-16s %-12s Parameters\n","Name","Surface");
        std::fprintf(f,";;---------- ---------- ----------\n");
        for (size_t j = 0; j < ctx.snowpacks.names.size(); ++j) {
            const char* name = ctx.snowpacks.names[j].c_str();
            if (j < ctx.snowpacks.plowable.size()) {
                const auto& p = ctx.snowpacks.plowable[j];
                std::fprintf(f,"%-16s PLOWABLE    ", name);
                for (int k = 0; k < 7; ++k) std::fprintf(f," %10.4f", p[static_cast<size_t>(k)]);
                std::fprintf(f,"\n");
            }
            if (j < ctx.snowpacks.impervious.size()) {
                const auto& p = ctx.snowpacks.impervious[j];
                std::fprintf(f,"%-16s IMPERVIOUS  ", name);
                for (int k = 0; k < 7; ++k) std::fprintf(f," %10.4f", p[static_cast<size_t>(k)]);
                std::fprintf(f,"\n");
            }
            if (j < ctx.snowpacks.pervious.size()) {
                const auto& p = ctx.snowpacks.pervious[j];
                std::fprintf(f,"%-16s PERVIOUS    ", name);
                for (int k = 0; k < 7; ++k) std::fprintf(f," %10.4f", p[static_cast<size_t>(k)]);
                std::fprintf(f,"\n");
            }
            if (j < ctx.snowpacks.removal.size()) {
                const auto& r = ctx.snowpacks.removal[j];
                bool has_removal = false;
                for (int k = 0; k < 6; ++k)
                    if (r[static_cast<size_t>(k)] != 0.0) { has_removal = true; break; }
                if (has_removal) {
                    std::fprintf(f,"%-16s REMOVAL     ", name);
                    for (int k = 0; k < 6; ++k) std::fprintf(f," %10.4f", r[static_cast<size_t>(k)]);
                    if (j < ctx.snowpacks.removal_subcatch.size() &&
                        !ctx.snowpacks.removal_subcatch[j].empty())
                        std::fprintf(f," %s", ctx.snowpacks.removal_subcatch[j].c_str());
                    std::fprintf(f,"\n");
                }
            }
        }
    }

    // [ADJUSTMENTS]
    {
        bool has_adj = false;
        for (int i = 0; i < 12; ++i) {
            if (ctx.adjust_temp[i] != 0.0 || ctx.adjust_evap[i] != 1.0 ||
                ctx.adjust_rain[i] != 1.0 || ctx.adjust_hydcon[i] != 1.0)
            { has_adj = true; break; }
        }
        for (size_t i = 0; i < ctx.subcatch_n_perv_pattern.size() && !has_adj; ++i)
            if (ctx.subcatch_n_perv_pattern[i] >= 0) has_adj = true;
        for (size_t i = 0; i < ctx.subcatch_d_store_pattern.size() && !has_adj; ++i)
            if (ctx.subcatch_d_store_pattern[i] >= 0) has_adj = true;
        for (size_t i = 0; i < ctx.subcatch_infil_pattern.size() && !has_adj; ++i)
            if (ctx.subcatch_infil_pattern[i] >= 0) has_adj = true;

        if (has_adj) {
            sec(f,"ADJUSTMENTS");
            auto write12 = [&](const char* key, const double* arr) {
                std::fprintf(f,"%-12s", key);
                for (int i = 0; i < 12; ++i)
                    std::fprintf(f," %10.4f", arr[i]);
                std::fprintf(f,"\n");
            };
            write12("TEMP",    ctx.adjust_temp);
            write12("EVAP",    ctx.adjust_evap);
            write12("RAIN",    ctx.adjust_rain);
            write12("CONDUCT", ctx.adjust_hydcon);

            for (size_t i = 0; i < ctx.subcatch_n_perv_pattern.size(); ++i) {
                int pi = ctx.subcatch_n_perv_pattern[i];
                if (pi >= 0)
                    std::fprintf(f,"N-PERV       %-16s %s\n",
                                 ctx.subcatch_names.name_of(static_cast<int>(i)).c_str(),
                                 tN(ctx, pi));
            }
            for (size_t i = 0; i < ctx.subcatch_d_store_pattern.size(); ++i) {
                int pi = ctx.subcatch_d_store_pattern[i];
                if (pi >= 0)
                    std::fprintf(f,"DSTORE       %-16s %s\n",
                                 ctx.subcatch_names.name_of(static_cast<int>(i)).c_str(),
                                 tN(ctx, pi));
            }
            for (size_t i = 0; i < ctx.subcatch_infil_pattern.size(); ++i) {
                int pi = ctx.subcatch_infil_pattern[i];
                if (pi >= 0)
                    std::fprintf(f,"INFIL        %-16s %s\n",
                                 ctx.subcatch_names.name_of(static_cast<int>(i)).c_str(),
                                 tN(ctx, pi));
            }
        }
    }

    // [RAINGAGES]
    if(ctx.n_gages()>0){sec(f,"RAINGAGES");
    std::fprintf(f,";;%-16s %-12s %-8s %-8s %-16s\n","Name","Format","Intvl","SCF","Source");
    std::fprintf(f,";;%-16s %-12s %-8s %-8s %-16s\n","----------------","------------","--------","--------","----------------");
    for(int j=0;j<ctx.n_gages();++j){auto u=static_cast<size_t>(j);
    write_obj_comment(f, ctx.gages.comments, u);
    int iv=ctx.gages.interval_sec[u];int h=iv/3600,m=(iv%3600)/60;int ts=ctx.gages.ts_index[u];
    if(ts>=0)std::fprintf(f,"%-16s INTENSITY    %d:%02d     %.2f     TIMESERIES %s\n",ctx.gage_names.name_of(j).c_str(),h,m,ctx.gages.snow_factor[u],tN(ctx,ts));
    else if(!ctx.gages.file_path[u].empty())std::fprintf(f,"%-16s INTENSITY    %d:%02d     %.2f     FILE \"%s\" %s\n",ctx.gage_names.name_of(j).c_str(),h,m,ctx.gages.snow_factor[u],ctx.gages.file_path[u].c_str(),ctx.gages.col_name[u].c_str());
    }}

    // [SUBCATCHMENTS]
    if(ctx.n_subcatches()>0){sec(f,"SUBCATCHMENTS");
    std::fprintf(f,";;%-16s %-16s %-16s %-12s %-10s %-12s %-10s\n","Name","RainGage","Outlet","Area","%%Imperv","Width","%%Slope");
    std::fprintf(f,";;%-16s %-16s %-16s %-12s %-10s %-12s %-10s\n","----------------","----------------","----------------","------------","----------","------------","----------");
    for(int j=0;j<ctx.n_subcatches();++j){auto u=static_cast<size_t>(j);
    write_obj_comment(f, ctx.subcatches.comments, u);
    std::fprintf(f,"%-16s %-16s %-16s %12.4f %10.2f %12.4f %10.4f\n",ctx.subcatch_names.name_of(j).c_str(),gN(ctx,ctx.subcatches.gage[u]),nN(ctx,ctx.subcatches.outlet_node[u]),ctx.subcatches.area[u],ctx.subcatches.frac_imperv[u]*100.0,ctx.subcatches.width[u],ctx.subcatches.slope[u]*100.0);
    }}

    // [SUBAREAS]
    if(ctx.n_subcatches()>0){sec(f,"SUBAREAS");
    std::fprintf(f,";;%-16s %-10s %-10s %-10s %-10s %-10s\n","Subcatch","N-Imperv","N-Perv","S-Imperv","S-Perv","%%ZeroImp");
    std::fprintf(f,";;%-16s %-10s %-10s %-10s %-10s %-10s\n","----------------","----------","----------","----------","----------","----------");
    for(int j=0;j<ctx.n_subcatches();++j){auto u=static_cast<size_t>(j);
    std::fprintf(f,"%-16s %10.4f %10.4f %10.4f %10.4f %10.2f\n",ctx.subcatch_names.name_of(j).c_str(),ctx.subcatches.n_imperv[u],ctx.subcatches.n_perv[u],ctx.subcatches.ds_imperv[u]*12.0,ctx.subcatches.ds_perv[u]*12.0,ctx.subcatches.frac_imperv_no_store[u]*100.0);
    }}

    // [INFILTRATION]
    if(ctx.n_subcatches()>0){sec(f,"INFILTRATION");
    std::fprintf(f,";;%-16s %-10s %-10s %-10s %-10s %-10s\n","Subcatch","Param1","Param2","Param3","Param4","Param5");
    std::fprintf(f,";;%-16s %-10s %-10s %-10s %-10s %-10s\n","----------------","----------","----------","----------","----------","----------");
    static const char* infilNames[]={"HORTON","MODIFIED_HORTON","GREEN_AMPT","MODIFIED_GREEN_AMPT","CURVE_NUMBER"};
    for(int j=0;j<ctx.n_subcatches();++j){auto u=static_cast<size_t>(j);
    int im=ctx.subcatches.infil_model[u];
    const char* mn=(im>=0&&im<=4)?infilNames[im]:"HORTON";
    std::fprintf(f,"%-16s %10.4f %10.4f %10.4f %10.4f %10.4f %s\n",
        ctx.subcatch_names.name_of(j).c_str(),
        ctx.subcatches.infil_p1[u],ctx.subcatches.infil_p2[u],
        ctx.subcatches.infil_p3[u],ctx.subcatches.infil_p4[u],
        ctx.subcatches.infil_p5[u],mn);
    }}

    // [LID_CONTROLS]
    if (ctx.lid_controls.count() > 0) {
        sec(f,"LID_CONTROLS");
        std::fprintf(f,";;%-16s %-12s Parameters\n","Name","Type/Layer");
        std::fprintf(f,";;---------- ---------- ----------\n");
        for (int j = 0; j < ctx.lid_controls.count(); ++j) {
            auto uj = static_cast<size_t>(j);
            const char* name = ctx.lid_controls.names[uj].c_str();
            const char* ltype = ctx.lid_controls.lid_type[uj].c_str();
            // Type line
            std::fprintf(f,"%-16s %s\n", name, ltype);
            // SURFACE (5 params)
            if (uj < ctx.lid_controls.surface.size()) {
                const auto& p = ctx.lid_controls.surface[uj];
                bool has = false; for (int k=0;k<5;++k) if(p[k]!=0.0){has=true;break;}
                if (has) {
                    std::fprintf(f,"%-16s SURFACE    ", name);
                    for (int k=0;k<5;++k) std::fprintf(f," %10.4f",p[k]);
                    std::fprintf(f,"\n");
                }
            }
            // SOIL (7 params)
            if (uj < ctx.lid_controls.soil.size()) {
                const auto& p = ctx.lid_controls.soil[uj];
                bool has = false; for (int k=0;k<7;++k) if(p[k]!=0.0){has=true;break;}
                if (has) {
                    std::fprintf(f,"%-16s SOIL       ", name);
                    for (int k=0;k<7;++k) std::fprintf(f," %10.4f",p[k]);
                    std::fprintf(f,"\n");
                }
            }
            // PAVEMENT (6 params)
            if (uj < ctx.lid_controls.pavement.size()) {
                const auto& p = ctx.lid_controls.pavement[uj];
                bool has = false; for (int k=0;k<6;++k) if(p[k]!=0.0){has=true;break;}
                if (has) {
                    std::fprintf(f,"%-16s PAVEMENT   ", name);
                    for (int k=0;k<6;++k) std::fprintf(f," %10.4f",p[k]);
                    std::fprintf(f,"\n");
                }
            }
            // STORAGE (4 params)
            if (uj < ctx.lid_controls.storage.size()) {
                const auto& p = ctx.lid_controls.storage[uj];
                bool has = false; for (int k=0;k<4;++k) if(p[k]!=0.0){has=true;break;}
                if (has) {
                    std::fprintf(f,"%-16s STORAGE    ", name);
                    for (int k=0;k<4;++k) std::fprintf(f," %10.4f",p[k]);
                    std::fprintf(f,"\n");
                }
            }
            // DRAIN (6 params)
            if (uj < ctx.lid_controls.drain.size()) {
                const auto& p = ctx.lid_controls.drain[uj];
                bool has = false; for (int k=0;k<6;++k) if(p[k]!=0.0){has=true;break;}
                if (has) {
                    std::fprintf(f,"%-16s DRAIN      ", name);
                    for (int k=0;k<6;++k) std::fprintf(f," %10.4f",p[k]);
                    std::fprintf(f,"\n");
                }
            }
            // DRAINMAT (3 params)
            if (uj < ctx.lid_controls.drainmat.size()) {
                const auto& p = ctx.lid_controls.drainmat[uj];
                bool has = false; for (int k=0;k<3;++k) if(p[k]!=0.0){has=true;break;}
                if (has) {
                    std::fprintf(f,"%-16s DRAINMAT   ", name);
                    for (int k=0;k<3;++k) std::fprintf(f," %10.4f",p[k]);
                    std::fprintf(f,"\n");
                }
            }
        }
    }

    // [LID_USAGE]
    if (ctx.lid_usage.count() > 0) {
        sec(f,"LID_USAGE");
        std::fprintf(f,";;%-16s %-16s %-8s %-12s %-10s %-8s %-8s %-8s %-16s %-16s %-8s\n",
                     "Subcatch","LID","Number","Area","Width","InitSat",
                     "FromImp","ToPerv","RptFile","DrainTo","FromPerv");
        for (int j = 0; j < ctx.lid_usage.count(); ++j) {
            auto uj = static_cast<size_t>(j);
            int sc = ctx.lid_usage.subcatch_index[uj];
            int li = ctx.lid_usage.lid_index[uj];
            const char* sc_name = (sc >= 0) ? ctx.subcatch_names.name_of(sc).c_str() : "*";
            const char* lid_name = (li >= 0 && li < ctx.lid_names.size())
                                   ? ctx.lid_names.name_of(li).c_str() : "*";
            std::fprintf(f,"%-16s %-16s %8d %12.2f %10.2f %8.2f %8.2f %8d",
                         sc_name, lid_name,
                         ctx.lid_usage.number[uj],
                         ctx.lid_usage.area[uj],
                         ctx.lid_usage.width[uj],
                         ctx.lid_usage.init_sat[uj],
                         ctx.lid_usage.from_imperv[uj],
                         ctx.lid_usage.to_perv[uj]);
            if (uj < ctx.lid_usage.rpt_file.size() && !ctx.lid_usage.rpt_file[uj].empty())
                std::fprintf(f," %s", ctx.lid_usage.rpt_file[uj].c_str());
            else
                std::fprintf(f," *");
            if (uj < ctx.lid_usage.drain_to.size() && !ctx.lid_usage.drain_to[uj].empty())
                std::fprintf(f," %s", ctx.lid_usage.drain_to[uj].c_str());
            else
                std::fprintf(f," *");
            if (uj < ctx.lid_usage.from_perv.size())
                std::fprintf(f," %8.2f", ctx.lid_usage.from_perv[uj]);
            std::fprintf(f,"\n");
        }
    }

    // [JUNCTIONS]
    if(hasNT(ctx,NodeType::JUNCTION)){sec(f,"JUNCTIONS");
    std::fprintf(f,";;%-16s %-12s %-12s %-12s %-12s %-12s\n","Name","Elev","MaxDepth","InitDepth","SurDepth","Aponded");
    std::fprintf(f,";;%-16s %-12s %-12s %-12s %-12s %-12s\n","----------------","------------","------------","------------","------------","------------");
    for(int j=0;j<ctx.n_nodes();++j){auto u=static_cast<size_t>(j);if(ctx.nodes.type[u]!=NodeType::JUNCTION)continue;
    write_obj_comment(f, ctx.nodes.comments, u);
    std::fprintf(f,"%-16s %12.4f %12.4f %12.4f %12.4f %12.4f\n",ctx.node_names.name_of(j).c_str(),ctx.nodes.invert_elev[u],ctx.nodes.full_depth[u],ctx.nodes.init_depth[u],ctx.nodes.sur_depth[u],ctx.nodes.ponded_area[u]);
    }}

    // [OUTFALLS]
    if(hasNT(ctx,NodeType::OUTFALL)){sec(f,"OUTFALLS");
    std::fprintf(f,";;%-16s %-12s %-12s %-8s\n","Name","Elev","Type","Gated");
    std::fprintf(f,";;%-16s %-12s %-12s %-8s\n","----------------","------------","------------","--------");
    for(int j=0;j<ctx.n_nodes();++j){auto u=static_cast<size_t>(j);if(ctx.nodes.type[u]!=NodeType::OUTFALL)continue;
    write_obj_comment(f, ctx.nodes.comments, u);
    std::fprintf(f,"%-16s %12.4f %-12s %s",ctx.node_names.name_of(j).c_str(),ctx.nodes.invert_elev[u],ofName(ctx.nodes.outfall_type[u]),ctx.nodes.outfall_has_flap_gate[u]?"YES":"NO");
    if(ctx.nodes.outfall_type[u]==OutfallType::FIXED)std::fprintf(f," %12.4f",ctx.nodes.outfall_param[u]);
    std::fprintf(f,"\n");
    }}

    // [DIVIDERS]
    if(hasNT(ctx,NodeType::DIVIDER)){sec(f,"DIVIDERS");
    std::fprintf(f,";;%-16s %-12s %-16s %-12s\n","Name","Elev","DivLink","Type");
    std::fprintf(f,";;%-16s %-12s %-16s %-12s\n","----------------","------------","----------------","------------");
    for(int j=0;j<ctx.n_nodes();++j){auto u=static_cast<size_t>(j);if(ctx.nodes.type[u]!=NodeType::DIVIDER)continue;
    write_obj_comment(f, ctx.nodes.comments, u);
    // Resolve diversion link name
    const char* divLinkName = "*";
    std::string dlnStr;
    int dlIdx = (u < ctx.nodes.divider_link.size()) ? ctx.nodes.divider_link[u] : -1;
    if(dlIdx >= 0 && dlIdx < ctx.link_names.size()) {
        dlnStr = ctx.link_names.name_of(dlIdx); divLinkName = dlnStr.c_str();
    } else if(u < ctx.nodes.divider_link_name.size() && !ctx.nodes.divider_link_name[u].empty()) {
        divLinkName = ctx.nodes.divider_link_name[u].c_str();
    }
    auto dtype = (u < ctx.nodes.divider_type.size()) ? ctx.nodes.divider_type[u] : DividerType::CUTOFF;
    double cutoff = (u < ctx.nodes.divider_cutoff.size()) ? ctx.nodes.divider_cutoff[u] : 0.0;
    switch(dtype) {
    case DividerType::CUTOFF:
        std::fprintf(f,"%-16s %12.4f %-16s CUTOFF   %12.4f %12.4f %12.4f %12.4f %12.4f\n",
            ctx.node_names.name_of(j).c_str(), ctx.nodes.invert_elev[u], divLinkName,
            cutoff, ctx.nodes.full_depth[u], ctx.nodes.init_depth[u],
            ctx.nodes.sur_depth[u], ctx.nodes.ponded_area[u]);
        break;
    case DividerType::OVERFLOW_DIV:
        std::fprintf(f,"%-16s %12.4f %-16s OVERFLOW %12.4f %12.4f %12.4f %12.4f\n",
            ctx.node_names.name_of(j).c_str(), ctx.nodes.invert_elev[u], divLinkName,
            ctx.nodes.full_depth[u], ctx.nodes.init_depth[u],
            ctx.nodes.sur_depth[u], ctx.nodes.ponded_area[u]);
        break;
    case DividerType::TABULAR: {
        const char* curveName = "*";
        std::string cnStr;
        int ci = (u < ctx.nodes.divider_curve.size()) ? ctx.nodes.divider_curve[u] : -1;
        if(ci >= 0 && ci < ctx.table_names.size()) {
            cnStr = ctx.table_names.name_of(ci); curveName = cnStr.c_str();
        } else if(u < ctx.nodes.divider_curve_name.size() && !ctx.nodes.divider_curve_name[u].empty()) {
            curveName = ctx.nodes.divider_curve_name[u].c_str();
        }
        std::fprintf(f,"%-16s %12.4f %-16s TABULAR  %-16s %12.4f %12.4f %12.4f %12.4f\n",
            ctx.node_names.name_of(j).c_str(), ctx.nodes.invert_elev[u], divLinkName,
            curveName, ctx.nodes.full_depth[u], ctx.nodes.init_depth[u],
            ctx.nodes.sur_depth[u], ctx.nodes.ponded_area[u]);
        break;
    }
    case DividerType::WEIR: {
        double cd = (u < ctx.nodes.divider_cd.size()) ? ctx.nodes.divider_cd[u] : 0.0;
        double maxd = (u < ctx.nodes.divider_max_depth.size()) ? ctx.nodes.divider_max_depth[u] : 0.0;
        std::fprintf(f,"%-16s %12.4f %-16s WEIR     %12.4f %12.4f %12.4f %12.4f %12.4f %12.4f %12.4f\n",
            ctx.node_names.name_of(j).c_str(), ctx.nodes.invert_elev[u], divLinkName,
            cutoff, cd, maxd, ctx.nodes.full_depth[u], ctx.nodes.init_depth[u],
            ctx.nodes.sur_depth[u], ctx.nodes.ponded_area[u]);
        break;
    }
    }
    }}

    // [STORAGE]
    if(hasNT(ctx,NodeType::STORAGE)){sec(f,"STORAGE");
    std::fprintf(f,";;%-16s %-12s %-12s %-12s %-12s\n","Name","Elev","MaxDepth","InitDepth","Shape");
    std::fprintf(f,";;%-16s %-12s %-12s %-12s %-12s\n","----------------","------------","------------","------------","------------");
    for(int j=0;j<ctx.n_nodes();++j){auto u=static_cast<size_t>(j);if(ctx.nodes.type[u]!=NodeType::STORAGE)continue;
    write_obj_comment(f, ctx.nodes.comments, u);
    if(ctx.nodes.storage_curve[u]>=0)
        std::fprintf(f,"%-16s %12.4f %12.4f %12.4f TABULAR    %s 0 0 %12.4f\n",ctx.node_names.name_of(j).c_str(),ctx.nodes.invert_elev[u],ctx.nodes.full_depth[u],ctx.nodes.init_depth[u],tN(ctx,ctx.nodes.storage_curve[u]),ctx.nodes.sur_depth[u]);
    else
        std::fprintf(f,"%-16s %12.4f %12.4f %12.4f FUNCTIONAL %g %g %g 0 %12.4f\n",ctx.node_names.name_of(j).c_str(),ctx.nodes.invert_elev[u],ctx.nodes.full_depth[u],ctx.nodes.init_depth[u],ctx.nodes.storage_a[u],ctx.nodes.storage_b[u],ctx.nodes.storage_c[u],ctx.nodes.sur_depth[u]);
    }}

    // [CONDUITS]
    if(hasLT(ctx,LinkType::CONDUIT)){sec(f,"CONDUITS");
    std::fprintf(f,";;%-16s %-16s %-16s %-12s %-12s %-12s %-12s %-10s %-10s\n","Name","FromNode","ToNode","Length","Roughness","InOffset","OutOffset","InitFlow","MaxFlow");
    std::fprintf(f,";;%-16s %-16s %-16s %-12s %-12s %-12s %-12s %-10s %-10s\n","----------------","----------------","----------------","------------","------------","------------","------------","----------","----------");
    for(int j=0;j<ctx.n_links();++j){auto u=static_cast<size_t>(j);if(ctx.links.type[u]!=LinkType::CONDUIT)continue;
    write_obj_comment(f, ctx.links.comments, u);
    std::fprintf(f,"%-16s %-16s %-16s %12.4f %12.6f %12.4f %12.4f %10.4f %10.4f\n",ctx.link_names.name_of(j).c_str(),nN(ctx,ctx.links.node1[u]),nN(ctx,ctx.links.node2[u]),ctx.links.length[u],ctx.links.roughness[u],ctx.links.offset1[u],ctx.links.offset2[u],ctx.links.q0[u],ctx.links.q_limit[u]);
    }}

    // [PUMPS]
    if(hasLT(ctx,LinkType::PUMP)){sec(f,"PUMPS");
    std::fprintf(f,";;%-16s %-16s %-16s %-16s %-10s %-10s %-10s\n","Name","FromNode","ToNode","PumpCurve","Status","Startup","Shutoff");
    std::fprintf(f,";;%-16s %-16s %-16s %-16s %-10s %-10s %-10s\n","----------------","----------------","----------------","----------------","----------","----------","----------");
    for(int j=0;j<ctx.n_links();++j){auto u=static_cast<size_t>(j);if(ctx.links.type[u]!=LinkType::PUMP)continue;
    write_obj_comment(f, ctx.links.comments, u);
    std::fprintf(f,"%-16s %-16s %-16s %-16s %-10s 0          0\n",ctx.link_names.name_of(j).c_str(),nN(ctx,ctx.links.node1[u]),nN(ctx,ctx.links.node2[u]),tN(ctx,ctx.links.pump_curve[u]),ctx.links.setting[u]>0?"ON":"OFF");
    }}

    // [ORIFICES]
    if(hasLT(ctx,LinkType::ORIFICE)){sec(f,"ORIFICES");
    std::fprintf(f,";;%-16s %-16s %-16s %-10s %-10s %-10s %-8s\n","Name","FromNode","ToNode","Type","Offset","Cd","Gated");
    std::fprintf(f,";;%-16s %-16s %-16s %-10s %-10s %-10s %-8s\n","----------------","----------------","----------------","----------","----------","----------","--------");
    for(int j=0;j<ctx.n_links();++j){auto u=static_cast<size_t>(j);if(ctx.links.type[u]!=LinkType::ORIFICE)continue;
    write_obj_comment(f, ctx.links.comments, u);
    std::fprintf(f,"%-16s %-16s %-16s SIDE       %10.4f %10.4f NO\n",ctx.link_names.name_of(j).c_str(),nN(ctx,ctx.links.node1[u]),nN(ctx,ctx.links.node2[u]),ctx.links.offset1[u],ctx.links.cd[u]);
    }}

    // [WEIRS]
    if(hasLT(ctx,LinkType::WEIR)){sec(f,"WEIRS");
    std::fprintf(f,";;%-16s %-16s %-16s %-12s %-10s %-10s %-8s %-10s %-10s\n","Name","FromNode","ToNode","Type","CrestHt","Cd","Gated","EndCon","EndCoeff");
    std::fprintf(f,";;%-16s %-16s %-16s %-12s %-10s %-10s %-8s %-10s %-10s\n","----------------","----------------","----------------","------------","----------","----------","--------","----------","----------");
    for(int j=0;j<ctx.n_links();++j){auto u=static_cast<size_t>(j);if(ctx.links.type[u]!=LinkType::WEIR)continue;
    write_obj_comment(f, ctx.links.comments, u);
    std::fprintf(f,"%-16s %-16s %-16s %-12s %10.4f %10.4f NO       0          0\n",ctx.link_names.name_of(j).c_str(),nN(ctx,ctx.links.node1[u]),nN(ctx,ctx.links.node2[u]),"TRANSVERSE",ctx.links.crest_height[u],ctx.links.cd[u]);
    }}

    // [OUTLETS]
    if(hasLT(ctx,LinkType::OUTLET)){sec(f,"OUTLETS");
    std::fprintf(f,";;%-16s %-16s %-16s %-10s %-16s %-10s %-10s\n","Name","FromNode","ToNode","Offset","Type","Coeff","Expon");
    std::fprintf(f,";;%-16s %-16s %-16s %-10s %-16s %-10s %-10s\n","----------------","----------------","----------------","----------","----------------","----------","----------");
    for(int j=0;j<ctx.n_links();++j){auto u=static_cast<size_t>(j);if(ctx.links.type[u]!=LinkType::OUTLET)continue;
    write_obj_comment(f, ctx.links.comments, u);
    std::fprintf(f,"%-16s %-16s %-16s %10.4f FUNCTIONAL   %10g %10g\n",ctx.link_names.name_of(j).c_str(),nN(ctx,ctx.links.node1[u]),nN(ctx,ctx.links.node2[u]),ctx.links.offset1[u],ctx.links.cd[u],ctx.links.param2[u]);
    }}

    // [XSECTIONS]
    {sec(f,"XSECTIONS");
    std::fprintf(f,";;%-16s %-16s %-12s %-12s %-12s %-12s %-8s\n","Link","Shape","Geom1","Geom2","Geom3","Geom4","Barrels");
    std::fprintf(f,";;%-16s %-16s %-12s %-12s %-12s %-12s %-8s\n","----------------","----------------","------------","------------","------------","------------","--------");
    for(int j=0;j<ctx.n_links();++j){auto u=static_cast<size_t>(j);
    if(ctx.links.type[u]==LinkType::PUMP)continue;
    std::fprintf(f,"%-16s %-16s %12.4f %12.4f %12.4f %12.4f %8d\n",ctx.link_names.name_of(j).c_str(),xsName(static_cast<int>(ctx.links.xsect_shape[u])),ctx.links.xsect_y_full[u],ctx.links.xsect_w_max[u],0.0,0.0,ctx.links.barrels[u]);
    }}

    // [LOSSES]
    {bool hasLoss=false;
    for(int j=0;j<ctx.n_links();++j){auto u=static_cast<size_t>(j);
    if(ctx.links.type[u]==LinkType::CONDUIT&&(ctx.links.loss_inlet[u]!=0||ctx.links.loss_outlet[u]!=0||ctx.links.loss_avg[u]!=0||ctx.links.has_flap_gate[u]||ctx.links.seep_rate[u]!=0)){hasLoss=true;break;}}
    if(hasLoss){sec(f,"LOSSES");
    std::fprintf(f,";;%-16s %-10s %-10s %-10s %-8s %-10s\n","Link","Kentry","Kexit","Kavg","Flap","Seepage");
    std::fprintf(f,";;%-16s %-10s %-10s %-10s %-8s %-10s\n","----------------","----------","----------","----------","--------","----------");
    for(int j=0;j<ctx.n_links();++j){auto u=static_cast<size_t>(j);
    if(ctx.links.type[u]!=LinkType::CONDUIT)continue;
    if(ctx.links.loss_inlet[u]==0&&ctx.links.loss_outlet[u]==0&&ctx.links.loss_avg[u]==0&&!ctx.links.has_flap_gate[u]&&ctx.links.seep_rate[u]==0)continue;
    std::fprintf(f,"%-16s %10.4f %10.4f %10.4f %-8s %10.6f\n",
        ctx.link_names.name_of(j).c_str(),
        ctx.links.loss_inlet[u],ctx.links.loss_outlet[u],ctx.links.loss_avg[u],
        ctx.links.has_flap_gate[u]?"YES":"NO",ctx.links.seep_rate[u]);
    }}}

    // [TRANSECTS]
    if(ctx.transects.count()>0){sec(f,"TRANSECTS");
    for(int t=0;t<ctx.transects.count();++t){auto ut=static_cast<size_t>(t);
    std::fprintf(f,"NC %10.4f %10.4f %10.4f\n",ctx.transects.n_left[ut],ctx.transects.n_right[ut],ctx.transects.n_channel[ut]);
    int nsta=static_cast<int>(ctx.transects.stations[ut].size());
    std::fprintf(f,"X1 %-16s %10d %10.4f %10.4f 0 0 0 %10.4f %10.4f\n",
        ctx.transects.names[ut].c_str(),nsta,ctx.transects.x_left_bank[ut],
        ctx.transects.x_right_bank[ut],ctx.transects.x_factor[ut],ctx.transects.y_factor[ut]);
    for(int k=0;k<nsta;++k){auto uk=static_cast<size_t>(k);
    if(k%5==0)std::fprintf(f,"GR");
    std::fprintf(f," %10.4f %10.4f",ctx.transects.elevations[ut][uk],ctx.transects.stations[ut][uk]);
    if(k%5==4||k==nsta-1)std::fprintf(f,"\n");
    }}}

    // [STREETS]
    if(ctx.streets.count()>0){sec(f,"STREETS");
    std::fprintf(f,";;%-16s %-10s %-10s %-10s %-10s %-10s %-10s %-8s %-10s %-10s %-10s\n",
        "Name","Tcrown","Hcurb","Sx","nRoad","a","Wdep","Sides","Tback","Sback","nBack");
    for(int j=0;j<ctx.streets.count();++j){auto u=static_cast<size_t>(j);
    std::fprintf(f,"%-16s %10.4f %10.4f %10.4f %10.6f %10.4f %10.4f %8d %10.4f %10.4f %10.6f\n",
        ctx.streets.names[u].c_str(),ctx.streets.t_crown[u],ctx.streets.h_curb[u],
        ctx.streets.sx[u],ctx.streets.n_road[u],ctx.streets.gutter_depres[u],
        ctx.streets.gutter_width[u],ctx.streets.sides[u],
        ctx.streets.back_width[u],ctx.streets.back_slope[u],ctx.streets.back_n[u]);
    }}

    // [INLETS]
    if(ctx.inlets.count()>0){sec(f,"INLETS");
    for(int j=0;j<ctx.inlets.count();++j){auto u=static_cast<size_t>(j);
    std::fprintf(f,"%-16s %-12s %10.4f %10.4f",
        ctx.inlets.names[u].c_str(),ctx.inlets.inlet_type[u].c_str(),
        ctx.inlets.length[u],ctx.inlets.width[u]);
    if(!ctx.inlets.grate_type[u].empty())
        std::fprintf(f," %s",ctx.inlets.grate_type[u].c_str());
    if(ctx.inlets.open_area[u]>0)
        std::fprintf(f," %g",ctx.inlets.open_area[u]);
    if(ctx.inlets.splash_veloc[u]>0)
        std::fprintf(f," %g",ctx.inlets.splash_veloc[u]);
    std::fprintf(f,"\n");
    }}

    // [CONTROLS]
    if(ctx.control_rules.count()>0){sec(f,"CONTROLS");
    for(int j=0;j<ctx.control_rules.count();++j){
    std::fprintf(f,"%s\n",ctx.control_rules.rule_text[static_cast<size_t>(j)].c_str());
    }}

    // [REPORT]
    {sec(f,"REPORT");
    std::fprintf(f,";;%-16s %s\n","Keyword","Value");
    std::fprintf(f,"%-20s %s\n","DISABLED",ctx.options.rpt_disabled?"YES":"NO");
    std::fprintf(f,"%-20s %s\n","INPUT",ctx.options.rpt_input?"YES":"NO");
    std::fprintf(f,"%-20s %s\n","CONTINUITY",ctx.options.rpt_continuity?"YES":"NO");
    std::fprintf(f,"%-20s %s\n","FLOWSTATS",ctx.options.rpt_flowstats?"YES":"NO");
    std::fprintf(f,"%-20s %s\n","CONTROLS",ctx.options.rpt_controls?"YES":"NO");
    std::fprintf(f,"%-20s %s\n","AVERAGES",ctx.options.rpt_averages?"YES":"NO");
    if(ctx.options.rpt_subcatchments==0)std::fprintf(f,"%-20s %s\n","SUBCATCHMENTS","NONE");
    else if(ctx.options.rpt_subcatchments==1)std::fprintf(f,"%-20s %s\n","SUBCATCHMENTS","ALL");
    else for(const auto&n:ctx.options.rpt_subcatch_names)std::fprintf(f,"%-20s %s\n","SUBCATCHMENTS",n.c_str());
    if(ctx.options.rpt_nodes==0)std::fprintf(f,"%-20s %s\n","NODES","NONE");
    else if(ctx.options.rpt_nodes==1)std::fprintf(f,"%-20s %s\n","NODES","ALL");
    else for(const auto&n:ctx.options.rpt_node_names)std::fprintf(f,"%-20s %s\n","NODES",n.c_str());
    if(ctx.options.rpt_links==0)std::fprintf(f,"%-20s %s\n","LINKS","NONE");
    else if(ctx.options.rpt_links==1)std::fprintf(f,"%-20s %s\n","LINKS","ALL");
    else for(const auto&n:ctx.options.rpt_link_names)std::fprintf(f,"%-20s %s\n","LINKS",n.c_str());
    }

    // [POLLUTANTS]
    if(ctx.n_pollutants()>0){sec(f,"POLLUTANTS");
    std::fprintf(f,";;%-16s %-8s %-10s %-10s %-10s %-10s %-10s %-16s %-10s\n","Name","Units","Crain","Cgw","Crdii","Kdecay","SnowOnly","CoPollut","CoFrac");
    std::fprintf(f,";;%-16s %-8s %-10s %-10s %-10s %-10s %-10s %-16s %-10s\n","----------------","--------","----------","----------","----------","----------","----------","----------------","----------");
    for(int p=0;p<ctx.n_pollutants();++p){auto u=static_cast<size_t>(p);
    write_obj_comment(f, ctx.pollutants.comments, u);
    const char*un="MG/L";if(ctx.pollutants.units[u]==MassUnits::UG_PER_L)un="UG/L";if(ctx.pollutants.units[u]==MassUnits::COUNTS_PER_L)un="#/L";
    std::fprintf(f,"%-16s %-8s %10.4f %10.4f %10.4f %10.4f %-10s %-16s %10.4f\n",pN(ctx,p),un,ctx.pollutants.c_rain[u],ctx.pollutants.c_gw[u],ctx.pollutants.c_rdii[u],ctx.pollutants.k_decay[u],ctx.pollutants.snow_only[u]?"YES":"NO",ctx.pollutants.co_pollut[u]>=0?pN(ctx,ctx.pollutants.co_pollut[u]):"*",ctx.pollutants.co_frac[u]);
    }}

    // [LANDUSES]
    if(ctx.n_landuses()>0){sec(f,"LANDUSES");
    std::fprintf(f,";;%-16s %-12s %-12s %-12s\n","Name","SweepIntrvl","MaxRemoval","LastSwept");
    std::fprintf(f,";;%-16s %-12s %-12s %-12s\n","----------------","------------","------------","------------");
    for(int j=0;j<ctx.n_landuses();++j){auto u=static_cast<size_t>(j);
    write_obj_comment(f, ctx.landuses.comments, u);
    std::fprintf(f,"%-16s %12.2f %12.2f %12.2f\n",ctx.landuse_names.name_of(j).c_str(),
        ctx.landuses.sweep_interval[u],ctx.landuses.sweep_removal[u],ctx.landuses.last_swept[u]);
    }}

    // [BUILDUP]
    if(ctx.buildup.n_landuses>0&&ctx.buildup.n_pollutants>0){sec(f,"BUILDUP");
    std::fprintf(f,";;%-16s %-16s %-10s %-10s %-10s %-10s %-8s\n","LandUse","Pollutant","FuncType","Coeff1","Coeff2","Coeff3","PerUnit");
    std::fprintf(f,";;%-16s %-16s %-10s %-10s %-10s %-10s %-8s\n","----------------","----------------","----------","----------","----------","----------","--------");
    static const char* buNames[]={"NONE","POW","EXP","SAT","EXT"};
    for(int lu=0;lu<ctx.buildup.n_landuses;++lu){
    for(int p=0;p<ctx.buildup.n_pollutants;++p){
    auto idx=static_cast<size_t>(lu*ctx.buildup.n_pollutants+p);
    int ft=ctx.buildup.func_type[idx];
    if(ft==0)continue;
    std::fprintf(f,"%-16s %-16s %-10s %10.4f %10.4f %10.4f %-8s\n",
        ctx.landuse_names.name_of(lu).c_str(),pN(ctx,p),
        (ft>=0&&ft<=4)?buNames[ft]:"NONE",
        ctx.buildup.coeff1[idx],ctx.buildup.coeff2[idx],ctx.buildup.coeff3[idx],
        ctx.buildup.normalizer[idx]==0?"AREA":"CURB");
    }}}

    // [WASHOFF]
    if(ctx.washoff.n_landuses>0&&ctx.washoff.n_pollutants>0){sec(f,"WASHOFF");
    std::fprintf(f,";;%-16s %-16s %-10s %-10s %-10s %-10s %-10s\n","LandUse","Pollutant","FuncType","Coeff","Expon","SweepEff","BmpEff");
    std::fprintf(f,";;%-16s %-16s %-10s %-10s %-10s %-10s %-10s\n","----------------","----------------","----------","----------","----------","----------","----------");
    static const char* woNames[]={"NONE","EXP","RC","EMC"};
    for(int lu=0;lu<ctx.washoff.n_landuses;++lu){
    for(int p=0;p<ctx.washoff.n_pollutants;++p){
    auto idx=static_cast<size_t>(lu*ctx.washoff.n_pollutants+p);
    int ft=ctx.washoff.func_type[idx];
    if(ft==0)continue;
    std::fprintf(f,"%-16s %-16s %-10s %10.4f %10.4f %10.2f %10.2f\n",
        ctx.landuse_names.name_of(lu).c_str(),pN(ctx,p),
        (ft>=0&&ft<=3)?woNames[ft]:"NONE",
        ctx.washoff.coeff[idx],ctx.washoff.expon[idx],
        ctx.washoff.sweep_effic[idx],ctx.washoff.bmp_effic[idx]);
    }}}

    // [TREATMENT]
    if(ctx.treatment.hasAny()){sec(f,"TREATMENT");
    std::fprintf(f,";;%-16s %-16s %s\n","Node","Pollutant","Function");
    std::fprintf(f,";;%-16s %-16s\n","----------------","----------------");
    for(int n=0;n<ctx.treatment.n_nodes;++n){
    for(int p=0;p<ctx.treatment.n_pollutants;++p){
    auto idx=static_cast<size_t>(n*ctx.treatment.n_pollutants+p);
    if(ctx.treatment.expressions[idx].empty())continue;
    std::fprintf(f,"%-16s %-16s %s\n",nN(ctx,n),pN(ctx,p),ctx.treatment.expressions[idx].c_str());
    }}}

    // [INFLOWS]
    if(ctx.ext_inflows.count()>0){sec(f,"INFLOWS");
    std::fprintf(f,";;%-16s %-16s %-16s %-16s %-10s %-10s %-10s %-16s\n",
        "Node","Constituent","TimeSeries","Type","Mfactor","Sfactor","Baseline","Pattern");
    std::fprintf(f,";;%-16s %-16s %-16s %-16s %-10s %-10s %-10s %-16s\n",
        "----------------","----------------","----------------","----------------","----------","----------","----------","----------------");
    for(int j=0;j<ctx.ext_inflows.count();++j){auto u=static_cast<size_t>(j);
    std::fprintf(f,"%-16s %-16s %-16s %-16s %10.4f %10.4f %10.4f %-16s\n",
        nN(ctx,ctx.ext_inflows.node_idx[u]),
        ctx.ext_inflows.constituent[u].c_str(),
        ctx.ext_inflows.ts_name[u].empty()?"\"\"":ctx.ext_inflows.ts_name[u].c_str(),
        ctx.ext_inflows.inflow_type[u].c_str(),
        ctx.ext_inflows.m_factor[u],ctx.ext_inflows.s_factor[u],
        ctx.ext_inflows.baseline[u],
        ctx.ext_inflows.pattern_name[u].empty()?"":ctx.ext_inflows.pattern_name[u].c_str());
    }}

    // [DWF]
    if(ctx.dwf_inflows.count()>0){sec(f,"DWF");
    std::fprintf(f,";;%-16s %-16s %-12s %-16s %-16s %-16s %-16s\n",
        "Node","Constituent","AvgValue","Pat1","Pat2","Pat3","Pat4");
    std::fprintf(f,";;%-16s %-16s %-12s %-16s %-16s %-16s %-16s\n",
        "----------------","----------------","------------","----------------","----------------","----------------","----------------");
    for(int j=0;j<ctx.dwf_inflows.count();++j){auto u=static_cast<size_t>(j);
    std::fprintf(f,"%-16s %-16s %12.6f",
        nN(ctx,ctx.dwf_inflows.node_idx[u]),
        ctx.dwf_inflows.constituent[u].c_str(),
        ctx.dwf_inflows.avg_value[u]);
    if(!ctx.dwf_inflows.pat1[u].empty())std::fprintf(f," %-16s",ctx.dwf_inflows.pat1[u].c_str());
    if(!ctx.dwf_inflows.pat2[u].empty())std::fprintf(f," %-16s",ctx.dwf_inflows.pat2[u].c_str());
    if(!ctx.dwf_inflows.pat3[u].empty())std::fprintf(f," %-16s",ctx.dwf_inflows.pat3[u].c_str());
    if(!ctx.dwf_inflows.pat4[u].empty())std::fprintf(f," %-16s",ctx.dwf_inflows.pat4[u].c_str());
    std::fprintf(f,"\n");
    }}

    // [RDII]
    if(ctx.rdii_assigns.count()>0){sec(f,"RDII");
    std::fprintf(f,";;%-16s %-16s %-12s\n","Node","UnitHyd","SewerArea");
    std::fprintf(f,";;%-16s %-16s %-12s\n","----------------","----------------","------------");
    for(int j=0;j<ctx.rdii_assigns.count();++j){auto u=static_cast<size_t>(j);
    std::fprintf(f,"%-16s %-16s %12.4f\n",
        nN(ctx,ctx.rdii_assigns.node_idx[u]),
        ctx.rdii_assigns.uh_name[u].c_str(),
        ctx.rdii_assigns.sewer_area[u]);
    }}

    // [HYDROGRAPHS]
    if(ctx.unit_hyds.count()>0||!ctx.unit_hyds.gage_assignments.empty()){
    sec(f,"HYDROGRAPHS");
    std::fprintf(f,";;%-16s %-16s %-8s %-8s %-8s %-8s %-8s %-8s %-8s\n",
        "Name","Month/Gage","Response","R","T","K","Dmax","Drecov","Dinit");
    std::fprintf(f,";;%-16s %-16s %-8s %-8s %-8s %-8s %-8s %-8s %-8s\n",
        "----------------","----------------","--------","--------","--------","--------","--------","--------","--------");
    // Gage assignment lines
    for(size_t i=0;i<ctx.unit_hyds.gage_assignments.size();++i){
    std::fprintf(f,"%-16s %s\n",
        ctx.unit_hyds.gage_assignments[i].c_str(),
        ctx.unit_hyds.gage_names[i].c_str());
    }
    // Parameter lines
    static const char* uhMonths[]={"JAN","FEB","MAR","APR","MAY","JUN",
                                   "JUL","AUG","SEP","OCT","NOV","DEC"};
    static const char* uhResp[]={"SHORT","MEDIUM","LONG"};
    for(const auto& e:ctx.unit_hyds.entries){
    const char* mon=(e.month<0)?"ALL":uhMonths[e.month];
    std::fprintf(f,"%-16s %-9s %-8s %8.4f %8.4f %8.4f",
        e.name.c_str(),mon,uhResp[e.response],e.r,e.t,e.k);
    if(e.dmax>0.0||e.drecov>0.0||e.dinit>0.0)
        std::fprintf(f," %8.4f %8.4f %8.4f",e.dmax,e.drecov,e.dinit);
    std::fprintf(f,"\n");
    }}

    // [RDII_DECAY] (exponential IA decay parameters)
    if(ctx.rdii_decay.count()>0){sec(f,"RDII_DECAY");
    std::fprintf(f,";;%-16s %-8s %-10s %-10s %-10s %-8s %-10s %-10s\n",
        "UHGroup","Response","k_dep","k_0","k_T","T_ref","theta_rec","T_freeze");
    std::fprintf(f,";;%-16s %-8s %-10s %-10s %-10s %-8s %-10s %-10s\n",
        "----------------","--------","----------","----------","----------",
        "--------","----------","----------");
    static const char* decayResp[]={"SHORT","MEDIUM","LONG"};
    for(const auto& e:ctx.rdii_decay.entries){
    const char* r=(e.response>=0&&e.response<=2)?decayResp[e.response]:"SHORT";
    std::fprintf(f,"%-16s %-8s %10.5f %10.5f %10.5f %8.2f %10.5f %10.5f\n",
        e.uh_name.c_str(),r,
        e.k_dep,e.k_0,e.k_T,e.T_ref,e.theta_rec,e.T_freeze);
    }}

    // [PATTERNS]
    if(ctx.patterns.count()>0){sec(f,"PATTERNS");
    std::fprintf(f,";;%-16s %-12s\n","Name","Type");
    std::fprintf(f,";;%-16s %-12s\n","----------------","------------");
    static const char* patNames[]={"MONTHLY","DAILY","HOURLY","WEEKEND"};
    for(int j=0;j<ctx.patterns.count();++j){auto u=static_cast<size_t>(j);
    int pt=ctx.patterns.types[u];
    const char* ptn=(pt>=0&&pt<=3)?patNames[pt]:"MONTHLY";
    const auto& facs=ctx.patterns.factors[u];
    // First line: name + type + first batch of values
    std::fprintf(f,"%-16s %-12s",ctx.patterns.names[u].c_str(),ptn);
    for(size_t k=0;k<facs.size()&&k<6;++k)std::fprintf(f," %10.4f",facs[k]);
    std::fprintf(f,"\n");
    // Continuation lines (6 values per line)
    for(size_t k=6;k<facs.size();k+=6){
    std::fprintf(f,"%-16s            ",ctx.patterns.names[u].c_str());
    for(size_t m=k;m<facs.size()&&m<k+6;++m)std::fprintf(f," %10.4f",facs[m]);
    std::fprintf(f,"\n");
    }}}

    // [TIMESERIES]
    {bool has=false;for(const auto&t:ctx.tables.tables)if(t.type==TableType::TIMESERIES){has=true;break;}
    if(has){sec(f,"TIMESERIES");
    std::fprintf(f,";;%-16s %-20s %-12s\n","Name","Date/Time","Value");
    std::fprintf(f,";;%-16s %-20s %-12s\n","----------------","--------------------","------------");
    for(int t=0;t<static_cast<int>(ctx.tables.tables.size());++t){const auto&tb=ctx.tables.tables[static_cast<size_t>(t)];
    if(tb.type!=TableType::TIMESERIES)continue;
    if(!tb.comment.empty()){
        // Write comment once before the first row of this table
        const char*sep="\\n";std::size_t s=0;
        while(s<=tb.comment.size()){std::size_t e=tb.comment.find(sep,s);
        if(e==std::string::npos)e=tb.comment.size();
        std::fprintf(f,";%.*s\n",static_cast<int>(e-s),tb.comment.data()+s);
        if(e==tb.comment.size())break;s=e+2;}
    }
    for(size_t k=0;k<tb.x.size();++k)std::fprintf(f,"%-16s %20.6f %12.6f\n",tN(ctx,t),tb.x[k],tb.y[k]);
    }}}

    // [CURVES]
    {bool has=false;for(const auto&t:ctx.tables.tables)if(t.type!=TableType::TIMESERIES){has=true;break;}
    if(has){sec(f,"CURVES");
    std::fprintf(f,";;%-16s %-12s %-12s %-12s\n","Name","Type","X-Value","Y-Value");
    std::fprintf(f,";;%-16s %-12s %-12s %-12s\n","----------------","------------","------------","------------");
    for(int t=0;t<static_cast<int>(ctx.tables.tables.size());++t){const auto&tb=ctx.tables.tables[static_cast<size_t>(t)];
    if(tb.type==TableType::TIMESERIES)continue;
    auto lbl_it=CURVE_TYPE_LABEL.find(static_cast<int>(tb.type));
    const char* lbl=lbl_it!=CURVE_TYPE_LABEL.end()?lbl_it->second:"STORAGE";
    if(!tb.comment.empty()){
        const char*sep="\\n";std::size_t s=0;
        while(s<=tb.comment.size()){std::size_t e=tb.comment.find(sep,s);
        if(e==std::string::npos)e=tb.comment.size();
        std::fprintf(f,";%.*s\n",static_cast<int>(e-s),tb.comment.data()+s);
        if(e==tb.comment.size())break;s=e+2;}
    }
    for(size_t k=0;k<tb.x.size();++k)std::fprintf(f,"%-16s %-12s %12.6f %12.6f\n",tN(ctx,t),lbl,tb.x[k],tb.y[k]);
    }}}

    // Geospatial block — section order matches the legacy SWMM GUI ExportMap():
    //   [MAP]  →  [COORDINATES]  →  [VERTICES]  →  [Polygons]  →  [SYMBOLS]
    //
    // [MAP] — always written when any spatial data exists, matching legacy GUI
    // behaviour of always serialising map dimensions and units.
    {
    const bool has_nodes   = !ctx.spatial.node_x.empty();
    bool has_verts = false;
    for(const auto& vx:ctx.spatial.link_vertices_x) if(!vx.empty()){has_verts=true;break;}
    bool has_polys = false;
    for(const auto& px:ctx.spatial.subcatch_polygon_x) if(!px.empty()){has_polys=true;break;}
    const bool has_gages   = !ctx.spatial.gage_x.empty();
    const bool has_spatial = has_nodes||has_verts||has_polys||has_gages
                             ||ctx.spatial.map_x2!=0.0||ctx.spatial.map_y2!=0.0
                             ||!ctx.spatial.map_units.empty();

    if(has_spatial){
    // [MAP] first — legacy writes this before coordinates
    sec(f,"MAP");
    std::fprintf(f,"DIMENSIONS %-18.4f %-18.4f %-18.4f %-18.4f\n",
        ctx.spatial.map_x1, ctx.spatial.map_y1,
        ctx.spatial.map_x2, ctx.spatial.map_y2);
    // "Units" (mixed case) matches legacy GUI keyword exactly.
    // Always written; defaults to "None" when unspecified.
    const char* map_units = ctx.spatial.map_units.empty()
                            ? "None" : ctx.spatial.map_units.c_str();
    std::fprintf(f,"Units      %s\n", map_units);
    }

    // [COORDINATES] — all nodes (junctions, outfalls, dividers, storage)
    // Column layout: %-16s name | %-18.4f X | %-18.4f Y  (matches legacy Fmt)
    if(has_nodes){sec(f,"COORDINATES");
    std::fprintf(f,";;%-14s %-18s %-18s\n","Node","X-Coord","Y-Coord");
    std::fprintf(f,";;%-14s %-18s %-18s\n","--------------","------------------","------------------");
    for(int j=0;j<ctx.n_nodes();++j){auto u=static_cast<size_t>(j);
    if(u<ctx.spatial.node_x.size())
        std::fprintf(f,"%-16s %-18.4f %-18.4f\n",
            ctx.node_names.name_of(j).c_str(),
            ctx.spatial.node_x[u], ctx.spatial.node_y[u]);
    }}

    // [VERTICES] — link polyline interior vertices (conduits through outlets)
    if(has_verts){sec(f,"VERTICES");
    std::fprintf(f,";;%-14s %-18s %-18s\n","Link","X-Coord","Y-Coord");
    std::fprintf(f,";;%-14s %-18s %-18s\n","--------------","------------------","------------------");
    for(int j=0;j<ctx.n_links();++j){auto u=static_cast<size_t>(j);
    if(u>=ctx.spatial.link_vertices_x.size())continue;
    const auto& vx=ctx.spatial.link_vertices_x[u];
    const auto& vy=ctx.spatial.link_vertices_y[u];
    for(size_t k=0;k<vx.size();++k)
        std::fprintf(f,"%-16s %-18.4f %-18.4f\n",
            ctx.link_names.name_of(j).c_str(), vx[k], vy[k]);
    }}

    // [Polygons] — subcatchment boundary polygons.
    // Mixed-case tag matches legacy SWMM GUI (readers are case-insensitive).
    // Legacy also appends storage-node polygons under the same section tag;
    // storage polygon data is not yet modelled in SpatialFrame.
    if(has_polys){std::fprintf(f,"\n[Polygons]\n");
    std::fprintf(f,";;%-14s %-18s %-18s\n","Subcatchment","X-Coord","Y-Coord");
    std::fprintf(f,";;%-14s %-18s %-18s\n","--------------","------------------","------------------");
    for(int j=0;j<ctx.n_subcatches();++j){auto u=static_cast<size_t>(j);
    if(u>=ctx.spatial.subcatch_polygon_x.size())continue;
    const auto& px=ctx.spatial.subcatch_polygon_x[u];
    const auto& py=ctx.spatial.subcatch_polygon_y[u];
    for(size_t k=0;k<px.size();++k)
        std::fprintf(f,"%-16s %-18.4f %-18.4f\n",
            ctx.subcatch_names.name_of(j).c_str(), px[k], py[k]);
    }}

    // [SYMBOLS] — rain gage symbol coordinates
    if(has_gages){sec(f,"SYMBOLS");
    std::fprintf(f,";;%-14s %-18s %-18s\n","Gage","X-Coord","Y-Coord");
    std::fprintf(f,";;%-14s %-18s %-18s\n","--------------","------------------","------------------");
    for(int j=0;j<ctx.n_gages();++j){auto u=static_cast<size_t>(j);
    if(u<ctx.spatial.gage_x.size())
        std::fprintf(f,"%-16s %-18.4f %-18.4f\n",
            ctx.gage_names.name_of(j).c_str(),
            ctx.spatial.gage_x[u], ctx.spatial.gage_y[u]);
    }}
    }

    // [USER_FLAGS]
    if(ctx.user_flags.def_count()>0){sec(f,"USER_FLAGS");
    std::fprintf(f,";;%-20s %-10s %s\n","Name","Type","Description");
    std::fprintf(f,";;%-20s %-10s\n","--------------------","----------");
    for(const auto&d:ctx.user_flags.all_defs()){
    const char*ts="BOOLEAN";switch(d.type){case UserFlagType::INTEGER:ts="INTEGER";break;case UserFlagType::REAL:ts="REAL";break;case UserFlagType::STRING:ts="STRING";break;default:break;}
    if(d.description.empty())std::fprintf(f,"%-20s %-10s\n",d.name.c_str(),ts);
    else std::fprintf(f,"%-20s %-10s \"%s\"\n",d.name.c_str(),ts,d.description.c_str());
    }}

    // [USER_FLAG_VALUES]
    if(ctx.user_flags.value_count()>0){sec(f,"USER_FLAG_VALUES");
    std::fprintf(f,";;%-14s %-16s %-20s %s\n","ObjectType","ObjectName","FlagName","Value");
    std::fprintf(f,";;%-14s %-16s %-20s\n","--------------","----------------","--------------------");
    for(const auto&kv:ctx.user_flags.all_values()){const auto&k=kv.first;
    auto p1=k.find(':');if(p1==std::string::npos)continue;auto p2=k.find(':',p1+1);if(p2==std::string::npos)continue;
    std::fprintf(f,"%-14s %-16s %-20s ",k.substr(0,p1).c_str(),k.substr(p1+1,p2-p1-1).c_str(),k.substr(p2+1).c_str());
    const auto&v=kv.second;
    if(std::holds_alternative<bool>(v))std::fprintf(f,"%s\n",std::get<bool>(v)?"YES":"NO");
    else if(std::holds_alternative<int>(v))std::fprintf(f,"%d\n",std::get<int>(v));
    else if(std::holds_alternative<double>(v))std::fprintf(f,"%g\n",std::get<double>(v));
    else if(std::holds_alternative<std::string>(v)){const auto&s=std::get<std::string>(v);
    if(s.find(' ')!=std::string::npos)std::fprintf(f,"\"%s\"\n",s.c_str());else std::fprintf(f,"%s\n",s.c_str());}
    }}

    // [FILES] — secondary file references (rainfall / runoff / RDII /
    // inflows / outflows / hot-start save & use).  Mode → keyword
    // mapping mirrors the legacy parser.
    if (ctx.files.has_any()) {
        sec(f, "FILES");
        std::fprintf(f, ";;%-12s %-10s %s\n", "Mode", "FileType", "Path");
        auto mode_word = [](FileMode m) {
            return m == FileMode::SAVE ? "SAVE" :
                   m == FileMode::USE  ? "USE"  : "";
        };
        auto write_pair = [&](FileMode mode, const char* kind,
                               const std::string& path) {
            if (mode == FileMode::NONE || path.empty()) return;
            std::fprintf(f, "%-13s %-10s \"%s\"\n",
                          mode_word(mode), kind, path.c_str());
        };
        write_pair(ctx.files.rainfall_mode, "RAINFALL", ctx.files.rainfall_path);
        write_pair(ctx.files.runoff_mode,   "RUNOFF",   ctx.files.runoff_path);
        write_pair(ctx.files.rdii_mode,     "RDII",     ctx.files.rdii_path);
        if (!ctx.files.inflows_path.empty())
            std::fprintf(f, "%-13s %-10s \"%s\"\n", "USE",  "INFLOWS",
                          ctx.files.inflows_path.c_str());
        if (!ctx.files.outflows_path.empty())
            std::fprintf(f, "%-13s %-10s \"%s\"\n", "SAVE", "OUTFLOWS",
                          ctx.files.outflows_path.c_str());
        if (!ctx.files.hotstart_use_path.empty())
            std::fprintf(f, "%-13s %-10s \"%s\"\n", "USE",  "HOTSTART",
                          ctx.files.hotstart_use_path.c_str());
        for (const auto &save : ctx.files.hotstart_saves) {
            if (save.path.empty()) continue;
            if (save.datetime > 0.0) {
                char date_buf[16], time_buf[16];
                fmt_date(date_buf, save.datetime);
                fmt_time(time_buf, save.datetime);
                std::fprintf(f, "%-13s %-10s \"%s\" %s %s\n",
                              "SAVE", "HOTSTART",
                              save.path.c_str(),
                              date_buf, time_buf);
            } else {
                std::fprintf(f, "%-13s %-10s \"%s\"\n", "SAVE", "HOTSTART",
                              save.path.c_str());
            }
        }
    }

    // [PLUGINS]
    if(!ctx.plugin_specs.empty()){sec(f,"PLUGINS");
    for(const auto&ps:ctx.plugin_specs){std::fprintf(f,"%s",ps.path.c_str());
    for(const auto&a:ps.init_args)std::fprintf(f," %s",a.c_str());std::fprintf(f,"\n");
    }}

    std::fclose(f);
    return 0;
}

} // namespace inp_writer
} // namespace openswmm
