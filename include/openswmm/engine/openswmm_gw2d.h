/* SPDX-License-Identifier: Apache-2.0
 *
 * Copyright 2026 Caleb Buahin
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * @file      openswmm_gw2d.h
 * @brief     G-step 19 — the two-zone groundwater kernel's C API: authoring the
 *        `[2D_AQUIFER*]` rows, and reading the state and ledger while a run
 *        is in flight.
 *
 * @details The API has two halves and they obey different rules.
 *
 *          **Authoring** (`swmm_gw2d_option_*`, `swmm_gw2d_row_*`,
 *          `swmm_gw2d_node_*`) edits the rows the kernel is built from, so it
 *          requires BUILDING or OPENED — exactly like the 2D boundary and
 *          infiltration APIs. A mid-run edit would be silently ignored, which
 *          is worse than being refused. Values are in the PROJECT's units
 *          (see SubsurfaceSections.hpp's table); the API does not convert,
 *          because a host that sets what it read back must get the same
 *          number.
 *
 *          **State** (`swmm_gw2d_get_*`) reads the running kernel and is
 *          available whenever a run has started. Everything it returns is
 *          **SI** — metres, seconds, m³ — because that is what the kernel
 *          holds and converting on the way out would make a reader's units
 *          depend on the project's, which is precisely the bug class the
 *          engine's 1D↔2D seam already had once.
 *
 *          **Cell-generic.** Every index is a CELL (`0 .. cell_count−1` here;
 *          1-based in the `.inp`). Nothing assumes three edges.
 *
 * @ingroup engine_2d
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_GW2D_H
#define OPENSWMM_GW2D_H

#include "openswmm_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Scopes (same encoding as the [GW_*] transport API) ------------------ */

#define SWMM_GW2D_SCOPE_GLOBAL 0 /**< the `*` row */
#define SWMM_GW2D_SCOPE_TAG 1    /**< every cell carrying a tag */
#define SWMM_GW2D_SCOPE_CELL 2   /**< one cell, 0-based here */

/* ---- Soil-characteristic laws ------------------------------------------- */

#define SWMM_GW2D_SOIL_RUSSO 0
#define SWMM_GW2D_SOIL_GARDNER 1
#define SWMM_GW2D_SOIL_BROOKS_COREY 2
#define SWMM_GW2D_SOIL_VAN_GENUCHTEN 3

/* ---- Unsaturated-zone closures ------------------------------------------ */

#define SWMM_GW2D_CLOSURE_AUTO (-1)
#define SWMM_GW2D_CLOSURE_CLOSED_FORM 0
#define SWMM_GW2D_CLOSURE_ENSLAVED 1
#define SWMM_GW2D_CLOSURE_SIGMA 2

/* ---- Exterior hydraulic boundaries ------------------------------------- */
#define SWMM_GW2D_BOUNDARY_HEAD 0
#define SWMM_GW2D_BOUNDARY_FLOW 1
/** Authored HEAD is project-length elevation; FLOW is total signed m3/s,
 * positive into the aquifer. cell/edge are zero-based. A nonempty series
 * supplied to set/get replaces the constant value. Edits require BUILDING/OPENED. */
typedef struct SWMM_GwBoundary {
    int cell, edge, kind;
    double value;
} SWMM_GwBoundary;
/**
 * @brief Read the authored exterior aquifer-boundary count.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 *
 * @return Number of authored boundaries, or SWMM_ERR_BADHANDLE for a null engine.
 */
SWMM_ENGINE_API int swmm_gw2d_boundary_count(SWMM_Engine engine);
/**
 * @brief Copy one authored exterior aquifer boundary and series name.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] index Zero-based authored boundary index.
 * @param[out] row Required destination for the complete boundary row in authored units.
 * @param[out] series Required destination for the complete series name, including NUL.
 * @param[in] series_len Positive byte capacity; a short buffer is refused without truncation.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE for a null engine, or SWMM_ERR_BADPARAM for invalid
 * arguments. Invalid boundary indices return SWMM_ERR_BADINDEX; outputs remain unchanged on
 * error.
 */
SWMM_ENGINE_API int swmm_gw2d_boundary_get(SWMM_Engine engine, int index, SWMM_GwBoundary* row,
                                           char* series, int series_len);
/**
 * @brief Upsert an authored exterior aquifer boundary by cell and edge.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] row Required row; cell >= 0, edge in [0,3], kind HEAD or FLOW,
 * and finite value. Mesh-local edge existence is resolved when initialized.
 * @param[in] series Optional copied series name; NULL means constant forcing.
 * Newlines, carriage returns and quote characters are rejected.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE for a null engine, or SWMM_ERR_BADPARAM for invalid
 * arguments.
 * @note Requires an editable BUILDING or OPENED model; later states return
 * SWMM_ERR_LIFECYCLE. Input data is copied and no input storage is retained.
 */
SWMM_ENGINE_API int swmm_gw2d_boundary_set(SWMM_Engine engine, const SWMM_GwBoundary* row,
                                           const char* series);
/**
 * @brief Erase one authored exterior aquifer boundary.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] index Zero-based authored boundary to erase; later indices shift down.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE for a null engine, or SWMM_ERR_BADPARAM for invalid
 * arguments. Out-of-range indices return SWMM_ERR_BADINDEX.
 * @note Requires an editable BUILDING or OPENED model; later states return
 * SWMM_ERR_LIFECYCLE. Input data is copied and no input storage is retained.
 */
