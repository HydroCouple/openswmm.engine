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
 * @license  Apache-2.0
 */

#include "DefaultReportPlugin.hpp"
#include "core/FileIO.hpp"   // issue #7: UTF-8 paths on Windows

#include "../../../include/openswmm/plugin_sdk/PluginState.hpp"
#include "../../../include/openswmm/plugin_sdk/SimulationSnapshot.hpp"
#include "../core/SimulationContext.hpp"
#include "../core/UnitConversion.hpp"
#include "../core/DateTime.hpp"
#include "../hydraulics/Node.hpp"   // node::getVolume — storage volume from its depth-relation
#include "../hydraulics/Link.hpp"       // link::buildXSectParams — street spread at max depth
#include "../hydraulics/XSectBatch.hpp" // xsect::getWofY
#include "../hydraulics/Transect.hpp"   // shape / street geometry tables
#include "../hydraulics/Street.hpp"
#include "../transport/TransportPolicy.hpp"   // E2: Domain x Species matrix block
#include "../2d/quality/SurfaceQuality2D.hpp"   // S7: 2D Surface Washoff Summary
#include "../2d/data/MeshData.hpp"
#include "../2d/subsurface/SubsurfaceData.hpp"   // G-O: 2D Aquifer Continuity
#include "../2d/subsurface/SubsurfaceTransportState.hpp"  // T7.5: its quality twin

#include <version.h>

