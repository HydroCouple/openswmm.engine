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
 * @file      openswmm_statistics.h
 * @brief     OpenSWMM Engine — Statistics Query C API.
 *
 * @details Provides functions for querying cumulative statistics for nodes,
 *          links, and subcatchments after (or during) simulation.
 *
 * @ingroup engine_api
 * @see openswmm_engine.h
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_STATISTICS_H
#define OPENSWMM_STATISTICS_H

#include "openswmm_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Node Statistics
 * ========================================================================= */

/**
 * @brief Read maximum recorded water depth for one node.
 *
 * Does not advance/finalize the simulation or recompute the statistic. Distinct node/link object
 * APIs may apply display-unit conversions; these swmm_stat_* accessors return the raw stored
 * values.
 *
 * @param[in] engine Non-null live engine with initialized statistics storage; exclusive access is
 *     required. No lifecycle guard is imposed, so a running read reflects current accumulation.
 * @param[in] idx Zero-based node index in the current model.
 * @param[out] val Optional destination in ft. Values are copied from internal statistics without
 *     project display-unit conversion.
 * @return SWMM_OK, including NULL val; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADINDEX for
 *     invalid idx.
 */
SWMM_ENGINE_API int swmm_stat_node_max_depth(SWMM_Engine engine, int idx, double* val);

/**
 * @brief Read maximum recorded overflow discharge for one node.
 *
 * Does not advance/finalize the simulation or recompute the statistic. Distinct node/link object
 * APIs may apply display-unit conversions; these swmm_stat_* accessors return the raw stored
 * values.
 *
 * @param[in] engine Non-null live engine with initialized statistics storage; exclusive access is
 *     required. No lifecycle guard is imposed, so a running read reflects current accumulation.
 * @param[in] idx Zero-based node index in the current model.
 * @param[out] val Optional destination in ft3/s. Values are copied from internal statistics without
 *     project display-unit conversion.
 * @return SWMM_OK, including NULL val; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADINDEX for
 *     invalid idx.
 */
SWMM_ENGINE_API int swmm_stat_node_max_overflow(SWMM_Engine engine, int idx, double* val);

/**
 * @brief Read cumulative flooded water volume for one node.
 *
 * Does not advance/finalize the simulation or recompute the statistic. Distinct node/link object
 * APIs may apply display-unit conversions; these swmm_stat_* accessors return the raw stored
 * values.
 *
 * @param[in] engine Non-null live engine with initialized statistics storage; exclusive access is
 *     required. No lifecycle guard is imposed, so a running read reflects current accumulation.
 * @param[in] idx Zero-based node index in the current model.
 * @param[out] val Optional destination in ft3. Values are copied from internal statistics without
 *     project display-unit conversion.
 * @return SWMM_OK, including NULL val; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADINDEX for
 *     invalid idx.
 */
SWMM_ENGINE_API int swmm_stat_node_vol_flooded(SWMM_Engine engine, int idx, double* val);

/**
 * @brief Read cumulative flooding duration for one node.
 *
 * Does not advance/finalize the simulation or recompute the statistic. Distinct node/link object
 * APIs may apply display-unit conversions; these swmm_stat_* accessors return the raw stored
 * values.
 *
 * @param[in] engine Non-null live engine with initialized statistics storage; exclusive access is
 *     required. No lifecycle guard is imposed, so a running read reflects current accumulation.
 * @param[in] idx Zero-based node index in the current model.
 * @param[out] val Optional destination in s. Values are copied from internal statistics without
 *     project display-unit conversion.
 * @return SWMM_OK, including NULL val; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADINDEX for
 *     invalid idx.
 */
SWMM_ENGINE_API int swmm_stat_node_time_flooded(SWMM_Engine engine, int idx, double* val);

/* =========================================================================
 * Link Statistics
 * ========================================================================= */

/**
 * @brief Read maximum recorded absolute flow for one link.
 *
 * Does not advance/finalize the simulation or recompute the statistic. Distinct node/link object
 * APIs may apply display-unit conversions; these swmm_stat_* accessors return the raw stored
 * values.
 *
 * @param[in] engine Non-null live engine with initialized statistics storage; exclusive access is
 *     required. No lifecycle guard is imposed, so a running read reflects current accumulation.
 * @param[in] idx Zero-based link index in the current model.
 * @param[out] val Optional destination in ft3/s. Values are copied from internal statistics without
 *     project display-unit conversion.
 * @return SWMM_OK, including NULL val; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADINDEX for
 *     invalid idx.
 */
