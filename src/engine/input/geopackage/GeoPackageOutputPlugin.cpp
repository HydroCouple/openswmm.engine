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

namespace {

/// The `variables.units` label for a species column.
///
/// Water age is HOURS; a pollutant carries its own concentration unit. The
/// binary `.out` cannot express this (its per-column unit field is a
/// three-value concentration enum with no HOURS slot, which is why the
/// species NAME is the discriminator there) — the GeoPackage `variables`
/// table has a free-text `units` column, so here the unit can simply be
/// stated.
const char* species_units_label(MassUnits u) {
    switch (u) {
        case MassUnits::UG_PER_L:     return "ug/L";
        case MassUnits::COUNTS_PER_L: return "#/L";
        case MassUnits::MG_PER_L:     break;
    }
    return "mg/L";
}

}  // namespace

int GeoPackageOutputPlugin::register_species_variables(const SimulationContext& ctx) {
    // Mirrors DefaultOutputPlugin: IGNORE_QUALITY means no species columns
    // anywhere, so nothing is registered and update()'s lookups keep
    // returning −1 as before.
    if (ctx.options.ignore_quality) return 0;

    const int nr = ctx.n_reported_species();
    if (nr <= 0) return 0;

    // The REPORTED stride (pollutants, then __WATER_AGE__ when enabled) —
    // the same single truth the .out writer strides by. Indices at or past
    // n_pollutants() are the reserved age row.
    const int np = ctx.n_pollutants();

    // All three object types: update() already reads subcatch_quality,
    // node_quality and link_quality and looks each species up per type.
    // Subcatchment age reports 0 until plan phase A3; that is a VALUE
    // question, not a registration one, and the .out already carries the
    // same zero column, so the two outputs stay consistent.
    static const char* const kObjTypes[] = {"NODE", "LINK", "SUBCATCH"};

    auto probe = gpkg::prepare(db_.get(),
        "SELECT category FROM variables WHERE name = ? AND object_type = ?");
    auto ins = gpkg::prepare(db_.get(),
        "INSERT INTO variables (name, object_type, category, units, description) "
        "VALUES (?, ?, 'QUALITY', ?, ?)");

    for (int s = 0; s < nr; ++s) {
        const std::string& sname = ctx.reported_species_names[static_cast<std::size_t>(s)];
        const bool is_age = (s >= np);
        const std::string units = is_age
            ? "hours"
            : species_units_label(ctx.pollutants.units[static_cast<std::size_t>(s)]);
        const std::string desc = is_age ? "Water age" : "Species concentration";

        for (const char* obj : kObjTypes) {
            sqlite3_reset(probe.get());
            sqlite3_clear_bindings(probe.get());
            bind_text(probe.get(), 1, sname);
            bind_text(probe.get(), 2, obj);

            if (sqlite3_step(probe.get()) == SQLITE_ROW) {
                // A row already exists for this (name, object_type).
                if (column_text(probe.get(), 0) == "QUALITY") continue;  // re-run into the same file

                // NAME COLLISION with a hydraulic variable. This must not be
                // ignored: `variables` is UNIQUE(name, object_type), so an
                // INSERT OR IGNORE here would leave the hydraulic row in
                // place and update()'s lookup_variable(sname, obj) would
                // resolve to ITS id — writing species concentrations into,
                // say, the node depth series. Silently corrupting an
                // existing result is worse than refusing to open, and the
                // condition is entirely under the user's control (rename the
                // pollutant), so this fails loudly instead.
                error_msg_ = "GeoPackage output: species '" + sname +
                             "' collides with the built-in " + obj +
                             " variable of the same name. Rename the "
                             "pollutant; writing it would overwrite that "
                             "variable's results.";
                return -1;
            }

            sqlite3_reset(ins.get());
            sqlite3_clear_bindings(ins.get());
            bind_text(ins.get(), 1, sname);
            bind_text(ins.get(), 2, obj);
            bind_text(ins.get(), 3, units);
            bind_text(ins.get(), 4, desc);
            sqlite3_step(ins.get());
        }
    }
    return 0;
}

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

        // Species are model-dependent, so they cannot live in the static
        // default list. MUST run before the variable-ID cache below, which
        // is built once from the table.
        if (register_species_variables(ctx) != 0) {
            state_ = PluginState::ERROR;
            return -1;
        }

        // Per-timestep results arrive pre-converted to display units (engine
        // boundary); no plugin-side conversion factors needed.

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
                    snapshot.subcatch.rainfall[ui]});

            vid = lookup_variable("snow_depth", "SUBCATCH");
            if (vid >= 0 && ui < snapshot.subcatch.snow_depth.size())
                buffer_.push_back({"SUBCATCH", name, vid, sim_time,
                    snapshot.subcatch.snow_depth[ui]});

            vid = lookup_variable("evap_loss", "SUBCATCH");
            if (vid >= 0 && ui < snapshot.subcatch.evap.size())
                buffer_.push_back({"SUBCATCH", name, vid, sim_time,
                    snapshot.subcatch.evap[ui]});

            vid = lookup_variable("infil_loss", "SUBCATCH");
            if (vid >= 0 && ui < snapshot.subcatch.infil.size())
                buffer_.push_back({"SUBCATCH", name, vid, sim_time,
                    snapshot.subcatch.infil[ui]});

            vid = lookup_variable("runoff", "SUBCATCH");
            if (vid >= 0 && ui < snapshot.subcatch.runoff.size())
                buffer_.push_back({"SUBCATCH", name, vid, sim_time,
                    snapshot.subcatch.runoff[ui]});

            vid = lookup_variable("gw_flow", "SUBCATCH");
            if (vid >= 0 && ui < snapshot.subcatch.gw_flow.size())
                buffer_.push_back({"SUBCATCH", name, vid, sim_time,
                    snapshot.subcatch.gw_flow[ui]});

            vid = lookup_variable("gw_elev", "SUBCATCH");
            if (vid >= 0 && ui < snapshot.subcatch.gw_elev.size())
                buffer_.push_back({"SUBCATCH", name, vid, sim_time,
                    snapshot.subcatch.gw_elev[ui]});

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
                    snapshot.nodes.depth[ui]});

            vid = lookup_variable("head", "NODE");
            if (vid >= 0 && ui < snapshot.nodes.head.size())
                buffer_.push_back({"NODE", name, vid, sim_time,
                    snapshot.nodes.head[ui]});

            vid = lookup_variable("volume", "NODE");
            if (vid >= 0 && ui < snapshot.nodes.volume.size())
                buffer_.push_back({"NODE", name, vid, sim_time,
                    snapshot.nodes.volume[ui]});

            vid = lookup_variable("lateral_inflow", "NODE");
            if (vid >= 0 && ui < snapshot.nodes.lateral_inflow.size())
                buffer_.push_back({"NODE", name, vid, sim_time,
                    snapshot.nodes.lateral_inflow[ui]});

            vid = lookup_variable("total_inflow", "NODE");
            if (vid >= 0 && ui < snapshot.nodes.total_inflow.size())
                buffer_.push_back({"NODE", name, vid, sim_time,
                    snapshot.nodes.total_inflow[ui]});

            vid = lookup_variable("overflow", "NODE");
            if (vid >= 0 && ui < snapshot.nodes.overflow.size())
                buffer_.push_back({"NODE", name, vid, sim_time,
                    snapshot.nodes.overflow[ui]});

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
                    snapshot.links.flow[ui]});

            vid = lookup_variable("depth", "LINK");
            if (vid >= 0 && ui < snapshot.links.depth.size())
                buffer_.push_back({"LINK", name, vid, sim_time,
                    snapshot.links.depth[ui]});

            vid = lookup_variable("velocity", "LINK");
            if (vid >= 0 && ui < snapshot.links.velocity.size())
                buffer_.push_back({"LINK", name, vid, sim_time,
                    snapshot.links.velocity[ui]});

            vid = lookup_variable("volume", "LINK");
            if (vid >= 0 && ui < snapshot.links.volume.size())
                buffer_.push_back({"LINK", name, vid, sim_time,
                    snapshot.links.volume[ui]});

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

            // Temperature already in display units (°F US | °C SI) from boundary
            vid = lookup_variable("air_temp", "SYSTEM");
            if (vid >= 0)
                buffer_.push_back({"SYSTEM", "system", vid, sim_time,
                    snapshot.sys_temperature});

            vid = lookup_variable("rainfall", "SYSTEM");
            if (vid >= 0)
                buffer_.push_back({"SYSTEM", "system", vid, sim_time,
                    snapshot.sys_rainfall});

            vid = lookup_variable("snow_depth", "SYSTEM");
            if (vid >= 0)
                buffer_.push_back({"SYSTEM", "system", vid, sim_time,
                    snapshot.sys_snow_depth});

            vid = lookup_variable("infil", "SYSTEM");
            if (vid >= 0)
                buffer_.push_back({"SYSTEM", "system", vid, sim_time,
                    snapshot.sys_infil});

            vid = lookup_variable("runoff", "SYSTEM");
            if (vid >= 0)
                buffer_.push_back({"SYSTEM", "system", vid, sim_time,
                    snapshot.sys_runoff});

            vid = lookup_variable("dw_inflow", "SYSTEM");
            if (vid >= 0)
                buffer_.push_back({"SYSTEM", "system", vid, sim_time,
                    snapshot.sys_dw_inflow});

            vid = lookup_variable("gw_inflow", "SYSTEM");
            if (vid >= 0)
                buffer_.push_back({"SYSTEM", "system", vid, sim_time,
                    snapshot.sys_gw_inflow});

            vid = lookup_variable("ii_inflow", "SYSTEM");
            if (vid >= 0)
                buffer_.push_back({"SYSTEM", "system", vid, sim_time,
                    snapshot.sys_ii_inflow});

            vid = lookup_variable("ext_inflow", "SYSTEM");
            if (vid >= 0)
                buffer_.push_back({"SYSTEM", "system", vid, sim_time,
                    snapshot.sys_ext_inflow});

            vid = lookup_variable("total_inflow", "SYSTEM");
            if (vid >= 0) {
                double total = snapshot.sys_runoff + snapshot.sys_dw_inflow +
                               snapshot.sys_gw_inflow + snapshot.sys_ii_inflow +
                               snapshot.sys_ext_inflow;
                buffer_.push_back({"SYSTEM", "system", vid, sim_time, total});
            }

            vid = lookup_variable("flooding", "SYSTEM");
            if (vid >= 0)
                buffer_.push_back({"SYSTEM", "system", vid, sim_time,
                    snapshot.sys_flooding});

            vid = lookup_variable("outflow", "SYSTEM");
            if (vid >= 0)
                buffer_.push_back({"SYSTEM", "system", vid, sim_time,
                    snapshot.sys_outflow});

            vid = lookup_variable("storage", "SYSTEM");
            if (vid >= 0)
                buffer_.push_back({"SYSTEM", "system", vid, sim_time,
                    snapshot.sys_storage});

            vid = lookup_variable("evap", "SYSTEM");
            if (vid >= 0)
                buffer_.push_back({"SYSTEM", "system", vid, sim_time,
                    snapshot.sys_evap});

            vid = lookup_variable("pet", "SYSTEM");
            if (vid >= 0)
                buffer_.push_back({"SYSTEM", "system", vid, sim_time,
                    snapshot.sys_pet});
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
