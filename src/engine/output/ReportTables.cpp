// SPDX-License-Identifier: Apache-2.0

/**
 * @file      ReportTables.cpp
 * @brief     Row computation of the `.rpt` summary sections (see ReportTables.hpp).
 *
 * @details Each builder's expressions are the ones DefaultReportPlugin::write_results used to
 *          evaluate inline, in the same order, so the report prints bit-identical doubles.
 *          Skip rules stay outside the per-row lambdas: they decide the row positions even when
 *          a builder computes a single row.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include "ReportTables.hpp"

#include "../core/SimulationContext.hpp"
#include "../core/UnitConversion.hpp"
#include "../../../include/openswmm/engine/openswmm_statistics.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <utility>

namespace openswmm {
namespace report_tables {
namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

const char* const NodeTypeWords[] = {"JUNCTION", "OUTFALL", "DIVIDER", "STORAGE"};
const char* const LinkTypeWords[] = {"CONDUIT", "PUMP", "ORIFICE", "WEIR", "OUTLET"};

/** @return The report's node type word; JUNCTION outside [0,3]. */
const char* nodeTypeWord(int nt) {
    return (nt >= 0 && nt <= 3) ? NodeTypeWords[nt] : "JUNCTION";
}

/** @return The report's link type word; CONDUIT outside [0,4]. */
const char* linkTypeWord(int lt) {
    return (lt >= 0 && lt <= 4) ? LinkTypeWords[lt] : "CONDUIT";
}

/** @brief write_results' internal→display factors and unit words for this run. */
struct Units {
    explicit Units(const SimulationContext& ctx) {
        const ucf::DisplayUnits du = ucf::DisplayUnits::from(ctx.options);
        si = (du.unit_system == 1);
        Qcf = du.flow;
        depth_vcf = du.raindepth;
        len_ucf = du.length;
        svol_ucf = du.volume;
        // legacy statsrpt.c's own factor for every summary-table volume
        Vcf = si ? 28.317 / 1.0e6 : 7.48 / 1.0e6;
        cont_mass = ucf::Ucf[ucf::MASS][si ? 1 : 0]; // mg -> lb | kg
        flow = du.flow_word;
        vol = du.mvol_word;
        len = du.length_word;
        depth = du.depth_word;
        vel = du.vel_word;
        svol = du.storage_vol_word;
    }
    bool si = false;
    double Qcf = 1.0, depth_vcf = 1.0, len_ucf = 1.0, svol_ucf = 1.0, Vcf = 1.0, cont_mass = 1.0;
    std::string flow, vol, len, depth, vel, svol;
};

/** @brief A row of object @p index with its values; @p date is its maximum's absolute date. */
ReportRow makeRow(int type, int index, std::string label, std::vector<double> values,
                  double date = 0.0) {
    ReportRow r;
    r.object_type = type;
    r.object_index = index;
    r.label = std::move(label);
    r.values = std::move(values);
    r.date = date;
    return r;
}

/** @brief Mark @p columns of @p r as left blank by the report; their values become NaN. */
void blankColumns(ReportRow& r, std::initializer_list<int> columns) {
    for (const int c : columns) {
        r.blank |= std::uint64_t{1} << c;
        r.values[static_cast<std::size_t>(c)] = kNaN;
    }
}

/**
 * @brief List the table's next row, computing it with @p make only when it was asked for.
 * @param only_row kAllRows, kNoRows or the zero-based row wanted.
 */
template <class Make> void listRow(ReportTable& t, int only_row, Make&& make) {
    if (only_row == kAllRows || only_row == t.row_count)
        t.rows.push_back(make());
    ++t.row_count;
}

} // namespace

bool routingReported(const SimulationContext& ctx) {
    // legacy statsrpt_writeReport writes the flow-routing tables only when
    // there are links and routing is not ignored (statsrpt.c:117).
    return ctx.n_links() > 0 && !ctx.options.ignore_routing;
}

bool runoffReported(const SimulationContext& ctx) {
    // legacy statsrpt_writeReport writes the runoff tables (runoff, LID
    // performance, groundwater, washoff) only when rainfall, snowmelt or
    // groundwater is analyzed (statsrpt.c:103-113).
    const auto& opt = ctx.options;
    return ctx.n_subcatches() > 0 &&
           (!opt.ignore_rainfall || (ctx.snowpack_names.size() > 0 && !opt.ignore_snow_melt) ||
            (ctx.aquifer_names.size() > 0 && !opt.ignore_groundwater));
}