SWMM_ENGINE_API int swmm_gw2d_boundary_remove(SWMM_Engine engine, int index);
/**
 * @brief Runtime totals, in SI m3; getters leave outputs unchanged on error.
 *
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] index Zero-based resolved runtime boundary index.
 * @param[out] requested_in Required destination for the cumulative requested in total.
 * @param[out] requested_out Required destination for the cumulative requested out total.
 * @param[out] applied_in Required destination for the cumulative applied in total.
 * @param[out] applied_out Required destination for the cumulative applied out total.
 *
 * @return SWMM_OK, SWMM_ERR_BADHANDLE for a null engine, or SWMM_ERR_BADPARAM for invalid
 * arguments. Invalid boundary/species indices return SWMM_ERR_BADINDEX; all outputs remain
 * unchanged on error.
 * @note Requires an active aquifer runtime; otherwise returns SWMM_ERR_LIFECYCLE. Does not
 * advance time.
 */
SWMM_ENGINE_API int swmm_gw2d_boundary_water(SWMM_Engine engine, int index, double* requested_in,
                                             double* requested_out, double* applied_in,
                                             double* applied_out);
/**
 * @brief Runtime totals in native concentration*m3 (age s*m3, temperature degC*m3).
 * advective in/out and independently imposed flux in/out are separate.
 *
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] index Zero-based resolved runtime boundary index.
 * @param[in] species Zero-based transported species index.
 * @param[out] advective_in Required destination for the cumulative advective in total.
 * @param[out] advective_out Required destination for the cumulative advective out total.
 * @param[out] imposed_in Required destination for the cumulative imposed in total.
 * @param[out] imposed_out Required destination for the cumulative imposed out total.
 *
 * @return SWMM_OK, SWMM_ERR_BADHANDLE for a null engine, or SWMM_ERR_BADPARAM for invalid
 * arguments. Invalid boundary/species indices return SWMM_ERR_BADINDEX; all outputs remain
 * unchanged on error.
 * @note Requires an active aquifer runtime; otherwise returns SWMM_ERR_LIFECYCLE. Does not
 * advance time.
 */
SWMM_ENGINE_API int swmm_gw2d_boundary_species(SWMM_Engine engine, int index, int species,
                                               double* advective_in, double* advective_out,
                                               double* imposed_in, double* imposed_out);

/* ---- Per-cell state selectors ------------------------------------------- */

#define SWMM_GW2D_VAR_HG 0       /**< saturated thickness (m) */
#define SWMM_GW2D_VAR_HU 1       /**< unsaturated storage (m of water) */
#define SWMM_GW2D_VAR_TABLE_EL 2 /**< water-table elevation (m) */
#define SWMM_GW2D_VAR_Q0 3       /**< recharge (m/s), + down */
#define SWMM_GW2D_VAR_QLAT 4     /**< net lateral Darcy in (m3/s) */
#define SWMM_GW2D_VAR_QNODE 5    /**< node exchange (m3/s), + out */
#define SWMM_GW2D_VAR_QDEEP 6    /**< deep loss (m/s) */
#define SWMM_GW2D_VAR_QET 7      /**< subsurface ET (m/s) */
#define SWMM_GW2D_VAR_DUNNE 8    /**< saturation excess to surface (m3/s) */
#define SWMM_GW2D_VAR_QPLUS 9    /**< infiltration delivered in (m/s) */
#define SWMM_GW2D_VAR_DT_CELL 10 /**< min(dt_g, dt_u) (s) */
#define SWMM_GW2D_VAR_TIER 11    /**< assigned LTS tier */
#define SWMM_GW2D_VAR_CLOSURE 12 /**< resolved closure (AUTO already gone) */
#define SWMM_GW2D_VAR_QLINK 13   /**< G-X3: conduit seepage delivered in (m3/s) */

#define SWMM_GW2D_VAR_INFIL_CAPACITY 14 /**< Aquifer top-interface receiving capacity (m s-1) */
#define SWMM_GW2D_VAR_INFIL_REMAINING                                                              \
    15 /**< Remaining receiving allowance after pending receipts (m3) */
#define SWMM_GW2D_VAR_INFIL_REFRESH 16 /**< Capacity refresh time since simulation start (s) */
#define SWMM_GW2D_VAR_INFIL_PENDING 17 /**< Reserved surface water pending aquifer firing (m3) */
#define SWMM_GW2D_VAR_REJECT 18 /**< Rejected top-interface delivery returned to surface (m3 s-1) */
#define SWMM_GW2D_VAR_REJECT_CUM 19 /**< Cumulative rejected top-interface delivery (m3) */
#define SWMM_GW2D_VAR_DUNNE_CUM 20  /**< Cumulative physical saturation excess (m3) */
#define SWMM_GW2D_VAR_ET_PENDING                                                                   \
    21 /**< Pending past atmospheric demand after actual surface evaporation (m3) */
#define SWMM_GW2D_VAR_ET_POTENTIAL_CUM                                                             \
    22 /**< Cumulative single mesh-area potential atmospheric demand (m3) */
#define SWMM_GW2D_VAR_ET_SURFACE_CUM 23 /**< Cumulative actual surface evaporation (m3) */
#define SWMM_GW2D_VAR_ET_SOIL_CUM 24    /**< Cumulative actual soil ET (m3) */
#define SWMM_GW2D_VAR_ET_UNUSED_CUM 25  /**< Cumulative expired unused soil demand (m3) */
#define SWMM_GW2D_VAR_ET_SURFACE                                                                   \
    26 /**< Actual surface evaporation at last surface interval (m s-1) */
#define SWMM_GW2D_VAR_ET_POTENTIAL 27 /**< Potential demand at last surface interval (m s-1) */
#define SWMM_GW2D_VAR_ET_STRESS 28    /**< Single extraction-site moisture stress factor (1) */
#define SWMM_GW2D_VAR_ET_REFRESH 29   /**< Last completed demand interval end since start (s) */

