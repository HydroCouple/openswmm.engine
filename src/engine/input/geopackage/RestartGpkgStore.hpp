// SPDX-License-Identifier: Apache-2.0
/**
 * @file      RestartGpkgStore.hpp
 * @brief     Restart records in a GeoPackage: tables restart_records and restart_record_chunks.
 */
#pragma once

#include "core/restart/RestartStore.hpp"

#include <memory>
#include <string>

namespace openswmm::gpkg {

/**
 * @brief Open a GeoPackage's restart records.
 *
 * Read-only opening never creates or changes the file; a file without the record tables lists no
 * runs. Writing creates the GeoPackage schema and the record tables when missing. Records are
 * stored in 4 MiB chunks; each record is inserted in one transaction.
 *
 * @param[in] path UTF-8 path.
 * @param[in] write True to record a run into the file.
 * @param[out] error Reason when the result is null.
 */
std::unique_ptr<restart::Store> openRestartStore(const std::string& path, bool write,
                                                 std::string& error);

} // namespace openswmm::gpkg