double elapsedDays(double date, double report_start) {
    // The cases in which elapsedToParts() in the report writer prints 0 00:00.
    if (date <= 0.0)
        return 0.0;
    const double x = date - report_start;
    if (x <= 0.0)
        return 0.0;
    return x;
}

// ---------------------------------------------------------------------------
// Node Depth Summary — legacy writeNodeDepths()
// ---------------------------------------------------------------------------
ReportTable nodeDepth(const SimulationContext& ctx, int only_row) {
    const Units u(ctx);
    ReportTable t;
    t.title = "Node Depth Summary";
    t.columns = {{"Average Depth", u.len},
                 {"Maximum Depth", u.len},
                 {"Maximum HGL", u.len},
                 {"Time of Max Occurrence", "days"},
                 {"Reported Max Depth", u.len}};
    t.reported = routingReported(ctx);
    if (!t.reported)
        return t;

    long report_steps = ctx.routing_stats.report_steps;
    report_steps = std::max(report_steps, 1L);

    for (int j = 0; j < ctx.n_nodes(); ++j)
        listRow(t, only_row, [&] {
            auto uj = static_cast<std::size_t>(j);
            int nt = static_cast<int>(ctx.nodes.type[uj]);
            double max_d_int = ctx.nodes.stat_max_depth[uj];
            double avg_d =
                ctx.nodes.stat_sum_depth[uj] / static_cast<double>(report_steps) * u.len_ucf;
            double max_d = max_d_int * u.len_ucf;
            double max_hgl = (ctx.nodes.invert_elev[uj] + max_d_int) * u.len_ucf;
            double rpt_max = ctx.nodes.stat_max_rpt_depth[uj]; // already display units
            const double date = ctx.nodes.stat_max_depth_date[uj];
            return makeRow(SWMM_SUMMARY_OBJECT_NODE, j, nodeTypeWord(nt),
                           {avg_d, max_d, max_hgl, elapsedDays(date, ctx.options.report_start),
                            rpt_max},
                           date);
        });
    return t;
}

// ---------------------------------------------------------------------------
// Node Inflow Summary — legacy writeNodeFlows()
// ---------------------------------------------------------------------------
ReportTable nodeInflow(const SimulationContext& ctx, int only_row) {
    const Units u(ctx);
    ReportTable t;
    t.title = "Node Inflow Summary";
    t.columns = {{"Maximum Lateral Inflow", u.flow},
                 {"Maximum Total Inflow", u.flow},
                 {"Time of Max Occurrence", "days"},
                 {"Lateral Inflow Volume", u.vol},
                 {"Total Inflow Volume", u.vol},
                 {"Flow Balance Error", "%"},
                 {"Flow Balance Error Volume", u.si ? "ltr" : "gal"}};
    t.reported = routingReported(ctx);
    if (!t.reported)
        return t;

    for (int j = 0; j < ctx.n_nodes(); ++j)
        listRow(t, only_row, [&] {
            auto uj = static_cast<std::size_t>(j);
            int nt = static_cast<int>(ctx.nodes.type[uj]);

            double max_lat = ctx.nodes.stat_max_lat_inflow[uj] * u.Qcf;
            double max_tot = ctx.nodes.stat_max_total_inflow[uj] * u.Qcf;
            double vol_lat = ctx.nodes.stat_lat_inflow_vol[uj] * u.Vcf;
            double vol_tot = ctx.nodes.stat_total_inflow_vol[uj] * u.Vcf;
            const double date = ctx.nodes.stat_max_inflow_date[uj];
            // Flow balance error, legacy writeNodeFlows: outflow includes the
            // final stored volume (massbal_getStorage(TRUE) runs first); below
            // 1 ft3 of outflow the volume difference is printed instead.
            const double in_v = ctx.nodes.stat_total_inflow_vol[uj];
            const double out_v = ctx.nodes.stat_total_outflow_vol[uj] + ctx.nodes.volume[uj];
            ReportRow r = makeRow(SWMM_SUMMARY_OBJECT_NODE, j, nodeTypeWord(nt),
                                  {max_lat, max_tot, elapsedDays(date, ctx.options.report_start),
                                   vol_lat, vol_tot, kNaN, kNaN},
                                  date);
            if (std::fabs(out_v) < 1.0) {
                r.values[6] = (in_v - out_v) * u.Vcf * 1.0e6;
                blankColumns(r, {5});
            } else {
                r.values[5] = (in_v - out_v) / out_v * 100.0;
                blankColumns(r, {6});
            }
            return r;
        });
    return t;
}

