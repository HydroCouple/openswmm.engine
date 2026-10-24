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
 * @file      openswmm_xsect.h
 * @brief     OpenSWMM Engine — standalone cross-section geometry C API.
 *
 * @details Exposes the engine's canonical cross-section geometry kernels as a
 *          reference implementation usable *without* a model. Construct a
 *          cross-section from shape + geometry parameters (or from transect,
 *          shape-curve or street data), then query area / top width / hydraulic
 *          radius / section factor / critical depth and their inverses. The
 *          same functions the routing solvers use answer these queries, so
 *          results agree with a simulation bit-for-bit.
 *
 *          A handle can also be taken from a link of an open model
 *          (swmm_link_create_xsect), using the geometry the engine actually
 *          built — including transect tables. Such a handle owns a deep copy
 *          and stays valid after the engine is closed.
 *
 *          @code
 *          SWMM_XSect xs;
 *          swmm_xsect_create(SWMM_XSECT_CIRCULAR, 1.0, 0, 0, 0,
 *                            SWMM_UNITS_SI, &xs);
 *          double a;
 *          swmm_xsect_area_of_depth(xs, 0.5, &a);   // half-full 1 m pipe
 *          swmm_xsect_free(xs);
 *          @endcode
 *
 * @par Units
 *      Every geometry value crossing this API is in the handle's *display*
 *      units, matching the rest of the C API: lengths in ft (US) or m (SI),
 *      areas in ft² / m², section factors in ft^(8/3) / m^(8/3). Flows (for
 *      swmm_xsect_critical_depth) are in the handle's flow units — CFS for a
 *      standalone US handle, CMS for a standalone SI handle, and the model's
 *      own flow units for a handle taken from a link. Query them with
 *      swmm_xsect_get_units().
 *
 * @ingroup engine_api
 * @see openswmm_links.h — SWMM_XSectShape shape codes and per-link geometry.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_XSECT_H
#define OPENSWMM_XSECT_H

#include "openswmm_engine.h"
#include "openswmm_links.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Opaque handle to a standalone cross-section. Free with swmm_xsect_free(). */
typedef void* SWMM_XSect;

/**
 * @brief Unit system for a standalone cross-section.
 *
 * @details Matches the engine's internal 0=US / 1=SI coding (see
 *          swmm_get_unit_system()). Selects both the length units (ft vs m)
 *          and the default flow unit used by swmm_xsect_critical_depth()
 *          (CFS vs CMS).
 */
typedef enum SWMM_UnitSystem {
    SWMM_UNITS_US = 0, /**< Feet, ft², ft^(8/3), CFS. */
    SWMM_UNITS_SI = 1  /**< Metres, m², m^(8/3), CMS. */
} SWMM_UnitSystem;

/* =========================================================================
 * Construction / destruction
 * ========================================================================= */

/**
 * @brief Create a cross-section from a shape code and its geometry parameters.
 *
 * @details Handles every self-contained shape — i.e. all of SWMM_XSectShape
 *          except SWMM_XSECT_IRREGULAR, SWMM_XSECT_CUSTOM and
 *          SWMM_XSECT_STREET, whose geometry comes from tabulated data and
 *          which have their own constructors below.
 *
 *          geom1–geom4 carry the same meaning as the `[XSECTIONS]` Geom1–Geom4
 *          columns; see the per-shape notes on SWMM_XSectShape. Unused
 *          parameters may be passed as 0.
 *
 * @note SWMM_XSECT_FORCE_MAIN is geometrically identical to
 *       SWMM_XSECT_CIRCULAR; it differs only in its friction law (its
 *       section factor uses the Hazen-Williams exponent), so its geometric
 *       queries match a circular pipe of the same diameter.
 * @note SWMM_XSECT_DUMMY has no geometry: it constructs successfully and every
 *       query returns 0.
 *
 * @param[in] shape A SWMM_XSectShape code.
 * @param[in] geom1 Full depth / diameter, in display units (see the unit conventions above).
 * @param[in] geom2 Second geometry parameter, or 0.
 * @param[in] geom3 Third geometry parameter, or 0.
 * @param[in] geom4 Fourth geometry parameter, or 0.
 * @param[in] unit_system A SWMM_UnitSystem value.
 * @param[out] out Receives the new handle on success.
 * @returns SWMM_OK, or SWMM_ERR_BADPARAM if `out` is NULL, `shape` is not a
 *          valid self-contained shape, `unit_system` is out of range, or the
 *          geometry is degenerate (e.g. non-positive depth).
 * @since 6.0
 *
 *
 * @note Success transfers a newly allocated independent handle to the caller; release with
 *     swmm_xsect_free(). A non-null out slot is cleared before geometry validation (except a null
 *     engine is rejected first for the link constructor). Replacing a variable does not free its
 *     prior handle. Input arrays/geometry are copied, not retained. All four geometry inputs must
 *     be finite, including unused parameters. Shape/geometry validation uses the canonical
 *     setParams kernel.
 */
