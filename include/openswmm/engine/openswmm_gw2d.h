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
 * @file openswmm_gw2d.h
 * @brief G-step 19 — the two-zone groundwater kernel's C API: authoring the
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
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_GW2D_H
#define OPENSWMM_GW2D_H

#include "openswmm_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Scopes (same encoding as the [GW_*] transport API) ------------------ */

#define SWMM_GW2D_SCOPE_GLOBAL 0   /**< the `*` row */
#define SWMM_GW2D_SCOPE_TAG    1   /**< every cell carrying a tag */
#define SWMM_GW2D_SCOPE_CELL   2   /**< one cell, 0-based here */

/* ---- Soil-characteristic laws ------------------------------------------- */

#define SWMM_GW2D_SOIL_RUSSO         0
#define SWMM_GW2D_SOIL_GARDNER       1
#define SWMM_GW2D_SOIL_BROOKS_COREY  2
#define SWMM_GW2D_SOIL_VAN_GENUCHTEN 3

/* ---- Unsaturated-zone closures ------------------------------------------ */

#define SWMM_GW2D_CLOSURE_AUTO        (-1)
#define SWMM_GW2D_CLOSURE_CLOSED_FORM 0
#define SWMM_GW2D_CLOSURE_ENSLAVED    1
#define SWMM_GW2D_CLOSURE_SIGMA       2

/* ---- Per-cell state selectors ------------------------------------------- */

#define SWMM_GW2D_VAR_HG        0   /**< saturated thickness (m) */
#define SWMM_GW2D_VAR_HU        1   /**< unsaturated storage (m of water) */
#define SWMM_GW2D_VAR_TABLE_EL  2   /**< water-table elevation (m) */
#define SWMM_GW2D_VAR_Q0        3   /**< recharge (m/s), + down */
#define SWMM_GW2D_VAR_QLAT      4   /**< net lateral Darcy in (m3/s) */
#define SWMM_GW2D_VAR_QNODE     5   /**< node exchange (m3/s), + out */
#define SWMM_GW2D_VAR_QDEEP     6   /**< deep loss (m/s) */
#define SWMM_GW2D_VAR_QET       7   /**< subsurface ET (m/s) */
#define SWMM_GW2D_VAR_DUNNE     8   /**< saturation excess to surface (m3/s) */
#define SWMM_GW2D_VAR_QPLUS     9   /**< infiltration delivered in (m/s) */
#define SWMM_GW2D_VAR_DT_CELL   10  /**< min(dt_g, dt_u) (s) */
#define SWMM_GW2D_VAR_TIER      11  /**< assigned LTS tier */
#define SWMM_GW2D_VAR_CLOSURE   12  /**< resolved closure (AUTO already gone) */

/* ---- Ledger selectors (m3, cumulative) ---------------------------------- */

#define SWMM_GW2D_LED_RECHARGE      0
#define SWMM_GW2D_LED_LATERAL       1
#define SWMM_GW2D_LED_DEEP          2
#define SWMM_GW2D_LED_NODE          3
#define SWMM_GW2D_LED_DUNNE         4
#define SWMM_GW2D_LED_CAPRISE       5
#define SWMM_GW2D_LED_ET            6
#define SWMM_GW2D_LED_INFIL_IN      7
#define SWMM_GW2D_LED_INIT_STORAGE  8
#define SWMM_GW2D_LED_STORAGE       9   /**< storage NOW, incl. accumulators */

/* =========================================================================
 * [2D_AQUIFER_OPTIONS]
 * ========================================================================= */

/** Read one option as text. Keys: SOIL_CHAR, CLOSURE, M_LAYERS,
 *  CAPILLARY_DIFF, C_GW, C_COL, FORCE_CLOSED_FORM, MODE, DUNNE, GW_ET. */
SWMM_ENGINE_API int swmm_gw2d_option_get(SWMM_Engine engine, const char* key,
                                         char* buf, int buflen);

/** Set one option from text. Same keys and the same token spellings the
 *  `.inp` accepts — one parser, so the file and the API cannot drift. */
SWMM_ENGINE_API int swmm_gw2d_option_set(SWMM_Engine engine, const char* key,
                                         const char* value);

/* =========================================================================
 * [2D_AQUIFER] rows
 * ========================================================================= */

/** Number of authored rows. */
SWMM_ENGINE_API int swmm_gw2d_row_count(SWMM_Engine engine, int* count);

/**
 * @brief Append one row. Values are in the PROJECT's units.
 *
 * @param scope   SWMM_GW2D_SCOPE_*
 * @param tag     tag name for SCOPE_TAG, else ignored (may be NULL)
 * @param cell    0-based cell for SCOPE_CELL, else ignored
 * @param ks      saturated conductivity (in/hr | mm/hr)
 * @param zs      soil column thickness (ft | m)
 * @param theta_s porosity
 * @param theta_r residual water content
 * @param alpha   sorptive number (1/ft | 1/m)
 */
