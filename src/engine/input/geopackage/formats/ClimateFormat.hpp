/**
 * @file ClimateFormat.hpp
 * @brief User-CSV climate-file parser / materialiser.
 *
 * @details Slice IO-6c. Canonical row form:
 *
 *  @code
 *  date,tmin,tmax,evap,wind,sky,humidity
 *  2026-01-01,32.0,52.0,0.10,5.0,0.5,55.0
 *  @endcode
 *
 *          One header line introduces the column order; the parser reads
 *          the header to map column position so future variants (column
 *          re-orderings) keep working. NCDC TD3200 and GHCND remain
 *          deferred behind the plugin SDK per the plan §3.6.
 *
 * @ingroup engine_geopackage
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_GEOPACKAGE_CLIMATE_FORMAT_HPP
#define OPENSWMM_GEOPACKAGE_CLIMATE_FORMAT_HPP

#include "FormatTypes.hpp"

#include <string>
#include <vector>

namespace openswmm::gpkg::formats {

FormatResult parseClimateCsv(const std::string&        path,
                              std::vector<ClimateRow>&  rows);

FormatResult writeClimateCsv(const std::string&             path,
                              const std::vector<ClimateRow>& rows);

} // namespace openswmm::gpkg::formats

#endif // OPENSWMM_GEOPACKAGE_CLIMATE_FORMAT_HPP
