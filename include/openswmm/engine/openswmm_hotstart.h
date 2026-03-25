/**
 * @file openswmm_hotstart.h
 * @brief Hot start file management — transparent C API.
 *
 * @details Hot start files capture the hydraulic and water quality state of a
 *          SWMM simulation so it can be resumed later without running a warm-up
 *          period from dry-weather initial conditions.
 *
 *          The new hot start format (OPENSWMM_HS_V1) extends the legacy SWMM
 *          binary hot start format with:
 *          - An object UUID map for robust missing-object detection
 *          - CRS string for spatial consistency checking
 *          - User flag values
 *          - A CRC32 integrity checksum
 *
 * @section hotstart_api_workflow Workflow
 *
 * @code{.c}
 * // ---- Save a hot start ----
 * SWMM_Engine e = swmm_engine_create();
 * swmm_engine_open(e, "warmup.inp", "warmup.rpt", "warmup.out");
 * swmm_engine_initialize(e);
 * swmm_engine_start(e, 0);
 * double t = 0.0;
 * while (swmm_engine_step(e, &t) == SWMM_OK && t > 0.0) {}
 * swmm_hotstart_save(e, "warmup.hs");
 * swmm_engine_end(e);
 * swmm_engine_close(e);
 * swmm_engine_destroy(e);
 *
 * // ---- Apply a hot start ----
 * SWMM_HotStart hs = NULL;
 * swmm_hotstart_open("warmup.hs", &hs);
 *
 * SWMM_Engine e2 = swmm_engine_create();
 * swmm_engine_open(e2, "storm.inp", "storm.rpt", "storm.out");
 * swmm_engine_initialize(e2);
 * swmm_hotstart_apply(e2, hs);   // warnings issued for missing objects
 *
 * int nwarn = swmm_hotstart_warning_count(hs);
 * for (int i = 0; i < nwarn; i++) {
 *     printf("Warning: %s\n", swmm_hotstart_warning(hs, i));
 * }
 *
 * swmm_hotstart_close(hs);
 * @endcode
 *
 * @defgroup engine_hotstart Hot Start File API
 * @ingroup  new_engine
 *
 * @see Legacy reference: src/solver/hotstart.c — swmm_saveHotstart(), swmm_useHotstart()
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_ENGINE_HOTSTART_H
#define OPENSWMM_ENGINE_HOTSTART_H

#include "openswmm_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Opaque handle to an open hot start file. */
typedef void* SWMM_HotStart;

/** @brief Magic string written at the start of every OPENSWMM_HS_V1 file. */
#define OPENSWMM_HOTSTART_MAGIC    "OPENSWMM_HS_V1\0"

/** @brief Current hot start format version number. */
#define OPENSWMM_HOTSTART_VERSION  1

/* =========================================================================
 * Create / save
 * ========================================================================= */

/**
 * @brief Save the current engine state to a new hot start file.
 *
 * @details Captures the full hydraulic state (node depths, heads, volumes;
 *          link flows, depths; subcatchment runoff and groundwater depths;
 *          user flag values) and writes it to the OPENSWMM_HS_V1 binary format.
 *
 * @param engine  Engine handle (must be in SWMM_STATE_STARTED or SWMM_STATE_RUNNING).
 * @param path    File path for the new hot start file.
 * @returns SWMM_OK on success; SWMM_ERR_HOTSTART or SWMM_ERR_IO on failure.
 *
 * @note The saved file can be applied to a *different* model as long as the
 *       object IDs overlap. Missing objects are handled gracefully.
 */
SWMM_ENGINE_API int swmm_hotstart_save(SWMM_Engine engine, const char* path);

/* =========================================================================
 * Open / read
 * ========================================================================= */

/**
 * @brief Open an existing hot start file for reading.
 *
 * @details Validates the magic number, version, and CRC32 checksum.
 *          Loads the full object UUID map into memory.
 *
 * @param path  Path to an existing OPENSWMM_HS_V1 hot start file.
 * @param hs    [out] Handle to the opened hot start.
 * @returns SWMM_OK on success; SWMM_ERR_HOTSTART if file is invalid.
 */
SWMM_ENGINE_API int swmm_hotstart_open(const char* path, SWMM_HotStart* hs);

/* =========================================================================
 * Apply
 * ========================================================================= */

/**
 * @brief Apply hot start state to an engine.
 *
 * @details For each object in the hot start file, the engine's corresponding
 *          object (matched by string ID) is updated. Objects in the hot start
 *          that do not exist in the target engine generate warnings (not errors).
 *          Objects in the target engine that are absent from the hot start are
 *          left at their default initial conditions.
 *
 * @param engine  Engine handle (must be in SWMM_STATE_INITIALIZED).
 * @param hs      Hot start handle (from swmm_hotstart_open()).
 * @returns SWMM_OK on success. Missing objects are warnings, not errors.
 *
 * @note Warnings are accessible via swmm_hotstart_warning_count() and
 *       swmm_hotstart_warning(). If a warning callback is registered, each
 *       missing object also fires SWMM_WARN_HOTSTART_MISSING.
 */
