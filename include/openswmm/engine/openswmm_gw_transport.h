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
 * @file      openswmm_gw_transport.h
 * @brief     U4 (2026-09-07) — C API for the `[GW_*]` subsurface-transport
 *        authoring sections.
 *
 * @details These sections round-trip authored subsurface transport inputs.
 *          The integrated two-zone groundwater kernel consumes them at
 *          initialization. Runtime availability depends on the model and
 *          enabled transport classes; query swmm_get_transport_matrix() for
 *          capabilities and openswmm_gw2d.h for runtime state and ledgers.
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
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_GW_TRANSPORT_H
#define OPENSWMM_GW_TRANSPORT_H

#include "openswmm_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Scopes and zones ---------------------------------------------------- */

#define SWMM_GW_SCOPE_GLOBAL 0 /**< the `*` row */
#define SWMM_GW_SCOPE_TAG 1    /**< every cell carrying a tag */
#define SWMM_GW_SCOPE_CELL 2   /**< one cell, 0-based here */

#define SWMM_GW_ZONE_SAT 0
#define SWMM_GW_ZONE_UNSAT 1
#define SWMM_GW_ZONE_LAYER 2 /**< with an explicit 1-based layer index */

/* =========================================================================
 * [GW_TRANSPORT_OPTIONS]
 * =========================================================================
 * Key/value like swmm_options_get_ext: TRANSPORT_POLLUTANTS, TRANSPORT_MSX,
 * TRANSPORT_AGE, TRANSPORT_TEMPERATURE, DISPERSION, CONDUCTION,
 * SURFACE_THERMAL_BC, DEEP_THERMAL_BC, THERMAL_MIXING, C_DIFF. Values use the
 * section's own spelling; SURFACE_THERMAL_BC / DEEP_THERMAL_BC read back with
 * their argument appended ("FIXED 12.5", "GEOTHERMAL_FLUX 0.065 DEPTH 20").
 * Additive porous-model keys: THERMAL_MODEL WATER_ONLY|POROUS_EQUILIBRIUM,
 * THERMAL_LAYERS (fixed depth cells, 1..128), LATERAL_CONDUCTION (default NO),
 * THERMAL_MAX_STEP (s), WATER_THERMAL_CONDUCTIVITY and AIR_THERMAL_CONDUCTIVITY
 * (W/m/K), SURFACE_THERMAL_RESISTANCE (m2 K/W), SOIL_ET_HEAT
 * LATENT_SURFACE|SENSIBLE_ONLY. WATER_ONLY never activates dormant conduction.
 */

/**
 * @brief Serialize one groundwater transport option using its section spelling.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] key Non-null case-sensitive recognized uppercase option key.
 * @param[out] buf Non-null writable value buffer; multi-token boundary values include their
 * arguments.
 * @param[in] buflen Positive byte capacity; values are truncated safely to len - 1 plus NUL.
 * @return SWMM_OK on success, SWMM_ERR_BADHANDLE for an invalid handle, or
 * SWMM_ERR_BADPARAM for invalid arguments. Output storage is caller-owned.
 * @note Unknown keys return SWMM_ERR_BADPARAM. Units follow the key: seconds,
 * W/m/K conductivity, m2 K/W resistance, or the documented thermal-BC units.
 */
SWMM_ENGINE_API int swmm_gw_transport_option_get(SWMM_Engine engine, const char* key, char* buf,
                                                 int buflen);

/**
 * @brief Parse and atomically assign one groundwater transport option.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] key Non-null input-section key, passed to the shared parser.
 * @param[in] value Non-null textual value; whitespace splits multi-token arguments.
 * @return SWMM_OK on success, SWMM_ERR_BADHANDLE for an invalid handle, or
 * SWMM_ERR_BADPARAM for invalid arguments. Output storage is caller-owned.
 * @note Requires an editable BUILDING or OPENED model; later states return
 * SWMM_ERR_LIFECYCLE. Input data is copied and no input storage is retained.
 * @note Parses a temporary options copy and publishes only on success;
 * success sets its authored flag. No mesh is required.
 */
