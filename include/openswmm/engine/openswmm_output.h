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
 * @file      openswmm_output.h
 * @brief     OpenSWMM Engine — Output File Reader C API.
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
 *          if (!out) return 1; // handle error
 *
 *          int n = swmm_output_get_node_count(out);
 *          int periods = swmm_output_get_period_count(out);
 *
 *          float* depths = malloc(n * sizeof(float));
 *          swmm_output_get_node_result(out, 5, SWMM_OUT_NODE_DEPTH, depths);
 *          // depths[j] is node j depth at period 5
 *
 *          float* ts_buf = malloc(periods * sizeof(float));
 *          swmm_output_get_node_series(out, 0, SWMM_OUT_NODE_DEPTH,
 *                                            0, periods - 1, ts_buf);
 *
 *          free(ts_buf);
 *          free(depths);
 *          swmm_output_close(out);
 *          @endcode
 *
 * @ingroup engine_api
 * @see DefaultOutputPlugin.hpp — the writer that produces these files
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
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

/**
 * @brief Read a species column stored concentration-unit code.
 *
 * Reserved __WATER_AGE__ and __TEMPERATURE__ columns reuse the concentration-unit field; identify
 * them by species name and interpret as hours and degrees C. The unit code alone cannot identify a
 * reserved species.
 *
 * @param[in] handle Non-null live output-reader handle. Serialize access on one thread because
 *     result reads share a file cursor; release with swmm_output_close().
 * @param[in] index Zero-based species-column index in [0,pollutant count).
 * @return Stored code 0=mg/L, 1=ug/L or 2=count/L, or -1 for null handle/invalid index.
 */
SWMM_ENGINE_API int swmm_output_get_pollut_units(SWMM_Output handle, int index);

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
    SWMM_OUT_SUBCATCH_RAINFALL = 0,    /**< Rainfall rate. */
    SWMM_OUT_SUBCATCH_SNOW_DEPTH = 1,  /**< Snow depth. */
    SWMM_OUT_SUBCATCH_EVAP = 2,        /**< Evaporation loss. */
    SWMM_OUT_SUBCATCH_INFIL = 3,       /**< Infiltration loss. */
    SWMM_OUT_SUBCATCH_RUNOFF = 4,      /**< Runoff flow rate. */
    SWMM_OUT_SUBCATCH_GW_FLOW = 5,     /**< Groundwater flow. */
    SWMM_OUT_SUBCATCH_GW_ELEV = 6,     /**< Groundwater table elevation. */
    SWMM_OUT_SUBCATCH_SOIL_MOIST = 7,  /**< Soil moisture content. */
    SWMM_OUT_SUBCATCH_POLLUT_BASE = 8, /**< First pollutant concentration slot. */
    /* Optional UEB fields. These are codes, not positional array indices.
       All use SI, averaged over the non-LID subcatchment area. Their accepted
       clock is recorded separately; it can differ from the hydraulic report
       clock. A file without UEB has no extension. Unavailable rows are NaN. */
    SWMM_OUT_SUBCATCH_SNOW_SWE = 65536,          /**< m, including retained liquid. */
    SWMM_OUT_SUBCATCH_SNOW_ENERGY = 65537,       /**< kJ/m2, including bare-ground energy. */
    SWMM_OUT_SUBCATCH_SNOW_SURFACE_TEMP = 65538, /**< degC, snow/ground surface. */
    SWMM_OUT_SUBCATCH_SNOW_BULK_TEMP = 65539,    /**< degC. */
    SWMM_OUT_SUBCATCH_SNOW_ALBEDO = 65540,       /**< fraction. */
    SWMM_OUT_SUBCATCH_SNOW_COVER = 65541,        /**< fraction. */
    SWMM_OUT_SUBCATCH_SNOW_MELT = 65542,         /**< m/s. */
    SWMM_OUT_SUBCATCH_SNOW_INPUT = 65543,        /**< m/s, liquid reaching surface. */
    SWMM_OUT_SUBCATCH_SNOW_SUBLIMATION = 65544,  /**< m/s, negative is condensation. */
    SWMM_OUT_SUBCATCH_SNOW_TIME = 65545          /**< accepted seconds since run start. */
} SWMM_OutSubcatchVar;

/**
 * @brief Node result variable indices.
 *
 * @details Variables stored per node per output period.
 *          Pollutant concentrations follow at indices >= SWMM_OUT_NODE_POLLUT_BASE.
 */