/* ---- Ledger selectors (m3, cumulative) ---------------------------------- */

#define SWMM_GW2D_LED_RECHARGE 0
#define SWMM_GW2D_LED_LATERAL 1
#define SWMM_GW2D_LED_DEEP 2
#define SWMM_GW2D_LED_NODE 3
#define SWMM_GW2D_LED_REJECT 13 /**< rejected infiltration only (m3) */
#define SWMM_GW2D_LED_DUNNE 4
#define SWMM_GW2D_LED_CAPRISE 5
#define SWMM_GW2D_LED_ET 6
#define SWMM_GW2D_LED_INFIL_IN 7
#define SWMM_GW2D_LED_INIT_STORAGE 8
#define SWMM_GW2D_LED_STORAGE 9       /**< storage NOW, incl. accumulators */
#define SWMM_GW2D_LED_SOURCE_IN 11    /**< Named source injection, m3. */
#define SWMM_GW2D_LED_BOUNDARY_IN 14  /**< Gross boundary inflow; included in LATERAL. */
#define SWMM_GW2D_LED_BOUNDARY_OUT 15 /**< Gross boundary outflow; included in LATERAL. */
#define SWMM_GW2D_LED_SOURCE_OUT 12   /**< Actual availability-limited extraction, m3. */
#define SWMM_GW2D_LED_LINK 10         /**< G-X3: conduit seepage delivered in */

/* ---- T7.5: the transported tuple (read-only results) --------------------- */

/** Zone selector for the species accessors. */
#define SWMM_GW2D_ZONE_SAT 0   /**< the saturated zone */
#define SWMM_GW2D_ZONE_UNSAT 1 /**< the unsaturated column store */

/** Species ledger terms — the `groundwater_species_ledger` order. */
#define SWMM_GW2D_SPL_INIT 0
#define SWMM_GW2D_SPL_STORAGE 1 /**< ledgered storage NOW */
#define SWMM_GW2D_SPL_INFIL_IN 2
#define SWMM_GW2D_SPL_NODE_IN 3
#define SWMM_GW2D_SPL_LINK_IN 4
#define SWMM_GW2D_SPL_LATERAL_NET 5
#define SWMM_GW2D_SPL_DEEP_OUT 6
#define SWMM_GW2D_SPL_NODE_OUT 7
#define SWMM_GW2D_SPL_LINK_OUT 8
#define SWMM_GW2D_SPL_DUNNE_OUT 9
#define SWMM_GW2D_SPL_ET_OUT 10
#define SWMM_GW2D_SPL_REACTED 11
#define SWMM_GW2D_SPL_SOURCE_IN 13  /**< Native concentration times m3. */
#define SWMM_GW2D_SPL_SOURCE_OUT 14 /**< Extracted dissolved species. */
#define SWMM_GW2D_SPL_BOUNDARY_IN 15
#define SWMM_GW2D_SPL_BOUNDARY_OUT 16
#define SWMM_GW2D_SPL_BOUNDARY_FLUX_IN 17
#define SWMM_GW2D_SPL_BOUNDARY_FLUX_OUT 18
#define SWMM_GW2D_SPL_AGE_GENERATED 19
#define SWMM_GW2D_SPL_RESIDUAL 12 /**< storage + out - in - init */

/**
 * @brief How many species rows the aquifer transports (0 when it carries none).
 *
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[out] count Required species-count destination; zero when transport is inactive.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE for a null engine, or SWMM_ERR_BADPARAM for invalid
 * arguments.
 * @note Requires an active aquifer runtime; otherwise returns SWMM_ERR_LIFECYCLE. Does not
 * advance time.
 */
SWMM_ENGINE_API int swmm_gw2d_species_count(SWMM_Engine engine, int* count);

/**
 * @brief One species row's name (TransportPolicy order: pollutants, MSX,
 *  `__WATER_AGE__`, `__TEMPERATURE__`).
 *
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] species Zero-based transported species index.
 * @param[out] buf Required writable name destination; safely truncated and NUL-terminated.
 * @param[in] buflen Positive byte capacity including the terminator.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE for a null engine, or SWMM_ERR_BADPARAM for invalid
 * arguments. Invalid species indices return SWMM_ERR_BADINDEX.
 * @note Requires an active aquifer runtime; otherwise returns SWMM_ERR_LIFECYCLE. Does not
 * advance time.
 */
SWMM_ENGINE_API int swmm_gw2d_species_name(SWMM_Engine engine, int species, char* buf, int buflen);

/**
 * @brief Per-cell CONCENTRATION of one species in one zone, in the species' own
 *  units. BULK retains total inventory / water volume (zero when dry).
 *  LAYERED uses aqueous plus sorption capacity for a dissolved equivalent.
 *  Writes at most @p len values and sets @p written.
 *
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] zone SWMM_GW2D_ZONE_SAT or SWMM_GW2D_ZONE_UNSAT.
 * @param[in] species Zero-based transported species index.
 * @param[out] out Required writable array, in the species native concentration units.
 * @param[in] len Positive capacity in doubles; partial cell reads are permitted.
 * @param[out] written Optional count destination, min(len, transport cell count).
 * @return SWMM_OK, SWMM_ERR_BADHANDLE for a null engine, or SWMM_ERR_BADPARAM for invalid
 * arguments. Inactive transport or invalid species returns SWMM_ERR_BADINDEX.
 * @note Requires an active aquifer runtime; otherwise returns SWMM_ERR_LIFECYCLE. Does not
 * advance time.
 */
SWMM_ENGINE_API int swmm_gw2d_get_cell_conc(SWMM_Engine engine, int zone, int species, double* out,
                                            int len, int* written);