SWMM_ENGINE_API int swmm_gw_transport_option_set(SWMM_Engine engine, const char* key,
                                                 const char* value);

/**
 * @brief Test whether groundwater transport has any authored configuration.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @return One when any options/rows make the configuration nonempty, zero
 * otherwise, including for a null engine handle. No runtime is required.
 */
SWMM_ENGINE_API int swmm_gw_transport_authored(SWMM_Engine engine);

/* =========================================================================
 * [GW_TRANSPORT_PARAMS]
 * ========================================================================= */

typedef struct SWMM_GwParams {
    int scope;       /**< SWMM_GW_SCOPE_* */
    int cell;        /**< 0-based; -1 for GLOBAL/TAG */
    double rho_s;    /**< grain density (kg/m3) */
    double c_s;      /**< grain specific heat (J/kg/K) */
    double lambda_s; /**< grain thermal conductivity (W/m/K) */
    double a_s;      /**< specific surface area (m2/kg) */
    double alpha_L;  /**< longitudinal dispersivity (m) */
    double alpha_T;  /**< transverse dispersivity (m) */
    double D_m;      /**< molecular diffusivity (m2/s) */
    double D_v;      /**< vapour diffusivity (m2/s) */
    double geo_flux; /**< geothermal flux (W/m2) or deep temperature (degC) */
} SWMM_GwParams;

/**
 * @brief Count authored groundwater physical-parameter rows.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @return Nonnegative row count, or -1 for a null engine handle. This is a
 * count-returning function, not a SWMM status-returning function.
 * @note No initialized mesh or active groundwater runtime is required.
 */
SWMM_ENGINE_API int swmm_gw_params_count(SWMM_Engine engine);
/**
 * @brief Copy an authored porous-media parameter row and its optional tag.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] idx Zero-based parameter-row index.
 * @param[out] row Non-null structure receiving scope/cell and raw physical properties;
 * SI units are stated on SWMM_GwParams fields, with geo_flux depending on the deep thermal mode.
 * @param[out] tag_buf Optional writable tag-name buffer; NULL skips it.
 * @param[in] tag_len Byte capacity; <= 0 skips copying, otherwise truncates with NUL.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE, SWMM_ERR_BADINDEX for an invalid
 * row, or SWMM_ERR_BADPARAM for required missing/invalid arguments.
 * @note No resolution, conversion or storage retention occurs.
 */
SWMM_ENGINE_API int swmm_gw_params_get(SWMM_Engine engine, int idx, SWMM_GwParams* row,
                                       char* tag_buf, int tag_len);
/**
 * @brief Upsert a raw porous-media parameter row keyed by scope/tag/cell.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] row Non-null borrowed physical-property record in the SI units
 * specified on SWMM_GwParams. CELL indices must be nonnegative; TAG needs a name.
 * @param[in] tag Nonempty for TAG scope, ignored otherwise and may be NULL.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE, SWMM_ERR_BADINDEX for an invalid
 * row, or SWMM_ERR_BADPARAM for required missing/invalid arguments.
 * @note Requires an editable BUILDING or OPENED model; later states return
 * SWMM_ERR_LIFECYCLE. Input data is copied and no input storage is retained.
 * @note Unknown scope codes convert to GLOBAL. This authoring setter copies
 * physical coefficients without comprehensive range/finite checks; initialization
 * provides the runtime validity gate. Successful writes mark configuration authored.
 */
SWMM_ENGINE_API int swmm_gw_params_set(SWMM_Engine engine, const SWMM_GwParams* row,
                                       const char* tag);
/**
 * @brief Remove one authored groundwater physical-parameter row.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] idx Zero-based current row index; later indices shift on removal.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE, SWMM_ERR_BADINDEX or SWMM_ERR_LIFECYCLE.
 * @note Requires an editable BUILDING or OPENED model; later states return
 * SWMM_ERR_LIFECYCLE. Input data is copied and no input storage is retained.
 * @note Edits authoring only; removal does not clear every other authored option.
 */
SWMM_ENGINE_API int swmm_gw_params_remove(SWMM_Engine engine, int idx);

/* =========================================================================
 * [GW_SORPTION]
 * ========================================================================= */