/**
 * @brief Check whether a subcatchment result variable CODE is present.
 *
 * Examines header codes without requiring any completed reporting period.
 *
 * @param[in] handle Non-null live output-reader handle. Serialize access on one thread because
 *     result reads share a file cursor; release with swmm_output_close().
 * @param[in] code SWMM_OutSubcatchVar code, including optional UEB snow codes; not a physical
 *     column index.
 * @return 1 when present, 0 when absent (including invalid codes), or -1 for null handle.
 */
SWMM_ENGINE_API int swmm_output_has_subcatch_variable(SWMM_Output handle, int code);

typedef enum SWMM_OutNodeVar {
    SWMM_OUT_NODE_DEPTH = 0,          /**< Water depth above invert. */
    SWMM_OUT_NODE_HEAD = 1,           /**< Hydraulic head. */
    SWMM_OUT_NODE_VOLUME = 2,         /**< Stored water volume. */
    SWMM_OUT_NODE_LATERAL_INFLOW = 3, /**< Lateral inflow rate. */
    SWMM_OUT_NODE_TOTAL_INFLOW = 4,   /**< Total inflow rate. */
    SWMM_OUT_NODE_OVERFLOW = 5,       /**< Overflow rate. */
    SWMM_OUT_NODE_POLLUT_BASE = 6     /**< First pollutant concentration slot. */
} SWMM_OutNodeVar;

/**
 * @brief Link result variable indices.
 *
 * @details Variables stored per link per output period.
 *          Pollutant concentrations follow at indices >= SWMM_OUT_LINK_POLLUT_BASE.
 */
typedef enum SWMM_OutLinkVar {
    SWMM_OUT_LINK_FLOW = 0,       /**< Flow rate. */
    SWMM_OUT_LINK_DEPTH = 1,      /**< Flow depth. */
    SWMM_OUT_LINK_VELOCITY = 2,   /**< Flow velocity. */
    SWMM_OUT_LINK_VOLUME = 3,     /**< Volume. */
    SWMM_OUT_LINK_CAPACITY = 4,   /**< Fraction of conduit capacity. */
    SWMM_OUT_LINK_POLLUT_BASE = 5 /**< First pollutant concentration slot. */
} SWMM_OutLinkVar;

/**
 * @brief System-wide result variable indices.
 *
 * @details 15 system-wide summary results stored per output period, in the
 *          slot order the .out writer uses (legacy SWMM 5 SysResults order).
 */
typedef enum SWMM_OutSystemVar {
    SWMM_OUT_SYS_TEMPERATURE = 0, /**< Air temperature. */
    SWMM_OUT_SYS_RAINFALL = 1,    /**< Average rainfall. */
    SWMM_OUT_SYS_SNOW_DEPTH = 2,  /**< Average snow depth. */
    SWMM_OUT_SYS_INFIL = 3,       /**< Average infiltration. */
    SWMM_OUT_SYS_RUNOFF = 4,      /**< Total runoff flow. */
    SWMM_OUT_SYS_DW_INFLOW = 5,   /**< Dry weather inflow. */
    SWMM_OUT_SYS_GW_INFLOW = 6,   /**< Groundwater inflow. */
    SWMM_OUT_SYS_RDII_INFLOW = 7, /**< RDII inflow. */
    SWMM_OUT_SYS_EXT_INFLOW = 8,  /**< External (direct) inflow. */
    SWMM_OUT_SYS_TOTAL_INFLOW =
        9,                      /**< Total lateral inflow (runoff + DWF + GW + RDII + external). */
    SWMM_OUT_SYS_FLOODING = 10, /**< Total flooding. */
    SWMM_OUT_SYS_OUTFLOW = 11,  /**< Total outfall outflow. */
    SWMM_OUT_SYS_STORAGE = 12,  /**< Total storage volume. */
    SWMM_OUT_SYS_EVAP = 13,     /**< Evaporation rate. */
    SWMM_OUT_SYS_PET = 14,      /**< Potential evapotranspiration. */
    SWMM_OUT_SYS_LAT_INFLOW = SWMM_OUT_SYS_TOTAL_INFLOW /**< Deprecated alias of TOTAL_INFLOW. */
} SWMM_OutSystemVar;

/* =========================================================================
 * Lifecycle
 * ========================================================================= */

