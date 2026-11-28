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
 * @file      openswmm_transport.h
 * @brief     E6 — the ARD transport configuration surface ([TRANSPORT_OPTIONS],
 *        [TRANSPORT_BOUNDARIES], [TRANSPORT_SOURCES]).
 *
 * @details The last quality subsystem with no C API of its own: heat,
 *          reactions, water age and the pollutant tables all have theirs,
 *          and G7g (the dispersion/boundary property editors) was gated on
 *          this header existing. Follows `openswmm_water_age.h`'s
 *          conventions: values cross the boundary in the CONFIG FILE'S
 *          units, the engine's internal units stay internal, and setters
 *          mark the config `configured` so a save renders it.
 *
 *          Boundary/source authoring calls require BUILDING or OPENED. During
 *          BUILDING references may remain unresolved; OPENED edits resolve
 *          names immediately and restore the prior configuration on failure.
 *          The original dispersion/spacing option setters predate these
 *          lifecycle checks; each function documents its actual restriction.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_TRANSPORT_H
#define OPENSWMM_TRANSPORT_H

#include "openswmm_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief `[TRANSPORT_OPTIONS] DISPERSION` mode. */
typedef enum SWMM_TransportDispersionMode {
    SWMM_DISPERSION_OFF = 0,     /**< No global dispersion (per-conduit
                                      overrides still apply). */
    SWMM_DISPERSION_FISCHER = 1, /**< Fischer et al. (1979) closure from the
                                      local hydraulics. */
    SWMM_DISPERSION_VALUE = 2    /**< One user coefficient, display len²/s. */
} SWMM_TransportDispersionMode;

/**
 * @brief Read whether an ARD transport component is authored.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[out] configured Non-null caller-owned slot for 0 absent or 1 configured.
 * @return SWMM_OK on success, SWMM_ERR_BADHANDLE for an invalid handle, or
 * SWMM_ERR_BADPARAM for invalid arguments. Output storage is caller-owned.
 * @note Reads authored configuration; no transport runtime is advanced.
 */
SWMM_ENGINE_API int swmm_transport_get_configured(SWMM_Engine engine, int* configured);

/**
 * @brief Read the global longitudinal-dispersion mode.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[out] mode Non-null caller-owned slot for SWMM_TransportDispersionMode value.
 * @return SWMM_OK on success, SWMM_ERR_BADHANDLE for an invalid handle, or
 * SWMM_ERR_BADPARAM for invalid arguments. Output storage is caller-owned.
 * @note Reads authored configuration; no transport runtime is advanced.
 */
SWMM_ENGINE_API int swmm_transport_get_dispersion_mode(SWMM_Engine engine, int* mode);

/**
 * @brief Assign the global dispersion mode and mark transport configuration present.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] mode SWMM_DISPERSION_OFF, SWMM_DISPERSION_FISCHER or SWMM_DISPERSION_VALUE; VALUE
 * preserves the previous coefficient.
 * @return SWMM_OK on success, SWMM_ERR_BADHANDLE for an invalid handle, or
 * SWMM_ERR_BADPARAM for invalid arguments. Output storage is caller-owned.
 * @note This option setter performs no editable-state check. It changes
 * stored configuration and does not promise to rebuild an initialized transport kernel.
 */
SWMM_ENGINE_API int swmm_transport_set_dispersion_mode(SWMM_Engine engine, int mode);

/**
 * @brief Read the fixed dispersion coefficient.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[out] value Non-null caller-owned slot for authored ft2/s or m2/s, not converted by
 * this getter.
 * @return SWMM_OK on success, SWMM_ERR_BADHANDLE for an invalid handle, or
 * SWMM_ERR_BADPARAM for invalid arguments. Output storage is caller-owned.
 * @note Reads authored configuration; no transport runtime is advanced.
 */
SWMM_ENGINE_API int swmm_transport_get_dispersion_value(SWMM_Engine engine, double* value);

/**
 * @brief Assign the fixed dispersion coefficient and mark transport configuration present.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] value Finite nonnegative authored coefficient in ft2/s or m2/s.
 * @return SWMM_OK on success, SWMM_ERR_BADHANDLE for an invalid handle, or
 * SWMM_ERR_BADPARAM for invalid arguments. Output storage is caller-owned.
 * @note This option setter performs no editable-state check. It changes
 * stored configuration and does not promise to rebuild an initialized transport kernel.
 */
