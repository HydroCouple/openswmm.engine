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
 * @file Hdf5InputPlugin.hpp
 * @brief IInputPlugin implementation for the columnar HDF5 model format.
 *
 * @details Mirrors GeoPackageInputPlugin: a thin lifecycle wrapper that
 *          delegates to Hdf5ModelWriter / Hdf5ModelReader. Because the engine
 *          dispatches `swmm_model_write_with_plugin` on a plugin id rather
 *          than a file extension, registering this in
 *          PluginFactory::register_builtin_infos() is all that is needed to
 *          make
 *
 *          @code
 *          swmm_model_write_with_plugin(e, "model.h5",
 *              "org.hydrocouple.openswmm.plugins.hdf5");
 *          @endcode
 *
 *          work with no other engine change.
 *
 * @ingroup engine_hdf5
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_HDF5_INPUT_PLUGIN_HPP
#define OPENSWMM_HDF5_INPUT_PLUGIN_HPP

#include <openswmm/plugin_sdk/IInputPlugin.hpp>

#include <string>
#include <vector>

namespace openswmm::h5io {

class Hdf5InputPlugin : public IInputPlugin {
public:
    Hdf5InputPlugin() = default;

    PluginState state() const noexcept override { return state_; }

    int initialize(const std::vector<std::string>& init_args,
                   const IPluginComponentInfo* info) override;

    int validate(const SimulationContext& ctx) override;

    /**
     * @brief Read a model from an HDF5 file.
     *
     * @note Not implemented in schema 1.0 — the format ships write-first, as
     *       an export target. Returns a non-zero code and sets
     *       last_error_message() rather than half-populating a context, which
     *       would be far worse than refusing.
     */
    int read(const std::string& path, SimulationContext& ctx) override;

    /** @brief Write the model held by @p ctx to @p path. */
    int write(const std::string& path, const SimulationContext& ctx) override;

    /** @brief Sections the last write() could not carry. */
    std::vector<std::string> skipped_sections() const override { return skipped_; }

    int finalize(const SimulationContext& ctx) override;

    const char* last_error_message() const noexcept override { return error_.c_str(); }

private:
    PluginState              state_ = PluginState::UNLOADED;
    std::string              error_;
    std::vector<std::string> skipped_;
};

}  // namespace openswmm::h5io

#endif  // OPENSWMM_HDF5_INPUT_PLUGIN_HPP