/**
 * @brief Canonical SIGMA chemistry, zero-based layer and species. Outputs are
 * unchanged on error; BULK chemistry returns SWMM_ERR_BADPARAM. Mass uses
 * native concentration times m3, water uses m3, concentration is dissolved.
 *
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] cell Zero-based covered cell index.
 * @param[in] layer Zero-based unsaturated chemistry layer, surface first.
 * @param[in] species Zero-based transported species index.
 * @param[out] water Required layer-water destination, m3.
 * @param[out] mass Required total inventory destination, native concentration times m3.
 * @param[out] concentration Required dissolved equivalent concentration destination;
 * uses water plus sorption capacity, or zero if that capacity is nonpositive.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE for a null engine, or SWMM_ERR_BADPARAM for invalid
 * arguments. Invalid cell/layer/species returns SWMM_ERR_BADINDEX.
 * @note Requires an active aquifer runtime; otherwise returns SWMM_ERR_LIFECYCLE. Does not
 * advance time.
 */
SWMM_ENGINE_API int swmm_gw2d_get_layer_quality(SWMM_Engine engine, int cell, int layer,
                                                int species, double* water, double* mass,
                                                double* concentration);

/**
 * @brief One term of one species' cumulative ledger — see SWMM_GW2D_SPL_*.
 *
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] species Zero-based active transported species index.
 * @param[in] term SWMM_GW2D_SPL_* selector.
 * @param[out] value Required destination, native concentration times m3;
 * water-age uses s*m3 and temperature uses degrees C*m3.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE for a null engine, or SWMM_ERR_BADPARAM for invalid
 * arguments. Inactive transport or an invalid species returns SWMM_ERR_BADINDEX.
 * @note Requires an active aquifer runtime; otherwise returns SWMM_ERR_LIFECYCLE. Does not
 * advance time.
 */
SWMM_ENGINE_API int swmm_gw2d_get_species_ledger(SWMM_Engine engine, int species, int term,
                                                 double* value);

/* =========================================================================
 * [2D_AQUIFER_OPTIONS]
 * ========================================================================= */

/**
 * @brief Read one option as text. Keys: SOIL_CHAR, CLOSURE, M_LAYERS,
 *  CAPILLARY_DIFF, C_GW, C_COL, FORCE_CLOSED_FORM, MODE, DUNNE, GW_ET,
 *  NODE_ENROLMENT (AUTO | ROWS), LINK_SEEPAGE (DEFAULT | ONE_WAY | TWO_WAY | NONE),
 *  WILTING_SUCTION (AUTO | positive project length). Read-only keys:
 *  GW_ET_EFFECTIVE, LINK_SEEPAGE_EFFECTIVE, WILTING_SUCTION_EFFECTIVE,
 *  CONFIGURED and OPTIONS_AUTHORED. Legacy LINK_SEEPAGE AUTO means ONE_WAY.
 *
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] key Required case-insensitive null-terminated option key from the list above.
 * @param[out] buf Required destination; positive capacity is NUL-terminated and may truncate.
 * @param[in] buflen Positive destination capacity in bytes, including NUL.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE for a null engine, or SWMM_ERR_BADPARAM for invalid
 * arguments.
 */
SWMM_ENGINE_API int swmm_gw2d_option_get(SWMM_Engine engine, const char* key, char* buf,
                                         int buflen);

/**
 * @brief Atomically replace GW_ET, LINK_SEEPAGE and WILTING_SUCTION before a run.
 * GW_ET AUTO defaults to BOTH on a mesh, NONE in PER_SUBCATCH.
 * LINK_SEEPAGE DEFAULT defaults to TWO_WAY on a mesh; ONE_WAY and legacy
 * AUTO retain explicit one-way delivery. WILTING_SUCTION AUTO means 150 m;
 * a custom positive number is in project length units. authored must be 0
 * or 1 and restores section provenance for atomic GUI Undo.
 *
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] et Required GW_ET parser value: AUTO, NONE, CAPILLARY_RISE, BOUNDARY_ET or BOTH.
 * @param[in] link Required LINK_SEEPAGE parser value: DEFAULT, NONE, ONE_WAY, TWO_WAY or legacy
 * AUTO.
 * @param[in] wilting Required AUTO or a positive project-length suction string.
 * @param[in] authored Exactly 0 or 1, restoring whether the options section was authored.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE for a null engine, or SWMM_ERR_BADPARAM for invalid
 * arguments.
 * @note Requires an editable BUILDING or OPENED model; later states return
 * SWMM_ERR_LIFECYCLE. Input data is copied and no input storage is retained.
 */
SWMM_ENGINE_API int swmm_gw2d_process_options_set(SWMM_Engine engine, const char* et,
                                                  const char* link, const char* wilting,
                                                  int authored);

/**
 * @brief Set an authored option from text through the same parser as the file.
 *
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] key Required case-insensitive writable option key.
 * @param[in] value Required null-terminated input-file value; invalid key/value text is refused.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE for a null engine, or SWMM_ERR_BADPARAM for invalid
 * arguments.
 * @note Requires an editable BUILDING or OPENED model; later states return
 * SWMM_ERR_LIFECYCLE. Input data is copied and no input storage is retained.
 */
SWMM_ENGINE_API int swmm_gw2d_option_set(SWMM_Engine engine, const char* key, const char* value);

/* =========================================================================
 * [2D_AQUIFER] rows
 * ========================================================================= */

/**
 * @brief Number of authored rows.
 *
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[out] count Required destination for the current row count.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE for a null engine, or SWMM_ERR_BADPARAM for invalid
 * arguments.
 */
SWMM_ENGINE_API int swmm_gw2d_row_count(SWMM_Engine engine, int* count);

