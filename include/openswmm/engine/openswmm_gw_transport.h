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
 * @file openswmm_gw_transport.h
 * @brief U4 (2026-09-07) — C API for the `[GW_*]` subsurface-transport
 *        authoring sections.
 *
 * @details **Authoring only in this release.** The integrated two-zone
 *          groundwater kernel that consumes these rows does not exist yet;
 *          a run emits one warning and the rows change nothing. Everything
 *          here nevertheless round-trips: a host can build a complete
 *          subsurface-transport model, save it, reopen it and get the same
 *          file back, and nothing about that file changes when the kernel
 *          lands.
 *
 *          **Cell-generic.** Every index is a CELL (`0 .. swmm_2d_cell_count-1`
 *          through this API; 1-based in the `.inp`), and an edge is the LOCAL
 *          edge `0 .. nv-1` of ITS cell — 3 for a triangle, 4 for a quad.
 *
 *          **Editing state.** Setters require BUILDING or OPENED, the same
 *          contract as the 2D boundary API: the rows seed the kernel at
 *          initialize, so a mid-run edit would silently no-op.
 *
 * @ingroup engine_2d
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_GW_TRANSPORT_H
#define OPENSWMM_GW_TRANSPORT_H

#include "openswmm_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Scopes and zones ---------------------------------------------------- */

#define SWMM_GW_SCOPE_GLOBAL 0   /**< the `*` row */
#define SWMM_GW_SCOPE_TAG    1   /**< every cell carrying a tag */
#define SWMM_GW_SCOPE_CELL   2   /**< one cell, 0-based here */

#define SWMM_GW_ZONE_SAT     0
#define SWMM_GW_ZONE_UNSAT   1
#define SWMM_GW_ZONE_LAYER   2   /**< with an explicit 1-based layer index */

/* =========================================================================
 * [GW_TRANSPORT_OPTIONS]
 * =========================================================================
 * Key/value like swmm_options_get_ext: TRANSPORT_POLLUTANTS, TRANSPORT_MSX,
 * TRANSPORT_AGE, TRANSPORT_TEMPERATURE, DISPERSION, CONDUCTION,
 * SURFACE_THERMAL_BC, DEEP_THERMAL_BC, THERMAL_MIXING, C_DIFF. Values use the
 * section's own spelling; SURFACE_THERMAL_BC / DEEP_THERMAL_BC read back with
 * their argument appended ("FIXED 12.5", "GEOTHERMAL_FLUX 0.065 DEPTH 20").
 */

/** @brief Read one option into @p buf. SWMM_ERR_BADPARAM on an unknown key. */
SWMM_ENGINE_API int swmm_gw_transport_option_get(SWMM_Engine engine,
        const char* key, char* buf, int buflen);

/** @brief Set one option (BUILDING/OPENED). */
SWMM_ENGINE_API int swmm_gw_transport_option_set(SWMM_Engine engine,
        const char* key, const char* value);

/** @brief 1 when any `[GW_*]` row was authored, else 0. */
SWMM_ENGINE_API int swmm_gw_transport_authored(SWMM_Engine engine);

/* =========================================================================
 * [GW_TRANSPORT_PARAMS]
 * ========================================================================= */

typedef struct SWMM_GwParams {
    int    scope;      /**< SWMM_GW_SCOPE_* */
    int    cell;       /**< 0-based; -1 for GLOBAL/TAG */
    double rho_s;      /**< grain density (kg/m3) */
    double c_s;        /**< grain specific heat (J/kg/K) */
    double lambda_s;   /**< grain thermal conductivity (W/m/K) */
    double a_s;        /**< specific surface area (m2/kg) */
    double alpha_L;    /**< longitudinal dispersivity (m) */
    double alpha_T;    /**< transverse dispersivity (m) */
    double D_m;        /**< molecular diffusivity (m2/s) */
    double D_v;        /**< vapour diffusivity (m2/s) */
    double geo_flux;   /**< geothermal flux (W/m2) or deep temperature (degC) */
} SWMM_GwParams;

SWMM_ENGINE_API int swmm_gw_params_count(SWMM_Engine engine);
/** @brief Read row @p idx; @p tag_buf receives the TAG (empty otherwise). */
SWMM_ENGINE_API int swmm_gw_params_get(SWMM_Engine engine, int idx,
        SWMM_GwParams* row, char* tag_buf, int tag_len);
/** @brief Append or replace the row for (@p scope, @p tag/@p cell). */
SWMM_ENGINE_API int swmm_gw_params_set(SWMM_Engine engine,
        const SWMM_GwParams* row, const char* tag);
