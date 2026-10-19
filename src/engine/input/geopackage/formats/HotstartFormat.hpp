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
 * @file HotstartFormat.hpp
 * @brief Legacy HSF v4 binary hot-start file — parser / materialiser.
 *
 * @details Slice IO-6e. Implements the routing-state portion of the
 *          EPA-SWMM hot-start file (`saveRouting`/`readRouting` in
 *          `src/legacy/engine/hotstart.c`):
 *
 *          - Header: "SWMM5-HOTSTART4" + `{nSubcatch, nLandUses, nNodes,
 *            nLinks, nPollut, flowUnits}` (six int32 fields).
 *          - Per-node state: float depth, lateral_inflow + nPollut quality
 *            floats. **`overflow` is not in the legacy HSF**; it is
 *            preserved through the GPKG schema only and round-trips
 *            through HSF as 0.0.
 *          - Per-link state: float flow, depth, setting + nPollut quality
 *            floats. The other GPKG `hotstart_link_state` fields
 *            (`volume`, `target_setting`, `time_open`, `time_closed`)
 *            have no HSF counterpart and round-trip through HSF as 0.0.
 *
 *          - The subcatchment-runoff section (saveRunoff/readRunoff),
 *            whose length depends on the model (`HsfLayout`), and anything
 *            after the link records (an OpenSWMM trailer) are carried
 *            verbatim, so a file round-trips byte for byte. A v4 file's
 *            per-storage-node residence time float is decoded.
 *
 *          Endianness: HSF is written little-endian on all SWMM5
 *          platforms.  We rely on the SWMM5 convention (no byte-swapping)
 *          and document the assumption.
 *
 * @ingroup engine_geopackage
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_GEOPACKAGE_HOTSTART_FORMAT_HPP
#define OPENSWMM_GEOPACKAGE_HOTSTART_FORMAT_HPP

#include "FormatTypes.hpp"

#include <string>

namespace openswmm::gpkg::formats {

/**
 * @brief Parse an HSF v4 file into a `HotstartSnapshot`.
 *
 * @details Populates `header.{num_*,flow_units,file_version,file_stamp}`,
 *          `node_state[]`, `link_state[]`, `runoff_raw` and `tail_raw`.
 *          A file with subcatchments needs `layout` (the model's
 *          subcatchment count must match the file's); v1/v2 files with
 *          subcatchments are refused.
 *
 * @param path      Filesystem path to the HSF file.
 * @param snapshot  Output snapshot (existing content is overwritten).
 */
FormatResult parseHotstartHsf(const std::string&  path,
                               HotstartSnapshot&  snapshot,
                               const HsfLayout&   layout = {});

/**
 * @brief Write a `HotstartSnapshot` to an HSF v4 file at `path`.
 *
 * @details Header counts must already match the lengths of the per-object
 *          state vectors; mismatches return a `FormatResult` with `ok=false`
 *          rather than writing a corrupt file. `runoff_raw` and `tail_raw`
 *          are written as they are; without `runoff_raw` the subcatchment
 *          count is written as 0.
 */
FormatResult writeHotstartHsf(const std::string&        path,
                               const HotstartSnapshot&   snapshot);

} // namespace openswmm::gpkg::formats

#endif // OPENSWMM_GEOPACKAGE_HOTSTART_FORMAT_HPP
