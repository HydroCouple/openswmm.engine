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
 * @file      openswmm_process_components.h
 * @brief     C API — [PROCESS_COMPONENTS] registrations (E-C3 subset of the
 *        transport IO plan §5): enumerate, find, register, remove.
 *
 * @details The GUI's file-binding surface: an editor locates its component's
 *          config path here, and the "create component + config file" flow
 *          registers first, then writes the file (registration with a
 *          not-yet-existing config path is legal — resolve reads it at the
 *          next open).
 *
 * @ingroup engine_api
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_PROCESS_COMPONENTS_H
#define OPENSWMM_PROCESS_COMPONENTS_H

#include "openswmm_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Count stored model process-component registrations.
 *
 * Counts authoring entries, not discovered libraries or initialized runtime instances.
 *
 * @param[in] engine Non-null live engine with exclusive caller access.
 * @return Nonnegative registration count, or -1 for null engine.
 */
SWMM_ENGINE_API int swmm_process_component_count(SWMM_Engine engine);

/**
 * @brief Copy one process-component registration and its last resolved config path.
 *
 * This is metadata retrieval only; it does not load the component or reread its configuration.
 *
 * @param[in] engine Non-null live engine with exclusive caller access.
 * @param[in] idx Zero-based registration index.
 * @param[out] id_buf Non-null caller-owned component-ID buffer, NUL-terminated and silently
 *     truncated to id_len-1 bytes.
 * @param[in] id_len Positive ID capacity in bytes including NUL.
 * @param[out] config_buf Non-null caller-owned authored config-path buffer; empty is allowed,
 *     NUL-terminated/truncated.
 * @param[in] config_len Positive config capacity including NUL.
 * @param[out] resolved_buf Non-null caller-owned resolved effective path buffer; empty until
 *     resolution, NUL-terminated/truncated.
 * @param[in] resolved_len Positive resolved-path capacity including NUL.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for invalid idx, or
 *     SWMM_ERR_BADPARAM for a null/nonpositive-capacity buffer.
 */
SWMM_ENGINE_API int swmm_process_component_get(SWMM_Engine engine, int idx, char* id_buf,
                                               int id_len, char* config_buf, int config_len,
                                               char* resolved_buf, int resolved_len);

/**
 * @brief Find a model registration by exact component ID.
 *
 * @param[in] engine Non-null live engine with exclusive caller access.
 * @param[in] id Non-null NUL-terminated ID, matched case-sensitively.
 * @return Zero-based registration index, or -1 for absent ID/null engine/null ID.
 */
SWMM_ENGINE_API int swmm_process_component_find(SWMM_Engine engine, const char* id);

/**
 * @brief Append an editable process-component registration.
 *
 * Does not resolve the ID against a catalogue or initialize/read the config immediately. Normal
 * model resolution at a subsequent open consumes the registration; no automatic config file is
 * created.
 *
 * @param[in] engine Non-null live engine with exclusive caller access.
 * @param[in] id Non-null, nonempty NUL-terminated component ID to copy; an exact existing ID is
 *     refused.
 * @param[in] config_path Optional copied config path token; NULL means no config argument. The file
 *     need not exist yet.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_LIFECYCLE outside BUILDING/OPENED,
 *     or SWMM_ERR_BADPARAM for null/empty/duplicate ID.
 */
SWMM_ENGINE_API int swmm_process_component_register(SWMM_Engine engine, const char* id,
                                                    const char* config_path);

/**
 * @brief Remove a model registration and shift later indices down.
 *
 * Does not delete its config file or independently unload a runtime instance.
 *
 * @param[in] engine Non-null live engine with exclusive caller access.
 * @param[in] idx Zero-based existing registration index.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_LIFECYCLE outside BUILDING/OPENED,
 *     or SWMM_ERR_BADINDEX for invalid idx.
 */
SWMM_ENGINE_API int swmm_process_component_remove(SWMM_Engine engine, int idx);

/* ---- Built-in component catalogue (U1, 2026-09-07) -----------------------
 * The ids the engine knows how to bind, independent of any model: the six
 * planned/implemented built-ins of the process-global registry. This is the
 * catalogue the GUI's Process Components table offers in its Id combo. The
 * external component-library discovery (`swmm_component_library_*`,
 * PROG stream D) is a separate, later seam; this one never scans disk.
 */

/**
 * @brief Count process-global built-in catalogue entries.
 *
 * Independent of any model handle and does not scan disk.
 * @return Number of known built-ins, including entries with no apply hook.
 */
SWMM_ENGINE_API int swmm_process_component_known_count(void);

