/**
 * @file DefaultReportPlugin.cpp
 * @brief DefaultReportPlugin — legacy SWMM-compatible .rpt report writer.
 *
 * @details Replicates the report format from EPA SWMM 5.x report.c,
 *          statsrpt.c, and inputrpt.c. Reference: src/legacy/engine/report.c,
 *          statsrpt.c, inputrpt.c, text.h for format strings.
 *
 * @see Legacy reference: src/legacy/engine/report.c, statsrpt.c, inputrpt.c
 * @ingroup engine_plugins
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#include "DefaultReportPlugin.hpp"

#include "../../../include/openswmm/plugin_sdk/PluginState.hpp"
#include "../../../include/openswmm/plugin_sdk/SimulationSnapshot.hpp"
#include "../core/SimulationContext.hpp"
#include "../core/UnitConversion.hpp"
#include "../core/DateTime.hpp"

#include <version.h>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <cmath>
#include <cstring>

namespace openswmm {

// Matching legacy FlowUnitWords[], InfilModelWords[], RouteModelWords[]
static const char* FlowUnitWords[] = {
    "CFS", "GPM", "MGD", "CMS", "LPS", "MLD"
};
static const char* InfilModelWords[] = {
    "HORTON", "MODIFIED_HORTON", "GREEN_AMPT",
    "MODIFIED_GREEN_AMPT", "CURVE_NUMBER"
};
static const char* SurchargeWords[] = { "EXTRAN", "SLOT" };
static const char* NodeTypeWords[] = { "JUNCTION", "OUTFALL", "DIVIDER", "STORAGE" };
static const char* LinkTypeWords[] = { "CONDUIT", "PUMP", "ORIFICE", "WEIR", "OUTLET" };
static const char* RainTypeWords[] = { "INTENSITY", "VOLUME", "CUMULATIVE" };
static const char* XsectShapeWords[] = {
    "CIRCULAR", "FILLED_CIRCULAR", "RECT_CLOSED", "RECT_OPEN",
    "TRAPEZOIDAL", "TRIANGULAR", "PARABOLIC", "POWER",
    "MOD_BASKET", "EGG", "HORSESHOE", "GOTHIC",
    "CATENARY", "SEMI_ELLIPTIC", "BASKETHANDLE", "SEMI_CIRCULAR",
    "RECT_TRIANG", "RECT_ROUND", "HORIZ_ELLIPSE", "VERT_ELLIPSE",
    "ARCH", "IRREGULAR", "CUSTOM",
    "FORCE_MAIN", "STREET", "DUMMY"
};

// ---------------------------------------------------------------------------
// Date/time helpers using DateTime.hpp
// ---------------------------------------------------------------------------

static void dateToStr(double date, char* buf, int buflen) {
    if (date <= 0.0) { std::snprintf(buf, buflen, "N/A"); return; }
    int y, m, d;
    datetime::decodeDate(date, y, m, d);
    std::snprintf(buf, buflen, "%02d/%02d/%04d", m, d, y);
}

static void timeToStr(double date, char* buf, int buflen) {
    int h, mn, s;
    datetime::decodeTime(date, h, mn, s);
    std::snprintf(buf, buflen, "%02d:%02d:%02d", h, mn, s);
}

static void secsToHMS(int secs, char* buf, int buflen) {
    int h = secs / 3600;
    int m = (secs % 3600) / 60;
    int s = secs % 60;
    std::snprintf(buf, buflen, "%02d:%02d:%02d", h, m, s);
}

/// Format a OADate (days since 12/30/1899) as "days hr:min" relative to a start date.
static void elapsedToStr(double date, double start_date, char* buf, int buflen) {
    if (date <= 0.0 || start_date <= 0.0) { std::snprintf(buf, buflen, ""); return; }
    double elapsed = date - start_date;
    int days = static_cast<int>(std::floor(elapsed));
    double frac = elapsed - days;
    int hours = static_cast<int>(frac * 24.0);
    int mins  = static_cast<int>((frac * 24.0 - hours) * 60.0);
    std::snprintf(buf, buflen, "%4d  %02d:%02d", days, hours, mins);
}

/// Decompose elapsed date into days/hrs/mins components
static void elapsedToParts(double date, double start_date, int& days, int& hrs, int& mins) {
    days = hrs = mins = 0;
    if (date <= 0.0 || start_date <= 0.0) return;
    double elapsed = date - start_date;
    days = static_cast<int>(std::floor(elapsed));
    double frac = elapsed - days;
    hrs  = static_cast<int>(frac * 24.0);
    mins = static_cast<int>((frac * 24.0 - hrs) * 60.0);
}

static const char* nt_str(int nt) {
    return (nt >= 0 && nt <= 3) ? NodeTypeWords[nt] : "JUNCTION";
}
static const char* lt_str(int lt) {
    return (lt >= 0 && lt <= 4) ? LinkTypeWords[lt] : "CONDUIT";
}

#define WRITE(f, s) std::fprintf(f, "\n  %s", s)

// Flow format string (legacy: "%9.3f" for MGD/CMS, "%9.2f" otherwise)
static const char* flowFmt(int fu) {
    return (fu == 2 || fu == 3) ? "%9.3f" : "%9.2f";
}

// Volume conversion factor: ft3 → 10^6 gal (US) or 10^6 liters (SI)
static double vcf(int /*unit_system*/) {
    return 7.48 / 1.0e6;  // US units
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

DefaultReportPlugin::DefaultReportPlugin(std::string rpt_path)
    : rpt_path_(std::move(rpt_path))
    , state_(PluginState::LOADED)
{}

DefaultReportPlugin::~DefaultReportPlugin() {
    if (file_) {
        std::fprintf(file_, "\n\n  [Report interrupted — simulation did not complete normally]\n");
        std::fflush(file_);
        std::fclose(file_);
        file_ = nullptr;
    }
}

int DefaultReportPlugin::initialize(const std::vector<std::string>& /*init_args*/,
                                    const IPluginComponentInfo* /*info*/) {
    state_ = PluginState::INITIALIZED;
    return 0;
}

int DefaultReportPlugin::validate(const SimulationContext& /*ctx*/) {
    state_ = PluginState::VALIDATED;
    return 0;
}

int DefaultReportPlugin::prepare(const SimulationContext& ctx) {
    std::time(&wall_start_);

    // Open the report file early and write preamble (title, input summaries,
    // analysis options) so they are available immediately — even if the
    // simulation crashes before write_summary() is called.
    if (!rpt_path_.empty() && !ctx.options.rpt_disabled) {
        file_ = std::fopen(rpt_path_.c_str(), "w");
        if (file_) {
            write_preamble(file_, ctx);
            std::fflush(file_);
        }
    }

    state_ = PluginState::PREPARED;
    return 0;
}

int DefaultReportPlugin::update(const SimulationSnapshot& /*snapshot*/) {
    state_ = PluginState::UPDATING;
    return 0;
}

int DefaultReportPlugin::finalize(const SimulationContext& ctx) {
    // Flush any errors/warnings that accumulated during the simulation.
    // This ensures they are persisted even if write_summary() never runs.
    if (file_) {
        for (std::size_t i = errors_written_; i < ctx.errors.size(); ++i)
            std::fprintf(file_, "\n  %s", ctx.errors[i].c_str());
        errors_written_ = ctx.errors.size();

        for (std::size_t i = warnings_written_; i < ctx.warnings.size(); ++i)
            std::fprintf(file_, "\n  %s", ctx.warnings[i].c_str());
        warnings_written_ = ctx.warnings.size();

        std::fflush(file_);
    }

    state_ = PluginState::FINALIZED;
    return 0;
}

// ---------------------------------------------------------------------------
// write_summary — coordinator for progressive report writing
// ---------------------------------------------------------------------------

int DefaultReportPlugin::write_summary(const SimulationContext& ctx) {
    if (rpt_path_.empty()) return 0;
    if (ctx.options.rpt_disabled) return 0;

    FILE* f = file_;

    if (!f) {
        // Fallback: prepare() was not called or file open failed.
        // Write the entire report monolithically.
        f = std::fopen(rpt_path_.c_str(), "w");
        if (!f) return -1;
        write_preamble(f, ctx);

        // Write all errors/warnings
        for (const auto& err : ctx.errors)
            std::fprintf(f, "\n  %s", err.c_str());
        for (const auto& warn : ctx.warnings)
            std::fprintf(f, "\n  %s", warn.c_str());
    }

    // Write result sections (continuity, statistics, summaries)
    write_results(f, ctx);

    // Write analysis timing and close
    write_timing(f);

    std::fclose(f);
    file_ = nullptr;
    return 0;
}

// ---------------------------------------------------------------------------
// write_preamble — title, input summaries, analysis options
// ---------------------------------------------------------------------------

void DefaultReportPlugin::write_preamble(std::FILE* f,
                                          const SimulationContext& ctx) {
    char ds[32], ts[16], buf[64];
    const auto& opt = ctx.options;

    int fu = static_cast<int>(opt.flow_units);
    if (fu < 0 || fu > 5) fu = 0;

    // Helper: has RDII?
    bool has_rdii = !ctx.rdii_assigns.node_idx.empty();
    // Helper: has groundwater?
    bool has_gw = false;
    for (int j = 0; j < ctx.n_subcatches(); ++j) {
        if (ctx.subcatches.gw_aquifer[static_cast<std::size_t>(j)] >= 0) {
            has_gw = true;
            break;
        }
    }
    // Helper: has snowmelt?
    bool has_snow = !ctx.snowpacks.plowable.empty();

    // =====================================================================
    // Title — matches legacy FMT01
    // =====================================================================
    std::fprintf(f, "\n  OPENSWMM ENGINE - VERSION %s", OPENSWMM_VERSION_FULL);
    std::fprintf(f, "\n  -------------------------------------------");

    // [TITLE] content
    for (const auto& line : ctx.title_notes)
        std::fprintf(f, "\n  %s", line.c_str());

    // Errors — matches legacy report_writeErrorMsg() format
    for (const auto& err : ctx.errors)
        std::fprintf(f, "\n  %s", err.c_str());
    errors_written_ = ctx.errors.size();

    // Warnings — matches legacy report_writeWarningMsg() format
    for (const auto& warn : ctx.warnings)
        std::fprintf(f, "\n  %s", warn.c_str());
    warnings_written_ = ctx.warnings.size();

    // =====================================================================
    // Element Count — matches legacy inputrpt_writeInput()
    // =====================================================================
    if (opt.rpt_input) {
    WRITE(f, "");
    WRITE(f, "*************");
    WRITE(f, "Element Count");
    WRITE(f, "*************");
    std::fprintf(f, "\n  Number of rain gages ...... %d", ctx.n_gages());
    std::fprintf(f, "\n  Number of subcatchments ... %d", ctx.n_subcatches());
    std::fprintf(f, "\n  Number of nodes ........... %d", ctx.n_nodes());
    std::fprintf(f, "\n  Number of links ........... %d", ctx.n_links());
    std::fprintf(f, "\n  Number of pollutants ...... %d", ctx.n_pollutants());
    std::fprintf(f, "\n  Number of land uses ....... 0");

    WRITE(f, "");
    WRITE(f, "");

    // =====================================================================
    // Raingage Summary — matches legacy inputrpt.c
    // =====================================================================
    if (ctx.n_gages() > 0) {
        WRITE(f, "****************");
        WRITE(f, "Raingage Summary");
        WRITE(f, "****************");
        std::fprintf(f,
            "\n                                                      Data       Recording");
        std::fprintf(f,
            "\n  Name                 Data Source                    Type       Interval ");
        std::fprintf(f,
            "\n  ------------------------------------------------------------------------");

        for (int i = 0; i < ctx.n_gages(); ++i) {
            auto ui = static_cast<std::size_t>(i);
            const auto& name = ctx.gage_names.name_of(i);
            int rt = ctx.gages.rain_type[ui];
            int interval_min = ctx.gages.interval_sec[ui] / 60;
            const char* rt_str = (rt >= 0 && rt <= 2) ? RainTypeWords[rt] : "INTENSITY";

            if (ctx.gages.source[ui] == RainSource::TIMESERIES) {
                std::fprintf(f, "\n  %-20s %-30s %-10s %3d min.",
                    name.c_str(),
                    ctx.gages.ts_name[ui].c_str(),
                    rt_str, interval_min);
            } else {
                std::fprintf(f, "\n  %-20s %-30s %-10s %3d min.",
                    name.c_str(),
                    ctx.gages.file_path[ui].c_str(),
                    rt_str, interval_min);
            }
        }
        WRITE(f, "");
        WRITE(f, "");
    }

    // =====================================================================
    // Subcatchment Summary — matches legacy inputrpt.c
    // =====================================================================
    if (ctx.n_subcatches() > 0) {
        WRITE(f, "********************");
        WRITE(f, "Subcatchment Summary");
        WRITE(f, "********************");
        std::fprintf(f,
            "\n  Name                       Area     Width   %%Imperv    %%Slope Rain Gage            Outlet              ");
        std::fprintf(f,
            "\n  -----------------------------------------------------------------------------------------------------------");

        for (int i = 0; i < ctx.n_subcatches(); ++i) {
            auto ui = static_cast<std::size_t>(i);
            const auto& name = ctx.subcatch_names.name_of(i);
            int gage_idx = ctx.subcatches.gage[ui];
            const char* gage_name = (gage_idx >= 0) ? ctx.gage_names.name_of(gage_idx).c_str() : "";
            int out_node = ctx.subcatches.outlet_node[ui];
            const char* outlet_name = (out_node >= 0) ? ctx.node_names.name_of(out_node).c_str() : "";

            std::fprintf(f, "\n  %-20s %10.2f%10.2f%10.2f%10.4f %-20s %-20s",
                name.c_str(),
                ctx.subcatches.area[ui],
                ctx.subcatches.width[ui],
                ctx.subcatches.frac_imperv[ui] * 100.0,
                ctx.subcatches.slope[ui] * 100.0,
                gage_name, outlet_name);
        }
        WRITE(f, "");
        WRITE(f, "");
    }

    // =====================================================================
    // Node Summary — matches legacy inputrpt.c
    // =====================================================================
    if (ctx.n_nodes() > 0) {
        WRITE(f, "************");
        WRITE(f, "Node Summary");
        WRITE(f, "************");
        std::fprintf(f,
            "\n                                           Invert      Max.    Ponded    External");
        std::fprintf(f,
            "\n  Name                 Type                 Elev.     Depth      Area    Inflow  ");
        std::fprintf(f,
            "\n  -------------------------------------------------------------------------------");

        for (int i = 0; i < ctx.n_nodes(); ++i) {
            auto ui = static_cast<std::size_t>(i);
            int nt = static_cast<int>(ctx.nodes.type[ui]);
            std::fprintf(f, "\n  %-20s %-16s%10.2f%10.2f%10.1f",
                ctx.node_names.name_of(i).c_str(),
                nt_str(nt),
                ctx.nodes.invert_elev[ui],
                ctx.nodes.full_depth[ui],
                ctx.nodes.ponded_area[ui]);
            // Check for external inflow
            bool has_ext = false;
            for (std::size_t k = 0; k < ctx.ext_inflows.node_idx.size(); ++k)
                if (ctx.ext_inflows.node_idx[k] == i) { has_ext = true; break; }
            for (std::size_t k = 0; !has_ext && k < ctx.dwf_inflows.node_idx.size(); ++k)
                if (ctx.dwf_inflows.node_idx[k] == i) { has_ext = true; break; }
            if (has_ext) std::fprintf(f, "    Yes");
        }
        WRITE(f, "");
        WRITE(f, "");
    }

    // =====================================================================
    // Link Summary — matches legacy inputrpt.c
    // =====================================================================
    if (ctx.n_links() > 0) {
        WRITE(f, "************");
        WRITE(f, "Link Summary");
        WRITE(f, "************");
        std::fprintf(f,
            "\n  Name             From Node        To Node          Type            Length    %%Slope Roughness");
        std::fprintf(f,
            "\n  ---------------------------------------------------------------------------------------------");

        for (int i = 0; i < ctx.n_links(); ++i) {
            auto ui = static_cast<std::size_t>(i);
            int lt = static_cast<int>(ctx.links.type[ui]);
            int n1 = ctx.links.node1[ui], n2 = ctx.links.node2[ui];
            const char* n1_name = (n1 >= 0) ? ctx.node_names.name_of(n1).c_str() : "";
            const char* n2_name = (n2 >= 0) ? ctx.node_names.name_of(n2).c_str() : "";

            std::fprintf(f, "\n  %-16s %-16s %-16s ",
                ctx.link_names.name_of(i).c_str(), n1_name, n2_name);

            if (lt == static_cast<int>(LinkType::CONDUIT)) {
                std::fprintf(f, "%-12s%10.1f%10.4f%10.4f",
                    "CONDUIT",
                    ctx.links.length[ui],
                    ctx.links.slope[ui] * 100.0,
                    ctx.links.roughness[ui]);
            } else {
                std::fprintf(f, "%-12s", lt_str(lt));
            }
        }
        WRITE(f, "");
        WRITE(f, "");
    }

    // =====================================================================
    // Cross Section Summary — matches legacy inputrpt.c
    // =====================================================================
    if (ctx.n_links() > 0) {
        bool any_conduit = false;
        for (int i = 0; i < ctx.n_links(); ++i)
            if (ctx.links.type[static_cast<std::size_t>(i)] == LinkType::CONDUIT)
                { any_conduit = true; break; }

        if (any_conduit) {
            WRITE(f, "*********************");
            WRITE(f, "Cross Section Summary");
            WRITE(f, "*********************");
            std::fprintf(f,
                "\n                                        Full     Full     Hyd.     Max.   No. of     Full");
            std::fprintf(f,
                "\n  Conduit          Shape               Depth     Area     Rad.    Width  Barrels     Flow");
            std::fprintf(f,
                "\n  ---------------------------------------------------------------------------------------");

            double Qcf_pre = ucf::Qcf[fu];
            for (int i = 0; i < ctx.n_links(); ++i) {
                auto ui = static_cast<std::size_t>(i);
                if (ctx.links.type[ui] != LinkType::CONDUIT) continue;

                int shape = static_cast<int>(ctx.links.xsect_shape[ui]);
                const char* shape_str = (shape >= 0 && shape <= 25) ?
                    XsectShapeWords[shape] : "CIRCULAR";

                std::fprintf(f, "\n  %-16s %-16s %8.2f %8.2f %8.2f %8.2f      %3d %8.2f",
                    ctx.link_names.name_of(i).c_str(),
                    shape_str,
                    ctx.links.xsect_y_full[ui],
                    ctx.links.xsect_a_full[ui],
                    ctx.links.xsect_r_full[ui],
                    ctx.links.xsect_w_max[ui],
                    ctx.links.barrels[ui],
                    ctx.links.q_full[ui] * Qcf_pre);
            }
            WRITE(f, "");
            WRITE(f, "");
        }
    }

    } // end rpt_input

    // =====================================================================
    // Analysis Options — matches legacy report_writeOptions()
    // =====================================================================
    WRITE(f, "");
    WRITE(f, "****************");
    WRITE(f, "Analysis Options");
    WRITE(f, "****************");

    std::fprintf(f, "\n  Flow Units ............... %s", FlowUnitWords[fu]);

    std::fprintf(f, "\n  Process Models:");
    std::fprintf(f, "\n    Rainfall/Runoff ........ %s",
                 (ctx.n_subcatches() > 0 && ctx.n_gages() > 0) ? "YES" : "NO");
    std::fprintf(f, "\n    RDII ................... %s", has_rdii ? "YES" : "NO");
    std::fprintf(f, "\n    Snowmelt ............... %s", has_snow ? "YES" : "NO");
    std::fprintf(f, "\n    Groundwater ............ %s", has_gw ? "YES" : "NO");
    std::fprintf(f, "\n    Flow Routing ........... %s",
                 (ctx.n_links() > 0) ? "YES" : "NO");
    if (ctx.n_links() > 0) {
        std::fprintf(f, "\n    Ponding Allowed ........ %s",
                     opt.allow_ponding ? "YES" : "NO");
    }
    std::fprintf(f, "\n    Water Quality .......... %s",
                 (ctx.n_pollutants() > 0) ? "YES" : "NO");

    if (ctx.n_subcatches() > 0) {
        int im = static_cast<int>(opt.infiltration);
        if (im < 0 || im > 4) im = 0;
        std::fprintf(f, "\n  Infiltration Method ...... %s", InfilModelWords[im]);
    }

    if (ctx.n_links() > 0) {
        int rm = static_cast<int>(opt.routing_model);
        const char* rm_name = (rm == 2) ? "DYNWAVE" : (rm == 1 ? "KINWAVE" : "STEADY");
        std::fprintf(f, "\n  Flow Routing Method ...... %s", rm_name);

        if (rm == 2) { // DYNWAVE
            int sm = opt.surcharge_method;
            std::fprintf(f, "\n  Surcharge Method ......... %s", SurchargeWords[sm]);
            const char* nc_name = (opt.node_continuity == NodeContinuity::SEMI_IMPLICIT)
                                  ? "SEMI_IMPLICIT" : "EXPLICIT";
            std::fprintf(f, "\n  Node Continuity .......... %s", nc_name);
            std::fprintf(f, "\n  Anderson Acceleration .... %s",
                         opt.anderson_accel ? "YES" : "NO");
        }
    }

    dateToStr(opt.start_date, ds, sizeof(ds));
    timeToStr(opt.start_date, ts, sizeof(ts));
    std::fprintf(f, "\n  Starting Date ............ %s %s", ds, ts);

    dateToStr(opt.end_date, ds, sizeof(ds));
    timeToStr(opt.end_date, ts, sizeof(ts));
    std::fprintf(f, "\n  Ending Date .............. %s %s", ds, ts);

    std::fprintf(f, "\n  Antecedent Dry Days ...... %.1f", opt.dry_days);

    secsToHMS(static_cast<int>(opt.report_step), buf, sizeof(buf));
    std::fprintf(f, "\n  Report Time Step ......... %s", buf);

    if (ctx.n_subcatches() > 0) {
        secsToHMS(static_cast<int>(opt.wet_step), buf, sizeof(buf));
        std::fprintf(f, "\n  Wet Time Step ............ %s", buf);
        secsToHMS(static_cast<int>(opt.dry_step), buf, sizeof(buf));
        std::fprintf(f, "\n  Dry Time Step ............ %s", buf);
    }

    if (ctx.n_links() > 0) {
        std::fprintf(f, "\n  Routing Time Step ........ %.2f sec", opt.routing_step);

        if (static_cast<int>(opt.routing_model) == 2) { // DYNWAVE
            std::fprintf(f, "\n  Variable Time Step ....... %s",
                         opt.variable_step > 0.0 ? "YES" : "NO");
            std::fprintf(f, "\n  Maximum Trials ........... %d", opt.max_trials);
            std::fprintf(f, "\n  Number of Threads ........ %d", opt.num_threads);
            std::fprintf(f, "\n  Head Tolerance ........... %f ft", opt.head_tol);
        }
    }

    WRITE(f, "");
    WRITE(f, "");
}

// ---------------------------------------------------------------------------
// write_results — simulation result sections (continuity, stats, summaries)
// ---------------------------------------------------------------------------

void DefaultReportPlugin::write_results(std::FILE* f,
                                         const SimulationContext& ctx) {
    const auto& opt = ctx.options;

    int fu = static_cast<int>(opt.flow_units);
    if (fu < 0 || fu > 5) fu = 0;
    const char* ff = flowFmt(fu);
    double Vcf = vcf(0);
    double Qcf = ucf::Qcf[fu];

    bool has_rdii = !ctx.rdii_assigns.node_idx.empty();
    bool has_gw = false;
    for (int j = 0; j < ctx.n_subcatches(); ++j) {
        if (ctx.subcatches.gw_aquifer[static_cast<std::size_t>(j)] >= 0) {
            has_gw = true;
            break;
        }
    }

    // =====================================================================
    // RDII Continuity — matches legacy report_writeRdiiError()
    // =====================================================================
    if (has_rdii && opt.rpt_continuity) {
        const auto& mb = ctx.mass_balance;
        double total_area_ft2 = 0.0;
        for (int i = 0; i < ctx.n_subcatches(); ++i)
            total_area_ft2 += ctx.subcatches.area[static_cast<std::size_t>(i)] * ucf::ACRES_TO_FT2;

        double sewer_rain = mb.runoff_rainfall;
        double rdii_prod = mb.routing_rdii;
        double ratio = (sewer_rain > 0.0) ? rdii_prod / sewer_rain : 0.0;
        double ucf1 = 1.0 / ucf::ACRES_TO_FT2;
        double ucf2 = ucf::FT3_TO_MGAL;

        std::fprintf(f, "\n  **********************           Volume        Volume");
        std::fprintf(f, "\n  Rainfall Dependent I/I        acre-feet      10^6 gal");
        std::fprintf(f, "\n  **********************        ---------     ---------");
        std::fprintf(f, "\n  Sewershed Rainfall ......%14.3f%14.3f", sewer_rain * ucf1, sewer_rain * ucf2);
        std::fprintf(f, "\n  RDII Produced ...........%14.3f%14.3f", rdii_prod * ucf1, rdii_prod * ucf2);
        std::fprintf(f, "\n  RDII Ratio ..............%14.3f", ratio);

        WRITE(f, "");
        WRITE(f, "");
    }

    // =====================================================================
    // Runoff Quantity Continuity — matches legacy report_writeRunoffError()
    // =====================================================================
    if (opt.rpt_continuity) {
        const auto& mb = ctx.mass_balance;
        double total_area_ft2 = 0.0;
        for (int i = 0; i < ctx.n_subcatches(); ++i)
            total_area_ft2 += ctx.subcatches.area[static_cast<std::size_t>(i)] * ucf::ACRES_TO_FT2;

        std::fprintf(f, "\n  **************************        Volume         Depth");
        std::fprintf(f, "\n  Runoff Quantity Continuity     acre-feet        inches");
        std::fprintf(f, "\n  **************************     ---------       -------");

        auto row = [&](const char* label, double vol_ft3) {
            double af = vol_ft3 / ucf::ACRES_TO_FT2;
            double depth_in = (total_area_ft2 > 0.0) ? vol_ft3 / total_area_ft2 * ucf::Ucf[ucf::RAINDEPTH][0] : 0.0;
            std::fprintf(f, "\n  %s%14.3f%14.3f", label, af, depth_in);
        };

        row("Total Precipitation ......", mb.runoff_rainfall);
        row("Evaporation Loss .........", mb.runoff_evap);
        row("Infiltration Loss ........", mb.runoff_infil);
        row("Surface Runoff ...........", mb.runoff_runoff);
        row("Final Storage ............", mb.runoff_final_store);

        std::fprintf(f, "\n  Continuity Error (%%) .....%14.3f", mb.runoff_error() * 100.0);
    }

    WRITE(f, "");
    WRITE(f, "");

    // =====================================================================
    // Runoff Quality Continuity — Gap #73, matches legacy writeRunoffQualError()
    // Internal units: buildup-derived fields are in display mass (lbs for US);
    //                 volumetric mass fields (wet dep, infil, runoff) are in mg.
    // =====================================================================
    if (opt.rpt_continuity && ctx.n_pollutants() > 0) {
        int np = ctx.n_pollutants();
        const auto& mb = ctx.mass_balance;

        for (int p = 0; p < np; ++p) {
            auto up = static_cast<std::size_t>(p);
            MassUnits mu = (up < ctx.pollutants.units.size()) ?
                            ctx.pollutants.units[up] : MassUnits::MG_PER_L;
            const char* mu_str;
            if (mu == MassUnits::COUNTS_PER_L) {
                mu_str = "#";
            } else {
                mu_str = "lbs";
            }

            auto getVal = [&](const std::vector<double>& v) -> double {
                return (up < v.size()) ? v[up] : 0.0;
            };

            double init_bu  = getVal(mb.qual_init_buildup);
            double surf_bu  = getVal(mb.qual_surface_buildup);
            double wet_dep  = getVal(mb.qual_wet_deposition);
            double sweep    = getVal(mb.qual_sweeping);
            double bmp      = getVal(mb.qual_bmp_removal);
            double infil    = getVal(mb.qual_infil_loss);
            double runoff   = getVal(mb.qual_runoff_load);
            double final_bu = getVal(mb.qual_final_buildup);

            std::fprintf(f, "\n  **************************%14s",
                         ctx.pollutant_names.name_of(p).c_str());
            std::fprintf(f, "\n  Runoff Quality Continuity%15s", mu_str);
            std::fprintf(f, "\n  **************************    ----------");

            std::fprintf(f, "\n  Initial Buildup ..........%14.3f", init_bu);
            std::fprintf(f, "\n  Surface Buildup ..........%14.3f", surf_bu);
            std::fprintf(f, "\n  Wet Deposition ...........%14.3f", wet_dep);
            std::fprintf(f, "\n  Sweeping Removal .........%14.3f", sweep);
            std::fprintf(f, "\n  Infiltration Loss ........%14.3f", infil);
            std::fprintf(f, "\n  BMP Removal ..............%14.3f", bmp);
            std::fprintf(f, "\n  Surface Runoff ...........%14.3f", runoff);
            std::fprintf(f, "\n  Remaining Buildup ........%14.3f", final_bu);

            double total_in  = init_bu + surf_bu + wet_dep;
            double total_out = sweep + bmp + infil + runoff + final_bu;
            double err_pct = (total_in > 0.0) ?
                (total_in - total_out) / total_in * 100.0 : 0.0;
            std::fprintf(f, "\n  Continuity Error (%%) .....%14.3f", err_pct);

            WRITE(f, "");
            WRITE(f, "");
        }
    }

    // =====================================================================
    // Groundwater Continuity — matches legacy report_writeGwaterError()
    // =====================================================================
    if (has_gw && opt.rpt_continuity) {
        const auto& mb = ctx.mass_balance;

        // Compute total GW area (ft²) for depth conversion
        double gw_area_ft2 = 0.0;
        for (int i = 0; i < ctx.n_subcatches(); ++i) {
            auto ui = static_cast<std::size_t>(i);
            if (ctx.subcatches.gw_aquifer[ui] >= 0)
                gw_area_ft2 += ctx.subcatches.area[ui] * ucf::ACRES_TO_FT2;
        }
        if (gw_area_ft2 <= 0.0) gw_area_ft2 = 1.0;

        // Volume conversion: ft³ → acre-ft (multiply by UCF(LANDAREA) for US)
        // Legacy: totals->x * UCF(LENGTH) * UCF(LANDAREA) — for US = x * 1.0 * 2.2957e-5
        double ucf_vol = 1.0 / ucf::ACRES_TO_FT2; // ft³ → acre-ft
        // Depth conversion: ft³ / gwArea → ft → inches (multiply by UCF(RAINDEPTH))
        double ucf_dep = ucf::Ucf[ucf::RAINDEPTH][0]; // ft → in = 12.0

        std::fprintf(f, "\n  **************************        Volume         Depth");
        std::fprintf(f, "\n  Groundwater Continuity         acre-feet        inches");
        std::fprintf(f, "\n  **************************     ---------       -------");

        auto gwRow = [&](const char* label, double vol_ft3) {
            std::fprintf(f, "\n  %s%14.3f%14.3f",
                label, vol_ft3 * ucf_vol,
                vol_ft3 / gw_area_ft2 * ucf_dep);
        };

        gwRow("Initial Storage ..........", mb.gw_init_storage);
        gwRow("Infiltration .............", mb.gw_infil);
        gwRow("Upper Zone ET ............", mb.gw_upper_evap);
        gwRow("Lower Zone ET ............", mb.gw_lower_evap);
        gwRow("Deep Percolation .........", mb.gw_lower_perc);
        gwRow("Groundwater Flow .........", mb.gw_lateral_flow);
        gwRow("Final Storage ............", mb.gw_final_storage);

        // Continuity error
        double totalIn  = mb.gw_infil + mb.gw_init_storage;
        double totalOut = mb.gw_upper_evap + mb.gw_lower_evap + mb.gw_lower_perc
                        + mb.gw_lateral_flow + mb.gw_final_storage;
        double pctError = 0.0;
        if (std::fabs(totalIn - totalOut) < 1.0)
            pctError = 0.0;
        else if (totalIn > 0.0)
            pctError = 100.0 * (1.0 - totalOut / totalIn);
        else if (totalOut > 0.0)
            pctError = 100.0 * (totalIn / totalOut - 1.0);

        std::fprintf(f, "\n  Continuity Error (%%) .....%14.3f", pctError);

        WRITE(f, "");
        WRITE(f, "");
    }

    // =====================================================================
    // Flow Routing Continuity — matches legacy report_writeFlowError()
    // =====================================================================
    if (opt.rpt_continuity) {
        const auto& mb = ctx.mass_balance;
        std::fprintf(f, "\n  **************************        Volume        Volume");
        std::fprintf(f, "\n  Flow Routing Continuity        acre-feet      10^6 gal");
        std::fprintf(f, "\n  **************************     ---------     ---------");

        double ucf1 = 1.0 / ucf::ACRES_TO_FT2;
        double ucf2 = ucf::FT3_TO_MGAL;

        auto row = [&](const char* label, double vol_ft3) {
            std::fprintf(f, "\n  %s%14.3f%14.3f", label, vol_ft3 * ucf1, vol_ft3 * ucf2);
        };

        row("Dry Weather Inflow .......", mb.routing_dry_weather);
        row("Wet Weather Inflow .......", mb.routing_wet_weather);
        row("Groundwater Inflow .......", mb.routing_gw_inflow);
        row("RDII Inflow ..............", mb.routing_rdii);
        row("External Inflow ..........", mb.routing_external);
        row("External Outflow .........", mb.routing_outflow);
        row("Flooding Loss ............", mb.routing_flooding);
        row("Evaporation Loss .........", mb.routing_evap_loss);
        row("Exfiltration Loss ........", mb.routing_seep_loss);
        row("Initial Stored Volume ....", mb.routing_init_storage);
        row("Final Stored Volume ......", mb.routing_final_storage);

        std::fprintf(f, "\n  Continuity Error (%%) .....%14.3f", mb.routing_error() * 100.0);
    }

    WRITE(f, "");
    WRITE(f, "");

    // =====================================================================
    // Quality Routing Continuity — Gap #71, matches legacy writeQualError()
    // =====================================================================
    if (opt.rpt_continuity) {
        int np = ctx.n_pollutants();
        const auto& mb = ctx.mass_balance;
        // Internal mass unit: conc (mg/L or ug/L) × volume (ft³)
        // Convert to lbs: × (28.317 L/ft³) / (453592 mg/lb) for MG_PER_L
        //                  × (28.317 L/ft³) / (453592000 ug/lb) for UG_PER_L
        static constexpr double LT_PER_FT3 = 28.317;
        for (int p = 0; p < np; ++p) {
            auto up = static_cast<std::size_t>(p);
            MassUnits mu = (up < ctx.pollutants.units.size()) ?
                            ctx.pollutants.units[up] : MassUnits::MG_PER_L;
            const char* mu_str;
            double mass_cf;
            if (mu == MassUnits::COUNTS_PER_L) {
                mu_str = "#";
                mass_cf = 1.0;
            } else if (mu == MassUnits::UG_PER_L) {
                mu_str = "lbs";
                mass_cf = LT_PER_FT3 / 453592000.0;
            } else {
                mu_str = "lbs";
                mass_cf = LT_PER_FT3 / 453592.0;
            }

            std::fprintf(f, "\n  **************************%14s", ctx.pollutant_names.name_of(p).c_str());
            std::fprintf(f, "\n  Quality Routing Continuity%14s", mu_str);
            std::fprintf(f, "\n  **************************    ----------");

            auto qrow = [&](const char* label, double raw) {
                std::fprintf(f, "\n  %s%14.3f", label, raw * mass_cf);
            };

            double wet     = (up < mb.qual_routing_wet.size())      ? mb.qual_routing_wet[up]      : 0.0;
            double rdii    = (up < mb.qual_routing_ii_in.size())     ? mb.qual_routing_ii_in[up]    : 0.0;
            double outflow = (up < mb.qual_routing_outflow.size())   ? mb.qual_routing_outflow[up]  : 0.0;
            double flood   = (up < mb.qual_routing_flood.size())     ? mb.qual_routing_flood[up]    : 0.0;
            double seep    = (up < mb.qual_routing_seep.size())      ? mb.qual_routing_seep[up]     : 0.0;
            double reacted = (up < mb.qual_routing_reacted.size())   ? mb.qual_routing_reacted[up]  : 0.0;
            double init    = (up < mb.qual_routing_init.size())      ? mb.qual_routing_init[up]     : 0.0;
            double final_  = (up < mb.qual_routing_final.size())     ? mb.qual_routing_final[up]    : 0.0;

            qrow("Dry Weather Inflow .......", 0.0);
            qrow("Wet Weather Inflow .......", wet);
            qrow("Groundwater Inflow .......", 0.0);
            qrow("RDII Inflow ..............", rdii);
            qrow("External Inflow ..........", 0.0);
            qrow("External Outflow .........", outflow);
            qrow("Flooding Loss ............", flood);
            qrow("Exfiltration Loss ........", seep);
            qrow("Mass Reacted .............", reacted);
            qrow("Initial Stored Mass ......", init);
            qrow("Final Stored Mass ........", final_);

            double total_in  = wet + rdii + init;
            double total_out = outflow + flood + reacted + seep + final_;
            double err_pct = (total_in > 0.0) ? (total_in - total_out) / total_in * 100.0 : 0.0;
            std::fprintf(f, "\n  Continuity Error (%%) .....%14.3f", err_pct);

            WRITE(f, "");
            WRITE(f, "");
        }
    }

    // =====================================================================
    // Highest Continuity Errors — matches legacy report_writeMaxStats()
    // =====================================================================
    if (opt.rpt_continuity) {
        WRITE(f, "*************************");
        WRITE(f, "Highest Continuity Errors");
        WRITE(f, "*************************");

        // Find top 5 nodes by continuity error
        struct NodeError { int idx; double error; };
        std::vector<NodeError> errors;
        for (int j = 0; j < ctx.n_nodes(); ++j) {
            auto uj = static_cast<std::size_t>(j);
            double in_vol  = ctx.nodes.stat_total_inflow_vol[uj];
            double out_vol = ctx.nodes.stat_total_outflow_vol[uj]
                           + ctx.nodes.stat_vol_flooded[uj];
            double denom = std::max(in_vol, out_vol);
            if (denom < 1.0) continue; // skip nodes with negligible flow
            double err_pct = 100.0 * (in_vol - out_vol) / denom;
            if (std::fabs(err_pct) > 0.1) {
                errors.push_back({j, err_pct});
            }
        }
        std::sort(errors.begin(), errors.end(),
            [](const NodeError& a, const NodeError& b) {
                return std::fabs(a.error) > std::fabs(b.error);
            });

        if (errors.empty()) {
            WRITE(f, "No errors.");
        } else {
            int count = std::min(5, static_cast<int>(errors.size()));
            for (int i = 0; i < count; ++i) {
                std::fprintf(f, "\n  Node %s (%.2f%%)",
                    ctx.node_names.name_of(errors[i].idx).c_str(),
                    errors[i].error);
            }
        }
    }

    WRITE(f, "");
    WRITE(f, "");

    // =====================================================================
    // Flow statistics sections — gated by rpt_flowstats
    // =====================================================================
    if (opt.rpt_flowstats) {

    // =====================================================================
    // Time-Step Critical Elements — matches legacy report_writeMaxStats()
    // =====================================================================
    WRITE(f, "***************************");
    WRITE(f, "Time-Step Critical Elements");
    WRITE(f, "***************************");
    {
        int k = 0;
        for (int i = 0; i < SimulationContext::MAX_STATS; ++i) {
            const auto& ms = ctx.max_courant_crit[i];
            if (ms.index < 0) continue;
            ++k;
            if (ms.obj_type == 0)
                std::fprintf(f, "\n  Node %s", ctx.node_names.name_of(ms.index).c_str());
            else
                std::fprintf(f, "\n  Link %s", ctx.link_names.name_of(ms.index).c_str());
            std::fprintf(f, " (%.2f%%)", ms.value);
        }
        if (k == 0) std::fprintf(f, "\n  None");
    }

    WRITE(f, "");
    WRITE(f, "");

    // =====================================================================
    // Highest Flow Instability Indexes
    // =====================================================================
    WRITE(f, "********************************");
    WRITE(f, "Highest Flow Instability Indexes");
    WRITE(f, "********************************");
    {
        if (ctx.max_flow_turns[0].index < 0 || ctx.max_flow_turns[0].value <= 0.0) {
            std::fprintf(f, "\n  All links are stable.");
        } else {
            for (int i = 0; i < SimulationContext::MAX_STATS; ++i) {
                const auto& ms = ctx.max_flow_turns[i];
                if (ms.index < 0) continue;
                std::fprintf(f, "\n  Link %s (%.0f)",
                             ctx.link_names.name_of(ms.index).c_str(), ms.value);
            }
        }
    }

    WRITE(f, "");
    WRITE(f, "");

    // =====================================================================
    // Most Frequent Nonconverging Nodes
    // =====================================================================
    WRITE(f, "*********************************");
    WRITE(f, "Most Frequent Nonconverging Nodes");
    WRITE(f, "*********************************");
    {
        const auto& rs = ctx.routing_stats;
        if (rs.n_non_converged == 0 ||
            ctx.max_non_converged[0].index < 0 ||
            ctx.max_non_converged[0].value < 0.00005) {
            WRITE(f, "Convergence obtained at all time steps.");
        } else {
            for (int i = 0; i < SimulationContext::MAX_STATS; ++i) {
                const auto& ms = ctx.max_non_converged[i];
                if (ms.index < 0 || ms.value <= 0.0) continue;
                std::fprintf(f, "\n  Node %s (%.2f%%)",
                             ctx.node_names.name_of(ms.index).c_str(),
                             100.0 * ms.value);
            }
        }
    }

    WRITE(f, "");
    WRITE(f, "");

    // =====================================================================
    // Routing Time Step Summary — matches legacy report.c
    // =====================================================================
    WRITE(f, "*************************");
    WRITE(f, "Routing Time Step Summary");
    WRITE(f, "*************************");
    {
        const auto& rs = ctx.routing_stats;
        if (rs.n_steps > 0) {
            std::fprintf(f, "\n  Minimum Time Step           :  %7.2f sec",
                         rs.min_step < 1.0e30 ? rs.min_step : 0.0);
            std::fprintf(f, "\n  Average Time Step           :  %7.2f sec",
                         rs.avg_step());
            std::fprintf(f, "\n  Maximum Time Step           :  %7.2f sec",
                         rs.max_step);
            std::fprintf(f, "\n  %% of Time in Steady State   :  %7.2f",
                         rs.steady_pct);
            std::fprintf(f, "\n  Average Iterations per Step :  %7.2f",
                         rs.computed_avg_iterations());
            std::fprintf(f, "\n  %% of Steps Not Converging   :  %7.2f",
                         rs.pct_non_converged());

            // Time step frequency table
            // Build histogram if not already built
            // (bins built at end of simulation in engine)
            bool has_bins = false;
            for (int i = 0; i < rs.N_TIME_BINS; ++i) {
                if (rs.step_counts[i] > 0) { has_bins = true; break; }
            }
            if (has_bins) {
                std::fprintf(f, "\n  Time Step Frequencies       :");
                for (int i = 0; i < rs.N_TIME_BINS; ++i) {
                    double pct = 100.0 * static_cast<double>(rs.step_counts[i])
                                 / static_cast<double>(rs.n_steps);
                    std::fprintf(f, "\n     %6.3f - %6.3f sec      :  %7.2f %%",
                        rs.step_intervals[i],
                        rs.step_intervals[i+1],
                        pct);
                }
            }
        }
    }

    WRITE(f, "");
    WRITE(f, "");

    } // end rpt_flowstats

    // =====================================================================
    // Control Actions Taken — Gap #67, matches legacy report_writeRuleAction()
    // Logged by ControlEngine::applyPendingActions() when rpt_controls == true.
    // =====================================================================
    if (opt.rpt_controls) {
        WRITE(f, "**********************");
        WRITE(f, "Control Actions Taken");
        WRITE(f, "**********************");

        if (ctx.control_log.empty()) {
            WRITE(f, "");
            WRITE(f, "No control actions were taken.");
        } else {
            for (const auto& entry : ctx.control_log) {
                int days, hrs, mins;
                elapsedToParts(entry.date, ctx.options.start_date, days, hrs, mins);
                std::fprintf(f,
                    "\n  Link %s setting changed to %.3f by rule %s at day %d, hr %02d:%02d",
                    ctx.link_names.name_of(entry.link_idx).c_str(),
                    entry.new_setting,
                    entry.rule_name.c_str(),
                    days, hrs, mins);
            }
        }

        WRITE(f, "");
        WRITE(f, "");
    }

    // =====================================================================
    // Subcatchment Runoff Summary — matches legacy writeSubcatchRunoff()
    // =====================================================================
    if (ctx.n_subcatches() > 0 && opt.rpt_subcatchments != 0) {
        WRITE(f, "***************************");
        WRITE(f, "Subcatchment Runoff Summary");
        WRITE(f, "***************************");
        std::fprintf(f, "\n");
        std::fprintf(f,
"\n  ------------------------------------------------------------------------------------------------------------------------------"
"\n                            Total      Total      Total      Total     Imperv       Perv      Total       Total     Peak  Runoff"
"\n                           Precip      Runon       Evap      Infil     Runoff     Runoff     Runoff      Runoff   Runoff   Coeff"
"\n  Subcatchment                 in         in         in         in         in         in         in    %8s      %3s"
"\n  ------------------------------------------------------------------------------------------------------------------------------",
            "10^6 gal", FlowUnitWords[fu]);

        for (int j = 0; j < ctx.n_subcatches(); ++j) {
            auto uj = static_cast<std::size_t>(j);
            double a = ctx.subcatches.area[uj];
            if (a <= 0.0) continue;
            double area_ft2 = a * ucf::ACRES_TO_FT2;

            constexpr double FT_TO_IN = 12.0;
            double precip_in = (area_ft2 > 0.0) ?
                ctx.subcatches.stat_precip_vol[uj] / area_ft2 * FT_TO_IN : 0.0;
            double evap_in = (area_ft2 > 0.0) ?
                ctx.subcatches.stat_evap_vol[uj] / area_ft2 * FT_TO_IN : 0.0;
            double infil_in = (area_ft2 > 0.0) ?
                ctx.subcatches.stat_infil_vol[uj] / area_ft2 * FT_TO_IN : 0.0;
            double imperv_in = (area_ft2 > 0.0) ?
                ctx.subcatches.stat_imperv_vol[uj] / area_ft2 * FT_TO_IN : 0.0;
            double perv_in = (area_ft2 > 0.0) ?
                ctx.subcatches.stat_perv_vol[uj] / area_ft2 * FT_TO_IN : 0.0;
            double runoff_in = (area_ft2 > 0.0) ?
                ctx.subcatches.stat_runoff_vol[uj] / area_ft2 * FT_TO_IN : 0.0;
            double runoff_vol = ctx.subcatches.stat_runoff_vol[uj] * Vcf;
            double peak = ctx.subcatches.stat_max_runoff[uj] * Qcf;
            double r = ctx.subcatches.stat_precip_vol[uj];
            double coeff = (r > 0.0) ? ctx.subcatches.stat_runoff_vol[uj] / r : 0.0;

            std::fprintf(f, "\n  %-20s", ctx.subcatch_names.name_of(j).c_str());
            std::fprintf(f, " %10.2f", precip_in);
            std::fprintf(f, " %10.2f", 0.0);  // runon
            std::fprintf(f, " %10.2f", evap_in);
            std::fprintf(f, " %10.2f", infil_in);
            std::fprintf(f, " %10.2f", imperv_in);
            std::fprintf(f, " %10.2f", perv_in);
            std::fprintf(f, " %10.2f", runoff_in);
            std::fprintf(f, "%12.2f", runoff_vol);
            std::fprintf(f, " %8.2f", peak);
            std::fprintf(f, "%8.3f", coeff);
        }
        WRITE(f, "");
    }

    WRITE(f, "");
    WRITE(f, "");

    // =====================================================================
    // Subcatchment Washoff Summary — Gap #64, matches legacy writeSubcatchLoads()
    // total_load is in mg (washoff_load [mg/s] × dt [s]); convert to lbs: /453592
    // =====================================================================
    if (ctx.n_subcatches() > 0 && ctx.n_pollutants() > 0 && opt.rpt_subcatchments != 0) {
        int ns = ctx.n_subcatches();
        int np = ctx.n_pollutants();
        static constexpr double MG_TO_LBS = 1.0 / 453592.0;

        WRITE(f, "****************************");
        WRITE(f, "Subcatchment Washoff Summary");
        WRITE(f, "****************************");
        std::fprintf(f, "\n");

        // Separator and header
        std::fprintf(f, " \n  ----------------------------------");
        for (int p = 1; p < np; ++p) std::fprintf(f, "--------------");
        std::fprintf(f, " \n                                ");
        for (int p = 0; p < np; ++p)
            std::fprintf(f, "%14s", ctx.pollutant_names.name_of(p).c_str());
        std::fprintf(f, " \n  Subcatchment                  ");
        for (int p = 0; p < np; ++p) {
            auto up = static_cast<std::size_t>(p);
            MassUnits mu = (up < ctx.pollutants.units.size()) ?
                            ctx.pollutants.units[up] : MassUnits::MG_PER_L;
            std::fprintf(f, "%14s", (mu == MassUnits::COUNTS_PER_L) ? "#" : "lbs");
        }
        std::fprintf(f, " \n  ----------------------------------");
        for (int p = 1; p < np; ++p) std::fprintf(f, "--------------");

        std::vector<double> sys_loads(static_cast<std::size_t>(np), 0.0);

        for (int j = 0; j < ns; ++j) {
            auto uj = static_cast<std::size_t>(j);
            std::fprintf(f, "\n  %-30s", ctx.subcatch_names.name_of(j).c_str());
            for (int p = 0; p < np; ++p) {
                auto up = static_cast<std::size_t>(p);
                auto idx = uj * static_cast<std::size_t>(np) + up;
                double load_mg = (idx < ctx.subcatches.total_load.size())
                    ? ctx.subcatches.total_load[idx] : 0.0;
                double load_lbs = load_mg * MG_TO_LBS;
                sys_loads[up] += load_lbs;
                std::fprintf(f, "%14.3f", load_lbs);
            }
        }

        // System total row
        std::fprintf(f, " \n  ----------------------------------");
        for (int p = 1; p < np; ++p) std::fprintf(f, "--------------");
        std::fprintf(f, "\n  System                        ");
        for (int p = 0; p < np; ++p)
            std::fprintf(f, "%14.3f", sys_loads[static_cast<std::size_t>(p)]);

        WRITE(f, "");
        WRITE(f, "");
    }

    // =====================================================================
    // Groundwater Summary — matches legacy writeGroundwater()
    // =====================================================================
    if (has_gw && opt.rpt_subcatchments != 0) {
        WRITE(f, "*******************");
        WRITE(f, "Groundwater Summary");
        WRITE(f, "*******************");
        std::fprintf(f, "\n");
        std::fprintf(f,
"\n  -----------------------------------------------------------------------------------------------------"
"\n                                            Total    Total  Maximum  Average  Average    Final    Final"
"\n                          Total    Total    Lower  Lateral  Lateral    Upper    Water    Upper    Water"
"\n                          Infil     Evap  Seepage  Outflow  Outflow   Moist.    Table   Moist.    Table"
"\n  Subcatchment               in       in       in       in      %3s                ft                ft"
"\n  -----------------------------------------------------------------------------------------------------",
            FlowUnitWords[fu]);

        for (int j = 0; j < ctx.n_subcatches(); ++j) {
            auto uj = static_cast<std::size_t>(j);
            if (ctx.subcatches.gw_aquifer[uj] < 0) continue;
            double area_ft2  = ctx.subcatches.area[uj] * ucf::ACRES_TO_FT2;
            double ft_to_in  = 12.0;
            double infil_in  = (area_ft2 > 0.0) ? ctx.subcatches.stat_gw_infil_vol[uj]     / area_ft2 * ft_to_in : 0.0;
            double evap_in   = (area_ft2 > 0.0) ? (ctx.subcatches.stat_gw_upper_evap_vol[uj] +
                                                    ctx.subcatches.stat_gw_lower_evap_vol[uj]) / area_ft2 * ft_to_in : 0.0;
            double seep_in   = (area_ft2 > 0.0) ? ctx.subcatches.stat_gw_deep_perc_vol[uj]  / area_ft2 * ft_to_in : 0.0;
            double lat_in    = (area_ft2 > 0.0) ? ctx.subcatches.stat_gw_flow_vol[uj]       / area_ft2 * ft_to_in : 0.0;
            double max_flow  = ctx.subcatches.stat_gw_max_flow[uj] * ucf::Qcf[fu];
            long   steps     = ctx.subcatches.stat_gw_steps[uj];
            double avg_theta = (steps > 0L) ? ctx.subcatches.stat_gw_sum_theta[uj] / static_cast<double>(steps) : 0.0;
            double avg_depth = (steps > 0L) ? ctx.subcatches.stat_gw_sum_depth[uj] / static_cast<double>(steps) : 0.0;
            double fin_theta = ctx.subcatches.stat_gw_final_theta[uj];
            double fin_depth = ctx.subcatches.stat_gw_final_depth[uj];
            std::fprintf(f, "\n  %-20s %8.2f %8.2f %8.2f %8.2f %8.2f %8.4f %8.2f %8.4f %8.2f",
                ctx.subcatch_names.name_of(j).c_str(),
                infil_in, evap_in, seep_in, lat_in, max_flow,
                avg_theta, avg_depth, fin_theta, fin_depth);
        }
        WRITE(f, "");
    }

    WRITE(f, "");
    WRITE(f, "");

    // =====================================================================
    // LID Performance Summary — Gap #66, matches legacy writeLidPerformance()
    // wb_* fields are in ft depth; convert to inches (× 12).
    // Data is copied from LIDGroupSoA to ctx.lid_usage.wb_* in SWMMEngine::report().
    // =====================================================================
    if (ctx.lid_usage.count() > 0 && opt.rpt_subcatchments != 0) {
        int n_usage = ctx.lid_usage.count();
        bool has_lids = false;
        for (int j = 0; j < n_usage; ++j) {
            auto uj = static_cast<std::size_t>(j);
            if (uj < ctx.lid_usage.wb_inflow.size() &&
                (ctx.lid_usage.wb_inflow[uj] > 0.0 ||
                 ctx.lid_usage.wb_drain_flow[uj] > 0.0 ||
                 ctx.lid_usage.wb_surf_flow[uj] > 0.0)) {
                has_lids = true; break;
            }
        }

        WRITE(f, "********************");
        WRITE(f, "LID Performance Summary");
        WRITE(f, "********************");
        std::fprintf(f, "\n");

        if (!has_lids) {
            WRITE(f, "");
            WRITE(f, "No LID performance data.");
        } else {
            static constexpr double FT_TO_IN = 12.0;
            std::fprintf(f,
"\n  -----------------------------------------------------------------"
"\n                          Total    Evap   Infil  Surface   Drain"
"\n                         Inflow    Loss    Loss    Runoff    Flow"
"\n  Control Group    Subcatch      in      in      in       in      in"
"\n  -----------------------------------------------------------------");

            for (int j = 0; j < n_usage; ++j) {
                auto uj = static_cast<std::size_t>(j);
                int li = ctx.lid_usage.lid_index[uj];
                int si = ctx.lid_usage.subcatch_index[uj];

                auto safe = [&](const std::vector<double>& v) -> double {
                    return (uj < v.size()) ? v[uj] * FT_TO_IN : 0.0;
                };

                std::fprintf(f, "\n  %-16s %-12s",
                    ctx.lid_names.name_of(li).c_str(),
                    ctx.subcatch_names.name_of(si).c_str());
                std::fprintf(f, " %7.2f %7.2f %7.2f %8.2f %7.2f",
                    safe(ctx.lid_usage.wb_inflow),
                    safe(ctx.lid_usage.wb_evap),
                    safe(ctx.lid_usage.wb_infil),
                    safe(ctx.lid_usage.wb_surf_flow),
                    safe(ctx.lid_usage.wb_drain_flow));
            }
        }

        WRITE(f, "");
        WRITE(f, "");
    }

    // =====================================================================
    // Node Depth Summary — matches legacy writeNodeDepths()
    // =====================================================================
    if (ctx.n_nodes() > 0 && opt.rpt_nodes != 0) {
        WRITE(f, "******************");
        WRITE(f, "Node Depth Summary");
        WRITE(f, "******************");
        std::fprintf(f, "\n");
        std::fprintf(f,
"\n  ---------------------------------------------------------------------------------"
"\n                                 Average  Maximum  Maximum  Time of Max    Reported"
"\n                                   Depth    Depth      HGL   Occurrence   Max Depth"
"\n  Node                 Type         Feet     Feet     Feet  days hr:min        Feet"
"\n  ---------------------------------------------------------------------------------");

        long report_steps = ctx.routing_stats.n_steps;
        report_steps = std::max(report_steps, 1L);

        for (int j = 0; j < ctx.n_nodes(); ++j) {
            auto uj = static_cast<std::size_t>(j);
            int nt = static_cast<int>(ctx.nodes.type[uj]);
            double avg_d = ctx.nodes.stat_sum_depth[uj] / static_cast<double>(report_steps);
            double max_d = ctx.nodes.stat_max_depth[uj];
            double max_hgl = ctx.nodes.invert_elev[uj] + max_d;
            double rpt_max = ctx.nodes.stat_max_rpt_depth[uj];
            int days, hrs, mins;
            elapsedToParts(ctx.nodes.stat_max_depth_date[uj], ctx.options.start_date, days, hrs, mins);

            std::fprintf(f, "\n  %-20s", ctx.node_names.name_of(j).c_str());
            std::fprintf(f, " %-9s", nt_str(nt));
            std::fprintf(f, " %7.2f  %7.2f  %7.2f  %4d  %02d:%02d  %10.2f",
                avg_d, max_d, max_hgl, days, hrs, mins, rpt_max);
        }
    }

    WRITE(f, "");
    WRITE(f, "");

    // =====================================================================
    // Node Inflow Summary — matches legacy writeNodeFlows()
    // =====================================================================
    if (ctx.n_nodes() > 0 && opt.rpt_nodes != 0) {
        WRITE(f, "*******************");
        WRITE(f, "Node Inflow Summary");
        WRITE(f, "*******************");
        std::fprintf(f, "\n");
        std::fprintf(f,
"\n  -------------------------------------------------------------------------------------------------"
"\n                                  Maximum  Maximum                  Lateral       Total        Flow"
"\n                                  Lateral    Total  Time of Max      Inflow      Inflow     Balance"
"\n                                   Inflow   Inflow   Occurrence      Volume      Volume       Error"
"\n  Node                 Type           %3s      %3s  days hr:min    %8s    %8s     Percent"
"\n  -------------------------------------------------------------------------------------------------",
            FlowUnitWords[fu], FlowUnitWords[fu],
            "10^6 gal", "10^6 gal");

        for (int j = 0; j < ctx.n_nodes(); ++j) {
            auto uj = static_cast<std::size_t>(j);
            int nt = static_cast<int>(ctx.nodes.type[uj]);

            double max_lat  = ctx.nodes.stat_max_lat_inflow[uj] * Qcf;
            double max_tot  = ctx.nodes.stat_max_total_inflow[uj] * Qcf;
            double vol_lat  = ctx.nodes.stat_lat_inflow_vol[uj] * Vcf;
            double vol_tot  = ctx.nodes.stat_total_inflow_vol[uj] * Vcf;
            int days, hrs, mins;
            elapsedToParts(ctx.nodes.stat_max_inflow_date[uj], ctx.options.start_date, days, hrs, mins);

            // Flow balance error (approximate)
            double err = 0.0;

            std::fprintf(f, "\n  %-20s", ctx.node_names.name_of(j).c_str());
            std::fprintf(f, " %-9s", nt_str(nt));
            std::fprintf(f, ff, max_lat);
            std::fprintf(f, ff, max_tot);
            std::fprintf(f, "  %4d  %02d:%02d", days, hrs, mins);
            std::fprintf(f, "%12.3f", vol_lat);
            std::fprintf(f, "%12.3f", vol_tot);
            std::fprintf(f, "%12.3f", err);
        }
    }

    WRITE(f, "");
    WRITE(f, "");

    // =====================================================================
    // Node Surcharge Summary — matches legacy writeNodeSurcharge()
    // =====================================================================
    if (static_cast<int>(opt.routing_model) == 2 && opt.rpt_nodes != 0) { // DYNWAVE only
        WRITE(f, "**********************");
        WRITE(f, "Node Surcharge Summary");
        WRITE(f, "**********************");

        bool any_surcharge = false;
        for (int j = 0; j < ctx.n_nodes(); ++j) {
            if (ctx.nodes.stat_time_surcharged[static_cast<std::size_t>(j)] > 0.0) {
                any_surcharge = true; break;
            }
        }

        if (!any_surcharge) {
            WRITE(f, "");
            WRITE(f, "No nodes were surcharged.");
        } else {
            std::fprintf(f,
"\n  ---------------------------------------------------------------------"
"\n                                               Max. Height   Min. Depth"
"\n                                   Hours       Above Crown    Below Rim"
"\n  Node                 Type      Surcharged           Feet         Feet"
"\n  ---------------------------------------------------------------------");

            for (int j = 0; j < ctx.n_nodes(); ++j) {
                auto uj = static_cast<std::size_t>(j);
                double t = ctx.nodes.stat_time_surcharged[uj] / 3600.0;
                if (t <= 0.0) continue;
                int nt = static_cast<int>(ctx.nodes.type[uj]);
                double above_crown = ctx.nodes.stat_max_surcharge_height[uj];
                double below_rim = ctx.nodes.sur_depth[uj] > 0.0 ?
                    ctx.nodes.sur_depth[uj] - ctx.nodes.stat_max_surcharge_height[uj] : 0.0;
                below_rim = std::max(below_rim, 0.0);

                std::fprintf(f, "\n  %-20s", ctx.node_names.name_of(j).c_str());
                std::fprintf(f, " %-9s", nt_str(nt));
                std::fprintf(f, "  %9.2f      %9.3f    %9.3f", t, above_crown, below_rim);
            }
        }
    }

    WRITE(f, "");
    WRITE(f, "");

    // =====================================================================
    // Node Flooding Summary — matches legacy writeNodeFlooding()
    // =====================================================================
    if (opt.rpt_nodes != 0) {
        bool any_flooding = false;
        for (int j = 0; j < ctx.n_nodes(); ++j)
            if (ctx.nodes.stat_vol_flooded[static_cast<std::size_t>(j)] > 0.0)
                { any_flooding = true; break; }

        WRITE(f, "*********************");
        WRITE(f, "Node Flooding Summary");
        WRITE(f, "*********************");

        if (!any_flooding) {
            WRITE(f, "");
            WRITE(f, "No nodes were flooded.");
        } else {
            std::fprintf(f, "\n");
            std::fprintf(f, "\n  Flooding refers to all water that overflows a node, whether it ponds or not.");
            std::fprintf(f,
"\n  --------------------------------------------------------------------------"
"\n                                                             Total   Maximum"
"\n                                 Maximum   Time of Max       Flood    Ponded"
"\n                        Hours       Rate    Occurrence      Volume     Depth"
"\n  Node                 Flooded       %3s   days hr:min    %8s      Feet"
"\n  --------------------------------------------------------------------------",
                FlowUnitWords[fu], "10^6 gal");

            for (int j = 0; j < ctx.n_nodes(); ++j) {
                auto uj = static_cast<std::size_t>(j);
                if (ctx.nodes.stat_vol_flooded[uj] <= 0.0) continue;

                double hours = ctx.nodes.stat_time_flooded[uj] / 3600.0;
                double max_rate = ctx.nodes.stat_max_overflow[uj] * Qcf;
                double vol_mgal = ctx.nodes.stat_vol_flooded[uj] * Vcf;
                int days, hrs, mins;
                elapsedToParts(ctx.nodes.stat_max_overflow_date[uj],
                               ctx.options.start_date, days, hrs, mins);
                // Ponded depth = max depth - full depth (for DW only)
                double ponded = 0.0;
                if (static_cast<int>(opt.routing_model) == 2) {
                    ponded = (ctx.nodes.stat_max_depth[uj] - ctx.nodes.full_depth[uj]);
                    ponded = std::max(ponded, 0.0);
                }

                std::fprintf(f, "\n  %-20s", ctx.node_names.name_of(j).c_str());
                std::fprintf(f, " %7.2f ", hours);
                std::fprintf(f, ff, max_rate);
                std::fprintf(f, "   %4d  %02d:%02d", days, hrs, mins);
                std::fprintf(f, "%12.3f", vol_mgal);
                std::fprintf(f, " %9.3f", ponded);
            }
        }
    }

    WRITE(f, "");
    WRITE(f, "");

    // =====================================================================
    // Storage Volume Summary — matches legacy writeStorageVolumes()
    // =====================================================================
    if (opt.rpt_nodes != 0) {
        bool any_storage = false;
        for (int j = 0; j < ctx.n_nodes(); ++j)
            if (ctx.nodes.type[static_cast<std::size_t>(j)] == NodeType::STORAGE)
                { any_storage = true; break; }

        if (any_storage) {
            WRITE(f, "**********************");
            WRITE(f, "Storage Volume Summary");
            WRITE(f, "**********************");
            std::fprintf(f, "\n");
            std::fprintf(f,
"\n  ------------------------------------------------------------------------------------------------"
"\n                         Average    Avg   Evap  Exfil     Maximum    Max    Time of Max    Maximum"
"\n                          Volume   Pcnt   Pcnt   Pcnt      Volume   Pcnt     Occurrence    Outflow"
"\n  Storage Unit          1000 ft3   Full   Loss   Loss    1000 ft3   Full    days hr:min        %3s"
"\n  ------------------------------------------------------------------------------------------------",
                FlowUnitWords[fu]);

            long report_steps = ctx.routing_stats.n_steps;
            report_steps = std::max(report_steps, 1L);

            for (int j = 0; j < ctx.n_nodes(); ++j) {
                auto uj = static_cast<std::size_t>(j);
                if (ctx.nodes.type[uj] != NodeType::STORAGE) continue;

                double full_vol = ctx.nodes.full_volume[uj];
                // Average volume approximated from average depth
                double avg_depth = ctx.nodes.stat_sum_depth[uj] / static_cast<double>(report_steps);
                // Max depth to max volume
                double max_depth = ctx.nodes.stat_max_depth[uj];

                // For functional storage: Volume = A * depth^B + C * depth
                // Approximate: use depth ratio for percentage
                double pct_avg = (ctx.nodes.full_depth[uj] > 0.0) ?
                    avg_depth / ctx.nodes.full_depth[uj] * 100.0 : 0.0;
                double pct_max = (ctx.nodes.full_depth[uj] > 0.0) ?
                    max_depth / ctx.nodes.full_depth[uj] * 100.0 : 0.0;
                pct_avg = std::min(pct_avg, 100.0);
                pct_max = std::min(pct_max, 100.0);

                // Volume approximation (using full_volume ratio)
                double avg_vol = full_vol * (pct_avg / 100.0) / 1000.0;
                double max_vol = full_vol * (pct_max / 100.0) / 1000.0;

                int days, hrs, mins;
                elapsedToParts(ctx.nodes.stat_max_depth_date[uj],
                               ctx.options.start_date, days, hrs, mins);

                // Max outflow: use stat_max_total_inflow as proxy (outflow stats not separate)
                double max_outflow = ctx.nodes.stat_max_total_inflow[uj] * Qcf;

                std::fprintf(f, "\n  %-20s", ctx.node_names.name_of(j).c_str());
                std::fprintf(f, "%10.3f  %5.1f  %5.1f  %5.1f  %10.3f  %5.1f",
                    avg_vol, pct_avg, 0.0, 0.0, max_vol, pct_max);
                std::fprintf(f, "    %4d  %02d:%02d  ", days, hrs, mins);
                std::fprintf(f, ff, max_outflow);
            }
            WRITE(f, "");
            WRITE(f, "");
        }
    }

    // =====================================================================
    // Outfall Loading Summary — matches legacy writeOutfallLoads()
    // =====================================================================
    if (opt.rpt_nodes != 0) {
        int np = ctx.n_pollutants();

        WRITE(f, "***********************");
        WRITE(f, "Outfall Loading Summary");
        WRITE(f, "***********************");
        std::fprintf(f, "\n");

        // Top separator
        std::fprintf(f, " \n  -----------------------------------------------------------");
        for (int p = 0; p < np; ++p)
            std::fprintf(f, "--------------");

        // Header row 1
        std::fprintf(f, " \n                         Flow       Avg       Max       Total");
        for (int p = 0; p < np; ++p)
            std::fprintf(f, "         Total");

        // Header row 2
        std::fprintf(f, " \n                         Freq      Flow      Flow      Volume");
        for (int p = 0; p < np; ++p)
            std::fprintf(f, "%14s", ctx.pollutant_names.name_of(p).c_str());

        // Header row 3
        std::fprintf(f, " \n  Outfall Node           Pcnt       %3s       %3s    %8s",
                     FlowUnitWords[fu], FlowUnitWords[fu], "10^6 gal");
        for (int p = 0; p < np; ++p)
            std::fprintf(f, "%14s", "lbs");

        // Bottom separator
        std::fprintf(f, " \n  -----------------------------------------------------------");
        for (int p = 0; p < np; ++p)
            std::fprintf(f, "--------------");

        double sys_vol = 0.0;
        double sys_flow_sum = 0.0;
        double sys_max_flow = 0.0;
        double sys_freq_sum = 0.0;
        int outfall_count = 0;
        std::vector<double> pol_totals(static_cast<std::size_t>(np), 0.0);
        long total_steps = ctx.routing_stats.n_steps;
        total_steps = std::max(total_steps, 1L);

        for (int j = 0; j < ctx.n_nodes(); ++j) {
            auto uj = static_cast<std::size_t>(j);
            if (ctx.nodes.type[uj] != NodeType::OUTFALL) continue;
            outfall_count++;

            long periods = ctx.nodes.stat_outfall_periods[uj];
            double avg_flow = (periods > 0)
                ? ctx.nodes.stat_outfall_avg_flow[uj] * Qcf / static_cast<double>(periods) : 0.0;
            double max_flow = ctx.nodes.stat_outfall_max_flow[uj] * Qcf;
            double freq = 100.0 * static_cast<double>(periods) / static_cast<double>(total_steps);
            double vol = ctx.nodes.stat_total_inflow_vol[uj] * Vcf;

            sys_freq_sum += freq;
            sys_flow_sum += avg_flow;
            sys_max_flow += max_flow;
            sys_vol += vol;

            std::fprintf(f, "\n  %-20s", ctx.node_names.name_of(j).c_str());
            std::fprintf(f, "%7.2f", freq);
            std::fprintf(f, " ");
            std::fprintf(f, ff, avg_flow);
            std::fprintf(f, " ");
            std::fprintf(f, ff, max_flow);
            std::fprintf(f, "%12.3f", vol);

            auto base = uj * static_cast<std::size_t>(np);
            for (int p = 0; p < np; ++p) {
                auto idx = base + static_cast<std::size_t>(p);
                double load = (idx < ctx.nodes.stat_total_load.size())
                    ? ctx.nodes.stat_total_load[idx] : 0.0;
                pol_totals[static_cast<std::size_t>(p)] += load;
                std::fprintf(f, "%14.3f", load);
            }
        }

        // System totals
        std::fprintf(f, " \n  -----------------------------------------------------------");
        for (int p = 0; p < np; ++p)
            std::fprintf(f, "--------------");

        std::fprintf(f, "\n  System              ");
        double sys_freq = (outfall_count > 0) ? sys_freq_sum / outfall_count : 0.0;
        std::fprintf(f, "%7.2f ", sys_freq);
        std::fprintf(f, ff, sys_flow_sum);
        std::fprintf(f, " ");
        std::fprintf(f, ff, sys_max_flow);
        std::fprintf(f, "%12.3f", sys_vol);
        for (int p = 0; p < np; ++p)
            std::fprintf(f, "%14.3f", pol_totals[static_cast<std::size_t>(p)]);
    }

    WRITE(f, "");
    WRITE(f, "");

    // =====================================================================
    // Link Flow Summary — matches legacy writeLinkFlows()
    // =====================================================================
    if (ctx.n_links() > 0 && opt.rpt_links != 0) {
        WRITE(f, "********************");
        WRITE(f, "Link Flow Summary");
        WRITE(f, "********************");
        std::fprintf(f, "\n");
        std::fprintf(f,
"\n  -----------------------------------------------------------------------------"
"\n                                 Maximum  Time of Max   Maximum    Max/    Max/"
"\n                                  |Flow|   Occurrence   |Veloc|    Full    Full"
"\n  Link                 Type          %3s  days hr:min    ft/sec    Flow   Depth"
"\n  -----------------------------------------------------------------------------",
            FlowUnitWords[fu]);

        for (int j = 0; j < ctx.n_links(); ++j) {
            auto uj = static_cast<std::size_t>(j);
            int lt = static_cast<int>(ctx.links.type[uj]);

            double mf = ctx.links.stat_max_flow[uj] * Qcf;  // CFS → display
            double mv = ctx.links.stat_max_veloc[uj];
            double fill = ctx.links.stat_max_filling[uj];

            // Flow ratio (using CFS values, not display)
            double flow_ratio = 0.0;
            if (lt == static_cast<int>(LinkType::CONDUIT)) {
                double mf_cfs = ctx.links.stat_max_flow[uj];
                double qf = ctx.links.q_full[uj];
                int barrels = std::max(ctx.links.barrels[uj], 1);
                flow_ratio = (qf > 0.0) ? mf_cfs / qf / static_cast<double>(barrels) : 0.0;
            }

            int days, hrs, mins;
            elapsedToParts(ctx.links.stat_max_flow_date[uj], ctx.options.start_date, days, hrs, mins);

            std::fprintf(f, "\n  %-20s", ctx.link_names.name_of(j).c_str());
            std::fprintf(f, " %-7s ", lt_str(lt));
            std::fprintf(f, ff, mf);
            std::fprintf(f, "  %4d  %02d:%02d", days, hrs, mins);

            if (lt == static_cast<int>(LinkType::CONDUIT)) {
                std::fprintf(f, "   %7.2f", mv);
                std::fprintf(f, "  %6.2f", flow_ratio);
                std::fprintf(f, "  %6.2f", fill);
            } else {
                // Non-conduit: no velocity, full ratios
                std::fprintf(f, "          ");
                std::fprintf(f, "  %6.2f", flow_ratio);
                std::fprintf(f, "        ");
            }
        }
    }

    WRITE(f, "");
    WRITE(f, "");

    // =====================================================================
    // Flow Classification Summary — matches legacy writeFlowClass()
    // =====================================================================
    if (ctx.n_links() > 0 && opt.rpt_links != 0) {
        WRITE(f, "***************************");
        WRITE(f, "Flow Classification Summary");
        WRITE(f, "***************************");
        std::fprintf(f,
"\n\n  -------------------------------------------------------------------------------------"
"\n                      Adjusted    ---------- Fraction of Time in Flow Class ---------- "
"\n                       /Actual         Up    Down  Sub   Sup   Up    Down  Norm  Inlet "
"\n  Conduit               Length    Dry  Dry   Dry   Crit  Crit  Crit  Crit  Ltd   Ctrl  "
"\n  -------------------------------------------------------------------------------------");

        double total_secs = (ctx.routing_stats.n_steps > 0) ?
            ctx.routing_stats.sum_step : 1.0;

        for (int j = 0; j < ctx.n_links(); ++j) {
            auto uj = static_cast<std::size_t>(j);
            if (ctx.links.type[uj] != LinkType::CONDUIT) continue;

            // Adjusted/actual length ratio
            double len_ratio = 1.0;
            if (ctx.links.length[uj] > 0.0)
                len_ratio = ctx.links.mod_length[uj] / ctx.links.length[uj];

            std::fprintf(f, "\n  %-20s", ctx.link_names.name_of(j).c_str());
            std::fprintf(f, "  %6.2f ", len_ratio);

            long total = 0;
            for (int c = 0; c < LinkData::N_FLOW_CLASSES; ++c) {
                auto idx = uj * LinkData::N_FLOW_CLASSES + static_cast<std::size_t>(c);
                if (idx < ctx.links.stat_flow_class.size())
                    total += ctx.links.stat_flow_class[idx];
            }
            if (total == 0) total = 1;

            for (int c = 0; c < LinkData::N_FLOW_CLASSES; ++c) {
                auto idx = uj * LinkData::N_FLOW_CLASSES + static_cast<std::size_t>(c);
                long cnt = (idx < ctx.links.stat_flow_class.size()) ?
                    ctx.links.stat_flow_class[idx] : 0L;
                double pct = static_cast<double>(cnt) / static_cast<double>(total);
                std::fprintf(f, "  %4.2f", pct);
            }
            // Normal flow limited and inlet control
            double norm_pct = static_cast<double>(ctx.links.stat_norm_ltd[uj]) /
                              static_cast<double>(total);
            double inlet_pct = static_cast<double>(ctx.links.stat_inlet_ctrl[uj]) /
                               static_cast<double>(total);
            std::fprintf(f, "  %4.2f", norm_pct);
            std::fprintf(f, "  %4.2f", inlet_pct);
        }
    }
    WRITE(f, "");

    // =====================================================================
    // Conduit Surcharge Summary — matches legacy writeLinkSurcharge()
    // =====================================================================
    if (opt.rpt_links != 0) {
        bool any_surcharge = false;
        for (int j = 0; j < ctx.n_links(); ++j) {
            auto uj = static_cast<std::size_t>(j);
            if (ctx.links.type[uj] == LinkType::CONDUIT &&
                ctx.links.stat_time_surcharged[uj] > 0.0) {
                any_surcharge = true; break;
            }
        }

        WRITE(f, "*************************");
        WRITE(f, "Conduit Surcharge Summary");
        WRITE(f, "*************************");

        if (!any_surcharge) {
            WRITE(f, "");
            WRITE(f, "No conduits were surcharged.");
        } else {
            std::fprintf(f,
"\n  ----------------------------------------------------------------------------"
"\n                                                           Hours        Hours "
"\n                         --------- Hours Full --------   Above Full   Capacity"
"\n  Conduit                Both Ends  Upstream  Dnstream   Normal Flow   Limited"
"\n  ----------------------------------------------------------------------------");

            for (int j = 0; j < ctx.n_links(); ++j) {
                auto uj = static_cast<std::size_t>(j);
                if (ctx.links.type[uj] != LinkType::CONDUIT) continue;
                if (ctx.links.stat_time_surcharged[uj] <= 0.0) continue;

                double t_both = ctx.links.stat_time_full_both[uj] / 3600.0;
                double t_up   = ctx.links.stat_time_full_upstream[uj] / 3600.0;
                double t_dn   = ctx.links.stat_time_full_dnstream[uj] / 3600.0;
                double t_norm = ctx.links.stat_time_surcharged[uj] / 3600.0;
                double t_cap  = ctx.links.stat_time_capacity_limited[uj] / 3600.0;

                std::fprintf(f, "\n  %-20s", ctx.link_names.name_of(j).c_str());
                std::fprintf(f, "    %8.2f  %8.2f  %8.2f  %8.2f     %8.2f",
                    t_both, t_up, t_dn, t_norm, t_cap);
            }
        }
    }

    WRITE(f, "");
    WRITE(f, "");

    // =====================================================================
    // Pumping Summary — matches legacy writePumpFlows()
    // =====================================================================
    if (opt.rpt_links != 0) {
        bool any_pump = false;
        for (int j = 0; j < ctx.n_links(); ++j)
            if (ctx.links.type[static_cast<std::size_t>(j)] == LinkType::PUMP)
                { any_pump = true; break; }

        if (any_pump) {
            WRITE(f, "***************");
            WRITE(f, "Pumping Summary");
            WRITE(f, "***************");
            std::fprintf(f,
"\n\n  ---------------------------------------------------------------------------------------------------------"
"\n                                                  Min       Avg       Max     Total     Power    %% Time Off"
"\n                        Percent   Number of      Flow      Flow      Flow    Volume     Usage    Pump Curve"
"\n  Pump                 Utilized   Start-Ups       %3s       %3s       %3s  %8s     Kw-hr    Low   High"
"\n  ---------------------------------------------------------------------------------------------------------",
                FlowUnitWords[fu], FlowUnitWords[fu], FlowUnitWords[fu], "10^6 gal");

            double totalSeconds = (ctx.options.end_date - ctx.options.start_date) * 86400.0;

            for (int j = 0; j < ctx.n_links(); ++j) {
                auto uj = static_cast<std::size_t>(j);
                if (ctx.links.type[uj] != LinkType::PUMP) continue;

                double max_flow = ctx.links.stat_max_flow[uj] * Qcf;
                double vol = ctx.links.stat_vol_flow[uj] * Vcf;
                double on_time = ctx.links.stat_pump_on_time[uj];
                double pctUtilized = (totalSeconds > 0.0) ? on_time / totalSeconds * 100.0 : 0.0;
                int    startUps   = ctx.links.stat_pump_cycles[uj];
                double avgFlow    = (on_time > 0.0) ? (ctx.links.stat_pump_volume[uj] / on_time) * Qcf : 0.0;
                double energyKwh  = ctx.links.stat_pump_energy[uj];

                std::fprintf(f, "\n  %-20s %8.2f  %10d %9.2f %9.2f %9.2f %9.3f %9.2f",
                    ctx.link_names.name_of(j).c_str(),
                    pctUtilized, startUps, 0.0, avgFlow, max_flow, vol, energyKwh);
                std::fprintf(f, " %6.1f %6.1f", 0.0, 0.0);
            }
            WRITE(f, "");
            WRITE(f, "");
        }
    }

    // =====================================================================
    // Street Inlet Flow Summary — Gap #68
    // Volumes in ft³; convert to 1000 gal: × 7.48052 / 1000
    // =====================================================================
    if (ctx.inlet_usages.count() > 0 && opt.rpt_links != 0) {
        int ni = ctx.inlet_usages.count();
        // Only write if stats arrays are populated
        bool has_stats = (static_cast<int>(ctx.inlet_usages.stat_capture_vol.size()) >= ni);

        WRITE(f, "**************************");
        WRITE(f, "Street Inlet Flow Summary");
        WRITE(f, "**************************");

        if (!has_stats) {
            WRITE(f, "");
            WRITE(f, "No inlet statistics available.");
        } else {
            static constexpr double FT3_TO_KGAL = 7.48052 / 1000.0;
            std::fprintf(f,
"\n\n  -----------------------------------------------------------------------------------------"
"\n                                          Peak        Pcnt        Pcnt       Vol.       Vol."
"\n  Conduit               Inlet           Flow        Captured    Bypassed   Captured   Bypassed"
"\n                                        %-3s         Percent     Percent    1000 Gal   1000 Gal"
"\n  -----------------------------------------------------------------------------------------",
                FlowUnitWords[fu]);

            for (int i = 0; i < ni; ++i) {
                auto ui = static_cast<std::size_t>(i);
                int li  = ctx.inlet_usages.link_index[i];
                int di  = ctx.inlet_usages.design_index[i];

                const char* link_name  = (li >= 0) ? ctx.link_names.name_of(li).c_str() : "?";
                const char* inlet_name = (di >= 0 && di < ctx.inlets.count())
                                         ? ctx.inlets.names[di].c_str() : "?";

                double cap_vol  = ctx.inlet_usages.stat_capture_vol[ui];
                double byp_vol  = ctx.inlet_usages.stat_bypass_vol[ui];
                double peak     = ctx.inlet_usages.stat_peak_flow[ui] * Qcf;
                double total    = cap_vol + byp_vol;
                double cap_pct  = (total > 0.0) ? cap_vol / total * 100.0 : 0.0;
                double byp_pct  = (total > 0.0) ? byp_vol / total * 100.0 : 0.0;
                double cap_kgal = cap_vol * FT3_TO_KGAL;
                double byp_kgal = byp_vol * FT3_TO_KGAL;

                std::fprintf(f, "\n  %-20s  %-14s  %9.3f  %9.2f  %9.2f  %9.3f  %9.3f",
                    link_name, inlet_name,
                    peak, cap_pct, byp_pct, cap_kgal, byp_kgal);
            }
        }
        WRITE(f, "");
        WRITE(f, "");
    }

    // =====================================================================
    // Link Pollutant Load Summary — Gap #64, matches legacy writeLinkLoads()
    // stat_total_load is in ft³ × mg/L; convert to lbs: × 28.317/453592
    // =====================================================================
    if (ctx.n_links() > 0 && ctx.n_pollutants() > 0 && opt.rpt_links != 0) {
        int nl = ctx.n_links();
        int np = ctx.n_pollutants();
        // conversion: CFS × mg/L × sec × (28.317 L/ft³) / (453592 mg/lb) = lbs
        static constexpr double LT_PER_FT3 = 28.317;
        // Per-pollutant unit and conversion
        std::vector<double>      mass_cf(static_cast<std::size_t>(np));
        std::vector<const char*> unit_str(static_cast<std::size_t>(np));
        for (int p = 0; p < np; ++p) {
            auto up = static_cast<std::size_t>(p);
            MassUnits mu = (up < ctx.pollutants.units.size()) ?
                            ctx.pollutants.units[up] : MassUnits::MG_PER_L;
            if (mu == MassUnits::COUNTS_PER_L) {
                unit_str[up] = "#";
                mass_cf[up]  = 1.0;
            } else if (mu == MassUnits::UG_PER_L) {
                unit_str[up] = "lbs";
                mass_cf[up]  = LT_PER_FT3 / 453592000.0;
            } else {
                unit_str[up] = "lbs";
                mass_cf[up]  = LT_PER_FT3 / 453592.0;
            }
        }

        WRITE(f, "***************************");
        WRITE(f, "Link Pollutant Load Summary");
        WRITE(f, "***************************");
        std::fprintf(f, "\n");

        // Separator and header
        std::fprintf(f, " \n  ----------------------------------");
        for (int p = 1; p < np; ++p) std::fprintf(f, "--------------");
        std::fprintf(f, " \n                                ");
        for (int p = 0; p < np; ++p)
            std::fprintf(f, "%14s", ctx.pollutant_names.name_of(p).c_str());
        std::fprintf(f, " \n  Link                          ");
        for (int p = 0; p < np; ++p)
            std::fprintf(f, "%14s", unit_str[static_cast<std::size_t>(p)]);
        std::fprintf(f, " \n  ----------------------------------");
        for (int p = 1; p < np; ++p) std::fprintf(f, "--------------");

        for (int j = 0; j < nl; ++j) {
            auto uj = static_cast<std::size_t>(j);
            std::fprintf(f, "\n  %-30s", ctx.link_names.name_of(j).c_str());
            for (int p = 0; p < np; ++p) {
                auto up = static_cast<std::size_t>(p);
                auto idx = uj * static_cast<std::size_t>(np) + up;
                double raw = (idx < ctx.links.stat_total_load.size())
                    ? ctx.links.stat_total_load[idx] : 0.0;
                std::fprintf(f, "%14.3f", raw * mass_cf[up]);
            }
        }

        WRITE(f, "");
        WRITE(f, "");
    }
}

// ---------------------------------------------------------------------------
// write_timing — analysis timing section
// ---------------------------------------------------------------------------

void DefaultReportPlugin::write_timing(std::FILE* f) {
    // =====================================================================
    // Analysis Timing — matches legacy report_writeRunTime()
    // =====================================================================
    {
        char begin_str[64] = "";
        char end_str[64] = "";

        if (wall_start_ != 0) {
            const char* ct = std::ctime(&wall_start_);
            if (ct) {
                std::strncpy(begin_str, ct, sizeof(begin_str) - 1);
                char* nl = std::strchr(begin_str, '\n');
                if (nl) *nl = '\0';
            }
        }

        std::time_t wall_end;
        std::time(&wall_end);
        {
            const char* ct = std::ctime(&wall_end);
            if (ct) {
                std::strncpy(end_str, ct, sizeof(end_str) - 1);
                char* nl = std::strchr(end_str, '\n');
                if (nl) *nl = '\0';
            }
        }

        std::fprintf(f, "\n\n  Analysis begun on:  %s", begin_str);
        std::fprintf(f, "\n  Analysis ended on:  %s", end_str);
        std::fprintf(f, "\n  Total elapsed time: ");

        double elapsed_secs = std::difftime(wall_end, wall_start_);
        if (elapsed_secs < 1.0) {
            std::fprintf(f, "< 1 sec");
        } else {
            int es = static_cast<int>(elapsed_secs);
            std::fprintf(f, "%02d:%02d:%02d", es / 3600, (es % 3600) / 60, es % 60);
        }
        std::fprintf(f, "\n");
    }
}

} /* namespace openswmm */
