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
 * @file InpWriter.hpp
 * @brief Write a SimulationContext to a SWMM .inp file.
 *
 * @details Serialises all model data from SoA arrays into standard SWMM
 *          input file format. Supports all sections from the legacy format
 *          plus new sections ([USER_FLAGS], [PLUGINS]).
 *
 * @ingroup engine_core
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_INP_WRITER_HPP
#define OPENSWMM_INP_WRITER_HPP

#include <string>
#include <vector>

namespace openswmm {

struct SimulationContext;

namespace inp_writer {

/**
 * @brief Write the full model to a SWMM .inp file.
 *
 * @details Sections written (in order):
 *   [TITLE], [OPTIONS],
 *   [EVAPORATION], [TEMPERATURE], [SNOWPACKS], [ADJUSTMENTS],
 *   [RAINGAGES], [SUBCATCHMENTS], [SUBAREAS],
 *   [INFILTRATION], [JUNCTIONS], [OUTFALLS], [STORAGE], [DIVIDERS],
 *   [CONDUITS], [PUMPS], [ORIFICES], [WEIRS], [OUTLETS],
 *   [XSECTIONS], [TRANSECTS], [LOSSES], [CONTROLS], [REPORT],
 *   [INFLOWS], [DWF], [RDII],
 *   [POLLUTANTS], [LANDUSES], [BUILDUP], [WASHOFF], [TREATMENT],
 *   [TIMESERIES], [CURVES], [PATTERNS],
 *   [USER_FLAGS], [PLUGINS],
 *   [MAP], [COORDINATES], [VERTICES], [Polygons], [SYMBOLS]
 *
 *   Slice IO-4: every external-file path slot (`[FILES]`, `[RAINGAGES]
 *   FILE`, `[TIMESERIES] FILE`, `[TEMPERATURE] FILE`, etc.) is emitted
 *   *relative to* the destination directory by default. Set
 *   `ctx.options.write_absolute_paths = true` to disable rebasing.
 *   Slots that cannot be expressed relatively (cross-volume on Windows,
 *   UNC roots, beyond the depth cap) fall back to absolute form and
 *   append a human-readable explanation to `warnings` when that vector
 *   is non-null.
 *
 * @param ctx       Simulation context with all model data.
 * @param path      Output file path.
 * @param warnings  Optional sink for non-fatal portability warnings
 *                  (cross-volume slots, etc.). Pass nullptr to discard.
 * @returns 0 on success, -1 on file error.
 */
int writeInpFile(const SimulationContext& ctx,
                 const std::string&       path,
                 std::vector<std::string>* warnings = nullptr);

/**
 * @brief Write profile: which engine the file is written for.
 *
 * @details `Full` is the native format. `Swmm5` writes a file a SWMM 5.x
 *          engine can read (MULTI_ENGINE plan V2 Phase 4): the v6-only
 *          sections are omitted ([2D_*], [PLUGINS], [PROCESS_COMPONENTS],
 *          [USER_FLAGS], [USER_FLAG_VALUES], [RDII_DECAY]), v6-only option
 *          keys are omitted and incompatible values mapped (FLOW_ROUTING FV →
 *          DYNWAVE, SURCHARGE_METHOD DYNAMIC_SLOT/TPA → SLOT), a virtual
 *          junction becomes an ordinary junction, and an inlet junction
 *          becomes an ordinary junction plus an [INLET_USAGE] row on its
 *          approach conduit with the same capture node (the legacy-equivalent
 *          model). Every substitution is reported through `warnings`; the
 *          file starts with a comment naming the profile.
 */
struct InpWriteOptions {
    enum class Profile { Full, Swmm5 };
    Profile profile = Profile::Full;
};

int writeInpFile(const SimulationContext&  ctx,
                 const std::string&        path,
                 std::vector<std::string>* warnings,
                 const InpWriteOptions&    opts);

} // namespace inp_writer
} // namespace openswmm

#endif // OPENSWMM_INP_WRITER_HPP