SWMM_ENGINE_API int swmm_xsect_create(int shape, double geom1, double geom2, double geom3,
                                      double geom4, int unit_system, SWMM_XSect* out);

/**
 * @brief Create an irregular (natural channel) cross-section from a transect.
 *
 * @details Mirrors an `[TRANSECTS]` entry: `stations`/`elevations` are the GR
 *          station-elevation pairs, and the Manning's n triple is the NC line.
 *          The roughness values are load-bearing, not decorative — the
 *          hydraulic-radius table is conveyance-weighted across the left
 *          overbank / channel / right overbank subsections, so passing the
 *          wrong n changes the geometry this handle reports.
 *
 * @param[in] stations Station (horizontal) coordinates, display units, ascending.
 * @param[in] elevations Elevation at each station, display units.
 * @param[in] n_pts Number of station/elevation pairs (>= 2).
 * @param[in] x_left_bank Station where the left overbank ends. Pass the same
 *                       value as `x_right_bank` for a channel with no overbanks.
 * @param[in] x_right_bank Station where the right overbank begins.
 * @param[in] n_left Manning's n, left overbank. 0 → use `n_channel`.
 * @param[in] n_channel Manning's n, main channel. Must be > 0.
 * @param[in] n_right Manning's n, right overbank. 0 → use `n_channel`.
 * @param[in] length_factor Main-channel / flood-plain length ratio. 0 → 1.0.
 * @param[in] unit_system A SWMM_UnitSystem value.
 * @param[out] out Receives the new handle on success.
 * @returns SWMM_OK, or SWMM_ERR_BADPARAM on a NULL pointer, `n_pts` < 2,
 *          `n_channel` <= 0, an out-of-range `unit_system`, or a transect whose
 *          stations yield no wetted area.
 * @since 6.0
 *
 *
 * @note Success transfers a newly allocated independent handle to the caller; release with
 *     swmm_xsect_free(). A non-null out slot is cleared before geometry validation (except a null
 *     engine is rejected first for the link constructor). Replacing a variable does not free its
 *     prior handle. Input arrays/geometry are copied, not retained. Each station/elevation must be
 *     finite. Negative overbank roughness or length_factor is refused; caller must also provide
 *     finite bank/roughness/ratio values. Source arrays must have n_pts entries and are copied into
 *     owned internal-foot tables.
 */
SWMM_ENGINE_API int swmm_xsect_create_irregular(const double* stations, const double* elevations,
                                                int n_pts, double x_left_bank, double x_right_bank,
                                                double n_left, double n_channel, double n_right,
                                                double length_factor, int unit_system,
                                                SWMM_XSect* out);

/**
 * @brief Create a custom cross-section from a normalized shape curve.
 *
 * @details Mirrors a `SHAPE`-type `[CURVES]` entry scaled to `y_full`:
 *          `curve_depths` are y/yFull in [0,1] and `curve_widths` are the
 *          corresponding width/wMax.
 *
 * @param[in] y_full Full depth of the section, display units (> 0).
 * @param[in] curve_depths Normalized depths (y/yFull), ascending.
 * @param[in] curve_widths Normalized widths (w/wMax) at each depth.
 * @param[in] n_pts Number of curve points (>= 2).
 * @param[in] unit_system A SWMM_UnitSystem value.
 * @param[out] out Receives the new handle on success.
 * @returns SWMM_OK, or SWMM_ERR_BADPARAM on a NULL pointer, `n_pts` < 2,
 *          `y_full` <= 0, or an out-of-range `unit_system`.
 * @since 6.0
 *
 *
 * @note Success transfers a newly allocated independent handle to the caller; release with
 *     swmm_xsect_free(). A non-null out slot is cleared before geometry validation (except a null
 *     engine is rejected first for the link constructor). Replacing a variable does not free its
 *     prior handle. Input arrays/geometry are copied, not retained. Curve arrays must have n_pts
 *     finite pairs; the builder rejects a resulting nonpositive full area. Caller must provide an
 *     ordered valid normalized profile; this wrapper does not separately enforce every depth/width
 *     bound.
 */