/**
 * @brief Read one process-global built-in catalogue entry.
 *
 * Optional outputs may all be suppressed; this does not validate whether a particular model/config
 * can activate the built-in.
 *
 * @param[in] idx Zero-based index in [0,swmm_process_component_known_count()).
 * @param[out] id_buf Optional caller-owned ID buffer, NUL-terminated/truncated when id_len>0.
 * @param[in] id_len ID capacity including NUL; nonpositive suppresses ID output.
 * @param[out] desc_buf Optional caller-owned description buffer, NUL-terminated/truncated when
 *     desc_len>0.
 * @param[in] desc_len Description capacity including NUL; nonpositive suppresses description
 *     output.
 * @param[out] implemented Optional destination: 1 when an apply hook is present in this build, 0
 *     for a placeholder.
 * @return SWMM_OK; SWMM_ERR_BADINDEX for invalid idx or SWMM_ERR_INTERNAL for an inconsistent
 *     registry entry.
 */
SWMM_ENGINE_API int swmm_process_component_known_get(int idx, char* id_buf, int id_len,
                                                     char* desc_buf, int desc_len,
                                                     int* implemented);

/* ------------------------------------------------------------------------
 * D2 (program plan §B.2.5) — discovered HydroCouple component libraries.
 *
 * Process-global, like the built-in catalogue above: no engine handle. A
 * component library is a shared library exporting HydroCouple's loader
 * contract (`hydrocouple_component_abi_v1` + `hydrocouple_component_info_v1`,
 * from HydroCouple's hydrocouplecomponentabi.h), or the legacy
 * `CreateComponentInfo`.
 *
 * Discovery (§B.2.3) runs once, on the first call to either query below:
 * the engine library's directory and its plugins/ and components/
 * subdirectories, then each directory in the path-list environment
 * variable HYDROCOUPLE_COMPONENT_PATH (':'-separated; ';' on Windows).
 * swmm_component_search_path_add() scans a further directory immediately.
 *
 * Every library the engine RECOGNISED as a component is listed — including
 * ones it refused. A stamped library whose ABI stamp differs from the
 * engine's is never called beyond its stamp, and is listed with both stamps
 * in `load_error`. A legacy library is loaded best-effort and listed with
 * stamp "unstamped(legacy CreateComponentInfo)", so it is never mistaken
 * for a verified one. Libraries that are not components are not listed.
 *
 * Built without OPENSWMM_WITH_HYDROCOUPLE, discovery finds nothing: the
 * count is 0 and swmm_component_search_path_add returns SWMM_ERR_PLUGIN.
 *
 * Argument / exchange-item / capability enumeration
 * (swmm_component_info_*) is a later part of D2 and not yet available.
 * ------------------------------------------------------------------------ */

/** @brief One discovered (or refused) component library. Strings are
 *         NUL-terminated and truncated to their field size. */
typedef struct SWMM_ComponentLibraryInfo {
    char id[256]; /**< component id; "" when refused */
    char caption[256];
    char version[64];
    char path[1024];       /**< the library file */
    char kind[16];         /**< "model" | "other"; "" when refused */
    char stamp[256];       /**< the library's own ABI stamp, or "unstamped(…)" */
    char load_error[1024]; /**< "" on success; the reason otherwise */
} SWMM_ComponentLibraryInfo;

/**
 * @brief Scan an additional directory for HydroCouple component libraries.
 *
 * Discovery can load recognized shared libraries to inspect their component ABI. Recognized refused
 * libraries remain in the catalogue with load_error; successful scanning does not imply every
 * library was accepted. See the header discovery/stamp contract.
 *
 * @param[in] dir Non-null, nonempty NUL-terminated directory path, copied into the process-global
 *     search configuration.
 * @return SWMM_OK on accepted scan; SWMM_ERR_BADPARAM for null/empty/invalid directory, or
 *     SWMM_ERR_PLUGIN when built without HydroCouple.
 */
SWMM_ENGINE_API int swmm_component_search_path_add(const char* dir);

/**
 * @brief Count recognized discovered component libraries, including refused candidates.
 *
 * Triggers normal initial discovery when required. No engine handle is needed; this count is
 * distinct from model process-component registrations.
 * @return Nonnegative process-global catalogue size; zero when no component libraries are
 *     recognized (including builds without HydroCouple).
 */
SWMM_ENGINE_API int swmm_component_library_count(void);

/**
 * @brief Copy metadata for one recognized component-library candidate.
 *
 * May trigger initial discovery. Inspect load_error before treating the ID/caption/version as an
 * accepted usable library; a returned catalogue entry can represent an ABI refusal.
 *
 * @param[in] idx Zero-based index in [0,swmm_component_library_count()).
 * @param[out] info Non-null caller-owned fixed-size metadata destination. All string fields are
 *     NUL-terminated and silently truncated to their own array capacity.
 * @return SWMM_OK; SWMM_ERR_BADPARAM for null info or SWMM_ERR_BADINDEX for invalid idx.
 */
SWMM_ENGINE_API int swmm_component_library_get(int idx, SWMM_ComponentLibraryInfo* info);

#ifdef __cplusplus
}
#endif

#endif /* OPENSWMM_PROCESS_COMPONENTS_H */