SWMM_ENGINE_API int swmm_stat_link_max_flow(SWMM_Engine engine, int idx, double* val);

/**
 * @brief Read maximum recorded absolute flow velocity for one link.
 *
 * Does not advance/finalize the simulation or recompute the statistic. Distinct node/link object
 * APIs may apply display-unit conversions; these swmm_stat_* accessors return the raw stored
 * values.
 *
 * @param[in] engine Non-null live engine with initialized statistics storage; exclusive access is
 *     required. No lifecycle guard is imposed, so a running read reflects current accumulation.
 * @param[in] idx Zero-based link index in the current model.
 * @param[out] val Optional destination in ft/s. Values are copied from internal statistics without
 *     project display-unit conversion.
 * @return SWMM_OK, including NULL val; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADINDEX for
 *     invalid idx.
 */
SWMM_ENGINE_API int swmm_stat_link_max_velocity(SWMM_Engine engine, int idx, double* val);

/**
 * @brief Read maximum recorded depth/full-depth ratio for one link.
 *
 * Does not advance/finalize the simulation or recompute the statistic. Distinct node/link object
 * APIs may apply display-unit conversions; these swmm_stat_* accessors return the raw stored
 * values.
 *
 * @param[in] engine Non-null live engine with initialized statistics storage; exclusive access is
 *     required. No lifecycle guard is imposed, so a running read reflects current accumulation.
 * @param[in] idx Zero-based link index in the current model.
 * @param[out] val Optional destination in dimensionless. Values are copied from internal statistics
 *     without project display-unit conversion.
 * @return SWMM_OK, including NULL val; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADINDEX for
 *     invalid idx.
 */
SWMM_ENGINE_API int swmm_stat_link_max_filling(SWMM_Engine engine, int idx, double* val);

/**
 * @brief Read cumulative conveyed water volume for one link.
 *
 * Does not advance/finalize the simulation or recompute the statistic. Distinct node/link object
 * APIs may apply display-unit conversions; these swmm_stat_* accessors return the raw stored
 * values.
 *
 * @param[in] engine Non-null live engine with initialized statistics storage; exclusive access is
 *     required. No lifecycle guard is imposed, so a running read reflects current accumulation.
 * @param[in] idx Zero-based link index in the current model.
 * @param[out] val Optional destination in ft3. Values are copied from internal statistics without
 *     project display-unit conversion.
 * @return SWMM_OK, including NULL val; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADINDEX for
 *     invalid idx.
 */
SWMM_ENGINE_API int swmm_stat_link_vol_flow(SWMM_Engine engine, int idx, double* val);

/**
 * @brief Read cumulative surcharged duration for one link.
 *
 * Does not advance/finalize the simulation or recompute the statistic. Distinct node/link object
 * APIs may apply display-unit conversions; these swmm_stat_* accessors return the raw stored
 * values.
 *
 * @param[in] engine Non-null live engine with initialized statistics storage; exclusive access is
 *     required. No lifecycle guard is imposed, so a running read reflects current accumulation.
 * @param[in] idx Zero-based link index in the current model.
 * @param[out] val Optional destination in s. Values are copied from internal statistics without
 *     project display-unit conversion.
 * @return SWMM_OK, including NULL val; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADINDEX for
 *     invalid idx.
 */
SWMM_ENGINE_API int swmm_stat_link_surcharge_time(SWMM_Engine engine, int idx, double* val);

/* =========================================================================
 * Subcatchment Statistics
 * ========================================================================= */

/**
 * @brief Read cumulative precipitation volume for one subcatchment.
 *
 * Does not advance/finalize the simulation or recompute the statistic. Distinct node/link object
 * APIs may apply display-unit conversions; these swmm_stat_* accessors return the raw stored
 * values.
 *
 * @param[in] engine Non-null live engine with initialized statistics storage; exclusive access is
 *     required. No lifecycle guard is imposed, so a running read reflects current accumulation.
 * @param[in] idx Zero-based subcatchment index in the current model.
 * @param[out] val Optional destination in ft3. Values are copied from internal statistics without
 *     project display-unit conversion.
 * @return SWMM_OK, including NULL val; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADINDEX for
 *     invalid idx.
 */