SWMM_ENGINE_API int swmm_xsect_create_custom(double y_full, const double* curve_depths,
                                             const double* curve_widths, int n_pts, int unit_system,
                                             SWMM_XSect* out);

/**
 * @brief Create a street cross-section.
 *
 * @details Mirrors a `[STREETS]` entry; parameters match
 *          swmm_street_set_params(). Slopes are percentages, as in the input
 *          file.
 *
 * @param[in] width Distance from curb to crown, display units.
 * @param[in] curb_height Curb height, display units.
 * @param[in] slope Transverse road slope, percent.
 * @param[in] roughness Manning's n of the road surface (> 0).
 * @param[in] gutter_depression Depressed-gutter depth, display units.
 * @param[in] gutter_width Depressed-gutter width, display units.
 * @param[in] sides 1 = half street, 2 = full street.
 * @param[in] back_width Backing width, display units (0 if none).
 * @param[in] back_slope Backing slope, percent.
 * @param[in] back_roughness Backing Manning's n.
 * @param[in] unit_system A SWMM_UnitSystem value.
 * @param[out] out Receives the new handle on success.
 * @returns SWMM_OK, or SWMM_ERR_BADPARAM on a NULL `out`, a non-positive
 *          width/curb height/roughness, `sides` outside {1,2}, or an
 *          out-of-range `unit_system`.
 * @since 6.0
 *
 *
 * @note Success transfers a newly allocated independent handle to the caller; release with
 *     swmm_xsect_free(). A non-null out slot is cleared before geometry validation (except a null
 *     engine is rejected first for the link constructor). Replacing a variable does not free its
 *     prior handle. Input arrays/geometry are copied, not retained. Lengths are converted to
 *     internal feet and percent slopes to fractions; the generated table must have positive
 *     area/depth. Does not retain caller geometry or a model handle.
 */
SWMM_ENGINE_API int swmm_xsect_create_street(double width, double curb_height, double slope,
                                             double roughness, double gutter_depression,
                                             double gutter_width, int sides, double back_width,
                                             double back_slope, double back_roughness,
                                             int unit_system, SWMM_XSect* out);

/**
 * @brief Create a cross-section from a link of an open model.
 *
 * @details Deep-copies the geometry the engine actually built for the link,
 *          including any transect / shape-curve / street tables, so the handle
 *          remains valid after the engine is closed or destroyed. The handle
 *          inherits the model's unit system and flow units.
 *
 * @note Requires resolved geometry. A model still under programmatic
 *       construction has only the raw geoms stored, not the derived full-flow
 *       properties, so this returns SWMM_ERR_LIFECYCLE until
 *       swmm_finalize_model() (or swmm_engine_open()) has run.
 *
 * @param[in] engine Engine handle with geometry resolved (OPENED or later).
 * @param[in] link_idx Zero-based link index.
 * @param[out] out Receives the new handle on success.
 * @returns SWMM_OK; SWMM_ERR_BADHANDLE for a NULL engine; SWMM_ERR_BADINDEX if
 *          `link_idx` is out of range; SWMM_ERR_LIFECYCLE if the model is still
 *          in the BUILDING state; SWMM_ERR_BADPARAM if `out` is NULL or the
 *          link has no cross-section (e.g. a pump).
 * @since 6.0
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. A failed link/pollutant index check returns
 *     SWMM_ERR_BADINDEX. Success transfers a newly allocated independent handle to the caller;
 *     release with swmm_xsect_free(). A non-null out slot is cleared before geometry validation
 *     (except a null engine is rejected first for the link constructor). Replacing a variable does
 *     not free its prior handle. Input arrays/geometry are copied, not retained.
 */
SWMM_ENGINE_API int swmm_link_create_xsect(SWMM_Engine engine, int link_idx, SWMM_XSect* out);

