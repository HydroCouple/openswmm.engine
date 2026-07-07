/**
 * @file TimeseriesFormat.hpp
 * @brief SWMM-native timeseries text-file parser / materialiser.
 *
 * @details Slice IO-6a.  Wire format (one row per line, whitespace or tab
 *          delimited):
 *
 *  @code
 *  ;; optional comment lines (start with `;`)
 *  MM/DD/YYYY  H:MM  value
 *  MM/DD/YYYY  H:MM[:SS]  value
 *  @endcode
 *
 *          Mirrors the loader already present in
 *          `input/PostParseResolver.cpp::load_external_timeseries_files`
 *          (Slice IO-3); the parser here is the extraction so other
 *          consumers (the GeoPackage import path in IO-7) can read the
 *          same files into `formats::TimeseriesRow` rows directly.
 *
 * @ingroup engine_geopackage
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_GEOPACKAGE_TIMESERIES_FORMAT_HPP
#define OPENSWMM_GEOPACKAGE_TIMESERIES_FORMAT_HPP

#include "FormatTypes.hpp"

#include <string>
#include <vector>

namespace openswmm::gpkg::formats {

/**
 * @brief Parse a SWMM-native timeseries text file at `path` into rows.
 *
 * @param path     Filesystem path to read.
 * @param rows     Output rows (appended; caller must clear if needed).
 * @returns FormatResult — `ok` on success; `error` carries open / parse
 *          diagnostics on failure.
 */
FormatResult parseTimeseriesText(const std::string&          path,
                                  std::vector<TimeseriesRow>& rows);

/**
 * @brief Write rows back to a SWMM-native timeseries text file at `path`.
 *
 * @details Output uses `MM/DD/YYYY HH:MM:SS` followed by a fixed-precision
 *          value. Idempotent round-trip with `parseTimeseriesText`.
 */
FormatResult writeTimeseriesText(const std::string&                  path,
                                  const std::vector<TimeseriesRow>&   rows);

} // namespace openswmm::gpkg::formats

#endif // OPENSWMM_GEOPACKAGE_TIMESERIES_FORMAT_HPP