/**
 * @brief Allocate a reader for a binary output with a valid closing footer.
 *
 * Opening does not imply the simulation footer error is zero; inspect swmm_output_get_error_code().
 * Object/species order and display units come from this output file, independently of any live
 * model.
 *
 * @param[in] path Non-null NUL-terminated UTF-8 output path, borrowed for this call. File is opened
 *     read-only.
 * @return Caller-owned independent output handle, or NULL for allocation/open/header/footer
 *     failure. Close every successful handle exactly once.
 */
SWMM_ENGINE_API SWMM_Output swmm_output_open(const char* path);

/**
 * @brief Allocate a reader for the complete records of an output still being written.
 *
 * Forward-parses offsets and counts only whole report records; no closing footer is needed. Can
 * inspect running or interrupted output without modifying it. Refresh later for growth; a valid
 * final footer is adopted when it arrives.
 *
 * @param[in] path Non-null NUL-terminated UTF-8 output path with a complete parseable header;
 *     opened read-only.
 * @return Caller-owned output handle, or NULL for allocation/open/header failure. Release with
 *     swmm_output_close().
 */
SWMM_ENGINE_API SWMM_Output swmm_output_open_live(const char* path);

/**
 * @brief Update whole-period count and adopt a final footer on a live reader.
 *
 * A non-live handle keeps its footer count. Live refresh sees new complete records and becomes
 * non-live after a matching final footer appears; seek/size-query failure can return the previous
 * count, so success does not guarantee growth. Does not retain partial report records.
 *
 * @param[in] handle Non-null live output-reader handle. Serialize access on one thread because
 *     result reads share a file cursor; release with swmm_output_close().
 * @param[out] periods Optional destination for the current period count, written after a
 *     nonnegative refresh result.
 * @return 0 on a nonnegative refresh result, -1 for null/unopened reader.
 */
SWMM_ENGINE_API int swmm_output_refresh(SWMM_Output handle, int* periods);

/**
 * @brief Read whether the output reader is still using live record counting.
 *
 * @param[in] handle Non-null live output-reader handle. Serialize access on one thread because
 *     result reads share a file cursor; release with swmm_output_close().
 * @return 1 while live, 0 after adopting a final footer or opening normally, -1 for null handle.
 */
SWMM_ENGINE_API int swmm_output_is_live(SWMM_Output handle);

/**
 * @brief Close the file and delete its independent output-reader handle.
 *
 * All borrowed ID strings become invalid. Does not close or alter a separate simulation-engine
 * handle.
 *
 * @param[in] handle Caller-owned live reader, or NULL for a no-op. Do not use afterward, close
 *     twice or close concurrently with another read.
 */
SWMM_ENGINE_API void swmm_output_close(SWMM_Output handle);

/* =========================================================================
 * Metadata queries
 * ========================================================================= */

/**
 * @brief Read the output writer version.
 *
 * @param[in] handle Non-null live output-reader handle. Serialize access on one thread because
 *     result reads share a file cursor; release with swmm_output_close().
 * @return Integer writer version (for example 52001); -1 for null handle.
 */
SWMM_ENGINE_API int swmm_output_get_version(SWMM_Output handle);

/**
 * @brief Read the output file flow-unit selector.
 *
 * @param[in] handle Non-null live output-reader handle. Serialize access on one thread because
 *     result reads share a file cursor; release with swmm_output_close().
 * @return 0=CFS, 1=GPM, 2=MGD, 3=CMS, 4=LPS or 5=MLD; -1 for null handle.
 */
SWMM_ENGINE_API int swmm_output_get_flow_units(SWMM_Output handle);

/**
 * @brief Read the output subcatchment count.
 *
 * @param[in] handle Non-null live output-reader handle. Serialize access on one thread because
 *     result reads share a file cursor; release with swmm_output_close().
 * @return Nonnegative number of subcatchment result rows; -1 for null handle.
 */
SWMM_ENGINE_API int swmm_output_get_subcatch_count(SWMM_Output handle);

/**
 * @brief Read the output node count.
 *
 * @param[in] handle Non-null live output-reader handle. Serialize access on one thread because
 *     result reads share a file cursor; release with swmm_output_close().
 * @return Nonnegative number of node result rows; -1 for null handle.
 */
SWMM_ENGINE_API int swmm_output_get_node_count(SWMM_Output handle);

/**
 * @brief Read the output link count.
 *
 * @param[in] handle Non-null live output-reader handle. Serialize access on one thread because
 *     result reads share a file cursor; release with swmm_output_close().
 * @return Nonnegative number of link result rows; -1 for null handle.
 */
