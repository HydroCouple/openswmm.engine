/**
 * @file openswmm_operator_snapshot.h
 * @brief Operator Snapshot Layer — per-substep linearized DW system state.
 *
 * @details Exposes the dynamic-wave solver's internal state (Jacobian entries,
 *          topology, regime flags, iteration telemetry) through a zero-copy
 *          snapshot structure and an optional per-substep callback.
 *
 *          **Design intent:**
 *          - The snapshot is populated at effectively zero cost when no callback
 *            is registered (single null-pointer check).
 *          - When enabled, the callback receives read-only pointers into the
 *            solver's per-instance buffers for the duration of the call.
 *            The pointers are **only valid inside the callback**; do not
 *            retain them after the callback returns.
 *          - All state is per-engine-instance.  No file-scope statics.
 *          - The callback is invoked on the main simulation thread, inside
 *            the routing substep, after Picard convergence but before the
 *            engine advances to the next substep.
 *          - The callback must NOT call back into mutable engine APIs.
 *
 *          **Sign convention for dqdh:**
 *          For directed link `j` with flow from node1 → node2:
 *            - `dqdh_up[j]  = +dqdh[j]` (sensitivity at upstream node)
 *            - `dqdh_down[j] = -dqdh[j]` (sensitivity at downstream node)
 *          Users constructing a Jacobian or graph-Laplacian operator must
 *          respect this sign convention.
 *
 *          **Future integration boundary (pyBME / pybme-mcp):**
 *          This header exposes raw evidence and operator summaries.  A future
 *          adapter, plugin, or MCP bridge may translate these into pyBME-style
 *          evidence objects (network specification, hard/soft observations,
 *          time-indexed edge-weight summaries for spectral-Hodge operators).
 *          This change does NOT implement BME inference inside SWMM.
 *
 * @ingroup engine_api
 * @see openswmm_engine.h
 * @see DynamicWave.hpp (internal solver)
 *
 * @author   OpenSWMM Contributors
 * @copyright Copyright (c) 2026. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_OPERATOR_SNAPSHOT_H
#define OPENSWMM_OPERATOR_SNAPSHOT_H

#include "openswmm_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Snapshot structure
 * ========================================================================= */

/**
 * @brief Per-substep operator snapshot — read-only view of DW solver state.
 *
 * @details All pointer fields reference per-instance solver buffers.
 *          They are valid only inside the operator snapshot callback.
 *          Array sizes are given by `n_nodes` and `n_links`.
 *
 *          Optional sections (Anderson acceleration, dynamic slot) have their
 *          pointer fields set to NULL when the corresponding feature is
 *          disabled.
 */
typedef struct SWMM_OperatorSnapshot {

    /* ----- Dimensions ----- */

    int n_nodes;            /**< Number of nodes in the model. */
    int n_links;            /**< Number of links in the model. */
    int n_conduits;         /**< Number of conduit links (subset of n_links). */

    /* ----- Directed topology (immutable after model load) ----- */

    const int* node1;       /**< [n_links] Upstream node index per link. */
    const int* node2;       /**< [n_links] Downstream node index per link. */
    const int* link_type;   /**< [n_links] Link type (0=CONDUIT,1=PUMP,2=ORIFICE,3=WEIR,4=OUTLET). */

    /* ----- Per-link state ----- */

    const double* link_flow;      /**< [n_links] Current flow (+ve = node1→node2). */
    const double* dqdh;           /**< [n_links] dQ/dH from momentum equation.
                                       Sign convention: dqdh_up = +dqdh[j], dqdh_down = -dqdh[j]. */
    const double* link_velocity;  /**< [n_links] Current velocity (ft/s or m/s). */
    const double* link_froude;    /**< [n_links] Current Froude number. */
    const double* link_area_mid;  /**< [n_links] Midpoint cross-section area. */
    const int8_t* flow_class;     /**< [n_links] FlowClass enum (0=DRY..6=DN_CRITICAL). */
    const uint8_t* bypassed;      /**< [n_links] Non-zero if link was bypassed (both nodes converged). */

    /* ----- Per-node state ----- */

    const double* node_head;      /**< [n_nodes] Current hydraulic head. */
    const double* node_depth;     /**< [n_nodes] Current water depth. */
    const double* node_volume;    /**< [n_nodes] Current stored volume. */
    const double* sumdqdh;        /**< [n_nodes] Jacobian diagonal: sum of dQ/dH for all connected links. */

    /* ----- Per-node convergence flags ----- */

    const uint8_t* node_converged; /**< [n_nodes] Non-zero if node converged in Picard iteration. */
    const uint8_t* node_surcharged;/**< [n_nodes] Non-zero if node is surcharged. */

    /* ----- Picard iteration telemetry ----- */

    int    iterations;      /**< Number of Picard iterations used this substep. */
    int    converged;       /**< Non-zero if Picard loop converged this substep. */
    double routing_dt;      /**< Routing substep duration (seconds). */
    double sim_time;        /**< Current simulation time (decimal days from start). */

    /* ----- Timestep telemetry ----- */

    double adaptive_dt;             /**< Current adaptive routing step (seconds), or 0 if fixed. */
    int    cfl_critical_link;       /**< Link index that constrained the CFL timestep (-1 if N/A). */

    /* ----- Anderson acceleration (optional, NULL when disabled) ----- */

    const double* aa_y_prev;     /**< [n_nodes] Node depths at iteration k-1 (NULL if AA off). */
    const double* aa_g_prev;     /**< [n_nodes] G(y_{k-1}) computed depths (NULL if AA off). */
    const double* aa_r_prev;     /**< [n_nodes] Residual r_{k-1} = G - y (NULL if AA off). */

    /* ----- Dynamic Preissmann Slot (optional, NULL when DPS disabled) ----- */

    const double* dps_slot_area;      /**< [n_conduits] Accumulated slot area As (NULL if not DPS). */
    const double* dps_surcharge_head; /**< [n_conduits] Current surcharge head hs (NULL if not DPS). */
    const double* dps_preissmann_num; /**< [n_conduits] Current Preissmann Number P (NULL if not DPS). */

    /* ----- Unit metadata ----- */

    int flow_units;         /**< Flow unit code (0=CFS,1=GPM,2=MGD,3=CMS,4=LPS,5=MLD). */
    int surcharge_method;   /**< Surcharge method (0=EXTRAN,1=SLOT,2=DYNAMIC_SLOT). */

} SWMM_OperatorSnapshot;