// ---------------------------------------------------------------------------
// Node Surcharge Summary — legacy writeNodeSurcharge()
// ---------------------------------------------------------------------------
ReportTable nodeSurcharge(const SimulationContext& ctx, int only_row) {
    const Units u(ctx);
    ReportTable t;
    t.title = "Node Surcharge Summary";
    t.columns = {{"Hours Surcharged", "hours"},
                 {"Max. Height Above Crown", u.len},
                 {"Min. Depth Below Rim", u.len}};
    t.reported = routingReported(ctx) && static_cast<int>(ctx.options.routing_model) == 2;
    if (!t.reported)
        return t;

    // legacy writeNodeSurcharge: outfalls skipped; hours floored at 0.01;
    // height above the highest conduit crown and depth below the rim,
    // both from the node's maximum depth.
    for (int j = 0; j < ctx.n_nodes(); ++j) {
        auto uj = static_cast<std::size_t>(j);
        if (ctx.nodes.type[uj] == NodeType::OUTFALL)
            continue;
        if (ctx.nodes.stat_time_surcharged[uj] == 0.0)
            continue;
        listRow(t, only_row, [&] {
            const double hours = std::max(0.01, ctx.nodes.stat_time_surcharged[uj] / 3600.0);
            const double maxd = ctx.nodes.stat_max_depth[uj];
            const double d1 =
                std::max(0.0, maxd + ctx.nodes.invert_elev[uj] - ctx.nodes.crown_elev[uj]);
            const double d2 = std::max(0.0, ctx.nodes.full_depth[uj] - maxd);
            return makeRow(SWMM_SUMMARY_OBJECT_NODE, j,
                           nodeTypeWord(static_cast<int>(ctx.nodes.type[uj])),
                           {hours, d1 * u.len_ucf, d2 * u.len_ucf});
        });
    }
    return t;
}

// ---------------------------------------------------------------------------
// Node Flooding Summary — legacy writeNodeFlooding()
// ---------------------------------------------------------------------------
ReportTable nodeFlooding(const SimulationContext& ctx, int only_row) {
    const Units u(ctx);
    ReportTable t;
    t.title = "Node Flooding Summary";
    t.columns = {{"Hours Flooded", "hours"},
                 {"Maximum Rate", u.flow},
                 {"Time of Max Occurrence", "days"},
                 {"Total Flood Volume", u.vol},
                 {"Maximum Ponded Depth", u.len}};
    t.reported = routingReported(ctx);
    if (!t.reported)
        return t;

    bool any_flooding = false;
    for (int j = 0; j < ctx.n_nodes(); ++j)
        if (ctx.nodes.stat_time_flooded[static_cast<std::size_t>(j)] > 0.0) {
            any_flooding = true;
            break;
        }
    if (!any_flooding)
        return t; // "No nodes were flooded."

    for (int j = 0; j < ctx.n_nodes(); ++j) {
        auto uj = static_cast<std::size_t>(j);
        if (ctx.nodes.stat_time_flooded[uj] <= 0.0)
            continue;
        listRow(t, only_row, [&] {
            double hours = std::max(0.01, ctx.nodes.stat_time_flooded[uj] / 3600.0);
            double max_rate = ctx.nodes.stat_max_overflow[uj] * u.Qcf;
            double vol_mgal = ctx.nodes.stat_vol_flooded[uj] * u.Vcf;
            const double date = ctx.nodes.stat_max_overflow_date[uj];
            // Ponded depth = max depth - full depth (for DW only)
            double ponded = 0.0;
            if (static_cast<int>(ctx.options.routing_model) == 2) {
                ponded = (ctx.nodes.stat_max_depth[uj] - ctx.nodes.full_depth[uj]);
                ponded = std::max(ponded, 0.0);
            }
            return makeRow(SWMM_SUMMARY_OBJECT_NODE, j, "",
                           {hours, max_rate, elapsedDays(date, ctx.options.report_start),
                            vol_mgal, ponded * u.len_ucf},
                           date);
        });
    }
    return t;
}

