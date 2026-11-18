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
 * @file      GeoPackageReportPlugin.hpp
 * @brief     IReportPlugin that writes summary statistics to a GeoPackage.
 * @ingroup engine_geopackage
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_GEOPACKAGE_REPORT_PLUGIN_HPP
#define OPENSWMM_GEOPACKAGE_REPORT_PLUGIN_HPP

#include "GpkgUtils.hpp"
#include <openswmm/plugin_sdk/IReportPlugin.hpp>
#include <string>
#include <unordered_map>

namespace openswmm::gpkg {

/**
 * @brief Own configuration and report IO state for a GeoPackage adapter.
 *
 * Caller follows initialize/validate/prepare/update/finalize ordering as applicable and serializes
 * all calls. State markers are descriptive; most methods do not enforce ordering. SQLite handles
 * are privately owned; borrowed contexts/snapshots must remain valid during synchronous calls.
 */
class GeoPackageReportPlugin : public IReportPlugin {
public:
    /**
     * @brief Create an unloaded report adapter with no open database.
     */
    GeoPackageReportPlugin() = default;

    /**
     * @brief Read the stored adapter lifecycle marker.
     * @return Last assigned PluginState; individual operations do not independently enforce ordering.
     */
    PluginState state() const noexcept override { return state_; }

    /**
     * @brief Copy report adapter configuration without opening the database.
     *
     * Sets INITIALIZED on success. Does not clear prior diagnostic/cache/buffer state; use a fresh
     * adapter per independently configured run. Allocation exceptions can propagate during argument
     * copying.
     *
     * @param[in] init_args Borrowed arguments: [0] required database path, [1] optional simulation ID
     *     (default run_1). Extra arguments are ignored.
     * @param[in] info Unused borrowed metadata pointer; NULL is accepted.
     * @return Zero after configuration; -1 with diagnostic when no database-path argument exists.
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
     * @brief Open/create the database and cache existing variable identifiers.
     *
     * Requires variables schema already present; this adapter does not call create_schema(). Sets
     * PREPARED only on success; failure stores diagnostic without forcing ERROR. Exact variable cache
     * entries from earlier preparation are not cleared.
     *
     * @param[in] ctx Unused borrowed context; model values are read later in write_summary().
     * @return Zero after normal cache construction; -1 after caught std::exception.
     */
    int prepare(const SimulationContext& ctx) override;
    /**
     * @brief Accept a snapshot without per-step work.
     *
     * @param[in] snapshot Unused borrowed snapshot; final summary reads context statistics instead.
     * @return Always zero; no state changes or IO.
     */
    int update(const SimulationSnapshot& snapshot) override;
    /**
     * @brief Write selected object statistics and run continuity summaries.
     *
     * Requires prepared DB/schema/cache and aligned report-flag arrays. Stores raw context statistics
     * without display conversion: internal ft/cfs/ft3 and seconds where applicable, dimensionless
     * filling. Missing variable IDs skip individual rows. Sets run quality-continuity field to the
     * current placeholder zero. SQLite bind/step statuses are discarded; exceptions trigger best-effort
     * transaction rollback.
     *
     * @param[in] ctx Borrowed aligned model context; retained scalar/vector metadata is copied where
     *     needed, not owned through this reference.
     * @return Zero after normal transaction commit; -1 after caught std::exception.
     */
    int write_summary(const SimulationContext& ctx) override;
    /**
     * @brief Close the owned report database and mark finalized.
     *
     * @param[in] ctx Unused borrowed context.
     * @return Always zero; sets FINALIZED without generating a summary.
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
    std::string db_path_;
    std::string simulation_id_;
    DbPtr db_;

    std::unordered_map<std::string, int> variable_ids_;
    /**
     * @brief Read a cached exact-byte summary variable key.
     *
     * @param[in] name Borrowed case-sensitive variable token.
     * @param[in] obj_type Borrowed exact object category.
     * @return Cached ID for name:object_type, or -1 when not present.
     */
    int lookup_variable(const std::string& name, const std::string& obj_type);
};

} // namespace openswmm::gpkg

#endif // OPENSWMM_GEOPACKAGE_REPORT_PLUGIN_HPP