/**
 * @brief Release a cross-section handle.
 * @param[in] xsect Handle from any swmm_xsect_create* / swmm_link_create_xsect.
 *               NULL is accepted and ignored.
 * @returns SWMM_OK.
 * @since 6.0
 *
 *
 * @note Deletes the handle even when no model exists; do not use it afterward or free a non-null
 *     handle twice.
 */
SWMM_ENGINE_API int swmm_xsect_free(SWMM_XSect xsect);

/* =========================================================================
 * Identity
 * ========================================================================= */

/**
 * @brief Get the shape code of a cross-section.
 * @param[in] xsect Cross-section handle.
 * @param[out] shape Receives a SWMM_XSectShape code.
 * @returns SWMM_OK, SWMM_ERR_BADHANDLE, or SWMM_ERR_BADPARAM if `shape` is NULL.
 * @since 6.0
 *
 *
 * @note Does not alter stored geometry or a live model. A non-null handle must remain alive
 *     throughout the call.
 */
SWMM_ENGINE_API int swmm_xsect_get_shape(SWMM_XSect xsect, int* shape);

/**
 * @brief Get the unit system and flow units a handle reports its results in.
 *
 * @details `flow_units` matters only for swmm_xsect_critical_depth(); every
 *          other query is governed by `unit_system` alone.
 *
 * @param[in] xsect Cross-section handle.
 * @param[out] unit_system Receives a SWMM_UnitSystem value (may be NULL).
 * @param[out] flow_units Receives the flow-unit code (may be NULL):
 *                          0=CFS, 1=GPM, 2=MGD, 3=CMS, 4=LPS, 5=MLD. A
 *                          standalone handle reports CFS (US) or CMS (SI); a
 *                          handle from a link reports its model's units.
 * @returns SWMM_OK or SWMM_ERR_BADHANDLE.
 * @since 6.0
 *
 *
 * @note unit_system may be NULL to suppress that output. flow_units may be NULL to suppress that
 *     output. Does not alter stored geometry or a live model. A non-null handle must remain alive
 *     throughout the call.
 */
SWMM_ENGINE_API int swmm_xsect_get_units(SWMM_XSect xsect, int* unit_system, int* flow_units);

/**
 * @brief Get the name of a cross-section shape code.
 *
 * @details Useful for reporting, and for checking a code at runtime after the
 *          6.0 SWMM_XSectShape renumbering (see openswmm_links.h).
 *
 * @param[in] shape A SWMM_XSectShape code.
 * @returns A static, never-freed string such as `"CIRCULAR"`, or NULL if
 *          `shape` is not a valid code.
 * @since 6.0
 */
SWMM_ENGINE_API const char* swmm_xsect_shape_name(int shape);

/* =========================================================================
 * Queries — scalar
 * ========================================================================= */

/**
 * @brief Flow area at a given depth.
 * @param[in] xsect Cross-section handle.
 * @param[in] depth Depth of flow, display units. Depths above the full depth of
 *                  a closed shape are clamped, as in the routing solvers.
 * @param[out] area Receives the area, display units squared.
 * @returns SWMM_OK; SWMM_ERR_BADHANDLE; SWMM_ERR_BADPARAM if `area` is NULL or
 *          `depth` is negative or not finite.
 * @since 6.0
 *
 *
 * @note Does not alter stored geometry or a live model. A non-null handle must remain alive
 *     throughout the call.
 */
SWMM_ENGINE_API int swmm_xsect_area_of_depth(SWMM_XSect xsect, double depth, double* area);

/**
 * @brief Top width of the water surface at a given depth.
 * @param[in] xsect Cross-section handle.
 * @param[in] depth Depth of flow, display units.
 * @param[out] width Receives the top width, display units.
 * @returns SWMM_OK; SWMM_ERR_BADHANDLE; SWMM_ERR_BADPARAM if `width` is NULL or
 *          `depth` is negative or not finite.
 * @since 6.0
 *
 *
 * @note Does not alter stored geometry or a live model. A non-null handle must remain alive
 *     throughout the call.
 */
SWMM_ENGINE_API int swmm_xsect_width_of_depth(SWMM_XSect xsect, double depth, double* width);

