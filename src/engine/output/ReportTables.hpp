// SPDX-License-Identifier: Apache-2.0

/**
 * @file      ReportTables.hpp
 * @brief     The end-of-run report summary tables as data.
 *
 * @details Each builder holds the row computation of one `.rpt` summary section. The report
 *          writer (DefaultReportPlugin::write_results) prints its section from the rows a
 *          builder returns, and the swmm_summary_* C API (openswmm_statistics.h) reads the same
 *          rows, so the two can never disagree. Values are in the report's display units.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace openswmm {

struct SimulationContext;
struct LidUsageStore;

/** @brief One value column: the report's stacked header words and its unit word. */
struct ReportColumn {
    std::string heading; ///< e.g. "Maximum Total Inflow"
    std::string units;   ///< e.g. "CFS", "10^6 gal", "%", "days"; "" when dimensionless
};

/** @brief One row of a summary table, as the report prints it. */
struct ReportRow {
    int object_type = 0;   ///< SWMM_SUMMARY_OBJECT_* (-1 for a System row)
    int object_index = -1; ///< node, link or subcatchment index; -1 for a System row
    std::string label;     ///< the report's text column: node/link type, LID control name, or ""
    std::vector<double> values; ///< one per column; NaN where the report prints nothing
    std::uint64_t blank = 0;    ///< bit c set: the report leaves column c blank
    bool truncated = false; ///< the report ends the line after its last printed column
    double date = 0.0;      ///< absolute date of the row's maximum, as elapsedToParts() takes it

    /** @return Whether the report prints column @p column of this row. */
    bool shows(int column) const { return ((blank >> column) & 1u) == 0; }
};

/** @brief One summary table: its title, columns and rows. */
struct ReportTable {
    std::string title;
    bool reported = false; ///< whether the report prints this section for this run
    std::vector<ReportColumn> columns;
    int row_count = 0;            ///< rows the report lists, System rows included
    std::vector<ReportRow> rows;  ///< all of them, or only the one a builder was asked for
};

namespace report_tables {

/// Builders compute every row.
constexpr int kAllRows = -1;
/// Builders compute no row, only the title, columns and row count.
constexpr int kNoRows = -2;

/** @return Whether the report writes its flow-routing tables (links present, routing on). */
bool routingReported(const SimulationContext& ctx);

/** @return Whether the report writes its runoff tables (rainfall, snowmelt or groundwater). */
bool runoffReported(const SimulationContext& ctx);

/**
 * @brief Elapsed days from @p report_start to @p date, as the report's "days hr:min" splits it.
 * @return 0 when @p date is not positive or not after @p report_start (the report prints
 *     0 00:00 then); otherwise date - report_start.
 */
double elapsedDays(double date, double report_start);

/**
 * @name Builders, one per report section, in report order.
 * @param ctx Engine context after end().
 * @param only_row kAllRows, kNoRows, or the zero-based row to compute; every skip rule still
 *     runs, so row_count and the row's position match the full table.
 * @{
 */
ReportTable nodeDepth(const SimulationContext& ctx, int only_row = kAllRows);
ReportTable nodeInflow(const SimulationContext& ctx, int only_row = kAllRows);
ReportTable nodeSurcharge(const SimulationContext& ctx, int only_row = kAllRows);
ReportTable nodeFlooding(const SimulationContext& ctx, int only_row = kAllRows);
ReportTable storageVolume(const SimulationContext& ctx, int only_row = kAllRows);
ReportTable outfallLoading(const SimulationContext& ctx, int only_row = kAllRows);
ReportTable linkFlow(const SimulationContext& ctx, int only_row = kAllRows);
ReportTable flowClassification(const SimulationContext& ctx, int only_row = kAllRows);
ReportTable conduitSurcharge(const SimulationContext& ctx, int only_row = kAllRows);
ReportTable pumping(const SimulationContext& ctx, int only_row = kAllRows);
/** @param usage LID usage whose wb_* water balance is final (ctx.lid_usage after report()). */
ReportTable lidPerformance(const SimulationContext& ctx, const LidUsageStore& usage,
                           int only_row = kAllRows);
/** @} */

/**
 * @brief Build table @p table (a SWMM_SummaryTable value in [0, SWMM_SUMMARY_TABLE_COUNT)).
 * @param lid_usage As lidPerformance().
 */
ReportTable build(const SimulationContext& ctx, int table, const LidUsageStore& lid_usage,
                  int only_row = kAllRows);

} // namespace report_tables
} // namespace openswmm
