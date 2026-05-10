/**
 * @file openswmm_subcatchments.h
 * @brief OpenSWMM Engine — Subcatchment C API.
 *
 * @details Subcatchment add (BUILDING state), property setters, state get,
 *          rainfall forcing, bulk access, quality.
 *
 * @ingroup engine_api
 * @see openswmm_engine.h
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_SUBCATCHMENTS_H
#define OPENSWMM_SUBCATCHMENTS_H

#include "openswmm_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Identity
 * ========================================================================= */

/**
 * @brief Get the total number of subcatchments in the model.
 * @param engine  Engine handle.
 * @returns Number of subcatchments, or -1 on error.
 */
SWMM_ENGINE_API int swmm_subcatch_count(SWMM_Engine engine);

/**
 * @brief Look up a subcatchment's zero-based index by its string identifier.
 * @param engine  Engine handle.
 * @param id      Null-terminated subcatchment identifier.
 * @returns Zero-based index, or -1 if not found.
 */
SWMM_ENGINE_API int swmm_subcatch_index(SWMM_Engine engine, const char* id);

/**
 * @brief Get the string identifier of a subcatchment by index.
 * @param engine  Engine handle.
 * @param idx     Zero-based subcatchment index.
 * @returns Null-terminated string owned by the engine, or NULL on error.
 */
SWMM_ENGINE_API const char* swmm_subcatch_id(SWMM_Engine engine, int idx);

/* =========================================================================
 * Creation (BUILDING or OPENED — "editable" states)
 * ========================================================================= */

/**
 * @brief Add a new subcatchment to the model.
 *
 * @details The engine must be in SWMM_STATE_BUILDING or SWMM_STATE_OPENED.
 *          Returns SWMM_ERR_LIFECYCLE for any other state. After creation,
 *          use the property setters to configure area, slope, imperviousness,
 *          etc.
 *
 * @param engine  Engine handle.
 * @param id      Unique null-terminated identifier for the new subcatchment.
 * @returns SWMM_OK on success, SWMM_ERR_LIFECYCLE if not in an editable
 *          state, or another error code.
 */
SWMM_ENGINE_API int swmm_subcatch_add(SWMM_Engine engine, const char* id);

/* =========================================================================
 * Property setters (BUILDING or OPENED)
 * ========================================================================= */

/**
 * @brief Set the outlet node that receives runoff from this subcatchment.
 * @param engine    Engine handle.
 * @param idx       Zero-based subcatchment index.
 * @param node_idx  Zero-based node index of the receiving node.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_subcatch_set_outlet(SWMM_Engine engine, int idx, int node_idx);

/**
 * @brief Set the subcatchment area.
 * @param engine  Engine handle.
 * @param idx     Zero-based subcatchment index.
 * @param area    Area in project area units (acres or hectares).
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_subcatch_set_area(SWMM_Engine engine, int idx, double area);

/**
 * @brief Set the characteristic overland flow width.
 * @param engine  Engine handle.
 * @param idx     Zero-based subcatchment index.
 * @param width   Width in project length units.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_subcatch_set_width(SWMM_Engine engine, int idx, double width);

/**
 * @brief Set the average surface slope.
 * @param engine  Engine handle.
 * @param idx     Zero-based subcatchment index.
 * @param slope   Slope as a percentage (e.g., 2.0 = 2%).
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_subcatch_set_slope(SWMM_Engine engine, int idx, double slope);

/**
 * @brief Set the percentage of impervious area.
 * @param engine  Engine handle.
 * @param idx     Zero-based subcatchment index.
 * @param pct     Imperviousness as a percentage (0–100).
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_subcatch_set_imperv_pct(SWMM_Engine engine, int idx, double pct);

/**
 * @brief Set Manning's n for the impervious area.
 * @param engine  Engine handle.
 * @param idx     Zero-based subcatchment index.
 * @param n       Manning's roughness coefficient.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_subcatch_set_n_imperv(SWMM_Engine engine, int idx, double n);

/**
 * @brief Set Manning's n for the pervious area.
 * @param engine  Engine handle.
 * @param idx     Zero-based subcatchment index.
 * @param n       Manning's roughness coefficient.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_subcatch_set_n_perv(SWMM_Engine engine, int idx, double n);

/**
 * @brief Set the depression storage depth for the impervious area.
 * @param engine  Engine handle.
 * @param idx     Zero-based subcatchment index.
 * @param ds      Depression storage in project length units.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_subcatch_set_ds_imperv(SWMM_Engine engine, int idx, double ds);

/**
 * @brief Set the depression storage depth for the pervious area.
 * @param engine  Engine handle.
 * @param idx     Zero-based subcatchment index.
 * @param ds      Depression storage in project length units.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_subcatch_set_ds_perv(SWMM_Engine engine, int idx, double ds);

/**
 * @brief Assign a rain gage to a subcatchment.
 * @param engine    Engine handle.
 * @param idx       Zero-based subcatchment index.
 * @param gage_idx  Zero-based rain gage index.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_subcatch_set_gage(SWMM_Engine engine, int idx, int gage_idx);

/* --- Infiltration parameters (BUILDING or OPENED) --- */

