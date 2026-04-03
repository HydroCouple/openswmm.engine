/**
 * @file GeoPackageOutputPlugin.cpp
 * @brief IOutputPlugin that writes per-timestep results to a GeoPackage.
 * @ingroup engine_geopackage
 */

#include "GeoPackageOutputPlugin.hpp"
#include "GeoPackageSchema.hpp"
#include "GeoPackageWriter.hpp"

#include "core/SimulationContext.hpp"
#include <openswmm/plugin_sdk/SimulationSnapshot.hpp>

#include <version.h>
#include <chrono>
#include <sstream>
#include <iomanip>

namespace openswmm::gpkg {

int GeoPackageOutputPlugin::initialize(const std::vector<std::string>& init_args,
                                        const IPluginComponentInfo* /*info*/) {
    if (init_args.size() < 1) {
        error_msg_ = "GeoPackageOutputPlugin requires at least 1 argument: output path";
        return -1;
    }
    db_path_ = init_args[0];
    simulation_id_ = init_args.size() > 1 ? init_args[1] : "run_1";
    state_ = PluginState::INITIALIZED;
    return 0;
}

int GeoPackageOutputPlugin::validate(const SimulationContext& /*ctx*/) {
    state_ = PluginState::VALIDATED;
    return 0;
}

int GeoPackageOutputPlugin::prepare(const SimulationContext& ctx) {
    try {
        db_ = open_database(db_path_);
        create_schema(db_.get());

        // Write model input
        write_model(db_.get(), ctx, simulation_id_);

        // Register simulation run
        auto now = std::chrono::system_clock::now();
        auto now_t = std::chrono::system_clock::to_time_t(now);
        std::ostringstream ts;
        ts << std::put_time(std::gmtime(&now_t), "%Y-%m-%dT%H:%M:%SZ");

        auto stmt = gpkg::prepare(db_.get(),
            "INSERT OR REPLACE INTO simulations "
            "(simulation_id, name, created_at, engine_version, status) "
            "VALUES (?, ?, ?, ?, 'running')");
        bind_text(stmt.get(), 1, simulation_id_);
        bind_text(stmt.get(), 2, simulation_id_);
        bind_text(stmt.get(), 3, ts.str());
        bind_text(stmt.get(), 4, OPENSWMM_VERSION_FULL);
        sqlite3_step(stmt.get());

        populate_default_variables(db_.get());

        // Cache variable IDs
        auto var_stmt = gpkg::prepare(db_.get(), "SELECT variable_id, name, object_type FROM variables");
        while (sqlite3_step(var_stmt.get()) == SQLITE_ROW) {
            int vid = column_int(var_stmt.get(), 0);
            std::string key = column_text(var_stmt.get(), 1) + ":" + column_text(var_stmt.get(), 2);
            variable_ids_[key] = vid;
        }

        // Prepare insert statement
        insert_stmt_ = gpkg::prepare(db_.get(),
            "INSERT INTO result_timeseries (simulation_id, object_type, object_id, variable_id, elapsed_time, value) "
            "VALUES (?, ?, ?, ?, ?, ?)");

        exec(db_.get(), "PRAGMA synchronous=NORMAL");

        state_ = PluginState::PREPARED;
        return 0;
    } catch (const std::exception& e) {
        error_msg_ = e.what();
        state_ = PluginState::ERROR;
        return -1;
    }
}

int GeoPackageOutputPlugin::update(const SimulationSnapshot& snapshot) {
    try {
        double sim_time = snapshot.sim_time;

        // Buffer node results
        for (int i = 0; i < snapshot.node_count; ++i) {
            std::string name = (snapshot.node_ids && i < (int)snapshot.node_ids->size())
                ? (*snapshot.node_ids)[i] : std::to_string(i);
            int vid = lookup_variable("depth", "NODE");
            if (vid >= 0 && i < (int)snapshot.nodes.depth.size())
                buffer_.push_back({"NODE", name, vid, sim_time, snapshot.nodes.depth[i]});
            vid = lookup_variable("head", "NODE");
            if (vid >= 0 && i < (int)snapshot.nodes.head.size())
                buffer_.push_back({"NODE", name, vid, sim_time, snapshot.nodes.head[i]});
            vid = lookup_variable("volume", "NODE");
            if (vid >= 0 && i < (int)snapshot.nodes.volume.size())
                buffer_.push_back({"NODE", name, vid, sim_time, snapshot.nodes.volume[i]});
            vid = lookup_variable("lateral_inflow", "NODE");
            if (vid >= 0 && i < (int)snapshot.nodes.lateral_inflow.size())
                buffer_.push_back({"NODE", name, vid, sim_time, snapshot.nodes.lateral_inflow[i]});
            vid = lookup_variable("overflow", "NODE");
            if (vid >= 0 && i < (int)snapshot.nodes.overflow.size())
                buffer_.push_back({"NODE", name, vid, sim_time, snapshot.nodes.overflow[i]});
        }

        // Buffer link results
        for (int i = 0; i < snapshot.link_count; ++i) {
            std::string name = (snapshot.link_ids && i < (int)snapshot.link_ids->size())
                ? (*snapshot.link_ids)[i] : std::to_string(i);
            int vid = lookup_variable("flow", "LINK");
            if (vid >= 0 && i < (int)snapshot.links.flow.size())
                buffer_.push_back({"LINK", name, vid, sim_time, snapshot.links.flow[i]});
            vid = lookup_variable("depth", "LINK");
            if (vid >= 0 && i < (int)snapshot.links.depth.size())
                buffer_.push_back({"LINK", name, vid, sim_time, snapshot.links.depth[i]});
            vid = lookup_variable("velocity", "LINK");
            if (vid >= 0 && i < (int)snapshot.links.velocity.size())
                buffer_.push_back({"LINK", name, vid, sim_time, snapshot.links.velocity[i]});
        }

        // Buffer subcatchment results
        for (int i = 0; i < snapshot.subcatch_count; ++i) {
            std::string name = (snapshot.subcatch_ids && i < (int)snapshot.subcatch_ids->size())
                ? (*snapshot.subcatch_ids)[i] : std::to_string(i);
            int vid = lookup_variable("rainfall", "SUBCATCH");
            if (vid >= 0 && i < (int)snapshot.subcatch.rainfall.size())
                buffer_.push_back({"SUBCATCH", name, vid, sim_time, snapshot.subcatch.rainfall[i]});
            vid = lookup_variable("runoff", "SUBCATCH");
            if (vid >= 0 && i < (int)snapshot.subcatch.runoff.size())
                buffer_.push_back({"SUBCATCH", name, vid, sim_time, snapshot.subcatch.runoff[i]});
        }

        if (buffer_.size() >= FLUSH_THRESHOLD)
            flush_buffer();

        return 0;
    } catch (const std::exception& e) {
        error_msg_ = e.what();
        return -1;
    }
}

int GeoPackageOutputPlugin::finalize(const SimulationContext& /*ctx*/) {
    try {
        if (!buffer_.empty())
            flush_buffer();

        // Update simulation status
        auto stmt = gpkg::prepare(db_.get(),
            "UPDATE simulations SET status = 'completed' WHERE simulation_id = ?");
        bind_text(stmt.get(), 1, simulation_id_);
        sqlite3_step(stmt.get());

        insert_stmt_.reset();
        db_.reset();
        state_ = PluginState::FINALIZED;
        return 0;
    } catch (const std::exception& e) {
        error_msg_ = e.what();
        state_ = PluginState::ERROR;
        return -1;
    }
}

void GeoPackageOutputPlugin::flush_buffer() {
    Transaction txn(db_.get());
    for (const auto& row : buffer_) {
        sqlite3_reset(insert_stmt_.get());
        sqlite3_clear_bindings(insert_stmt_.get());
        bind_text(insert_stmt_.get(), 1, simulation_id_);
        bind_text(insert_stmt_.get(), 2, row.object_type);
        bind_text(insert_stmt_.get(), 3, row.object_id);
        bind_int(insert_stmt_.get(), 4, row.variable_id);
        bind_double(insert_stmt_.get(), 5, row.elapsed_time);
        bind_double(insert_stmt_.get(), 6, row.value);
        sqlite3_step(insert_stmt_.get());
    }
    txn.commit();
    buffer_.clear();
}

int GeoPackageOutputPlugin::lookup_variable(const std::string& name, const std::string& obj_type) {
    auto it = variable_ids_.find(name + ":" + obj_type);
    return it != variable_ids_.end() ? it->second : -1;
}

} // namespace openswmm::gpkg
