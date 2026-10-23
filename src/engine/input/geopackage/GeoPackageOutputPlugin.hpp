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
 * @file      GeoPackageOutputPlugin.hpp
 * @brief     IOutputPlugin that writes per-timestep results to a GeoPackage.
 * @ingroup engine_geopackage
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_GEOPACKAGE_OUTPUT_PLUGIN_HPP
#define OPENSWMM_GEOPACKAGE_OUTPUT_PLUGIN_HPP

#include "GpkgUtils.hpp"
#include <openswmm/plugin_sdk/IOutputPlugin.hpp>
#include <string>
#include <unordered_map>
#include <vector>

namespace openswmm::gpkg {

/**
 * @brief Own configuration and output IO state for a GeoPackage adapter.
 *
 * Caller follows initialize/validate/prepare/update/finalize ordering as applicable and serializes
 * all calls. State markers are descriptive; most methods do not enforce ordering. SQLite handles
 * are privately owned; borrowed contexts/snapshots must remain valid during synchronous calls.
 */
class GeoPackageOutputPlugin : public IOutputPlugin {
public:
    /**
     * @brief Create an unloaded output adapter with no open database.
     */
    GeoPackageOutputPlugin() = default;

    /**
     * @brief Read the stored adapter lifecycle marker.
     * @return Last assigned PluginState; individual operations do not independently enforce ordering.
     */
    PluginState state() const noexcept override { return state_; }

    /**
     * @brief Copy output adapter configuration without opening the database.
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
     * @brief Open/create the results database, write model metadata and prepare result insertion.
     *
     * Creates schema, writes the selected model/run, populates variables/species metadata, caches
     * variable IDs/report flags and prepares an insert statement. Sets PREPARED on success, ERROR on
     * handled failure. Does not roll back all earlier model/run setup after a later refusal. Connection
     * and statement remain adapter-owned until finalize/destruction.
     *
     * @param[in] ctx Borrowed aligned model context; retained scalar/vector metadata is copied where
     *     needed, not owned through this reference.
     * @return Zero for normal preparation, -1 for reported species-name collision or caught
     *     std::exception.
     */
    int prepare(const SimulationContext& ctx) override;
    /**
     * @brief Copy a snapshot's report-selected values into buffered database rows.
     *
     * Rows own copied object IDs/values; absent cached variables are skipped. Flushes at >=5000
     * buffered rows. No PREPARED guard, no rollback of rows buffered before a later exception, and no
     * state-marker transition on update failure. SQLite bind/step statuses in the flush loop are
     * discarded, so zero alone does not certify every row was inserted.
     *
     * @param[in] snapshot Borrowed snapshot with valid counts/parallel arrays and reported-species
     *     names. Values are already in display units, sim_time is elapsed seconds; no conversion here.
     * @return Zero after normal buffering/threshold flush; -1 after caught std::exception.
     */
    int update(const SimulationSnapshot& snapshot) override;
    /**
     * @brief Flush buffered rows, mark the run completed and release database resources.
     *
     * Requires a successfully prepared connection. Finalizes insert statement before closing DB and
     * sets FINALIZED; handled failure sets ERROR and may leave resources/buffer for destruction. SQL
     * step status for run completion is not checked.
     *
     * @param[in] ctx Unused borrowed context.
     * @return Zero after normal cleanup; -1 after caught std::exception.
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

    /**
     * @brief Insert buffered result rows inside an immediate transaction.
     *
     * Requires owned open connection and prepared insert statement. Resets/binds/steps each row,
     * commits, then clears buffer. Bind/step status is not checked; transaction setup/commit exceptions
     * propagate and retain buffer, while the guard attempts rollback. Caller serializes updates.
     */
    void flush_buffer();
    /**
     * @brief Resolve a cached exact-byte variable/object-type key.
     *
     * No database query; caller prepares the cache first.
     *
     * @param[in] name Borrowed case-sensitive variable name.
     * @param[in] obj_type Borrowed exact object-category token.
     * @return Cached variable ID or -1 for no name:object_type entry.
     */
    int lookup_variable(const std::string& name, const std::string& obj_type);

    /**
     * @brief Register model-dependent quality variable rows for node/link/subcatchment reports.
     *
     * Requires prepared DB/schema. Existing QUALITY rows are reused. Pollutants use declared
     * concentration units and reaction species their declared units; current reserved nonreaction
     * entries use hours metadata. Does not wrap all insertions in its own transaction or check every
     * SQLite insert-step status; SQL preparation exceptions propagate.
     *
     * @param[in] ctx Borrowed aligned model context; retained scalar/vector metadata is copied where
     *     needed, not owned through this reference.
     * @return Zero normally or when quality ignored/no species; -1 after a QUALITY-name collision with
     *     an existing nonquality variable.
     */
    int register_species_variables(const SimulationContext& ctx);

    // Per-timestep results arrive pre-converted to display units (engine
    // boundary), so no plugin-side unit-conversion factors are required.

    // Per-object report flags, captured during prepare() from SimulationContext
    std::vector<char> subcatch_rpt_flag_;
    std::vector<char> node_rpt_flag_;
    std::vector<char> link_rpt_flag_;
};

} // namespace openswmm::gpkg

#endif // OPENSWMM_GEOPACKAGE_OUTPUT_PLUGIN_HPP
