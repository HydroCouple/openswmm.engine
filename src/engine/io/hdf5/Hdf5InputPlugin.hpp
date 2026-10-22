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
 * @file      Hdf5InputPlugin.hpp
 * @brief     IInputPlugin implementation for the columnar HDF5 model format.
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
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_HDF5_INPUT_PLUGIN_HPP
#define OPENSWMM_HDF5_INPUT_PLUGIN_HPP

#include <openswmm/plugin_sdk/IInputPlugin.hpp>

#include <string>
#include <vector>

namespace openswmm::h5io {

class Hdf5InputPlugin : public IInputPlugin {
public:
    /**
     * @brief Construct an unloaded plugin with empty diagnostics.
     */
    Hdf5InputPlugin() = default;

    /**
     * @brief Inspect the last explicitly assigned plugin lifecycle state.
     * @return Current state; a successful write() does not itself advance it.
     */
    PluginState state() const noexcept override { return state_; }

    /**
     * @brief Reset diagnostics and mark the plugin initialized.
     * @param[in] init_args Currently ignored initialization arguments; not retained.
     * @param[in] info Currently ignored metadata pointer; may be null and is not retained.
     * @return Zero. Clears the error message and skipped-section list and sets
     * state() to INITIALIZED without opening a file.
     */
    int initialize(const std::vector<std::string>& init_args,
                   const IPluginComponentInfo* info) override;

    /**
     * @brief Mark the plugin validated without inspecting the model.
     * @param[in] ctx Model context, currently ignored and not modified.
     * @return Zero; state() becomes VALIDATED. No schema or model checks occur.
     */
    int validate(const SimulationContext& ctx) override;

    /**
     * @brief Refuse unsupported HDF5 model import in schema 1.0.
     * @param[in] path Source path, currently ignored; no file is opened.
     * @param[in,out] ctx Model context left completely unchanged.
     * @return One; state() becomes ERROR and last_error_message() explains
     * that the current schema supports export only.
     */
    int read(const std::string& path, SimulationContext& ctx) override;

    /**
     * @brief Export a model to a columnar HDF5 file.
     * @param[in] path Destination filename; existing files are truncated.
     * @param[in] ctx Model in internal units; conversion uses a copy when needed.
     * @return write_model()'s status. A failure sets state() to ERROR and stores
     * a diagnostic; success preserves the current state and exposes omissions
     * through skipped_sections(). Allocation/conversion exceptions propagate.
     * @note Clears previous errors and omissions before writing. A zero result
     * has write_model()'s limited I/O-status guarantee; inspect skipped_sections().
     */
    int write(const std::string& path, const SimulationContext& ctx) override;

    /**
     * @brief Copy the most recent successful export's omission diagnostics.
     * @return Owned vector of warning strings; empty after initialize() or a
     * failed write(). One string may describe multiple unsupported sections.
     */
    std::vector<std::string> skipped_sections() const override { return skipped_; }

    /**
     * @brief Mark this export plugin finalized.
     * @param[in] ctx Model context, currently ignored and not modified.
     * @return Zero; state() becomes FINALIZED. Diagnostics are retained and
     * there are no persistent file handles to release.
     */
    int finalize(const SimulationContext& ctx) override;

    /**
     * @brief Access the plugin's most recent error diagnostic.
     * @return Borrowed null-terminated string, possibly empty; valid until the
     * next initialize(), read(), write(), or destruction. Do not free or retain
     * across a call that can modify the plugin's error string.
     */
    const char* last_error_message() const noexcept override { return error_.c_str(); }

private:
    PluginState state_ = PluginState::UNLOADED;
    std::string error_;
    std::vector<std::string> skipped_;
};

} // namespace openswmm::h5io

#endif // OPENSWMM_HDF5_INPUT_PLUGIN_HPP
