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
 * @file ExternalContentWriter.hpp
 * @brief Slice IO-7 — fan-out from in-memory external-file slots to the
 *        Part D GeoPackage tables.
 *
 * @details Walks every external-file slot on `SimulationContext`
 *          (`ctx.files`, `ctx.gages.file_path[]`, `ctx.options.temp_file`,
 *          file-backed `ctx.tables.tables[i].file_path`) and:
 *
 *            1. If the slot's resolved path points to a readable file,
 *               invokes the matching format parser from Slice IO-6 and
 *               inserts the parsed rows into the Part D content tables
 *               (raingage_data, climate_data, input_timeseries with
 *               provenance, routing_interface_*, hotstart_*).
 *
 *            2. If the slot is `SAVE`-direction and the file does not
 *               exist yet, a `status='pending'` parent row is written
 *               (hotstart_slots only — other SAVE slots don't carry an
 *               explicit pending registry today) so the engine's output
 *               plugin can populate it after the simulation finishes.
 *
 *            3. If the slot is `USE`-direction but the referenced file
 *               is missing, the writer throws a `GpkgError` — saving a
 *               model that references unreadable USE files would
 *               silently lose the inputs.
 *
 *          Per the plan §3.5 the writer presumes `PostParseResolver` has
 *          already populated `slot.absolute`. Programmatic models with
 *          only `slot.original` set are handled too — the helper falls
 *          back to the original token verbatim and lets `fopen` decide.
 *
 * @ingroup engine_geopackage
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_GEOPACKAGE_EXTERNAL_CONTENT_WRITER_HPP
#define OPENSWMM_GEOPACKAGE_EXTERNAL_CONTENT_WRITER_HPP

#include <string>

struct sqlite3;

namespace openswmm {
    struct SimulationContext;
}

namespace openswmm::gpkg {

/**
 * @brief Populate the Part D content tables for `simulation_id` from
 *        the in-memory model.
 *
 * @throws GpkgError when a USE-direction slot references a missing file
 *         or when a format parser reports a hard failure.
 */
void write_external_content(sqlite3*                          db,
                             const openswmm::SimulationContext& ctx,
                             const std::string&                 simulation_id);

} // namespace openswmm::gpkg

#endif // OPENSWMM_GEOPACKAGE_EXTERNAL_CONTENT_WRITER_HPP