/**
 * @brief Append one row. Values are in the PROJECT's units.
 *
 * @param[in] scope   SWMM_GW2D_SCOPE_*
 * @param[in] tag     tag name for SCOPE_TAG, else ignored (may be NULL)
 * @param[in] cell    0-based cell for SCOPE_CELL, else ignored
 * @param[in] ks      saturated conductivity (in/hr | mm/hr)
 * @param[in] zs      soil column thickness (ft | m)
 * @param[in] theta_s porosity
 * @param[in] theta_r residual water content
 * @param[in] alpha   sorptive number (1/ft | 1/m)
 *
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @note ks, zs and alpha must be positive; 0 < theta_s <= 1 and
 * 0 <= theta_r < theta_s. TAG requires a nonempty tag; CELL requires cell >= 0.
 * Appends a row and marks aquifer options authored.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE for a null engine, or SWMM_ERR_BADPARAM for invalid
 * arguments.
 * @note Requires an editable BUILDING or OPENED model; later states return
 * SWMM_ERR_LIFECYCLE. Input data is copied and no input storage is retained.
 */
SWMM_ENGINE_API int swmm_gw2d_row_add(SWMM_Engine engine, int scope, const char* tag, int cell,
                                      double ks, double zs, double theta_s, double theta_r,
                                      double alpha);

/**
 * @brief Read row @p index back, in the units it was authored in. Any out pointer
 *  may be NULL. @p tag needs @p taglen bytes.
 *
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] index Zero-based authored aquifer row index.
 * @param[out] scope Optional scope-code destination.
 * @param[out] tag Optional tag string; positive capacity truncates safely with NUL.
 * @param[in] taglen Tag capacity in bytes; nonpositive capacity skips the string.
 * @param[out] cell Optional cell-index destination, -1 for non-CELL rows.
 * @param[out] ks Optional conductivity in in/hr or mm/hr.
 * @param[out] zs Optional soil thickness in ft or m.
 * @param[out] theta_s Optional dimensionless porosity.
 * @param[out] theta_r Optional dimensionless residual water content.
 * @param[out] alpha Optional sorptive number in inverse ft or inverse m.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE for a null engine, or SWMM_ERR_BADPARAM for invalid
 * arguments.
 */
SWMM_ENGINE_API int swmm_gw2d_row_get(SWMM_Engine engine, int index, int* scope, char* tag,
                                      int taglen, int* cell, double* ks, double* zs,
                                      double* theta_s, double* theta_r, double* alpha);

/**
 * @brief Set one of a row's optional properties. Keys: PSI_B, LAMBDA, N, L,
 *  C_LOSS, HG0, SOIL_CHAR, CLOSURE, M_LAYERS. The numeric-valued keys take
 *  @p value; SOIL_CHAR and CLOSURE take the SWMM_GW2D_SOIL_* /
 *  SWMM_GW2D_CLOSURE_* codes as a double, which keeps this one entry point
 *  instead of nine.
 *
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] index Zero-based authored aquifer row index.
 * @param[in] key Required case-insensitive property key.
 * @param[in] value New authored value. PSI_B and HG0 use project length;
 * LAMBDA, N, L and C_LOSS are dimensionless; law/closure/layer selectors are numeric codes.
 * PSI_B and C_LOSS must be nonnegative, LAMBDA positive, N > 1; rounded M_LAYERS is [2,128],
 * SOIL_CHAR [0,3], and CLOSURE [-1,2].
 * @return SWMM_OK, SWMM_ERR_BADHANDLE for a null engine, or SWMM_ERR_BADPARAM for invalid
 * arguments.
 * @note Requires an editable BUILDING or OPENED model; later states return
 * SWMM_ERR_LIFECYCLE. Input data is copied and no input storage is retained.
 */
SWMM_ENGINE_API int swmm_gw2d_row_set_property(SWMM_Engine engine, int index, const char* key,
                                               double value);

/**
 * @brief Read one optional property back.
 *
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] index Zero-based authored aquifer row index.
 * @param[in] key Required case-insensitive property key (also SOIL_CHAR_SET and CLOSURE_SET for
 * provenance).
 * @param[out] value Required result destination for the authored value. PSI_B and HG0 use
 * project length;
 * LAMBDA, N, L and C_LOSS are dimensionless; law/closure/layer selectors are numeric codes.
 * Unset optional properties may contain their parser sentinel values.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE for a null engine, or SWMM_ERR_BADPARAM for invalid
 * arguments.
 */
SWMM_ENGINE_API int swmm_gw2d_row_get_property(SWMM_Engine engine, int index, const char* key,
                                               double* value);

/**
 * @brief Remove row @p index.
 *
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] index Zero-based row row to erase; later rows shift down.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE for a null engine, or SWMM_ERR_BADPARAM for invalid
 * arguments.
 * @note Requires an editable BUILDING or OPENED model; later states return
 * SWMM_ERR_LIFECYCLE. Input data is copied and no input storage is retained.
 */
SWMM_ENGINE_API int swmm_gw2d_row_remove(SWMM_Engine engine, int index);

/* =========================================================================
 * [2D_AQUIFER_NODE] beds
 * ========================================================================= */

/**
 * @brief Read the current aquifer node-bed row count.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[out] count Required destination for the current row count.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE for a null engine, or SWMM_ERR_BADPARAM for invalid
 * arguments.
 */
SWMM_ENGINE_API int swmm_gw2d_node_count(SWMM_Engine engine, int* count);

