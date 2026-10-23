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
 * @file      Hdf5InputPlugin.cpp
 * @brief     Lifecycle wrapper over Hdf5ModelWriter / Hdf5ModelReader.
 * @ingroup engine_hdf5
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include "Hdf5InputPlugin.hpp"
#include "Hdf5ModelWriter.hpp"
#include "Hdf5Util.hpp"
#include "plugins/DefaultInputPlugin.hpp"
#include "core/SimulationContext.hpp"
#include "core/NativeModelBundle.hpp"

namespace openswmm::h5io {

// Contract: openswmm::h5io::Hdf5InputPlugin::initialize.
int Hdf5InputPlugin::initialize(const std::vector<std::string>& /*init_args*/,
                                const IPluginComponentInfo* /*info*/) {
    error_.clear();
    skipped_.clear();
    state_ = PluginState::INITIALIZED;
    return 0;
}

// Contract: openswmm::h5io::Hdf5InputPlugin::validate.
int Hdf5InputPlugin::validate(const SimulationContext& /*ctx*/) {
    state_ = PluginState::VALIDATED;
    return 0;
}

// Contract: openswmm::h5io::Hdf5InputPlugin::read.
int Hdf5InputPlugin::read(const std::string& path, SimulationContext& ctx) {
    error_.clear();
    try {
        Handle file(H5Fopen(path.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT), Handle::Kind::File);
        std::vector<std::string> input;
        if (!file || H5Lexists(file.get(), "native_input", H5P_DEFAULT) <= 0 ||
            !read_column(file.get(), "native_input", input) || input.size() != 1) {
            error_ = "This HDF5 file has no complete native model configuration. "
                     "Older column-only exports cannot be reopened as models.";
            state_ = PluginState::ERROR;
            return 1;
        }
        ctx.inp_file_path = path;
        ctx.gpkg_units_internal = false;
        DefaultInputPlugin parser;
        const int rc = parser.readText(input.front(), ctx);
        if (rc != 0) { error_ = ctx.error_message; state_ = PluginState::ERROR; }
        else if (H5Lexists(file.get(), "component_ids", H5P_DEFAULT) > 0) {
            std::vector<std::string> ids, configs;
            if (!read_column(file.get(), "component_ids", ids) ||
                !read_column(file.get(), "component_configurations", configs) || ids.size() != configs.size())
                throw std::runtime_error("Invalid HDF5 component configurations");
            std::map<std::string, std::string> content;
            for (std::size_t i = 0; i < ids.size(); ++i) content[ids[i]] = configs[i];
            native_model::restoreConfigurations(ctx, content);
        }
        return rc;
    } catch (const std::exception& e) {
        error_ = e.what(); state_ = PluginState::ERROR; return 1;
    }
}

// Contract: openswmm::h5io::Hdf5InputPlugin::write.
int Hdf5InputPlugin::write(const std::string& path, const SimulationContext& ctx) {
    error_.clear();
    skipped_.clear();
    std::vector<std::string> warnings;
    int rc;
    try { rc = write_model(path, ctx, &warnings); }
    catch (const std::exception& e) { error_ = e.what(); state_ = PluginState::ERROR; return 1; }
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

// Contract: openswmm::h5io::Hdf5InputPlugin::finalize.
int Hdf5InputPlugin::finalize(const SimulationContext& /*ctx*/) {
    state_ = PluginState::FINALIZED;
    return 0;
}

} // namespace openswmm::h5io
