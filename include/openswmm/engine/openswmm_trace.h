// SPDX-License-Identifier: Apache-2.0
/**
 * @file      openswmm_trace.h
 * @brief     Offline flow tracing and travel time over reported hydraulic averages.
 * @ingroup engine_api
 * All dimensional values in this API are SI (m, m3, m3/s, m/s, seconds).
 * Handles are independent, own their inputs, and must be used by one thread at
 * a time. Callbacks run on that thread. No editable model handle is retained.
 * Times are frozen-average, passage-weighted estimates, not particle arrivals.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_TRACE_H
#define OPENSWMM_TRACE_H
#include "openswmm_engine_export.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef void* SWMM_Trace;
/**
 * @brief Receive synchronous progress during trace preparation or estimation.
 *
 * Must not throw, reenter or close the trace handle; copy the stage string if it must be retained.
 *
 * @param[in] fraction Operation progress fraction, conventionally in [0,1].
 * @param[in] stage Borrowed NUL-terminated stage description, valid only for this invocation.
 * @param[in] user Borrowed context pointer supplied to the operation; may be NULL.
 * @return Zero to continue, any nonzero value to cancel.
 */
typedef int (*SWMM_TraceProgress)(double fraction, const char* stage, void* user);
/* Return nonzero from progress to cancel. */
enum {
    SWMM_TRACE_OK = 0,
    SWMM_TRACE_INVALID = -1,
    SWMM_TRACE_IO = -2,
    SWMM_TRACE_MISMATCH = -3,
    SWMM_TRACE_CANCELLED = -4,
    SWMM_TRACE_SOLVER = -5,
    SWMM_TRACE_NO_HDF5 = -6
};
enum { SWMM_TRACE_DOWNSTREAM = 0, SWMM_TRACE_UPSTREAM = 1 };
enum {
    SWMM_TRACE_PONDING = 1,
    SWMM_TRACE_EXTERNAL_EXCHANGE = 2,
    SWMM_TRACE_ROUTED_OUTFALL = 4,
    SWMM_TRACE_UNKNOWN_LOSSES = 8
};
enum {
    SWMM_TRACE_UNREACHABLE = 1,
    SWMM_TRACE_NO_DIRECTION = 2,
    SWMM_TRACE_REVERSAL = 4,
    SWMM_TRACE_UNKNOWN_TIME = 8,
    SWMM_TRACE_PARTIAL_TIME = 16,
    SWMM_TRACE_TRAPPED = 32,
    SWMM_TRACE_APPROXIMATE = 64
};
enum {
    SWMM_TRACE_OUTFALL = 0,
    SWMM_TRACE_LOSS = 1,
    SWMM_TRACE_SOURCE = 2,
    SWMM_TRACE_RETAINED = 3,
    SWMM_TRACE_UNRESOLVED = 4,
    SWMM_TRACE_CIRCULATION = 5,
    SWMM_TRACE_TERMINAL_COUNT = 6
};

typedef struct SWMM_TraceNodeInput {
    const char* id;
    int type;
    int flags;
} SWMM_TraceNodeInput;
typedef struct SWMM_TraceLinkInput {
    const char* id;
    int from_node;
    int to_node;
    int type;
    double length_m;
} SWMM_TraceLinkInput;
typedef struct SWMM_TraceOptions {
    double flow_epsilon_m3s;
    double velocity_epsilon_mps;
    double reversal_dominance;
    double solver_tolerance;
    int max_iterations;
} SWMM_TraceOptions;
typedef struct SWMM_TraceNodeAverage {
    double volume_m3, inflow_m3s, lateral_in_m3s, withdrawal_m3s;
    double overflow_m3s, outgoing_m3s, residence_s;
    double first_volume_m3, last_volume_m3;
    int flags;
} SWMM_TraceNodeAverage;
typedef struct SWMM_TraceLinkAverage {
    double net_flow_m3s, absolute_flow_m3s, absolute_velocity_mps;
    double forward_volume_m3, reverse_volume_m3, travel_s;
    double first_volume_m3, last_volume_m3;
    int direction, flags;
} SWMM_TraceLinkAverage;
typedef struct SWMM_TraceInfo {
    int schema_version, algorithm_version, node_count, link_count;
    int periods, source_flow_units, cache_loaded;
    double first_report_date, last_report_date, duration_s;
} SWMM_TraceInfo;
typedef struct SWMM_TraceValue {
    double ratio;                  /**< Expected passages per unit; may exceed 1. */
    double time_s;                 /**< NaN means unknown, never a zero substitute. */
    double time_coverage;          /**< Known-time passages / total passages. */
    double from_time_s, to_time_s; /**< Edge-conditioned physical endpoint times. */
    double terminal_fraction;      /**< Node absorption, NOT sum of incident ratios. */
    int terminal_kind, flags;
} SWMM_TraceValue;
typedef struct SWMM_TraceSummary {
    double terminal[SWMM_TRACE_TERMINAL_COUNT];
    double accounting_error, solver_residual;
    double boundary_in_m3, boundary_out_m3, lateral_in_m3, withdrawal_m3;
    double known_loss_m3, storage_change_m3, partial_balance_residual_m3;
    int cyclic, reached_nodes, reached_links;
} SWMM_TraceSummary;