/** @brief Set Horton infiltration parameters. */
SWMM_ENGINE_API int swmm_subcatch_set_infil_horton(SWMM_Engine engine, int idx,
                                                     double f0, double fmin,
                                                     double decay, double dry_time);

/** @brief Set Green-Ampt infiltration parameters. */
SWMM_ENGINE_API int swmm_subcatch_set_infil_green_ampt(SWMM_Engine engine, int idx,
                                                         double suction, double conductivity,
                                                         double initial_deficit);

/** @brief Set Curve Number infiltration parameter. */
SWMM_ENGINE_API int swmm_subcatch_set_infil_curve_number(SWMM_Engine engine, int idx,
                                                           double cn);

/* =========================================================================
 * Property getters
 * ========================================================================= */

/**
 * @brief Get the subcatchment area.
 * @param engine     Engine handle.
 * @param idx        Zero-based subcatchment index.
 * @param[out] area  Receives the area in project area units.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_subcatch_get_area(SWMM_Engine engine, int idx, double* area);

/**
 * @brief Get the percentage of impervious area.
 * @param engine    Engine handle.
 * @param idx       Zero-based subcatchment index.
 * @param[out] pct  Receives the imperviousness percentage (0–100).
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_subcatch_get_imperv_pct(SWMM_Engine engine, int idx, double* pct);

/**
 * @brief Get the outlet node index for a subcatchment.
 * @param engine         Engine handle.
 * @param idx            Zero-based subcatchment index.
 * @param[out] node_idx  Receives the outlet node index.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_subcatch_get_outlet(SWMM_Engine engine, int idx, int* node_idx);

/**
 * @brief Get the characteristic overland flow width.
 * @param engine  Engine handle.
 * @param idx     Zero-based subcatchment index.
 * @param[out] w  Receives the width in project length units.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_subcatch_get_width(SWMM_Engine engine, int idx, double* w);

/**
 * @brief Get the average surface slope.
 * @param engine  Engine handle.
 * @param idx     Zero-based subcatchment index.
 * @param[out] s  Receives the slope percentage.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_subcatch_get_slope(SWMM_Engine engine, int idx, double* s);

/**
 * @brief Get Manning's n for the impervious area.
 * @param engine  Engine handle.
 * @param idx     Zero-based subcatchment index.
 * @param[out] n  Receives Manning's n.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_subcatch_get_n_imperv(SWMM_Engine engine, int idx, double* n);

/**
 * @brief Get Manning's n for the pervious area.
 * @param engine  Engine handle.
 * @param idx     Zero-based subcatchment index.
 * @param[out] n  Receives Manning's n.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_subcatch_get_n_perv(SWMM_Engine engine, int idx, double* n);

/**
 * @brief Get the depression storage depth for the impervious area.
 * @param engine   Engine handle.
 * @param idx      Zero-based subcatchment index.
 * @param[out] ds  Receives the depression storage depth.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_subcatch_get_ds_imperv(SWMM_Engine engine, int idx, double* ds);

/**
 * @brief Get the depression storage depth for the pervious area.
 * @param engine   Engine handle.
 * @param idx      Zero-based subcatchment index.
 * @param[out] ds  Receives the depression storage depth.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_subcatch_get_ds_perv(SWMM_Engine engine, int idx, double* ds);

/**
 * @brief Get the rain gage index assigned to a subcatchment.
 * @param engine         Engine handle.
 * @param idx            Zero-based subcatchment index.
 * @param[out] gage_idx  Receives the gage index.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_subcatch_get_gage(SWMM_Engine engine, int idx, int* gage_idx);

/**
 * @brief Set a subcatchment's outlet to another subcatchment (cascading).
 *
 * @details When a subcatchment drains to another subcatchment instead of
 *          directly to a node, use this function. Mutually exclusive with
 *          swmm_subcatch_set_outlet().
 *
 * @param engine  Engine handle.
 * @param idx     Zero-based subcatchment index.
 * @param sc_idx  Zero-based index of the receiving subcatchment.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_subcatch_set_outlet_subcatch(SWMM_Engine engine, int idx, int sc_idx);

/**
 * @brief Get the downstream subcatchment index (for cascading outlets).
 * @param engine       Engine handle.
 * @param idx          Zero-based subcatchment index.
 * @param[out] sc_idx  Receives the downstream subcatchment index, or -1 if none.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_subcatch_get_outlet_subcatch(SWMM_Engine engine, int idx, int* sc_idx);

/* =========================================================================
 * Infiltration getters
 * ========================================================================= */