// ---------------------------------------------------------------------------
// Storage Volume Summary — legacy writeStorageVolumes()
// ---------------------------------------------------------------------------
ReportTable storageVolume(const SimulationContext& ctx, int only_row) {
    const Units u(ctx);
    ReportTable t;
    t.title = "Storage Volume Summary";
    t.columns = {{"Average Volume", u.svol},          {"Avg Pcnt Full", "%"},
                 {"Evap Pcnt Loss", "%"},             {"Exfil Pcnt Loss", "%"},
                 {"Maximum Volume", u.svol},          {"Max Pcnt Full", "%"},
                 {"Time of Max Occurrence", "days"}, {"Maximum Outflow", u.flow}};
    bool any_storage = false;
    for (int j = 0; j < ctx.n_nodes(); ++j)
        if (ctx.nodes.type[static_cast<std::size_t>(j)] == NodeType::STORAGE) {
            any_storage = true;
            break;
        }
    t.reported = routingReported(ctx) && any_storage;
    if (!t.reported)
        return t;

    long report_steps = ctx.routing_stats.report_steps;
    report_steps = std::max(report_steps, 1L);

    for (int j = 0; j < ctx.n_nodes(); ++j) {
        auto uj = static_cast<std::size_t>(j);
        if (ctx.nodes.type[uj] != NodeType::STORAGE)
            continue;
        listRow(t, only_row, [&] {
            const double full_vol = ctx.nodes.full_volume[uj];
            // Volume peaks can occur before depth peaks once full storage
            // has been reached, so use the dedicated volume statistics.
            const double avg_vol_ft3 =
                ctx.nodes.stat_sum_volume[uj] / static_cast<double>(report_steps);
            const double max_vol_ft3 = ctx.nodes.stat_storage_max_volume[uj];

            // Percent full is by VOLUME (legacy semantics), not by depth.
            double pct_avg = (full_vol > 0.0) ? avg_vol_ft3 / full_vol * 100.0 : 0.0;
            double pct_max = (full_vol > 0.0) ? max_vol_ft3 / full_vol * 100.0 : 0.0;

            // ft³ → 1000 ft³|m³
            const double avg_vol = avg_vol_ft3 * u.svol_ucf / 1000.0;
            const double max_vol = max_vol_ft3 * u.svol_ucf / 1000.0;

            const double date = ctx.nodes.stat_storage_max_volume_date[uj];
            double max_outflow = ctx.nodes.stat_storage_max_outflow[uj] * u.Qcf;

            // Evaporation and exfiltration losses print as 0.0.
            return makeRow(SWMM_SUMMARY_OBJECT_NODE, j, "",
                           {avg_vol, pct_avg, 0.0, 0.0, max_vol, pct_max,
                            elapsedDays(date, ctx.options.report_start), max_outflow},
                           date);
        });
    }
    return t;
}

