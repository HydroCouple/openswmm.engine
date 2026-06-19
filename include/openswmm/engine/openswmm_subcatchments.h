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

/**
 * @brief Set ONLY the infiltration model code for a subcatchment.
 *
 * @details model: 0=HORTON, 1=MOD_HORTON, 2=GREEN_AMPT, 3=MOD_GREEN_AMPT,
 *          4=CURVE_NUMBER. The per-model parameters are positionally
 *          overloaded, so after switching the model code callers should set
 *          the matching parameters via swmm_subcatch_set_infil_horton /
 *          _green_ampt / _curve_number.
 * @param engine  Engine handle.
 * @param idx     Zero-based subcatchment index.
 * @param model   Infiltration model code (0..4).
 * @returns SWMM_OK, or SWMM_ERR_BADPARAM if model is out of range.
 */
SWMM_ENGINE_API int swmm_subcatch_set_infil_model(SWMM_Engine engine, int idx, int model);

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

/* =========================================================================
 * Groundwater configuration ([GROUNDWATER])
 * =========================================================================
 * Configure a subcatchment's [GROUNDWATER] flow routing. Values are stored
 * exactly as parsed from the input file (raw user units) so they round-trip
 * identically. Distinct from the runtime gw STATE (theta / lower_depth)
 * injected via swmm_subcatch_set_gw_state. All are BUILDING/OPENED editable.
 */

/** @brief Assign the aquifer (by index, -1 = none) used by a subcatchment. */
SWMM_ENGINE_API int swmm_subcatch_set_aquifer(SWMM_Engine engine, int idx, int aquifer_idx);

/** @brief Get the aquifer index assigned to a subcatchment (-1 = none). */
SWMM_ENGINE_API int swmm_subcatch_get_aquifer(SWMM_Engine engine, int idx, int* aquifer_idx);

/** @brief Set the node (by index, -1 = none) receiving the subcatchment's groundwater flow. */
SWMM_ENGINE_API int swmm_subcatch_set_gw_node(SWMM_Engine engine, int idx, int node_idx);

/** @brief Get the node index receiving the subcatchment's groundwater flow (-1 = none). */
SWMM_ENGINE_API int swmm_subcatch_get_gw_node(SWMM_Engine engine, int idx, int* node_idx);

/**
 * @brief Set the groundwater flow parameters ([GROUNDWATER] token order).
 * @param surf_elev  Surface elevation (SurfEl).
 * @param a1,b1      Groundwater outflow coefficient & exponent.
 * @param a2,b2      Surface-water outflow coefficient & exponent.
 * @param a3         Surface/groundwater interaction coefficient.
 * @param tw         Threshold groundwater table elevation (Twgr).
 * @param hstar      Water-table elevation at which lateral GW flow ceases (Hstar).
 * @returns SWMM_OK or error code.
 */
SWMM_ENGINE_API int swmm_subcatch_set_gw_params(SWMM_Engine engine, int idx,
                                                double surf_elev, double a1, double b1,
                                                double a2, double b2, double a3,
                                                double tw, double hstar);

/** @brief Get the groundwater flow parameters (see swmm_subcatch_set_gw_params). */
SWMM_ENGINE_API int swmm_subcatch_get_gw_params(SWMM_Engine engine, int idx,
                                                double* surf_elev, double* a1, double* b1,
                                                double* a2, double* b2, double* a3,
                                                double* tw, double* hstar);

/* =========================================================================
 * State injection (data assimilation)
 * ========================================================================= */

/**
 * @brief Set the groundwater state on a subcatchment.
 *
 * State injection for data assimilation / external coupling. Pass a
 * negative value to leave that component unchanged. Note that mass
 * balance reports will reflect the storage discontinuity, mirroring
 * hotstart loading.
 *
 * @param engine       Engine handle.
 * @param idx          Subcatchment index (must have groundwater).
 * @param theta        Upper zone moisture content (0..porosity), or < 0 to keep.
 * @param lower_depth  Saturated zone depth above aquifer bottom in user
 *                     length units (ft US, m SI), or < 0 to keep.
 * @returns SWMM_OK or error code.
 */
SWMM_ENGINE_API int swmm_subcatch_set_gw_state(SWMM_Engine engine, int idx,
                                               double theta, double lower_depth);