SWMM_ENGINE_API int swmm_output_get_link_count(SWMM_Output handle);

/**
 * @brief Read the output species-column count.
 *
 * @param[in] handle Non-null live output-reader handle. Serialize access on one thread because
 *     result reads share a file cursor; release with swmm_output_close().
 * @return Nonnegative count including any reserved water-age/temperature species; -1 for null
 *     handle.
 */
SWMM_ENGINE_API int swmm_output_get_pollut_count(SWMM_Output handle);

/**
 * @brief Read the output currently known whole-report count.
 *
 * A live period count does not advance automatically; call swmm_output_refresh() to discover
 * growth.
 *
 * @param[in] handle Non-null live output-reader handle. Serialize access on one thread because
 *     result reads share a file cursor; release with swmm_output_close().
 * @return Nonnegative current complete-period count; -1 for null handle.
 */
SWMM_ENGINE_API int swmm_output_get_period_count(SWMM_Output handle);

/**
 * @brief Read the calendar baseline stored in output report metadata.
 *
 * Use actual period times for sample timestamps rather than assuming a particular
 * origin/report-start alignment.
 *
 * @param[in] handle Non-null live output-reader handle. Serialize access on one thread because
 *     result reads share a file cursor; release with swmm_output_close().
 * @param[out] start_date Non-null destination for SWMM DateTime/OADate decimal days since
 *     1899-12-30; no timezone conversion.
 * @return 0 on success, -1 for null handle/start_date.
 */
SWMM_ENGINE_API int swmm_output_get_start_date(SWMM_Output handle, double* start_date);

/**
 * @brief Read the output configured reporting interval.
 *
 * @param[in] handle Non-null live output-reader handle. Serialize access on one thread because
 *     result reads share a file cursor; release with swmm_output_close().
 * @return Report interval in seconds; -1 for null handle.
 */
SWMM_ENGINE_API int swmm_output_get_report_step(SWMM_Output handle);

/* =========================================================================
 * Object ID retrieval
 * ========================================================================= */

/**
 * @brief Borrow a subcatchment identifier from the output header.
 *
 * @param[in] handle Non-null live output-reader handle. Serialize access on one thread because
 *     result reads share a file cursor; release with swmm_output_close().
 * @param[in] index Zero-based subcatchment index in this file, independent of a model engine row
 *     order.
 * @return Reader-owned NUL-terminated ID, or NULL for null handle/out-of-range index. Never free
 *     it; storage remains valid until the reader is closed.
 */
SWMM_ENGINE_API const char* swmm_output_get_subcatch_id(SWMM_Output handle, int index);

/**
 * @brief Borrow a node identifier from the output header.
 *
 * @param[in] handle Non-null live output-reader handle. Serialize access on one thread because
 *     result reads share a file cursor; release with swmm_output_close().
 * @param[in] index Zero-based node index in this file, independent of a model engine row order.
 * @return Reader-owned NUL-terminated ID, or NULL for null handle/out-of-range index. Never free
 *     it; storage remains valid until the reader is closed.
 */
SWMM_ENGINE_API const char* swmm_output_get_node_id(SWMM_Output handle, int index);

/**
 * @brief Borrow a link identifier from the output header.
 *
 * @param[in] handle Non-null live output-reader handle. Serialize access on one thread because
 *     result reads share a file cursor; release with swmm_output_close().
 * @param[in] index Zero-based link index in this file, independent of a model engine row order.
 * @return Reader-owned NUL-terminated ID, or NULL for null handle/out-of-range index. Never free
 *     it; storage remains valid until the reader is closed.
 */
SWMM_ENGINE_API const char* swmm_output_get_link_id(SWMM_Output handle, int index);

/**
 * @brief Borrow a species identifier from the output header.
 *
 * Species IDs __WATER_AGE__ and __TEMPERATURE__ denote hours and degrees C regardless of their
 * stored concentration code.
 *
 * @param[in] handle Non-null live output-reader handle. Serialize access on one thread because
 *     result reads share a file cursor; release with swmm_output_close().
 * @param[in] index Zero-based species index in this file, independent of a model engine row order.
 * @return Reader-owned NUL-terminated ID, or NULL for null handle/out-of-range index. Never free
 *     it; storage remains valid until the reader is closed.
 */
SWMM_ENGINE_API const char* swmm_output_get_pollut_id(SWMM_Output handle, int index);

