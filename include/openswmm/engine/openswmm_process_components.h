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

#ifdef __cplusplus
}
#endif

#endif /* OPENSWMM_PROCESS_COMPONENTS_H */