/**
 * @brief Count authored groundwater species-sorption rows.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @return Nonnegative row count, or -1 for a null engine handle. This is a
 * count-returning function, not a SWMM status-returning function.
 * @note No initialized mesh or active groundwater runtime is required.
 */
SWMM_ENGINE_API int swmm_gw_sorption_count(SWMM_Engine engine);
/**
 * @brief Read an authored scope/species sorption record.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] idx Zero-based sorption row index.
 * @param[out] scope Optional SWMM_GW_SCOPE_* slot.
 * @param[out] tag_buf Optional tag-name buffer.
 * @param[in] tag_len Tag buffer byte capacity.
 * @param[out] cell Optional zero-based cell slot, -1 outside CELL scope.
 * @param[out] species_buf Optional species-name buffer.
 * @param[in] species_len Species-buffer byte capacity.
 * @param[out] kd Optional partition-coefficient slot in L/kg.
 * @param[out] decay Optional 1/day decay slot; negative means inherit the pollutant decay.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE, SWMM_ERR_BADINDEX for an invalid
 * row, or SWMM_ERR_BADPARAM for required missing/invalid arguments.
 * @note Optional output pointers may be NULL. String capacities <= 0 skip
 * copying; positive capacities truncate safely with a terminal NUL. No storage is retained.
 */
SWMM_ENGINE_API int swmm_gw_sorption_get(SWMM_Engine engine, int idx, int* scope, char* tag_buf,
                                         int tag_len, int* cell, char* species_buf, int species_len,
                                         double* kd, double* decay);
/**
 * @brief Upsert one scope/species partition coefficient and decay override.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] scope SWMM_GW_SCOPE_GLOBAL, TAG or CELL; the legacy scope
 * conversion treats other codes as GLOBAL unless this setter states otherwise.
 * @param[in] tag Required nonempty text for TAG; otherwise ignored and may be NULL.
 * @param[in] cell Nonnegative zero-based cell for CELL; otherwise ignored.
 * @param[in] species Non-null nonempty species name, copied without eager name resolution.
 * @param[in] kd Partition coefficient in L/kg; negative values are rejected.
 * @param[in] decay Decay in 1/day; negative inherits the pollutant's kDecay.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE, SWMM_ERR_BADINDEX for an invalid
 * row, or SWMM_ERR_BADPARAM for required missing/invalid arguments.
 * @note Requires an editable BUILDING or OPENED model; later states return
 * SWMM_ERR_LIFECYCLE. Input data is copied and no input storage is retained.
 * @note Physical finite/domain validation is not comprehensive here. Successful
 * writes mark groundwater configuration authored; a negative CELL index is BADINDEX.
 */
SWMM_ENGINE_API int swmm_gw_sorption_set(SWMM_Engine engine, int scope, const char* tag, int cell,
                                         const char* species, double kd, double decay);
/**
 * @brief Remove one authored groundwater species-sorption row.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] idx Zero-based current row index; later indices shift on removal.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE, SWMM_ERR_BADINDEX or SWMM_ERR_LIFECYCLE.
 * @note Requires an editable BUILDING or OPENED model; later states return
 * SWMM_ERR_LIFECYCLE. Input data is copied and no input storage is retained.
 * @note Edits authoring only; removal does not clear every other authored option.
 */
SWMM_ENGINE_API int swmm_gw_sorption_remove(SWMM_Engine engine, int idx);

/* =========================================================================
 * [GW_INITIAL_QUALITY]
 * ========================================================================= */

/**
 * @brief Count authored groundwater initial-quality rows.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @return Nonnegative row count, or -1 for a null engine handle. This is a
 * count-returning function, not a SWMM status-returning function.
 * @note No initialized mesh or active groundwater runtime is required.
 */