SWMM_ENGINE_API int swmm_stat_subcatch_precip(SWMM_Engine engine, int idx, double* val);

/**
 * @brief Read cumulative runoff volume for one subcatchment.
 *
 * Does not advance/finalize the simulation or recompute the statistic. Distinct node/link object
 * APIs may apply display-unit conversions; these swmm_stat_* accessors return the raw stored
 * values.
 *
 * @param[in] engine Non-null live engine with initialized statistics storage; exclusive access is
 *     required. No lifecycle guard is imposed, so a running read reflects current accumulation.
 * @param[in] idx Zero-based subcatchment index in the current model.
 * @param[out] val Optional destination in ft3. Values are copied from internal statistics without
 *     project display-unit conversion.
 * @return SWMM_OK, including NULL val; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADINDEX for
 *     invalid idx.
 */
SWMM_ENGINE_API int swmm_stat_subcatch_runoff_vol(SWMM_Engine engine, int idx, double* val);

/**
 * @brief Read maximum recorded runoff discharge for one subcatchment.
 *
 * Does not advance/finalize the simulation or recompute the statistic. Distinct node/link object
 * APIs may apply display-unit conversions; these swmm_stat_* accessors return the raw stored
 * values.
 *
 * @param[in] engine Non-null live engine with initialized statistics storage; exclusive access is
 *     required. No lifecycle guard is imposed, so a running read reflects current accumulation.
 * @param[in] idx Zero-based subcatchment index in the current model.
 * @param[out] val Optional destination in ft3/s. Values are copied from internal statistics without
 *     project display-unit conversion.
 * @return SWMM_OK, including NULL val; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADINDEX for
 *     invalid idx.
 */
SWMM_ENGINE_API int swmm_stat_subcatch_max_runoff(SWMM_Engine engine, int idx, double* val);

/* =========================================================================
 * Bulk Statistics
 * ========================================================================= */

/**
 * @brief Copy maximum recorded water depth for a prefix of the node table.
 *
 * Uses the same raw internal units and object order as swmm_stat_node_max_depth(). No required-size
 * output or short-buffer error is provided; query the object count separately when all rows are
 * needed.
 *
 * @param[in] engine Non-null live engine with initialized statistics storage; exclusive access is
 *     required. No lifecycle guard is imposed, so a running read reflects current accumulation.
 * @param[out] buf Non-null caller-owned array of at least count doubles. Copies min(count,object
 *     count) values in ft; entries beyond that prefix are unchanged.
 * @param[in] count Positive buffer capacity/requested prefix length in elements, not bytes. May be
 *     smaller than the full object count.
 * @return SWMM_OK even for a partial prefix; SWMM_ERR_BADHANDLE for null engine or
 *     SWMM_ERR_BADPARAM for null buf/count<=0.
 */
SWMM_ENGINE_API int swmm_stat_node_max_depth_bulk(SWMM_Engine engine, double* buf, int count);

/**
 * @brief Copy maximum recorded absolute flow for a prefix of the link table.
 *
 * Uses the same raw internal units and object order as swmm_stat_link_max_flow(). No required-size
 * output or short-buffer error is provided; query the object count separately when all rows are
 * needed.
 *
 * @param[in] engine Non-null live engine with initialized statistics storage; exclusive access is
 *     required. No lifecycle guard is imposed, so a running read reflects current accumulation.
 * @param[out] buf Non-null caller-owned array of at least count doubles. Copies min(count,object
 *     count) values in ft3/s; entries beyond that prefix are unchanged.
 * @param[in] count Positive buffer capacity/requested prefix length in elements, not bytes. May be
 *     smaller than the full object count.
 * @return SWMM_OK even for a partial prefix; SWMM_ERR_BADHANDLE for null engine or
 *     SWMM_ERR_BADPARAM for null buf/count<=0.
 */
