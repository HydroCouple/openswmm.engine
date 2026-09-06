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
 * @file RoutingInterfaceFormat.hpp
 * @brief SWMM5 routing interface text format — parser / materialiser.
 *
 * @details Slice IO-6b. Reads and writes the EPA-SWMM routing interface
 *          text file format produced and consumed by the legacy
 *          `iface.c::openFileForOutput` / `readIfaceFileHeader`. Layout:
 *
 *  @code
 *  SWMM5 Interface File
 *  <title>
 *  <step>  - reporting time step in sec
 *  <N+1>   - number of constituents as listed below:
 *  FLOW   <flow-units>
 *  <pollut1>  <units>
 *  ...
 *  <M>     - number of nodes as listed below:
 *  <node1>
 *  <node2>
 *  ...
 *  Node             Year Mon Day Hr  Min Sec FLOW      <pollut1> ...
 *  <node>  <yyyy> <mm> <dd> <hh> <mm> <ss>  <flow>  <pollut1_value> ...
 *  ...
 *  @endcode
 *
 *          NOTE: the EPA-SWMM interface file is text, not binary
 *          (clarification vs. the plan §3.6 v1 table, which originally
 *          said "binary" — the legacy implementation in `iface.c` writes
 *          via `fprintf` exclusively).
 *
 * @ingroup engine_geopackage
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_GEOPACKAGE_ROUTING_INTERFACE_FORMAT_HPP
#define OPENSWMM_GEOPACKAGE_ROUTING_INTERFACE_FORMAT_HPP

#include "FormatTypes.hpp"

#include <string>
#include <vector>

namespace openswmm::gpkg::formats {

/**
 * @brief Parse a SWMM5 routing-interface text file into metadata + rows.
 *
 * @param path      Filesystem path to read.
 * @param meta      Output metadata (title, step, units, pollutants, objects).
 *                  Pollutant_values length on each row equals
 *                  `meta.pollutant_ids.size()`.
 * @param rows      Output data rows (appended; caller clears if needed).
 */
FormatResult parseRoutingInterfaceText(
    const std::string&                path,
    RoutingInterfaceMetadata&         meta,
    std::vector<RoutingInterfaceRow>& rows);

/**
 * @brief Write metadata + rows out to a SWMM5 routing-interface text file.
 *
 * @details `meta.object_ids` is written verbatim to the header. Rows whose
 *          `object_id` doesn't appear in `meta.object_ids` are still
 *          emitted — the legacy reader would silently skip them, so we
 *          do not enforce the registry.
 */
FormatResult writeRoutingInterfaceText(
    const std::string&                      path,
    const RoutingInterfaceMetadata&         meta,
    const std::vector<RoutingInterfaceRow>& rows);

} // namespace openswmm::gpkg::formats

#endif // OPENSWMM_GEOPACKAGE_ROUTING_INTERFACE_FORMAT_HPP