SWMM_ENGINE_API int swmm_transport_set_dispersion_value(SWMM_Engine engine, double value);

/**
 * @brief Read the requested transport-cell spacing.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[out] dx Non-null caller-owned slot for authored feet or meters; zero selects the
 * engine default.
 * @return SWMM_OK on success, SWMM_ERR_BADHANDLE for an invalid handle, or
 * SWMM_ERR_BADPARAM for invalid arguments. Output storage is caller-owned.
 * @note Reads authored configuration; no transport runtime is advanced.
 */
SWMM_ENGINE_API int swmm_transport_get_target_dx(SWMM_Engine engine, double* dx);

/**
 * @brief Assign the requested transport-cell spacing and mark transport configuration present.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] dx Finite nonnegative authored length in feet/meters; zero restores the engine
 * default.
 * @return SWMM_OK on success, SWMM_ERR_BADHANDLE for an invalid handle, or
 * SWMM_ERR_BADPARAM for invalid arguments. Output storage is caller-owned.
 * @note This option setter performs no editable-state check. It changes
 * stored configuration and does not promise to rebuild an initialized transport kernel.
 */
SWMM_ENGINE_API int swmm_transport_set_target_dx(SWMM_Engine engine, double dx);

/**
 * @brief Count authored per-conduit dispersion override rows.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[out] count Non-null caller-owned nonnegative row count.
 * @return SWMM_OK on success, SWMM_ERR_BADHANDLE for an invalid handle, or
 * SWMM_ERR_BADPARAM for invalid arguments. Output storage is caller-owned.
 * @note Counts raw configuration rows, including rows awaiting BUILDING-state resolution.
 */
SWMM_ENGINE_API int swmm_transport_conduit_disp_count(SWMM_Engine engine, int* count);

/**
 * @brief Read one authored conduit-dispersion override.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] index Zero-based override row index, below swmm_transport_conduit_disp_count().
 * @param[out] link_index Optional output slot for the global link index; NULL skips it.
 * @param[out] value Optional output slot for the authored ft2/s or m2/s coefficient.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE or SWMM_ERR_BADINDEX for an invalid row.
 * @note Both output pointers may be NULL; no storage is retained.
 */
SWMM_ENGINE_API int swmm_transport_get_conduit_disp(SWMM_Engine engine, int index, int* link_index,
                                                    double* value);

/**
 * @brief Count authored transport boundary rows.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[out] count Non-null caller-owned nonnegative row count.
 * @return SWMM_OK on success, SWMM_ERR_BADHANDLE for an invalid handle, or
 * SWMM_ERR_BADPARAM for invalid arguments. Output storage is caller-owned.
 * @note Counts raw configuration rows, including rows awaiting BUILDING-state resolution.
 */
SWMM_ENGINE_API int swmm_transport_boundary_count(SWMM_Engine engine, int* count);

/**
 * @brief Copy an authored transport boundary row with optional output fields.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] index Zero-based raw row index, below its count function.
 * @param[out] element Optional writable node-name buffer; NULL skips copying.
 * @param[in] elem_len Element-buffer byte capacity; <= 0 skips copying, positive
 * capacity truncates safely to len - 1 bytes and appends NUL.
 * @param[out] species Optional species-name buffer.
 * @param[in] spec_len Species-buffer byte capacity; same skip/truncation convention.
 * @param[out] is_ts Optional slot: 1 time-series mode, 0 constant-value mode.
 * @param[out] value Optional constant field in species concentration units.
 * @param[out] ts_name Optional time-series-name buffer; empty for constant mode.
 * @param[in] ts_len Series-buffer byte capacity; same skip/truncation convention.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE or SWMM_ERR_BADINDEX.
 * @note All output pointers may be NULL. Names/values are raw authored fields,
 * not sampled series values; no storage is retained.
 */
SWMM_ENGINE_API int swmm_transport_get_boundary(SWMM_Engine engine, int index, char* element,
                                                int elem_len, char* species, int spec_len,
                                                int* is_ts, double* value, char* ts_name,
                                                int ts_len);