/**
 * @brief Get the groundwater state on a subcatchment.
 *
 * @param engine            Engine handle.
 * @param idx               Subcatchment index (must have groundwater).
 * @param[out] theta        Receives the upper zone moisture content.
 * @param[out] lower_depth  Receives the saturated zone depth (user length units).
 * @returns SWMM_OK or error code.
 */
SWMM_ENGINE_API int swmm_subcatch_get_gw_state(SWMM_Engine engine, int idx,
                                               double* theta, double* lower_depth);

/**
 * @brief Set the snow pack state on one snow subarea of a subcatchment.
 *
 * State injection for data assimilation (e.g. observed SWE). Pass a
 * negative value to leave that component unchanged.
 *
 * @param engine   Engine handle.
 * @param idx      Subcatchment index (must have a snow pack).
 * @param surface  Snow subarea: 0 plowable, 1 impervious, 2 pervious.
 * @param swe      Snow water equivalent in user depth units (in US, mm SI), or < 0 to keep.
 * @param fw       Free water in user depth units, or < 0 to keep.
 * @param ati      Antecedent temperature index (deg F US, deg C SI); pass
 *                 <= -999 to keep (negative temperatures are valid).
 * @param coldc    Cold content in user depth units of melt equivalent, or < 0 to keep.
 * @returns SWMM_OK or error code.
 */
SWMM_ENGINE_API int swmm_subcatch_set_snow_state(SWMM_Engine engine, int idx,
                                                 int surface, double swe, double fw,
                                                 double ati, double coldc);

/**
 * @brief Get the snow pack state on one snow subarea of a subcatchment.
 *
 * @param engine      Engine handle.
 * @param idx         Subcatchment index (must have a snow pack).
 * @param surface     Snow subarea: 0 plowable, 1 impervious, 2 pervious.
 * @param[out] swe    Receives SWE (user depth units); may be NULL.
 * @param[out] fw     Receives free water (user depth units); may be NULL.
 * @param[out] ati    Receives ATI (user temperature units); may be NULL.
 * @param[out] coldc  Receives cold content (user depth units); may be NULL.
 * @returns SWMM_OK or error code.
 */
SWMM_ENGINE_API int swmm_subcatch_get_snow_state(SWMM_Engine engine, int idx,
                                                 int surface, double* swe, double* fw,
                                                 double* ati, double* coldc);

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
 * Phase 3 bulk getters — added in OpenSWMM 6.0.0 to eliminate the N
 * round-trip cost of per-subcatchment scalar accessors. All return a
 * caller-allocated @c double buffer of length @c count (clipped at
 * @c swmm_subcatch_count()); the IDs variant returns a stride-packed
 * UTF-8 buffer following the same format as @ref swmm_node_get_ids_bulk.
 * ========================================================================= */

/**
 * @brief Get rainfall rates for all subcatchments in a single call.
 * @details Bulk variant of @ref swmm_subcatch_get_rainfall. Simple SoA copy.
 * @since 6.0.0
 */
SWMM_ENGINE_API int swmm_subcatch_get_rainfall_bulk(SWMM_Engine engine, double* buf, int count);

/**
 * @brief Get evaporation losses for all subcatchments in a single call.
 * @details Bulk variant of @ref swmm_subcatch_get_evap. Simple SoA copy of
 *          the @c evap_loss column.
 * @since 6.0.0
 */
SWMM_ENGINE_API int swmm_subcatch_get_evap_bulk(SWMM_Engine engine, double* buf, int count);

/**
 * @brief Get infiltration losses for all subcatchments in a single call.
 * @details Bulk variant of @ref swmm_subcatch_get_infil. Simple SoA copy
 *          of the @c infil_loss column.
 * @since 6.0.0
 */
SWMM_ENGINE_API int swmm_subcatch_get_infil_bulk(SWMM_Engine engine, double* buf, int count);

/**
 * @brief Get snow depths for all subcatchments in a single call.
 * @details Bulk variant of @ref swmm_subcatch_get_snow_depth. Snow state
 *          currently lives in the SnowSolver, not SubcatchData; like the
 *          scalar accessor this returns zeros for every entry pending
 *          full snow-state integration (see plan Appendix A).
 * @since 6.0.0
 */