SWMM_ENGINE_API int swmm_stat_link_max_flow_bulk(SWMM_Engine engine, double* buf, int count);

/**
 * @brief Copy cumulative runoff volume for a prefix of the subcatchment table.
 *
 * Uses the same raw internal units and object order as swmm_stat_subcatch_runoff_vol(). No
 * required-size output or short-buffer error is provided; query the object count separately when
 * all rows are needed.
 *
 * @param[in] engine Non-null live engine with initialized statistics storage; exclusive access is
 *     required. No lifecycle guard is imposed, so a running read reflects current accumulation.
 * @param[out] buf Non-null caller-owned array of at least count doubles. Copies min(count,object
 *     count) values in ft3; entries beyond that prefix are unchanged.
 * @param[in] count Positive buffer capacity/requested prefix length in elements, not bytes. May be
 *     smaller than the full object count.
 * @return SWMM_OK even for a partial prefix; SWMM_ERR_BADHANDLE for null engine or
 *     SWMM_ERR_BADPARAM for null buf/count<=0.
 */
SWMM_ENGINE_API int swmm_stat_subcatch_runoff_vol_bulk(SWMM_Engine engine, double* buf, int count);

/* -------------------------------------------------------------------------
 * Phase 3 statistics bulk getters — added in OpenSWMM 6.0.0 to power the
 * MCP server's flooding / capacity summary tools without a per-node
 * Python loop. Each is a simple SoA memcpy from the corresponding
 * scalar accessor's column.
 * ------------------------------------------------------------------------- */

/**
 * @brief Copy maximum recorded overflow discharge for a prefix of the node table.
 *
 * Uses the same raw internal units and object order as swmm_stat_node_max_overflow(). No
 * required-size output or short-buffer error is provided; query the object count separately when
 * all rows are needed.
 *
 * @param[in] engine Non-null live engine with initialized statistics storage; exclusive access is
 *     required. No lifecycle guard is imposed, so a running read reflects current accumulation.
 * @param[out] buf Non-null caller-owned array of at least count doubles. Copies min(count,object
 *     count) values in ft3/s; entries beyond that prefix are unchanged.
 * @param[in] count Positive buffer capacity/requested prefix length in elements, not bytes. May be
 *     smaller than the full object count.
 * @return SWMM_OK even for a partial prefix; SWMM_ERR_BADHANDLE for null engine or
 *     SWMM_ERR_BADPARAM for null buf/count<=0.
 */
SWMM_ENGINE_API int swmm_stat_node_max_overflow_bulk(SWMM_Engine engine, double* buf, int count);

/**
 * @brief Copy cumulative flooded water volume for a prefix of the node table.
 *
 * Uses the same raw internal units and object order as swmm_stat_node_vol_flooded(). No
 * required-size output or short-buffer error is provided; query the object count separately when
 * all rows are needed.
 *
 * @param[in] engine Non-null live engine with initialized statistics storage; exclusive access is
 *     required. No lifecycle guard is imposed, so a running read reflects current accumulation.
 * @param[out] buf Non-null caller-owned array of at least count doubles. Copies min(count,object
 *     count) values in ft3; entries beyond that prefix are unchanged.
 * @param[in] count Positive buffer capacity/requested prefix length in elements, not bytes. May be
 *     smaller than the full object count.
 * @return SWMM_OK even for a partial prefix; SWMM_ERR_BADHANDLE for null engine or
 *     SWMM_ERR_BADPARAM for null buf/count<=0.
 */
SWMM_ENGINE_API int swmm_stat_node_vol_flooded_bulk(SWMM_Engine engine, double* buf, int count);

/**
 * @brief Copy cumulative flooding duration for a prefix of the node table.
 *
 * Uses the same raw internal units and object order as swmm_stat_node_time_flooded(). No
 * required-size output or short-buffer error is provided; query the object count separately when
 * all rows are needed.
 *
 * @param[in] engine Non-null live engine with initialized statistics storage; exclusive access is
 *     required. No lifecycle guard is imposed, so a running read reflects current accumulation.
 * @param[out] buf Non-null caller-owned array of at least count doubles. Copies min(count,object
 *     count) values in s; entries beyond that prefix are unchanged.
 * @param[in] count Positive buffer capacity/requested prefix length in elements, not bytes. May be
 *     smaller than the full object count.
 * @return SWMM_OK even for a partial prefix; SWMM_ERR_BADHANDLE for null engine or
 *     SWMM_ERR_BADPARAM for null buf/count<=0.
 */
