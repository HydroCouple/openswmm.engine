// SPDX-License-Identifier: Apache-2.0
/**
 * @file      Hdf5RestartStore.hpp
 * @brief     Restart records in an HDF5 file, under the /restart group.
 *
 * Layout, schema 1.0:
 * - `/restart` with attribute `openswmm_restart_schema` = "1.0".
 * - `/restart/runs/<run_id>` with run attributes and a `model_input` dataset holding the model's
 *   native input text, for provenance.
 * - `/restart/runs/<run_id>/records/<record_id>`: a chunked `uint8` dataset of the record's native
 *   hot start bytes, without a compression filter, with record attributes. Its `status` attribute
 *   is written last, and is "complete" once the record can be restored.
 */
#pragma once

#include "core/restart/RestartStore.hpp"

#include <memory>
#include <string>

namespace openswmm::h5io {

/**
 * @brief Open an HDF5 file's restart records.
 *
 * Read-only opening never changes the file; a file without `/restart` lists no runs. Writing
 * creates a missing or empty file, and refuses an existing file that holds other content but no
 * `/restart` group, such as a model or 2D results file.
 *
 * @param[in] path UTF-8 path.
 * @param[in] write True to record a run into the file.
 * @param[out] error Reason when the result is null.
 */
std::unique_ptr<restart::Store> openRestartStore(const std::string& path, bool write,
                                                 std::string& error);

} // namespace openswmm::h5io