/* =========================================================================
 * Per-period result retrieval (all objects, one variable, one period)
 * ========================================================================= */

/**
 * @brief Read one subcatchment variable for all subcatchment rows at one period.
 *
 * Values retain the file display-unit convention; optional UEB snow variables use their documented
 * SI units. Reserved species use hours/degrees C. This API has no buffer-capacity argument, so
 * allocation size is the caller responsibility.
 *
 * @param[in] handle Non-null live output-reader handle. Serialize access on one thread because
 *     result reads share a file cursor; release with swmm_output_close().
 * @param[in] period Zero-based index in [0,swmm_output_get_period_count(handle)). Live readers
 *     require refresh to discover new whole records.
 * @param[in] var SWMM_OutSubcatchVar variable CODE, including optional snow extension codes; not a
 *     physical column index.
 * @param[out] values Non-null caller-owned array of at least this file's subcatchment count floats,
 *     in file object order.
 * @return 0 on success, -1 for null handle/output, invalid indices/variable/range, or a seek/read
 *     failure. A partial output prefix may have been written before an IO failure.
 */
SWMM_ENGINE_API int swmm_output_get_subcatch_result(SWMM_Output handle, int period, int var,
                                                    float* values);

/**
 * @brief Read one node variable for all node rows at one period.
 *
 * Values retain the file display-unit convention; optional UEB snow variables use their documented
 * SI units. Reserved species use hours/degrees C. This API has no buffer-capacity argument, so
 * allocation size is the caller responsibility.
 *
 * @param[in] handle Non-null live output-reader handle. Serialize access on one thread because
 *     result reads share a file cursor; release with swmm_output_close().
 * @param[in] period Zero-based index in [0,swmm_output_get_period_count(handle)). Live readers
 *     require refresh to discover new whole records.
 * @param[in] var SWMM_OutNodeVar variable index, including supported species slots for node/link
 *     results.
 * @param[out] values Non-null caller-owned array of at least this file's node count floats, in file
 *     object order.
 * @return 0 on success, -1 for null handle/output, invalid indices/variable/range, or a seek/read
 *     failure. A partial output prefix may have been written before an IO failure.
 */
SWMM_ENGINE_API int swmm_output_get_node_result(SWMM_Output handle, int period, int var,
                                                float* values);

/**
 * @brief Read one link variable for all link rows at one period.
 *
 * Values retain the file display-unit convention; optional UEB snow variables use their documented
 * SI units. Reserved species use hours/degrees C. This API has no buffer-capacity argument, so
 * allocation size is the caller responsibility.
 *
 * @param[in] handle Non-null live output-reader handle. Serialize access on one thread because
 *     result reads share a file cursor; release with swmm_output_close().
 * @param[in] period Zero-based index in [0,swmm_output_get_period_count(handle)). Live readers
 *     require refresh to discover new whole records.
 * @param[in] var SWMM_OutLinkVar variable index, including supported species slots for node/link
 *     results.
 * @param[out] values Non-null caller-owned array of at least this file's link count floats, in file
 *     object order.
 * @return 0 on success, -1 for null handle/output, invalid indices/variable/range, or a seek/read
 *     failure. A partial output prefix may have been written before an IO failure.
 */
SWMM_ENGINE_API int swmm_output_get_link_result(SWMM_Output handle, int period, int var,
                                                float* values);

/**
 * @brief Read one system variable for the system at one period.
 *
 * Values retain the file display-unit convention; optional UEB snow variables use their documented
 * SI units. Reserved species use hours/degrees C. This API has no buffer-capacity argument, so
 * allocation size is the caller responsibility.
 *
 * @param[in] handle Non-null live output-reader handle. Serialize access on one thread because
 *     result reads share a file cursor; release with swmm_output_close().
 * @param[in] period Zero-based index in [0,swmm_output_get_period_count(handle)). Live readers
 *     require refresh to discover new whole records.
 * @param[in] var SWMM_OutSystemVar variable index, including supported species slots for node/link
 *     results.
 * @param[out] value Non-null caller-owned single float destination.
 * @return 0 on success, -1 for null handle/output, invalid indices/variable/range, or a seek/read
 *     failure. A partial output prefix may have been written before an IO failure.
 */
SWMM_ENGINE_API int swmm_output_get_system_result(SWMM_Output handle, int period, int var,
                                                  float* value);

