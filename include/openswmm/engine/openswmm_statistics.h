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
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_STATISTICS_H
#define OPENSWMM_STATISTICS_H

#include "openswmm_engine.h"
#include <stdint.h>

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
 * Object Counts
 * ========================================================================= */

/** @brief Number of nodes in the network (valid after swmm_engine_initialize). */
SWMM_ENGINE_API int swmm_engine_node_count(SWMM_Engine engine, int* n);

/** @brief Number of links in the network (valid after swmm_engine_initialize). */
SWMM_ENGINE_API int swmm_engine_link_count(SWMM_Engine engine, int* n);

/* =========================================================================
 * Bulk Statistics
 * ========================================================================= */

/** @brief Get maximum depth for all nodes into a caller-supplied buffer. */
SWMM_ENGINE_API int swmm_stat_node_max_depth_bulk(SWMM_Engine engine, double* buf, int count);

/** @brief Get maximum flow for all links into a caller-supplied buffer. */
SWMM_ENGINE_API int swmm_stat_link_max_flow_bulk(SWMM_Engine engine, double* buf, int count);

/** @brief Get total runoff volume for all subcatchments into a caller-supplied buffer. */
SWMM_ENGINE_API int swmm_stat_subcatch_runoff_vol_bulk(SWMM_Engine engine, double* buf, int count);

/* =========================================================================
 * Routing Solver Statistics
 * ========================================================================= */

/**
 * @brief Get the cumulative number of Picard sweeps executed by the dynamic-wave
 *        solver across all routing timesteps since the last swmm_engine_start().
 *
 * @param engine        Engine handle.
 * @param[out] sweeps   Receives total Picard sweep count, or 0 if not DYNWAVE.
 * @return SWMM_OK on success.
 */
SWMM_ENGINE_API int swmm_stat_routing_picard_sweeps(SWMM_Engine engine, int64_t* sweeps);

/**
 * @brief Get spectral coarse-correction counters from the dynamic-wave solver.
 *
 * @param engine         Engine handle.
 * @param[out] attempted Corrections attempted (NULL safe).
 * @param[out] accepted  Corrections accepted   (NULL safe).
 * @param[out] rejected  Corrections rejected   (NULL safe).
 * @return SWMM_OK on success.
 */
SWMM_ENGINE_API int swmm_stat_spectral_corrections(SWMM_Engine engine,
                                                    int* attempted,
                                                    int* accepted,
                                                    int* rejected);

/* -------------------------------------------------------------------------
 * Phase 3 statistics bulk getters — added in OpenSWMM 6.0.0 to power the
 * MCP server's flooding / capacity summary tools without a per-node
 * Python loop. Each is a simple SoA memcpy from the corresponding
 * scalar accessor's column.
 * ------------------------------------------------------------------------- */

/**
 * @brief Get maximum overflow rate for all nodes into a caller-supplied buffer.
 * @details Bulk variant of @ref swmm_stat_node_max_overflow. SoA copy of
 *          @c ctx.nodes.stat_max_overflow.
 * @since 6.0.0
 */
SWMM_ENGINE_API int swmm_stat_node_max_overflow_bulk(SWMM_Engine engine, double* buf, int count);

/**
 * @brief Get total flooded volume for all nodes into a caller-supplied buffer.
 * @details Bulk variant of @ref swmm_stat_node_vol_flooded.
 * @since 6.0.0
 */
SWMM_ENGINE_API int swmm_stat_node_vol_flooded_bulk(SWMM_Engine engine, double* buf, int count);

/**
 * @brief Get cumulative time-flooded for all nodes into a caller-supplied buffer.
 * @details Bulk variant of @ref swmm_stat_node_time_flooded.
 * @since 6.0.0
 */
SWMM_ENGINE_API int swmm_stat_node_time_flooded_bulk(SWMM_Engine engine, double* buf, int count);

/**
 * @brief Get peak runoff rate for all subcatchments into a caller-supplied buffer.
 * @details Bulk variant of @ref swmm_stat_subcatch_max_runoff.
 * @since 6.0.0
 */
SWMM_ENGINE_API int swmm_stat_subcatch_max_runoff_bulk(SWMM_Engine engine, double* buf, int count);

/* -------------------------------------------------------------------------
 * Phase 4e link-stat bulks — complete the per-link statistics surface so
 * the MCP server's capacity_summary tool can collapse to a single-pass
 * shape (same as flooding_summary).  Each is a simple SoA memcpy from
 * the matching scalar accessor's column.
 * ------------------------------------------------------------------------- */

/**
 * @brief Get peak velocity for all links into a caller-supplied buffer.
 * @details Bulk variant of @ref swmm_stat_link_max_velocity. SoA copy of
 *          @c ctx.links.stat_max_veloc.
 * @since 6.0.0
 */
SWMM_ENGINE_API int swmm_stat_link_max_velocity_bulk(SWMM_Engine engine, double* buf, int count);

/**
 * @brief Get peak depth-to-full-depth ratio for all links into a buffer.
 * @details Bulk variant of @ref swmm_stat_link_max_filling. SoA copy of
 *          @c ctx.links.stat_max_filling.
 * @since 6.0.0
 */
SWMM_ENGINE_API int swmm_stat_link_max_filling_bulk(SWMM_Engine engine, double* buf, int count);

/**
 * @brief Get cumulative flow volume per link into a caller-supplied buffer.
 * @details Bulk variant of @ref swmm_stat_link_vol_flow. SoA copy of
 *          @c ctx.links.stat_vol_flow.
 * @since 6.0.0
 */
SWMM_ENGINE_API int swmm_stat_link_vol_flow_bulk(SWMM_Engine engine, double* buf, int count);

/**
 * @brief Get cumulative surcharge time per link into a caller-supplied buffer.
 * @details Bulk variant of @ref swmm_stat_link_surcharge_time. SoA copy of
 *          @c ctx.links.stat_time_surcharged.
 * @since 6.0.0
 */
SWMM_ENGINE_API int swmm_stat_link_surcharge_time_bulk(SWMM_Engine engine, double* buf, int count);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* OPENSWMM_STATISTICS_H */