SWMM_ENGINE_API int swmm_stat_node_time_flooded_bulk(SWMM_Engine engine, double* buf, int count);

/**
 * @brief Copy maximum recorded runoff discharge for a prefix of the subcatchment table.
 *
 * Uses the same raw internal units and object order as swmm_stat_subcatch_max_runoff(). No
 * required-size output or short-buffer error is provided; query the object count separately when
 * all rows are needed.
 *
 * @param[in] engine Non-null live engine with initialized statistics storage; exclusive access is
 *     required. No lifecycle guard is imposed, so a running read reflects current accumulation.
 * @param[out] buf Non-null caller-owned array of at least count doubles. Copies min(count,object
 *     count) values in ft3/s; entries beyond that prefix are unchanged.
 * @param[in] count Positive buffer capacity/requested prefix length in elements, not bytes. May be
 *     smaller than the full object count.
 * @return SWMM_OK even for a partial prefix; SWMM_ERR_BADHANDLE for null engine or
 *     SWMM_ERR_BADPARAM for null buf/count<=0.
 */
SWMM_ENGINE_API int swmm_stat_subcatch_max_runoff_bulk(SWMM_Engine engine, double* buf, int count);

/* -------------------------------------------------------------------------
 * Phase 4e link-stat bulks — complete the per-link statistics surface so
 * the MCP server's capacity_summary tool can collapse to a single-pass
 * shape (same as flooding_summary).  Each is a simple SoA memcpy from
 * the matching scalar accessor's column.
 * ------------------------------------------------------------------------- */

/**
 * @brief Copy maximum recorded absolute flow velocity for a prefix of the link table.
 *
 * Uses the same raw internal units and object order as swmm_stat_link_max_velocity(). No
 * required-size output or short-buffer error is provided; query the object count separately when
 * all rows are needed.
 *
 * @param[in] engine Non-null live engine with initialized statistics storage; exclusive access is
 *     required. No lifecycle guard is imposed, so a running read reflects current accumulation.
 * @param[out] buf Non-null caller-owned array of at least count doubles. Copies min(count,object
 *     count) values in ft/s; entries beyond that prefix are unchanged.
 * @param[in] count Positive buffer capacity/requested prefix length in elements, not bytes. May be
 *     smaller than the full object count.
 * @return SWMM_OK even for a partial prefix; SWMM_ERR_BADHANDLE for null engine or
 *     SWMM_ERR_BADPARAM for null buf/count<=0.
 */
SWMM_ENGINE_API int swmm_stat_link_max_velocity_bulk(SWMM_Engine engine, double* buf, int count);

/**
 * @brief Copy maximum recorded depth/full-depth ratio for a prefix of the link table.
 *
 * Uses the same raw internal units and object order as swmm_stat_link_max_filling(). No
 * required-size output or short-buffer error is provided; query the object count separately when
 * all rows are needed.
 *
 * @param[in] engine Non-null live engine with initialized statistics storage; exclusive access is
 *     required. No lifecycle guard is imposed, so a running read reflects current accumulation.
 * @param[out] buf Non-null caller-owned array of at least count doubles. Copies min(count,object
 *     count) values in dimensionless; entries beyond that prefix are unchanged.
 * @param[in] count Positive buffer capacity/requested prefix length in elements, not bytes. May be
 *     smaller than the full object count.
 * @return SWMM_OK even for a partial prefix; SWMM_ERR_BADHANDLE for null engine or
 *     SWMM_ERR_BADPARAM for null buf/count<=0.
 */
SWMM_ENGINE_API int swmm_stat_link_max_filling_bulk(SWMM_Engine engine, double* buf, int count);