/**
 * @brief Fill the standard SI flow-tracing solver tolerances.
 *
 * Writes flow_epsilon_m3s=1e-9, velocity_epsilon_mps=1e-6, reversal_dominance=0.8,
 * solver_tolerance=1e-11 and max_iterations=10000.
 *
 * @param[out] options Caller-owned options destination, or NULL for a no-op.
 */
SWMM_ENGINE_API void swmm_trace_default_options(SWMM_TraceOptions* options);
/**
 * @brief Copy model topology and options into an independent offline trace handle.
 *
 * No editable engine is retained. Close every successful handle exactly once. Call
 * prepare/prepare_live/set_averages before estimating; initial info has schema/algorithm version 2
 * and zero periods.
 *
 * @param[in] nodes Non-null array of node_count nodes. IDs must be nonempty and unique within
 *     nodes; node type must be 0..3. IDs and topology are copied.
 * @param[in] node_count Positive number of nodes.
 * @param[in] links Array of link_count links, or NULL when count=0. IDs must be nonempty/unique
 *     within links, endpoints must index nodes, and lengths must be finite and nonnegative in m.
 * @param[in] link_count Nonnegative number of links.
 * @param[in] options Optional borrowed options; NULL selects defaults. Positive finite
 *     flow/velocity epsilons, finite reversal_dominance in [0,1], finite tolerance in (0,0.01] and
 *     max_iterations>=1 are required.
 * @param[out] handle Non-null output slot, cleared to NULL before validation. Success transfers
 *     ownership of a new handle; any previous value in this variable is not freed.
 * @return SWMM_TRACE_OK on success or SWMM_TRACE_INVALID for invalid inputs or caught construction
 *     exceptions.
 */
SWMM_ENGINE_API int swmm_trace_create(const SWMM_TraceNodeInput* nodes, int node_count,
                                      const SWMM_TraceLinkInput* links, int link_count,
                                      const SWMM_TraceOptions* options, SWMM_Trace* handle);
/**
 * @brief Release a trace handle and all copied analysis data.
 *
 * @param[in] handle Caller-owned trace handle, or NULL for a no-op. Do not use afterward, destroy
 *     twice or close during another operation/callback.
 */
SWMM_ENGINE_API void swmm_trace_close(SWMM_Trace handle);
/**
 * @brief Borrow the current trace diagnostic.
 *
 * Successful calls do not uniformly clear every diagnostic; an empty/nonempty error string is not a
 * substitute for the operation return code.
 *
 * @param[in] handle Live trace handle, or NULL to read the thread-local diagnostic for handle-less
 *     guarded failures.
 * @return Borrowed NUL-terminated string; never free it. Copy before another operation changes its
 *     storage or before closing a non-null handle.
 */
SWMM_ENGINE_API const char* swmm_trace_error(SWMM_Trace handle);
/**
 * @brief Build hydraulic averages from a completed output, optionally using a content-bound cache.
 *
 * Binds cache identity to copied topology/options and verifies file size/mtime across scanning.
 * Cache writes are atomic. Preparation invalidates the prior state before deriving new averages.
 * Missing cache digest or cache-write failure can return INVALID/IO after deriving usable prepared
 * averages; those late failures do not clear the prepared flag. Cancellation at the 0.9 pre-cache
 * progress
 * checkpoint explicitly clears preparation. The final 1.0 completion notification ignores its
 * callback return value. Use the return code to distinguish a fully completed
 * prepare/cache operation from these partial outcomes.
 *
 * @param[in] handle Non-null live trace handle, used by one thread at a time; caller retains
 *     ownership until swmm_trace_close().
 * @param[in] output_path Non-null, nonempty NUL-terminated completed successful binary-output path;
 *     IDs/counts must match copied topology, independent of row order.
 * @param[in] cache_path Optional NUL-terminated HDF5 cache destination/source; NULL/empty disables
 *     cache. A nonempty cache is refused when HDF5 support is unavailable.
 * @param[in] fingerprint Caller-verified NUL-terminated digest of output CONTENTS. Required for a
 *     reusable cache; NULL/empty cannot identify reusable cache data. Must change whenever output
 *     bytes change; path/mtime alone is insufficient.
 * @param[in] progress Optional synchronous progress callback on the calling thread. Any nonzero
 *     result cancels; must not throw, reenter or close the trace handle.
 * @param[in] user Borrowed callback context, passed through unchanged; may be NULL and need only
 *     remain valid during this synchronous call.
 * @return SWMM_TRACE_OK on prepared averages; SWMM_TRACE_INVALID for invalid input/averages or
 *     caught exceptions, SWMM_TRACE_IO for read/cache IO, SWMM_TRACE_MISMATCH for topology/file
 *     changes, SWMM_TRACE_CANCELLED for callback cancellation, or SWMM_TRACE_NO_HDF5 for an
 *     unsupported requested cache.
 */
