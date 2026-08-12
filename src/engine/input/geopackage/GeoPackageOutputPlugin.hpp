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
 * @file GeoPackageOutputPlugin.hpp
 * @brief IOutputPlugin that writes per-timestep results to a GeoPackage.
 * @ingroup engine_geopackage
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_GEOPACKAGE_OUTPUT_PLUGIN_HPP
#define OPENSWMM_GEOPACKAGE_OUTPUT_PLUGIN_HPP

#include "GpkgUtils.hpp"
#include <openswmm/plugin_sdk/IOutputPlugin.hpp>
#include <string>
#include <unordered_map>
#include <vector>

namespace openswmm::gpkg {

class GeoPackageOutputPlugin : public IOutputPlugin {
public:
    GeoPackageOutputPlugin() = default;

    PluginState state() const noexcept override { return state_; }

    int initialize(const std::vector<std::string>& init_args,
                   const IPluginComponentInfo* info) override;

    int validate(const SimulationContext& ctx) override;
    int prepare(const SimulationContext& ctx) override;
    int update(const SimulationSnapshot& snapshot) override;
    int finalize(const SimulationContext& ctx) override;

    const char* last_error_message() const noexcept override {
        return error_msg_.c_str();
    }

private:
    PluginState state_ = PluginState::UNLOADED;
    std::string error_msg_;
    std::string db_path_;
    std::string simulation_id_;
    DbPtr db_;
    StmtPtr insert_stmt_;

    // Cached variable IDs
    std::unordered_map<std::string, int> variable_ids_;

    // Batch buffer
    struct ResultRow {
        std::string object_type;
        std::string object_id;
        int variable_id;
        double elapsed_time;
        double value;
    };
    std::vector<ResultRow> buffer_;
    static constexpr size_t FLUSH_THRESHOLD = 5000;

    void flush_buffer();
    int lookup_variable(const std::string& name, const std::string& obj_type);

    // Per-timestep results arrive pre-converted to display units (engine
    // boundary), so no plugin-side unit-conversion factors are required.

    // Per-object report flags, captured during prepare() from SimulationContext
    std::vector<char> subcatch_rpt_flag_;
    std::vector<char> node_rpt_flag_;
    std::vector<char> link_rpt_flag_;
};

} // namespace openswmm::gpkg

#endif // OPENSWMM_GEOPACKAGE_OUTPUT_PLUGIN_HPP