/**
 * @brief Add a node bed. @p kc in in/hr | mm/hr, @p dc in ft | m, @p area in
 *  ft2 | m2 (0 = the cell's own area). @p kc == 0 means direct Darcy.
 *
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] node Required nonempty copied node name; resolved at initialize.
 * @param[in] cell Nonnegative cell index, or -1 to locate from node coordinates.
 * @param[in] kc Finite nonnegative bed conductivity; zero selects direct Darcy.
 * @param[in] dc Finite nonnegative bed thickness; positive when kc > 0.
 * @param[in] area Finite nonnegative contact area; zero means the entire cell.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE for a null engine, or SWMM_ERR_BADPARAM for invalid
 * arguments.
 * @note Requires an editable BUILDING or OPENED model; later states return
 * SWMM_ERR_LIFECYCLE. Input data is copied and no input storage is retained.
 */
SWMM_ENGINE_API int swmm_gw2d_node_add(SWMM_Engine engine, const char* node, int cell, double kc,
                                       double dc, double area);

/**
 * @brief Read one authored/resolved aquifer node-bed row.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] index Zero-based node-bed row index.
 * @param[out] node Optional destination for the node name; positive capacity safely truncates
 * with NUL.
 * @param[in] nodelen Name capacity in bytes; nonpositive capacity skips the copy.
 * @param[out] cell Optional authored/resolved cell index; -1 denotes unresolved AUTO.
 * @param[out] kc Optional conductivity in project infiltration units, in/hr or mm/hr.
 * @param[out] dc Optional thickness in ft or m.
 * @param[out] area Optional area in ft2 or m2; zero means whole cell.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE for a null engine, or SWMM_ERR_BADPARAM for invalid
 * arguments.
 */
SWMM_ENGINE_API int swmm_gw2d_node_get(SWMM_Engine engine, int index, char* node, int nodelen,
                                       int* cell, double* kc, double* dc, double* area);

/**
 * @brief Erase one aquifer node-bed row and its stored node name.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] index Zero-based node row to erase; later rows shift down.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE for a null engine, or SWMM_ERR_BADPARAM for invalid
 * arguments.
 * @note Requires an editable BUILDING or OPENED model; later states return
 * SWMM_ERR_LIFECYCLE. Input data is copied and no input storage is retained.
 */
SWMM_ENGINE_API int swmm_gw2d_node_remove(SWMM_Engine engine, int index);

/**
 * @brief G-X2 (2026-09-19): a bed's enrolment state. `locate` — the cell is found
 *  from the node's [COORDINATES] at initialize (`CELL AUTO`; pass cell −1 to
 *  swmm_gw2d_node_add); `exchange` — 0 when the row opts the node OUT
 *  (`EXCHANGE NO`); `automatic` — enrolled by NODE_ENROLMENT AUTO rather than
 *  authored (present after initialize only; not written back). Any pointer
 *  may be NULL.
 *
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] index Zero-based node-bed row index.
 * @param[out] locate Optional 0/1 destination for coordinate-based cell location.
 * @param[out] exchange Optional 0/1 destination for enabled exchange.
 * @param[out] automatic Optional 0/1 destination for runtime automatic enrollment.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE for a null engine, or SWMM_ERR_BADPARAM for invalid
 * arguments.
 */
SWMM_ENGINE_API int swmm_gw2d_node_get_flags(SWMM_Engine engine, int index, int* locate,
                                             int* exchange, int* automatic);

/**
 * @brief G-X2: set a row's EXCHANGE flag (0 = the node exchanges with nothing).
 *
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] index Zero-based node-bed row index.
 * @param[in] exchange Zero disables exchange; every nonzero value enables it.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE for a null engine, or SWMM_ERR_BADPARAM for invalid
 * arguments.
 * @note Requires an editable BUILDING or OPENED model; later states return
 * SWMM_ERR_LIFECYCLE. Input data is copied and no input storage is retained.
 */
SWMM_ENGINE_API int swmm_gw2d_node_set_exchange(SWMM_Engine engine, int index, int exchange);

/** Complete authored/resolved bed snapshot in project units. locate preserves
 * CELL AUTO even after it has resolved to a nonnegative cell. automatic rows
 * may be retained unchanged; they cannot be fabricated by this edit API. */
typedef struct SWMM_GwNodeBed {
    const char* node;
    int cell, locate, exchange, automatic;
    double kc, dc, area;
} SWMM_GwNodeBed;
/** Compare-and-replace a per-node owner as part of the same bed transaction.
 * Modes are SWMM_LID_BOTTOM_* from openswmm_infrastructure.h. */
typedef struct SWMM_LidNodeBottomEdit {
    int node, expected_mode, mode;
} SWMM_LidNodeBottomEdit;
/**
 * @brief BUILDING/OPENED only. Compares the entire current bed list with expected,
 * validates every replacement and owner edit, then publishes once. Stale,
 * duplicate, unknown, nonfinite or contradictory input returns BADPARAM and
 * changes nothing. Mesh ownership requires an enabled MESH aquifer and an
 * enabled authored bed with positive AREA; mapping is checked at initialize.
 * Mesh execution remains gated until R5 numerical/restart qualification.
 * Existing AREA=0 retains whole-cell semantics. NULL arrays require count=0.
 * apply=0 validates a private draft only; apply=1 publishes it.
 *
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] expected Borrowed expected_count-element exact snapshot of the current ordered beds.
 * @param[in] expected_count Nonnegative expected row count; must match current configuration.
 * @param[in] replacement Borrowed count-element replacement list; names must resolve uniquely.
 * @param[in] count Nonnegative replacement row count.
 * @param[in] bottoms Borrowed bottom_count-element LID owner edit array; expected modes must match.
 * @param[in] bottom_count Nonnegative number of unique storage-node owner edits.
 * @param[in] apply 0 validates only; 1 atomically publishes the draft.
 * @note Inputs are not retained. Automatic beds must remain exact existing snapshots.
 * Allocation failures return SWMM_ERR_INTERNAL before publication.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE for a null engine, or SWMM_ERR_BADPARAM for invalid
 * arguments.
 * @note Requires an editable BUILDING or OPENED model; later states return
 * SWMM_ERR_LIFECYCLE. Input data is copied and no input storage is retained.
 */