/* =========================================================================
 * Time series retrieval (one object, one variable, period range)
 * ========================================================================= */

/**
 * @brief Read a closed range of subcatchment report samples.
 *
 * The range endpoints are both inclusive. Returns stored reporting samples without temporal
 * interpolation or automatic live refresh.
 *
 * @param[in] handle Non-null live output-reader handle. Serialize access on one thread because
 *     result reads share a file cursor; release with swmm_output_close().
 * @param[in] subcatch_idx Zero-based subcatchment index in this output file.
 * @param[in] var SWMM_OutSubcatchVar variable CODE, including optional snow extension codes; not a
 *     physical column index.
 * @param[in] start_period Inclusive first period, >=0 and <=end_period.
 * @param[in] end_period Inclusive last period, less than the currently known period count.
 * @param[out] values Non-null caller-owned array of end_period-start_period+1 floats, ordered
 *     chronologically in file display/extension units.
 * @return 0 on success, -1 for null handle/output, invalid indices/variable/range, or a seek/read
 *     failure. A partial output prefix may have been written before an IO failure.
 */
SWMM_ENGINE_API int swmm_output_get_subcatch_series(SWMM_Output handle, int subcatch_idx, int var,
                                                    int start_period, int end_period,
                                                    float* values);

/**
 * @brief Read a closed range of node report samples.
 *
 * The range endpoints are both inclusive. Returns stored reporting samples without temporal
 * interpolation or automatic live refresh.
 *
 * @param[in] handle Non-null live output-reader handle. Serialize access on one thread because
 *     result reads share a file cursor; release with swmm_output_close().
 * @param[in] node_idx Zero-based node index in this output file.
 * @param[in] var SWMM_OutNodeVar variable index, including supported species slots for node/link
 *     results.
 * @param[in] start_period Inclusive first period, >=0 and <=end_period.
 * @param[in] end_period Inclusive last period, less than the currently known period count.
 * @param[out] values Non-null caller-owned array of end_period-start_period+1 floats, ordered
 *     chronologically in file display/extension units.
 * @return 0 on success, -1 for null handle/output, invalid indices/variable/range, or a seek/read
 *     failure. A partial output prefix may have been written before an IO failure.
 */
SWMM_ENGINE_API int swmm_output_get_node_series(SWMM_Output handle, int node_idx, int var,
                                                int start_period, int end_period, float* values);

/**
 * @brief Read a closed range of link report samples.
 *
 * The range endpoints are both inclusive. Returns stored reporting samples without temporal
 * interpolation or automatic live refresh.
 *
 * @param[in] handle Non-null live output-reader handle. Serialize access on one thread because
 *     result reads share a file cursor; release with swmm_output_close().
 * @param[in] link_idx Zero-based link index in this output file.
 * @param[in] var SWMM_OutLinkVar variable index, including supported species slots for node/link
 *     results.
 * @param[in] start_period Inclusive first period, >=0 and <=end_period.
 * @param[in] end_period Inclusive last period, less than the currently known period count.
 * @param[out] values Non-null caller-owned array of end_period-start_period+1 floats, ordered
 *     chronologically in file display/extension units.
 * @return 0 on success, -1 for null handle/output, invalid indices/variable/range, or a seek/read
 *     failure. A partial output prefix may have been written before an IO failure.
 */
SWMM_ENGINE_API int swmm_output_get_link_series(SWMM_Output handle, int link_idx, int var,
                                                int start_period, int end_period, float* values);

/**
 * @brief Read a closed range of system report samples.
 *
 * The range endpoints are both inclusive. Returns stored reporting samples without temporal
 * interpolation or automatic live refresh.
 *
 * @param[in] handle Non-null live output-reader handle. Serialize access on one thread because
 *     result reads share a file cursor; release with swmm_output_close().
 * @param[in] var SWMM_OutSystemVar variable index, including supported species slots for node/link
 *     results.
 * @param[in] start_period Inclusive first period, >=0 and <=end_period.
 * @param[in] end_period Inclusive last period, less than the currently known period count.
 * @param[out] values Non-null caller-owned array of end_period-start_period+1 floats, ordered
 *     chronologically in file display/extension units.
 * @return 0 on success, -1 for null handle/output, invalid indices/variable/range, or a seek/read
 *     failure. A partial output prefix may have been written before an IO failure.
 */
SWMM_ENGINE_API int swmm_output_get_system_series(SWMM_Output handle, int var, int start_period,
                                                  int end_period, float* values);