SWMM_ENGINE_API int swmm_gw_init_quality_count(SWMM_Engine engine);
/**
 * @brief Read an authored groundwater initial-quality record.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] idx Zero-based row index.
 * @param[out] scope Optional SWMM_GW_SCOPE_* slot.
 * @param[out] tag_buf Optional scope-tag buffer.
 * @param[in] tag_len Tag-buffer byte capacity.
 * @param[out] cell Optional zero-based CELL slot or -1.
 * @param[out] zone Optional SWMM_GW_ZONE_* selector.
 * @param[out] layer Optional one-based layer index, meaningful only for LAYER scope.
 * @param[out] species_buf Optional species-name buffer.
 * @param[in] species_len Species-buffer byte capacity.
 * @param[out] value Optional raw initial concentration in species units;
 * reserved temperature/age fields use their respective degrees-C/seconds conventions.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE, SWMM_ERR_BADINDEX for an invalid
 * row, or SWMM_ERR_BADPARAM for required missing/invalid arguments.
 * @note Optional output pointers may be NULL. String capacities <= 0 skip
 * copying; positive capacities truncate safely with a terminal NUL. No storage is retained.
 */
SWMM_ENGINE_API int swmm_gw_init_quality_get(SWMM_Engine engine, int idx, int* scope, char* tag_buf,
                                             int tag_len, int* cell, int* zone, int* layer,
                                             char* species_buf, int species_len, double* value);
/**
 * @brief Upsert initial quality keyed by scope, zone/layer and species.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] scope SWMM_GW_SCOPE_GLOBAL, TAG or CELL; the legacy scope
 * conversion treats other codes as GLOBAL unless this setter states otherwise.
 * @param[in] tag Required nonempty text for TAG; otherwise ignored and may be NULL.
 * @param[in] cell Nonnegative zero-based cell for CELL; otherwise ignored.
 * @param[in] zone SAT, UNSAT or LAYER; unrecognized codes convert to SAT.
 * @param[in] layer Positive one-based layer for LAYER, ignored and stored -1 otherwise.
 * @param[in] species Non-null nonempty species name; reserved __TEMPERATURE__
 * and __WATER_AGE__ permit signed authored values where ordinary concentrations do not.
 * @param[in] value Raw species concentration or reserved temperature/age value.
 * Negative ordinary-species values are rejected; finite/range checks are not comprehensive.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE, SWMM_ERR_BADINDEX for an invalid
 * row, or SWMM_ERR_BADPARAM for required missing/invalid arguments.
 * @note Requires an editable BUILDING or OPENED model; later states return
 * SWMM_ERR_LIFECYCLE. Input data is copied and no input storage is retained.
 * @note Active bulk groundwater rejects LAYER quality at initialization; drafts
 * can still be authored and saved. No source text pointer is retained.
 */
SWMM_ENGINE_API int swmm_gw_init_quality_set(SWMM_Engine engine, int scope, const char* tag,
                                             int cell, int zone, int layer, const char* species,
                                             double value);
/**
 * @brief Remove one authored groundwater initial-quality row.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] idx Zero-based current row index; later indices shift on removal.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE, SWMM_ERR_BADINDEX or SWMM_ERR_LIFECYCLE.
 * @note Requires an editable BUILDING or OPENED model; later states return
 * SWMM_ERR_LIFECYCLE. Input data is copied and no input storage is retained.
 * @note Edits authoring only; removal does not clear every other authored option.
 */
SWMM_ENGINE_API int swmm_gw_init_quality_remove(SWMM_Engine engine, int idx);
/**
 * @brief Read the initial-quality CSV sidecar reference.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[out] buf Non-null writable path buffer.
 * @param[in] buflen Positive byte capacity; long paths are truncated with NUL.
 * @return SWMM_OK on success, SWMM_ERR_BADHANDLE for an invalid handle, or
 * SWMM_ERR_BADPARAM for invalid arguments. Output storage is caller-owned.
 * @note Returns stored authoring text without opening the file.
 */
SWMM_ENGINE_API int swmm_gw_init_quality_file_get(SWMM_Engine engine, char* buf, int buflen);
/**
 * @brief Assign the initial-quality CSV sidecar reference.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] path Null-terminated path copied as given; NULL or empty clears the reference.
 * @return SWMM_OK on success, SWMM_ERR_BADHANDLE for an invalid handle, or
 * SWMM_ERR_BADPARAM for invalid arguments. Output storage is caller-owned.
 * @note Requires an editable BUILDING or OPENED model; later states return
 * SWMM_ERR_LIFECYCLE. Input data is copied and no input storage is retained.
 * @note A nonempty reference marks configuration authored; clearing it does not reset other
 * authored flags.
 */
