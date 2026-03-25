/**
 * @file openswmm_inflows.h
 * @brief OpenSWMM Engine — Inflows (External, DWF, RDII) C API.
 *
 * @details External inflow addition, dry weather flow, RDII assignment,
 *          and count queries.
 *
 * @ingroup engine_api
 * @see openswmm_engine.h
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_INFLOWS_H
#define OPENSWMM_INFLOWS_H

#include "openswmm_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * External inflows
 * ========================================================================= */

/**
 * @brief Add an external inflow to a node.
 *
 * @details External inflows define time-varying flows or pollutant loads
 *          applied at a node, optionally driven by a time series, with
 *          scaling, baseline, and pattern modifiers.
 *
 * @param engine     Engine handle.
 * @param node_idx   Zero-based index of the receiving node.
 * @param constituent Constituent name ("FLOW" for flow, or a pollutant name).
 * @param ts_name    Time series name (NULL or "" for constant baseline only).
 * @param type       Inflow type: "FLOW", "CONCEN", or "MASS".
 * @param m_factor   Multiplier applied to the time series values.
 * @param s_factor   Scale factor (unit conversion).
 * @param baseline   Constant baseline value added to the time series.
 * @param pattern    Time pattern name (NULL or "" for none).
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_ext_inflow_add(SWMM_Engine engine, int node_idx, const char* constituent,
                                          const char* ts_name, const char* type,
                                          double m_factor, double s_factor, double baseline,
                                          const char* pattern);

/* =========================================================================
 * Dry weather flow
 * ========================================================================= */

/**
 * @brief Add a dry weather flow component to a node.
 *
 * @details Dry weather flow represents the base sanitary flow entering the
 *          system at a node, modulated by up to four time patterns.
 *
 * @param engine      Engine handle.
 * @param node_idx    Zero-based index of the receiving node.
 * @param constituent Constituent name ("FLOW" or a pollutant name).
 * @param avg_value   Average DWF value.
 * @param pat1        Monthly time pattern name (NULL for none).
 * @param pat2        Daily time pattern name (NULL for none).
 * @param pat3        Hourly time pattern name (NULL for none).
 * @param pat4        Weekend time pattern name (NULL for none).
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_dwf_add(SWMM_Engine engine, int node_idx, const char* constituent,
                                   double avg_value, const char* pat1, const char* pat2,
                                   const char* pat3, const char* pat4);

/* =========================================================================
 * RDII (Rainfall-Dependent Infiltration/Inflow)
 * ========================================================================= */

/**
 * @brief Add RDII inflow to a node using a unit hydrograph.
 *
 * @details Associates a node with a unit hydrograph group and its sewershed
 *          area to compute rainfall-dependent infiltration/inflow.
 *
 * @param engine    Engine handle.
 * @param node_idx  Zero-based index of the receiving node.
 * @param uh_name   Unit hydrograph group name.
 * @param area      Sewershed area in project area units.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_rdii_add(SWMM_Engine engine, int node_idx, const char* uh_name, double area);

/* =========================================================================
 * Count queries
 * ========================================================================= */

/**
 * @brief Get the total number of external inflows defined.
 * @param engine  Engine handle.
 * @returns Number of external inflows, or -1 on error.
 */
SWMM_ENGINE_API int swmm_ext_inflow_count(SWMM_Engine engine);

/**
 * @brief Get the total number of dry weather flow entries defined.
 * @param engine  Engine handle.
 * @returns Number of DWF entries, or -1 on error.
 */
SWMM_ENGINE_API int swmm_dwf_count(SWMM_Engine engine);

/**
 * @brief Get the total number of RDII entries defined.
 * @param engine  Engine handle.
 * @returns Number of RDII entries, or -1 on error.
 */
SWMM_ENGINE_API int swmm_rdii_count(SWMM_Engine engine);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* OPENSWMM_INFLOWS_H */