// ---------------------------------------------------------------------------
// Outfall Loading Summary — legacy writeOutfallLoads()
// ---------------------------------------------------------------------------
ReportTable outfallLoading(const SimulationContext& ctx, int only_row) {
    const Units u(ctx);
    const int np = ctx.n_pollutants();
    ReportTable t;
    t.title = "Outfall Loading Summary";
    t.columns = {{"Flow Freq", "%"},
                 {"Avg Flow", u.flow},
                 {"Max Flow", u.flow},
                 {"Total Volume", u.vol}};
    // legacy LoadUnitsWords: lbs | kg, LogN for a count pollutant
    for (int p = 0; p < np; ++p)
        t.columns.push_back(
            {ctx.pollutant_names.name_of(p),
             ctx.pollutants.units[static_cast<std::size_t>(p)] == MassUnits::COUNTS_PER_L
                 ? "LogN"
                 : (u.si ? "kg" : "lbs")});

    // legacy writeOutfallLoads: only when there are outfalls
    bool any_outfall = false;
    for (int j = 0; j < ctx.n_nodes() && !any_outfall; ++j)
        any_outfall = ctx.nodes.type[static_cast<std::size_t>(j)] == NodeType::OUTFALL;
    t.reported = routingReported(ctx) && any_outfall;
    if (!t.reported)
        return t;

    double sys_vol = 0.0;
    double sys_flow_sum = 0.0;
    double sys_freq_sum = 0.0;
    int outfall_count = 0;
    std::vector<double> pol_totals(static_cast<std::size_t>(np), 0.0);
    long total_steps = ctx.routing_stats.report_steps;
    total_steps = std::max(total_steps, 1L);

    // Every outfall's numbers are computed: the System row sums them.
    for (int j = 0; j < ctx.n_nodes(); ++j) {
        auto uj = static_cast<std::size_t>(j);
        if (ctx.nodes.type[uj] != NodeType::OUTFALL)
            continue;
        outfall_count++;

        long periods = ctx.nodes.stat_outfall_periods[uj];
        double avg_flow = (periods > 0) ? ctx.nodes.stat_outfall_avg_flow[uj] * u.Qcf /
                                              static_cast<double>(periods)
                                        : 0.0;
        double max_flow = ctx.nodes.stat_outfall_max_flow[uj] * u.Qcf;
        double freq = 100.0 * static_cast<double>(periods) / static_cast<double>(total_steps);
        double vol = ctx.nodes.stat_total_inflow_vol[uj] * u.Vcf;

        sys_freq_sum += freq;
        sys_flow_sum += avg_flow;
        sys_vol += vol;

        std::vector<double> values{freq, avg_flow, max_flow, vol};
        auto base = uj * static_cast<std::size_t>(np);
        for (int p = 0; p < np; ++p) {
            auto idx = base + static_cast<std::size_t>(p);
            // legacy: totalLoad * LperFT3 * Pollut.mcf, LOG10 for counts
            const auto mu = ctx.pollutants.units[static_cast<std::size_t>(p)];
            const double mcf = (mu == MassUnits::MG_PER_L)   ? u.cont_mass
                               : (mu == MassUnits::UG_PER_L) ? u.cont_mass / 1000.0
                                                             : 1.0;
            double load =
                ((idx < ctx.nodes.stat_total_load.size()) ? ctx.nodes.stat_total_load[idx]
                                                          : 0.0) *
                28.317 * mcf;
            pol_totals[static_cast<std::size_t>(p)] += load;
            if (mu == MassUnits::COUNTS_PER_L && load > 0.0)
                load = std::log10(load);
            values.push_back(load);
        }
        listRow(t, only_row,
                [&] { return makeRow(SWMM_SUMMARY_OBJECT_NODE, j, "", std::move(values)); });
    }

    // System totals
    listRow(t, only_row, [&] {
        double sys_freq = (outfall_count > 0) ? sys_freq_sum / outfall_count : 0.0;
        // legacy MaxOutfallFlow: the peak of the summed outfall inflow
        std::vector<double> values{sys_freq, sys_flow_sum,
                                   ctx.routing_stats.max_outfall_flow * u.Qcf, sys_vol};
        for (int p = 0; p < np; ++p) {
            double x = pol_totals[static_cast<std::size_t>(p)];
            if (ctx.pollutants.units[static_cast<std::size_t>(p)] == MassUnits::COUNTS_PER_L &&
                x > 0.0)
                x = std::log10(x);
            values.push_back(x);
        }
        return makeRow(SWMM_SUMMARY_OBJECT_SYSTEM, -1, "", std::move(values));
    });
    return t;
}