SWMM_ENGINE_API int swmm_gw_init_quality_file_set(SWMM_Engine engine, const char* path);

/* =========================================================================
 * [GW_BOUNDARY_QUALITY]
 * =========================================================================
 * `kind` is "CONC", "TS", "MASSFLUX" or "HEATFLUX". A row carries either a
 * constant value or a series name (never both).
 */

/**
 * @brief Count authored groundwater exterior-quality rows.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @return Nonnegative row count, or -1 for a null engine handle. This is a
 * count-returning function, not a SWMM status-returning function.
 * @note No initialized mesh or active groundwater runtime is required.
 */
SWMM_ENGINE_API int swmm_gw_boundary_quality_count(SWMM_Engine engine);
/**
 * @brief Read an authored groundwater edge-quality record.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] idx Zero-based boundary-quality row index.
 * @param[out] cell Optional zero-based cell index slot.
 * @param[out] edge Optional local edge slot, 0..cell vertex count - 1.
 * @param[out] species_buf Optional species-name buffer.
 * @param[in] species_len Species-buffer byte capacity.
 * @param[out] kind_buf Optional kind buffer: CONC, TS, MASSFLUX or HEATFLUX.
 * @param[in] kind_len Kind-buffer byte capacity.
 * @param[out] value Optional raw constant concentration/flux field in the kind's section units.
 * @param[out] ts_buf Optional series-name buffer, empty when not authored.
 * @param[in] ts_len Time-series-buffer byte capacity.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE, SWMM_ERR_BADINDEX for an invalid
 * row, or SWMM_ERR_BADPARAM for required missing/invalid arguments.
 * @note Optional output pointers may be NULL. String capacities <= 0 skip
 * copying; positive capacities truncate safely with a terminal NUL. No storage is retained.
 */
SWMM_ENGINE_API int swmm_gw_boundary_quality_get(SWMM_Engine engine, int idx, int* cell, int* edge,
                                                 char* species_buf, int species_len, char* kind_buf,
                                                 int kind_len, double* value, char* ts_buf,
                                                 int ts_len);
/**
 * @brief Upsert groundwater edge quality for a species and concentration/flux category.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] cell Nonnegative zero-based cell index; checked against an available mesh.
 * @param[in] edge Nonnegative local edge, below the cell's vertex count when a mesh exists.
 * @param[in] species Non-null nonempty species name, copied for later resolution.
 * @param[in] kind Exact CONC, TS, MASSFLUX or HEATFLUX string.
 * @param[in] value Finite raw constant in the chosen concentration/flux section units,
 * also required finite for TS mode even though a series supplies runtime values.
 * @param[in] ts_name Nonempty series name for TS, optional/NULL otherwise.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE, SWMM_ERR_BADINDEX for an invalid
 * row, or SWMM_ERR_BADPARAM for required missing/invalid arguments.
 * @note Requires an editable BUILDING or OPENED model; later states return
 * SWMM_ERR_LIFECYCLE. Input data is copied and no input storage is retained.
 * @note Concentration and imposed-flux rows are distinct categories for upsert.
 * Hydraulic edge bindings are required before active groundwater initialization;
 * name resolution and runtime compatibility are deferred to that stage.
 */
SWMM_ENGINE_API int swmm_gw_boundary_quality_set(SWMM_Engine engine, int cell, int edge,
                                                 const char* species, const char* kind,
                                                 double value, const char* ts_name);
/**
 * @brief Remove one authored groundwater exterior-quality row.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] idx Zero-based current row index; later indices shift on removal.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE, SWMM_ERR_BADINDEX or SWMM_ERR_LIFECYCLE.
 * @note Requires an editable BUILDING or OPENED model; later states return
 * SWMM_ERR_LIFECYCLE. Input data is copied and no input storage is retained.
 * @note Edits authoring only; removal does not clear every other authored option.
 */