SWMM_ENGINE_API int swmm_subcatch_get_snow_depth_bulk(SWMM_Engine engine, double* buf, int count);

/**
 * @brief Get subcatchment IDs for all subcatchments in a single call
 *        (stride-packed UTF-8).
 * @details Format identical to @ref swmm_node_get_ids_bulk.
 * @param engine  Engine handle.
 * @param[out] buf Caller-allocated buffer of @c stride*count bytes.
 * @param stride  Per-ID slot size in bytes (must be > 1).
 * @param count   Number of IDs to read.
 * @returns @c SWMM_OK on success; @c SWMM_ERR_BADHANDLE if @p engine is
 *          invalid; @c SWMM_ERR_BADPARAM if @p buf is NULL,
 *          @p stride < 2, or @p count <= 0.
 * @see swmm_subcatch_id, swmm_node_get_ids_bulk
 * @since 6.0.0
 */
SWMM_ENGINE_API int swmm_subcatch_get_ids_bulk(SWMM_Engine engine,
                                                char* buf,
                                                int stride,
                                                int count);

/* =========================================================================
 * Ponded quality (mass in standing water between events)
 * ========================================================================= */

/** @brief Get ponded quality mass for a subcatchment-pollutant pair. */
SWMM_ENGINE_API int swmm_subcatch_get_ponded_quality(SWMM_Engine engine,
    int subcatch_idx, int pollutant_idx, double* mass);

/** @brief Set ponded quality mass for a subcatchment-pollutant pair. */
SWMM_ENGINE_API int swmm_subcatch_set_ponded_quality(SWMM_Engine engine,
    int subcatch_idx, int pollutant_idx, double mass);

/** @brief Rename the subcatchment at `idx` to `newId`.
 *  Returns SWMM_ERR_BADPARAM if newId is null, empty, already in use, or
 *  idx is out of range. */
SWMM_ENGINE_API int swmm_subcatch_rename(SWMM_Engine engine, int idx, const char* newId);

/* =========================================================================
 * Aquifer definitions ([AQUIFERS] section) — Slice BM.0 / BP.6.6.4
 * ========================================================================= */

/**
 * @brief Get the total number of aquifer definitions in the model.
 * @param engine  Engine handle.
 * @returns Number of aquifer definitions, or -1 on error.
 */
SWMM_ENGINE_API int swmm_aquifer_count(SWMM_Engine engine);

/**
 * @brief Look up an aquifer's zero-based index by its string identifier.
 * @param engine  Engine handle.
 * @param id      Null-terminated aquifer identifier.
 * @returns Zero-based index, or -1 if not found.
 */
SWMM_ENGINE_API int swmm_aquifer_index(SWMM_Engine engine, const char* id);

/**
 * @brief Get the string identifier of an aquifer definition by index.
 * @param engine  Engine handle.
 * @param idx     Zero-based aquifer index.
 * @returns Null-terminated string owned by the engine, or NULL on error.
 */
SWMM_ENGINE_API const char* swmm_aquifer_id(SWMM_Engine engine, int idx);

/**
 * @brief Add a new aquifer definition with default (zero) parameters.
 *
 * @details Parameters default to 0.0 / empty strings — use the property
 *          setters from Slice BP `AquiferEditor` to configure porosity,
 *          conductivity, etc. Lifecycle: BUILDING or OPENED.
 *
 * @param engine  Engine handle.
 * @param id      Unique null-terminated identifier.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_aquifer_add(SWMM_Engine engine, const char* id);

/**
 * @brief Aquifer parameter codes for swmm_aquifer_get_param / _set_param.
 *
 * @details Values use input-file units (the same columns as the [AQUIFERS]
 *          line). The flux-coefficient parameters (CONDUCTIVITY,
 *          CONDUCT_SLOPE, TENSION_SLOPE, UPPER_EVAP_FRAC, LOWER_EVAP_DEPTH,
 *          LOWER_LOSS_COEFF) are settable both before the simulation starts
 *          and while it is running; the structural / initial-condition
 *          parameters (POROSITY, WILTING_POINT, FIELD_CAPACITY, BOTTOM_ELEV,
 *          WATER_TABLE_ELEV, UPPER_MOISTURE) bound or seed the groundwater
 *          state and are pre-start-only.
 */