/* =========================================================================
 * Per-object result retrieval (all variables for one object at one period)
 * ========================================================================= */

/**
 * @brief Read subcatchment variables at one report record.
 *
 * Values are in stored column order and each field retains its file unit. Provide enough space
 * before calling; optional snow extensions are accessed by code through the subcatchment
 * result/series APIs.
 *
 * @param[in] handle Non-null live output-reader handle. Serialize access on one thread because
 *     result reads share a file cursor; release with swmm_output_close().
 * @param[in] subcatch_idx Zero-based subcatchment index in this output file.
 * @param[in] period Zero-based index in [0,swmm_output_get_period_count(handle)). Live readers
 *     require refresh to discover new whole records.
 * @param[out] values Non-null caller-owned destination sized for 8+pollutant count legacy floats
 *     (optional UEB process fields are excluded).
 * @param[out] count Optional number of variables written, set only after the entire record read
 *     succeeds; this is not an input capacity or size-query mode.
 * @return 0 on success, -1 for null handle/output, invalid indices/variable/range, or a seek/read
 *     failure. A partial output prefix may have been written before an IO failure.
 */
SWMM_ENGINE_API int swmm_output_get_subcatch_attribute(SWMM_Output handle, int subcatch_idx,
                                                       int period, float* values, int* count);

/**
 * @brief Read node variables at one report record.
 *
 * Values are in stored column order and each field retains its file unit. Provide enough space
 * before calling; optional snow extensions are accessed by code through the subcatchment
 * result/series APIs.
 *
 * @param[in] handle Non-null live output-reader handle. Serialize access on one thread because
 *     result reads share a file cursor; release with swmm_output_close().
 * @param[in] node_idx Zero-based node index in this output file.
 * @param[in] period Zero-based index in [0,swmm_output_get_period_count(handle)). Live readers
 *     require refresh to discover new whole records.
 * @param[out] values Non-null caller-owned destination sized for the full node-variable count
 *     floats (normally 6+species count).
 * @param[out] count Optional number of variables written, set only after the entire record read
 *     succeeds; this is not an input capacity or size-query mode.
 * @return 0 on success, -1 for null handle/output, invalid indices/variable/range, or a seek/read
 *     failure. A partial output prefix may have been written before an IO failure.
 */
SWMM_ENGINE_API int swmm_output_get_node_attribute(SWMM_Output handle, int node_idx, int period,
                                                   float* values, int* count);

/**
 * @brief Read link variables at one report record.
 *
 * Values are in stored column order and each field retains its file unit. Provide enough space
 * before calling; optional snow extensions are accessed by code through the subcatchment
 * result/series APIs.
 *
 * @param[in] handle Non-null live output-reader handle. Serialize access on one thread because
 *     result reads share a file cursor; release with swmm_output_close().
 * @param[in] link_idx Zero-based link index in this output file.
 * @param[in] period Zero-based index in [0,swmm_output_get_period_count(handle)). Live readers
 *     require refresh to discover new whole records.
 * @param[out] values Non-null caller-owned destination sized for the full link-variable count
 *     floats (normally 5+species count).
 * @param[out] count Optional number of variables written, set only after the entire record read
 *     succeeds; this is not an input capacity or size-query mode.
 * @return 0 on success, -1 for null handle/output, invalid indices/variable/range, or a seek/read
 *     failure. A partial output prefix may have been written before an IO failure.
 */
SWMM_ENGINE_API int swmm_output_get_link_attribute(SWMM_Output handle, int link_idx, int period,
                                                   float* values, int* count);

/* =========================================================================
 * Simulation time retrieval
 * ========================================================================= */

/**
 * @brief Read the exact calendar timestamp of one report record.
 *
 * @param[in] handle Non-null live output-reader handle. Serialize access on one thread because
 *     result reads share a file cursor; release with swmm_output_close().
 * @param[in] period Zero-based index in [0,swmm_output_get_period_count(handle)). Live readers
 *     require refresh to discover new whole records.
 * @param[out] time Non-null destination for SWMM DateTime decimal days since 1899-12-30; no
 *     timezone conversion.
 * @return 0 on success, -1 for null handle/output, invalid indices/variable/range, or a seek/read
 *     failure. A partial output prefix may have been written before an IO failure.
 */