// ---------------------------------------------------------------------------
// Link Flow Summary — legacy writeLinkFlows()
// ---------------------------------------------------------------------------
ReportTable linkFlow(const SimulationContext& ctx, int only_row) {
    const Units u(ctx);
    ReportTable t;
    t.title = "Link Flow Summary";
    t.columns = {{"Maximum |Flow|", u.flow},
                 {"Time of Max Occurrence", "days"},
                 {"Maximum |Veloc|", u.vel},
                 {"Max/Full Flow", ""},
                 {"Max/Full Depth", ""}};
    t.reported = routingReported(ctx);
    if (!t.reported)
        return t;

    for (int j = 0; j < ctx.n_links(); ++j)
        listRow(t, only_row, [&] {
            auto uj = static_cast<std::size_t>(j);
            int lt = static_cast<int>(ctx.links.type[uj]);

            double mf = ctx.links.stat_max_flow[uj] * u.Qcf;      // CFS → display
            double mv = ctx.links.stat_max_veloc[uj] * u.len_ucf; // ft/s → display
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

            const double date = ctx.links.stat_max_flow_date[uj];

            // Row layout of legacy writeLinkFlows (statsrpt.c). A link with no
            // [XSECTIONS] row keeps xsect.type DUMMY (0) there — pumps and
            // outlets — and an IRREGULAR section prints CHANNEL.
            const auto shape = ctx.links.xsect_shape[uj];
            const bool pump = lt == static_cast<int>(LinkType::PUMP);
            const bool dummy =
                !pump && (shape == XsectShape::DUMMY || lt == static_cast<int>(LinkType::OUTLET));
            ReportRow r = makeRow(SWMM_SUMMARY_OBJECT_LINK, j,
                                  dummy                              ? "DUMMY"
                                  : shape == XsectShape::IRREGULAR   ? "CHANNEL"
                                                                     : linkTypeWord(lt),
                                  {mf, elapsedDays(date, ctx.options.report_start), mv,
                                   flow_ratio, fill},
                                  date);

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
                            q_full = *std::max_element(ty.begin(), ty.end()) / u.Qcf;
                    }
                }
                if (q_full > 0.0) {
                    r.values[3] = ctx.links.stat_max_flow[uj] / q_full;
                    blankColumns(r, {2, 4});
                    r.truncated = true;
                    return r;
                }
            }
            if (dummy || pump) {
                blankColumns(r, {2, 3, 4});
                r.truncated = true;
                return r;
            }

            if (lt != static_cast<int>(LinkType::CONDUIT))
                blankColumns(r, {2, 3});

            // Max/full depth; a bottom orifice has no full depth.
            bool no_full_depth = ctx.links.xsect_y_full[uj] <= 0.0;
            if (lt == static_cast<int>(LinkType::ORIFICE)) {
                const int orow = ctx.link_subtypes.orifice_row(j);
                if (orow >= 0 &&
                    ctx.link_subtypes.orifices.orifice_type[static_cast<std::size_t>(orow)] == 0.0)
                    no_full_depth = true;
            }
            if (no_full_depth)
                blankColumns(r, {4});
            return r;
        });
    return t;
}

// ---------------------------------------------------------------------------
// Flow Classification Summary — legacy writeFlowClass()
// ---------------------------------------------------------------------------
ReportTable flowClassification(const SimulationContext& ctx, int only_row) {
    static_assert(LinkData::N_FLOW_CLASSES == 7, "one column per flow class");
    ReportTable t;
    t.title = "Flow Classification Summary";
    t.columns = {{"Adjusted/Actual Length", ""},
                 {"Dry", ""},
                 {"Up Dry", ""},
                 {"Down Dry", ""},
                 {"Sub Crit", ""},
                 {"Sup Crit", ""},
                 {"Up Crit", ""},
                 {"Down Crit", ""},
                 {"Norm Ltd", ""},
                 {"Inlet Ctrl", ""}};
    // Dynamic wave only in legacy (FV, a v6 router, keeps the table);
    // fractions of the routed time after the report start.
    t.reported = routingReported(ctx) && (ctx.options.routing_model == RoutingModel::DYNWAVE ||
                                          ctx.options.routing_model == RoutingModel::FV);
    if (!t.reported)
        return t;

    const double total_secs = ctx.routing_stats.report_time;

    for (int j = 0; j < ctx.n_links(); ++j) {
        auto uj = static_cast<std::size_t>(j);
        if (ctx.links.type[uj] != LinkType::CONDUIT)
            continue;
        if (ctx.links.xsect_shape[uj] == XsectShape::DUMMY)
            continue;
        listRow(t, only_row, [&] {
            // Adjusted/actual length ratio
            double len_ratio = 1.0;
            const int cr = ctx.link_subtypes.conduit_row(j);
            const auto& CD = ctx.link_subtypes.conduits;
            const double L = (cr >= 0) ? CD.length[static_cast<size_t>(cr)] : 0.0;
            if (L > 0.0)
                len_ratio = ((cr >= 0) ? CD.mod_length[static_cast<size_t>(cr)] : 0.0) / L;

            std::vector<double> values{len_ratio};
            for (int c = 0; c < LinkData::N_FLOW_CLASSES; ++c) {
                auto idx = uj * LinkData::N_FLOW_CLASSES + static_cast<std::size_t>(c);
                const double tc =
                    (idx < ctx.links.stat_flow_class.size()) ? ctx.links.stat_flow_class[idx] : 0.0;
                values.push_back(tc / total_secs);
            }
            values.push_back(ctx.links.stat_norm_ltd[uj] / total_secs);
            values.push_back(ctx.links.stat_inlet_ctrl[uj] / total_secs);
            return makeRow(SWMM_SUMMARY_OBJECT_LINK, j, "", std::move(values));
        });
    }
    return t;
}