/**
 * @brief Hydraulic radius (area / wetted perimeter) at a given depth.
 * @param[in] xsect Cross-section handle.
 * @param[in] depth Depth of flow, display units.
 * @param[out] hydrad Receives the hydraulic radius, display units.
 * @returns SWMM_OK; SWMM_ERR_BADHANDLE; SWMM_ERR_BADPARAM if `hydrad` is NULL
 *          or `depth` is negative or not finite.
 * @since 6.0
 *
 *
 * @note Does not alter stored geometry or a live model. A non-null handle must remain alive
 *     throughout the call.
 */
SWMM_ENGINE_API int swmm_xsect_hydrad_of_depth(SWMM_XSect xsect, double depth, double* hydrad);

/**
 * @brief Depth of flow for a given area — the inverse of swmm_xsect_area_of_depth().
 * @param[in] xsect Cross-section handle.
 * @param[in] area Flow area, display units squared.
 * @param[out] depth Receives the depth, display units.
 * @returns SWMM_OK; SWMM_ERR_BADHANDLE; SWMM_ERR_BADPARAM if `depth` is NULL or
 *          `area` is negative or not finite.
 * @since 6.0
 *
 *
 * @note Does not alter stored geometry or a live model. A non-null handle must remain alive
 *     throughout the call.
 */
SWMM_ENGINE_API int swmm_xsect_depth_of_area(SWMM_XSect xsect, double area, double* depth);

/**
 * @brief Hydraulic radius for a given area.
 * @param[in] xsect Cross-section handle.
 * @param[in] area Flow area, display units squared.
 * @param[out] hydrad Receives the hydraulic radius, display units.
 * @returns SWMM_OK; SWMM_ERR_BADHANDLE; SWMM_ERR_BADPARAM if `hydrad` is NULL
 *          or `area` is negative or not finite.
 * @since 6.0
 *
 *
 * @note Does not alter stored geometry or a live model. A non-null handle must remain alive
 *     throughout the call.
 */
SWMM_ENGINE_API int swmm_xsect_hydrad_of_area(SWMM_XSect xsect, double area, double* hydrad);

/**
 * @brief Section factor (A·R^(2/3)) for a given area.
 * @param[in] xsect Cross-section handle.
 * @param[in] area Flow area, display units squared.
 * @param[out] sf Receives the section factor, display units^(8/3).
 * @returns SWMM_OK; SWMM_ERR_BADHANDLE; SWMM_ERR_BADPARAM if `sf` is NULL or
 *          `area` is negative or not finite.
 * @since 6.0
 *
 *
 * @note Does not alter stored geometry or a live model. A non-null handle must remain alive
 *     throughout the call.
 */
SWMM_ENGINE_API int swmm_xsect_sectfactor_of_area(SWMM_XSect xsect, double area, double* sf);

/**
 * @brief Flow area for a given section factor — the inverse of
 *        swmm_xsect_sectfactor_of_area(), used to solve for normal depth.
 * @param[in] xsect Cross-section handle.
 * @param[in] sf Section factor, display units^(8/3).
 * @param[out] area Receives the area, display units squared.
 * @returns SWMM_OK; SWMM_ERR_BADHANDLE; SWMM_ERR_BADPARAM if `area` is NULL or
 *          `sf` is negative or not finite.
 * @since 6.0
 *
 *
 * @note Does not alter stored geometry or a live model. A non-null handle must remain alive
 *     throughout the call.
 */
SWMM_ENGINE_API int swmm_xsect_area_of_sectfactor(SWMM_XSect xsect, double sf, double* area);

/**
 * @brief Derivative of the section factor with respect to area, dS/dA.
 * @param[in] xsect Cross-section handle.
 * @param[in] area Flow area, display units squared.
 * @param[out] dsda Receives dS/dA, display units^(2/3).
 * @returns SWMM_OK; SWMM_ERR_BADHANDLE; SWMM_ERR_BADPARAM if `dsda` is NULL or
 *          `area` is negative or not finite.
 * @since 6.0
 *
 *
 * @note Does not alter stored geometry or a live model. A non-null handle must remain alive
 *     throughout the call.
 */
SWMM_ENGINE_API int swmm_xsect_dsda(SWMM_XSect xsect, double area, double* dsda);