/**
 * @brief Count authored transport source rows.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[out] count Non-null caller-owned nonnegative row count.
 * @return SWMM_OK on success, SWMM_ERR_BADHANDLE for an invalid handle, or
 * SWMM_ERR_BADPARAM for invalid arguments. Output storage is caller-owned.
 * @note Counts raw configuration rows, including rows awaiting BUILDING-state resolution.
 */
SWMM_ENGINE_API int swmm_transport_source_count(SWMM_Engine engine, int* count);

/**
 * @brief Copy an authored transport source row with optional output fields.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] index Zero-based raw row index, below its count function.
 * @param[out] element Optional writable conduit-name buffer; NULL skips copying.
 * @param[in] elem_len Element-buffer byte capacity; <= 0 skips copying, positive
 * capacity truncates safely to len - 1 bytes and appends NUL.
 * @param[out] species Optional species-name buffer.
 * @param[in] spec_len Species-buffer byte capacity; same skip/truncation convention.
 * @param[out] is_ts Optional slot: 1 time-series mode, 0 constant-value mode.
 * @param[out] value Optional constant field in species-native mass units per second; negative
 * values represent extraction.
 * @param[out] ts_name Optional time-series-name buffer; empty for constant mode.
 * @param[in] ts_len Series-buffer byte capacity; same skip/truncation convention.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE or SWMM_ERR_BADINDEX.
 * @note All output pointers may be NULL. Names/values are raw authored fields,
 * not sampled series values; no storage is retained.
 */
SWMM_ENGINE_API int swmm_transport_get_source(SWMM_Engine engine, int index, char* element,
                                              int elem_len, char* species, int spec_len, int* is_ts,
                                              double* value, char* ts_name, int ts_len);

/* =========================================================================
 * Authoring (closed C-API gaps 2026-10-11)
 *
 * The setters below edit the transport.ard configuration that the
 * serializer writes back to the component file, so API edits persist on
 * save. They are accepted while the model is being edited (BUILDING or
 * OPENED) and return SWMM_ERR_LIFECYCLE afterwards. Each edit marks the
 * configuration as present (swmm_transport_get_configured() == 1).
 * ========================================================================= */

/**
 * @brief Read the configured detailed transport CSV sidecar path.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[out] buf Non-null writable string buffer; empty when detailed output is disabled.
 * @param[in] buflen Positive byte capacity including terminator; long paths are safely truncated.
 * @return SWMM_OK on success, SWMM_ERR_BADHANDLE for an invalid handle, or
 * SWMM_ERR_BADPARAM for invalid arguments. Output storage is caller-owned.
 * @note Returns stored text without resolving or opening the destination.
 */
SWMM_ENGINE_API int swmm_transport_get_detailed_output(SWMM_Engine engine, char* buf, int buflen);

/**
 * @brief Store a detailed transport CSV sidecar path and mark configuration present.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] path Null-terminated path to copy; NULL or empty disables the sidecar.
 * @return SWMM_OK on success, SWMM_ERR_BADHANDLE for an invalid handle, or
 * SWMM_ERR_BADPARAM for invalid arguments. Output storage is caller-owned.
 * @note Requires an editable BUILDING or OPENED model; later states return
 * SWMM_ERR_LIFECYCLE. Input data is copied and no input storage is retained.
 * @note Relative API-authored paths are resolved against the process working
 * directory when output is written; component-file parsing resolves paths
 * against its own directory. This call does not open the file.
 */
SWMM_ENGINE_API int swmm_transport_set_detailed_output(SWMM_Engine engine, const char* path);

/**
 * @brief Add or replace the dispersion coefficient for one conduit.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] link_index Valid global link index identifying a conduit; structures are refused.
 * @param[in] value Finite nonnegative coefficient in authored ft2/s or m2/s.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE, SWMM_ERR_BADINDEX, SWMM_ERR_BADPARAM
 * for a nonconduit/invalid coefficient, or SWMM_ERR_LIFECYCLE.
 * @note Requires an editable BUILDING or OPENED model; later states return
 * SWMM_ERR_LIFECYCLE. Input data is copied and no input storage is retained.
 * @note Marks transport configuration present; replacement keeps the existing row position.
 */
SWMM_ENGINE_API int swmm_transport_set_conduit_disp(SWMM_Engine engine, int link_index,
                                                    double value);