/**
 * @brief Copy cumulative conveyed water volume for a prefix of the link table.
 *
 * Uses the same raw internal units and object order as swmm_stat_link_vol_flow(). No required-size
 * output or short-buffer error is provided; query the object count separately when all rows are
 * needed.
 *
 * @param[in] engine Non-null live engine with initialized statistics storage; exclusive access is
 *     required. No lifecycle guard is imposed, so a running read reflects current accumulation.
 * @param[out] buf Non-null caller-owned array of at least count doubles. Copies min(count,object
 *     count) values in ft3; entries beyond that prefix are unchanged.
 * @param[in] count Positive buffer capacity/requested prefix length in elements, not bytes. May be
 *     smaller than the full object count.
 * @return SWMM_OK even for a partial prefix; SWMM_ERR_BADHANDLE for null engine or
 *     SWMM_ERR_BADPARAM for null buf/count<=0.
 */
SWMM_ENGINE_API int swmm_stat_link_vol_flow_bulk(SWMM_Engine engine, double* buf, int count);

/**
 * @brief Copy cumulative surcharged duration for a prefix of the link table.
 *
 * Uses the same raw internal units and object order as swmm_stat_link_surcharge_time(). No
 * required-size output or short-buffer error is provided; query the object count separately when
 * all rows are needed.
 *
 * @param[in] engine Non-null live engine with initialized statistics storage; exclusive access is
 *     required. No lifecycle guard is imposed, so a running read reflects current accumulation.
 * @param[out] buf Non-null caller-owned array of at least count doubles. Copies min(count,object
 *     count) values in s; entries beyond that prefix are unchanged.
 * @param[in] count Positive buffer capacity/requested prefix length in elements, not bytes. May be
 *     smaller than the full object count.
 * @return SWMM_OK even for a partial prefix; SWMM_ERR_BADHANDLE for null engine or
 *     SWMM_ERR_BADPARAM for null buf/count<=0.
 */
SWMM_ENGINE_API int swmm_stat_link_surcharge_time_bulk(SWMM_Engine engine, double* buf, int count);

/* =========================================================================
 * Summary tables — the end-of-run report tables as data
 * ========================================================================= */

/**
 * @brief The report summary tables available as data, in report order.
 *
 * Each table holds the rows and columns the `.rpt` prints for it, with the same values: they
 * are computed by the code that writes the report, in the model's display units (the units
 * the report shows). A table the report would not print for this run (for example Node Depth
 * when routing is ignored, Pumping without pumps) has no rows.
 *
 * The whole-table reads (swmm_summary_rows_read(), swmm_summary_values_read(),
 * swmm_summary_labels_read()) are the fast path: each builds its table once, O(rows x
 * columns). The per-cell calls (swmm_summary_row(), swmm_summary_value()) rebuild the table's
 * rows on every call, so reading a whole table cell by cell costs O(rows^2 x columns).
 */
/** Defined when this header declares the summary-table API below. */
#define OPENSWMM_HAS_SUMMARY_TABLES 1

typedef enum SWMM_SummaryTable {
    SWMM_SUMMARY_NODE_DEPTH = 0,          /**< Node Depth Summary */
    SWMM_SUMMARY_NODE_INFLOW = 1,         /**< Node Inflow Summary */
    SWMM_SUMMARY_NODE_SURCHARGE = 2,      /**< Node Surcharge Summary (surcharged nodes only) */
    SWMM_SUMMARY_NODE_FLOODING = 3,       /**< Node Flooding Summary (flooded nodes only) */
    SWMM_SUMMARY_STORAGE_VOLUME = 4,      /**< Storage Volume Summary */
    SWMM_SUMMARY_OUTFALL_LOADING = 5,     /**< Outfall Loading Summary, then a System row */
    SWMM_SUMMARY_LINK_FLOW = 6,           /**< Link Flow Summary */
    SWMM_SUMMARY_FLOW_CLASSIFICATION = 7, /**< Flow Classification Summary (conduits) */
    SWMM_SUMMARY_CONDUIT_SURCHARGE = 8,   /**< Conduit Surcharge Summary (surcharged conduits) */
    SWMM_SUMMARY_PUMPING = 9,             /**< Pumping Summary */
    SWMM_SUMMARY_LID_PERFORMANCE = 10,    /**< LID Performance Summary (one row per LID unit) */
    SWMM_SUMMARY_TABLE_COUNT = 11
} SWMM_SummaryTable;