#include <array>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <cmath>
#include <vector>
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
static const char* SurchargeWords[] = { "EXTRAN", "SLOT", "DYNAMIC_SLOT", "TPA" };
static const char* NodeTypeWords[] = { "JUNCTION", "OUTFALL", "DIVIDER", "STORAGE" };
static const char* LinkTypeWords[] = { "CONDUIT", "PUMP", "ORIFICE", "WEIR", "OUTLET" };
static const char* RainTypeWords[] = { "INTENSITY", "VOLUME", "CUMULATIVE" };
// Spelled as legacy XsectTypeWords (keywords.c), which the report prints.
static const char* XsectShapeWords[] = {
    "CIRCULAR", "FILLED_CIRCULAR", "RECT_CLOSED", "RECT_OPEN",
    "TRAPEZOIDAL", "TRIANGULAR", "PARABOLIC", "POWER",
    "MODBASKETHANDLE", "EGG", "HORSESHOE", "GOTHIC",
    "CATENARY", "SEMIELLIPTICAL", "BASKETHANDLE", "SEMICIRCULAR",
    "RECT_TRIANGULAR", "RECT_ROUND", "HORIZ_ELLIPSE", "VERT_ELLIPSE",
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
// legacy getElapsedTime (swmm5.c) + datetime_decodeTime: time since the
// REPORT start, rounded to the nearest second before it is split, so a
// maximum at 12:59:59.99 reads 13:00 (truncating the fraction read 12:59).
static void elapsedToParts(double date, double report_start, int& days, int& hrs, int& mins) {
    days = hrs = mins = 0;
    if (date <= 0.0) return;
    const double x = date - report_start;
    if (x <= 0.0) return;
    days = static_cast<int>(x);
    int secs = static_cast<int>(std::floor((x - std::floor(x)) * 86400.0 + 0.5));
    if (secs >= 86400) secs = 86399;
    const int total_mins = secs / 60;
    hrs  = total_mins / 60;
    mins = total_mins % 60;
    if (hrs > 23) hrs = 0;
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

// Project land-area units → internal ft². Subcatchment area is stored in
// acres (US) or hectares (SI); multiply by this to get ft² (43560 for US,
// 107639 for SI). Matches legacy 1/UCF(LANDAREA).
static double landAreaToFt2(int flow_units) {
    const int us = (flow_units >= 3) ? 1 : 0; // CMS/LPS/MLD → SI
    return 1.0 / ucf::Ucf[ucf::LANDAREA][us];
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
    // Open the report file early and write preamble (title, input summaries,
    // analysis options) so they are available immediately — even if the
    // simulation crashes before write_summary() is called.
    if (!rpt_path_.empty() && !ctx.options.rpt_disabled) {
        file_ = openswmm::io::fopen_utf8(rpt_path_, "w");
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

// Legacy input.c reports at most MAXERRS (100) input errors, then
// "Maximum error count exceeded." and stops reading. ctx.errors keeps every
// error for API callers; only the .rpt is capped. `from` is the first index
// not yet written.
// Messages are stored with their own leading blanks; legacy prints every
// error and warning at a two-blank indent.
static const char* unindented(const std::string& m) {
    const auto k = m.find_first_not_of(' ');
    return m.c_str() + (k == std::string::npos ? m.size() : k);
}

static void write_errors(std::FILE* f, const std::vector<std::string>& errors,
                         std::size_t from) {
    constexpr std::size_t kMaxErrs = 100;
    for (std::size_t i = from; i < errors.size() && i < kMaxErrs; ++i)
        std::fprintf(f, "\n  %s", unindented(errors[i]));
    if (errors.size() > kMaxErrs && from <= kMaxErrs)
        std::fprintf(f, "\n  \n  Maximum error count exceeded.");
}

int DefaultReportPlugin::finalize(const SimulationContext& ctx) {
    // Flush any errors/warnings that accumulated during the simulation.
    // This ensures they are persisted even if write_summary() never runs.
    if (file_) {
        write_errors(file_, ctx.errors, errors_written_);
        errors_written_ = ctx.errors.size();

        for (std::size_t i = warnings_written_; i < ctx.warnings.size(); ++i)
            std::fprintf(file_, "\n  %s", unindented(ctx.warnings[i]));
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
        f = openswmm::io::fopen_utf8(rpt_path_, "w");
        if (!f) return -1;
        write_preamble(f, ctx);

        // Write all errors/warnings
        write_errors(f, ctx.errors, 0);
        for (const auto& warn : ctx.warnings)
            std::fprintf(f, "\n  %s", unindented(warn));
    }

    // Write result sections (continuity, statistics, summaries)
    write_results(f, ctx);

    // Write analysis timing and close
    write_timing(f, ctx);

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

    const ucf::DisplayUnits du_opt = ucf::DisplayUnits::from(opt);

    // =====================================================================
    // Title — matches legacy FMT01
    // =====================================================================
    std::fprintf(f, "\n  OPENSWMM ENGINE - VERSION %s", OPENSWMM_VERSION_FULL);
    std::fprintf(f, "\n  ------------------------------------------------------------");
    std::fprintf(f, "\n");   // legacy FMT10

    // Warnings raised while legacy reads the input (unknown section / option
    // keyword) carry their own leading "\n  " and precede the title; the rest
    // come from validation, after it.
    for (const auto& warn : ctx.warnings)
        if (!warn.empty() && warn[0] == '\n')
            std::fprintf(f, "\n  %s", warn.c_str());

    // [TITLE]: legacy keeps at most MAXTITLE (3) lines, skips comment lines,
    // and stores each raw line with its line feed turned into a blank.
    {
        int n_title = 0;
        for (const auto& line : ctx.title_notes) {
            const auto first = line.find_first_not_of(" \t");
            if (first == std::string::npos || line[first] == ';') continue;
            if (n_title++ == 3) break;
            std::fprintf(f, "\n  %s ", line.c_str());
        }
    }

    // Errors — matches legacy report_writeErrorMsg() format
    write_errors(f, ctx.errors, 0);
    errors_written_ = ctx.errors.size();

    // Warnings — matches legacy report_writeWarningMsg() format
    for (const auto& warn : ctx.warnings)
        if (warn.empty() || warn[0] != '\n')
            std::fprintf(f, "\n  %s", unindented(warn));
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
    std::fprintf(f, "\n  Number of land uses ....... %d",
                 static_cast<int>(ctx.landuse_names.size()));

    WRITE(f, "");
    WRITE(f, "");

    // =====================================================================
    // Pollutant / Landuse Summary — legacy inputrpt.c
    // =====================================================================
    if (ctx.n_pollutants() > 0) {
        static const char* const kQualUnits[] = {"MG/L", "UG/L", "#/L"};
        WRITE(f, "*****************");
        WRITE(f, "Pollutant Summary");
        WRITE(f, "*****************");
        std::fprintf(f, "\n                               Ppt.      GW         Kdecay");
        std::fprintf(f, "\n  Name                 Units   Concen.   Concen.    1/days    CoPollutant");
        std::fprintf(f, "\n  -----------------------------------------------------------------------");
        const auto& P = ctx.pollutants;
        for (int i = 0; i < ctx.n_pollutants(); ++i) {
            const auto ui = static_cast<std::size_t>(i);
            const int u = static_cast<int>(P.units[ui]);
            std::fprintf(f, "\n  %-20s %5s%10.2f%10.2f%10.2f",
                ctx.pollutant_names.name_of(i).c_str(),
                (u >= 0 && u <= 2) ? kQualUnits[u] : "MG/L",
                P.c_rain[ui], P.c_gw[ui], P.k_decay[ui] * 86400.0);
            if (P.co_pollut[ui] >= 0)
                std::fprintf(f, "    %-s  (%.2f)",
                    ctx.pollutant_names.name_of(P.co_pollut[ui]).c_str(), P.co_frac[ui]);
        }
        WRITE(f, "");
        WRITE(f, "");
    }
    if (ctx.landuse_names.size() > 0) {
        WRITE(f, "***************");
        WRITE(f, "Landuse Summary");
        WRITE(f, "***************");
        std::fprintf(f, "\n                         Sweeping   Maximum      Last");
        std::fprintf(f, "\n  Name                   Interval   Removal     Swept");
        std::fprintf(f, "\n  ---------------------------------------------------");
        for (int i = 0; i < ctx.landuses.count(); ++i) {
            const auto ui = static_cast<std::size_t>(i);
            std::fprintf(f, "\n  %-20s %10.2f%10.2f%10.2f",
                ctx.landuse_names.name_of(i).c_str(),
                ctx.landuses.sweep_interval[ui], ctx.landuses.sweep_removal[ui],
                ctx.landuses.last_swept[ui]);
        }
        WRITE(f, "");
        WRITE(f, "");
    }

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
            // legacy prints the outlet subcatchment when it drains to one
            const int out_sub = ctx.subcatches.outlet_subcatch[ui];
            if (out_node < 0 && out_sub >= 0)
                outlet_name = ctx.subcatch_names.name_of(out_sub).c_str();

            std::fprintf(f, "\n  %-20s %10.2f%10.2f%10.2f%10.4f %-20s %-20s",
                name.c_str(),
                ctx.subcatches.area[ui],
                ctx.subcatches.width[ui] * du_opt.length,
                ctx.subcatches.frac_imperv[ui] * 100.0,
                ctx.subcatches.slope[ui] * 100.0,
                gage_name, outlet_name);
        }
        WRITE(f, "");
        WRITE(f, "");
    }

    // =====================================================================
    // LID Control Summary — legacy lid_writeSummary, written when some
    // subcatchment has LID area. Each subcatchment's units are listed in
    // legacy lidList order, which is [LID_USAGE] order REVERSED (prepended).
    // =====================================================================
    {
        const auto& LU = ctx.lid_usage;
        bool any_lid = false;
        for (int k = 0; k < LU.count() && !any_lid; ++k)
            any_lid = LU.area[static_cast<std::size_t>(k)] *
                      LU.number[static_cast<std::size_t>(k)] > 0.0;
        if (any_lid) {
            const double ucf_len  = du_opt.length;
            const double ucf_len2 = ucf_len * ucf_len;
            const double la_ft2   = landAreaToFt2(fu);
            std::fprintf(f, "\n  *******************");
            std::fprintf(f, "\n  LID Control Summary");
            std::fprintf(f, "\n  *******************");
            std::fprintf(f,
"\n                                   No. of        Unit        Unit      %% Area    %% Imperv      %% Perv");
            std::fprintf(f,
"\n  Subcatchment     LID Control      Units        Area       Width     Covered     Treated     Treated");
            std::fprintf(f,
"\n  ---------------------------------------------------------------------------------------------------");
            for (int j = 0; j < ctx.n_subcatches(); ++j) {
                const double sub_area = ctx.subcatches.area[static_cast<std::size_t>(j)] * la_ft2;
                for (int k = LU.count() - 1; k >= 0; --k) {
                    const auto uk = static_cast<std::size_t>(k);
                    if (LU.subcatch_index[uk] != j) continue;
                    const double area  = LU.area[uk] / ucf_len2;      // lid.c:551
                    const double width = LU.width[uk] / ucf_len;
                    const int    lid   = LU.lid_index[uk];
                    std::fprintf(f, "\n  %-16s %-16s",
                        ctx.subcatch_names.name_of(j).c_str(),
                        lid >= 0 ? ctx.lid_names.name_of(lid).c_str() : "");
                    std::fprintf(f, "%6d  %10.2f  %10.2f  %10.2f  %10.2f  %10.2f",
                        LU.number[uk], area * ucf_len2, width * ucf_len,
                        area * LU.number[uk] / sub_area * 100.0,
                        LU.from_imperv[uk] / 100.0 * 100.0,
                        LU.from_perv[uk] / 100.0 * 100.0);
                }
            }
            WRITE(f, "");
            WRITE(f, "");
        }
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

        // Which nodes carry an inflow, resolved in one pass over the inflow
        // rows instead of a scan of both row sets per node. The old form was
        // O(n_nodes x n_inflow_rows) — around 10^10 comparisons on a model
        // with 100k nodes and 100k inflows, all of it inside the report.
        std::vector<std::uint8_t> has_inflow(
            static_cast<std::size_t>(ctx.n_nodes()), 0u);
        auto mark = [&](const std::vector<int>& node_idx) {
            for (const int n : node_idx)
                if (n >= 0 && n < ctx.n_nodes())
                    has_inflow[static_cast<std::size_t>(n)] = 1u;
        };
        mark(ctx.ext_inflows.node_idx);
        mark(ctx.dwf_inflows.node_idx);
        mark(ctx.rdii_assigns.node_idx);   // legacy Node.rdiiInflow

        for (int i = 0; i < ctx.n_nodes(); ++i) {
            auto ui = static_cast<std::size_t>(i);
            int nt = static_cast<int>(ctx.nodes.type[ui]);
            std::fprintf(f, "\n  %-20s %-16s%10.2f%10.2f%10.1f",
                ctx.node_names.name_of(i).c_str(),
                nt_str(nt),
                ctx.nodes.invert_elev[ui] * du_opt.length,
                ctx.nodes.full_depth[ui] * du_opt.length,
                ctx.nodes.ponded_area[ui] * du_opt.length * du_opt.length);
            if (has_inflow[ui] != 0u) std::fprintf(f, "    Yes");
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
            // legacy lists the end nodes in their original orientation
            const int dir = ctx.links.direction[ui];
            if (dir < 0) std::swap(n1, n2);
            const char* n1_name = (n1 >= 0) ? ctx.node_names.name_of(n1).c_str() : "";
            const char* n2_name = (n2 >= 0) ? ctx.node_names.name_of(n2).c_str() : "";

            std::fprintf(f, "\n  %-16s %-16s %-16s ",
                ctx.link_names.name_of(i).c_str(), n1_name, n2_name);

            if (lt == static_cast<int>(LinkType::CONDUIT)) {
                const int cr = ctx.link_subtypes.conduit_row(i);
                const auto& CD = ctx.link_subtypes.conduits;
                // Legacy prints Conduit.roughness, which conduit_validate has
                // replaced by the transect's channel n for an IRREGULAR
                // conduit (link.c:1024); the stored value here stays authored.
                double n_rep = (cr >= 0) ? CD.roughness[static_cast<size_t>(cr)] : 0.01;
                if (cr >= 0 && ctx.links.xsect_shape[static_cast<size_t>(i)] == XsectShape::IRREGULAR) {
                    const int ti = ctx.links.xsect_curve[static_cast<size_t>(i)];
                    if (ti >= 0 && static_cast<size_t>(ti) < ctx.transects.n_channel.size() &&
                        ctx.transects.n_channel[static_cast<size_t>(ti)] > 0.0)
                        n_rep = ctx.transects.n_channel[static_cast<size_t>(ti)];
                }
                std::fprintf(f, "%-12s%10.1f%10.4f%10.4f",
                    "CONDUIT",
                    ((cr >= 0) ? CD.length[static_cast<size_t>(cr)] : 0.0) * du_opt.length,
                    ((cr >= 0) ? CD.slope[static_cast<size_t>(cr)] : 0.0) * 100.0 * dir,
                    n_rep);
            } else if (lt == static_cast<int>(LinkType::PUMP)) {
                // legacy "%-5s PUMP  " with PumpTypeWords; curve_type 1-5 is
                // TYPE1..TYPE5, anything else (6, or no curve) is IDEAL
                static const char* const kPumpWords[] =
                    {"TYPE1", "TYPE2", "TYPE3", "TYPE4", "TYPE5"};
                const int pr = ctx.link_subtypes.pump_row(i);
                const int ct = (pr >= 0) ? ctx.link_subtypes.pumps.curve_type[static_cast<size_t>(pr)] : -1;
                std::fprintf(f, "%-5s PUMP  ", (ct >= 1 && ct <= 5) ? kPumpWords[ct - 1] : "IDEAL");
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
    // legacy writes the heading whenever there are links, conduits or not
    if (ctx.n_links() > 0) {
        {
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
                // legacy names a CUSTOM / IRREGULAR / STREET section by its
                // curve, transect or street
                const auto xs = ctx.links.xsect_shape[ui];
                if ((xs == XsectShape::CUSTOM || xs == XsectShape::IRREGULAR ||
                     xs == XsectShape::STREET_XSECT) &&
                    !ctx.links.pump_curve_name[ui].empty())
                    shape_str = ctx.links.pump_curve_name[ui].c_str();

                const int cr = ctx.link_subtypes.conduit_row(i);
                const auto& CD = ctx.link_subtypes.conduits;
                std::fprintf(f, "\n  %-16s %-16s %8.2f %8.2f %8.2f %8.2f      %3d %8.2f",
                    ctx.link_names.name_of(i).c_str(),
                    shape_str,
                    ctx.links.xsect_y_full[ui] * du_opt.length,
                    ctx.links.xsect_a_full[ui] * du_opt.length * du_opt.length,
                    ctx.links.xsect_r_full[ui] * du_opt.length,
                    ctx.links.xsect_w_max[ui] * du_opt.length,
                    (cr >= 0) ? CD.barrels[static_cast<size_t>(cr)] : 1,
                    ((cr >= 0) ? CD.q_full[static_cast<size_t>(cr)] : 0.0) * Qcf_pre);
            }
            WRITE(f, "");
            WRITE(f, "");
        }
    }

    // =====================================================================
    // Shape / Transect / Street Summary — legacy inputrpt.c: the normalized
    // geometry tables, five values a row, entries 1..N-1.
    // =====================================================================
    {
        auto tables = [&](const char* kind, const std::string& name,
                          const transect::TransectData& td) {
            std::fprintf(f, "\n\n  %s %s", kind, name.c_str());
            const double* tbl[3] = {td.area_tbl, td.hrad_tbl, td.width_tbl};
            const char* lbl[3] = {"Area:  ", "Hrad:  ", "Width: "};
            for (int t = 0; t < 3; ++t) {
                std::fprintf(f, "\n  %s", lbl[t]);
                for (int m = 1; m < transect::N_TRANSECT_TBL; ++m) {
                    if (m % 5 == 1) std::fprintf(f, "\n          ");
                    std::fprintf(f, "%10.4f ", tbl[t][m]);
                }
            }
        };

        bool any_shape = false;
        for (const auto& t : ctx.tables.tables)
            if (t.type == TableType::CURVE_SHAPE) { any_shape = true; break; }
        if (any_shape) {
            WRITE(f, "*************");
            WRITE(f, "Shape Summary");
            WRITE(f, "*************");
            for (const auto& t : ctx.tables.tables) {
                if (t.type != TableType::CURVE_SHAPE) continue;
                transect::TransectData td;    // unit height, as shape.c builds it
                if (!t.x.empty())
                    transect::buildCustomTables(td, 1.0, t.x.data(), t.y.data(),
                                                static_cast<int>(t.x.size()));
                tables("Shape", t.id, td);
            }
            WRITE(f, "");
            WRITE(f, "");
        }

        const int nt = ctx.transects.count();
        if (nt > 0) {
            WRITE(f, "****************");
            WRITE(f, "Transect Summary");
            WRITE(f, "****************");
            for (int i = 0; i < nt && i < static_cast<int>(ctx.transect_tables.size()); ++i) {
                const auto& td = ctx.transect_tables[static_cast<std::size_t>(i)];
                tables("Transect", td.name, td);
            }
            WRITE(f, "");
            WRITE(f, "");
        }

        if (ctx.streets.count() > 0) {
            WRITE(f, "**************");
            WRITE(f, "Street Summary");
            WRITE(f, "**************");
            const int us = (fu >= 3) ? 1 : 0;
            const double ucf_len = ucf::Ucf[ucf::LENGTH][us];
            for (int s = 0; s < ctx.streets.count(); ++s) {
                const auto su = static_cast<std::size_t>(s);
                // as PostParseResolver builds it for a STREET cross section
                street::StreetParams sp;
                sp.width             = ctx.streets.t_crown[su]       / ucf_len;
                sp.curb_height       = ctx.streets.h_curb[su]        / ucf_len;
                sp.slope             = ctx.streets.sx[su]            / 100.0;
                sp.roughness         = ctx.streets.n_road[su];
                sp.gutter_depression = ctx.streets.gutter_depres[su] / ucf_len;
                sp.gutter_width      = ctx.streets.gutter_width[su]  / ucf_len;
                sp.sides             = ctx.streets.sides[su];
                sp.back_width        = ctx.streets.back_width[su]    / ucf_len;
                sp.back_slope        = ctx.streets.back_slope[su]    / 100.0;
                sp.back_roughness    = ctx.streets.back_n[su];
                transect::TransectData td;
                street::buildTransect(sp, td);
                tables("Street", ctx.streets.names[su], td);
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

    // Process Models — legacy report_writeOptions() prints NO when the ignore
    // flag is set OR the object class is empty (report.c:270-298).
    std::fprintf(f, "\n  Process Models:");
    // The object classes are legacy's Nobjects[GAGE/UNITHYD/SNOWMELT/AQUIFER]:
    // a declared object counts whether or not anything uses it.
    std::fprintf(f, "\n    Rainfall/Runoff ........ %s",
                 (ctx.n_gages() > 0 && !opt.ignore_rainfall) ? "YES" : "NO");
    std::fprintf(f, "\n    RDII ................... %s",
                 (ctx.unit_hyds.count() > 0 && !opt.ignore_rdii) ? "YES" : "NO");
    std::fprintf(f, "\n    Snowmelt ............... %s",
                 (ctx.snowpack_names.size() > 0 && !opt.ignore_snow_melt) ? "YES" : "NO");
    std::fprintf(f, "\n    Groundwater ............ %s",
                 (ctx.aquifer_names.size() > 0 && !opt.ignore_groundwater) ? "YES" : "NO");
    std::fprintf(f, "\n    Flow Routing ........... %s",
                 (ctx.n_links() > 0 && !opt.ignore_routing) ? "YES" : "NO");
    if (ctx.n_links() > 0) {
        std::fprintf(f, "\n    Ponding Allowed ........ %s",
                     opt.allow_ponding ? "YES" : "NO");
    }
    std::fprintf(f, "\n    Water Quality .......... %s",
                 (ctx.n_pollutants() > 0 && !opt.ignore_quality) ? "YES" : "NO");

    // E2 — the Domain x Species transport matrix (TransportPolicy), printed
    // only when the project uses a species class legacy does not have (water
    // age, heat, MSX), so a legacy-equivalent report is unchanged.
    if (ctx.options.water_age || ctx.options.heat_transport ||
        (ctx.reactions.configured && ctx.reactions.compiled)) {
        const auto matrix = openswmm::transport::resolve(ctx);
        std::fprintf(f, "\n%s", openswmm::transport::formatReportBlock(matrix).c_str());
    }

    if (ctx.n_subcatches() > 0) {
        int im = static_cast<int>(opt.infiltration);
        if (im < 0 || im > 4) im = 0;
        std::fprintf(f, "\n  Infiltration Method ...... %s", InfilModelWords[im]);
    }

    if (ctx.n_links() > 0) {
        int rm = static_cast<int>(opt.routing_model);
        const char* rm_name = (rm == 3) ? "FV"
                            : (rm == 2) ? "DYNWAVE"
                            : (rm == 1) ? "KINWAVE"
                                        : "STEADY";
        std::fprintf(f, "\n  Flow Routing Method ...... %s", rm_name);

        // TPA pressure closure (issue #156 Phase 4): FV, non-default only.
        if (rm == 3 && opt.fv.pressure_closure == 1) {
            std::fprintf(f, "\n  Pressure Closure ......... TPA");
        }

        // Unsteady friction (issue #156): applies to DW and FV alike.
        if ((rm == 2 || rm == 3) && opt.unsteady_friction != 0) {
            std::fprintf(f, "\n  Unsteady Friction ........ VITKOVSKY (k3 = %g)",
                         opt.uf_k3);
        }

    }

    // Legacy prints the surcharge method for DYNWAVE even with no links.
    if (static_cast<int>(opt.routing_model) == 2) { // DYNWAVE
        int sm = opt.surcharge_method;
        const char* sm_name = (sm >= 0 && sm <= 3) ? SurchargeWords[sm] : "EXTRAN";
        std::fprintf(f, "\n  Surcharge Method ......... %s", sm_name);
        // v6-only options, shown only when changed from the default.
        if (opt.node_continuity == NodeContinuity::SEMI_IMPLICIT)
            std::fprintf(f, "\n  Node Continuity .......... SEMI_IMPLICIT");
        if (opt.anderson_accel)
            std::fprintf(f, "\n  Anderson Acceleration .... YES");
    }

    dateToStr(opt.start_date, ds, sizeof(ds));
    timeToStr(opt.start_date, ts, sizeof(ts));
    std::fprintf(f, "\n  Starting Date ............ %s %s", ds, ts);

    dateToStr(opt.end_date, ds, sizeof(ds));
    timeToStr(opt.end_date, ts, sizeof(ts));
    std::fprintf(f, "\n  Ending Date .............. %s %s", ds, ts);

    std::fprintf(f, "\n  Antecedent Dry Days ...... %.1f", opt.dry_days);

    // Legacy formats these through datetime_encodeTime, which wraps at 24 h.
    secsToHMS(static_cast<int>(opt.report_step) % 86400, buf, sizeof(buf));
    std::fprintf(f, "\n  Report Time Step ......... %s", buf);

    if (ctx.n_subcatches() > 0) {
        secsToHMS(static_cast<int>(opt.wet_step) % 86400, buf, sizeof(buf));
        std::fprintf(f, "\n  Wet Time Step ............ %s", buf);
        secsToHMS(static_cast<int>(opt.dry_step) % 86400, buf, sizeof(buf));
        std::fprintf(f, "\n  Dry Time Step ............ %s", buf);
    }

    if (ctx.n_links() > 0) {
        std::fprintf(f, "\n  Routing Time Step ........ %.2f sec", opt.routing_step);

        if (static_cast<int>(opt.routing_model) == 2) { // DYNWAVE
            std::fprintf(f, "\n  Variable Time Step ....... %s",
                         opt.variable_step > 0.0 ? "YES" : "NO");
            // 0 = default; legacy dynwave_validate substitutes 8 before printing.
            std::fprintf(f, "\n  Maximum Trials ........... %d",
                         opt.max_trials > 0 ? opt.max_trials : 8);
            std::fprintf(f, "\n  Number of Threads ........ %d", opt.num_threads);
            std::fprintf(f, "\n  Head Tolerance ........... %f %s",
                         // authored in user units; the default is 0.005 ft
                         opt.head_tol == 0.0 ? 0.005 * du_opt.length : opt.head_tol,
                         du_opt.unit_system == 1 ? "m" : "ft");
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

    // Single source of truth for internal→display factors + unit words. The
    // local aliases below preserve the names used throughout this function and
    // mirror legacy statsrpt.c / report.c UCF()-based output exactly.
    const ucf::DisplayUnits du = ucf::DisplayUnits::from(opt);
    const bool   si_report = (du.unit_system == 1);
    const double Qcf       = du.flow;        // cfs → display flow
    const double land_vcf  = du.landvol;     // ft³ → acre-ft | hectare-m
    const double mvol_vcf  = du.mvol;        // ft³ → 10^6 gal | 10^6 ltr
    const double depth_vcf = du.raindepth;   // ft  → in | mm
    const double len_ucf   = du.length;      // ft  → ft | m  (depth/HGL/velocity)
    const double svol_ucf  = du.volume;      // ft³ → ft³ | m³ (storage volume)
    const char*  len_word  = du.length_word; // Feet | Meters
    // legacy statsrpt.c's own factor for every summary-table volume
    // (7.48 gal/ft3, 28.317 L/ft3 — not the massbal/continuity constants).
    const double Vcf       = si_report ? 28.317 / 1.0e6 : 7.48 / 1.0e6;
    const char*  vol_word  = du.mvol_word;    // 10^6 gal | 10^6 ltr
    // legacy report.c continuity factors: UCF(LENGTH) * UCF(LANDAREA) for
    // acre-feet / hectare-m, and MGDperCFS (MLDperCFS) / SECperDAY for the
    // 10^6 gal (ltr) column.
    const int    cont_us   = si_report ? 1 : 0;
    const double cont_vcf1 = ucf::Ucf[ucf::LENGTH][cont_us] * ucf::Ucf[ucf::LANDAREA][cont_us];
    const double cont_vcf2 = si_report ? 2.4466 / 86400.0 : 0.64632 / 86400.0;
    const double cont_mass = ucf::Ucf[ucf::MASS][cont_us];   // mg -> lb | kg
    const char*  load_word = si_report ? "kg" : "lbs";
    const char*  depth_word = du.depth_word;  // in | mm (rainfall/runoff depth)

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
            total_area_ft2 += ctx.subcatches.area[static_cast<std::size_t>(i)] * landAreaToFt2(fu);

        double sewer_rain = mb.runoff_rainfall;
        double rdii_prod = mb.routing_rdii;
        double ratio = (sewer_rain > 0.0) ? rdii_prod / sewer_rain : 0.0;
        double ucf1 = cont_vcf1;
        double ucf2 = cont_vcf2;

        std::fprintf(f, "\n  **********************           Volume        Volume");
        std::fprintf(f, si_report
            ? "\n  Rainfall Dependent I/I        hectare-m      10^6 ltr"
            : "\n  Rainfall Dependent I/I        acre-feet      10^6 gal");
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
    // legacy massbal_report: only with subcatchments of some area; printed
    // under CONTINUITY or when the error exceeds MAX_RUNOFF_BALANCE_ERR (10).
    {
        const auto& mb = ctx.mass_balance;
        double total_area_ft2 = 0.0;
        for (int i = 0; i < ctx.n_subcatches(); ++i)
            total_area_ft2 += ctx.subcatches.area[static_cast<std::size_t>(i)] * landAreaToFt2(fu);
        const double runoff_err_pct = mb.runoff_error() * 100.0;
        if (ctx.n_subcatches() > 0 && total_area_ft2 > 0.0 &&
            (opt.rpt_continuity || runoff_err_pct > 10.0)) {
        std::fprintf(f, "\n  **************************        Volume         Depth");
        std::fprintf(f, si_report
            ? "\n  Runoff Quantity Continuity     hectare-m            mm"
            : "\n  Runoff Quantity Continuity     acre-feet        inches");
        std::fprintf(f, "\n  **************************     ---------       -------");

        auto row = [&](const char* label, double vol_ft3) {
            double af = vol_ft3 * cont_vcf1;
            double depth_in = vol_ft3 / total_area_ft2 * depth_vcf;
            std::fprintf(f, "\n  %s%14.3f%14.3f", label, af, depth_in);
        };
        const bool has_snow = ctx.snowpack_names.size() > 0;

        if (mb.runoff_init_store > 0.0)
            row("Initial LID Storage ......", mb.runoff_init_store);
        if (has_snow)
            row("Initial Snow Cover .......", mb.runoff_init_snow);
        row("Total Precipitation ......", mb.runoff_rainfall);
        if (mb.runoff_runon > 0.0)
            row("Outfall Runon ............", mb.runoff_runon);
        row("Evaporation Loss .........", mb.runoff_evap);
        row("Infiltration Loss ........", mb.runoff_infil);
        row("Surface Runoff ...........", mb.runoff_runoff);
        if (mb.runoff_lid_drain > 0.0)
            row("LID Drainage .............", mb.runoff_lid_drain);
        if (has_snow) {
            row("Snow Removed .............", mb.runoff_snowremov);
            row("Final Snow Cover .........", mb.runoff_final_snow);
        }
        row("Final Storage ............", mb.runoff_final_store);

        std::fprintf(f, "\n  Continuity Error (%%) .....%14.3f", runoff_err_pct);
        WRITE(f, "");
        WRITE(f, "");
        }
    }

    // =====================================================================
    // Runoff Quality Continuity — matches legacy report_writeLoadingError():
    // up to five pollutants per block, side by side. Totals are user mass;
    // COUNT pollutants print LOG10 (legacy LOG10 macro: non-positive values
    // pass through). Error: |in - out| < 0.001 -> 0 (TINY), else relative
    // to the larger side's convention (massbal_getLoadingError).
    // =====================================================================
    if (ctx.n_subcatches() > 0 && ctx.n_pollutants() > 0 && !opt.ignore_quality) {
        const int np = ctx.n_pollutants();
        const auto& mb = ctx.mass_balance;
        auto at = [](const std::vector<double>& v, int p) {
            return (static_cast<std::size_t>(p) < v.size()) ? v[static_cast<std::size_t>(p)] : 0.0;
        };
        auto logn = [](double x) { return x > 0.0 ? std::log10(x) : x; };
        std::vector<std::array<double, 9>> vals(static_cast<std::size_t>(np));
        double max_err = 0.0;
        for (int p = 0; p < np; ++p) {
            const double in_  = at(mb.qual_init_buildup, p) + at(mb.qual_surface_buildup, p) +
                                at(mb.qual_wet_deposition, p);
            const double out_ = at(mb.qual_sweeping, p) + at(mb.qual_infil_loss, p) +
                                at(mb.qual_bmp_removal, p) + at(mb.qual_runoff_load, p) +
                                at(mb.qual_final_buildup, p);
            double err = 0.0;
            if (std::fabs(in_ - out_) < 0.001) err = 0.0;
            else if (in_ > 0.0)  err = 100.0 * (1.0 - out_ / in_);
            else if (out_ > 0.0) err = 100.0 * (in_ / out_ - 1.0);
            max_err = std::max(max_err, err);
            auto& v = vals[static_cast<std::size_t>(p)];
            v = { at(mb.qual_init_buildup, p), at(mb.qual_surface_buildup, p),
                  at(mb.qual_wet_deposition, p), at(mb.qual_sweeping, p),
                  at(mb.qual_infil_loss, p), at(mb.qual_bmp_removal, p),
                  at(mb.qual_runoff_load, p), at(mb.qual_final_buildup, p), err };
            const bool counts = static_cast<std::size_t>(p) < ctx.pollutants.units.size() &&
                                ctx.pollutants.units[static_cast<std::size_t>(p)] == MassUnits::COUNTS_PER_L;
            if (counts) for (int k = 0; k < 8; ++k) v[static_cast<std::size_t>(k)] = logn(v[static_cast<std::size_t>(k)]);
        }
        if (opt.rpt_continuity || max_err > 10.0) {
            static const char* labels[9] = {
                "Initial Buildup ..........", "Surface Buildup ..........",
                "Wet Deposition ...........", "Sweeping Removal .........",
                "Infiltration Loss ........", "BMP Removal ..............",
                "Surface Runoff ...........", "Remaining Buildup ........",
                "Continuity Error (%) ....." };
            for (int p1 = 0; p1 < np; p1 += 5) {
                const int p2 = std::min(p1 + 5, np);
                std::fprintf(f, "\n  **************************");
                for (int p = p1; p < p2; ++p)
                    std::fprintf(f, "%14s", ctx.pollutant_names.name_of(p).c_str());
                std::fprintf(f, "\n  Runoff Quality Continuity ");
                for (int p = p1; p < p2; ++p) {
                    const bool counts = static_cast<std::size_t>(p) < ctx.pollutants.units.size() &&
                        ctx.pollutants.units[static_cast<std::size_t>(p)] == MassUnits::COUNTS_PER_L;
                    std::fprintf(f, "%14s", counts ? "LogN" : load_word);
                }
                std::fprintf(f, "\n  **************************");
                for (int p = p1; p < p2; ++p) std::fprintf(f, "    ----------");
                for (int k = 0; k < 9; ++k) {
                    std::fprintf(f, "\n  %s", labels[k]);
                    for (int p = p1; p < p2; ++p)
                        std::fprintf(f, "%14.3f", vals[static_cast<std::size_t>(p)][static_cast<std::size_t>(k)]);
                }
                WRITE(f, "");
                WRITE(f, "");
            }
        }
    }

    // =====================================================================
    // Groundwater Continuity — matches legacy report_writeGwaterError()
    // =====================================================================
    if (has_gw && opt.rpt_continuity && !opt.ignore_groundwater) {
        const auto& mb = ctx.mass_balance;

        // Compute total GW area (ft²) for depth conversion
        double gw_area_ft2 = 0.0;
        for (int i = 0; i < ctx.n_subcatches(); ++i) {
            auto ui = static_cast<std::size_t>(i);
            if (ctx.subcatches.gw_aquifer[ui] >= 0)
                gw_area_ft2 += ctx.subcatches.area[ui] * landAreaToFt2(fu);
        }
        if (gw_area_ft2 <= 0.0) gw_area_ft2 = 1.0;

        // Volume conversion: ft³ → acre-ft (multiply by UCF(LANDAREA) for US)
        // Legacy: totals->x * UCF(LENGTH) * UCF(LANDAREA) — for US = x * 1.0 * 2.2957e-5
        double ucf_vol = cont_vcf1;  // ft³ → acre-ft (US) | hectare-m (SI)
        // Depth conversion: ft³ / gwArea → ft → inches (US) | mm (SI)
        double ucf_dep = depth_vcf;

        std::fprintf(f, "\n  **************************        Volume         Depth");
        std::fprintf(f, si_report
            ? "\n  Groundwater Continuity         hectare-m            mm"
            : "\n  Groundwater Continuity         acre-feet        inches");
        std::fprintf(f, "\n  **************************     ---------       -------");

        auto gwRow = [&](const char* label, double vol_ft3) {
            std::fprintf(f, "\n  %s%14.3f%14.3f",
                label, vol_ft3 * ucf_vol,
                vol_ft3 / gw_area_ft2 * ucf_dep);
        };

        gwRow("Initial Storage ..........", mb.gw_init_storage);
        gwRow("Infiltration .............", mb.gw_infil);
        // U3 (track I-b): the 2D surface's share of that infiltration, named
        // so a user can see the cross-domain transfer. Printed only when a
        // deck actually routes it (INFIL_DESTINATION SUBCATCH_AQUIFER); it
        // is INSIDE the Infiltration row above, not a second inflow.
        if (mb.gw_infil_2d_recharge != 0.0)
            gwRow("  of which from 2D .......", mb.gw_infil_2d_recharge);
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
    // (report.c:288: suppressed when routing is ignored).
    // =====================================================================
    // legacy massbal_report: nodes present and routing on; printed under
    // CONTINUITY or when the error exceeds MAX_FLOW_BALANCE_ERR (10).
    if (ctx.n_nodes() > 0 && !opt.ignore_routing &&
        (opt.rpt_continuity || ctx.mass_balance.routing_error() * 100.0 > 10.0)) {
        const auto& mb = ctx.mass_balance;
        std::fprintf(f, "\n  **************************        Volume        Volume");
        std::fprintf(f, si_report
            ? "\n  Flow Routing Continuity        hectare-m      10^6 ltr"
            : "\n  Flow Routing Continuity        acre-feet      10^6 gal");
        std::fprintf(f, "\n  **************************     ---------     ---------");

        double ucf1 = cont_vcf1;
        double ucf2 = cont_vcf2;

        auto row = [&](const char* label, double vol_ft3) {
            std::fprintf(f, "\n  %s%14.3f%14.3f", label, vol_ft3 * ucf1, vol_ft3 * ucf2);
        };

        // The printed terms are legacy's half-step totals (routing_report).
        const auto& rr = mb.routing_report;
        row("Dry Weather Inflow .......", rr[0]);
        row("Wet Weather Inflow .......", rr[1]);
        row("Groundwater Inflow .......", rr[2]);
        // G-X4: a gaining reach — the two-zone aquifer feeding a conduit that
        // runs below the water table. Printed only when non-zero, so every
        // deck without the signed conduit exchange keeps its report
        // line-for-line.
        if (rr[3] != 0.0)
            row("Conduit GW Inflow ........", rr[3]);
        row("RDII Inflow ..............", rr[4]);
        row("External Inflow ..........", rr[5]);
        row("External Outflow .........", rr[8]);
        row("Flooding Loss ............", rr[6]);
        // C2: the 1D→2D coupling spill, split out of Flooding Loss. Printed
        // only when non-zero so an uncoupled model's report is unchanged
        // line-for-line. (COUPLING_IN_FLOODING YES puts it back in the row
        // above and leaves this one at zero, hence hidden.)
        if (rr[7] != 0.0)
            row("2D Coupling Outflow ......", rr[7]);
        row("Evaporation Loss .........", rr[9]);
        row("Exfiltration Loss ........", rr[10]);
        row("Initial Stored Volume ....", mb.routing_init_storage);
        row("Final Stored Volume ......", mb.routing_final_storage);

        // FV only (slot_volume stays 0.0 under DW): the share of Final
        // Stored Volume standing in the Preissmann slot. Informational —
        // already inside Final Stored Volume, never added again.
        {
            double slot_ft3 = 0.0;
            for (double sv : ctx.links.slot_volume) slot_ft3 += sv;
            if (slot_ft3 > 0.0)
                row("Final Slot Storage .......", slot_ft3);
        }

        // legacy massbal_getFlowError: signed terms move sides; |in - out|
        // under 1 ft3 reads 0 (TINY).
        {
            double tin  = mb.routing_init_storage + rr[1] + rr[4] + rr[3];
            double tout = mb.routing_final_storage + rr[6] + rr[7] + rr[9] + rr[10];
            if (rr[0] >= 0.0) tin += rr[0]; else tout -= rr[0];
            if (rr[2] >= 0.0) tin += rr[2]; else tout -= rr[2];
            if (rr[5] >= 0.0) tin += rr[5]; else tout -= rr[5];
            if (rr[8] >= 0.0) tout += rr[8]; else tin -= rr[8];
            double pct = 0.0;
            if (std::fabs(tin - tout) < 1.0)   pct = 0.0;
            else if (std::fabs(tin) > 0.0)     pct = 100.0 * (1.0 - tout / tin);
            else if (std::fabs(tout) > 0.0)    pct = 100.0 * (tin / tout - 1.0);
            std::fprintf(f, "\n  Continuity Error (%%) .....%14.3f", pct);
        }
    }

    // =====================================================================
    // 2D Surface Routing Continuity — system mass balance for the optional
    // 2D overland-flow domain. Volumes are SI (m³), a separate balance from
    // the 1D routing block above (coupling terms are signed oppositely).
    // =====================================================================
    if (opt.rpt_continuity && ctx.mass_balance_2d.active) {
        const auto& mb2 = ctx.mass_balance_2d;

        WRITE(f, "");
        WRITE(f, "");
        std::fprintf(f, "\n  **************************        Volume        Volume");
        std::fprintf(f, "\n  2D Surface Routing Continuity  cubic meters      10^6 ltr");
        std::fprintf(f, "\n  **************************     ---------     ---------");

        auto row2 = [&](const char* label, double vol_m3) {
            std::fprintf(f, "\n  %s%14.3f%14.3f", label, vol_m3, vol_m3 * 1.0e-3);
        };

        row2("Initial Stored Volume ....", mb2.init_storage);
        row2("Rainfall Inflow ..........", mb2.rainfall_in);
        row2("1D -> 2D Spill Inflow ....", mb2.coupling_1d_to_2d_in);
        row2("Outfall Inflow ...........", mb2.outfall_in);
        row2("Boundary Inflow ..........", mb2.boundary_in);
        row2("2D -> 1D Drain Outflow ...", mb2.coupling_2d_to_1d_out);
        row2("Outfall Withdrawal .......", mb2.outfall_out);
        row2("Boundary Outflow .........", mb2.boundary_out);
        row2("Evaporation Loss .........", mb2.evap_out);
        row2("Infiltration Loss ........", mb2.infil_out);
        // G1-c item 1: the SUBCATCH_AQUIFER share — delivered to the legacy
        // aquifer (the same number the Groundwater Continuity block prints as
        // "of which from 2D", in ft³) and the tail still in flight at the end.
        if (mb2.infil_to_aquifer > 0.0 || mb2.infil_aquifer_pending > 0.0) {
            row2("  to Aquifer (delivered) .", mb2.infil_to_aquifer);
            row2("  to Aquifer (in flight) .", mb2.infil_aquifer_pending);
        }
        row2("Final Stored Volume ......", mb2.final_storage);

        std::fprintf(f, "\n  Continuity Error (%%) .....%14.3f",
                     mb2.error() * 100.0);

#ifdef OPENSWMM_HAS_2D
        // G-O: the two-zone [2D_AQUIFER] ledger, the same terms the .h5
        // /groundwater_2d group and groundwater_ledger series carry. Recharge
        // and capillary rise are internal (unsaturated <-> saturated zone of
        // the same cell) and are listed for information, not in the balance.
        if (ctx.twod_io.aquifer_state && ctx.twod_io.aquifer_state->active) {
            const auto& g = *ctx.twod_io.aquifer_state;
            WRITE(f, "");
            WRITE(f, "");
            std::fprintf(f, "\n  **************************        Volume        Volume");
            std::fprintf(f, "\n  2D Aquifer Continuity          cubic meters      10^6 ltr");
            std::fprintf(f, "\n  **************************     ---------     ---------");
            row2("Initial Stored Volume ....", g.led_init_storage);
            row2("Infiltration Inflow ......", g.led_infil_in);
            row2("Conduit Seepage Inflow ...", g.led_link);   // G-X3
            row2("Lateral Net Inflow .......", g.led_lateral);
            row2("Deep Percolation .........", g.led_deep);
            row2("Node Exchange Outflow ....", g.led_node);
            row2("Subsurface ET ............", g.led_et);
            row2("Saturation Excess Return .", g.led_dunne);
            row2("Final Stored Volume ......", g.liveStorage());
            row2("  (Recharge, internal) ...", g.led_recharge);
            row2("  (Capillary Rise, int.) .", g.led_caprise);
            const double denom = g.led_init_storage + g.led_infil_in + g.led_link +   // G-X3
                                 std::max(0.0, g.led_lateral);
            std::fprintf(f, "\n  Continuity Error (%%) .....%14.3f",
                         (denom > 0.0) ? g.continuityResidual() / denom * 100.0 : 0.0);
        }

        // T7.5: the aquifer's SPECIES continuity, one block per transported
        // row — the quality twin of the water block above, and the only
        // place a modeller sees where a plume went. Printed only when the
        // kernel actually carried a tuple, so a water-only aquifer deck's
        // report is unchanged line-for-line.
        if (ctx.twod_io.aquifer_transport != nullptr &&
            ctx.twod_io.aquifer_transport->active()) {
            const auto& t = *ctx.twod_io.aquifer_transport;
            for (int sp = 0; sp < t.n_species; ++sp) {
                const auto u = static_cast<std::size_t>(sp);
                const std::string& nm =
                    (u < t.row_names.size()) ? t.row_names[u] : std::string("?");
                WRITE(f, "");
                WRITE(f, "");
                std::fprintf(f, "\n  **************************%14s", nm.c_str());
                std::fprintf(f, "\n  2D Aquifer Quality Continuity%11s", "mass");
                std::fprintf(f, "\n  **************************    ----------");
                auto qrow = [&](const char* label, double v) {
                    std::fprintf(f, "\n  %s%14.4f", label, v);
                };
                qrow("Initial Stored Mass ......", t.init_mass[u]);
                qrow("Infiltration Inflow ......", t.gained_infil[u]);
                if (t.gained_node[u] != 0.0)
                    qrow("Node Exchange Inflow .....", t.gained_node[u]);
                if (t.gained_link[u] != 0.0)
                    qrow("Conduit Seepage Inflow ...", t.gained_link[u]);
                qrow("Lateral Net Inflow .......", t.net_lateral[u]);
                qrow("Deep Percolation .........", t.lost_deep[u]);
                qrow("Node Exchange Outflow ....", t.lost_node[u]);
                if (t.lost_link[u] != 0.0)
                    qrow("Conduit Seepage Outflow ..", t.lost_link[u]);
                qrow("Saturation Excess Return .", t.lost_dunne[u]);
                qrow("Evapotranspiration .......", t.lost_et[u]);
                if (t.lost_reaction[u] != 0.0)
                    qrow("Reacted / Decayed ........", t.lost_reaction[u]);
                qrow("Final Stored Mass ........", t.ledgeredStorage(sp));
                // The denominator is the scale the residual is judged
                // against: what the aquifer started with plus every route
                // mass can arrive by.
                //
                // T7.4 (2026-09-21): the sum lives on the state, beside the
                // residual it scales, so a new channel cannot be added to one
                // and forgotten in the other — which is exactly what happened
                // when T7.4 added the node and link seams and this block's
                // own copy kept dividing by three terms.
                //
                // T7.4b (2026-09-21): when the scale is zero there is no
                // percentage to print. This used to print 0.000, which claims
                // a perfect balance where the truth is that the question is
                // unanswerable — and with the denominator now naming every
                // route, a zero scale beside a NON-zero residual is precisely
                // the signature of a route nobody has booked yet. Printing
                // 0 % there would hide the next instance of the bug this
                // denominator was fixed for, so it prints `n/a` instead.
                const double denom = t.continuityDenominator(sp);
                if (denom > 0.0)
                    std::fprintf(f, "\n  Continuity Error (%%) .....%14.3f",
                                 t.residual(sp) / denom * 100.0);
                else
                    std::fprintf(f, "\n  Continuity Error (%%) .....%14s",
                                 "n/a");
            }
        }

#endif

        // 2D Solver Statistics — cumulative marcher throughput. Printed only
        // when populated (>=0).
        if (mb2.solver_nsteps >= 0) {
            WRITE(f, "");
            WRITE(f, "");
            std::fprintf(f, "\n  *************************");
            std::fprintf(f, "\n  2D Solver Statistics");
            std::fprintf(f, "\n  *************************");
            auto srow = [&](const char* label, long v) {
                std::fprintf(f, "\n  %s%14ld", label, v);
            };
            srow("Internal Steps ...........", mb2.solver_nsteps);
            srow("Face-Kernel Evals ........", mb2.solver_nrhs);
            std::fprintf(f, "\n  Avg Internal Step (s) ....%14.4f", mb2.solver_avg_h);
            std::fprintf(f, "\n  Last Internal Step (s) ...%14.4f", mb2.solver_last_h);

            // Marcher-only telemetry (EXPLICIT integrator): active-fraction
            // spread over rebuild samples + LTS tier-occupancy shares.
            if (mb2.solver_active_mean >= 0.0) {
                std::fprintf(f,
                    "\n  Active Cells min/mean/max %8.1f /%5.1f /%5.1f  (%%)",
                    100.0 * mb2.solver_active_min,
                    100.0 * mb2.solver_active_mean,
                    100.0 * mb2.solver_active_max);
            }
            if (mb2.solver_n_tiers > 0) {
                long total = 0;
                for (int k = 0; k < mb2.solver_n_tiers; ++k)
                    total += mb2.solver_tier_cells[k];
                if (total > 0) {
                    for (int k = 0; k < mb2.solver_n_tiers; ++k)
                        std::fprintf(f,
                            "\n  LTS Tier %d Occupancy (%%) .%14.1f", k,
                            100.0 * static_cast<double>(mb2.solver_tier_cells[k])
                                  / static_cast<double>(total));
                }
            }
        }

        // 1D <-> 2D Exchange Reconciliation — the coupled-loop ledger books
        // spill as Flooding Loss and drain as external inflow, so the plain
        // flow-routing continuity above double-counts exchanged water as both
        // a loss and a new inflow. Recompute it here with the exchange treated
        // as an internal transfer between the two domains (report-only; the
        // underlying ledgers are untouched).
        if (mb2.coupling_1d_to_2d_in > 0.0 || mb2.coupling_2d_to_1d_out > 0.0) {
            const auto& mb1 = ctx.mass_balance;
            // 2D ledger volumes are SI m³; the 1D balance is internal ft³
            // regardless of flow units (legacy parity; land_vcf/mvol_vcf
            // convert ft³ → report units), so convert the exchange to ft³.
            const double m3_to_1d = 1.0 / 0.028316846592;
            const double spill_1d = mb2.coupling_1d_to_2d_in  * m3_to_1d;
            const double drain_1d = mb2.coupling_2d_to_1d_out * m3_to_1d;

            WRITE(f, "");
            WRITE(f, "");
            std::fprintf(f, "\n  **************************        Volume        Volume");
            std::fprintf(f, si_report
                ? "\n  1D <-> 2D Exchange Reconcil.   hectare-m      10^6 ltr"
                : "\n  1D <-> 2D Exchange Reconcil.   acre-feet      10^6 gal");
            std::fprintf(f, "\n  **************************     ---------     ---------");
            auto rowx = [&](const char* label, double vol_1d) {
                std::fprintf(f, "\n  %s%14.3f%14.3f", label,
                             vol_1d * land_vcf, vol_1d * mvol_vcf);
            };
            rowx("1D -> 2D Spill ...........", spill_1d);
            rowx("2D -> 1D Drain ...........", drain_1d);
            rowx("Net 1D -> 2D .............", spill_1d - drain_1d);

            // Flow-routing continuity with the exchange internal: remove the
            // drain from external inflow and the spill from whichever outflow
            // category carries it.
            //
            // C2: the spill now lives in routing_coupling_out by default, and
            // only in routing_flooding under COUPLING_IN_FLOODING. Subtracting
            // it from the SUM of the two is correct in both groupings and
            // needs no branch — the category it is not in contributes zero.
            const double in_adj  = mb1.routing_dry_weather + mb1.routing_wet_weather
                                 + mb1.routing_gw_inflow
                                 + mb1.routing_link_gw_inflow   // G-X4
                                 + mb1.routing_rdii
                                 + (mb1.routing_external - drain_1d)
                                 + mb1.routing_init_storage;
            const double out_adj = (mb1.routing_flooding
                                    + mb1.routing_coupling_out - spill_1d)
                                 + mb1.routing_outflow + mb1.routing_evap_loss
                                 + mb1.routing_seep_loss + mb1.routing_final_storage;
            const double err_adj = (in_adj > 0.0)
                                 ? (in_adj - out_adj) / in_adj : 0.0;
            std::fprintf(f,
                "\n  Flow Continuity w/ Exchange Internal (%%) %8.3f",
                err_adj * 100.0);
        }
    }

    WRITE(f, "");
    WRITE(f, "");

    // =====================================================================
    // Quality Routing Continuity — matches legacy report_writeQualError():
    // five pollutants per block. Raw totals are concentration x ft3; the
    // error is taken on them (massbal_getQualError) before conversion:
    // x LperFT3 x UCF(MASS) (/1000 for ug), or LOG10(LperFT3 x) for counts.
    // =====================================================================
    if (ctx.n_nodes() > 0 && !opt.ignore_routing &&
        ctx.n_pollutants() > 0 && !opt.ignore_quality) {
        const int np = ctx.n_pollutants();
        const auto& mb = ctx.mass_balance;
        static constexpr double LperFT3 = 28.317;
        auto at = [](const std::vector<double>& v, int p) {
            return (static_cast<std::size_t>(p) < v.size()) ? v[static_cast<std::size_t>(p)] : 0.0;
        };
        std::vector<std::array<double, 12>> vals(static_cast<std::size_t>(np));
        double max_err = 0.0;
        for (int p = 0; p < np; ++p) {
            std::array<double, 11> raw = {
                at(mb.qual_routing_dw_in, p), at(mb.qual_routing_wet, p),
                at(mb.qual_routing_gw_in, p), at(mb.qual_routing_ii_in, p),
                at(mb.qual_routing_ex_in, p), at(mb.qual_routing_outflow, p),
                at(mb.qual_routing_flood, p), at(mb.qual_routing_seep, p),
                at(mb.qual_routing_reacted, p), at(mb.qual_routing_init, p),
                at(mb.qual_routing_final, p) };
            const double in_  = raw[0] + raw[1] + raw[2] + raw[3] + raw[4] + raw[9];
            const double out_ = raw[6] + raw[5] + raw[8] + raw[7] + raw[10];
            double err = 0.0;
            if (std::fabs(in_ - out_) < 0.001) err = 0.0;
            else if (in_ > 0.0)  err = 100.0 * (1.0 - out_ / in_);
            else if (out_ > 0.0) err = 100.0 * (in_ / out_ - 1.0);
            if (std::fabs(err) > std::fabs(max_err)) max_err = err;
            const MassUnits mu = (static_cast<std::size_t>(p) < ctx.pollutants.units.size())
                ? ctx.pollutants.units[static_cast<std::size_t>(p)] : MassUnits::MG_PER_L;
            auto& v = vals[static_cast<std::size_t>(p)];
            for (int k = 0; k < 11; ++k) {
                const double x = raw[static_cast<std::size_t>(k)];
                double y;
                if (mu == MassUnits::COUNTS_PER_L) {
                    const double z = LperFT3 * x;
                    y = z > 0.0 ? std::log10(z) : z;
                } else {
                    double cf = LperFT3 * cont_mass;
                    if (mu == MassUnits::UG_PER_L) cf /= 1000.0;
                    y = x * cf;
                }
                v[static_cast<std::size_t>(k)] = y;
            }
            v[11] = err;
        }
        if (opt.rpt_continuity || max_err > 10.0) {
            static const char* labels[12] = {
                "Dry Weather Inflow .......", "Wet Weather Inflow .......",
                "Groundwater Inflow .......", "RDII Inflow ..............",
                "External Inflow ..........", "External Outflow .........",
                "Flooding Loss ............", "Exfiltration Loss ........",
                "Mass Reacted .............", "Initial Stored Mass ......",
                "Final Stored Mass ........", "Continuity Error (%) ....." };
            for (int p1 = 0; p1 < np; p1 += 5) {
                const int p2 = std::min(p1 + 5, np);
                std::fprintf(f, "\n  **************************");
                for (int p = p1; p < p2; ++p)
                    std::fprintf(f, "%14s", ctx.pollutant_names.name_of(p).c_str());
                std::fprintf(f, "\n  Quality Routing Continuity");
                for (int p = p1; p < p2; ++p) {
                    const bool counts = static_cast<std::size_t>(p) < ctx.pollutants.units.size() &&
                        ctx.pollutants.units[static_cast<std::size_t>(p)] == MassUnits::COUNTS_PER_L;
                    std::fprintf(f, "%14s", counts ? "LogN" : load_word);
                }
                std::fprintf(f, "\n  **************************");
                for (int p = p1; p < p2; ++p) std::fprintf(f, "    ----------");
                for (int k = 0; k < 12; ++k) {
                    std::fprintf(f, "\n  %s", labels[k]);
                    for (int p = p1; p < p2; ++p)
                        std::fprintf(f, "%14.3f", vals[static_cast<std::size_t>(p)][static_cast<std::size_t>(k)]);
                }
                WRITE(f, "");
                WRITE(f, "");
            }
        }
    }

    // =====================================================================
    // Highest Continuity Errors — matches legacy report_writeMaxStats()
    // =====================================================================
    // legacy stats_findMaxStats + report_writeMaxStats: dynamic wave with
    // links, under FLOWSTATS. Nodes with links and more than 0.1 ft3 of
    // inflow; error = 1 - outflow/inflow (outflow includes final storage);
    // the five largest by magnitude above 1 % (the slots start at -1.0),
    // earlier nodes winning ties. Nothing qualifies -> no section.
    const bool dw_like = opt.routing_model == RoutingModel::DYNWAVE ||
                         opt.routing_model == RoutingModel::FV;
    if (opt.rpt_flowstats && dw_like && ctx.n_links() > 0) {
        struct NodeError { int idx; double error; };
        std::vector<NodeError> errors;
        for (int j = 0; j < ctx.n_nodes(); ++j) {
            auto uj = static_cast<std::size_t>(j);
            if (ctx.nodes.degree[uj] <= 0) continue;
            const double in_vol = ctx.nodes.stat_total_inflow_vol[uj];
            if (in_vol <= 0.1) continue;
            const double out_vol = ctx.nodes.stat_total_outflow_vol[uj] +
                                   ctx.nodes.volume[uj];
            const double x = 100.0 * (1.0 - out_vol / in_vol);
            if (std::fabs(x) > 1.0) errors.push_back({j, x});
        }
        std::stable_sort(errors.begin(), errors.end(),
            [](const NodeError& a, const NodeError& b) {
                return std::fabs(a.error) > std::fabs(b.error);
            });
        if (!errors.empty()) {
            WRITE(f, "*************************");
            WRITE(f, "Highest Continuity Errors");
            WRITE(f, "*************************");
            const int count = std::min(5, static_cast<int>(errors.size()));
            for (int i = 0; i < count; ++i)
                std::fprintf(f, "\n  Node %s (%.2f%%)",
                    ctx.node_names.name_of(errors[i].idx).c_str(),
                    errors[i].error);
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
    // legacy report_writeMaxStats: dynamic wave with links, variable step.
    if (opt.routing_model == RoutingModel::DYNWAVE && ctx.n_links() > 0 &&
        opt.variable_step != 0.0) {
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
    }

    WRITE(f, "");
    WRITE(f, "");

    // =====================================================================
    // Highest Flow Instability Indexes
    // =====================================================================
    // legacy report_writeMaxFlowTurns; a top entry at index 0 reads as
    // "stable" there (index <= 0) and is reproduced.
    if (ctx.n_links() > 0) {
    WRITE(f, "********************************");
    WRITE(f, "Highest Flow Instability Indexes");
    WRITE(f, "********************************");
    if (ctx.max_flow_turns[0].index <= 0) {
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
    // legacy report_writeNonconvergedStats: dynamic wave only; index <= 0
    // reads as converged there, reproduced.
    if (ctx.n_nodes() > 0 && opt.routing_model == RoutingModel::DYNWAVE) {
    WRITE(f, "*********************************");
    WRITE(f, "Most Frequent Nonconverging Nodes");
    WRITE(f, "*********************************");
    {
        if (ctx.max_non_converged[0].index <= 0 ||
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
    }

    WRITE(f, "");
    WRITE(f, "");

    // =====================================================================
    // Routing Time Step Summary — matches legacy report.c
    // =====================================================================
    // legacy report_writeTimeStepStats: nothing without links or steps.
    if (ctx.n_links() > 0 && ctx.routing_stats.n_steps > 0) {
    WRITE(f, "*************************");
    WRITE(f, "Routing Time Step Summary");
    WRITE(f, "*************************");
    {
        const auto& rs = ctx.routing_stats;
        const double t_total = rs.steady_time + rs.sum_step;
        const double steady_pct =
            std::min(t_total > 0.0 ? 100.0 * rs.steady_time / t_total : 0.0, 100.0);
        if (rs.n_steps > 0) {
            std::fprintf(f, "\n  Minimum Time Step           :  %7.2f sec",
                         rs.min_step < 1.0e30 ? rs.min_step : 0.0);
            std::fprintf(f, "\n  Average Time Step           :  %7.2f sec",
                         rs.avg_step());
            std::fprintf(f, "\n  Maximum Time Step           :  %7.2f sec",
                         rs.max_step);
            std::fprintf(f, "\n  %% of Time in Steady State   :  %7.2f",
                         steady_pct);
            // FV has no Picard loop, so what the counter holds is the number
            // of explicit SUBSTEPS the step was filled with. Printing that
            // under the iteration label reads as catastrophic non-convergence
            // when the value runs to the hundreds — it is the opposite, a
            // scheme that never iterates. Both lines are relabelled rather
            // than dropped: the substep count is the useful number here.
            const bool is_fv =
                (opt.routing_model == RoutingModel::FV);
            if (is_fv) {
                std::fprintf(f, "\n  Average Substeps per Step   :  %7.2f",
                             rs.computed_avg_iterations());
                std::fprintf(f, "\n  %% of Steps Not Converging   :      n/a");
            } else {
                std::fprintf(f, "\n  Average Iterations per Step :  %7.2f",
                             rs.computed_avg_iterations());
                std::fprintf(f, "\n  %% of Steps Not Converging   :  %7.2f",
                             rs.pct_non_converged());
            }

            // Time step frequency table
            // Build histogram if not already built
            // (bins built at end of simulation in engine)
            // legacy report_RouteStepFreq: variable-step dynamic wave only,
            // as shares of the binned steps.
            long binned = 0;
            for (int i = 0; i < rs.N_TIME_BINS; ++i) binned += rs.step_counts[i];
            if (opt.routing_model == RoutingModel::DYNWAVE &&
                opt.variable_step > 0.0 && binned > 0) {
                std::fprintf(f, "\n  Time Step Frequencies       :");
                for (int i = 0; i < rs.N_TIME_BINS; ++i) {
                    double pct = 100.0 * static_cast<double>(rs.step_counts[i])
                                 / static_cast<double>(binned);
                    std::fprintf(f, "\n     %6.3f - %6.3f sec      :  %7.2f %%",
                        rs.step_intervals[i],
                        rs.step_intervals[i+1],
                        pct);
                }
            }
        }
    }
    }  // links and counted steps

    // =====================================================================
    // FV Solver Statistics — cumulative explicit-integrator throughput, the
    // 1D counterpart of the "2D Solver Statistics" block above. The Routing
    // Time Step Summary reports the ROUTING step; this reports the substeps
    // the FV solver filled it with, which is the number that actually sets
    // the run time.
    //
    // Already inside the rpt_flowstats guard opened well above (the braces
    // that close just before this comment are the anonymous scope holding the
    // Routing Time Step Summary's `rs`, not the conditional) — so the FV block
    // follows FLOWSTATS, whereas the 2D block above follows CONTINUITY. The
    // two solver blocks answer to different report flags; that is inherited,
    // not chosen here.
    //
    // Skipped on the -1 sentinel, which means "not an FV run".
    // =====================================================================
    if (ctx.routing_stats.fv_nsteps >= 0) {
        const auto& rs = ctx.routing_stats;
        WRITE(f, "");
        WRITE(f, "");
        WRITE(f, "*********************");
        WRITE(f, "FV Solver Statistics");
        WRITE(f, "*********************");
        auto srow = [&](const char* label, long v) {
            std::fprintf(f, "\n  %s%14ld", label, v);
        };
        srow("Explicit Substeps ........", rs.fv_nsteps);
        srow("Face Flux Evaluations ....", rs.fv_nflux);
        std::fprintf(f, "\n  Avg Substep (s) ..........%14.6f", rs.fv_avg_h);
        std::fprintf(f, "\n  Min Substep (s) ..........%14.6f", rs.fv_min_h);
        std::fprintf(f, "\n  Last Substep (s) .........%14.6f", rs.fv_last_h);
        // Whether local time stepping ever ran a macro cycle. The tier
        // occupancy rows below are filled by the tier ASSIGNMENT, which runs
        // whether or not a cycle fits the routing step, so on their own they
        // cannot distinguish "tiering did not help" from "tiering never
        // engaged" — these two rows can.
        srow("LTS Macro Cycles Fired ...", rs.fv_macro_cycles);
        srow("LTS Macro Cycles Rejected ", rs.fv_macro_rejected);

        // Compaction telemetry: the share of faces on the active list at each
        // rebuild. A mean near 1.0 means compaction is finding nothing to skip.
        if (rs.fv_active_mean >= 0.0) {
            std::fprintf(f,
                "\n  Active Faces min/mean/max %8.1f /%5.1f /%5.1f  (%%)",
                100.0 * rs.fv_active_min,
                100.0 * rs.fv_active_mean,
                100.0 * rs.fv_active_max);
        }

        // dt-argmin attribution (slot program R0): which regime owned the
        // binding CFL element, as a share of census/re-tier events. The
        // number that says whether the step is set by the slot.
        {
            const long tot = rs.fv_dt_argmin_pressurized + rs.fv_dt_argmin_band +
                             rs.fv_dt_argmin_free + rs.fv_dt_argmin_node;
            if (tot > 0) {
                auto pct = [&](long v) {
                    return 100.0 * static_cast<double>(v) /
                           static_cast<double>(tot);
                };
                std::fprintf(f, "\n  dt Argmin Pressurized (%%) %14.1f",
                             pct(rs.fv_dt_argmin_pressurized));
                std::fprintf(f, "\n  dt Argmin Taper Band (%%) .%14.1f",
                             pct(rs.fv_dt_argmin_band));
                std::fprintf(f, "\n  dt Argmin Free (%%) .......%14.1f",
                             pct(rs.fv_dt_argmin_free));
                std::fprintf(f, "\n  dt Argmin Node Bound (%%) .%14.1f",
                             pct(rs.fv_dt_argmin_node));
            }
        }

        // LTS tier occupancy. A run that quietly collapsed to one tier reads
        // as n_tiers == 1 here rather than as a silently ordinary run — which
        // is the difference between "tiering did not help" and "tiering never
        // engaged", and they call for opposite responses.
        if (rs.fv_n_tiers > 0) {
            long total = 0;
            for (int k = 0; k < rs.fv_n_tiers && k < 8; ++k)
                total += rs.fv_tier_cells[k];
            if (total > 0) {
                for (int k = 0; k < rs.fv_n_tiers && k < 8; ++k)
                    std::fprintf(f,
                        "\n  LTS Tier %d Occupancy (%%) .%14.1f", k,
                        100.0 * static_cast<double>(rs.fv_tier_cells[k])
                              / static_cast<double>(total));
            }
        }
    }

    // =====================================================================
    // Slot Storage Summary (FV slot program R0) — how much of the run's
    // conduit storage stood in the Preissmann slot. The run share is the
    // ratio of time integrals; links are listed when their peak share
    // crossed the 1 % budget. Silent when the slot never held water.
    // =====================================================================
    {
        double sum_slot_dt = 0.0, sum_vol_dt = 0.0;
        for (std::size_t j = 0; j < ctx.links.stat_slot_vol_dt.size(); ++j) {
            sum_slot_dt += ctx.links.stat_slot_vol_dt[j];
            sum_vol_dt  += ctx.links.stat_vol_dt[j];
        }
        if (sum_slot_dt > 0.0) {
            WRITE(f, "");
            WRITE(f, "");
            WRITE(f, "*********************");
            WRITE(f, "Slot Storage Summary");
            WRITE(f, "*********************");
            std::fprintf(f, "\n  Run Slot Share (%%) .......%14.2f",
                         100.0 * sum_slot_dt / std::max(sum_vol_dt, 1e-30));
            std::fprintf(f, "\n  Peak Slot Share (%%) ......%14.2f",
                         100.0 * ctx.routing_stats.slot_peak_share);
            std::fprintf(f, "\n  Hours Share Above 1%% .....%14.2f",
                         ctx.routing_stats.slot_time_above_s / 3600.0);

            // Per-link rows for offenders (peak share >= 1%).
            bool header = false;
            for (int j = 0; j < ctx.n_links(); ++j) {
                const auto uj = static_cast<std::size_t>(j);
                if (ctx.links.stat_peak_slot_share[uj] < 0.01) continue;
                if (!header) {
                    std::fprintf(f,
                        "\n\n  Link                    Peak Share    Run Share"
                        "   Hrs >1%%"
                        "\n  ----------------------------------------------------------");
                    header = true;
                }
                const double run_share =
                    (ctx.links.stat_vol_dt[uj] > 0.0)
                        ? ctx.links.stat_slot_vol_dt[uj] /
                              ctx.links.stat_vol_dt[uj]
                        : 0.0;
                std::fprintf(f, "\n  %-20s %9.2f%%   %9.2f%% %9.2f",
                             ctx.link_names.name_of(j).c_str(),
                             100.0 * ctx.links.stat_peak_slot_share[uj],
                             100.0 * run_share,
                             ctx.links.stat_time_slot_above[uj] / 3600.0);
            }
        }
    }

    WRITE(f, "");
    WRITE(f, "");

    // =====================================================================
    // Virtual Junction Summary — refactored-engine feature (momentum-
    // residual diagnostic from DWSolver; see the virtual-junction plan §8).
    // =====================================================================
    if (!ctx.vj_diag.node_idx.empty()) {
        WRITE(f, "***********************");
        WRITE(f, "Virtual Junction Summary");
        WRITE(f, "***********************");
        std::fprintf(f,
            "\n                        Upstream         Downstream         Momentum Residual (cfs·ft/s)");
        std::fprintf(f,
            "\n  Name                 Conduit          Conduit               Maximum          Mean");
        std::fprintf(f,
            "\n  ------------------------------------------------------------------------------------");
        bool any_fed = false;
        for (std::size_t r = 0; r < ctx.vj_diag.node_idx.size(); ++r) {
            const int ni = ctx.vj_diag.node_idx[r];
            const int ju = ctx.vj_diag.up_link[r];
            const int jd = ctx.vj_diag.dn_link[r];
            const long long n = ctx.vj_diag.resid_n[r];
            const double mean = (n > 0)
                ? ctx.vj_diag.resid_sum[r] / static_cast<double>(n) : 0.0;
            // A virtual junction fed by a lateral inflow (plans/
            // VJ_LATERAL_INFLOW_PLAN_2026-09-04.md) legitimately shows a
            // nonzero residual — the added mass carries no momentum — so
            // the row says so. Unfed rows keep the original format.
            const auto uni = static_cast<std::size_t>(ni);
            const double max_lat = (ni >= 0 && uni < ctx.nodes.stat_max_lat_inflow.size())
                ? ctx.nodes.stat_max_lat_inflow[uni] : 0.0;
            // Signed statistic (an inlet junction's capture sink is negative).
            if (std::fabs(max_lat) > 0.0) {
                std::fprintf(f, "\n  %-20s %-16s %-16s %13.6f %13.6f   lateral %.3f %s",
                    ctx.node_names.name_of(ni).c_str(),
                    (ju >= 0) ? ctx.link_names.name_of(ju).c_str() : "*",
                    (jd >= 0) ? ctx.link_names.name_of(jd).c_str() : "*",
                    ctx.vj_diag.resid_max[r], mean,
                    max_lat * Qcf, FlowUnitWords[fu]);
                any_fed = true;
            } else {
                std::fprintf(f, "\n  %-20s %-16s %-16s %13.6f %13.6f",
                    ctx.node_names.name_of(ni).c_str(),
                    (ju >= 0) ? ctx.link_names.name_of(ju).c_str() : "*",
                    (jd >= 0) ? ctx.link_names.name_of(jd).c_str() : "*",
                    ctx.vj_diag.resid_max[r], mean);
            }
        }
        if (any_fed)
            std::fprintf(f, "\n  (lateral: maximum lateral inflow at the node; "
                            "its residual includes the added zero-momentum mass)");
        WRITE(f, "");
        WRITE(f, "");
    }

    } // end rpt_flowstats

    // =====================================================================
    // Rainfall File Summary — matches legacy report_writeRainStats().
    // Printed once per simulation when any gage reads an external rain file.
    // =====================================================================
    {
        bool any_rain_file = false;
        for (int g = 0; g < ctx.n_gages(); ++g) {
            if (ctx.gages.source[static_cast<std::size_t>(g)] == RainSource::FILE_RAIN) {
                any_rain_file = true;
                break;
            }
        }
        if (any_rain_file) {
            WRITE(f, "");
            WRITE(f, "*********************");
            WRITE(f, "Rainfall File Summary");
            WRITE(f, "*********************");
            std::fprintf(f,
"\n  Station    First        Last         Recording   Periods    Periods    Periods");
            std::fprintf(f,
"\n  ID         Date         Date         Frequency  w/Precip    Missing    Malfunc.");
            std::fprintf(f,
"\n  -------------------------------------------------------------------------------");

            for (int g = 0; g < ctx.n_gages(); ++g) {
                const auto ug = static_cast<std::size_t>(g);
                if (ctx.gages.source[ug] != RainSource::FILE_RAIN) continue;

                char d1[16] = "***********", d2[16] = "***********";
                if (ctx.gages.file_first_date[ug] > 0.0) {
                    int y, m, d; datetime::decodeDate(ctx.gages.file_first_date[ug], y, m, d);
                    std::snprintf(d1, sizeof(d1), "%02d/%02d/%04d", m, d, y);
                }
                if (ctx.gages.file_last_date[ug] > 0.0) {
                    int y, m, d; datetime::decodeDate(ctx.gages.file_last_date[ug], y, m, d);
                    std::snprintf(d2, sizeof(d2), "%02d/%02d/%04d", m, d, y);
                }
                const std::string& sta = ctx.gages.station_id[ug].empty()
                                           ? ctx.gage_names.name_of(g)
                                           : ctx.gages.station_id[ug];
                std::fprintf(f,
                    "\n  %-10s %10s   %-10s   %5d min    %6ld     %6ld     %6ld",
                    sta.c_str(), d1, d2,
                    ctx.gages.interval_sec[ug] / 60,
                    ctx.gages.file_periods_precip[ug], 0L, 0L);
                WRITE(f, "");
            }
            WRITE(f, "");
        }
    }

    // =====================================================================
    // Control Actions Taken — Gap #67, matches legacy report_writeRuleAction()
    // Logged by ControlEngine::applyPendingActions() when rpt_controls == true.
    // =====================================================================
    if (opt.rpt_controls) {
        WRITE(f, "*********************");
        WRITE(f, "Control Actions Taken");
        WRITE(f, "*********************");

        // legacy writes only the heading when no action was taken
        if (!ctx.control_log.empty()) {
            // Match legacy report_writeControlAction() exactly:
            //   "  %11s: %8s Link %s setting changed to %6.2f by Control %s"
            // with absolute calendar date/time (not elapsed days).
            for (const auto& entry : ctx.control_log) {
                int yr, mo, dy, hr, mn, sc;
                datetime::decodeDate(entry.date, yr, mo, dy);
                datetime::decodeTime(entry.date, hr, mn, sc);
                char datebuf[16], timebuf[16];
                std::snprintf(datebuf, sizeof(datebuf), "%02d/%02d/%04d", mo, dy, yr);
                std::snprintf(timebuf, sizeof(timebuf), "%02d:%02d:%02d", hr, mn, sc);
                const char* rule_name =
                    (entry.rule_idx >= 0
                     && entry.rule_idx < static_cast<int>(ctx.control_rule_names.size()))
                        ? ctx.control_rule_names[static_cast<std::size_t>(entry.rule_idx)].c_str()
                        : "Rule?";
                std::fprintf(f,
                    "\n  %11s: %8s Link %s setting changed to %6.2f by Control %s",
                    datebuf, timebuf,
                    ctx.link_names.name_of(entry.link_idx).c_str(),
                    entry.new_setting,
                    rule_name);
            }
            if (ctx.control_log_dropped > 0)
                std::fprintf(f,
                    "\n  (%zu further control actions not listed: the log is "
                    "capped at %zu entries)",
                    ctx.control_log_dropped, SimulationContext::kMaxControlLog);
        }

        WRITE(f, "");
        WRITE(f, "");
    }

    // =====================================================================
    // Subcatchment Runoff Summary — matches legacy writeSubcatchRunoff()
    // =====================================================================
    if (ctx.n_subcatches() > 0) {
        WRITE(f, "***************************");
        WRITE(f, "Subcatchment Runoff Summary");
        WRITE(f, "***************************");
        std::fprintf(f, "\n");
        std::fprintf(f,
"\n  ------------------------------------------------------------------------------------------------------------------------------"
"\n                            Total      Total      Total      Total     Imperv       Perv      Total       Total     Peak  Runoff"
"\n                           Precip      Runon       Evap      Infil     Runoff     Runoff     Runoff      Runoff   Runoff   Coeff"
"\n  Subcatchment                 %2s         %2s         %2s         %2s         %2s         %2s         %2s    %8s      %3s"
"\n  ------------------------------------------------------------------------------------------------------------------------------",
            depth_word, depth_word, depth_word, depth_word, depth_word,
            depth_word, depth_word, vol_word, FlowUnitWords[fu]);

        for (int j = 0; j < ctx.n_subcatches(); ++j) {
            auto uj = static_cast<std::size_t>(j);
            double a = ctx.subcatches.area[uj];
            if (a <= 0.0) continue;
            double area_ft2 = a * landAreaToFt2(fu);

            // Depth columns: ft³/ft² = ft → in (US) | mm (SI) via depth_vcf.
            double precip_in = (area_ft2 > 0.0) ?
                ctx.subcatches.stat_precip_vol[uj] / area_ft2 * depth_vcf : 0.0;
            double evap_in = (area_ft2 > 0.0) ?
                ctx.subcatches.stat_evap_vol[uj] / area_ft2 * depth_vcf : 0.0;
            double infil_in = (area_ft2 > 0.0) ?
                ctx.subcatches.stat_infil_vol[uj] / area_ft2 * depth_vcf : 0.0;
            double imperv_in = (area_ft2 > 0.0) ?
                ctx.subcatches.stat_imperv_vol[uj] / area_ft2 * depth_vcf : 0.0;
            double perv_in = (area_ft2 > 0.0) ?
                ctx.subcatches.stat_perv_vol[uj] / area_ft2 * depth_vcf : 0.0;
            double runoff_in = (area_ft2 > 0.0) ?
                ctx.subcatches.stat_runoff_vol[uj] / area_ft2 * depth_vcf : 0.0;
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
    // total_load is in USER MASS (lbs/kg) since the 2026-08-23 units fix:
    // the washoff loop applies mcf at the booking seam, exactly as legacy
    // does at surfqual.c:357 — so this summary prints the value RAW, and it
    // agrees with the Runoff Quality Continuity ledger row by construction.
    // The /453592 that used to sit here was half of the defect: with the
    // booking in mg/L·ft³ the summary printed 1/28.3 of the true pounds
    // while the ledger row printed 16057× them (known-mass audit,
    // QUALITY_LEDGER_UNITS_AUDIT §7).
    // =====================================================================
    if (ctx.n_subcatches() > 0 && ctx.n_pollutants() > 0
        && !opt.ignore_quality) {
        int ns = ctx.n_subcatches();
        int np = ctx.n_pollutants();

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
                double load_lbs = (idx < ctx.subcatches.total_load.size())
                    ? ctx.subcatches.total_load[idx] : 0.0;
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
    // BW-MSX (2026-09-19): the reactions component's species that build up
    // and wash off — same layout as the pollutant block, user mass per
    // species (MG/UG species print lbs/kg; other units print "mass").
    // =====================================================================
    {
        const auto& ms = ctx.reactions.surface;
        if (ms.active() && ctx.n_subcatches() > 0
            && !opt.ignore_quality) {
            const int ns = ctx.n_subcatches();
            const int nm = ms.n_species;
            WRITE(f, "********************************");
            WRITE(f, "Subcatchment MSX Washoff Summary");
            WRITE(f, "********************************");
            std::fprintf(f, "\n");
            std::fprintf(f, " \n  ----------------------------------");
            for (int m = 1; m < nm; ++m) std::fprintf(f, "--------------");
            std::fprintf(f, " \n                                ");
            for (int m = 0; m < nm; ++m)
                std::fprintf(f, "%14s", ctx.reactions.species_name[static_cast<std::size_t>(m)].c_str());
            std::fprintf(f, " \n  Subcatchment                  ");
            for (int m = 0; m < nm; ++m) {
                const double mcf = ms.mcf[static_cast<std::size_t>(m)];
                std::fprintf(f, "%14s", (mcf == 1.0) ? "mass" : (si_report ? "kg" : "lbs"));
            }
            std::fprintf(f, " \n  ----------------------------------");
            for (int m = 1; m < nm; ++m) std::fprintf(f, "--------------");
            std::vector<double> sys(static_cast<std::size_t>(nm), 0.0);
            for (int j = 0; j < ns; ++j) {
                std::fprintf(f, "\n  %-30s", ctx.subcatch_names.name_of(j).c_str());
                for (int m = 0; m < nm; ++m) {
                    const double v = ms.led_subcatch_load[ms.sidx(j, m)];
                    sys[static_cast<std::size_t>(m)] += v;
                    std::fprintf(f, "%14.3f", v);
                }
            }
            std::fprintf(f, " \n  ----------------------------------");
            for (int m = 1; m < nm; ++m) std::fprintf(f, "--------------");
            std::fprintf(f, "\n  System                        ");
            for (int m = 0; m < nm; ++m) std::fprintf(f, "%14.3f", sys[static_cast<std::size_t>(m)]);
            WRITE(f, "");
            std::fprintf(f, "  Surface ledger (species: initial buildup, net buildup, washed to network, swept, BMP removed):\n");
            for (int m = 0; m < nm; ++m) {
                const auto um = static_cast<std::size_t>(m);
                std::fprintf(f, "    %-16s %14.3f %14.3f %14.3f %14.3f %14.3f\n",
                             ctx.reactions.species_name[um].c_str(),
                             ms.led_init_buildup[um], ms.led_buildup[um],
                             ms.led_runoff_load[um], ms.led_sweeping[um], ms.led_bmp_removal[um]);
            }
            WRITE(f, "");
            WRITE(f, "");
        }
    }

#ifdef OPENSWMM_HAS_2D
    // S7: 2D Surface Washoff Summary — the cells' land-use surfaces, per
    // surface species (pollutants, then MSX). The same ledger rows as the
    // subcatchment blocks above, in the same user mass, plus the store left
    // on the mesh; "washed" is what entered the cell rows (it reaches the
    // network through the coupling tuple, so it is not a node load here).
    {
        const auto* sq = ctx.twod_io.surface_quality;
        if (sq && sq->active() && !opt.ignore_quality) {
            const int nsp = sq->nSpecies();
            WRITE(f, "**************************");
            WRITE(f, "2D Surface Washoff Summary");
            WRITE(f, "**************************");
            std::fprintf(f, "\n");
            std::fprintf(f, "  Cells: %d  Land uses: %d  (buildup per %s)\n",
                         sq->nCells(), sq->nLandUses(), si_report ? "hectare" : "acre");
            std::fprintf(f, "  %-16s %14s %14s %14s %14s %14s %14s\n", "Species",
                         "Init Buildup", "Net Buildup", "Washed Off", "Swept", "BMP Removed", "On Mesh");
            for (int sidx = 0; sidx < nsp; ++sidx) {
                const auto us = static_cast<std::size_t>(sidx);
                double store = 0.0;
                if (ctx.twod_io.mesh) {
                    const auto& mesh = *ctx.twod_io.mesh;
                    const double to_la = si_report ? 1.0e-4 : 1.0 / 4046.8564224;   // m² → ha | acre
                    for (int c = 0; c < sq->nCells(); ++c)
                        store += sq->buildupPerArea(c, sidx) *
                                 mesh.tri_area[static_cast<std::size_t>(c)] * to_la;
                }
                std::fprintf(f, "  %-16s %14.3f %14.3f %14.3f %14.3f %14.3f %14.3f\n",
                             sq->speciesName(sidx).c_str(),
                             sq->ledInitBuildup()[us], sq->ledBuildup()[us], sq->ledWashoff()[us],
                             sq->ledSweeping()[us], sq->ledBmp()[us], store);
            }
            std::fprintf(f, "  (%s; a reactions species declared in UG or as a count keeps its own unit)\n",
                         si_report ? "kg" : "lbs");
            WRITE(f, "");
            WRITE(f, "");
        }
    }

#endif

    // =====================================================================
    // Groundwater Summary — matches legacy writeGroundwater()
    // =====================================================================
    if (has_gw && !opt.ignore_groundwater) {
        WRITE(f, "*******************");
        WRITE(f, "Groundwater Summary");
        WRITE(f, "*******************");
        std::fprintf(f, "\n");
        std::fprintf(f,
"\n  -----------------------------------------------------------------------------------------------------"
"\n                                            Total    Total  Maximum  Average  Average    Final    Final"
"\n                          Total    Total    Lower  Lateral  Lateral    Upper    Water    Upper    Water"
"\n                          Infil     Evap  Seepage  Outflow  Outflow   Moist.    Table   Moist.    Table");
        std::fprintf(f, si_report
            ? "\n  Subcatchment               mm       mm       mm       mm      %3s                 m                 m"
            : "\n  Subcatchment               in       in       in       in      %3s                ft                ft",
            FlowUnitWords[fu]);
        std::fprintf(f,
"\n  -----------------------------------------------------------------------------------------------------");

        for (int j = 0; j < ctx.n_subcatches(); ++j) {
            auto uj = static_cast<std::size_t>(j);
            if (ctx.subcatches.gw_aquifer[uj] < 0) continue;
            double area_ft2  = ctx.subcatches.area[uj] * landAreaToFt2(fu);
            // Depth columns: ft → in | mm (legacy UCF(RAINDEPTH)); water table: ft → ft | m (UCF(LENGTH)).
            double infil_in  = (area_ft2 > 0.0) ? ctx.subcatches.stat_gw_infil_vol[uj]     / area_ft2 * depth_vcf : 0.0;
            double evap_in   = (area_ft2 > 0.0) ? (ctx.subcatches.stat_gw_upper_evap_vol[uj] +
                                                    ctx.subcatches.stat_gw_lower_evap_vol[uj]) / area_ft2 * depth_vcf : 0.0;
            double seep_in   = (area_ft2 > 0.0) ? ctx.subcatches.stat_gw_deep_perc_vol[uj]  / area_ft2 * depth_vcf : 0.0;
            double lat_in    = (area_ft2 > 0.0) ? ctx.subcatches.stat_gw_flow_vol[uj]       / area_ft2 * depth_vcf : 0.0;
            double max_flow  = ctx.subcatches.stat_gw_max_flow[uj] * Qcf;
            long   steps     = ctx.subcatches.stat_gw_steps[uj];
            double avg_theta = (steps > 0L) ? ctx.subcatches.stat_gw_sum_theta[uj] / static_cast<double>(steps) : 0.0;
            double avg_depth = (steps > 0L) ? ctx.subcatches.stat_gw_sum_depth[uj] / static_cast<double>(steps) * len_ucf : 0.0;
            double fin_theta = ctx.subcatches.stat_gw_final_theta[uj];
            double fin_depth = ctx.subcatches.stat_gw_final_depth[uj] * len_ucf;
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
    if (ctx.lid_usage.count() > 0) {
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
            std::fprintf(f,
"\n  -----------------------------------------------------------------"
"\n                          Total    Evap   Infil  Surface   Drain"
"\n                         Inflow    Loss    Loss    Runoff    Flow");
            std::fprintf(f, si_report
                ? "\n  Control Group    Subcatch      mm      mm      mm       mm      mm"
                : "\n  Control Group    Subcatch      in      in      in       in      in");
            std::fprintf(f,
"\n  -----------------------------------------------------------------");

            for (int j = 0; j < n_usage; ++j) {
                auto uj = static_cast<std::size_t>(j);
                int li = ctx.lid_usage.lid_index[uj];
                int si = ctx.lid_usage.subcatch_index[uj];

                // Water-balance depths: ft → in | mm (legacy UCF(RAINDEPTH)).
                auto safe = [&](const std::vector<double>& v) -> double {
                    return (uj < v.size()) ? v[uj] * depth_vcf : 0.0;
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
    if (ctx.n_nodes() > 0) {
        WRITE(f, "******************");
        WRITE(f, "Node Depth Summary");
        WRITE(f, "******************");
        std::fprintf(f, "\n");
        std::fprintf(f,
"\n  ---------------------------------------------------------------------------------"
"\n                                 Average  Maximum  Maximum  Time of Max    Reported"
"\n                                   Depth    Depth      HGL   Occurrence   Max Depth");
        std::fprintf(f, si_report
            ? "\n  Node                 Type       Meters   Meters   Meters  days hr:min      Meters"
            : "\n  Node                 Type         Feet     Feet     Feet  days hr:min        Feet");
        std::fprintf(f,
"\n  ---------------------------------------------------------------------------------");

        long report_steps = ctx.routing_stats.report_steps;
        report_steps = std::max(report_steps, 1L);

        for (int j = 0; j < ctx.n_nodes(); ++j) {
            auto uj = static_cast<std::size_t>(j);
            int nt = static_cast<int>(ctx.nodes.type[uj]);
            double max_d_int = ctx.nodes.stat_max_depth[uj];
            double avg_d = ctx.nodes.stat_sum_depth[uj] / static_cast<double>(report_steps) * len_ucf;
            double max_d = max_d_int * len_ucf;
            double max_hgl = (ctx.nodes.invert_elev[uj] + max_d_int) * len_ucf;
            double rpt_max = ctx.nodes.stat_max_rpt_depth[uj];  // already display units
            int days, hrs, mins;
            elapsedToParts(ctx.nodes.stat_max_depth_date[uj], ctx.options.report_start, days, hrs, mins);

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
"\n  Node                 Type           %3s      %3s  days hr:min    %8s    %8s     Percent"
"\n  -------------------------------------------------------------------------------------------------",
            FlowUnitWords[fu], FlowUnitWords[fu],
            vol_word, vol_word);

        for (int j = 0; j < ctx.n_nodes(); ++j) {
            auto uj = static_cast<std::size_t>(j);
            int nt = static_cast<int>(ctx.nodes.type[uj]);

            double max_lat  = ctx.nodes.stat_max_lat_inflow[uj] * Qcf;
            double max_tot  = ctx.nodes.stat_max_total_inflow[uj] * Qcf;
            double vol_lat  = ctx.nodes.stat_lat_inflow_vol[uj] * Vcf;
            double vol_tot  = ctx.nodes.stat_total_inflow_vol[uj] * Vcf;
            int days, hrs, mins;
            elapsedToParts(ctx.nodes.stat_max_inflow_date[uj], ctx.options.report_start, days, hrs, mins);

            std::fprintf(f, "\n  %-20s", ctx.node_names.name_of(j).c_str());
            std::fprintf(f, " %-9s", nt_str(nt));
            std::fprintf(f, ff, max_lat);
            std::fprintf(f, ff, max_tot);
            std::fprintf(f, "  %4d  %02d:%02d", days, hrs, mins);
            std::fprintf(f, "%12.3g", vol_lat);
            std::fprintf(f, "%12.3g", vol_tot);
            // Flow balance error, legacy writeNodeFlows: outflow includes the
            // final stored volume (massbal_getStorage(TRUE) runs first); below
            // 1 ft3 of outflow the volume difference is printed instead.
            const double in_v  = ctx.nodes.stat_total_inflow_vol[uj];
            const double out_v = ctx.nodes.stat_total_outflow_vol[uj] +
                                 ctx.nodes.volume[uj];
            if (std::fabs(out_v) < 1.0)
                std::fprintf(f, "%12.3f %s", (in_v - out_v) * Vcf * 1.0e6,
                             si_report ? "ltr" : "gal");
            else
                std::fprintf(f, "%12.3f", (in_v - out_v) / out_v * 100.0);
        }
    }

    WRITE(f, "");
    WRITE(f, "");

    // =====================================================================
    // Node Surcharge Summary — matches legacy writeNodeSurcharge()
    // =====================================================================
    if (static_cast<int>(opt.routing_model) == 2) { // DYNWAVE only
        WRITE(f, "**********************");
        WRITE(f, "Node Surcharge Summary");
        WRITE(f, "**********************");

        // legacy writeNodeSurcharge: outfalls skipped; hours floored at 0.01;
        // height above the highest conduit crown and depth below the rim,
        // both from the node's maximum depth.
        int n_written = 0;
        for (int j = 0; j < ctx.n_nodes(); ++j) {
            auto uj = static_cast<std::size_t>(j);
            if (ctx.nodes.type[uj] == NodeType::OUTFALL) continue;
            if (ctx.nodes.stat_time_surcharged[uj] == 0.0) continue;
            const double t = std::max(0.01, ctx.nodes.stat_time_surcharged[uj] / 3600.0);
            if (n_written == 0) {
                WRITE(f, "");
                WRITE(f, "Surcharging occurs when water rises above the top of the highest conduit.");
                std::fprintf(f,
"\n  ---------------------------------------------------------------------"
"\n                                               Max. Height   Min. Depth"
"\n                                   Hours       Above Crown    Below Rim");
                std::fprintf(f, si_report
                    ? "\n  Node                 Type      Surcharged         Meters       Meters"
                    : "\n  Node                 Type      Surcharged           Feet         Feet");
                std::fprintf(f,
"\n  ---------------------------------------------------------------------");
                n_written = 1;
            }
            const double maxd = ctx.nodes.stat_max_depth[uj];
            const double d1 = std::max(0.0, maxd + ctx.nodes.invert_elev[uj] -
                                                ctx.nodes.crown_elev[uj]);
            const double d2 = std::max(0.0, ctx.nodes.full_depth[uj] - maxd);
            std::fprintf(f, "\n  %-20s", ctx.node_names.name_of(j).c_str());
            std::fprintf(f, " %-9s", nt_str(static_cast<int>(ctx.nodes.type[uj])));
            std::fprintf(f, "  %9.2f      %9.3f    %9.3f",
                t, d1 * len_ucf, d2 * len_ucf);
        }
        if (n_written == 0) {
            WRITE(f, "");
            WRITE(f, "No nodes were surcharged.");
        }
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
            std::fprintf(f, "\n  Flooding refers to all water that overflows a node, whether it ponds or not.");
            std::fprintf(f,
"\n  --------------------------------------------------------------------------"
"\n                                                             Total   Maximum"
"\n                                 Maximum   Time of Max       Flood    Ponded"
"\n                        Hours       Rate    Occurrence      Volume     Depth"
"\n  Node                 Flooded       %3s   days hr:min    %8s    %6s"
"\n  --------------------------------------------------------------------------",
                FlowUnitWords[fu], vol_word, len_word);

            for (int j = 0; j < ctx.n_nodes(); ++j) {
                auto uj = static_cast<std::size_t>(j);
                if (ctx.nodes.stat_vol_flooded[uj] <= 0.0) continue;

                double hours = ctx.nodes.stat_time_flooded[uj] / 3600.0;
                double max_rate = ctx.nodes.stat_max_overflow[uj] * Qcf;
                double vol_mgal = ctx.nodes.stat_vol_flooded[uj] * Vcf;
                int days, hrs, mins;
                elapsedToParts(ctx.nodes.stat_max_overflow_date[uj],
                               ctx.options.report_start, days, hrs, mins);
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
                std::fprintf(f, " %9.3f", ponded * len_ucf);
            }
        }
    }

    WRITE(f, "");
    WRITE(f, "");

    // =====================================================================
    // Storage Volume Summary — matches legacy writeStorageVolumes()
    // =====================================================================
    {
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
"\n                          Volume   Pcnt   Pcnt   Pcnt      Volume   Pcnt     Occurrence    Outflow");
            std::fprintf(f, si_report
                // legacy writes the ANSI superscript byte (statsrpt.c:535-537)
                ? "\n  Storage Unit           1000 m\xB3   Full   Loss   Loss     1000 m\xB3   Full    days hr:min        %3s"
                : "\n  Storage Unit          1000 ft\xB3   Full   Loss   Loss    1000 ft\xB3   Full    days hr:min        %3s",
                FlowUnitWords[fu]);
            std::fprintf(f,
"\n  ------------------------------------------------------------------------------------------------");

            long report_steps = ctx.routing_stats.report_steps;
            report_steps = std::max(report_steps, 1L);

            for (int j = 0; j < ctx.n_nodes(); ++j) {
                auto uj = static_cast<std::size_t>(j);
                if (ctx.nodes.type[uj] != NodeType::STORAGE) continue;

                const double full_vol = ctx.nodes.full_volume[uj];
                const double max_depth = ctx.nodes.stat_max_depth[uj];

                // Volume from the node's actual depth→volume relation, not a linear
                // depth ratio. Storage surface area varies with depth for FUNCTIONAL,
                // TABULAR and the geometric shapes, so full_vol*(depth/full_depth) is
                // exact only for a constant-area unit and grossly overstates the rest
                // (a shallow functional/geometric node was reported ~20x too full).
                // Average is the true time-average accumulated per routing step
                // (stat_sum_volume); maximum is exact — V(d) is monotonic, so the max
                // volume occurs at the max depth. node::getVolume returns internal ft³
                // from the same relation the solver routes on.
                const int us =
                    ucf::getUnitSystem(static_cast<int>(ctx.options.flow_units));
                // const_cast: getVolume takes a mutable TableData* for the tabular
                // curve's lookup cursor cache; the report context is const and this
                // is a read-only computation, so mutating the cursor is benign.
                auto* tbls = const_cast<TableData*>(&ctx.tables);
                const double avg_vol_ft3 =
                    ctx.nodes.stat_sum_volume[uj] / static_cast<double>(report_steps);
                const double max_vol_ft3 = node::getVolume(
                    ctx.nodes, j, max_depth, tbls, us, &ctx.node_subtypes);

                // Percent full is by VOLUME (legacy semantics), not by depth.
                double pct_avg = (full_vol > 0.0) ? avg_vol_ft3 / full_vol * 100.0 : 0.0;
                double pct_max = (full_vol > 0.0) ? max_vol_ft3 / full_vol * 100.0 : 0.0;
                pct_avg = std::min(pct_avg, 100.0);
                pct_max = std::min(pct_max, 100.0);

                // ft³ → 1000 ft³|m³
                const double avg_vol = avg_vol_ft3 * svol_ucf / 1000.0;
                const double max_vol = max_vol_ft3 * svol_ucf / 1000.0;

                int days, hrs, mins;
                elapsedToParts(ctx.nodes.stat_max_depth_date[uj],
                               ctx.options.report_start, days, hrs, mins);

                double max_outflow = ctx.nodes.stat_storage_max_outflow[uj] * Qcf;

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
    {
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
                     FlowUnitWords[fu], FlowUnitWords[fu], vol_word);
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
        long total_steps = ctx.routing_stats.report_steps;
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
    if (ctx.n_links() > 0) {
        WRITE(f, "********************");
        WRITE(f, "Link Flow Summary");
        WRITE(f, "********************");
        std::fprintf(f, "\n");
        std::fprintf(f,
"\n  -----------------------------------------------------------------------------"
"\n                                 Maximum  Time of Max   Maximum    Max/    Max/"
"\n                                  |Flow|   Occurrence   |Veloc|    Full    Full"
"\n  Link                 Type          %3s  days hr:min    %6s    Flow   Depth"
"\n  -----------------------------------------------------------------------------",
            FlowUnitWords[fu], si_report ? "m/sec" : "ft/sec");

        for (int j = 0; j < ctx.n_links(); ++j) {
            auto uj = static_cast<std::size_t>(j);
            int lt = static_cast<int>(ctx.links.type[uj]);

            double mf = ctx.links.stat_max_flow[uj] * Qcf;  // CFS → display
            double mv = ctx.links.stat_max_veloc[uj] * len_ucf; // ft/s → display
            double fill = ctx.links.stat_max_filling[uj];

            // Flow ratio (using CFS values, not display)
            double flow_ratio = 0.0;
            if (lt == static_cast<int>(LinkType::CONDUIT)) {
                const int cr = ctx.link_subtypes.conduit_row(j);
                const auto& CD = ctx.link_subtypes.conduits;
                double mf_cfs = ctx.links.stat_max_flow[uj];
                double qf = (cr >= 0) ? CD.q_full[static_cast<size_t>(cr)] : 0.0;
                int barrels = std::max((cr >= 0) ? CD.barrels[static_cast<size_t>(cr)] : 1, 1);
                flow_ratio = (qf > 0.0) ? mf_cfs / qf / static_cast<double>(barrels) : 0.0;
            }

            int days, hrs, mins;
            elapsedToParts(ctx.links.stat_max_flow_date[uj], ctx.options.report_start, days, hrs, mins);

            // Row layout of legacy writeLinkFlows (statsrpt.c). A link with no
            // [XSECTIONS] row keeps xsect.type DUMMY (0) there — pumps and
            // outlets — and an IRREGULAR section prints CHANNEL.
            const auto shape = ctx.links.xsect_shape[uj];
            const bool dummy = shape == XsectShape::DUMMY ||
                               lt == static_cast<int>(LinkType::PUMP) ||
                               lt == static_cast<int>(LinkType::OUTLET);
            std::fprintf(f, "\n  %-20s", ctx.link_names.name_of(j).c_str());
            if (dummy)                               std::fprintf(f, " DUMMY   ");
            else if (shape == XsectShape::IRREGULAR) std::fprintf(f, " CHANNEL ");
            else                                     std::fprintf(f, " %-7s ", lt_str(lt));
            std::fprintf(f, ff, mf);
            std::fprintf(f, "  %4d  %02d:%02d", days, hrs, mins);

            // Pumps: max flow over the pump curve's largest flow (legacy
            // Link.qFull, link.c:1505-1516; 0 for an ideal pump).
            if (lt == static_cast<int>(LinkType::PUMP)) {
                double q_full = 0.0;
                const int pr = ctx.link_subtypes.pump_row(j);
                if (pr >= 0) {
                    const int ci = ctx.link_subtypes.pumps.curve[static_cast<std::size_t>(pr)];
                    if (ci >= 0 && ci < static_cast<int>(ctx.tables.tables.size())) {
                        const auto& ty = ctx.tables.tables[static_cast<std::size_t>(ci)].y;
                        if (!ty.empty())
                            q_full = *std::max_element(ty.begin(), ty.end()) / Qcf;
                    }
                }
                if (q_full > 0.0) {
                    std::fprintf(f, "          ");
                    std::fprintf(f, "  %6.2f", ctx.links.stat_max_flow[uj] / q_full);
                    continue;
                }
            }
            if (dummy) continue;

            if (lt == static_cast<int>(LinkType::CONDUIT)) {
                if (mv > 50.0) std::fprintf(f, "    >50.00");
                else           std::fprintf(f, "   %7.2f", mv);
                std::fprintf(f, "  %6.2f", flow_ratio);
            } else {
                std::fprintf(f, "                  ");
            }

            // Max/full depth; a bottom orifice has no full depth.
            bool no_full_depth = ctx.links.xsect_y_full[uj] <= 0.0;
            if (lt == static_cast<int>(LinkType::ORIFICE)) {
                const int orow = ctx.link_subtypes.orifice_row(j);
                if (orow >= 0 &&
                    ctx.link_subtypes.orifices.orifice_type[static_cast<std::size_t>(orow)] == 0.0)
                    no_full_depth = true;
            }
            if (no_full_depth) std::fprintf(f, "        ");
            else               std::fprintf(f, "  %6.2f", fill);
        }
    }

    WRITE(f, "");
    WRITE(f, "");

    // =====================================================================
    // Flow Classification Summary — matches legacy writeFlowClass()
    // =====================================================================
    // Dynamic wave only in legacy (FV, a v6 router, keeps the table);
    // fractions of the routed time after the report start.
    if (ctx.n_links() > 0 && (opt.routing_model == RoutingModel::DYNWAVE ||
                              opt.routing_model == RoutingModel::FV)) {
        WRITE(f, "***************************");
        WRITE(f, "Flow Classification Summary");
        WRITE(f, "***************************");
        std::fprintf(f,
"\n\n  -------------------------------------------------------------------------------------"
"\n                      Adjusted    ---------- Fraction of Time in Flow Class ---------- "
"\n                       /Actual         Up    Down  Sub   Sup   Up    Down  Norm  Inlet "
"\n  Conduit               Length    Dry  Dry   Dry   Crit  Crit  Crit  Crit  Ltd   Ctrl  "
"\n  -------------------------------------------------------------------------------------");

        const double total_secs = ctx.routing_stats.report_time;

        for (int j = 0; j < ctx.n_links(); ++j) {
            auto uj = static_cast<std::size_t>(j);
            if (ctx.links.type[uj] != LinkType::CONDUIT) continue;
            if (ctx.links.xsect_shape[uj] == XsectShape::DUMMY) continue;

            // Adjusted/actual length ratio
            double len_ratio = 1.0;
            const int cr = ctx.link_subtypes.conduit_row(j);
            const auto& CD = ctx.link_subtypes.conduits;
            const double L = (cr >= 0) ? CD.length[static_cast<size_t>(cr)] : 0.0;
            if (L > 0.0)
                len_ratio = ((cr >= 0) ? CD.mod_length[static_cast<size_t>(cr)] : 0.0) / L;

            std::fprintf(f, "\n  %-20s", ctx.link_names.name_of(j).c_str());
            std::fprintf(f, "  %6.2f ", len_ratio);

            for (int c = 0; c < LinkData::N_FLOW_CLASSES; ++c) {
                auto idx = uj * LinkData::N_FLOW_CLASSES + static_cast<std::size_t>(c);
                const double t = (idx < ctx.links.stat_flow_class.size()) ?
                    ctx.links.stat_flow_class[idx] : 0.0;
                std::fprintf(f, "  %4.2f", t / total_secs);
            }
            std::fprintf(f, "  %4.2f", ctx.links.stat_norm_ltd[uj] / total_secs);
            std::fprintf(f, "  %4.2f", ctx.links.stat_inlet_ctrl[uj] / total_secs);
        }
    }
    WRITE(f, "");

    // =====================================================================
    // Conduit Surcharge Summary — matches legacy writeLinkSurcharge()
    // =====================================================================
    {
        // legacy writeLinkSurcharge: true conduits whose four hour totals are
        // not all zero; each value floored at 0.01 h.
        WRITE(f, "*************************");
        WRITE(f, "Conduit Surcharge Summary");
        WRITE(f, "*************************");
        int n_written = 0;
        for (int j = 0; j < ctx.n_links(); ++j) {
            auto uj = static_cast<std::size_t>(j);
            if (ctx.links.type[uj] != LinkType::CONDUIT ||
                ctx.links.xsect_shape[uj] == XsectShape::DUMMY) continue;
            double t[5] = {
                ctx.links.stat_time_surcharged[uj] / 3600.0,
                ctx.links.stat_time_full_upstream[uj] / 3600.0,
                ctx.links.stat_time_full_dnstream[uj] / 3600.0,
                ctx.links.stat_time_full_both[uj] / 3600.0,
                0.0 };
            if (t[0] + t[1] + t[2] + t[3] == 0.0) continue;
            t[4] = ctx.links.stat_time_capacity_limited[uj] / 3600.0;
            for (double& x : t) x = std::max(0.01, x);
            if (n_written == 0) {
                WRITE(f, "");
                std::fprintf(f,
"\n  ----------------------------------------------------------------------------"
"\n                                                           Hours        Hours "
"\n                         --------- Hours Full --------   Above Full   Capacity"
"\n  Conduit                Both Ends  Upstream  Dnstream   Normal Flow   Limited"
"\n  ----------------------------------------------------------------------------");
                n_written = 1;
            }
            std::fprintf(f, "\n  %-20s", ctx.link_names.name_of(j).c_str());
            std::fprintf(f, "    %8.2f  %8.2f  %8.2f  %8.2f     %8.2f",
                t[0], t[1], t[2], t[3], t[4]);
        }
        if (n_written == 0) {
            WRITE(f, "");
            WRITE(f, "No conduits were surcharged.");
        }
    }

    WRITE(f, "");
    WRITE(f, "");

    // =====================================================================
    // Pumping Summary — matches legacy writePumpFlows()
    // =====================================================================
    {
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
                FlowUnitWords[fu], FlowUnitWords[fu], FlowUnitWords[fu], vol_word);

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
    // Street tables — Street Flow Summary (Gap #9) and the Street Inlet Flow
    // Summary (Gap #68). Both are gated the same way the inlet table already
    // was: link reporting on, and something street-related in the model.
    //
    // Street index of a STREET conduit: xsect_curve is the built transect
    // table, whose name is the street it was built from
    // (PostParseResolver.cpp:2298-2310).
    // =====================================================================
    auto street_of_link = [&ctx](int j) -> int {
        const auto uj = static_cast<std::size_t>(j);
        if (ctx.links.xsect_shape[uj] != XsectShape::STREET_XSECT) return -1;
        const int tt = ctx.links.xsect_curve[uj];
        if (tt < 0 || tt >= static_cast<int>(ctx.transect_tables.size())) return -1;
        const std::string& sname = ctx.transect_tables[static_cast<std::size_t>(tt)].name;
        for (int s = 0; s < ctx.streets.count(); ++s) {
            const std::string& a = ctx.streets.names[static_cast<std::size_t>(s)];
            if (a.size() != sname.size()) continue;
            bool same = true;
            for (std::size_t c = 0; c < a.size(); ++c)
                if (std::tolower(static_cast<unsigned char>(a[c])) !=
                    std::tolower(static_cast<unsigned char>(sname[c]))) { same = false; break; }
            if (same) return s;
        }
        return -1;
    };

    // ---------------------------------------------------------------------
    // Street Flow Summary — legacy writeStreetStats (inlet.c:1055-1094):
    // peak flow, SWMM's spread (flow width at max depth / sides, clipped to
    // the street's curb-to-crown width) and max depth per STREET conduit.
    // ---------------------------------------------------------------------
    if (ctx.streets.count() > 0) {
        bool header = false;
        const double inv_len = 1.0 / len_ucf;   // display → ft (street store is user units)
        for (int j = 0; j < ctx.n_links(); ++j) {
            const int si = street_of_link(j);
            if (si < 0) continue;
            const auto uj = static_cast<std::size_t>(j);
            const auto su = static_cast<std::size_t>(si);

            if (!header) {
                WRITE(f, "*******************");
                WRITE(f, "Street Flow Summary");
                WRITE(f, "*******************");
                std::fprintf(f,
"\n\n  -------------------------------------------------------"
"\n                          Peak   Maximum   Maximum"
"\n                          Flow    Spread     Depth"
"\n  Street Conduit           %-3s     %-5s     %-5s"
"\n  -------------------------------------------------------",
                    FlowUnitWords[fu], si_report ? "m" : "ft",
                    si_report ? "m" : "ft");
                header = true;
            }

            const double max_flow  = ctx.links.stat_max_flow[uj];
            const double max_depth = ctx.links.stat_max_filling[uj]
                                   * ctx.links.xsect_y_full[uj];

            // SWMM's spread (flow width) at max depth — not HEC-22's, which is
            // based on max flow and cannot see backwater (inlet.c:1077-1089).
            const XSectParams xs =
                link::buildXSectParams(ctx.links, uj, &ctx.transect_tables);
            const int sides = std::max(ctx.streets.sides[su], 1);
            double max_spread = xsect::getWofY(xs, max_depth) / sides;
            max_spread = std::min(max_spread, ctx.streets.t_crown[su] * inv_len);

            std::fprintf(f, "\n  %-16s %9.3f %9.3f %9.3f",
                ctx.link_names.name_of(j).c_str(),
                max_flow * Qcf, max_spread * len_ucf, max_depth * len_ucf);
        }
        if (header) { WRITE(f, ""); WRITE(f, ""); }
    }

    // ---------------------------------------------------------------------
    // Street Inlet Flow Summary — Gap #68 volumes plus the legacy per-inlet
    // performance columns (inlet.c:1096-1124). Inlet junctions are listed
    // under their node name with a "(node)" marker.
    // Volumes in ft³; convert to 1000 gal: × 7.48052 / 1000
    // ---------------------------------------------------------------------
    if (ctx.inlet_usages.count() > 0) {
        int ni = ctx.inlet_usages.count();
        // Only write if stats arrays are populated
        bool has_stats = (static_cast<int>(ctx.inlet_usages.stat_capture_vol.size()) >= ni);
        const bool has_diag = (ctx.inlet_diag.count() >= ni);

        WRITE(f, "**************************");
        WRITE(f, "Street Inlet Flow Summary");
        WRITE(f, "**************************");

        if (!has_stats) {
            WRITE(f, "");
            WRITE(f, "No inlet statistics available.");
        } else {
            static constexpr double FT3_TO_KGAL = 7.48052 / 1000.0;
            std::fprintf(f,
"\n\n  ------------------------------------------------------------------------------------------------------------------------------------------"
"\n                                                              Peak      Peak       Avg.    Bypass      Back      Peak      Peak      Vol.      Vol."
"\n                                                              Flow   Capture    Capture      Flow      Flow   Capture    Bypass  Captured  Bypassed"
"\n  Inlet Location        Inlet Design       Placement  Count     %-3s      Pcnt       Pcnt      Pcnt      Pcnt   / Inlet      %-3s  1000 Gal  1000 Gal"
"\n  ------------------------------------------------------------------------------------------------------------------------------------------",
                FlowUnitWords[fu], FlowUnitWords[fu]);

            for (int i = 0; i < ni; ++i) {
                auto ui = static_cast<std::size_t>(i);
                int li  = ctx.inlet_usages.link_index[i];
                int nh  = ctx.inlet_usages.node_host[ui];
                int di  = ctx.inlet_usages.design_index[i];

                std::string host_name = "?";
                if (nh >= 0) host_name = ctx.node_names.name_of(nh) + " (node)";
                else if (li >= 0) host_name = ctx.link_names.name_of(li);

                const char* inlet_name = (di >= 0 && di < ctx.inlets.count())
                                         ? ctx.inlets.names[di].c_str() : "?";

                double cap_vol  = ctx.inlet_usages.stat_capture_vol[ui];
                double byp_vol  = ctx.inlet_usages.stat_bypass_vol[ui];
                // "Peak Flow" is the peak APPROACH flow the inlet saw, like
                // legacy's street maxFlow column (inlet.c:1092); the peak
                // captured flow is the stat_peak_flow fallback only when the
                // solver never ran (no diagnostics block).
                double peak     = (has_diag ? ctx.inlet_diag.peak_flow[ui]
                                            : ctx.inlet_usages.stat_peak_flow[ui]) * Qcf;
                double cap_kgal = cap_vol * FT3_TO_KGAL;
                double byp_kgal = byp_vol * FT3_TO_KGAL;

                // Legacy performance block. fp/cp are the legacy period counts
                // divided by 100 so the sums below read out as percentages
                // (inlet.c:1106-1122).
                const char* placement = "ON-GRADE";
                int    n_inlets = 1;
                double pfc = 0.0, afc = 0.0, bpf = 0.0, bff = 0.0;
                double peak_cap_per_inlet = 0.0, peak_bypass = 0.0;
                if (has_diag) {
                    const auto& dg = ctx.inlet_diag;
                    placement = (dg.is_sag[ui] != 0) ? "ON-SAG  " : "ON-GRADE";
                    n_inlets  = std::max(dg.num_inlets[ui], 1);
                    const double fp = dg.flow_periods[ui] / 100.0;
                    if (fp > 0.0) {
                        const double cp = dg.capture_periods[ui] / 100.0;
                        pfc = dg.peak_flow_capture[ui];
                        if (cp > 0.0) {
                            afc = dg.avg_flow_capture[ui] / cp;
                            bpf = dg.bypass_freq[ui] / cp;
                        }
                        bff = dg.backflow_periods[ui] / fp;

                        // Peak capture per inlet / peak bypass are scaled off
                        // the approach conduit's peak flow (the host link, or
                        // the inlet junction's approach conduit).
                        const int hl = (li >= 0) ? li : dg.up_link[ui];
                        if (hl >= 0 && hl < ctx.n_links()) {
                            const auto uh = static_cast<std::size_t>(hl);
                            const double max_flow = ctx.links.stat_max_flow[uh];
                            const int si = street_of_link(hl);
                            const int sides = (si >= 0)
                                ? std::max(ctx.streets.sides[static_cast<std::size_t>(si)], 1)
                                : 1;
                            peak_cap_per_inlet = (max_flow / sides) * Qcf * 0.01
                                                 * pfc / n_inlets;
                            peak_bypass = max_flow * Qcf * 0.01 * (100.0 - pfc);
                        }
                    }
                }

                std::fprintf(f,
                    "\n  %-20s  %-16s   %-9s  %5d  %8.3f  %8.2f  %9.2f  %8.2f  %8.2f  %8.3f  %8.3f  %8.3f  %8.3f",
                    host_name.c_str(), inlet_name, placement, n_inlets,
                    peak, pfc, afc, bpf, bff,
                    peak_cap_per_inlet, peak_bypass, cap_kgal, byp_kgal);
            }
        }
        WRITE(f, "");
        WRITE(f, "");
    }

    // =====================================================================
    // Link Pollutant Load Summary — Gap #64, matches legacy writeLinkLoads()
    // stat_total_load is in ft³ × mg/L; convert to lbs: × 28.317/453592
    // =====================================================================
    if (ctx.n_links() > 0 && ctx.n_pollutants() > 0
        && !opt.ignore_quality) {
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

void DefaultReportPlugin::write_timing(std::FILE* f, const SimulationContext& ctx) {
    // =====================================================================
    // Analysis Timing — matches legacy report_writeSysTime()
    //
    // The start of the window is ctx.wall_start, stamped by
    // SWMMEngine::open() before input parsing, mirroring legacy where
    // report_writeLogo() takes SysTime ahead of project_readInput(). Elapsed
    // time therefore covers parse + validation + initialization + routing,
    // not just routing.
    // =====================================================================
    {
        char begin_str[64] = "";
        char end_str[64] = "";

        std::time_t wall_end;
        std::time(&wall_end);

        // A zero wall_start means open() never ran (e.g. the plugin was
        // driven directly). Fall back to the end time so the section reports
        // "< 1 sec" rather than seconds-since-the-epoch.
        std::time_t wall_start = (ctx.wall_start != 0) ? ctx.wall_start : wall_end;

        {
            const char* ct = std::ctime(&wall_start);
            if (ct) {
                std::strncpy(begin_str, ct, sizeof(begin_str) - 1);
                char* nl = std::strchr(begin_str, '\n');
                if (nl) *nl = '\0';
            }
        }
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

        double elapsed_secs = std::difftime(wall_end, wall_start);
        if (elapsed_secs < 1.0) {
            std::fprintf(f, "< 1 sec");
        } else {
            // Legacy rolls whole days into a "d." prefix ahead of hh:mm:ss.
            long es = static_cast<long>(elapsed_secs);
            long days = es / 86400L;
            long rem  = es % 86400L;
            if (days > 0) std::fprintf(f, "%ld.", days);
            std::fprintf(f, "%02ld:%02ld:%02ld",
                         rem / 3600L, (rem % 3600L) / 60L, rem % 60L);
        }
        std::fprintf(f, "\n");
    }
}

} /* namespace openswmm */