SWMM_ENGINE_API int swmm_gw_params_remove(SWMM_Engine engine, int idx);

/* =========================================================================
 * [GW_SORPTION]
 * ========================================================================= */

SWMM_ENGINE_API int swmm_gw_sorption_count(SWMM_Engine engine);
/** @param decay 1/day; negative means "use the [POLLUTANTS] kdecay". */
SWMM_ENGINE_API int swmm_gw_sorption_get(SWMM_Engine engine, int idx,
        int* scope, char* tag_buf, int tag_len, int* cell,
        char* species_buf, int species_len, double* kd, double* decay);
SWMM_ENGINE_API int swmm_gw_sorption_set(SWMM_Engine engine, int scope,
        const char* tag, int cell, const char* species, double kd, double decay);
SWMM_ENGINE_API int swmm_gw_sorption_remove(SWMM_Engine engine, int idx);

/* =========================================================================
 * [GW_INITIAL_QUALITY]
 * ========================================================================= */

SWMM_ENGINE_API int swmm_gw_init_quality_count(SWMM_Engine engine);
/** @param layer 1-based, meaningful only when @p zone is SWMM_GW_ZONE_LAYER. */
SWMM_ENGINE_API int swmm_gw_init_quality_get(SWMM_Engine engine, int idx,
        int* scope, char* tag_buf, int tag_len, int* cell,
        int* zone, int* layer, char* species_buf, int species_len, double* value);
SWMM_ENGINE_API int swmm_gw_init_quality_set(SWMM_Engine engine, int scope,
        const char* tag, int cell, int zone, int layer,
        const char* species, double value);
SWMM_ENGINE_API int swmm_gw_init_quality_remove(SWMM_Engine engine, int idx);
/** @brief The `FILE <csv>` sidecar reference ("" when none). */
SWMM_ENGINE_API int swmm_gw_init_quality_file_get(SWMM_Engine engine,
        char* buf, int buflen);
SWMM_ENGINE_API int swmm_gw_init_quality_file_set(SWMM_Engine engine,
        const char* path);

/* =========================================================================
 * [GW_BOUNDARY_QUALITY]
 * =========================================================================
 * `kind` is "CONC", "TS", "MASSFLUX" or "HEATFLUX". A row carries either a
 * constant value or a series name (never both).
 */

SWMM_ENGINE_API int swmm_gw_boundary_quality_count(SWMM_Engine engine);
SWMM_ENGINE_API int swmm_gw_boundary_quality_get(SWMM_Engine engine, int idx,
        int* cell, int* edge, char* species_buf, int species_len,
        char* kind_buf, int kind_len, double* value,
        char* ts_buf, int ts_len);
/** @param edge LOCAL edge 0..nv-1 of @p cell (3 for a triangle, 4 for a quad). */
SWMM_ENGINE_API int swmm_gw_boundary_quality_set(SWMM_Engine engine,
        int cell, int edge, const char* species, const char* kind,
        double value, const char* ts_name);
SWMM_ENGINE_API int swmm_gw_boundary_quality_remove(SWMM_Engine engine, int idx);

/* =========================================================================
 * [GW_SOURCES]
 * ========================================================================= */

SWMM_ENGINE_API int swmm_gw_source_count(SWMM_Engine engine);
SWMM_ENGINE_API int swmm_gw_source_get(SWMM_Engine engine, int idx,
        char* name_buf, int name_len, int* scope, char* tag_buf, int tag_len,
        int* cell, double* flow, char* flow_ts_buf, int flow_ts_len);
/** @brief Append or replace the source named @p name. */
SWMM_ENGINE_API int swmm_gw_source_set(SWMM_Engine engine, const char* name,
        int scope, const char* tag, int cell, double flow, const char* flow_ts);
SWMM_ENGINE_API int swmm_gw_source_remove(SWMM_Engine engine, int idx);

/** @brief Species terms carried by source @p src_idx. */
SWMM_ENGINE_API int swmm_gw_source_species_count(SWMM_Engine engine, int src_idx);
SWMM_ENGINE_API int swmm_gw_source_species_get(SWMM_Engine engine, int src_idx,
        int term_idx, char* species_buf, int species_len,
        char* kind_buf, int kind_len, double* value, char* ts_buf, int ts_len);
/** @param kind "CONC" or "MASS". Upserts on the species name. */
SWMM_ENGINE_API int swmm_gw_source_species_set(SWMM_Engine engine, int src_idx,
        const char* species, const char* kind, double value, const char* ts_name);
SWMM_ENGINE_API int swmm_gw_source_species_remove(SWMM_Engine engine,
        int src_idx, int term_idx);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* OPENSWMM_GW_TRANSPORT_H */