// ---------------------------------------------------------------------------
// Conduit Surcharge Summary — legacy writeLinkSurcharge()
// ---------------------------------------------------------------------------
ReportTable conduitSurcharge(const SimulationContext& ctx, int only_row) {
    ReportTable t;
    t.title = "Conduit Surcharge Summary";
    t.columns = {{"Hours Full Both Ends", "hours"},
                 {"Hours Full Upstream", "hours"},
                 {"Hours Full Dnstream", "hours"},
                 {"Hours Above Full Normal Flow", "hours"},
                 {"Hours Capacity Limited", "hours"}};
    t.reported = routingReported(ctx);
    if (!t.reported)
        return t;

    // legacy writeLinkSurcharge: true conduits whose four hour totals are
    // not all zero; each value floored at 0.01 h.
    for (int j = 0; j < ctx.n_links(); ++j) {
        auto uj = static_cast<std::size_t>(j);
        if (ctx.links.type[uj] != LinkType::CONDUIT ||
            ctx.links.xsect_shape[uj] == XsectShape::DUMMY)
            continue;
        const double h[4] = {ctx.links.stat_time_surcharged[uj] / 3600.0,
                             ctx.links.stat_time_full_upstream[uj] / 3600.0,
                             ctx.links.stat_time_full_dnstream[uj] / 3600.0,
                             ctx.links.stat_time_full_both[uj] / 3600.0};
        if (h[0] + h[1] + h[2] + h[3] == 0.0)
            continue;
        listRow(t, only_row, [&] {
            std::vector<double> values{h[0], h[1], h[2], h[3],
                                       ctx.links.stat_time_capacity_limited[uj] / 3600.0};
            for (double& x : values)
                x = std::max(0.01, x);
            return makeRow(SWMM_SUMMARY_OBJECT_LINK, j, "", std::move(values));
        });
    }
    return t;
}

// ---------------------------------------------------------------------------
// Pumping Summary — legacy writePumpFlows()
// ---------------------------------------------------------------------------
ReportTable pumping(const SimulationContext& ctx, int only_row) {
    const Units u(ctx);
    ReportTable t;
    t.title = "Pumping Summary";
    t.columns = {{"Percent Utilized", "%"},
                 {"Number of Start-Ups", ""},
                 {"Min Flow", u.flow},
                 {"Avg Flow", u.flow},
                 {"Max Flow", u.flow},
                 {"Total Volume", u.vol},
                 {"Power Usage", "Kw-hr"},
                 {"Time Off Pump Curve Low", "%"},
                 {"Time Off Pump Curve High", "%"}};
    bool any_pump = false;
    for (int j = 0; j < ctx.n_links(); ++j)
        if (ctx.links.type[static_cast<std::size_t>(j)] == LinkType::PUMP) {
            any_pump = true;
            break;
        }
    t.reported = routingReported(ctx) && any_pump;
    if (!t.reported)
        return t;

    double totalSeconds = ctx.routing_stats.report_time;

    for (int j = 0; j < ctx.n_links(); ++j) {
        auto uj = static_cast<std::size_t>(j);
        if (ctx.links.type[uj] != LinkType::PUMP)
            continue;
        listRow(t, only_row, [&] {
            double max_flow = ctx.links.stat_max_flow[uj] * u.Qcf;
            double vol = ctx.links.stat_pump_volume[uj] * u.Vcf;
            double on_time = ctx.links.stat_pump_on_time[uj];
            double pctUtilized = (totalSeconds > 0.0) ? on_time / totalSeconds * 100.0 : 0.0;
            int startUps = ctx.links.stat_pump_cycles[uj];
            double avgFlow =
                (ctx.links.stat_pump_periods[uj] > 0)
                    ? ctx.links.stat_pump_sum_flow[uj] / ctx.links.stat_pump_periods[uj] * u.Qcf
                    : 0.0;
            double energyKwh = ctx.links.stat_pump_energy[uj];
            // The minimum flow prints as 0.0; start-ups are exact as a double.
            return makeRow(
                SWMM_SUMMARY_OBJECT_LINK, j, "",
                {pctUtilized, static_cast<double>(startUps), 0.0, avgFlow, max_flow, vol,
                 energyKwh,
                 on_time > 0.0 ? ctx.links.stat_pump_off_curve_low[uj] / on_time * 100.0 : 0.0,
                 on_time > 0.0 ? ctx.links.stat_pump_off_curve_high[uj] / on_time * 100.0 : 0.0});
        });
    }
    return t;
}