/* =========================================================================
 * Callback typedef
 * ========================================================================= */

/**
 * @brief Called after each DW routing substep with the operator snapshot.
 *
 * @details Invoked on the main simulation thread after Picard convergence
 *          and post-loop bookkeeping, but before advancing to the next substep.
 *
 *          The snapshot pointer is valid only for the duration of this call.
 *          The callback must NOT:
 *          - Retain pointers from the snapshot beyond the call
 *          - Call mutable engine API functions
 *          - Block for extended periods (the routing loop is paused)
 *
 * @param engine    The engine handle.
 * @param snap      Read-only operator snapshot for this substep.
 * @param user_data User-supplied context pointer from registration.
 */
typedef void (*SWMM_OperatorSnapshotCallback)(
    SWMM_Engine                     engine,
    const SWMM_OperatorSnapshot*    snap,
    void*                           user_data
);

/* =========================================================================
 * Registration and query API
 * ========================================================================= */

/**
 * @brief Register an operator snapshot callback.
 *
 * @details The callback is invoked once per DW routing substep. Pass NULL
 *          to unregister. Only one callback may be active at a time; a new
 *          registration replaces the previous one.
 *
 * @param engine    Engine handle.
 * @param callback  Callback function, or NULL to unregister.
 * @param user_data Opaque pointer passed to the callback.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_set_operator_snapshot_callback(
    SWMM_Engine                     engine,
    SWMM_OperatorSnapshotCallback   callback,
    void*                           user_data
);

/**
 * @brief Query the most recent operator snapshot (poll mode).
 *
 * @details Returns a pointer to the last populated snapshot.  The snapshot
 *          is only valid after at least one routing substep has completed
 *          and before the engine is destroyed.  The pointer is stable until
 *          the next routing substep overwrites it.
 *
 *          If no substep has been executed yet, *out_snap is set to NULL.
 *
 * @param engine   Engine handle.
 * @param out_snap Receives a pointer to the snapshot (read-only).
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_get_operator_snapshot(
    SWMM_Engine                     engine,
    const SWMM_OperatorSnapshot**   out_snap
);

/**
 * @brief Enable iteration history recording.
 *
 * @details When enabled, the engine records per-node residual vectors for
 *          each Picard iteration in a ring buffer.  This is intended for
 *          advanced diagnostics and spectral-CFL analysis.
 *
 *          Call with max_iters=0 to disable and free the ring buffer.
 *
 * @param engine     Engine handle.
 * @param max_iters  Maximum number of iterations to record (0 = disable).
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_enable_iteration_history(
    SWMM_Engine engine,
    int         max_iters
);

/**
 * @brief Query one step of iteration history.
 *
 * @details Retrieves the per-node residual vector for the specified Picard
 *          iteration of the most recent routing substep.
 *
 * @param engine    Engine handle.
 * @param iter      Zero-based Picard iteration index.
 * @param residuals [out] Caller-allocated buffer of size n_nodes.
 * @param n_nodes   Size of the residuals buffer.
 * @returns SWMM_OK, SWMM_ERR_BADINDEX if iter is out of range, or error.
 */
SWMM_ENGINE_API int swmm_get_iteration_residual(
    SWMM_Engine engine,
    int         iter,
    double*     residuals,
    int         n_nodes
);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* OPENSWMM_OPERATOR_SNAPSHOT_H */