/**
 * @brief Remove an authored conduit dispersion row by index.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] index Zero-based current row index; removing a row shifts later indices.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE, SWMM_ERR_BADINDEX or SWMM_ERR_LIFECYCLE.
 * @note Requires an editable BUILDING or OPENED model; later states return
 * SWMM_ERR_LIFECYCLE. Input data is copied and no input storage is retained.
 * @note Does not clear the component configured flag when the final override is removed.
 */
SWMM_ENGINE_API int swmm_transport_remove_conduit_disp(SWMM_Engine engine, int index);

/**
 * @brief Upsert a transport boundary keyed by element and species names.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] element Non-null nonempty node name, copied into the row.
 * @param[in] species Non-null nonempty reactions-component species name.
 * @param[in] is_ts Exactly 0 for a constant or 1 for a named time series.
 * @param[in] value Finite nonnegative concentration in species units
 * when is_ts is zero; ignored and stored as zero otherwise.
 * @param[in] ts_name Nonempty existing time-series name when is_ts == 1;
 * may be NULL for constant mode and is then stored as empty text.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE, SWMM_ERR_BADPARAM for invalid data or
 * failed name resolution, or SWMM_ERR_LIFECYCLE.
 * @note Requires an editable BUILDING or OPENED model; later states return
 * SWMM_ERR_LIFECYCLE. Input data is copied and no input storage is retained.
 * @note BUILDING keeps unresolved rows so referenced objects can be declared
 * later. OPENED reruns row resolution and restores the prior configuration on
 * failure. Successful edits mark the component configured.
 */
SWMM_ENGINE_API int swmm_transport_set_boundary(SWMM_Engine engine, const char* element,
                                                const char* species, int is_ts, double value,
                                                const char* ts_name);

/**
 * @brief Remove an authored boundary row by index.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] index Zero-based current row index; removing a row shifts later indices.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE, SWMM_ERR_BADINDEX or SWMM_ERR_LIFECYCLE.
 * @note Requires an editable BUILDING or OPENED model; later states return
 * SWMM_ERR_LIFECYCLE. Input data is copied and no input storage is retained.
 * @note Boundary/source resolution is rerun outside BUILDING; a failed resolution restores the
 * previous configuration and returns SWMM_ERR_BADPARAM.
 */
SWMM_ENGINE_API int swmm_transport_remove_boundary(SWMM_Engine engine, int index);

/**
 * @brief Upsert a transport source keyed by element and species names.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] element Non-null nonempty conduit name, copied into the row.
 * @param[in] species Non-null nonempty reactions-component species name.
 * @param[in] is_ts Exactly 0 for a constant or 1 for a named time series.
 * @param[in] value Finite rate in species-native mass units/s; negative is extraction
 * when is_ts is zero; ignored and stored as zero otherwise.
 * @param[in] ts_name Nonempty existing time-series name when is_ts == 1;
 * may be NULL for constant mode and is then stored as empty text.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE, SWMM_ERR_BADPARAM for invalid data or
 * failed name resolution, or SWMM_ERR_LIFECYCLE.
 * @note Requires an editable BUILDING or OPENED model; later states return
 * SWMM_ERR_LIFECYCLE. Input data is copied and no input storage is retained.
 * @note BUILDING keeps unresolved rows so referenced objects can be declared
 * later. OPENED reruns row resolution and restores the prior configuration on
 * failure. Successful edits mark the component configured.
 */
SWMM_ENGINE_API int swmm_transport_set_source(SWMM_Engine engine, const char* element,
                                              const char* species, int is_ts, double value,
                                              const char* ts_name);

/**
 * @brief Remove an authored source row by index.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] index Zero-based current row index; removing a row shifts later indices.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE, SWMM_ERR_BADINDEX or SWMM_ERR_LIFECYCLE.
 * @note Requires an editable BUILDING or OPENED model; later states return
 * SWMM_ERR_LIFECYCLE. Input data is copied and no input storage is retained.
 * @note Boundary/source resolution is rerun outside BUILDING; a failed resolution restores the
 * previous configuration and returns SWMM_ERR_BADPARAM.
 */
SWMM_ENGINE_API int swmm_transport_remove_source(SWMM_Engine engine, int index);

#ifdef __cplusplus
}
#endif

#endif /* OPENSWMM_TRANSPORT_H */
