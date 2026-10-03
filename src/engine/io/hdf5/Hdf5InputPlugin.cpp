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
 * @file Hdf5InputPlugin.cpp
 * @brief Lifecycle wrapper over Hdf5ModelWriter / Hdf5ModelReader.
 * @ingroup engine_hdf5
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "Hdf5InputPlugin.hpp"
#include "Hdf5ModelWriter.hpp"

namespace openswmm::h5io {

int Hdf5InputPlugin::initialize(const std::vector<std::string>& /*init_args*/,
                                const IPluginComponentInfo* /*info*/) {
    error_.clear();
    skipped_.clear();
    state_ = PluginState::INITIALIZED;
    return 0;
}

int Hdf5InputPlugin::validate(const SimulationContext& /*ctx*/) {
    state_ = PluginState::VALIDATED;
    return 0;
}

int Hdf5InputPlugin::read(const std::string& /*path*/, SimulationContext& /*ctx*/) {
    // Schema 1.0 ships write-first. Refusing outright is the honest answer: a
    // partially populated SimulationContext would run and produce numbers,
    // which is a far worse failure than not opening at all.
    error_ = "HDF5 model read is not implemented in schema " +
             std::string(kSchemaVersion) +
             "; the format is currently an export target. Use the .inp or "
             ".gpkg reader to load a model.";
    state_ = PluginState::ERROR;
    return 1;
}

int Hdf5InputPlugin::write(const std::string& path, const SimulationContext& ctx) {
    error_.clear();
    skipped_.clear();
    std::vector<std::string> warnings;
    const int rc = write_model(path, ctx, &warnings);
    if (rc != 0) {
        error_ = warnings.empty() ? "HDF5 model write failed" : warnings.front();
        state_ = PluginState::ERROR;
        return rc;
    }
    // The writer reports every section it could not carry; surface them
    // through the interface's own channel rather than dropping them.
    skipped_ = std::move(warnings);
    return 0;
}

int Hdf5InputPlugin::finalize(const SimulationContext& /*ctx*/) {
    state_ = PluginState::FINALIZED;
    return 0;
}

}  // namespace openswmm::h5io