// ---------------------------------------------------------------------------
// LID Performance Summary — legacy writeLidPerformance()
// wb_* fields are in ft depth; converted to in | mm.
// ---------------------------------------------------------------------------
ReportTable lidPerformance(const SimulationContext& ctx, const LidUsageStore& usage,
                           int only_row) {
    const Units u(ctx);
    ReportTable t;
    t.title = "LID Performance Summary";
    t.columns = {{"Total Inflow", u.depth},
                 {"Evap Loss", u.depth},
                 {"Infil Loss", u.depth},
                 {"Surface Runoff", u.depth},
                 {"Drain Flow", u.depth}};
    t.reported = runoffReported(ctx) && usage.count() > 0;
    if (!t.reported)
        return t;

    int n_usage = usage.count();
    bool has_lids = false;
    for (int j = 0; j < n_usage; ++j) {
        auto uj = static_cast<std::size_t>(j);
        if (uj < usage.wb_inflow.size() &&
            (usage.wb_inflow[uj] > 0.0 || usage.wb_drain_flow[uj] > 0.0 ||
             usage.wb_surf_flow[uj] > 0.0)) {
            has_lids = true;
            break;
        }
    }
    if (!has_lids)
        return t; // "No LID performance data."

    for (int j = 0; j < n_usage; ++j)
        listRow(t, only_row, [&] {
            auto uj = static_cast<std::size_t>(j);
            int li = usage.lid_index[uj];
            int si = usage.subcatch_index[uj];

            // Water-balance depths: ft → in | mm (legacy UCF(RAINDEPTH)).
            auto safe = [&](const std::vector<double>& v) -> double {
                return (uj < v.size()) ? v[uj] * u.depth_vcf : 0.0;
            };
            return makeRow(SWMM_SUMMARY_OBJECT_SUBCATCH, si, ctx.lid_names.name_of(li),
                           {safe(usage.wb_inflow), safe(usage.wb_evap), safe(usage.wb_infil),
                            safe(usage.wb_surf_flow), safe(usage.wb_drain_flow)});
        });
    return t;
}

ReportTable build(const SimulationContext& ctx, int table, const LidUsageStore& lid_usage,
                  int only_row) {
    switch (table) {
    case SWMM_SUMMARY_NODE_DEPTH:
        return nodeDepth(ctx, only_row);
    case SWMM_SUMMARY_NODE_INFLOW:
        return nodeInflow(ctx, only_row);
    case SWMM_SUMMARY_NODE_SURCHARGE:
        return nodeSurcharge(ctx, only_row);
    case SWMM_SUMMARY_NODE_FLOODING:
        return nodeFlooding(ctx, only_row);
    case SWMM_SUMMARY_STORAGE_VOLUME:
        return storageVolume(ctx, only_row);
    case SWMM_SUMMARY_OUTFALL_LOADING:
        return outfallLoading(ctx, only_row);
    case SWMM_SUMMARY_LINK_FLOW:
        return linkFlow(ctx, only_row);
    case SWMM_SUMMARY_FLOW_CLASSIFICATION:
        return flowClassification(ctx, only_row);
    case SWMM_SUMMARY_CONDUIT_SURCHARGE:
        return conduitSurcharge(ctx, only_row);
    case SWMM_SUMMARY_PUMPING:
        return pumping(ctx, only_row);
    case SWMM_SUMMARY_LID_PERFORMANCE:
        return lidPerformance(ctx, lid_usage, only_row);
    default:
        return {};
    }
}

} // namespace report_tables
} // namespace openswmm