/**
 * @brief Critical depth for a given flow.
 * @param[in] xsect Cross-section handle.
 * @param[in] flow Flow, in the handle's flow units (see swmm_xsect_get_units()).
 * @param[out] ycrit Receives the critical depth, display units.
 * @returns SWMM_OK; SWMM_ERR_BADHANDLE; SWMM_ERR_BADPARAM if `ycrit` is NULL or
 *          `flow` is negative or not finite.
 * @since 6.0
 *
 *
 * @note Does not alter stored geometry or a live model. A non-null handle must remain alive
 *     throughout the call.
 */
SWMM_ENGINE_API int swmm_xsect_critical_depth(SWMM_XSect xsect, double flow, double* ycrit);

/**
 * @brief Full-depth (bank-full) properties of the cross-section.
 *
 * @details Every out-parameter is optional — pass NULL for any value not wanted.
 *
 * @param[in] xsect Cross-section handle.
 * @param[out] y_full Full depth, display units.
 * @param[out] a_full Area when full, display units squared.
 * @param[out] r_full Hydraulic radius when full, display units.
 * @param[out] w_max Width at the widest point, display units.
 * @param[out] s_full Section factor when full, display units^(8/3).
 * @param[out] a_max Area at which flow is a maximum, display units squared.
 * @returns SWMM_OK or SWMM_ERR_BADHANDLE.
 * @since 6.0
 *
 *
 * @note y_full may be NULL to suppress that output. a_full may be NULL to suppress that output.
 *     r_full may be NULL to suppress that output. w_max may be NULL to suppress that output. s_full
 *     may be NULL to suppress that output. a_max may be NULL to suppress that output. Does not
 *     alter stored geometry or a live model. A non-null handle must remain alive throughout the
 *     call.
 */
SWMM_ENGINE_API int swmm_xsect_full_properties(SWMM_XSect xsect, double* y_full, double* a_full,
                                               double* r_full, double* w_max, double* s_full,
                                               double* a_max);

/**
 * @brief Whether the cross-section is open to the atmosphere at its top.
 * @param[in] xsect Cross-section handle.
 * @param[out] is_open Receives 1 for an open channel, 0 for a closed conduit.
 * @returns SWMM_OK; SWMM_ERR_BADHANDLE; SWMM_ERR_BADPARAM if `is_open` is NULL.
 * @since 6.0
 *
 *
 * @note Does not alter stored geometry or a live model. A non-null handle must remain alive
 *     throughout the call.
 */
SWMM_ENGINE_API int swmm_xsect_is_open(SWMM_XSect xsect, int* is_open);

/* =========================================================================
 * Queries — array
 * ========================================================================= */
/*
 * Each scalar query above has an `_array` counterpart evaluating `n` inputs in
 * one call. `in` and `out` must each hold `n` elements and may alias. `n` == 0
 * is a no-op returning SWMM_OK. All inputs are validated before any result is written; invalid input
 * returns SWMM_ERR_BADPARAM and preserves the output array.
 */

/**
 * @brief Evaluate swmm_xsect_area_of_depth() for an entire input array.
 *
 * Validates every input before writing output; an invalid input preserves the output array. Uses
 * the same kernels, clamping and display-unit conversions as the scalar query. No new handle or
 * caller buffer is allocated.
 *
 * @param[in] xsect Non-null live standalone cross-section handle; do not free it during a query.
 *     Units belong to the handle and are independent of later model edits.
 * @param[in] depth Borrowed n-element input array in display length. Each element must be finite
 *     and nonnegative; may be NULL only when n=0.
 * @param[in] n Nonnegative element count. Zero is a no-op after validating the handle and permits
 *     null array pointers.
 * @param[out] area Caller-owned n-element destination in display length squared. Same-start
 *     in-place input/output is supported; avoid other overlapping ranges.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null xsect, or SWMM_ERR_BADPARAM for negative n, missing
 *     nonempty arrays or any invalid input.
 */
SWMM_ENGINE_API int swmm_xsect_area_of_depth_array(SWMM_XSect xsect, const double* depth, int n,
                                                   double* area);