/** Object kind of a summary row (swmm_summary_row()). */
#define SWMM_SUMMARY_OBJECT_SYSTEM (-1) /**< a total row, e.g. the Outfall Loading "System" row */
#define SWMM_SUMMARY_OBJECT_NODE 0
#define SWMM_SUMMARY_OBJECT_LINK 1
#define SWMM_SUMMARY_OBJECT_SUBCATCH 2 /**< LID Performance rows: the subcatchment of the unit */

/**
 * @brief Copy a summary table's report title, e.g. "Outfall Loading Summary".
 *
 * Tables are available once the run has ended (ENDED) and until the engine is closed.
 *
 * @param[in] engine Non-null engine in ENDED.
 * @param[in] table SWMM_SummaryTable value.
 * @param[out] buf Non-null buffer; NUL-terminated, truncated to buflen-1 bytes.
 * @param[in] buflen Positive capacity including NUL.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine; SWMM_ERR_LIFECYCLE outside ENDED;
 *     SWMM_ERR_BADINDEX for an unknown table; SWMM_ERR_BADPARAM for a null buffer or
 *     nonpositive length.
 */
SWMM_ENGINE_API int swmm_summary_table_title(SWMM_Engine engine, int table, char* buf,
                                             int buflen);

/**
 * @brief Count a summary table's value columns (the object name and label are not columns).
 * @param[in] engine Non-null engine in ENDED.
 * @param[in] table SWMM_SummaryTable value.
 * @param[out] count Non-null destination.
 * @return As swmm_summary_table_title(); SWMM_ERR_BADPARAM for null count.
 */
SWMM_ENGINE_API int swmm_summary_column_count(SWMM_Engine engine, int table, int* count);

/**
 * @brief Copy one value column's heading and units as the report prints them.
 *
 * The heading joins the report's stacked header words, e.g. "Maximum Total Inflow"; units are
 * the report's unit word for this run, e.g. "CMS", "10^6 ltr", "%", "hours", "days" (times of
 * maxima, elapsed from the report start) or "" for a dimensionless value. Pollutant columns of
 * Outfall Loading are headed by the pollutant name with its load unit ("kg", "lbs").
 *
 * @param[in] engine Non-null engine in ENDED.
 * @param[in] table SWMM_SummaryTable value.
 * @param[in] column Zero-based column in [0, swmm_summary_column_count()).
 * @param[out] name Optional heading buffer (NULL skips), NUL-terminated and truncated.
 * @param[in] name_len Capacity of @p name; positive when @p name is non-null.
 * @param[out] units Optional units buffer (NULL skips), NUL-terminated and truncated.
 * @param[in] units_len Capacity of @p units; positive when @p units is non-null.
 * @return As swmm_summary_table_title(); SWMM_ERR_BADINDEX for a column out of range.
 */
SWMM_ENGINE_API int swmm_summary_column(SWMM_Engine engine, int table, int column, char* name,
                                        int name_len, char* units, int units_len);

/**
 * @brief Count a summary table's rows, in report order (System rows included).
 * @param[in] engine Non-null engine in ENDED.
 * @param[in] table SWMM_SummaryTable value.
 * @param[out] count Non-null destination.
 * @return As swmm_summary_table_title(); SWMM_ERR_BADPARAM for null count.
 */
SWMM_ENGINE_API int swmm_summary_row_count(SWMM_Engine engine, int table, int* count);

/**
 * @brief Identify one row: its object and the report's text column for it.
 *
 * @param[in] engine Non-null engine in ENDED.
 * @param[in] table SWMM_SummaryTable value.
 * @param[in] row Zero-based row in [0, swmm_summary_row_count()).
 * @param[out] object_type Optional SWMM_SUMMARY_OBJECT_* destination.
 * @param[out] object_index Optional destination: the node, link or subcatchment index, -1 for a
 *     System row.
 * @param[out] label Optional buffer for the row's text column (node or link type such as
 *     "JUNCTION" or "CONDUIT", the LID control name for LID Performance, empty otherwise),
 *     NUL-terminated and truncated.
 * @param[in] label_len Capacity of @p label; positive when @p label is non-null.
 * @return As swmm_summary_table_title(); SWMM_ERR_BADINDEX for a row out of range.
 */