SWMM_ENGINE_API int swmm_gw2d_node_replace(SWMM_Engine engine, const SWMM_GwNodeBed* expected,
                                           int expected_count, const SWMM_GwNodeBed* replacement,
                                           int count, const SWMM_LidNodeBottomEdit* bottoms,
                                           int bottom_count, int apply);

/* =========================================================================
 * [2D_AQUIFER_LINKS] conduit beds (closed C-API gap 2026-10-11)
 * ========================================================================= */

/**
 * @brief Read the authored conduit-bed row count.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[out] count Required destination for the current row count.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE for a null engine, or SWMM_ERR_BADPARAM for invalid
 * arguments.
 */
SWMM_ENGINE_API int swmm_gw2d_link_count(SWMM_Engine engine, int* count);

/**
 * @brief Add a conduit bed row. @p kc in the [LOSSES] seepage units (0 = the
 *  conduit's own seep rate), @p dc in ft | m (0 = the conduit's full depth);
 *  both >= 0. @p exchange 0 keeps the conduit a legacy loss (EXCHANGE NO).
 *  The link name resolves at initialize, as for node beds.
 *
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] link Required nonempty copied conduit name, resolved at initialize.
 * @param[in] kc Nonnegative seepage conductivity in project LOSSES units; zero inherits the
 * conduit.
 * @param[in] dc Nonnegative thickness in ft or m; zero inherits full conduit depth.
 * @param[in] exchange Zero disables aquifer exchange; nonzero enables it.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE for a null engine, or SWMM_ERR_BADPARAM for invalid
 * arguments.
 * @note Requires an editable BUILDING or OPENED model; later states return
 * SWMM_ERR_LIFECYCLE. Input data is copied and no input storage is retained.
 */
SWMM_ENGINE_API int swmm_gw2d_link_add(SWMM_Engine engine, const char* link, double kc, double dc,
                                       int exchange);

/**
 * @brief Read one authored aquifer conduit-bed row.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] index Zero-based conduit-bed row index.
 * @param[out] link Optional name destination; positive capacity truncates safely with NUL.
 * @param[in] linklen Name capacity in bytes; nonpositive capacity skips the string.
 * @param[out] kc Optional conductivity in authored project LOSSES units.
 * @param[out] dc Optional thickness in authored project length units.
 * @param[out] exchange Optional enabled-exchange flag, 0 or 1.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE for a null engine, or SWMM_ERR_BADPARAM for invalid
 * arguments.
 */
SWMM_ENGINE_API int swmm_gw2d_link_get(SWMM_Engine engine, int index, char* link, int linklen,
                                       double* kc, double* dc, int* exchange);

/**
 * @brief Erase one authored aquifer conduit-bed row.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] index Zero-based link row to erase; later rows shift down.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE for a null engine, or SWMM_ERR_BADPARAM for invalid
 * arguments.
 * @note Requires an editable BUILDING or OPENED model; later states return
 * SWMM_ERR_LIFECYCLE. Input data is copied and no input storage is retained.
 */
SWMM_ENGINE_API int swmm_gw2d_link_remove(SWMM_Engine engine, int index);

/* =========================================================================
 * Running state — SI throughout
 * ========================================================================= */

/**
 * @brief Non-zero once a `[2D_AQUIFER]` row has resolved and the kernel is live.
 *
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[out] active Required result, 1 for a live aquifer kernel or 0 otherwise.
 * @note Safe before initialization; does not require an active runtime.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE for a null engine, or SWMM_ERR_BADPARAM for invalid
 * arguments.
 */
SWMM_ENGINE_API int swmm_gw2d_is_active(SWMM_Engine engine, int* active);

/**
 * @brief Cells the kernel covers, and the sigma layer count (1 when no cell uses
 *  closure B). Either pointer may be NULL.
 *
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[out] n_cells Optional destination for the covered cell count.
 * @param[out] m_layers Optional destination for sigma layer count, or 1 without SIGMA.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE for a null engine, or SWMM_ERR_BADPARAM for invalid
 * arguments.
 * @note Requires an active aquifer runtime; otherwise returns SWMM_ERR_LIFECYCLE. Does not
 * advance time.
 */
SWMM_ENGINE_API int swmm_gw2d_get_dimensions(SWMM_Engine engine, int* n_cells, int* m_layers);

/**
 * @brief One cell, one variable — see SWMM_GW2D_VAR_*.
 *
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] cell Zero-based covered cell index.
 * @param[in] var SWMM_GW2D_VAR_* selector; units are declared with each selector.
 * @param[out] value Required destination; unchanged on failure. A nonfinite state
 * value returns SWMM_ERR_LIFECYCLE rather than publishing it.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE for a null engine, or SWMM_ERR_BADPARAM for invalid
 * arguments.
 * @note Requires an active aquifer runtime; otherwise returns SWMM_ERR_LIFECYCLE. Does not
 * advance time.
 */
SWMM_ENGINE_API int swmm_gw2d_get_cell(SWMM_Engine engine, int cell, int var, double* value);