SWMM_ENGINE_API int swmm_trace_prepare(SWMM_Trace handle, const char* output_path,
                                       const char* cache_path, const char* fingerprint,
                                       SWMM_TraceProgress progress, void* user);
/**
 * @brief Average complete reporting periods already present in a growing output.
 *
 * Scans the complete reporting periods present at open time, ignores subsequent appends and writes
 * no cache. Truncation during the scan is a mismatch. swmm_trace_get_info() reports the periods
 * used; the result is a frozen average, not a live particle simulation.
 *
 * @param[in] handle Non-null live trace handle, used by one thread at a time; caller retains
 *     ownership until swmm_trace_close().
 * @param[in] output_path Non-null, nonempty NUL-terminated running/cancelled output path; no
 *     closing success footer is required.
 * @param[in] progress Optional synchronous progress callback on the calling thread. Any nonzero
 *     result cancels; must not throw, reenter or close the trace handle.
 * @param[in] user Borrowed callback context, passed through unchanged; may be NULL and need only
 *     remain valid during this synchronous call.
 * @return SWMM_TRACE_OK; SWMM_TRACE_INVALID for invalid inputs/averages or caught exceptions,
 *     SWMM_TRACE_IO for unreadable output, SWMM_TRACE_MISMATCH for topology/truncation changes, or
 *     SWMM_TRACE_CANCELLED.
 */
SWMM_ENGINE_API int swmm_trace_prepare_live(SWMM_Trace handle, const char* output_path,
                                            SWMM_TraceProgress progress, void* user);
/**
 * @brief Copy supplied SI hydraulic averages and recompute derived trace fields.
 *
 * Derived link direction/travel time/flags and node outgoing flow/residence time/flags are
 * recomputed. The operation copies arrays before validating all metadata/averages, so refusal can
 * change stored data and clear preparation; a successful retry is required before estimation.
 *
 * @param[in] handle Non-null live trace handle, used by one thread at a time; caller retains
 *     ownership until swmm_trace_close().
 * @param[in] nodes Non-null node_count array in copied topology order. Base node
 *     volume/inflow/lateral-in/withdrawal/overflow values must be finite and nonnegative.
 * @param[in] node_count Must equal the copied topology node count exactly.
 * @param[in] links link_count array in topology order, or NULL when count=0. Net flow must be
 *     finite; absolute flow/velocity must be finite/nonnegative and absolute flow must cover
 *     absolute net flow within tolerance.
 * @param[in] link_count Must equal copied link count exactly.
 * @param[in] info Optional metadata to copy, with finite nonnegative duration_s. Node/link counts
 *     are overwritten from topology and schema/algorithm versions forced to 2; NULL keeps existing
 *     metadata.
 * @return SWMM_TRACE_OK when derived state is ready, otherwise SWMM_TRACE_INVALID.
 */
SWMM_ENGINE_API int swmm_trace_set_averages(SWMM_Trace handle, const SWMM_TraceNodeAverage* nodes,
                                            int node_count, const SWMM_TraceLinkAverage* links,
                                            int link_count, const SWMM_TraceInfo* info);
/**
 * @brief Copy current topology/reporting metadata.
 *
 * Does not require prepared averages; initial/failed-preparation metadata is not a certificate that
 * estimation is ready.
 *
 * @param[in] handle Non-null live trace handle, used by one thread at a time; caller retains
 *     ownership until swmm_trace_close().
 * @param[out] info Non-null caller-owned metadata destination.
 * @return SWMM_TRACE_OK, or SWMM_TRACE_INVALID for null handle/info.
 */
SWMM_ENGINE_API int swmm_trace_get_info(SWMM_Trace handle, SWMM_TraceInfo* info);
/**
 * @brief Copy prepared SI node/link averages in topology order.
 *
 * Checks preparation and both requested capacities before copying either array. Values use SI
 * m/m3/m3/s/m/s and seconds; unknown derived times may be NaN.
 *
 * @param[in] handle Non-null live trace handle, used by one thread at a time; caller retains
 *     ownership until swmm_trace_close().
 * @param[out] nodes Optional caller-owned node array; NULL suppresses node output.
 * @param[in] node_capacity Capacity in node records, at least the node count when nodes is
 *     non-null.
 * @param[out] links Optional caller-owned link array; NULL suppresses link output.
 * @param[in] link_capacity Capacity in link records, at least the link count when links is
 *     non-null.
 * @return SWMM_TRACE_OK; SWMM_TRACE_INVALID for null handle, unprepared state or a short requested
 *     output array.
 */
