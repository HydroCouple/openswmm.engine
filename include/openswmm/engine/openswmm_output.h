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
 * @file openswmm_output.h
 * @brief OpenSWMM Engine — Output File Reader C API.
 *
 * @details Provides a transparent C89-compatible API for reading binary output
 *          files (.out) produced by the DefaultOutputPlugin. The binary format
 *          is the standard SWMM 5.x output format containing:
 *            - Header (magic, version, flow units, object counts)
 *            - Object ID names
 *            - Input properties (subcatchment areas, node geometry, link geometry)
 *            - Per-period time series results (subcatch/node/link/system)
 *            - Footer (section offsets, period count, error code)
 *
 *          Usage:
 *          @code{.c}
 *          #include <openswmm/engine/openswmm_output.h>
 *
 *          SWMM_Output out = swmm_output_open("model.out");
 *          if (!out) { // handle error }
 *
 *          int n = swmm_output_get_node_count(out);
 *          int periods = swmm_output_get_period_count(out);
 *
 *          float* depths = malloc(n * sizeof(float));
 *          swmm_output_get_node_result(out, 5, SWMM_OUT_NODE_DEPTH, depths);
 *          // depths[j] is node j depth at period 5
 *
 *          float ts[1];
 *          swmm_output_get_node_time_series(out, 0, SWMM_OUT_NODE_DEPTH,
 *                                            0, periods - 1, ts_buf);
 *
 *          swmm_output_close(out);
 *          @endcode
 *
 * @ingroup engine_api
 * @see DefaultOutputPlugin.hpp — the writer that produces these files
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_OUTPUT_H
#define OPENSWMM_OUTPUT_H

#include "openswmm_engine_export.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Opaque handle
 * ========================================================================= */

/** @brief Opaque handle to an opened output file reader. */
typedef void* SWMM_Output;

/* =========================================================================
 * Result variable enumerations
 * ========================================================================= */

/**
 * @brief Subcatchment result variable indices.
 *
 * @details Variables stored per subcatchment per output period.
 *          Pollutant concentrations follow at indices >= SWMM_OUT_SUBCATCH_POLLUT_BASE.
 */
typedef enum SWMM_OutSubcatchVar {
    SWMM_OUT_SUBCATCH_RAINFALL      = 0, /**< Rainfall rate. */
    SWMM_OUT_SUBCATCH_SNOW_DEPTH    = 1, /**< Snow depth. */
    SWMM_OUT_SUBCATCH_EVAP          = 2, /**< Evaporation loss. */
    SWMM_OUT_SUBCATCH_INFIL         = 3, /**< Infiltration loss. */
    SWMM_OUT_SUBCATCH_RUNOFF        = 4, /**< Runoff flow rate. */
    SWMM_OUT_SUBCATCH_GW_FLOW       = 5, /**< Groundwater flow. */
    SWMM_OUT_SUBCATCH_GW_ELEV       = 6, /**< Groundwater table elevation. */
    SWMM_OUT_SUBCATCH_SOIL_MOIST    = 7, /**< Soil moisture content. */
    SWMM_OUT_SUBCATCH_POLLUT_BASE   = 8  /**< First pollutant concentration slot. */
} SWMM_OutSubcatchVar;

/**
 * @brief Node result variable indices.
 *
 * @details Variables stored per node per output period.
 *          Pollutant concentrations follow at indices >= SWMM_OUT_NODE_POLLUT_BASE.
 */
typedef enum SWMM_OutNodeVar {
    SWMM_OUT_NODE_DEPTH          = 0, /**< Water depth above invert. */
    SWMM_OUT_NODE_HEAD           = 1, /**< Hydraulic head. */
    SWMM_OUT_NODE_VOLUME         = 2, /**< Stored water volume. */
    SWMM_OUT_NODE_LATERAL_INFLOW = 3, /**< Lateral inflow rate. */
    SWMM_OUT_NODE_TOTAL_INFLOW   = 4, /**< Total inflow rate. */
    SWMM_OUT_NODE_OVERFLOW       = 5, /**< Overflow rate. */
    SWMM_OUT_NODE_POLLUT_BASE    = 6  /**< First pollutant concentration slot. */
} SWMM_OutNodeVar;

/**
 * @brief Link result variable indices.
 *
 * @details Variables stored per link per output period.
 *          Pollutant concentrations follow at indices >= SWMM_OUT_LINK_POLLUT_BASE.
 */
typedef enum SWMM_OutLinkVar {
    SWMM_OUT_LINK_FLOW           = 0, /**< Flow rate. */
    SWMM_OUT_LINK_DEPTH          = 1, /**< Flow depth. */
    SWMM_OUT_LINK_VELOCITY       = 2, /**< Flow velocity. */
    SWMM_OUT_LINK_VOLUME         = 3, /**< Volume. */
    SWMM_OUT_LINK_CAPACITY       = 4, /**< Fraction of conduit capacity. */
    SWMM_OUT_LINK_POLLUT_BASE    = 5  /**< First pollutant concentration slot. */
} SWMM_OutLinkVar;

/**
 * @brief System-wide result variable indices.
 *
 * @details 14 system-wide summary results stored per output period.
 */
typedef enum SWMM_OutSystemVar {
    SWMM_OUT_SYS_TEMPERATURE    =  0, /**< Air temperature. */
    SWMM_OUT_SYS_RAINFALL       =  1, /**< Average rainfall. */
    SWMM_OUT_SYS_SNOW_DEPTH     =  2, /**< Average snow depth. */
    SWMM_OUT_SYS_EVAP           =  3, /**< Average evaporation. */
    SWMM_OUT_SYS_INFIL          =  4, /**< Average infiltration. */
    SWMM_OUT_SYS_RUNOFF         =  5, /**< Total runoff flow. */
    SWMM_OUT_SYS_DW_INFLOW      =  6, /**< Dry weather inflow. */
    SWMM_OUT_SYS_GW_INFLOW      =  7, /**< Groundwater inflow. */
    SWMM_OUT_SYS_LAT_INFLOW     =  8, /**< Lateral inflow. */
    SWMM_OUT_SYS_FLOODING       =  9, /**< Total flooding. */
    SWMM_OUT_SYS_OUTFLOW        = 10, /**< Total outfall outflow. */
    SWMM_OUT_SYS_STORAGE        = 11, /**< Total storage volume. */
    SWMM_OUT_SYS_EVAP_TOTAL     = 12, /**< Total evaporation. */
    SWMM_OUT_SYS_PET            = 13  /**< Potential evapotranspiration. */
} SWMM_OutSystemVar;

/* =========================================================================
 * Lifecycle
 * ========================================================================= */

/**
 * @brief Open a binary output file for reading.
 *
 * @param path  Path to a .out file produced by DefaultOutputPlugin.
 * @returns Opaque handle, or NULL on failure (invalid file, I/O error).
 */
SWMM_ENGINE_API SWMM_Output swmm_output_open(const char* path);

/**
 * @brief Open a binary output file that is still being written.
 *
 * @details Unlike swmm_output_open(), this does not need the closing
 *          records: offsets come from a forward parse of the header and the
 *          period count from the file size (whole records only). Use it to
 *          tail a run in progress — both the 6.x engine and the legacy 5.x
 *          engines flush the file after every report period — or to salvage
 *          the completed periods of a run that died before finishing. Poll
 *          swmm_output_refresh() for growth. Never modifies the file.
 *
 * @param path  Path to a .out file whose header is complete.
 * @returns Opaque handle, or NULL if the header cannot be parsed yet.
 */
SWMM_ENGINE_API SWMM_Output swmm_output_open_live(const char* path);

/**
 * @brief Re-count the periods of a file opened with swmm_output_open_live().
 *
 * @details Once the writer's closing records appear, they are adopted
 *          (period count + error code) and the handle stops being live; it
 *          then behaves exactly like one from swmm_output_open(). On a
 *          non-live handle this is a no-op that reports the current count.
 *
 * @param handle   Output reader handle.
 * @param periods  Optional out: current period count.
 * @returns 0 on success, -1 on a NULL handle.
 */
SWMM_ENGINE_API int swmm_output_refresh(SWMM_Output handle, int* periods);

/**
 * @brief 1 while a handle from swmm_output_open_live() has not yet seen the
 *        closing records, 0 otherwise, -1 on NULL.
 */
SWMM_ENGINE_API int swmm_output_is_live(SWMM_Output handle);

/**
 * @brief Close the output file and free all resources.
 *
 * @param handle  Output reader handle (NULL safe).
 */
SWMM_ENGINE_API void swmm_output_close(SWMM_Output handle);

/* =========================================================================
 * Metadata queries
 * ========================================================================= */

/**
 * @brief Get the SWMM version stored in the output file.
 * @param handle  Output reader handle.
 * @returns Version number (e.g. 52001), or -1 on error.
 */
SWMM_ENGINE_API int swmm_output_get_version(SWMM_Output handle);

/**
 * @brief Get the flow units code.
 * @param handle  Output reader handle.
 * @returns Flow units code (0=CFS, 1=GPM, 2=MGD, 3=CMS, 4=LPS, 5=MLD),
 *          or -1 on error.
 */
SWMM_ENGINE_API int swmm_output_get_flow_units(SWMM_Output handle);

/**
 * @brief Get the number of subcatchments in the output file.
 * @param handle  Output reader handle.
 * @returns Subcatchment count, or -1 on error.
 */
SWMM_ENGINE_API int swmm_output_get_subcatch_count(SWMM_Output handle);

/**
 * @brief Get the number of nodes in the output file.
 * @param handle  Output reader handle.
 * @returns Node count, or -1 on error.
 */
SWMM_ENGINE_API int swmm_output_get_node_count(SWMM_Output handle);

/**
 * @brief Get the number of links in the output file.
 * @param handle  Output reader handle.
 * @returns Link count, or -1 on error.
 */
SWMM_ENGINE_API int swmm_output_get_link_count(SWMM_Output handle);

/**
 * @brief Get the number of pollutants in the output file.
 * @param handle  Output reader handle.
 * @returns Pollutant count, or -1 on error.
 */
SWMM_ENGINE_API int swmm_output_get_pollut_count(SWMM_Output handle);

/**
 * @brief Get the number of reporting periods (timesteps) in the output file.
 * @param handle  Output reader handle.
 * @returns Period count, or -1 on error.
 */
SWMM_ENGINE_API int swmm_output_get_period_count(SWMM_Output handle);

/**
 * @brief Get the simulation start date as a SWMM DateTime double.
 *
 * @details SWMM's native DateTime representation is a double where the
 *          integer part is days since 1899-12-30 (OLE Automation epoch)
 *          and the fractional part is the time-of-day fraction
 *          (0.5 = noon). Convert to a calendar date/time via the
 *          datetime functions in `openswmm_datetime.h`.
 *
 * @param handle      Output reader handle.
 * @param start_date  Pointer to receive the start date value (SWMM DateTime).
 * @returns 0 on success, -1 on error.
 */
SWMM_ENGINE_API int swmm_output_get_start_date(SWMM_Output handle,
                                                 double* start_date);

/**
 * @brief Get the report step in seconds.
 * @param handle  Output reader handle.
 * @returns Report step (seconds), or -1 on error.
 */
SWMM_ENGINE_API int swmm_output_get_report_step(SWMM_Output handle);

/* =========================================================================
 * Object ID retrieval
 * ========================================================================= */

/**
 * @brief Get the string ID of a subcatchment by index.
 * @param handle  Output reader handle.
 * @param index   Zero-based subcatchment index.
 * @returns  Null-terminated string owned by the reader, or NULL on error.
 */
SWMM_ENGINE_API const char* swmm_output_get_subcatch_id(SWMM_Output handle,
                                                          int index);

/**
 * @brief Get the string ID of a node by index.
 * @param handle  Output reader handle.
 * @param index   Zero-based node index.
 * @returns  Null-terminated string owned by the reader, or NULL on error.
 */
SWMM_ENGINE_API const char* swmm_output_get_node_id(SWMM_Output handle,
                                                      int index);

/**
 * @brief Get the string ID of a link by index.
 * @param handle  Output reader handle.
 * @param index   Zero-based link index.
 * @returns  Null-terminated string owned by the reader, or NULL on error.
 */
SWMM_ENGINE_API const char* swmm_output_get_link_id(SWMM_Output handle,
                                                      int index);

/**
 * @brief Get the string ID of a species (pollutant) column by index.
 *
 * @details The `.out` header carries one name per species column. Reading
 *          it is the ONLY way to identify a column's meaning: the per-column
 *          unit field is a three-value concentration enum, so the water-age
 *          column (`__WATER_AGE__`, reported in HOURS) necessarily reuses a
 *          concentration code. Consumers plotting or converting a species
 *          column must key on this name, not on the unit code.
 *
 * @param handle  Output reader handle.
 * @param index   Zero-based species index (0 .. pollut_count-1).
 * @returns  Null-terminated string owned by the reader, or NULL on error.
 */
SWMM_ENGINE_API const char* swmm_output_get_pollut_id(SWMM_Output handle,
                                                        int index);

/* =========================================================================
 * Per-period result retrieval (all objects, one variable, one period)
 * ========================================================================= */

/**
 * @brief Get one subcatchment variable for all subcatchments at a given period.
 *
 * @param handle   Output reader handle.
 * @param period   Zero-based period index.
 * @param var      Subcatchment variable (see @ref SWMM_OutSubcatchVar).
 * @param values   Caller-allocated array of at least subcatch_count floats.
 * @returns 0 on success, -1 on error.
 */
SWMM_ENGINE_API int swmm_output_get_subcatch_result(SWMM_Output handle,
                                                      int period,
                                                      int var,
                                                      float* values);

/**
 * @brief Get one node variable for all nodes at a given period.
 *
 * @param handle   Output reader handle.
 * @param period   Zero-based period index.
 * @param var      Node variable (see @ref SWMM_OutNodeVar).
 * @param values   Caller-allocated array of at least node_count floats.
 * @returns 0 on success, -1 on error.
 */
SWMM_ENGINE_API int swmm_output_get_node_result(SWMM_Output handle,
                                                  int period,
                                                  int var,
                                                  float* values);

/**
 * @brief Get one link variable for all links at a given period.
 *
 * @param handle   Output reader handle.
 * @param period   Zero-based period index.
 * @param var      Link variable (see @ref SWMM_OutLinkVar).
 * @param values   Caller-allocated array of at least link_count floats.
 * @returns 0 on success, -1 on error.
 */
SWMM_ENGINE_API int swmm_output_get_link_result(SWMM_Output handle,
                                                  int period,
                                                  int var,
                                                  float* values);

/**
 * @brief Get system-wide results at a given period.
 *
 * @param handle   Output reader handle.
 * @param period   Zero-based period index.
 * @param var      System variable (see @ref SWMM_OutSystemVar).
 * @param value    Pointer to receive the single float value.
 * @returns 0 on success, -1 on error.
 */
SWMM_ENGINE_API int swmm_output_get_system_result(SWMM_Output handle,
                                                    int period,
                                                    int var,
                                                    float* value);

/* =========================================================================
 * Time series retrieval (one object, one variable, period range)
 * ========================================================================= */

/**
 * @brief Get a subcatchment variable time series across a range of periods.
 *
 * @param handle       Output reader handle.
 * @param subcatch_idx Zero-based subcatchment index.
 * @param var          Subcatchment variable.
 * @param start_period First period (inclusive, zero-based).
 * @param end_period   Last period (inclusive, zero-based).
 * @param values       Caller-allocated array of (end_period - start_period + 1) floats.
 * @returns 0 on success, -1 on error.
 */
SWMM_ENGINE_API int swmm_output_get_subcatch_series(SWMM_Output handle,
                                                      int subcatch_idx,
                                                      int var,
                                                      int start_period,
                                                      int end_period,
                                                      float* values);

/**
 * @brief Get a node variable time series across a range of periods.
 *
 * @param handle       Output reader handle.
 * @param node_idx     Zero-based node index.
 * @param var          Node variable.
 * @param start_period First period (inclusive, zero-based).
 * @param end_period   Last period (inclusive, zero-based).
 * @param values       Caller-allocated array of (end_period - start_period + 1) floats.
 * @returns 0 on success, -1 on error.
 */
SWMM_ENGINE_API int swmm_output_get_node_series(SWMM_Output handle,
                                                  int node_idx,
                                                  int var,
                                                  int start_period,
                                                  int end_period,
                                                  float* values);

/**
 * @brief Get a link variable time series across a range of periods.
 *
 * @param handle       Output reader handle.
 * @param link_idx     Zero-based link index.
 * @param var          Link variable.
 * @param start_period First period (inclusive, zero-based).
 * @param end_period   Last period (inclusive, zero-based).
 * @param values       Caller-allocated array of (end_period - start_period + 1) floats.
 * @returns 0 on success, -1 on error.
 */
SWMM_ENGINE_API int swmm_output_get_link_series(SWMM_Output handle,
                                                  int link_idx,
                                                  int var,
                                                  int start_period,
                                                  int end_period,
                                                  float* values);

/**
 * @brief Get a system variable time series across a range of periods.
 *
 * @param handle       Output reader handle.
 * @param var          System variable.
 * @param start_period First period (inclusive, zero-based).
 * @param end_period   Last period (inclusive, zero-based).
 * @param values       Caller-allocated array of (end_period - start_period + 1) floats.
 * @returns 0 on success, -1 on error.
 */
SWMM_ENGINE_API int swmm_output_get_system_series(SWMM_Output handle,
                                                    int var,
                                                    int start_period,
                                                    int end_period,
                                                    float* values);

/* =========================================================================
 * Per-object result retrieval (all variables for one object at one period)
 * ========================================================================= */

/**
 * @brief Get all result variables for a single subcatchment at one period.
 *
 * @param handle       Output reader handle.
 * @param subcatch_idx Zero-based subcatchment index.
 * @param period       Zero-based period index.
 * @param values       Caller-allocated array of n_subcatch_vars floats.
 * @param count        Pointer to receive the number of variables written.
 * @returns 0 on success, -1 on error.
 */
SWMM_ENGINE_API int swmm_output_get_subcatch_attribute(SWMM_Output handle,
                                                        int subcatch_idx,
                                                        int period,
                                                        float* values,
                                                        int* count);

/**
 * @brief Get all result variables for a single node at one period.
 *
 * @param handle    Output reader handle.
 * @param node_idx  Zero-based node index.
 * @param period    Zero-based period index.
 * @param values    Caller-allocated array of n_node_vars floats.
 * @param count     Pointer to receive the number of variables written.
 * @returns 0 on success, -1 on error.
 */
SWMM_ENGINE_API int swmm_output_get_node_attribute(SWMM_Output handle,
                                                     int node_idx,
                                                     int period,
                                                     float* values,
                                                     int* count);

/**
 * @brief Get all result variables for a single link at one period.
 *
 * @param handle    Output reader handle.
 * @param link_idx  Zero-based link index.
 * @param period    Zero-based period index.
 * @param values    Caller-allocated array of n_link_vars floats.
 * @param count     Pointer to receive the number of variables written.
 * @returns 0 on success, -1 on error.
 */
SWMM_ENGINE_API int swmm_output_get_link_attribute(SWMM_Output handle,
                                                     int link_idx,
                                                     int period,
                                                     float* values,
                                                     int* count);

/* =========================================================================
 * Simulation time retrieval
 * ========================================================================= */

/**
 * @brief Get the simulation time for a given period as a SWMM DateTime double.
 *
 * @details The value is the same SWMM DateTime convention used elsewhere in
 *          the API: integer days since 1899-12-30 plus fractional time of
 *          day. Use the helpers in `openswmm_datetime.h` to convert to a
 *          calendar date/time.
 *
 * @param handle  Output reader handle.
 * @param period  Zero-based period index.
 * @param time    Pointer to receive the simulation time as a SWMM DateTime double.
 * @returns 0 on success, -1 on error.
 */
SWMM_ENGINE_API int swmm_output_get_period_time(SWMM_Output handle,
                                                  int period,
                                                  double* time);

/* =========================================================================
 * Per-node summary statistics — Slice QA-01
 *
 * The SWMM 5.x binary output format does not include a stats block; these
 * functions reconstruct the four flooding statistics at read time by
 * walking the per-period node results stored in the file. The semantics
 * mirror the engine-side `swmm_node_get_stat_*` accessors (which read
 * from SimulationContext.nodes.stat_*), except sourced from a SWMM_Output
 * handle so per-output comparisons work across multiple loaded .out
 * files. Implementations are O(n_periods) per call.
 *
 * Caveat: aggregation runs over REPORT-step samples, not the engine's
 * internal routing-step samples. For runs where the report step is much
 * coarser than the routing step (e.g. report every hour vs. route every
 * 5 s), `time_flooded` and `vol_flooded` will under-count brief
 * sub-report-step flooding events. The engine-side getters
 * (`swmm_node_get_stat_*`) retain the legacy routing-step precision —
 * use those when a fresh in-process run is the source.
 * ========================================================================= */

/**
 * @brief Maximum node depth across all reporting periods.
 *
 * @details Computes `max(SWMM_OUT_NODE_DEPTH)` over the open file's
 *          period range. Result is in the model's length units (ft / m).
 *
 * @param handle    Output reader handle.
 * @param node_idx  Zero-based node index (0 .. node_count - 1).
 * @param value     [out] Maximum depth on success; unchanged on error.
 * @returns 0 on success, -1 on error (bad handle, index out of range, or
 *          I/O failure during aggregation).
 */
SWMM_ENGINE_API int swmm_output_get_node_stat_max_depth(SWMM_Output handle,
                                                          int          node_idx,
                                                          double*      value);

/**
 * @brief Maximum node overflow rate across all reporting periods.
 *
 * @details Computes `max(SWMM_OUT_NODE_OVERFLOW)` over the open file's
 *          period range. Result is in the file's flow units
 *          (see swmm_output_get_flow_units).
 */
SWMM_ENGINE_API int swmm_output_get_node_stat_max_overflow(SWMM_Output handle,
                                                             int          node_idx,
                                                             double*      value);

/**
 * @brief Total flood volume at the node across the simulation.
 *
 * @details Aggregates `sum(overflow_i * report_step)` over the periods
 *          where overflow > 0. Result is in cubic feet (US) or cubic
 *          metres (SI) — matching `swmm_node_get_stat_vol_flooded` units.
 *          Returns 0 when the file has no positive-overflow periods.
 */
SWMM_ENGINE_API int swmm_output_get_node_stat_vol_flooded(SWMM_Output handle,
                                                            int          node_idx,
                                                            double*      value);

/**
 * @brief Total time the node was flooded across the simulation.
 *
 * @details Counts the report periods where overflow > 0 and multiplies
 *          by the file's report-step seconds. Result is in seconds —
 *          matching `swmm_node_get_stat_time_flooded` units. Divide by
 *          3600 for hours (the convention SWMM's statsrpt uses for
 *          display).
 */
SWMM_ENGINE_API int swmm_output_get_node_stat_time_flooded(SWMM_Output handle,
                                                             int          node_idx,
                                                             double*      value);

/* =========================================================================
 * Error reporting
 * ========================================================================= */

/**
 * @brief Get the error code stored in the output file footer.
 * @param handle  Output reader handle.
 * @returns Error code from the simulation (0 = success), or -1 on reader error.
 */
SWMM_ENGINE_API int swmm_output_get_error_code(SWMM_Output handle);

#ifdef __cplusplus
}
#endif

#endif /* OPENSWMM_OUTPUT_H */
