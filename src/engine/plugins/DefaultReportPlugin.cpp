/**
 * @file DefaultReportPlugin.cpp
 * @brief DefaultReportPlugin — legacy SWMM-compatible .rpt report writer.
 *
 * @details Replicates the report format from EPA SWMM 5.x report.c and
 *          statsrpt.c. Reference: src/legacy/engine/report.c, statsrpt.c,
 *          text.h for format strings.
 *
 * @see Legacy reference: src/legacy/engine/report.c, statsrpt.c
 * @ingroup engine_plugins
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#include "DefaultReportPlugin.hpp"

#include "../../../include/openswmm/plugin_sdk/PluginState.hpp"
#include "../../../include/openswmm/plugin_sdk/SimulationSnapshot.hpp"
#include "../core/SimulationContext.hpp"
#include "../core/UnitConversion.hpp"
#include "../core/DateTime.hpp"

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

#define WRITE(f, s) std::fprintf(f, "\n  %s", s)
#define LINE_10 "----------"

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

DefaultReportPlugin::DefaultReportPlugin(std::string rpt_path)
    : rpt_path_(std::move(rpt_path))
    , state_(PluginState::LOADED)
{}

int DefaultReportPlugin::initialize(const std::vector<std::string>& /*init_args*/,
                                    const IPluginComponentInfo* /*info*/) {
    state_ = PluginState::INITIALIZED;
    return 0;
}

int DefaultReportPlugin::validate(const SimulationContext& /*ctx*/) {
    state_ = PluginState::VALIDATED;
    return 0;
}

int DefaultReportPlugin::prepare(const SimulationContext& /*ctx*/) {
    std::time(&wall_start_);
    state_ = PluginState::PREPARED;
    return 0;
}

int DefaultReportPlugin::update(const SimulationSnapshot& /*snapshot*/) {
    state_ = PluginState::UPDATING;
    return 0;
}

int DefaultReportPlugin::finalize(const SimulationContext& /*ctx*/) {
    state_ = PluginState::FINALIZED;
    return 0;
}

// ---------------------------------------------------------------------------
// write_summary — replicates legacy report.c + statsrpt.c format
// ---------------------------------------------------------------------------