/**
 * @brief Bulk read of one variable over `[0, n_cells)`. @p len must be at least
 *  the cell count; the number written is returned through @p written.
 *
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] var SWMM_GW2D_VAR_* selector; units follow that selector.
 * @param[out] out Required array with room for every covered cell; native state values,
 * including nonfinite values, are copied without the scalar getter's finite check.
 * @param[in] len Array capacity in doubles, at least n_cells.
 * @param[out] written Optional count destination, updated after successful completion.
 * @note An invalid selector can modify the first output slot before returning an error.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE for a null engine, or SWMM_ERR_BADPARAM for invalid
 * arguments.
 * @note Requires an active aquifer runtime; otherwise returns SWMM_ERR_LIFECYCLE. Does not
 * advance time.
 */
SWMM_ENGINE_API int swmm_gw2d_get_cell_bulk(SWMM_Engine engine, int var, double* out, int len,
                                            int* written);

/**
 * @brief The sigma column of one cell: `m_layers` water contents, layer 0 at the
 *  ground surface. Refused with SWMM_ERR_BADPARAM when the cell's closure is
 *  not SIGMA — an all-zero buffer would read as a bone-dry column.
 *
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] cell Zero-based covered SIGMA cell index.
 * @param[out] theta Required array receiving dimensionless water content, surface layer first.
 * @param[in] len Capacity in doubles, at least m_layers.
 * @param[out] written Optional successful result count.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE for a null engine, or SWMM_ERR_BADPARAM for invalid
 * arguments.
 * @note Requires an active aquifer runtime; otherwise returns SWMM_ERR_LIFECYCLE. Does not
 * advance time.
 */
SWMM_ENGINE_API int swmm_gw2d_get_column(SWMM_Engine engine, int cell, double* theta, int len,
                                         int* written);

/**
 * @brief One ledger term — see SWMM_GW2D_LED_*.
 *
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] term SWMM_GW2D_LED_* selector.
 * @param[out] value Required destination in m3. STORAGE is current live holdings,
 * including accumulators; other terms are cumulative accepted ledger entries.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE for a null engine, or SWMM_ERR_BADPARAM for invalid
 * arguments.
 * @note Requires an active aquifer runtime; otherwise returns SWMM_ERR_LIFECYCLE. Does not
 * advance time.
 */
SWMM_ENGINE_API int swmm_gw2d_get_ledger(SWMM_Engine engine, int term, double* value);

/**
 * @brief Continuity residual (m3): `storage_now − storage_init − (in − out)`.
 *
 * @details The single number that says whether the kernel is conserving.
 *          `storage_now` includes anything still parked in a side
 *          accumulator, because between firings that water is real and is
 *          simply not on a cell yet — a residual computed without it reports
 *          a leak that is not there. See SubsurfaceSolver::settle.
 *
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[out] value Required destination for the signed conservation residual in m3.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE for a null engine, or SWMM_ERR_BADPARAM for invalid
 * arguments.
 * @note Requires an active aquifer runtime; otherwise returns SWMM_ERR_LIFECYCLE. Does not
 * advance time.
 */
SWMM_ENGINE_API int swmm_gw2d_get_continuity_error(SWMM_Engine engine, double* value);

/**
 * @brief Cumulative firings per LTS tier — the G-A telemetry. Writes
 *  `min(len, tier_count)` entries.
 *
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[out] out Required writable counter array.
 * @param[in] len Positive capacity in long counters; partial reads are permitted.
 * @param[out] written Optional destination for min(len, tier_count).
 * @return SWMM_OK, SWMM_ERR_BADHANDLE for a null engine, or SWMM_ERR_BADPARAM for invalid
 * arguments.
 * @note Requires an active aquifer runtime; otherwise returns SWMM_ERR_LIFECYCLE. Does not
 * advance time.
 */
SWMM_ENGINE_API int swmm_gw2d_get_tier_histogram(SWMM_Engine engine, long* out, int len,
                                                 int* written);

/* Optional recharge execution status. These flags describe the pure operator,
 * never full groundwater/device execution. Counters are process-local; component
 * restore resets them. They exclude rolled-back numerical transactions. */
#define SWMM_GW2D_OP_CLOSED_FORM_RECHARGE 1u
#define SWMM_GW2D_HOST_WATER_AND_RECEIPTS 1u
#define SWMM_GW2D_HOST_TRANSPORT 2u
#define SWMM_GW2D_HOST_REACTIONS 4u
#define SWMM_GW2D_HOST_THERMAL 8u

typedef struct SWMM_GwExecutionStatus {
    unsigned int struct_size; /* set to sizeof(SWMM_GwExecutionStatus) */
    int active;
    unsigned int available_operators, executed_operators, host_owned_operations;
    int effective_threads; /* 0 before host runtime initialization or on CPU */
    unsigned long long accepted_batches, accepted_cells;
    char recharge_backend[16]; /* current selection: cpu or omp */
    char profile[128];
    char detail[256];
} SWMM_GwExecutionStatus;

/**
 * @brief Runtime after initialization. Availability is distinct from actual use;
 * surface backend selection is reported separately by swmm_2d_get_run_stats.
 * Invalid size/handle/state leaves the caller's record unchanged.
 *
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in,out] status Required caller-owned record with struct_size initialized
 * to sizeof(SWMM_GwExecutionStatus); completely replaced only on success.
 * @note Backend/status strings are fixed-size NUL-terminated copies. The record
 * reports host-owned work separately from recharge operator availability and use.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE for a null engine, or SWMM_ERR_BADPARAM for invalid
 * arguments.
 * @note Requires an active aquifer runtime; otherwise returns SWMM_ERR_LIFECYCLE. Does not
 * advance time.
 */
SWMM_ENGINE_API int swmm_gw2d_get_execution_status(SWMM_Engine engine,
                                                   SWMM_GwExecutionStatus* status);

#ifdef __cplusplus
}
#endif

#endif /* OPENSWMM_GW2D_H */
