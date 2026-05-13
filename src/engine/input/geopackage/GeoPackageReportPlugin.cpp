/**
 * @file GeoPackageReportPlugin.cpp
 * @brief IReportPlugin that writes summary statistics to a GeoPackage.
 * @ingroup engine_geopackage
 */

#include "GeoPackageReportPlugin.hpp"
#include "GeoPackageSchema.hpp"

#include "core/SimulationContext.hpp"
#include "data/NodeData.hpp"
#include "data/LinkData.hpp"
#include "data/SubcatchData.hpp"

namespace openswmm::gpkg {

int GeoPackageReportPlugin::initialize(const std::vector<std::string>& init_args,
                                        const IPluginComponentInfo* /*info*/) {
    if (init_args.size() < 1) {
        error_msg_ = "GeoPackageReportPlugin requires at least 1 argument: db path";
        return -1;
    }
    db_path_ = init_args[0];
    simulation_id_ = init_args.size() > 1 ? init_args[1] : "run_1";
    state_ = PluginState::INITIALIZED;
    return 0;
}

int GeoPackageReportPlugin::validate(const SimulationContext& /*ctx*/) {
    state_ = PluginState::VALIDATED;
    return 0;
}

int GeoPackageReportPlugin::prepare(const SimulationContext& /*ctx*/) {
    try {
        db_ = open_database(db_path_);

        // Cache variable IDs
        auto stmt = gpkg::prepare(db_.get(), "SELECT variable_id, name, object_type FROM variables");
        while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
            int vid = column_int(stmt.get(), 0);
            std::string key = column_text(stmt.get(), 1) + ":" + column_text(stmt.get(), 2);
            variable_ids_[key] = vid;
        }

        state_ = PluginState::PREPARED;
        return 0;
    } catch (const std::exception& e) {
        error_msg_ = e.what();
        return -1;
    }
}

int GeoPackageReportPlugin::update(const SimulationSnapshot& /*snapshot*/) {
    return 0; // No per-timestep accumulation needed; stats are in NodeData/LinkData
}

int GeoPackageReportPlugin::write_summary(const SimulationContext& ctx) {
    try {
        auto stmt = gpkg::prepare(db_.get(),
            "INSERT OR REPLACE INTO result_summary "
            "(simulation_id, object_type, object_id, variable_id, value) "
            "VALUES (?, ?, ?, ?, ?)");

        Transaction txn(db_.get());

        auto insert = [&](const std::string& obj_type, const std::string& obj_id,
                          const std::string& var_name, double value) {
            int vid = lookup_variable(var_name, obj_type);
            if (vid < 0) return;
            sqlite3_reset(stmt.get());
            sqlite3_clear_bindings(stmt.get());
            bind_text(stmt.get(), 1, simulation_id_);
            bind_text(stmt.get(), 2, obj_type);
            bind_text(stmt.get(), 3, obj_id);
            bind_int(stmt.get(), 4, vid);
            bind_double(stmt.get(), 5, value);
            sqlite3_step(stmt.get());
        };

        // Node statistics
        for (int i = 0; i < ctx.node_names.size(); ++i) {
            if (!ctx.nodes.rpt_flag[static_cast<std::size_t>(i)]) continue;
            const auto& name = ctx.node_names.name_of(i);
            if (i < (int)ctx.nodes.stat_max_depth.size())
                insert("NODE", name, "max_depth", ctx.nodes.stat_max_depth[i]);
            if (i < (int)ctx.nodes.stat_max_overflow.size())
                insert("NODE", name, "max_overflow", ctx.nodes.stat_max_overflow[i]);
            if (i < (int)ctx.nodes.stat_time_flooded.size())
                insert("NODE", name, "time_flooded", ctx.nodes.stat_time_flooded[i]);
        }

        // Link statistics
        for (int i = 0; i < ctx.link_names.size(); ++i) {
            if (!ctx.links.rpt_flag[static_cast<std::size_t>(i)]) continue;
            const auto& name = ctx.link_names.name_of(i);
            if (i < (int)ctx.links.stat_max_flow.size())
                insert("LINK", name, "max_flow", ctx.links.stat_max_flow[i]);
            if (i < (int)ctx.links.stat_max_veloc.size())
                insert("LINK", name, "max_velocity", ctx.links.stat_max_veloc[i]);
            if (i < (int)ctx.links.stat_max_filling.size())
                insert("LINK", name, "max_filling", ctx.links.stat_max_filling[i]);
            if (i < (int)ctx.links.stat_time_surcharged.size())
                insert("LINK", name, "time_surcharged", ctx.links.stat_time_surcharged[i]);
        }

        // Subcatchment statistics
        for (int i = 0; i < ctx.subcatch_names.size(); ++i) {
            if (!ctx.subcatches.rpt_flag[static_cast<std::size_t>(i)]) continue;
            const auto& name = ctx.subcatch_names.name_of(i);
            if (i < (int)ctx.subcatches.stat_precip_vol.size())
                insert("SUBCATCH", name, "precip_volume", ctx.subcatches.stat_precip_vol[i]);
            if (i < (int)ctx.subcatches.stat_runoff_vol.size())
                insert("SUBCATCH", name, "runoff_volume", ctx.subcatches.stat_runoff_vol[i]);
        }

        // Update simulation continuity errors
        auto upd = gpkg::prepare(db_.get(),
            "UPDATE simulations SET "
            "continuity_error_runoff = ?, continuity_error_flow = ?, "
            "continuity_error_quality = ?, status = 'completed' "
            "WHERE simulation_id = ?");
        bind_double(upd.get(), 1, ctx.mass_balance.runoff_error());
        bind_double(upd.get(), 2, ctx.mass_balance.routing_error());
        bind_double(upd.get(), 3, 0.0); // quality error not yet tracked
        bind_text(upd.get(), 4, simulation_id_);
        sqlite3_step(upd.get());

        txn.commit();
        return 0;
    } catch (const std::exception& e) {
        error_msg_ = e.what();
        return -1;
    }
}

int GeoPackageReportPlugin::finalize(const SimulationContext& /*ctx*/) {
    db_.reset();
    state_ = PluginState::FINALIZED;
    return 0;
}

int GeoPackageReportPlugin::lookup_variable(const std::string& name, const std::string& obj_type) {
    auto it = variable_ids_.find(name + ":" + obj_type);
    return it != variable_ids_.end() ? it->second : -1;
}

} // namespace openswmm::gpkg