/**
 * @brief Get the infiltration model type for a subcatchment.
 *
 * @details Returns an integer code: 0=HORTON, 1=MOD_HORTON, 2=GREEN_AMPT,
 *          3=MOD_GREEN_AMPT, 4=CURVE_NUMBER.
 *
 * @param engine      Engine handle.
 * @param idx         Zero-based subcatchment index.
 * @param[out] model  Receives the infiltration model code.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_subcatch_get_infil_model(SWMM_Engine engine, int idx, int* model);

/**
 * @brief Get Horton infiltration parameters for a subcatchment.
 * @param engine          Engine handle.
 * @param idx             Zero-based subcatchment index.
 * @param[out] f0         Receives the maximum infiltration rate.
 * @param[out] fmin       Receives the minimum infiltration rate.
 * @param[out] decay      Receives the decay constant (1/hr).
 * @param[out] dry_time   Receives the time to fully dry (hours).
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_subcatch_get_infil_horton(SWMM_Engine engine, int idx,
                                                          double* f0, double* fmin,
                                                          double* decay, double* dry_time);

/**
 * @brief Get Green–Ampt infiltration parameters for a subcatchment.
 * @param engine              Engine handle.
 * @param idx                 Zero-based subcatchment index.
 * @param[out] suction        Receives the soil capillary suction head.
 * @param[out] conductivity   Receives the saturated hydraulic conductivity.
 * @param[out] deficit        Receives the initial moisture deficit (fraction).
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_subcatch_get_infil_green_ampt(SWMM_Engine engine, int idx,
                                                          double* suction, double* conductivity,
                                                          double* deficit);

/**
 * @brief Get the Curve Number infiltration parameter for a subcatchment.
 * @param engine   Engine handle.
 * @param idx      Zero-based subcatchment index.
 * @param[out] cn  Receives the SCS curve number.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_subcatch_get_infil_curve_number(SWMM_Engine engine, int idx, double* cn);

/* =========================================================================
 * Subcatchment statistics
 * ========================================================================= */

/**
 * @brief Get the total precipitation volume at a subcatchment.
 * @param engine    Engine handle (ENDED or RUNNING state).
 * @param idx       Zero-based subcatchment index.
 * @param[out] vol  Receives the precipitation volume in project volume units.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_subcatch_get_stat_precip(SWMM_Engine engine, int idx, double* vol);

/**
 * @brief Get the total runoff volume from a subcatchment.
 * @param engine    Engine handle.
 * @param idx       Zero-based subcatchment index.
 * @param[out] vol  Receives the runoff volume in project volume units.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_subcatch_get_stat_runoff_vol(SWMM_Engine engine, int idx, double* vol);

/**
 * @brief Get the maximum runoff rate from a subcatchment.
 * @param engine     Engine handle.
 * @param idx        Zero-based subcatchment index.
 * @param[out] rate  Receives the maximum runoff rate in project flow units.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_subcatch_get_stat_max_runoff(SWMM_Engine engine, int idx, double* rate);

/* =========================================================================
 * Subcatchment landuse coverage
 * ========================================================================= */

/**
 * @brief Set the land use coverage fraction for a subcatchment.
 *
 * @details Assigns what fraction of a subcatchment's area is covered by
 *          a particular land use category (for buildup/washoff modeling).
 *
 * @param engine    Engine handle.
 * @param sc_idx    Zero-based subcatchment index.
 * @param lu_idx    Zero-based land use index.
 * @param fraction  Coverage fraction (0–1).
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_subcatch_set_coverage(SWMM_Engine engine, int sc_idx, int lu_idx, double fraction);

/**
 * @brief Get the land use coverage fraction for a subcatchment.
 * @param engine          Engine handle.
 * @param sc_idx          Zero-based subcatchment index.
 * @param lu_idx          Zero-based land use index.
 * @param[out] fraction   Receives the coverage fraction (0–1).
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_subcatch_get_coverage(SWMM_Engine engine, int sc_idx, int lu_idx, double* fraction);

/* =========================================================================
 * Hydraulic state getters
 * ========================================================================= */