SWMM_ENGINE_API int swmm_output_get_period_time(SWMM_Output handle, int period, double* time);

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
 * 5 s), `time_flooded` and `vol_flooded` can miss brief events or assign
 * an entire report interval to a brief positive sample. The engine-side getters
 * (`swmm_node_get_stat_*`) retain the legacy routing-step precision —
 * use those when a fresh in-process run is the source.
 * ========================================================================= */

/**
 * @brief Reconstruct node maximum reported depth from report records.
 *
 * Maximum over the currently known report samples. Scans O(period count) samples per call and may
 * miss short events; use engine routing-step statistics when that precision is required.
 *
 * @param[in] handle Non-null live output-reader handle. Serialize access on one thread because
 *     result reads share a file cursor; release with swmm_output_close().
 * @param[in] node_idx Zero-based node index in this output file.
 * @param[out] value Non-null destination in file display length (ft US, m SI); zero when there are
 *     no report periods, unchanged on validation/read failure.
 * @return 0 on success or -1 for null arguments, invalid node index or read failure.
 */
SWMM_ENGINE_API int swmm_output_get_node_stat_max_depth(SWMM_Output handle, int node_idx,
                                                        double* value);

/**
 * @brief Reconstruct node maximum reported overflow from report records.
 *
 * Maximum over the currently known report samples. Scans O(period count) samples per call and may
 * miss short events; use engine routing-step statistics when that precision is required.
 *
 * @param[in] handle Non-null live output-reader handle. Serialize access on one thread because
 *     result reads share a file cursor; release with swmm_output_close().
 * @param[in] node_idx Zero-based node index in this output file.
 * @param[out] value Non-null destination in file flow units selected by
 *     swmm_output_get_flow_units(); zero when there are no report periods, unchanged on
 *     validation/read failure.
 * @return 0 on success or -1 for null arguments, invalid node index or read failure.
 */
SWMM_ENGINE_API int swmm_output_get_node_stat_max_overflow(SWMM_Output handle, int node_idx,
                                                           double* value);

/**
 * @brief Reconstruct node report-sampled flooding integral from report records.
 *
 * Sums positive stored overflow*report_step with no flow-to-volume unit conversion. CFS gives ft3
 * and CMS gives m3; GPM/MGD/LPS/MLD outputs need caller conversion before interpreting this as a
 * physical volume. Scans O(period count) samples per call and may miss short events; use engine
 * routing-step statistics when that precision is required.
 *
 * @param[in] handle Non-null live output-reader handle. Serialize access on one thread because
 *     result reads share a file cursor; release with swmm_output_close().
 * @param[in] node_idx Zero-based node index in this output file.
 * @param[out] value Non-null destination in numeric file flow unit multiplied by seconds; zero when
 *     there are no report periods, unchanged on validation/read failure.
 * @return 0 on success or -1 for null arguments, invalid node index or read failure.
 */
SWMM_ENGINE_API int swmm_output_get_node_stat_vol_flooded(SWMM_Output handle, int node_idx,
                                                          double* value);

/**
 * @brief Reconstruct node report-sampled flooding duration from report records.
 *
 * Counts positive-overflow report records and multiplies by report-step seconds. Counts full report
 * intervals; it does not reconstruct sub-report-step onset/duration. Scans O(period count) samples
 * per call and may miss short events; use engine routing-step statistics when that precision is
 * required.
 *
 * @param[in] handle Non-null live output-reader handle. Serialize access on one thread because
 *     result reads share a file cursor; release with swmm_output_close().
 * @param[in] node_idx Zero-based node index in this output file.
 * @param[out] value Non-null destination in seconds; zero when there are no report periods,
 *     unchanged on validation/read failure.
 * @return 0 on success or -1 for null arguments, invalid node index or read failure.
 */
SWMM_ENGINE_API int swmm_output_get_node_stat_time_flooded(SWMM_Output handle, int node_idx,
                                                           double* value);

/* =========================================================================
 * Error reporting
 * ========================================================================= */

/**
 * @brief Read the output simulation footer error.
 *
 * @param[in] handle Non-null live output-reader handle. Serialize access on one thread because
 *     result reads share a file cursor; release with swmm_output_close().
 * @return Recorded simulation error (0 for success; live readers use 0 until a footer is adopted);
 *     -1 for null handle.
 */
SWMM_ENGINE_API int swmm_output_get_error_code(SWMM_Output handle);

#ifdef __cplusplus
}
#endif

#endif /* OPENSWMM_OUTPUT_H */
