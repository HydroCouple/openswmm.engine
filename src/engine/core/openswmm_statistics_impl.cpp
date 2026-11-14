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
 * @file      openswmm_statistics_impl.cpp
 * @brief     C API implementation — node, link, subcatchment statistics queries.
 *
 * @see include/openswmm/engine/openswmm_statistics.h
 * @ingroup engine_api
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include "openswmm_api_common.hpp"
#include "../output/ReportTables.hpp"
#include "../../../include/openswmm/engine/openswmm_statistics.h"

#include <new>
#include <string>

extern "C" {

// ============================================================================
// Node Statistics
// ============================================================================

SWMM_ENGINE_API int swmm_stat_node_max_depth(SWMM_Engine engine, int idx, double* val) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    if (val)
        *val = ctx.nodes.stat_max_depth[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_stat_node_max_overflow(SWMM_Engine engine, int idx, double* val) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    if (val)
        *val = ctx.nodes.stat_max_overflow[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_stat_node_vol_flooded(SWMM_Engine engine, int idx, double* val) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    if (val)
        *val = ctx.nodes.stat_vol_flooded[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_stat_node_time_flooded(SWMM_Engine engine, int idx, double* val) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    if (val)
        *val = ctx.nodes.stat_time_flooded[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

// ============================================================================
// Link Statistics
// ============================================================================

SWMM_ENGINE_API int swmm_stat_link_max_flow(SWMM_Engine engine, int idx, double* val) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    if (val)
        *val = ctx.links.stat_max_flow[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_stat_link_max_velocity(SWMM_Engine engine, int idx, double* val) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    if (val)
        *val = ctx.links.stat_max_veloc[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_stat_link_max_filling(SWMM_Engine engine, int idx, double* val) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    if (val)
        *val = ctx.links.stat_max_filling[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_stat_link_vol_flow(SWMM_Engine engine, int idx, double* val) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    if (val)
        *val = ctx.links.stat_vol_flow[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_stat_link_surcharge_time(SWMM_Engine engine, int idx, double* val) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    if (val)
        *val = ctx.links.stat_time_surcharged[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

// ============================================================================
// Subcatchment Statistics
// ============================================================================

SWMM_ENGINE_API int swmm_stat_subcatch_precip(SWMM_Engine engine, int idx, double* val) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_subcatches());
    if (val)
        *val = ctx.subcatches.stat_precip_vol[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_stat_subcatch_runoff_vol(SWMM_Engine engine, int idx, double* val) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_subcatches());
    if (val)
        *val = ctx.subcatches.stat_runoff_vol[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_stat_subcatch_max_runoff(SWMM_Engine engine, int idx, double* val) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_subcatches());
    if (val)
        *val = ctx.subcatches.stat_max_runoff[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

// ============================================================================
// Bulk Statistics
// ============================================================================

SWMM_ENGINE_API int swmm_stat_node_max_depth_bulk(SWMM_Engine engine, double* buf, int count) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    if (!buf || count <= 0)
        return SWMM_ERR_BADPARAM;
    const int n = std::min(count, ctx.n_nodes());
    std::copy(ctx.nodes.stat_max_depth.begin(), ctx.nodes.stat_max_depth.begin() + n, buf);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_stat_link_max_flow_bulk(SWMM_Engine engine, double* buf, int count) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    if (!buf || count <= 0)
        return SWMM_ERR_BADPARAM;
    const int n = std::min(count, ctx.n_links());
    std::copy(ctx.links.stat_max_flow.begin(), ctx.links.stat_max_flow.begin() + n, buf);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_stat_subcatch_runoff_vol_bulk(SWMM_Engine engine, double* buf, int count) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    if (!buf || count <= 0)
        return SWMM_ERR_BADPARAM;
    const int n = std::min(count, ctx.n_subcatches());
    std::copy(ctx.subcatches.stat_runoff_vol.begin(), ctx.subcatches.stat_runoff_vol.begin() + n,
              buf);
    return SWMM_OK;
}

// ----------------------------------------------------------------------------
// Phase 3 statistics bulk getters — node max_overflow, vol_flooded,
// time_flooded; subcatch max_runoff.
//
// All four are simple SoA memcpys; the rationale for adding them now is that
// they are the four most-hit scalar getters in the MCP server's flooding /
// capacity summary tools. Per-element loops there pay 4N ABI crossings; the
// bulk variants compress that to 4 single-pass copies.
// ----------------------------------------------------------------------------

SWMM_ENGINE_API int swmm_stat_node_max_overflow_bulk(SWMM_Engine engine, double* buf, int count) {
    CHECK_HANDLE(engine);
    if (!buf || count <= 0)
        return SWMM_ERR_BADPARAM;
    const auto& ctx = to_engine(engine)->context();
    const int n = std::min(count, ctx.n_nodes());
    std::copy(ctx.nodes.stat_max_overflow.begin(), ctx.nodes.stat_max_overflow.begin() + n, buf);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_stat_node_vol_flooded_bulk(SWMM_Engine engine, double* buf, int count) {
    CHECK_HANDLE(engine);
    if (!buf || count <= 0)
        return SWMM_ERR_BADPARAM;
    const auto& ctx = to_engine(engine)->context();
    const int n = std::min(count, ctx.n_nodes());
    std::copy(ctx.nodes.stat_vol_flooded.begin(), ctx.nodes.stat_vol_flooded.begin() + n, buf);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_stat_node_time_flooded_bulk(SWMM_Engine engine, double* buf, int count) {
    CHECK_HANDLE(engine);
    if (!buf || count <= 0)
        return SWMM_ERR_BADPARAM;
    const auto& ctx = to_engine(engine)->context();
    const int n = std::min(count, ctx.n_nodes());
    std::copy(ctx.nodes.stat_time_flooded.begin(), ctx.nodes.stat_time_flooded.begin() + n, buf);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_stat_subcatch_max_runoff_bulk(SWMM_Engine engine, double* buf, int count) {
    CHECK_HANDLE(engine);
    if (!buf || count <= 0)
        return SWMM_ERR_BADPARAM;
    const auto& ctx = to_engine(engine)->context();
    const int n = std::min(count, ctx.n_subcatches());
    std::copy(ctx.subcatches.stat_max_runoff.begin(), ctx.subcatches.stat_max_runoff.begin() + n,
              buf);
    return SWMM_OK;
}

// ----------------------------------------------------------------------------
// Phase 4e: link-stat bulks (max_velocity, max_filling, vol_flow,
// surcharge_time).  All simple SoA memcpys from the corresponding scalar
// accessor's column.  Added to complete the per-link statistics surface so
// the MCP server's capacity_summary tool can be collapsed to a single-pass
// shape.
// ----------------------------------------------------------------------------

SWMM_ENGINE_API int swmm_stat_link_max_velocity_bulk(SWMM_Engine engine, double* buf, int count) {
    CHECK_HANDLE(engine);
    if (!buf || count <= 0)
        return SWMM_ERR_BADPARAM;
    const auto& ctx = to_engine(engine)->context();
    const int n = std::min(count, ctx.n_links());
    std::copy(ctx.links.stat_max_veloc.begin(), ctx.links.stat_max_veloc.begin() + n, buf);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_stat_link_max_filling_bulk(SWMM_Engine engine, double* buf, int count) {
    CHECK_HANDLE(engine);
    if (!buf || count <= 0)
        return SWMM_ERR_BADPARAM;
    const auto& ctx = to_engine(engine)->context();
    const int n = std::min(count, ctx.n_links());
    std::copy(ctx.links.stat_max_filling.begin(), ctx.links.stat_max_filling.begin() + n, buf);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_stat_link_vol_flow_bulk(SWMM_Engine engine, double* buf, int count) {
    CHECK_HANDLE(engine);
    if (!buf || count <= 0)
        return SWMM_ERR_BADPARAM;
    const auto& ctx = to_engine(engine)->context();
    const int n = std::min(count, ctx.n_links());
    std::copy(ctx.links.stat_vol_flow.begin(), ctx.links.stat_vol_flow.begin() + n, buf);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_stat_link_surcharge_time_bulk(SWMM_Engine engine, double* buf, int count) {
    CHECK_HANDLE(engine);
    if (!buf || count <= 0)
        return SWMM_ERR_BADPARAM;
    const auto& ctx = to_engine(engine)->context();
    const int n = std::min(count, ctx.n_links());
    std::copy(ctx.links.stat_time_surcharged.begin(), ctx.links.stat_time_surcharged.begin() + n,
              buf);
    return SWMM_OK;
}

} /* extern "C" */

// ============================================================================
// Summary tables — the end-of-run report tables as data
// ============================================================================
//
// Each call builds its table with the report writer's own builder
// (output/ReportTables), so a value can never differ from the .rpt. Per-cell
// calls compute only the row they need but still scan every object; the
// *_read calls build the whole table once. Nothing is cached: the engine has no
// per-run slot to hold a cache safely.

namespace {

/** @brief Copy @p s into @p buf, NUL-terminated and truncated to @p len - 1 bytes. */
void copySummaryText(const std::string& s, char* buf, int len) {
    const auto n = std::min(s.size(), static_cast<std::size_t>(len - 1));
    std::memcpy(buf, s.data(), n);
    buf[n] = '\0';
}

/**
 * @brief Whether summary table @p table of @p engine can be read.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE, SWMM_ERR_LIFECYCLE outside ENDED, or
 *     SWMM_ERR_BADINDEX for an unknown table.
 */
int summaryReadable(SWMM_Engine engine, int table) {
    CHECK_HANDLE(engine);
    const auto state = to_engine(engine)->context().state;
    if (state != openswmm::EngineState::ENDED && state != openswmm::EngineState::REPORTED)
        return SWMM_ERR_LIFECYCLE;
    CHECK_INDEX(table >= 0 && table < SWMM_SUMMARY_TABLE_COUNT);
    return SWMM_OK;
}

/**
 * @brief Build summary table @p table of an ended run, computing only row @p row.
 *
 * SWMMEngine::report() copies the LID water balance from the solver groups into
 * ctx.lid_usage. Before it (internal ENDED, which the public API also calls ENDED)
 * the same copy is made here into a scratch store, so the table is the one the
 * report will print.
 *
 * @param[in] row Zero-based row, report_tables::kNoRows or report_tables::kAllRows.
 * @param[out] out The table; out.rows holds row @p row when it exists, or every row.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE, SWMM_ERR_LIFECYCLE outside ENDED,
 *     SWMM_ERR_BADINDEX for an unknown table, SWMM_ERR_NOMEM.
 */
int buildSummaryTable(SWMM_Engine engine, int table, int row, openswmm::ReportTable& out) {
    if (const int rc = summaryReadable(engine, table); rc != SWMM_OK)
        return rc;
    const openswmm::SWMMEngine& eng = *to_engine(engine);
    const auto& ctx = eng.context();
    try {
        if (table == SWMM_SUMMARY_LID_PERFORMANCE && ctx.state == openswmm::EngineState::ENDED) {
            // The copy SWMMEngine::report() makes (LID water balance → lid_usage).
            openswmm::LidUsageStore usage = ctx.lid_usage;
            const int n_usage = usage.count();
            for (int t = 0; t < eng.lid().numGroups(); ++t) {
                const auto& g = eng.lid().group(t);
                for (int s = 0; s < g.count; ++s) {
                    auto us = static_cast<std::size_t>(s);
                    int ctrl_idx = (us < g.control_idx.size()) ? g.control_idx[us] : -1;
                    int sc_idx = (us < g.subcatch_idx.size()) ? g.subcatch_idx[us] : -1;
                    if (ctrl_idx < 0 || sc_idx < 0)
                        continue;
                    for (int j = 0; j < n_usage; ++j) {
                        auto uj = static_cast<std::size_t>(j);
                        if (usage.lid_index[uj] == ctrl_idx && usage.subcatch_index[uj] == sc_idx) {
                            usage.wb_inflow[uj] = (us < g.wb_inflow.size()) ? g.wb_inflow[us] : 0.0;
                            usage.wb_evap[uj] = (us < g.wb_evap.size()) ? g.wb_evap[us] : 0.0;
                            usage.wb_infil[uj] = (us < g.wb_infil.size()) ? g.wb_infil[us] : 0.0;
                            usage.wb_surf_flow[uj] =
                                (us < g.wb_surf_flow.size()) ? g.wb_surf_flow[us] : 0.0;
                            usage.wb_drain_flow[uj] =
                                (us < g.wb_drain_flow.size()) ? g.wb_drain_flow[us] : 0.0;
                            break;
                        }
                    }
                }
            }
            out = openswmm::report_tables::lidPerformance(ctx, usage, row);
        } else {
            out = openswmm::report_tables::build(ctx, table, ctx.lid_usage, row);
        }
    } catch (const std::bad_alloc&) {
        return SWMM_ERR_NOMEM;
    }
    return SWMM_OK;
}

/** @return The table's row @p row, or nullptr when @p row is out of range. */
const openswmm::ReportRow* summaryRow(const openswmm::ReportTable& t, int row) {
    return (row >= 0 && row < t.row_count && !t.rows.empty()) ? &t.rows.front() : nullptr;
}

} // namespace

extern "C" {

SWMM_ENGINE_API int swmm_summary_table_title(SWMM_Engine engine, int table, char* buf,
                                             int buflen) {
    openswmm::ReportTable t;
    const int rc = buildSummaryTable(engine, table, openswmm::report_tables::kNoRows, t);
    if (rc != SWMM_OK)
        return rc;
    if (!buf || buflen <= 0)
        return SWMM_ERR_BADPARAM;
    copySummaryText(t.title, buf, buflen);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_summary_column_count(SWMM_Engine engine, int table, int* count) {
    openswmm::ReportTable t;
    const int rc = buildSummaryTable(engine, table, openswmm::report_tables::kNoRows, t);
    if (rc != SWMM_OK)
        return rc;
    if (!count)
        return SWMM_ERR_BADPARAM;
    *count = static_cast<int>(t.columns.size());
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_summary_column(SWMM_Engine engine, int table, int column, char* name,
                                        int name_len, char* units, int units_len) {
    openswmm::ReportTable t;
    const int rc = buildSummaryTable(engine, table, openswmm::report_tables::kNoRows, t);
    if (rc != SWMM_OK)
        return rc;
    CHECK_INDEX(column >= 0 && column < static_cast<int>(t.columns.size()));
    if ((name && name_len <= 0) || (units && units_len <= 0))
        return SWMM_ERR_BADPARAM;
    const auto& c = t.columns[static_cast<std::size_t>(column)];
    if (name)
        copySummaryText(c.heading, name, name_len);
    if (units)
        copySummaryText(c.units, units, units_len);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_summary_row_count(SWMM_Engine engine, int table, int* count) {
    openswmm::ReportTable t;
    const int rc = buildSummaryTable(engine, table, openswmm::report_tables::kNoRows, t);
    if (rc != SWMM_OK)
        return rc;
    if (!count)
        return SWMM_ERR_BADPARAM;
    *count = t.row_count;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_summary_row(SWMM_Engine engine, int table, int row, int* object_type,
                                     int* object_index, char* label, int label_len) {
    openswmm::ReportTable t;
    const int rc = buildSummaryTable(
        engine, table, row < 0 ? openswmm::report_tables::kNoRows : row, t);
    if (rc != SWMM_OK)
        return rc;
    const openswmm::ReportRow* r = summaryRow(t, row);
    CHECK_INDEX(r != nullptr);
    if (label && label_len <= 0)
        return SWMM_ERR_BADPARAM;
    if (object_type)
        *object_type = r->object_type;
    if (object_index)
        *object_index = r->object_index;
    if (label)
        copySummaryText(r->label, label, label_len);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_summary_value(SWMM_Engine engine, int table, int row, int column,
                                       double* value) {
    openswmm::ReportTable t;
    const int rc = buildSummaryTable(
        engine, table, row < 0 ? openswmm::report_tables::kNoRows : row, t);
    if (rc != SWMM_OK)
        return rc;
    const openswmm::ReportRow* r = summaryRow(t, row);
    CHECK_INDEX(r != nullptr);
    CHECK_INDEX(column >= 0 && column < static_cast<int>(t.columns.size()));
    if (!value)
        return SWMM_ERR_BADPARAM;
    *value = r->values[static_cast<std::size_t>(column)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_summary_rows_read(SWMM_Engine engine, int table, int* object_types,
                                           int* object_indices, int capacity) {
    if (const int rc = summaryReadable(engine, table); rc != SWMM_OK)
        return rc;
    if (!object_types || !object_indices || capacity < 0)
        return SWMM_ERR_BADPARAM;
    openswmm::ReportTable t;
    if (const int rc = buildSummaryTable(engine, table, openswmm::report_tables::kAllRows, t);
        rc != SWMM_OK)
        return rc;
    const std::size_t n = std::min(t.rows.size(), static_cast<std::size_t>(capacity));
    for (std::size_t r = 0; r < n; ++r) {
        object_types[r] = t.rows[r].object_type;
        object_indices[r] = t.rows[r].object_index;
    }
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_summary_values_read(SWMM_Engine engine, int table, double* values,
                                             int capacity) {
    if (const int rc = summaryReadable(engine, table); rc != SWMM_OK)
        return rc;
    if (!values || capacity < 0)
        return SWMM_ERR_BADPARAM;
    openswmm::ReportTable t;
    if (const int rc = buildSummaryTable(engine, table, openswmm::report_tables::kAllRows, t);
        rc != SWMM_OK)
        return rc;
    // Row-major: row * column_count + column.
    std::size_t k = 0;
    const auto cap = static_cast<std::size_t>(capacity);
    for (const auto& r : t.rows)
        for (std::size_t c = 0; c < t.columns.size() && k < cap; ++c)
            values[k++] = r.values[c];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_summary_labels_read(SWMM_Engine engine, int table, char* buf,
                                             int buflen, int* needed) {
    openswmm::ReportTable t;
    if (const int rc = buildSummaryTable(engine, table, openswmm::report_tables::kAllRows, t);
        rc != SWMM_OK)
        return rc;
    // Labels joined by '\n', no trailing newline, then the NUL.
    std::size_t total = 1;
    for (std::size_t r = 0; r < t.rows.size(); ++r)
        total += t.rows[r].label.size() + (r > 0 ? 1 : 0);
    if (needed)
        *needed = static_cast<int>(total);
    if (!buf || buflen < 0 || static_cast<std::size_t>(buflen) < total) {
        if (buf && buflen > 0)
            buf[0] = '\0';
        return SWMM_ERR_BADPARAM;
    }
    char* p = buf;
    for (std::size_t r = 0; r < t.rows.size(); ++r) {
        if (r > 0)
            *p++ = '\n';
        std::memcpy(p, t.rows[r].label.data(), t.rows[r].label.size());
        p += t.rows[r].label.size();
    }
    *p = '\0';
    return SWMM_OK;
}

} /* extern "C" */