/**
 * @brief Evaluate swmm_xsect_width_of_depth() for an entire input array.
 *
 * Validates every input before writing output; an invalid input preserves the output array. Uses
 * the same kernels, clamping and display-unit conversions as the scalar query. No new handle or
 * caller buffer is allocated.
 *
 * @param[in] xsect Non-null live standalone cross-section handle; do not free it during a query.
 *     Units belong to the handle and are independent of later model edits.
 * @param[in] depth Borrowed n-element input array in display length. Each element must be finite
 *     and nonnegative; may be NULL only when n=0.
 * @param[in] n Nonnegative element count. Zero is a no-op after validating the handle and permits
 *     null array pointers.
 * @param[out] width Caller-owned n-element destination in display length. Same-start in-place
 *     input/output is supported; avoid other overlapping ranges.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null xsect, or SWMM_ERR_BADPARAM for negative n, missing
 *     nonempty arrays or any invalid input.
 */
SWMM_ENGINE_API int swmm_xsect_width_of_depth_array(SWMM_XSect xsect, const double* depth, int n,
                                                    double* width);
/**
 * @brief Evaluate swmm_xsect_hydrad_of_depth() for an entire input array.
 *
 * Validates every input before writing output; an invalid input preserves the output array. Uses
 * the same kernels, clamping and display-unit conversions as the scalar query. No new handle or
 * caller buffer is allocated.
 *
 * @param[in] xsect Non-null live standalone cross-section handle; do not free it during a query.
 *     Units belong to the handle and are independent of later model edits.
 * @param[in] depth Borrowed n-element input array in display length. Each element must be finite
 *     and nonnegative; may be NULL only when n=0.
 * @param[in] n Nonnegative element count. Zero is a no-op after validating the handle and permits
 *     null array pointers.
 * @param[out] hydrad Caller-owned n-element destination in display length. Same-start in-place
 *     input/output is supported; avoid other overlapping ranges.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null xsect, or SWMM_ERR_BADPARAM for negative n, missing
 *     nonempty arrays or any invalid input.
 */
SWMM_ENGINE_API int swmm_xsect_hydrad_of_depth_array(SWMM_XSect xsect, const double* depth, int n,
                                                     double* hydrad);
/**
 * @brief Evaluate swmm_xsect_depth_of_area() for an entire input array.
 *
 * Validates every input before writing output; an invalid input preserves the output array. Uses
 * the same kernels, clamping and display-unit conversions as the scalar query. No new handle or
 * caller buffer is allocated.
 *
 * @param[in] xsect Non-null live standalone cross-section handle; do not free it during a query.
 *     Units belong to the handle and are independent of later model edits.
 * @param[in] area Borrowed n-element input array in display length squared. Each element must be
 *     finite and nonnegative; may be NULL only when n=0.
 * @param[in] n Nonnegative element count. Zero is a no-op after validating the handle and permits
 *     null array pointers.
 * @param[out] depth Caller-owned n-element destination in display length. Same-start in-place
 *     input/output is supported; avoid other overlapping ranges.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null xsect, or SWMM_ERR_BADPARAM for negative n, missing
 *     nonempty arrays or any invalid input.
 */
SWMM_ENGINE_API int swmm_xsect_depth_of_area_array(SWMM_XSect xsect, const double* area, int n,
                                                   double* depth);
/**
 * @brief Evaluate swmm_xsect_hydrad_of_area() for an entire input array.
 *
 * Validates every input before writing output; an invalid input preserves the output array. Uses
 * the same kernels, clamping and display-unit conversions as the scalar query. No new handle or
 * caller buffer is allocated.
 *
 * @param[in] xsect Non-null live standalone cross-section handle; do not free it during a query.
 *     Units belong to the handle and are independent of later model edits.
 * @param[in] area Borrowed n-element input array in display length squared. Each element must be
 *     finite and nonnegative; may be NULL only when n=0.
 * @param[in] n Nonnegative element count. Zero is a no-op after validating the handle and permits
 *     null array pointers.
 * @param[out] hydrad Caller-owned n-element destination in display length. Same-start in-place
 *     input/output is supported; avoid other overlapping ranges.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null xsect, or SWMM_ERR_BADPARAM for negative n, missing
 *     nonempty arrays or any invalid input.
 */
SWMM_ENGINE_API int swmm_xsect_hydrad_of_area_array(SWMM_XSect xsect, const double* area, int n,
                                                    double* hydrad);
