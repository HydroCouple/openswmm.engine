// SPDX-License-Identifier: Apache-2.0
/** @file openswmm_trace.h
 * @brief Offline flow tracing and travel time over reported hydraulic averages.
 * @ingroup engine_api
 * All dimensional values in this API are SI (m, m3, m3/s, m/s, seconds).
 * Handles are independent, own their inputs, and must be used by one thread at
 * a time. Callbacks run on that thread. No editable model handle is retained.
 * Times are frozen-average, passage-weighted estimates, not particle arrivals.
 */
#ifndef OPENSWMM_TRACE_H
#define OPENSWMM_TRACE_H
#include "openswmm_engine_export.h"
#ifdef __cplusplus
extern "C"
{
#endif

    typedef void *SWMM_Trace;
    typedef int (*SWMM_TraceProgress)(double fraction, const char *stage, void *user);
    /* Return nonzero from progress to cancel. */
    enum
    {
        SWMM_TRACE_OK = 0,
        SWMM_TRACE_INVALID = -1,
        SWMM_TRACE_IO = -2,
        SWMM_TRACE_MISMATCH = -3,
        SWMM_TRACE_CANCELLED = -4,
        SWMM_TRACE_SOLVER = -5,
        SWMM_TRACE_NO_HDF5 = -6
    };
    enum
    {
        SWMM_TRACE_DOWNSTREAM = 0,
        SWMM_TRACE_UPSTREAM = 1
    };
    enum
    {
        SWMM_TRACE_PONDING = 1,
        SWMM_TRACE_EXTERNAL_EXCHANGE = 2,
        SWMM_TRACE_ROUTED_OUTFALL = 4,
        SWMM_TRACE_UNKNOWN_LOSSES = 8
    };
    enum
    {
        SWMM_TRACE_UNREACHABLE = 1,
        SWMM_TRACE_NO_DIRECTION = 2,
        SWMM_TRACE_REVERSAL = 4,
        SWMM_TRACE_UNKNOWN_TIME = 8,
        SWMM_TRACE_PARTIAL_TIME = 16,
        SWMM_TRACE_TRAPPED = 32,
        SWMM_TRACE_APPROXIMATE = 64
    };
    enum
    {
        SWMM_TRACE_OUTFALL = 0,
        SWMM_TRACE_LOSS = 1,
        SWMM_TRACE_SOURCE = 2,
        SWMM_TRACE_RETAINED = 3,
        SWMM_TRACE_UNRESOLVED = 4,
        SWMM_TRACE_CIRCULATION = 5,
        SWMM_TRACE_TERMINAL_COUNT = 6
    };

    typedef struct SWMM_TraceNodeInput
    {
        const char *id;
        int type;
        int flags;
    } SWMM_TraceNodeInput;
    typedef struct SWMM_TraceLinkInput
    {
        const char *id;
        int from_node;
        int to_node;
        int type;
        double length_m;
    } SWMM_TraceLinkInput;
    typedef struct SWMM_TraceOptions
    {
        double flow_epsilon_m3s;
        double velocity_epsilon_mps;
        double reversal_dominance;
        double solver_tolerance;
        int max_iterations;
    } SWMM_TraceOptions;
    typedef struct SWMM_TraceNodeAverage
    {
        double volume_m3, inflow_m3s, lateral_in_m3s, withdrawal_m3s;
        double overflow_m3s, outgoing_m3s, residence_s;
        double first_volume_m3, last_volume_m3;
        int flags;
    } SWMM_TraceNodeAverage;
    typedef struct SWMM_TraceLinkAverage
    {
        double net_flow_m3s, absolute_flow_m3s, absolute_velocity_mps;
        double forward_volume_m3, reverse_volume_m3, travel_s;
        double first_volume_m3, last_volume_m3;
        int direction, flags;
    } SWMM_TraceLinkAverage;
    typedef struct SWMM_TraceInfo
    {
        int schema_version, algorithm_version, node_count, link_count;
        int periods, source_flow_units, cache_loaded;
        double first_report_date, last_report_date, duration_s;
    } SWMM_TraceInfo;
    typedef struct SWMM_TraceValue
    {
        double ratio;                  /**< Expected passages per unit; may exceed 1. */
        double time_s;                 /**< NaN means unknown, never a zero substitute. */
        double time_coverage;          /**< Known-time passages / total passages. */
        double from_time_s, to_time_s; /**< Edge-conditioned physical endpoint times. */
        double terminal_fraction;      /**< Node absorption, NOT sum of incident ratios. */
        int terminal_kind, flags;
    } SWMM_TraceValue;
    typedef struct SWMM_TraceSummary
    {
        double terminal[SWMM_TRACE_TERMINAL_COUNT];
        double accounting_error, solver_residual;
        double boundary_in_m3, boundary_out_m3, lateral_in_m3, withdrawal_m3;
        double known_loss_m3, storage_change_m3, partial_balance_residual_m3;
        int cyclic, reached_nodes, reached_links;
    } SWMM_TraceSummary;

    SWMM_ENGINE_API void swmm_trace_default_options(SWMM_TraceOptions *options);
    /** Copies topology and IDs. Counts may be zero only for links. */
    SWMM_ENGINE_API int swmm_trace_create(const SWMM_TraceNodeInput *nodes, int node_count,
                                          const SWMM_TraceLinkInput *links, int link_count,
                                          const SWMM_TraceOptions *options, SWMM_Trace *handle);
    SWMM_ENGINE_API void swmm_trace_close(SWMM_Trace handle);
    SWMM_ENGINE_API const char *swmm_trace_error(SWMM_Trace handle);
    /** Scan a completed output, matching by ID (not row order). Cache is optional.
     * fingerprint is a caller-verified content digest of the output. It must change
     * whenever output bytes change; path/mtime alone are not valid fingerprints.
     * The engine also binds the cache to copied topology/options and checks file
     * size and modification time across the scan. Cache writes are atomic.
     */
    SWMM_ENGINE_API int swmm_trace_prepare(SWMM_Trace handle, const char *output_path,
                                           const char *cache_path, const char *fingerprint,
                                           SWMM_TraceProgress progress, void *user);
    /** Restore validated averages, e.g. from a saved analysis, or analytic fixtures.
     * Derived direction/delay/throughput fields are recomputed. */
    SWMM_ENGINE_API int swmm_trace_set_averages(SWMM_Trace handle,
                                                const SWMM_TraceNodeAverage *nodes, int node_count,
                                                const SWMM_TraceLinkAverage *links, int link_count,
                                                const SWMM_TraceInfo *info);
    SWMM_ENGINE_API int swmm_trace_get_info(SWMM_Trace handle, SWMM_TraceInfo *info);
    SWMM_ENGINE_API int swmm_trace_get_averages(SWMM_Trace handle, SWMM_TraceNodeAverage *nodes,
                                                int node_capacity, SWMM_TraceLinkAverage *links,
                                                int link_capacity);
    SWMM_ENGINE_API const char *swmm_trace_node_id(SWMM_Trace handle, int index);
    SWMM_ENGINE_API const char *swmm_trace_link_id(SWMM_Trace handle, int index);
    SWMM_ENGINE_API int swmm_trace_get_topology(SWMM_Trace handle, SWMM_TraceNodeInput *nodes,
                                                int node_capacity, SWMM_TraceLinkInput *links,
                                                int link_capacity);
    SWMM_ENGINE_API int swmm_trace_estimate(SWMM_Trace handle, int direction, int seed,
                                            SWMM_TraceValue *nodes, int node_capacity,
                                            SWMM_TraceValue *links, int link_capacity,
                                            SWMM_TraceSummary *summary, SWMM_TraceProgress progress,
                                            void *user);
#ifdef __cplusplus
}
#endif
#endif
