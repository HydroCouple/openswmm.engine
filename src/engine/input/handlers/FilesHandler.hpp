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
 * @file      FilesHandler.hpp
 * @brief     [FILES] section handler — secondary file references.
 *
 * @details Parses the legacy SWMM5 `[FILES]` section into
 *          `SimulationContext::files`.  Row format:
 *
 * @code
 * [FILES]
 * SAVE/USE   FileType   "path"   [date  time]
 * @endcode
 *
 *          Where FileType ∈ {RAINFALL, RUNOFF, RDII, INFLOWS, OUTFLOWS,
 *          HOTSTART}.  The optional date/time tokens apply only to
 *          `SAVE HOTSTART` and are stored on the `hotstart_save_datetime`
 *          field as a fractional-day SWMM date.
 *
 *          Multi-row `SAVE HOTSTART` (legacy supported up to 10) is not
 *          yet honoured: each new SAVE row overwrites the previous slot.
 *          Lift to a vector when a real workflow needs it.
 *
 * @see src/engine/core/SimulationContext.hpp — `FilesSpec` struct.
 * @see src/legacy/engine/iface.c — legacy parser this mirrors.
 * @ingroup engine_input
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_FILES_HANDLER_HPP
#define OPENSWMM_ENGINE_FILES_HANDLER_HPP

#include <vector>
#include <string>

namespace openswmm {
struct SimulationContext;
}

namespace openswmm::input {

/**
 * @brief Parse the `[FILES]` section into `ctx.files`.
 *
 * @param ctx    Simulation context to populate.
 * @param lines  Non-comment, non-empty lines from the section.
 */
void handle_files(SimulationContext& ctx, const std::vector<std::string>& lines);

/**
 * @brief Parse the [RESTART] section into SimulationContext::restart.
 *
 * Rows are `KEY value...`: SOURCE_FILE, SOURCE_RUN, SOURCE_RECORD, MODE, RECORD_FILE,
 * RECORD_FORMAT, RECORD_AT (END or date time, repeatable), RECORD_EVERY (HH:MM:SS or seconds) and
 * RECORD_CONTINUATION (YES or NO). An unknown key or value is an input error; whether the settings
 * agree with each other and with [FILES] is checked when the engine initializes.
 *
 * @param[in,out] ctx Context receiving the settings and any input errors.
 * @param[in] lines Borrowed raw section rows without the header.
 */
void handle_restart(SimulationContext& ctx, const std::vector<std::string>& lines);

} /* namespace openswmm::input */

#endif /* OPENSWMM_ENGINE_FILES_HANDLER_HPP */
