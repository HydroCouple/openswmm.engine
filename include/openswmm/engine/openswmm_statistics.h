/**
 * @file openswmm_statistics.h
 * @brief OpenSWMM Engine — Statistics Query C API.
 *
 * @details Provides functions for querying cumulative statistics for nodes,
 *          links, and subcatchments after (or during) simulation.
 *
 * @ingroup engine_api
 * @see openswmm_engine.h
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
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

/** @brief Maximum depth at a node (project length units). */
SWMM_ENGINE_API int swmm_stat_node_max_depth(SWMM_Engine engine, int idx, double* val);

/** @brief Maximum overflow rate at a node (project flow units). */
SWMM_ENGINE_API int swmm_stat_node_max_overflow(SWMM_Engine engine, int idx, double* val);

/** @brief Total volume flooded at a node (project volume units). */
SWMM_ENGINE_API int swmm_stat_node_vol_flooded(SWMM_Engine engine, int idx, double* val);

/** @brief Total time flooded at a node (hours). */
SWMM_ENGINE_API int swmm_stat_node_time_flooded(SWMM_Engine engine, int idx, double* val);

/* =========================================================================
 * Link Statistics
 * ========================================================================= */

/** @brief Maximum flow in a link (project flow units). */
SWMM_ENGINE_API int swmm_stat_link_max_flow(SWMM_Engine engine, int idx, double* val);

/** @brief Maximum velocity in a link (project length/time units). */
SWMM_ENGINE_API int swmm_stat_link_max_velocity(SWMM_Engine engine, int idx, double* val);

/** @brief Maximum depth/full-depth ratio in a link. */
SWMM_ENGINE_API int swmm_stat_link_max_filling(SWMM_Engine engine, int idx, double* val);

/** @brief Total volume conveyed through a link (project volume units). */
SWMM_ENGINE_API int swmm_stat_link_vol_flow(SWMM_Engine engine, int idx, double* val);

/** @brief Total surcharge time for a link (hours). */
SWMM_ENGINE_API int swmm_stat_link_surcharge_time(SWMM_Engine engine, int idx, double* val);

/* =========================================================================
 * Subcatchment Statistics
 * ========================================================================= */

/** @brief Total precipitation volume at a subcatchment (project volume units). */
SWMM_ENGINE_API int swmm_stat_subcatch_precip(SWMM_Engine engine, int idx, double* val);

/** @brief Total runoff volume from a subcatchment (project volume units). */
SWMM_ENGINE_API int swmm_stat_subcatch_runoff_vol(SWMM_Engine engine, int idx, double* val);

/** @brief Maximum runoff rate from a subcatchment (project flow units). */
SWMM_ENGINE_API int swmm_stat_subcatch_max_runoff(SWMM_Engine engine, int idx, double* val);

/* =========================================================================
 * Bulk Statistics
 * ========================================================================= */

/** @brief Get maximum depth for all nodes into a caller-supplied buffer. */
SWMM_ENGINE_API int swmm_stat_node_max_depth_bulk(SWMM_Engine engine, double* buf, int count);

/** @brief Get maximum flow for all links into a caller-supplied buffer. */
SWMM_ENGINE_API int swmm_stat_link_max_flow_bulk(SWMM_Engine engine, double* buf, int count);

/** @brief Get total runoff volume for all subcatchments into a caller-supplied buffer. */
SWMM_ENGINE_API int swmm_stat_subcatch_runoff_vol_bulk(SWMM_Engine engine, double* buf, int count);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* OPENSWMM_STATISTICS_H */