SWMM_ENGINE_API int swmm_trace_get_averages(SWMM_Trace handle, SWMM_TraceNodeAverage* nodes,
                                            int node_capacity, SWMM_TraceLinkAverage* links,
                                            int link_capacity);
/**
 * @brief Borrow a copied topology node ID.
 *
 * @param[in] handle Non-null live trace handle, used by one thread at a time; caller retains
 *     ownership until swmm_trace_close().
 * @param[in] index Zero-based node index.
 * @return Handle-owned NUL-terminated ID, or NULL for null handle/out-of-range index. Never free
 *     it; valid until handle destruction.
 */
SWMM_ENGINE_API const char* swmm_trace_node_id(SWMM_Trace handle, int index);
/**
 * @brief Borrow a copied topology link ID.
 *
 * @param[in] handle Non-null live trace handle, used by one thread at a time; caller retains
 *     ownership until swmm_trace_close().
 * @param[in] index Zero-based link index.
 * @return Handle-owned NUL-terminated ID, or NULL for null handle/out-of-range index. Never free
 *     it; valid until handle destruction.
 */
SWMM_ENGINE_API const char* swmm_trace_link_id(SWMM_Trace handle, int index);
/**
 * @brief Copy topology records while borrowing their stable ID strings.
 *
 * Does not require prepared averages. Checks both requested capacities before copying; caller owns
 * array storage but must not free the borrowed id pointers.
 *
 * @param[in] handle Non-null live trace handle, used by one thread at a time; caller retains
 *     ownership until swmm_trace_close().
 * @param[out] nodes Optional caller-owned node array; record id pointers refer to handle-owned
 *     storage, valid until close.
 * @param[in] node_capacity Capacity in node records, at least the copied node count when nodes is
 *     non-null.
 * @param[out] links Optional caller-owned link array; IDs remain handle-owned and lengths are m.
 * @param[in] link_capacity Capacity in link records, at least the copied link count when links is
 *     non-null.
 * @return SWMM_TRACE_OK; SWMM_TRACE_INVALID for null handle or insufficient requested capacity.
 */
SWMM_ENGINE_API int swmm_trace_get_topology(SWMM_Trace handle, SWMM_TraceNodeInput* nodes,
                                            int node_capacity, SWMM_TraceLinkInput* links,
                                            int link_capacity);
/**
 * @brief Estimate upstream/downstream passage ratios and frozen-average travel times from a node.
 *
 * Ratios are expected passages per unit and can exceed 1 in circulation; node terminal_fraction is
 * absorption, not a sum of incident ratios. Time is passage-weighted over frozen report averages,
 * with NaN for unknown time and time_coverage reporting known-time fraction. Outputs are not
 * promised unchanged on cancellation/solver failure; inspect only on success. Reads copied analysis
 * data without mutating a live model.
 *
 * @param[in] handle Non-null live trace handle, used by one thread at a time; caller retains
 *     ownership until swmm_trace_close().
 * @param[in] direction SWMM_TRACE_DOWNSTREAM (0) or SWMM_TRACE_UPSTREAM (1).
 * @param[in] seed Zero-based node index for unit release (downstream) or traced receipt (upstream).
 * @param[out] nodes Non-null caller-owned array of node_capacity results in topology order.
 * @param[in] node_capacity At least the full copied node count.
 * @param[out] links Caller-owned array of link_capacity results; may be NULL only when topology has
 *     zero links.
 * @param[in] link_capacity At least the full copied link count, even when suppressing an empty
 *     topology.
 * @param[out] summary Non-null caller-owned terminal/accounting/solver summary destination.
 * @param[in] progress Optional synchronous progress callback on the calling thread. Any nonzero
 *     result cancels; must not throw, reenter or close the trace handle.
 * @param[in] user Borrowed callback context, passed through unchanged; may be NULL and need only
 *     remain valid during this synchronous call.
 * @return SWMM_TRACE_OK on solved results; SWMM_TRACE_INVALID for invalid handle/arguments or
 *     unprepared state, SWMM_TRACE_CANCELLED for callback cancellation, or SWMM_TRACE_SOLVER when
 *     solver convergence fails.
 */
SWMM_ENGINE_API int swmm_trace_estimate(SWMM_Trace handle, int direction, int seed,
                                        SWMM_TraceValue* nodes, int node_capacity,
                                        SWMM_TraceValue* links, int link_capacity,
                                        SWMM_TraceSummary* summary, SWMM_TraceProgress progress,
                                        void* user);
#ifdef __cplusplus
}
#endif
#endif
