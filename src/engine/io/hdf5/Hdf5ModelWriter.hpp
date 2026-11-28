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
 * @brief Write a complete native model and analysis columns to HDF5.
 * @param path Destination filename, created or truncated.
 * @param ctx Model in engine internal units; not modified.
 * @param warnings Optional diagnostics sink, preserved and appended to.
 * @return Zero after writing and flushing; nonzero on I/O failure.
 * The native_input dataset is the authoritative editable configuration. Existing
 * column datasets retain their authored-unit analysis layout. Component configuration
 * contents are embedded separately. Older column-only files lack enough data to reopen.
 * Conversion/allocation exceptions propagate. Failure may leave a partial destination.
 */
int write_model(const std::string& path, const SimulationContext& ctx,
                std::vector<std::string>* warnings = nullptr);

} // namespace h5io
} // namespace openswmm

#endif // OPENSWMM_HDF5_MODEL_WRITER_HPP