SWMM_ENGINE_API int swmm_summary_row(SWMM_Engine engine, int table, int row, int* object_type,
                                     int* object_index, char* label, int label_len);

/**
 * @brief Read one value of a summary table.
 *
 * @param[in] engine Non-null engine in ENDED.
 * @param[in] table SWMM_SummaryTable value.
 * @param[in] row Zero-based row.
 * @param[in] column Zero-based value column.
 * @param[out] value Non-null destination: the value the report prints, unrounded, in the
 *     column's units; NaN where the report prints no value for this row.
 * Each call rebuilds the table's rows to find this one; read whole tables with
 * swmm_summary_values_read().
 *
 * @return As swmm_summary_table_title(); SWMM_ERR_BADINDEX for a row or column out of range;
 *     SWMM_ERR_BADPARAM for null value.
 */
SWMM_ENGINE_API int swmm_summary_value(SWMM_Engine engine, int table, int row, int column,
                                       double* value);

/**
 * @brief Read every row's object at once, in row order (the fast path for swmm_summary_row()).
 *
 * Builds the table once. Row r's object is what swmm_summary_row() returns for row r.
 *
 * @param[in] engine Non-null engine in ENDED.
 * @param[in] table SWMM_SummaryTable value.
 * @param[out] object_types Non-null array of @p capacity ints: SWMM_SUMMARY_OBJECT_* per row.
 * @param[out] object_indices Non-null array of @p capacity ints: node, link or subcatchment
 *     index per row, -1 for a System row.
 * @param[in] capacity Elements in each array; min(capacity, row count) rows are written and
 *     the rest are unchanged. Zero writes nothing.
 * @return As swmm_summary_table_title(); SWMM_ERR_BADPARAM for a null array or capacity < 0.
 */
SWMM_ENGINE_API int swmm_summary_rows_read(SWMM_Engine engine, int table, int* object_types,
                                           int* object_indices, int capacity);

/**
 * @brief Read every value at once, row-major (the fast path for swmm_summary_value()).
 *
 * Builds the table once. Element row * column_count + column is what swmm_summary_value()
 * returns for that cell: the unrounded value in the column's units, NaN where the report
 * prints nothing.
 *
 * @param[in] engine Non-null engine in ENDED.
 * @param[in] table SWMM_SummaryTable value.
 * @param[out] values Non-null array of @p capacity doubles; min(capacity, rows x columns) are
 *     written and the rest are unchanged.
 * @param[in] capacity Elements in @p values. Zero writes nothing.
 * @return As swmm_summary_table_title(); SWMM_ERR_BADPARAM for null values or capacity < 0.
 */
SWMM_ENGINE_API int swmm_summary_values_read(SWMM_Engine engine, int table, double* values,
                                             int capacity);

/**
 * @brief Copy every row's label at once (the fast path for swmm_summary_row() labels).
 *
 * Builds the table once. The labels are joined by '\n' in row order, with no trailing
 * newline, and NUL-terminated: a table with no rows gives "", and a row with an empty label
 * leaves an empty line.
 *
 * To size the buffer, call it with buf NULL and buflen 0: it sets *needed and returns
 * SWMM_ERR_BADPARAM, writing nothing else. Then call again with a buffer of *needed bytes.
 *
 * @param[in] engine Non-null engine in ENDED.
 * @param[in] table SWMM_SummaryTable value.
 * @param[out] buf Buffer for the joined labels; NULL only with buflen 0 (a size query).
 * @param[in] buflen Capacity of @p buf in bytes, including the NUL.
 * @param[out] needed Optional (NULL skips): the full length including the NUL. Set whenever
 *     the table can be read, including when @p buf is NULL or too small.
 * @return As swmm_summary_table_title(); SWMM_ERR_BADPARAM for a NULL @p buf, a negative
 *     @p buflen, or @p buflen below the needed length. A buffer that is too small gets only a
 *     NUL at buf[0] (when buflen > 0); a NULL buffer gets nothing.
 */
SWMM_ENGINE_API int swmm_summary_labels_read(SWMM_Engine engine, int table, char* buf,
                                             int buflen, int* needed);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* OPENSWMM_STATISTICS_H */
