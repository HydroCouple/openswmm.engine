/**
 * @file GeoPackageOutputPlugin.cpp
 * @brief IOutputPlugin that writes per-timestep results to a GeoPackage.
 * @ingroup engine_geopackage
 */

#include "GeoPackageOutputPlugin.hpp"
#include "GeoPackageSchema.hpp"
#include "GeoPackageWriter.hpp"

#include "core/SimulationContext.hpp"
#include "core/UnitConversion.hpp"
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

        // Compute unit conversion factors once
        unit_system_   = ucf::getUnitSystem(static_cast<int>(ctx.options.flow_units));
        ucf_rainfall_  = ucf::UCF(ucf::RAINFALL,  ctx.options);
        ucf_raindepth_ = ucf::UCF(ucf::RAINDEPTH, ctx.options);
        ucf_evaprate_  = ucf::UCF(ucf::EVAPRATE,  ctx.options);
        ucf_length_    = ucf::UCF(ucf::LENGTH,    ctx.options);
        ucf_volume_    = ucf::UCF(ucf::VOLUME,    ctx.options);
        ucf_flow_      = ucf::UCF(ucf::FLOW,      ctx.options);

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

        // Cache per-object report flags for use in update()
        subcatch_rpt_flag_ = ctx.subcatches.rpt_flag;
        node_rpt_flag_     = ctx.nodes.rpt_flag;
        link_rpt_flag_     = ctx.links.rpt_flag;

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

        // -------------------------------------------------------------------
        // Subcatchment results
        // rainfall, snow_depth, evap_loss, infil_loss, runoff, gw_flow,
        // gw_elev, soil_moist + pollutant washoff
        // -------------------------------------------------------------------
        for (int i = 0; i < snapshot.subcatch_count; ++i) {
            auto ui = static_cast<std::size_t>(i);
            if (ui < subcatch_rpt_flag_.size() && !subcatch_rpt_flag_[ui]) continue;
            std::string name = (snapshot.subcatch_ids && i < (int)snapshot.subcatch_ids->size())
                ? (*snapshot.subcatch_ids)[i] : std::to_string(i);

            int vid;
            vid = lookup_variable("rainfall", "SUBCATCH");
            if (vid >= 0 && ui < snapshot.subcatch.rainfall.size())
                buffer_.push_back({"SUBCATCH", name, vid, sim_time,
                    snapshot.subcatch.rainfall[ui] * ucf_rainfall_});

            vid = lookup_variable("snow_depth", "SUBCATCH");
            if (vid >= 0 && ui < snapshot.subcatch.snow_depth.size())
                buffer_.push_back({"SUBCATCH", name, vid, sim_time,
                    snapshot.subcatch.snow_depth[ui] * ucf_raindepth_});

            vid = lookup_variable("evap_loss", "SUBCATCH");
            if (vid >= 0 && ui < snapshot.subcatch.evap.size())
                buffer_.push_back({"SUBCATCH", name, vid, sim_time,
                    snapshot.subcatch.evap[ui] * ucf_evaprate_});

            vid = lookup_variable("infil_loss", "SUBCATCH");
            if (vid >= 0 && ui < snapshot.subcatch.infil.size())
                buffer_.push_back({"SUBCATCH", name, vid, sim_time,
                    snapshot.subcatch.infil[ui] * ucf_rainfall_});

            vid = lookup_variable("runoff", "SUBCATCH");
            if (vid >= 0 && ui < snapshot.subcatch.runoff.size())
                buffer_.push_back({"SUBCATCH", name, vid, sim_time,
                    snapshot.subcatch.runoff[ui] * ucf_flow_});

            vid = lookup_variable("gw_flow", "SUBCATCH");
            if (vid >= 0 && ui < snapshot.subcatch.gw_flow.size())
                buffer_.push_back({"SUBCATCH", name, vid, sim_time,
                    snapshot.subcatch.gw_flow[ui] * ucf_flow_});

            vid = lookup_variable("gw_elev", "SUBCATCH");
            if (vid >= 0 && ui < snapshot.subcatch.gw_elev.size())
                buffer_.push_back({"SUBCATCH", name, vid, sim_time,
                    snapshot.subcatch.gw_elev[ui] * ucf_length_});

            vid = lookup_variable("soil_moist", "SUBCATCH");
            if (vid >= 0 && ui < snapshot.subcatch.soil_moist.size())
                buffer_.push_back({"SUBCATCH", name, vid, sim_time,
                    snapshot.subcatch.soil_moist[ui]});

            // Pollutant washoff concentrations
            for (int p = 0; p < snapshot.pollut_count; ++p) {
                auto qi = ui * static_cast<std::size_t>(snapshot.pollut_count) + static_cast<std::size_t>(p);
                if (qi < snapshot.subcatch_quality.size() && snapshot.pollut_names) {
                    std::string pname = (*snapshot.pollut_names)[p];
                    vid = lookup_variable(pname, "SUBCATCH");
                    if (vid >= 0)
                        buffer_.push_back({"SUBCATCH", name, vid, sim_time,
                            snapshot.subcatch_quality[qi]});
                }
            }
        }

        // -------------------------------------------------------------------
        // Node results
        // depth, head, volume, lateral_inflow, total_inflow, overflow
        // + pollutant concentrations
        // -------------------------------------------------------------------
        for (int i = 0; i < snapshot.node_count; ++i) {
            auto ui = static_cast<std::size_t>(i);
            if (ui < node_rpt_flag_.size() && !node_rpt_flag_[ui]) continue;
            std::string name = (snapshot.node_ids && i < (int)snapshot.node_ids->size())
                ? (*snapshot.node_ids)[i] : std::to_string(i);

            int vid;
            vid = lookup_variable("depth", "NODE");
            if (vid >= 0 && ui < snapshot.nodes.depth.size())
                buffer_.push_back({"NODE", name, vid, sim_time,
                    snapshot.nodes.depth[ui] * ucf_length_});

            vid = lookup_variable("head", "NODE");
            if (vid >= 0 && ui < snapshot.nodes.head.size())
                buffer_.push_back({"NODE", name, vid, sim_time,
                    snapshot.nodes.head[ui] * ucf_length_});

            vid = lookup_variable("volume", "NODE");
            if (vid >= 0 && ui < snapshot.nodes.volume.size())
                buffer_.push_back({"NODE", name, vid, sim_time,
                    snapshot.nodes.volume[ui] * ucf_volume_});

            vid = lookup_variable("lateral_inflow", "NODE");
            if (vid >= 0 && ui < snapshot.nodes.lateral_inflow.size())
                buffer_.push_back({"NODE", name, vid, sim_time,
                    snapshot.nodes.lateral_inflow[ui] * ucf_flow_});

            vid = lookup_variable("total_inflow", "NODE");
            if (vid >= 0 && ui < snapshot.nodes.total_inflow.size())
                buffer_.push_back({"NODE", name, vid, sim_time,
                    snapshot.nodes.total_inflow[ui] * ucf_flow_});

            vid = lookup_variable("overflow", "NODE");
            if (vid >= 0 && ui < snapshot.nodes.overflow.size())
                buffer_.push_back({"NODE", name, vid, sim_time,
                    snapshot.nodes.overflow[ui] * ucf_flow_});

            // Pollutant concentrations
            for (int p = 0; p < snapshot.pollut_count; ++p) {
                auto qi = ui * static_cast<std::size_t>(snapshot.pollut_count) + static_cast<std::size_t>(p);
                if (qi < snapshot.node_quality.size() && snapshot.pollut_names) {
                    std::string pname = (*snapshot.pollut_names)[p];
                    vid = lookup_variable(pname, "NODE");
                    if (vid >= 0)
                        buffer_.push_back({"NODE", name, vid, sim_time,
                            snapshot.node_quality[qi]});
                }
            }
        }

        // -------------------------------------------------------------------
        // Link results
        // flow, depth, velocity, volume, capacity + pollutant concentrations
        // Direction already applied in snapshot builder.
        // -------------------------------------------------------------------
        for (int i = 0; i < snapshot.link_count; ++i) {
            auto ui = static_cast<std::size_t>(i);
            if (ui < link_rpt_flag_.size() && !link_rpt_flag_[ui]) continue;
            std::string name = (snapshot.link_ids && i < (int)snapshot.link_ids->size())
                ? (*snapshot.link_ids)[i] : std::to_string(i);

            int vid;
            vid = lookup_variable("flow", "LINK");
            if (vid >= 0 && ui < snapshot.links.flow.size())
                buffer_.push_back({"LINK", name, vid, sim_time,
                    snapshot.links.flow[ui] * ucf_flow_});

            vid = lookup_variable("depth", "LINK");
            if (vid >= 0 && ui < snapshot.links.depth.size())
                buffer_.push_back({"LINK", name, vid, sim_time,
                    snapshot.links.depth[ui] * ucf_length_});

            vid = lookup_variable("velocity", "LINK");
            if (vid >= 0 && ui < snapshot.links.velocity.size())
                buffer_.push_back({"LINK", name, vid, sim_time,
                    snapshot.links.velocity[ui] * ucf_length_});

            vid = lookup_variable("volume", "LINK");
            if (vid >= 0 && ui < snapshot.links.volume.size())
                buffer_.push_back({"LINK", name, vid, sim_time,
                    snapshot.links.volume[ui] * ucf_volume_});

            vid = lookup_variable("capacity", "LINK");
            if (vid >= 0 && ui < snapshot.links.capacity.size())
                buffer_.push_back({"LINK", name, vid, sim_time,
                    snapshot.links.capacity[ui]});

            // Pollutant concentrations
            for (int p = 0; p < snapshot.pollut_count; ++p) {
                auto qi = ui * static_cast<std::size_t>(snapshot.pollut_count) + static_cast<std::size_t>(p);
                if (qi < snapshot.link_quality.size() && snapshot.pollut_names) {
                    std::string pname = (*snapshot.pollut_names)[p];
                    vid = lookup_variable(pname, "LINK");
                    if (vid >= 0)
                        buffer_.push_back({"LINK", name, vid, sim_time,
                            snapshot.link_quality[qi]});
                }
            }
        }

        // -------------------------------------------------------------------
        // System-level results (matching DefaultOutputPlugin order)
        // -------------------------------------------------------------------
        {
            int vid;

            // Temperature — convert °F → °C for SI
            vid = lookup_variable("air_temp", "SYSTEM");
            if (vid >= 0) {
                double temp = (unit_system_ == 0)
                    ? snapshot.sys_temperature
                    : (5.0 / 9.0) * (snapshot.sys_temperature - 32.0);
                buffer_.push_back({"SYSTEM", "system", vid, sim_time, temp});
            }

            vid = lookup_variable("rainfall", "SYSTEM");
            if (vid >= 0)
                buffer_.push_back({"SYSTEM", "system", vid, sim_time,
                    snapshot.sys_rainfall * ucf_rainfall_});

            vid = lookup_variable("snow_depth", "SYSTEM");
            if (vid >= 0)
                buffer_.push_back({"SYSTEM", "system", vid, sim_time,
                    snapshot.sys_snow_depth * ucf_raindepth_});

            vid = lookup_variable("infil", "SYSTEM");
            if (vid >= 0)
                buffer_.push_back({"SYSTEM", "system", vid, sim_time,
                    snapshot.sys_infil * ucf_rainfall_});

            vid = lookup_variable("runoff", "SYSTEM");
            if (vid >= 0)
                buffer_.push_back({"SYSTEM", "system", vid, sim_time,
                    snapshot.sys_runoff * ucf_flow_});

            vid = lookup_variable("dw_inflow", "SYSTEM");
            if (vid >= 0)
                buffer_.push_back({"SYSTEM", "system", vid, sim_time,
                    snapshot.sys_dw_inflow * ucf_flow_});

            vid = lookup_variable("gw_inflow", "SYSTEM");
            if (vid >= 0)
                buffer_.push_back({"SYSTEM", "system", vid, sim_time,
                    snapshot.sys_gw_inflow * ucf_flow_});

            vid = lookup_variable("ii_inflow", "SYSTEM");
            if (vid >= 0)
                buffer_.push_back({"SYSTEM", "system", vid, sim_time,
                    snapshot.sys_ii_inflow * ucf_flow_});

            vid = lookup_variable("ext_inflow", "SYSTEM");
            if (vid >= 0)
                buffer_.push_back({"SYSTEM", "system", vid, sim_time,
                    snapshot.sys_ext_inflow * ucf_flow_});

            vid = lookup_variable("total_inflow", "SYSTEM");
            if (vid >= 0) {
                double total = (snapshot.sys_runoff + snapshot.sys_dw_inflow +
                                snapshot.sys_gw_inflow + snapshot.sys_ii_inflow +
                                snapshot.sys_ext_inflow) * ucf_flow_;
                buffer_.push_back({"SYSTEM", "system", vid, sim_time, total});
            }

            vid = lookup_variable("flooding", "SYSTEM");
            if (vid >= 0)
                buffer_.push_back({"SYSTEM", "system", vid, sim_time,
                    snapshot.sys_flooding * ucf_flow_});

            vid = lookup_variable("outflow", "SYSTEM");
            if (vid >= 0)
                buffer_.push_back({"SYSTEM", "system", vid, sim_time,
                    snapshot.sys_outflow * ucf_flow_});

            vid = lookup_variable("storage", "SYSTEM");
            if (vid >= 0)
                buffer_.push_back({"SYSTEM", "system", vid, sim_time,
                    snapshot.sys_storage * ucf_volume_});

            vid = lookup_variable("evap", "SYSTEM");
            if (vid >= 0)
                buffer_.push_back({"SYSTEM", "system", vid, sim_time,
                    snapshot.sys_evap * ucf_evaprate_});

            vid = lookup_variable("pet", "SYSTEM");
            if (vid >= 0)
                buffer_.push_back({"SYSTEM", "system", vid, sim_time,
                    snapshot.sys_pet * ucf_evaprate_});
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
