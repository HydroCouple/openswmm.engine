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
 * @license  MIT License
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