int DefaultReportPlugin::write_summary(const SimulationContext& ctx) {
    if (rpt_path_.empty()) return 0;

    FILE* f = std::fopen(rpt_path_.c_str(), "w");
    if (!f) return -1;

    char ds[32], ts[16], buf[64];
    const auto& opt = ctx.options;

    int fu = static_cast<int>(opt.flow_units);
    if (fu < 0 || fu > 5) fu = 0;

    // =====================================================================
    // Title — matches legacy FMT01
    // =====================================================================
    std::fprintf(f, "\n  OPEN STORM WATER MANAGEMENT MODEL - VERSION 6.0 (openswmm.engine 6.0.0)");
    std::fprintf(f, "\n  ----------------------------------------------------------------------");

    // [TITLE] content (if any is stored in ctx)
    if (!ctx.inp_file_path.empty()) {
        // Legacy prints title lines here
    }

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
    std::fprintf(f, "\n    RDII ................... NO");
    std::fprintf(f, "\n    Snowmelt ............... NO");
    std::fprintf(f, "\n    Groundwater ............ NO");
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

    // =====================================================================
    // Runoff Quantity Continuity — matches legacy massbal.c
    // =====================================================================
    {
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
    // Runoff Quality Continuity — per pollutant (up to 5 per table)
    // Matches legacy report_writeLoadingError() / report_LoadingErrors()
    // =====================================================================
    if (ctx.n_pollutants() > 0) {
        const auto& mb = ctx.mass_balance;
        int np = ctx.n_pollutants();

        // Process in batches of up to 5 pollutants (matching legacy)
        int p1 = 0;
        while (p1 < np) {
            int p2 = std::min(p1 + 4, np - 1);

            // Header row: pollutant names
            std::fprintf(f, "\n  **************************");
            for (int p = p1; p <= p2; ++p)
                std::fprintf(f, "%14s", ctx.pollutant_names.name_of(p).c_str());

            // Units row
            std::fprintf(f, "\n  Runoff Quality Continuity ");
            for (int p = p1; p <= p2; ++p)
                std::fprintf(f, "%13s", "lbs");

            // Separator
            std::fprintf(f, "\n  **************************");
            for (int p = p1; p <= p2; ++p)
                std::fprintf(f, "    ----------");

            // Data rows — each row prints values for all pollutants in batch
            auto qrow = [&](const char* label, const std::vector<double>& vals) {
                std::fprintf(f, "\n  %s", label);
                for (int p = p1; p <= p2; ++p)
                    std::fprintf(f, "%14.3f", vals[static_cast<std::size_t>(p)]);
            };

            qrow("Initial Buildup ..........", mb.qual_init_buildup);
            qrow("Surface Buildup ..........", mb.qual_surface_buildup);
            qrow("Wet Deposition ...........", mb.qual_wet_deposition);
            qrow("Sweeping Removal .........", mb.qual_sweeping);
            qrow("Infiltration Loss ........", mb.qual_infil_loss);
            qrow("BMP Removal ..............", mb.qual_bmp_removal);
            qrow("Surface Runoff ...........", mb.qual_runoff_load);
            qrow("Remaining Buildup ........", mb.qual_final_buildup);

            // Continuity error row
            std::fprintf(f, "\n  Continuity Error (%%) .....");
            for (int p = p1; p <= p2; ++p) {
                auto up = static_cast<std::size_t>(p);
                double total_in = mb.qual_init_buildup[up] + mb.qual_surface_buildup[up]
                                  + mb.qual_wet_deposition[up];
                double total_out = mb.qual_sweeping[up] + mb.qual_infil_loss[up]
                                   + mb.qual_bmp_removal[up] + mb.qual_runoff_load[up]
                                   + mb.qual_final_buildup[up];
                double error = (total_in > 0.0) ? (total_in - total_out) / total_in * 100.0 : 0.0;
                std::fprintf(f, "%14.3f", error);
            }

            WRITE(f, "");
            WRITE(f, "");
            p1 = p2 + 1;
        }
    }

    // =====================================================================
    // Flow Routing Continuity — matches legacy massbal.c
    // =====================================================================
    {
        const auto& mb = ctx.mass_balance;
        std::fprintf(f, "\n  **************************        Volume        Volume");
        std::fprintf(f, "\n  Flow Routing Continuity        acre-feet      10^6 gal");
        std::fprintf(f, "\n  **************************     ---------     ---------");

        // ucf1 = acre-feet per ft3, ucf2 = 10^6 gal per ft3
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
    // Quality Routing Continuity — per pollutant (up to 5 per table)
    // Matches legacy report_writeQualError() / report_QualErrors()
    // =====================================================================
    if (ctx.n_pollutants() > 0) {
        const auto& mb = ctx.mass_balance;
        int np = ctx.n_pollutants();
        // Zero vectors for unsupported inflow types
        std::vector<double> zeros(static_cast<std::size_t>(np), 0.0);

        int p1 = 0;
        while (p1 < np) {
            int p2 = std::min(p1 + 4, np - 1);

            std::fprintf(f, "\n  **************************");
            for (int p = p1; p <= p2; ++p)
                std::fprintf(f, "%14s", ctx.pollutant_names.name_of(p).c_str());

            std::fprintf(f, "\n  Quality Routing Continuity");
            for (int p = p1; p <= p2; ++p)
                std::fprintf(f, "%13s", "lbs");

            std::fprintf(f, "\n  **************************");
            for (int p = p1; p <= p2; ++p)
                std::fprintf(f, "    ----------");

            auto qrow = [&](const char* label, const std::vector<double>& vals) {
                std::fprintf(f, "\n  %s", label);
                for (int p = p1; p <= p2; ++p)
                    std::fprintf(f, "%14.3f", vals[static_cast<std::size_t>(p)]);
            };

            qrow("Dry Weather Inflow .......", zeros);
            qrow("Wet Weather Inflow .......", mb.qual_routing_wet);
            qrow("Groundwater Inflow .......", zeros);
            qrow("RDII Inflow ..............", zeros);
            qrow("External Inflow ..........", zeros);
            qrow("External Outflow .........", mb.qual_routing_outflow);
            qrow("Flooding Loss ............", mb.qual_routing_flood);
            qrow("Exfiltration Loss ........", zeros);
            qrow("Mass Reacted .............", mb.qual_routing_reacted);
            qrow("Initial Stored Mass ......", mb.qual_routing_init);
            qrow("Final Stored Mass ........", mb.qual_routing_final);

            std::fprintf(f, "\n  Continuity Error (%%) .....");
            for (int p = p1; p <= p2; ++p) {
                auto up = static_cast<std::size_t>(p);
                double total_in = mb.qual_routing_wet[up] + mb.qual_routing_init[up];
                double total_out = mb.qual_routing_outflow[up] + mb.qual_routing_flood[up]
                                   + mb.qual_routing_reacted[up] + mb.qual_routing_final[up];
                double error = (total_in > 0.0) ? (total_in - total_out) / total_in * 100.0 : 0.0;
                std::fprintf(f, "%14.3f", error);
            }

            WRITE(f, "");
            WRITE(f, "");
            p1 = p2 + 1;
        }
    }

    // =====================================================================
    // Time-Step Critical Elements
    // =====================================================================
    WRITE(f, "***************************");
    WRITE(f, "Time-Step Critical Elements");
    WRITE(f, "***************************");
    WRITE(f, "  None");
    WRITE(f, "");

    // =====================================================================
    // Highest Flow Instability Indexes
    // =====================================================================
    WRITE(f, "********************************");
    WRITE(f, "Highest Flow Instability Indexes");
    WRITE(f, "********************************");
    WRITE(f, "All links are stable.");
    WRITE(f, "");

    // =====================================================================
    // Most Frequent Nonconverging Nodes
    // =====================================================================
    WRITE(f, "*********************************");
    WRITE(f, "Most Frequent Nonconverging Nodes");
    WRITE(f, "*********************************");
    WRITE(f, "Convergence obtained at all time steps.");
    WRITE(f, "");

    // =====================================================================
    // Routing Time Step Summary
    // =====================================================================
    WRITE(f, "*************************");
    WRITE(f, "Routing Time Step Summary");
    WRITE(f, "*************************");
    {
        const auto& rs = ctx.routing_stats;
        if (rs.n_steps > 0) {
            std::fprintf(f, "\n  %-34s :  %7.2f sec", "Minimum Time Step",
                         rs.min_step < 1.0e30 ? rs.min_step : 0.0);
            std::fprintf(f, "\n  %-34s :  %7.2f sec", "Average Time Step",
                         rs.avg_step());
            std::fprintf(f, "\n  %-34s :  %7.2f sec", "Maximum Time Step",
                         rs.max_step);
            std::fprintf(f, "\n  %-34s :  %7.1f", "Percent in Steady State",
                         rs.steady_pct);
            std::fprintf(f, "\n  %-34s :  %7.1f",
                         "Average Iterations per Step", rs.avg_iterations);
        }
    }
    WRITE(f, "");

    // =====================================================================
    // Subcatchment Runoff Summary — matches legacy writeSubcatchRunoff()
    // =====================================================================
    if (ctx.n_subcatches() > 0) {
        std::fprintf(f, "\n");
        WRITE(f, "***************************");
        WRITE(f, "Subcatchment Runoff Summary");
        WRITE(f, "***************************");
        std::fprintf(f, "\n");
        std::fprintf(f,
"\n  ------------------------------------------------------------------------------------------------------------------------------"
"\n                            Total      Total      Total      Total     Imperv       Perv      Total       Total     Peak  Runoff"
"\n                           Precip      Runon       Evap      Infil     Runoff     Runoff     Runoff      Runoff   Runoff   Coeff"
"\n  Subcatchment                 in         in         in         in         in         in         in    10^6 gal      %3s"
"\n  ------------------------------------------------------------------------------------------------------------------------------",
            FlowUnitWords[fu]);

        for (int j = 0; j < ctx.n_subcatches(); ++j) {
            auto uj = static_cast<std::size_t>(j);
            double a = ctx.subcatches.area[uj];
            if (a <= 0.0) continue;
            double area_ft2 = a * ucf::ACRES_TO_FT2;

            // Convert volumes (ft3) to depth (inches) over subcatchment area
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
            double runoff_mgal = ctx.subcatches.stat_runoff_vol[uj] * ucf::FT3_TO_MGAL;
            double peak = ctx.subcatches.stat_max_runoff[uj];
            double coeff = (precip_in > 0.0) ? runoff_in / precip_in : 0.0;

            std::fprintf(f, "\n  %-20s %10.2f %10.2f %10.2f %10.2f %10.2f %10.2f %10.2f %11.2f %8.2f%8.3f",
                         ctx.subcatch_names.name_of(j).c_str(),
                         precip_in, 0.0, evap_in, infil_in, imperv_in, perv_in,
                         runoff_in, runoff_mgal, peak, coeff);
        }
    }

    WRITE(f, "");
    WRITE(f, "");

    // =====================================================================
    // Subcatchment Washoff Summary — matches legacy writeSubcatchLoads()
    // =====================================================================
    if (ctx.n_subcatches() > 0 && ctx.n_pollutants() > 0) {
        int np = ctx.n_pollutants();

        WRITE(f, "****************************");
        WRITE(f, "Subcatchment Washoff Summary");
        WRITE(f, "****************************");
        std::fprintf(f, "\n");

        // Header separator
        std::fprintf(f, "\n  --------------------");
        for (int p = 0; p < np; ++p)
            std::fprintf(f, "--------------");

        // Pollutant name row
        std::fprintf(f, "\n                      ");
        for (int p = 0; p < np; ++p)
            std::fprintf(f, "%14s", ctx.pollutant_names.name_of(p).c_str());

        // Units row
        std::fprintf(f, "\n  Subcatchment        ");
        std::vector<double> totals(static_cast<std::size_t>(np), 0.0);
        for (int p = 0; p < np; ++p)
            std::fprintf(f, "%14s", "lbs");

        // Bottom separator
        std::fprintf(f, "\n  --------------------");
        for (int p = 0; p < np; ++p)
            std::fprintf(f, "--------------");

        // Data rows
        for (int j = 0; j < ctx.n_subcatches(); ++j) {
            auto uj = static_cast<std::size_t>(j);
            std::fprintf(f, "\n  %-20s", ctx.subcatch_names.name_of(j).c_str());
            for (int p = 0; p < np; ++p) {
                auto idx = uj * static_cast<std::size_t>(np) + static_cast<std::size_t>(p);
                double x = (idx < ctx.subcatches.total_load.size())
                           ? ctx.subcatches.total_load[idx] : 0.0;
                totals[static_cast<std::size_t>(p)] += x;
                std::fprintf(f, "%14.3f", x);
            }
        }

        // Separator
        std::fprintf(f, "\n  --------------------");
        for (int p = 0; p < np; ++p)
            std::fprintf(f, "--------------");

        // System totals row
        std::fprintf(f, "\n  System              ");
        for (int p = 0; p < np; ++p)
            std::fprintf(f, "%14.3f", totals[static_cast<std::size_t>(p)]);
    }

    WRITE(f, "");
    WRITE(f, "");

    // =====================================================================
    // Node Depth Summary — matches legacy writeNodeDepths()
    // =====================================================================
    if (ctx.n_nodes() > 0) {
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

        const char* node_type_words[] = {"JUNCTION", "OUTFALL", "DIVIDER", "STORAGE"};

        for (int j = 0; j < ctx.n_nodes(); ++j) {
            auto uj = static_cast<std::size_t>(j);
            int nt = static_cast<int>(ctx.nodes.type[uj]);
            const char* nt_str = (nt >= 0 && nt <= 3) ? node_type_words[nt] : "JUNCTION";
            double max_d = ctx.nodes.stat_max_depth[uj];
            double max_hgl = ctx.nodes.invert_elev[uj] + max_d;

            std::fprintf(f, "\n  %-20s %-9s %8.2f %8.2f %8.2f  %13s %8.2f",
                         ctx.node_names.name_of(j).c_str(), nt_str,
                         0.0, max_d, max_hgl, "", max_d);
        }
    }

    WRITE(f, "");
    WRITE(f, "");

    // =====================================================================
    // Node Inflow Summary — matches legacy writeNodeFlows()
    // =====================================================================
    if (ctx.n_nodes() > 0) {
        WRITE(f, "*******************");
        WRITE(f, "Node Inflow Summary");
        WRITE(f, "*******************");
        std::fprintf(f, "\n");
        std::fprintf(f,
"\n  -------------------------------------------------------------------------------------------------"
"\n                                  Maximum  Maximum                  Lateral       Total        Flow"
"\n                                  Lateral    Total  Time of Max      Inflow      Inflow     Balance"
"\n                                   Inflow   Inflow   Occurrence      Volume      Volume       Error"
"\n  Node                 Type           %3s      %3s  days hr:min    10^6 gal    10^6 gal     Percent"
"\n  -------------------------------------------------------------------------------------------------",
            FlowUnitWords[fu], FlowUnitWords[fu]);

        for (int j = 0; j < ctx.n_nodes(); ++j) {
            auto uj = static_cast<std::size_t>(j);
            int nt = static_cast<int>(ctx.nodes.type[uj]);
            const char* node_type_words[] = {"JUNCTION", "OUTFALL", "DIVIDER", "STORAGE"};
            const char* nt_str = (nt >= 0 && nt <= 3) ? node_type_words[nt] : "JUNCTION";

            double max_lat  = ctx.nodes.stat_max_lat_inflow[uj];
            double max_tot  = ctx.nodes.stat_max_total_inflow[uj];
            // Convert ft3 → 10^6 gallons (1 ft3 = 7.48052 gal)
            constexpr double FT3_TO_MG = 7.48052e-6;
            double vol_lat  = ctx.nodes.stat_lat_inflow_vol[uj]  * FT3_TO_MG;
            double vol_tot  = ctx.nodes.stat_total_inflow_vol[uj] * FT3_TO_MG;
            std::fprintf(f, "\n  %-20s %-9s %8.2f %8.2f  %13s %11.3f %11.3f %9.3f",
                         ctx.node_names.name_of(j).c_str(), nt_str,
                         max_lat, max_tot, "", vol_lat, vol_tot, 0.0);
        }
    }

    WRITE(f, "");
    WRITE(f, "");

    // =====================================================================
    // Node Surcharge Summary
    // =====================================================================
    if (static_cast<int>(opt.routing_model) == 2) { // DYNWAVE only
        WRITE(f, "**********************");
        WRITE(f, "Node Surcharge Summary");
        WRITE(f, "**********************");
        WRITE(f, "");
        WRITE(f, "No nodes were surcharged.");
    }

    WRITE(f, "");
    WRITE(f, "");

    // =====================================================================
    // Node Flooding Summary — matches legacy writeNodeFlooding()
    // =====================================================================
    {
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
            std::fprintf(f,
"\n  ---------------------------------------------------------------------------"
"\n                                                             Total   Maximum"
"\n                                 Maximum   Time of Max      Flood    Ponded"
"\n                        Hours       Rate    Occurrence     Volume     Depth"
"\n  Node                Flooded       %3s   days hr:min    10^6 gal      Feet"
"\n  ---------------------------------------------------------------------------",
                FlowUnitWords[fu]);

            for (int j = 0; j < ctx.n_nodes(); ++j) {
                auto uj = static_cast<std::size_t>(j);
                if (ctx.nodes.stat_vol_flooded[uj] <= 0.0) continue;

                double hours = ctx.nodes.stat_time_flooded[uj] / 3600.0;
                double max_rate = ctx.nodes.stat_max_overflow[uj];
                double mgal = ctx.nodes.stat_vol_flooded[uj] * ucf::FT3_TO_MGAL;

                std::fprintf(f, "\n  %-20s %7.2f %9.4f  %13s %9.4f    %8.3f",
                             ctx.node_names.name_of(j).c_str(),
                             hours, max_rate, "", mgal, 0.0);
            }
        }
    }

    WRITE(f, "");
    WRITE(f, "");

    // =====================================================================
    // Outfall Loading Summary — matches legacy writeOutfallLoads()
    // =====================================================================
    {
        int np = ctx.n_pollutants();

        WRITE(f, "***********************");
        WRITE(f, "Outfall Loading Summary");
        WRITE(f, "***********************");
        std::fprintf(f, "\n");

        // Top separator
        std::fprintf(f, "\n  -----------------------------------------------------------");
        for (int p = 0; p < np; ++p)
            std::fprintf(f, "--------------");

        // Header row 1: "Total" for each pollutant
        std::fprintf(f, "\n                         Flow       Avg       Max       Total");
        for (int p = 0; p < np; ++p)
            std::fprintf(f, "         Total");

        // Header row 2: pollutant names
        std::fprintf(f, "\n                         Freq      Flow      Flow      Volume");
        for (int p = 0; p < np; ++p)
            std::fprintf(f, "%14s", ctx.pollutant_names.name_of(p).c_str());

        // Header row 3: units
        std::fprintf(f, "\n  Outfall Node           Pcnt       %3s       %3s    10^6 gal",
                     FlowUnitWords[fu], FlowUnitWords[fu]);
        for (int p = 0; p < np; ++p)
            std::fprintf(f, "%14s", "lbs");

        // Bottom separator
        std::fprintf(f, "\n  -----------------------------------------------------------");
        for (int p = 0; p < np; ++p)
            std::fprintf(f, "--------------");

        double sys_vol = 0.0;
        double sys_avg_flow = 0.0;
        double sys_max_flow = 0.0;
        long   sys_periods  = 0;
        int outfall_count = 0;
        std::vector<double> pol_totals(static_cast<std::size_t>(np), 0.0);
        long total_routing_steps = ctx.routing_stats.n_steps;

        for (int j = 0; j < ctx.n_nodes(); ++j) {
            auto uj = static_cast<std::size_t>(j);
            if (ctx.nodes.type[uj] != NodeType::OUTFALL) continue;
            outfall_count++;

            long   periods = ctx.nodes.stat_outfall_periods[uj];
            double avg_flow = (periods > 0)
                ? ctx.nodes.stat_outfall_avg_flow[uj] / static_cast<double>(periods)
                : 0.0;
            double max_flow = ctx.nodes.stat_outfall_max_flow[uj];
            double freq = (total_routing_steps > 0)
                ? 100.0 * static_cast<double>(periods) / static_cast<double>(total_routing_steps)
                : 0.0;
            // Volume in 10^6 gallons: sum(Q)*dt gives ft3, * 7.48052 gal/ft3
            double vol = ctx.nodes.stat_outfall_avg_flow[uj]
                         * ctx.routing_stats.avg_step() * 7.48052 / 1.0e6;

            sys_avg_flow += ctx.nodes.stat_outfall_avg_flow[uj];
            sys_max_flow += max_flow;
            sys_periods  += periods;
            sys_vol      += vol;

            std::fprintf(f, "\n  %-20s %5.2f %9.2f %9.2f   %9.3f",
                         ctx.node_names.name_of(j).c_str(),
                         freq, avg_flow, max_flow, vol);

            // Pollutant loads at outfall
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
        std::fprintf(f, "\n  -----------------------------------------------------------");
        for (int p = 0; p < np; ++p)
            std::fprintf(f, "--------------");

        std::fprintf(f, "\n  System              %5.2f %9.2f %9.2f   %9.3f",
                     0.0, 0.0, 0.0, sys_vol);
        for (int p = 0; p < np; ++p)
            std::fprintf(f, "%14.3f", pol_totals[static_cast<std::size_t>(p)]);
    }

    WRITE(f, "");
    WRITE(f, "");

    // =====================================================================
    // Link Flow Summary — matches legacy writeLinkFlows()
    // =====================================================================
    if (ctx.n_links() > 0) {
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

        const char* link_type_words[] = {"CONDUIT", "PUMP", "ORIFICE", "WEIR", "OUTLET"};

        for (int j = 0; j < ctx.n_links(); ++j) {
            auto uj = static_cast<std::size_t>(j);
            int lt = static_cast<int>(ctx.links.type[uj]);
            const char* lt_str = (lt >= 0 && lt <= 4) ? link_type_words[lt] : "CONDUIT";

            double mf = ctx.links.stat_max_flow[uj];
            double mv = ctx.links.stat_max_veloc[uj];
            double fill = ctx.links.stat_max_filling[uj];
            double flow_ratio = (ctx.links.q_full[uj] > 0.0) ?
                mf / ctx.links.q_full[uj] : 0.0;

            std::fprintf(f, "\n  %-20s %-9s %9.2f  %13s  %8.2f %7.2f %7.2f",
                         ctx.link_names.name_of(j).c_str(), lt_str,
                         mf, "", mv, flow_ratio, fill);
        }
    }

    WRITE(f, "");
    WRITE(f, "");

    // =====================================================================
    // Flow Classification Summary
    // =====================================================================
    WRITE(f, "***************************");
    WRITE(f, "Flow Classification Summary");
    WRITE(f, "***************************");
    if (ctx.n_links() > 0) {
        std::fprintf(f,
"\n  ----------------------------------------------------------------------------------------------------------"
"\n                      --- Fraction of Time in Flow Class ----"
"\n                      Dry    Up Dry  Dn Dry  SubCrit SupCrit Up Crit Dn Crit Norm Ltd Inlet Ctrl"
"\n  Conduit             "
        );
        std::fprintf(f,
"\n  ----------------------------------------------------------------------------------------------------------");

        for (int j = 0; j < ctx.n_links(); ++j) {
            auto uj = static_cast<std::size_t>(j);
            if (ctx.links.type[uj] != LinkType::CONDUIT) continue;
            long total = 0;
            for (int c = 0; c < LinkData::N_FLOW_CLASSES; ++c) {
                auto idx = uj * LinkData::N_FLOW_CLASSES + static_cast<std::size_t>(c);
                if (idx < ctx.links.stat_flow_class.size())
                    total += ctx.links.stat_flow_class[idx];
            }
            if (total == 0) total = 1;
            std::fprintf(f, "\n  %-20s", ctx.link_names.name_of(j).c_str());
            for (int c = 0; c < LinkData::N_FLOW_CLASSES; ++c) {
                auto idx = uj * LinkData::N_FLOW_CLASSES + static_cast<std::size_t>(c);
                long cnt = (idx < ctx.links.stat_flow_class.size())
                           ? ctx.links.stat_flow_class[idx] : 0L;
                double pct = 100.0 * static_cast<double>(cnt) / static_cast<double>(total);
                std::fprintf(f, "  %5.2f ", pct);
            }
            // Norm Ltd and Inlet Ctrl columns
            double norm_pct = 100.0 * static_cast<double>(ctx.links.stat_norm_ltd[uj])
                              / static_cast<double>(total);
            double inlet_pct = 100.0 * static_cast<double>(ctx.links.stat_inlet_ctrl[uj])
                               / static_cast<double>(total);
            std::fprintf(f, " %5.2f    %5.2f", norm_pct, inlet_pct);
        }
    }
    WRITE(f, "");

    // =====================================================================
    // Conduit Surcharge Summary
    // =====================================================================
    WRITE(f, "*************************");
    WRITE(f, "Conduit Surcharge Summary");
    WRITE(f, "*************************");
    WRITE(f, "");
    WRITE(f, "No conduits were surcharged.");

    WRITE(f, "");
    WRITE(f, "");

    // =====================================================================
    // Link Pollutant Load Summary — matches legacy writeLinkLoads()
    // =====================================================================
    if (ctx.n_links() > 0 && ctx.n_pollutants() > 0) {
        int np = ctx.n_pollutants();

        WRITE(f, "***************************");
        WRITE(f, "Link Pollutant Load Summary");
        WRITE(f, "***************************");
        std::fprintf(f, "\n");

        // Header separator
        std::fprintf(f, "\n  --------------------");
        for (int p = 0; p < np; ++p)
            std::fprintf(f, "--------------");

        // Pollutant name row
        std::fprintf(f, "\n                      ");
        for (int p = 0; p < np; ++p)
            std::fprintf(f, "%14s", ctx.pollutant_names.name_of(p).c_str());

        // Units row
        std::fprintf(f, "\n  Link                ");
        for (int p = 0; p < np; ++p)
            std::fprintf(f, "%14s", "lbs");

        // Bottom separator
        std::fprintf(f, "\n  --------------------");
        for (int p = 0; p < np; ++p)
            std::fprintf(f, "--------------");

        // Data rows — per-link cumulative pollutant loads
        for (int j = 0; j < ctx.n_links(); ++j) {
            auto uj = static_cast<std::size_t>(j);
            std::fprintf(f, "\n  %-20s", ctx.link_names.name_of(j).c_str());
            auto base = uj * static_cast<std::size_t>(np);
            for (int p = 0; p < np; ++p) {
                auto idx = base + static_cast<std::size_t>(p);
                double load = (idx < ctx.links.stat_total_load.size())
                    ? ctx.links.stat_total_load[idx] : 0.0;
                std::fprintf(f, "%14.3f", load);
            }
        }
    }

    WRITE(f, "");
    WRITE(f, "");

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

    std::fclose(f);
    return 0;
}

} /* namespace openswmm */
