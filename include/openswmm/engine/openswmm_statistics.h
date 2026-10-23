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

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* OPENSWMM_STATISTICS_H */
