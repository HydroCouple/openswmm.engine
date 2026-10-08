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
 * @file openswmm_process_components.h
 * @brief C API — [PROCESS_COMPONENTS] registrations (E-C3 subset of the
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
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_PROCESS_COMPONENTS_H
#define OPENSWMM_PROCESS_COMPONENTS_H

#include "openswmm_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Number of [PROCESS_COMPONENTS] registrations. -1 on bad handle. */
SWMM_ENGINE_API int swmm_process_component_count(SWMM_Engine engine);

/**
 * @brief Read one registration.
 * @param id_buf        [out] Component id (NUL-terminated).
 * @param config_buf    [out] The config="…" argument as written (may be "").
 * @param resolved_buf  [out] Effective path the config was READ from at the
 *                      last open ("" until resolution).
 */
SWMM_ENGINE_API int swmm_process_component_get(SWMM_Engine engine, int idx,
        char* id_buf, int id_len, char* config_buf, int config_len,
        char* resolved_buf, int resolved_len);

/** @brief Registration index for @p id, or -1. */
SWMM_ENGINE_API int swmm_process_component_find(SWMM_Engine engine,
        const char* id);

/**
 * @brief Register a component (BUILDING/OPENED). Duplicate id refused.
 *        The config file need not exist yet.
 */
SWMM_ENGINE_API int swmm_process_component_register(SWMM_Engine engine,
        const char* id, const char* config_path);

/** @brief Remove registration @p idx (BUILDING/OPENED). Indexes shift. */
SWMM_ENGINE_API int swmm_process_component_remove(SWMM_Engine engine,
        int idx);

/* ---- Built-in component catalogue (U1, 2026-09-07) -----------------------
 * The ids the engine knows how to bind, independent of any model: the six
 * planned/implemented built-ins of the process-global registry. This is the
 * catalogue the GUI's Process Components table offers in its Id combo. The
 * external component-library discovery (`swmm_component_library_*`,
 * PROG stream D) is a separate, later seam; this one never scans disk.
 */

/** @brief Number of built-in component ids. */
SWMM_ENGINE_API int swmm_process_component_known_count(void);

/**
 * @brief Read built-in id @p idx.
 * @param id_buf       [out] The id (e.g. "org.hydrocouple.openswmm.reactions").
 * @param desc_buf     [out] Short description.
 * @param implemented  [out] 1 when the component has an apply hook in this
 *                     build, 0 when it is a planned placeholder (its
 *                     registration line is diagnosed at open).
 */
SWMM_ENGINE_API int swmm_process_component_known_get(int idx,
        char* id_buf, int id_len, char* desc_buf, int desc_len,
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
    char id[256];          /**< component id; "" when refused */
    char caption[256];
    char version[64];
    char path[1024];       /**< the library file */
    char kind[16];         /**< "model" | "other"; "" when refused */
    char stamp[256];       /**< the library's own ABI stamp, or "unstamped(…)" */
    char load_error[1024]; /**< "" on success; the reason otherwise */
} SWMM_ComponentLibraryInfo;

/**
 * @brief Scan @p dir for component libraries now, and keep it for later.
 * @return SWMM_OK; SWMM_ERR_BADPARAM if @p dir is null or not a directory;
 *         SWMM_ERR_PLUGIN if the engine was built without HydroCouple support.
 */
SWMM_ENGINE_API int swmm_component_search_path_add(const char* dir);

/** @brief Number of recognised component libraries, refused ones included. */
SWMM_ENGINE_API int swmm_component_library_count(void);

/**
 * @brief Read recognised library @p idx.
 * @return SWMM_OK; SWMM_ERR_BADINDEX; SWMM_ERR_BADPARAM if @p info is null.
 */
SWMM_ENGINE_API int swmm_component_library_get(int idx,
        SWMM_ComponentLibraryInfo* info);

#ifdef __cplusplus
}
#endif

#endif /* OPENSWMM_PROCESS_COMPONENTS_H */
