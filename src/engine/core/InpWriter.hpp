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

} // namespace inp_writer
} // namespace openswmm

#endif // OPENSWMM_INP_WRITER_HPP