SWMM_ENGINE_API int swmm_hotstart_apply(SWMM_Engine engine, SWMM_HotStart hs);

/* =========================================================================
 * Modify hot start data
 * ========================================================================= */

/**
 * @brief Modify the stored depth for a node in the hot start file.
 *
 * @details This allows adjusting initial conditions before applying a hot
 *          start. Changes are written back to the file when the handle is closed.
 *
 * @param hs       Hot start handle.
 * @param node_id  Node identifier string.
 * @param depth    New depth in the original project length units.
 * @returns SWMM_OK, or SWMM_ERR_BADPARAM if node_id not found in hot start.
 */
SWMM_ENGINE_API int swmm_hotstart_set_node_depth(
    SWMM_HotStart hs,
    const char*   node_id,
    double        depth
);

/**
 * @brief Modify the stored head for a node.
 * @param hs      Hot start handle.
 * @param node_id Node identifier.
 * @param head    New head value.
 * @returns SWMM_OK or SWMM_ERR_BADPARAM.
 */
SWMM_ENGINE_API int swmm_hotstart_set_node_head(
    SWMM_HotStart hs,
    const char*   node_id,
    double        head
);

/**
 * @brief Modify the stored flow for a link.
 * @param hs      Hot start handle.
 * @param link_id Link identifier.
 * @param flow    New flow value.
 * @returns SWMM_OK or SWMM_ERR_BADPARAM.
 */
SWMM_ENGINE_API int swmm_hotstart_set_link_flow(
    SWMM_HotStart hs,
    const char*   link_id,
    double        flow
);

/**
 * @brief Modify the stored depth for a link.
 * @param hs      Hot start handle.
 * @param link_id Link identifier.
 * @param depth   New depth value.
 * @returns SWMM_OK or SWMM_ERR_BADPARAM.
 */
SWMM_ENGINE_API int swmm_hotstart_set_link_depth(
    SWMM_HotStart hs,
    const char*   link_id,
    double        depth
);

/**
 * @brief Modify the stored runoff for a subcatchment.
 * @param hs         Hot start handle.
 * @param subcatch_id Subcatchment identifier.
 * @param runoff     New runoff value.
 * @returns SWMM_OK or SWMM_ERR_BADPARAM.
 */
SWMM_ENGINE_API int swmm_hotstart_set_subcatch_runoff(
    SWMM_HotStart hs,
    const char*   subcatch_id,
    double        runoff
);

/* =========================================================================
 * Query hot start metadata
 * ========================================================================= */

/**
 * @brief Get the simulation timestamp stored in the hot start file.
 * @param hs         Hot start handle.
 * @param sim_time   [out] Simulation time (decimal days) at which state was saved.
 * @returns SWMM_OK or SWMM_ERR_HOTSTART.
 */
SWMM_ENGINE_API int swmm_hotstart_get_sim_time(SWMM_HotStart hs, double* sim_time);

/**
 * @brief Get the CRS string stored in the hot start file.
 * @param hs     Hot start handle.
 * @param buf    Caller-allocated buffer.
 * @param buflen Buffer size.
 * @returns SWMM_OK or SWMM_ERR_HOTSTART.
 */
SWMM_ENGINE_API int swmm_hotstart_get_crs(SWMM_HotStart hs, char* buf, int buflen);

/**
 * @brief Get the number of nodes stored in the hot start file.
 * @param hs  Hot start handle.
 * @returns Node count, or -1 on error.
 */
SWMM_ENGINE_API int swmm_hotstart_node_count(SWMM_HotStart hs);

/**
 * @brief Get the number of links stored in the hot start file.
 * @param hs  Hot start handle.
 * @returns Link count, or -1 on error.
 */
SWMM_ENGINE_API int swmm_hotstart_link_count(SWMM_HotStart hs);

/* =========================================================================
 * Warning access (populated after swmm_hotstart_apply())
 * ========================================================================= */

/**
 * @brief Get the number of warnings generated by the last swmm_hotstart_apply().
 *
 * @details Warnings are issued for objects that were in the hot start file
 *          but not found in the target engine model.
 *
 * @param hs  Hot start handle.
 * @returns Warning count (0 if no apply was called, or all objects matched).
 */
SWMM_ENGINE_API int swmm_hotstart_warning_count(SWMM_HotStart hs);

/**
 * @brief Get the i-th warning message from the last swmm_hotstart_apply().
 *
 * @param hs     Hot start handle.
 * @param index  Warning index [0, count-1].
 * @returns Null-terminated warning string (owned by hs handle), or NULL.
 */
SWMM_ENGINE_API const char* swmm_hotstart_warning(SWMM_HotStart hs, int index);

/* =========================================================================
 * Close
 * ========================================================================= */

/**
 * @brief Close and free the hot start handle.
 *
 * @details If any modifications were made via swmm_hotstart_set_*(), the
 *          changes are written back to the file before closing.
 *
 * @param hs  Hot start handle. Safe to call with NULL.
 * @returns SWMM_OK, or SWMM_ERR_IO if write-back fails.
 */
SWMM_ENGINE_API int swmm_hotstart_close(SWMM_HotStart hs);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* OPENSWMM_ENGINE_HOTSTART_H */