SWMM_ENGINE_API int swmm_gw_boundary_quality_remove(SWMM_Engine engine, int idx);

/* =========================================================================
 * [GW_SOURCES]
 * ========================================================================= */

/**
 * @brief Count authored groundwater named-source rows.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @return Nonnegative row count, or -1 for a null engine handle. This is a
 * count-returning function, not a SWMM status-returning function.
 * @note No initialized mesh or active groundwater runtime is required.
 */
SWMM_ENGINE_API int swmm_gw_source_count(SWMM_Engine engine);
/**
 * @brief Read an authored named groundwater source and its scope.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] idx Zero-based source row index.
 * @param[out] name_buf Optional source-name buffer.
 * @param[in] name_len Name-buffer byte capacity.
 * @param[out] scope Optional GLOBAL/TAG/CELL scope slot.
 * @param[out] tag_buf Optional tag-name buffer.
 * @param[in] tag_len Tag-buffer byte capacity.
 * @param[out] cell Optional zero-based cell slot, -1 outside CELL scope.
 * @param[out] flow Optional total source discharge in m3/s, signed positive into groundwater.
 * @param[out] flow_ts_buf Optional flow time-series-name buffer.
 * @param[in] flow_ts_len Flow-series buffer byte capacity.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE, SWMM_ERR_BADINDEX for an invalid
 * row, or SWMM_ERR_BADPARAM for required missing/invalid arguments.
 * @note Optional output pointers may be NULL. String capacities <= 0 skip
 * copying; positive capacities truncate safely with a terminal NUL. No storage is retained.
 */
SWMM_ENGINE_API int swmm_gw_source_get(SWMM_Engine engine, int idx, char* name_buf, int name_len,
                                       int* scope, char* tag_buf, int tag_len, int* cell,
                                       double* flow, char* flow_ts_buf, int flow_ts_len);
/**
 * @brief Upsert a named groundwater water source and its scope.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] name Non-null nonempty source name, used as the replacement key.
 * @param[in] scope Exactly GLOBAL (0), TAG (1) or CELL (2); other codes are rejected.
 * @param[in] tag Non-null nonempty for TAG; ignored otherwise and may be NULL.
 * @param[in] cell Nonnegative zero-based index for CELL; ignored otherwise.
 * @param[in] flow Finite signed total discharge in m3/s over all active matching
 * cells; GLOBAL/TAG distribution is area-weighted and negative discharge extracts water.
 * @param[in] flow_ts Optional time-series name; NULL stores empty text.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE, SWMM_ERR_BADINDEX for an invalid
 * row, or SWMM_ERR_BADPARAM for required missing/invalid arguments.
 * @note Requires an editable BUILDING or OPENED model; later states return
 * SWMM_ERR_LIFECYCLE. Input data is copied and no input storage is retained.
 * @note Updates preserve existing species terms and scale, and clear XY-based
 * location on replacement. Source/name/series resolution is deferred; successful
 * writes mark options authored. Extraction is later bounded by drainable water.
 */
SWMM_ENGINE_API int swmm_gw_source_set(SWMM_Engine engine, const char* name, int scope,
                                       const char* tag, int cell, double flow, const char* flow_ts);
/**
 * @brief Read the multiplier for a named source's flow and MASS terms.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] idx Zero-based source row index.
 * @param[out] value Non-null slot for dimensionless source multiplier;
 * CONC terms remain unchanged by the multiplier.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE, SWMM_ERR_BADINDEX for an invalid
 * row, or SWMM_ERR_BADPARAM for required missing/invalid arguments.
 * @note Does not sample or change source time series.
 */
SWMM_ENGINE_API int swmm_gw_source_scale_get(SWMM_Engine engine, int idx, double* value);
/**
 * @brief Set the multiplier for a named source's flow and MASS terms.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] idx Zero-based source row index.
 * @param[in] value Finite nonnegative dimensionless source multiplier;
 * CONC terms remain unchanged by the multiplier.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE, SWMM_ERR_BADINDEX for an invalid
 * row, or SWMM_ERR_BADPARAM for required missing/invalid arguments.
 * @note Requires an editable BUILDING or OPENED model; later states return
 * SWMM_ERR_LIFECYCLE. Input data is copied and no input storage is retained.
 */