SWMM_ENGINE_API int swmm_gw2d_row_add(SWMM_Engine engine, int scope,
                                      const char* tag, int cell,
                                      double ks, double zs,
                                      double theta_s, double theta_r,
                                      double alpha);

/** Read row @p index back, in the units it was authored in. Any out pointer
 *  may be NULL. @p tag needs @p taglen bytes. */
SWMM_ENGINE_API int swmm_gw2d_row_get(SWMM_Engine engine, int index,
                                      int* scope, char* tag, int taglen,
                                      int* cell, double* ks, double* zs,
                                      double* theta_s, double* theta_r,
                                      double* alpha);

/** Set one of a row's optional properties. Keys: PSI_B, LAMBDA, N, L,
 *  C_LOSS, HG0, SOIL_CHAR, CLOSURE, M_LAYERS. The numeric-valued keys take
 *  @p value; SOIL_CHAR and CLOSURE take the SWMM_GW2D_SOIL_* /
 *  SWMM_GW2D_CLOSURE_* codes as a double, which keeps this one entry point
 *  instead of nine. */
SWMM_ENGINE_API int swmm_gw2d_row_set_property(SWMM_Engine engine, int index,
                                               const char* key, double value);

/** Read one optional property back. */
SWMM_ENGINE_API int swmm_gw2d_row_get_property(SWMM_Engine engine, int index,
                                               const char* key, double* value);

/** Remove row @p index. */
SWMM_ENGINE_API int swmm_gw2d_row_remove(SWMM_Engine engine, int index);

/* =========================================================================
 * [2D_AQUIFER_NODE] beds
 * ========================================================================= */

SWMM_ENGINE_API int swmm_gw2d_node_count(SWMM_Engine engine, int* count);

/** Add a node bed. @p kc in in/hr | mm/hr, @p dc in ft | m, @p area in
 *  ft2 | m2 (0 = the cell's own area). @p kc == 0 means direct Darcy. */
SWMM_ENGINE_API int swmm_gw2d_node_add(SWMM_Engine engine, const char* node,
                                       int cell, double kc, double dc,
                                       double area);

SWMM_ENGINE_API int swmm_gw2d_node_get(SWMM_Engine engine, int index,
                                       char* node, int nodelen, int* cell,
                                       double* kc, double* dc, double* area);

SWMM_ENGINE_API int swmm_gw2d_node_remove(SWMM_Engine engine, int index);

/* =========================================================================
 * Running state — SI throughout
 * ========================================================================= */

/** Non-zero once a `[2D_AQUIFER]` row has resolved and the kernel is live. */
SWMM_ENGINE_API int swmm_gw2d_is_active(SWMM_Engine engine, int* active);

/** Cells the kernel covers, and the sigma layer count (1 when no cell uses
 *  closure B). Either pointer may be NULL. */
SWMM_ENGINE_API int swmm_gw2d_get_dimensions(SWMM_Engine engine,
                                             int* n_cells, int* m_layers);

/** One cell, one variable — see SWMM_GW2D_VAR_*. */
SWMM_ENGINE_API int swmm_gw2d_get_cell(SWMM_Engine engine, int cell, int var,
                                       double* value);

/** Bulk read of one variable over `[0, n_cells)`. @p len must be at least
 *  the cell count; the number written is returned through @p written. */
SWMM_ENGINE_API int swmm_gw2d_get_cell_bulk(SWMM_Engine engine, int var,
                                            double* out, int len,
                                            int* written);

/** The sigma column of one cell: `m_layers` water contents, layer 0 at the
 *  ground surface. Refused with SWMM_ERR_BADPARAM when the cell's closure is
 *  not SIGMA — an all-zero buffer would read as a bone-dry column. */
SWMM_ENGINE_API int swmm_gw2d_get_column(SWMM_Engine engine, int cell,
                                         double* theta, int len,
                                         int* written);

/** One ledger term — see SWMM_GW2D_LED_*. */
SWMM_ENGINE_API int swmm_gw2d_get_ledger(SWMM_Engine engine, int term,
                                         double* value);

/**
 * @brief Continuity residual (m3): `storage_now − storage_init − (in − out)`.
 *
 * @details The single number that says whether the kernel is conserving.
 *          `storage_now` includes anything still parked in a side
 *          accumulator, because between firings that water is real and is
 *          simply not on a cell yet — a residual computed without it reports
 *          a leak that is not there. See SubsurfaceSolver::settle.
 */
SWMM_ENGINE_API int swmm_gw2d_get_continuity_error(SWMM_Engine engine,
                                                   double* value);

/** Cumulative firings per LTS tier — the G-A telemetry. Writes
 *  `min(len, tier_count)` entries. */
SWMM_ENGINE_API int swmm_gw2d_get_tier_histogram(SWMM_Engine engine,
                                                 long* out, int len,
                                                 int* written);

#ifdef __cplusplus
}
#endif

#endif /* OPENSWMM_GW2D_H */