typedef enum SWMM_AquiferParam {
    SWMM_AQUIFER_POROSITY = 0,
    SWMM_AQUIFER_WILTING_POINT = 1,
    SWMM_AQUIFER_FIELD_CAPACITY = 2,
    SWMM_AQUIFER_CONDUCTIVITY = 3,
    SWMM_AQUIFER_CONDUCT_SLOPE = 4,
    SWMM_AQUIFER_TENSION_SLOPE = 5,
    SWMM_AQUIFER_UPPER_EVAP_FRAC = 6,
    SWMM_AQUIFER_LOWER_EVAP_DEPTH = 7,
    SWMM_AQUIFER_LOWER_LOSS_COEFF = 8,
    SWMM_AQUIFER_BOTTOM_ELEV = 9,
    SWMM_AQUIFER_WATER_TABLE_ELEV = 10,
    SWMM_AQUIFER_UPPER_MOISTURE = 11
} SWMM_AquiferParam;

/**
 * @brief Get an aquifer parameter (input-file units).
 * @param engine  Engine handle.
 * @param idx     Zero-based aquifer index.
 * @param param   A SWMM_AquiferParam code.
 * @param value   Receives the parameter value.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_aquifer_get_param(SWMM_Engine engine, int idx, int param, double* value);

/**
 * @brief Set an aquifer parameter (input-file units).
 *
 * @details Flux-coefficient parameters take effect on the next step when set
 *          mid-run (the groundwater solver's per-subcatchment copies are
 *          refreshed); structural / initial-condition parameters return
 *          SWMM_ERR_LIFECYCLE while the simulation is running.
 *
 * @param engine  Engine handle.
 * @param idx     Zero-based aquifer index.
 * @param param   A SWMM_AquiferParam code.
 * @param value   New parameter value.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_aquifer_set_param(SWMM_Engine engine, int idx, int param, double value);

/* =========================================================================
 * Snowpack definitions ([SNOWPACKS] section) — Slice BM.0 / BP.6.6.5
 * ========================================================================= */

/**
 * @brief Get the total number of snowpack definitions in the model.
 * @param engine  Engine handle.
 * @returns Number of snowpack definitions, or -1 on error.
 */
SWMM_ENGINE_API int swmm_snowpack_count(SWMM_Engine engine);

/**
 * @brief Look up a snowpack's zero-based index by its string identifier.
 * @param engine  Engine handle.
 * @param id      Null-terminated snowpack identifier.
 * @returns Zero-based index, or -1 if not found.
 */
SWMM_ENGINE_API int swmm_snowpack_index(SWMM_Engine engine, const char* id);

/**
 * @brief Get the string identifier of a snowpack definition by index.
 * @param engine  Engine handle.
 * @param idx     Zero-based snowpack index.
 * @returns Null-terminated string owned by the engine, or NULL on error.
 */
SWMM_ENGINE_API const char* swmm_snowpack_id(SWMM_Engine engine, int idx);

/**
 * @brief Add a new snowpack definition with default (zero) parameters.
 *
 * @details All three surface arrays (plowable / impervious / pervious) and
 *          the removal row are zero-initialised. Use the property setters
 *          from Slice BP `SnowpackEditor` to configure melt coefficients,
 *          ATI, negative-melt-ratio, and removal fractions.
 *          Lifecycle: BUILDING or OPENED.
 *
 * @param engine  Engine handle.
 * @param id      Unique null-terminated identifier.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_snowpack_add(SWMM_Engine engine, const char* id);

/* =========================================================================
 * Tag — free-form string label from the INP `[TAGS]` section
 * ========================================================================= */

/** @brief Read the subcatchment's tag into `buf` (NUL-terminated, truncated if too small). */
SWMM_ENGINE_API int swmm_subcatch_get_tag(SWMM_Engine engine, int idx,
                                            char* buf, int buflen);

/** @brief Set or clear the subcatchment's tag. Null/empty clears. Persists across rename. */
SWMM_ENGINE_API int swmm_subcatch_set_tag(SWMM_Engine engine, int idx,
                                            const char* tag);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* OPENSWMM_SUBCATCHMENTS_H */