SWMM_ENGINE_API int swmm_gw_source_scale_set(SWMM_Engine engine, int idx, double value);
/**
 * @brief Remove one authored groundwater named-source row.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] idx Zero-based current row index; later indices shift on removal.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE, SWMM_ERR_BADINDEX or SWMM_ERR_LIFECYCLE.
 * @note Requires an editable BUILDING or OPENED model; later states return
 * SWMM_ERR_LIFECYCLE. Input data is copied and no input storage is retained.
 * @note Edits authoring only; removal does not clear every other authored option.
 */
SWMM_ENGINE_API int swmm_gw_source_remove(SWMM_Engine engine, int idx);

/**
 * @brief Count species terms carried by one named groundwater source.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] src_idx Zero-based source row index.
 * @return Nonnegative species-term count, or -1 for a null handle or invalid
 * source row. This returns a count rather than a SWMM status code.
 */
SWMM_ENGINE_API int swmm_gw_source_species_count(SWMM_Engine engine, int src_idx);
/**
 * @brief Read a named groundwater source species term.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] src_idx Zero-based source row index.
 * @param[in] term_idx Zero-based term index within that source.
 * @param[out] species_buf Optional species-name buffer.
 * @param[in] species_len Species-buffer byte capacity.
 * @param[out] kind_buf Optional CONC/MASS kind buffer.
 * @param[in] kind_len Kind-buffer byte capacity.
 * @param[out] value Optional constant concentration or species-native mass/s field.
 * @param[out] ts_buf Optional time-series-name buffer.
 * @param[in] ts_len Series-buffer byte capacity.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE, SWMM_ERR_BADINDEX for an invalid
 * row, or SWMM_ERR_BADPARAM for required missing/invalid arguments.
 * @note Optional output pointers may be NULL. String capacities <= 0 skip
 * copying; positive capacities truncate safely with a terminal NUL. No storage is retained.
 */
SWMM_ENGINE_API int swmm_gw_source_species_get(SWMM_Engine engine, int src_idx, int term_idx,
                                               char* species_buf, int species_len, char* kind_buf,
                                               int kind_len, double* value, char* ts_buf,
                                               int ts_len);
/**
 * @brief Upsert a named source's species term by species name.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] src_idx Zero-based source row index.
 * @param[in] species Non-null nonempty species name.
 * @param[in] kind Exact CONC or MASS text; other spellings are rejected.
 * @param[in] value Raw concentration or native species mass/s field; this setter
 * stores it without finite/range validation, so meaningful physical values remain required.
 * @param[in] ts_name Optional series name; NULL stores empty text. Series
 * are sampled linearly with held endpoint values on the absolute simulation clock.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE, SWMM_ERR_BADINDEX for an invalid
 * row, or SWMM_ERR_BADPARAM for required missing/invalid arguments.
 * @note Requires an editable BUILDING or OPENED model; later states return
 * SWMM_ERR_LIFECYCLE. Input data is copied and no input storage is retained.
 * @note MASS is supported for pollutant-native mg, ug or count rates; age,
 * temperature and unresolved MSX MASS are rejected by active runtime validation.
 * Updates preserve other terms; no borrowed strings are retained.
 */
SWMM_ENGINE_API int swmm_gw_source_species_set(SWMM_Engine engine, int src_idx, const char* species,
                                               const char* kind, double value, const char* ts_name);
/**
 * @brief Remove one species term from a named source.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] src_idx Zero-based source row index.
 * @param[in] term_idx Zero-based term index, shifting later term positions on success.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE, SWMM_ERR_BADINDEX or SWMM_ERR_LIFECYCLE.
 * @note Requires an editable BUILDING or OPENED model; later states return
 * SWMM_ERR_LIFECYCLE. Input data is copied and no input storage is retained.
 */
SWMM_ENGINE_API int swmm_gw_source_species_remove(SWMM_Engine engine, int src_idx, int term_idx);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* OPENSWMM_GW_TRANSPORT_H */
