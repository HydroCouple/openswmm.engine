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
 * @file RaingageFormat.hpp
 * @brief SWMM "Standard" raingage text-file parser / materialiser.
 *
 * @details Slice IO-6d. Canonical "STD" form (one record per line,
 *          whitespace-delimited):
 *
 *  @code
 *  STA01  2026  01  01  06  00   0.10
 *  STA01  2026  01  01  07  00   0.05
 *  @endcode
 *
 *          Columns: station_id, year, month, day, hour, minute, value.
 *          Comment lines beginning with `;` are skipped. NWS and NCDC
 *          DSI-3240 variants are deferred behind the plugin SDK per the
 *          plan §3.6.
 *
 * @ingroup engine_geopackage
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_GEOPACKAGE_RAINGAGE_FORMAT_HPP
#define OPENSWMM_GEOPACKAGE_RAINGAGE_FORMAT_HPP

#include "FormatTypes.hpp"

#include <string>
#include <vector>

namespace openswmm::gpkg::formats {

FormatResult parseRaingageStd(const std::string&         path,
                               std::vector<RaingageRow>&  rows);

FormatResult writeRaingageStd(const std::string&              path,
                               const std::vector<RaingageRow>& rows);

} // namespace openswmm::gpkg::formats

#endif // OPENSWMM_GEOPACKAGE_RAINGAGE_FORMAT_HPP