/**
 * @brief Evaluate swmm_xsect_sectfactor_of_area() for an entire input array.
 *
 * Validates every input before writing output; an invalid input preserves the output array. Uses
 * the same kernels, clamping and display-unit conversions as the scalar query. No new handle or
 * caller buffer is allocated.
 *
 * @param[in] xsect Non-null live standalone cross-section handle; do not free it during a query.
 *     Units belong to the handle and are independent of later model edits.
 * @param[in] area Borrowed n-element input array in display length squared. Each element must be
 *     finite and nonnegative; may be NULL only when n=0.
 * @param[in] n Nonnegative element count. Zero is a no-op after validating the handle and permits
 *     null array pointers.
 * @param[out] sf Caller-owned n-element destination in display length^(8/3). Same-start in-place
 *     input/output is supported; avoid other overlapping ranges.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null xsect, or SWMM_ERR_BADPARAM for negative n, missing
 *     nonempty arrays or any invalid input.
 */
SWMM_ENGINE_API int swmm_xsect_sectfactor_of_area_array(SWMM_XSect xsect, const double* area, int n,
                                                        double* sf);
/**
 * @brief Evaluate swmm_xsect_area_of_sectfactor() for an entire input array.
 *
 * Validates every input before writing output; an invalid input preserves the output array. Uses
 * the same kernels, clamping and display-unit conversions as the scalar query. No new handle or
 * caller buffer is allocated.
 *
 * @param[in] xsect Non-null live standalone cross-section handle; do not free it during a query.
 *     Units belong to the handle and are independent of later model edits.
 * @param[in] sf Borrowed n-element input array in display length^(8/3). Each element must be finite
 *     and nonnegative; may be NULL only when n=0.
 * @param[in] n Nonnegative element count. Zero is a no-op after validating the handle and permits
 *     null array pointers.
 * @param[out] area Caller-owned n-element destination in display length squared. Same-start
 *     in-place input/output is supported; avoid other overlapping ranges.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null xsect, or SWMM_ERR_BADPARAM for negative n, missing
 *     nonempty arrays or any invalid input.
 */
SWMM_ENGINE_API int swmm_xsect_area_of_sectfactor_array(SWMM_XSect xsect, const double* sf, int n,
                                                        double* area);
/**
 * @brief Evaluate swmm_xsect_dsda() for an entire input array.
 *
 * Validates every input before writing output; an invalid input preserves the output array. Uses
 * the same kernels, clamping and display-unit conversions as the scalar query. No new handle or
 * caller buffer is allocated.
 *
 * @param[in] xsect Non-null live standalone cross-section handle; do not free it during a query.
 *     Units belong to the handle and are independent of later model edits.
 * @param[in] area Borrowed n-element input array in display length squared. Each element must be
 *     finite and nonnegative; may be NULL only when n=0.
 * @param[in] n Nonnegative element count. Zero is a no-op after validating the handle and permits
 *     null array pointers.
 * @param[out] dsda Caller-owned n-element destination in display length^(2/3). Same-start in-place
 *     input/output is supported; avoid other overlapping ranges.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null xsect, or SWMM_ERR_BADPARAM for negative n, missing
 *     nonempty arrays or any invalid input.
 */
SWMM_ENGINE_API int swmm_xsect_dsda_array(SWMM_XSect xsect, const double* area, int n,
                                          double* dsda);
/**
 * @brief Evaluate swmm_xsect_critical_depth() for an entire input array.
 *
 * Validates every input before writing output; an invalid input preserves the output array. Uses
 * the same kernels, clamping and display-unit conversions as the scalar query. No new handle or
 * caller buffer is allocated.
 *
 * @param[in] xsect Non-null live standalone cross-section handle; do not free it during a query.
 *     Units belong to the handle and are independent of later model edits.
 * @param[in] flow Borrowed n-element input array in handle flow units (query
 *     swmm_xsect_get_units()). Each element must be finite and nonnegative; may be NULL only when
 *     n=0.
 * @param[in] n Nonnegative element count. Zero is a no-op after validating the handle and permits
 *     null array pointers.
 * @param[out] ycrit Caller-owned n-element destination in display length. Same-start in-place
 *     input/output is supported; avoid other overlapping ranges.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null xsect, or SWMM_ERR_BADPARAM for negative n, missing
 *     nonempty arrays or any invalid input.
 */
SWMM_ENGINE_API int swmm_xsect_critical_depth_array(SWMM_XSect xsect, const double* flow, int n,
                                                    double* ycrit);

#ifdef __cplusplus
}
#endif

#endif /* OPENSWMM_XSECT_H */
