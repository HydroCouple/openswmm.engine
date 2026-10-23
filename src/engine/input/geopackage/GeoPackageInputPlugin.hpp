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
 * @file      GeoPackageInputPlugin.hpp
 * @brief     IInputPlugin implementation for reading/writing SWMM models from/to GeoPackage.
 *
 * @details This plugin detects `.gpkg` file extensions and delegates to
 *          GeoPackageReader (for read) and GeoPackageWriter (for write).
 *          It replaces the text .inp format when a GeoPackage file is provided.
 *
 * @ingroup engine_geopackage
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_GEOPACKAGE_INPUT_PLUGIN_HPP
#define OPENSWMM_GEOPACKAGE_INPUT_PLUGIN_HPP

#include <openswmm/plugin_sdk/IInputPlugin.hpp>
#include <string>
#include <vector>

namespace openswmm::gpkg {

/**
 * @brief Own configuration and input IO state for a GeoPackage adapter.
 *
 * Caller follows initialize/validate/prepare/update/finalize ordering as applicable and serializes
 * all calls. State markers are descriptive; most methods do not enforce ordering. SQLite handles
 * are privately owned; borrowed contexts/snapshots must remain valid during synchronous calls.
 */
class GeoPackageInputPlugin : public IInputPlugin {
public:
    /**
     * @brief Create an unloaded input adapter with no open database.
     */
    GeoPackageInputPlugin() = default;

    /**
     * @brief Read the stored adapter lifecycle marker.
     * @return Last assigned PluginState; individual operations do not independently enforce ordering.
     */
    PluginState state() const noexcept override { return state_; }

    /**
     * @brief Copy input adapter configuration without opening the database.
     *
     * Sets INITIALIZED on success. Does not clear prior diagnostic/cache/buffer state; use a fresh
     * adapter per independently configured run. SRS uses stoi prefix parsing; its exceptions are caught
     * and set SRS zero.
     *
     * @param[in] init_args Borrowed arguments: [0] simulation ID (default retained/default), [1]
     *     optional SRS integer; missing entries preserve prior configuration.
     * @param[in] info Unused borrowed metadata pointer; NULL is accepted.
     * @return Zero after configuration; -1 is not returned for SRS parse errors: malformed SRS becomes
     *     zero.
     */
    int initialize(const std::vector<std::string>& init_args,
                   const IPluginComponentInfo* info) override;

    /**
     * @brief Mark the adapter validated without inspecting the model.
     *
     * @param[in] ctx Unused borrowed context; no model/schema/file validation is performed.
     * @return Always zero; sets VALIDATED.
     */
    int validate(const SimulationContext& ctx) override;

    /**
     * @brief Read one simulation from a read-only GeoPackage connection.
     *
     * When configured ID equals default, selects the first returned simulations row (no ordering
     * guarantee), then nodes fallback if none. Stores that selected ID for later calls. Does not change
     * PluginState on read result and does not enforce initialized/validated state.
     *
     * @param[in] path Borrowed existing .gpkg filename, opened read-only.
     * @param[in,out] ctx Caller-owned context populated directly by read_model(); no rollback of
     *     partial context adoption is supplied by this adapter.
     * @return Zero for normal read; read_model refusal code, or -1 after a caught std::exception.
     */
    int read(const std::string& path, SimulationContext& ctx) override;

    /**
     * @brief Write the model into the configured simulation of a GeoPackage.
     *
     * Creates/updates schema, infers SRS from text after a CRS colon only when configured SRS is zero,
     * and registers a positive inferred EPSG row. No model CRS transform. Does not change adapter
     * state; writer controls transaction semantics.
     *
     * @param[in] path Borrowed destination opened READWRITE|CREATE; existing model rows may be replaced
     *     by write_model().
     * @param[in] ctx Borrowed aligned model context; retained scalar/vector metadata is copied where
     *     needed, not owned through this reference.
     * @return Zero after writer returns; -1 after a caught std::exception.
     */
    int write(const std::string& path, const SimulationContext& ctx) override;

    /**
     * @brief Return the adapter's unsupported-section listing.
     * @return Always an empty owned vector; no read diagnostics are inferred from this listing.
     */
    std::vector<std::string> skipped_sections() const override { return {}; }

    /**
     * @brief Mark the input adapter finalized.
     *
     * @param[in] ctx Unused borrowed context.
     * @return Always zero; read/write use local connections so no persistent database is closed here.
     */
    int finalize(const SimulationContext& ctx) override;

    /**
     * @brief Borrow the most recently stored failure diagnostic.
     *
     * Success does not consistently clear an earlier diagnostic.
     * @return Adapter-owned NUL-terminated string; empty initially. Copy before a later diagnostic
     *     mutation or destruction.
     */
    const char* last_error_message() const noexcept override { return error_msg_.c_str(); }

private:
    PluginState state_ = PluginState::UNLOADED;
    std::string error_msg_;
    std::string simulation_id_ = "default";
    int srs_id_ = 0;
};

} // namespace openswmm::gpkg

#endif // OPENSWMM_GEOPACKAGE_INPUT_PLUGIN_HPP
