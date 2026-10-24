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
 * @file      RaingageFormat.hpp
 * @brief     SWMM "Standard" raingage text-file parser / materialiser.
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
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_GEOPACKAGE_RAINGAGE_FORMAT_HPP
#define OPENSWMM_GEOPACKAGE_RAINGAGE_FORMAT_HPP

#include "FormatTypes.hpp"

#include <string>
#include <vector>

namespace openswmm::gpkg::formats {

/**
 * @brief Append standard SWMM station/date/time/rain records.
 *
 * Uses 512-byte line reads and sscanf; comments/short malformed rows skip. No station filter,
 * start-date clipping, unit/rain-type conversion, monotonic/finite validation or late IO checking.
 * Timestamp validity follows permissive datetime encoders.
 *
 * @param[in] path UTF-8 filesystem path; caller supplies parent directories and owns
 *     destination/source lifetime.
 * @param[in,out] rows Caller-owned destination appended with station ID, OADate and raw rainfall
 *     values.
 * @return Failure on open refusal, otherwise success even if no row parses.
 */
FormatResult parseRaingageStd(const std::string& path, std::vector<RaingageRow>& rows);

/**
 * @brief Overwrite a standard SWMM station precipitation text file.
 *
 * Emits station, calendar date and hour/minute with rainfall four decimals; seconds are dropped. No
 * escaping, sorting, format/range validation, staging or fprintf/fclose status propagation.
 *
 * @param[in] path UTF-8 filesystem path; caller supplies parent directories and owns
 *     destination/source lifetime.
 * @param[in] rows Borrowed station/OADate/raw-rain rows; rainfall remains its source unit
 *     convention.
 * @return Failure only on open refusal, otherwise success.
 */
FormatResult writeRaingageStd(const std::string& path, const std::vector<RaingageRow>& rows);

} // namespace openswmm::gpkg::formats

#endif // OPENSWMM_GEOPACKAGE_RAINGAGE_FORMAT_HPP