/**
 * @brief Get the current runoff rate from a subcatchment.
 * @param engine       Engine handle.
 * @param idx          Zero-based subcatchment index.
 * @param[out] runoff  Receives the runoff rate in project flow units.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_subcatch_get_runoff(SWMM_Engine engine, int idx, double* runoff);

/**
 * @brief Get the current groundwater flow from a subcatchment.
 * @param engine        Engine handle.
 * @param idx           Zero-based subcatchment index.
 * @param[out] gw_flow  Receives the groundwater flow in project flow units.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_subcatch_get_groundwater(SWMM_Engine engine, int idx, double* gw_flow);

/**
 * @brief Get the current rainfall intensity at a subcatchment.
 * @param engine          Engine handle.
 * @param idx             Zero-based subcatchment index.
 * @param[out] rainfall   Receives the rainfall in project rate units.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_subcatch_get_rainfall(SWMM_Engine engine, int idx, double* rainfall);

/**
 * @brief Get the current snow depth on a subcatchment.
 * @param engine      Engine handle.
 * @param idx         Zero-based subcatchment index.
 * @param[out] depth  Receives the snow depth in project length units.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_subcatch_get_snow_depth(SWMM_Engine engine, int idx, double* depth);

/**
 * @brief Get the current evaporation rate at a subcatchment.
 * @param engine     Engine handle.
 * @param idx        Zero-based subcatchment index.
 * @param[out] evap  Receives the evaporation rate.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_subcatch_get_evap(SWMM_Engine engine, int idx, double* evap);

/**
 * @brief Get the current infiltration rate at a subcatchment.
 * @param engine      Engine handle.
 * @param idx         Zero-based subcatchment index.
 * @param[out] infil  Receives the infiltration rate.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_subcatch_get_infil(SWMM_Engine engine, int idx, double* infil);

/* --- Runtime forcing (RUNNING state only) --- */

/**
 * @brief Override rainfall on a subcatchment for the current timestep.
 *
 * @details Overrides the gage-driven rainfall for this subcatchment only.
 *          Value is applied for the current timestep; call again each step
 *          to sustain. Pass a negative value to revert to gage-driven.
 */
SWMM_ENGINE_API int swmm_subcatch_set_rainfall(SWMM_Engine engine, int idx, double rainfall);

/* =========================================================================
 * Water quality
 * ========================================================================= */

/**
 * @brief Get the pollutant concentration in subcatchment runoff.
 * @param engine        Engine handle.
 * @param subcatch_idx  Zero-based subcatchment index.
 * @param pollutant_idx Zero-based pollutant index.
 * @param[out] conc     Receives the concentration in pollutant units.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_subcatch_get_quality(SWMM_Engine engine, int subcatch_idx,
                                               int pollutant_idx, double* conc);

/* =========================================================================
 * Bulk access
 * ========================================================================= */

/**
 * @brief Get runoff rates for all subcatchments in a single call.
 * @param engine    Engine handle.
 * @param[out] buf  Caller-allocated buffer of at least @p count doubles.
 * @param count     Number of elements (should equal swmm_subcatch_count()).
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_subcatch_get_runoff_bulk(SWMM_Engine engine, double* buf, int count);

/**
 * @brief Get pollutant concentrations for all subcatchments for one pollutant.
 * @param engine        Engine handle.
 * @param pollutant_idx Zero-based pollutant index.
 * @param[out] buf      Caller-allocated buffer of at least @p count doubles.
 * @param count         Number of elements (should equal swmm_subcatch_count()).
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_subcatch_get_quality_bulk(SWMM_Engine engine, int pollutant_idx,
                                                    double* buf, int count);

/* =========================================================================
 * Ponded quality (mass in standing water between events)
 * ========================================================================= */

/** @brief Get ponded quality mass for a subcatchment-pollutant pair. */
SWMM_ENGINE_API int swmm_subcatch_get_ponded_quality(SWMM_Engine engine,
    int subcatch_idx, int pollutant_idx, double* mass);

/** @brief Set ponded quality mass for a subcatchment-pollutant pair. */
SWMM_ENGINE_API int swmm_subcatch_set_ponded_quality(SWMM_Engine engine,
    int subcatch_idx, int pollutant_idx, double mass);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* OPENSWMM_SUBCATCHMENTS_H */
