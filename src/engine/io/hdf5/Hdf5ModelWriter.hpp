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
 * @file      Hdf5ModelWriter.hpp
 * @brief     Serialise a SimulationContext to the columnar HDF5 model format.
 *
 * @details Design and rationale: `src/engine/io/hdf5/STRATEGY.md`.
 *
 *          Two decisions carry most of the weight:
 *
 *          1. **Authored units, exactly like the `.inp`** (STRATEGY §4). The
 *             writer runs the same `convert_internal_to_display` +
 *             `convert_internal_to_authored` pass on a copy that InpWriter
 *             does, so the numbers in the file are the numbers the modeller
 *             typed. GeoPackage made the opposite choice — internal units with
 *             a single display-unit island for the cross-section geoms — and
 *             the file-IO audit found that island is a standing trap.
 *          2. **References are written as NAMES, not indices.** A file that
 *             says `node1 = "J1"` survives renumbering and is legible in
 *             h5py; resolution happens once on read.
 *
 * @ingroup engine_hdf5
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_HDF5_MODEL_WRITER_HPP
#define OPENSWMM_HDF5_MODEL_WRITER_HPP

#include <string>
#include <vector>

namespace openswmm {

struct SimulationContext;

namespace h5io {

/// Bumped on any layout change. The reader refuses a major it does not know
/// rather than silently misreading a file written by a newer engine.
inline constexpr int kSchemaVersionMajor = 1;
inline constexpr int kSchemaVersionMinor = 0;
inline constexpr const char* kSchemaVersion = "1.0";

/**
 * @brief Export supported model sections to a columnar HDF5 file.
 * @param[in] path Destination filename, created or truncated using H5F_ACC_TRUNC.
 * @param[in] ctx Consistent model in engine internal units; not modified.
 * Authored/display conversion is performed on a local copy when necessary.
 * @param[in,out] warnings Optional caller-owned vector. Appends file-creation
 * errors or an unsupported-section summary; existing entries are preserved.
 * Null discards these diagnostics. The pointer is not retained.
 * @return One if the destination cannot be created; zero after invoking
 * all section writers. Individual dataset/attribute writes and close errors
 * are not aggregated, so zero does not certify complete I/O success.
 * @details References are exported by name and numeric data in authored units.
 * Schema 1.0 intentionally omits controls, LID, groundwater, snow, quality,
 * inflows, options/report settings, 2D sections and other sections listed
 * in the warning. Use the .inp/.gpkg writer for a complete model archive.
 * @note Failure may leave a truncated or partial file. Conversion/allocation
 * exceptions propagate; this function does not publish through a staging file.
 */
int write_model(const std::string& path, const SimulationContext& ctx,
                std::vector<std::string>* warnings = nullptr);

} // namespace h5io
} // namespace openswmm

#endif // OPENSWMM_HDF5_MODEL_WRITER_HPP
