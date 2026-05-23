/**
 * @file GeoPackageInputPlugin.cpp
 * @brief IInputPlugin implementation for GeoPackage format.
 * @ingroup engine_geopackage
 */

#include "GeoPackageInputPlugin.hpp"
#include "GeoPackageReader.hpp"
#include "GeoPackageWriter.hpp"
#include "GeoPackageSchema.hpp"
#include "GpkgUtils.hpp"

#include "core/SimulationContext.hpp"

namespace openswmm::gpkg {

int GeoPackageInputPlugin::initialize(const std::vector<std::string>& init_args,
                                       const IPluginComponentInfo* /*info*/) {
    // Optional args: [0] = simulation_id, [1] = srs_id
    if (!init_args.empty())
        simulation_id_ = init_args[0];
    if (init_args.size() > 1) {
        try { srs_id_ = std::stoi(init_args[1]); }
        catch (...) { srs_id_ = 0; }
    }
    state_ = PluginState::INITIALIZED;
    return 0;
}

int GeoPackageInputPlugin::validate(const SimulationContext& /*ctx*/) {
    state_ = PluginState::VALIDATED;
    return 0;
}

int GeoPackageInputPlugin::read(const std::string& path, SimulationContext& ctx) {
    try {
        auto db = open_database(path, SQLITE_OPEN_READONLY);

        // If simulation_id is "default", try to find the first available one
        if (simulation_id_ == "default") {
            // Check if there's a simulations table with entries
            auto stmt = prepare(db.get(),
                "SELECT simulation_id FROM simulations LIMIT 1");
            if (sqlite3_step(stmt.get()) == SQLITE_ROW) {
                simulation_id_ = column_text(stmt.get(), 0);
            } else {
                // No simulations table entry; try to find any simulation_id in nodes
                auto stmt2 = prepare(db.get(),
                    "SELECT DISTINCT simulation_id FROM nodes LIMIT 1");
                if (sqlite3_step(stmt2.get()) == SQLITE_ROW) {
                    simulation_id_ = column_text(stmt2.get(), 0);
                }
            }
        }

        int rc = read_model(db.get(), ctx, simulation_id_);
        if (rc != 0) {
            error_msg_ = "Failed to read model from GeoPackage";
            return rc;
        }
        return 0;
    } catch (const std::exception& e) {
        error_msg_ = e.what();
        return -1;
    }
}

int GeoPackageInputPlugin::write(const std::string& path, const SimulationContext& ctx) {
    try {
        auto db = open_database(path);
        create_schema(db.get());

        // Determine SRS ID from CRS string
        int srs_id = srs_id_;
        if (srs_id == 0 && !ctx.spatial.crs.empty()) {
            // Try to parse EPSG code from "EPSG:XXXX" format
            auto pos = ctx.spatial.crs.find(':');
            if (pos != std::string::npos) {
                try { srs_id = std::stoi(ctx.spatial.crs.substr(pos + 1)); }
                catch (...) {}
            }
            if (srs_id > 0) {
                register_crs(db.get(), srs_id, "EPSG", srs_id,
                             ctx.spatial.crs, ctx.spatial.crs);
            }
        }

        write_model(db.get(), ctx, simulation_id_, srs_id);
        return 0;
    } catch (const std::exception& e) {
        error_msg_ = e.what();
        return -1;
    }
}

int GeoPackageInputPlugin::finalize(const SimulationContext& /*ctx*/) {
    state_ = PluginState::FINALIZED;
    return 0;
}

} // namespace openswmm::gpkg
