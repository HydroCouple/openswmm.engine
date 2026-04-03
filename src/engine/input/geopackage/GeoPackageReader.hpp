/**
 * @file GeoPackageReader.hpp
 * @brief Reads a GeoPackage file into a SimulationContext (model input tables).
 * @ingroup engine_geopackage
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_GEOPACKAGE_READER_HPP
#define OPENSWMM_GEOPACKAGE_READER_HPP

#include <string>

struct sqlite3;

namespace openswmm {
    struct SimulationContext;
}

namespace openswmm::gpkg {

/**
 * @brief Read a model definition from a GeoPackage into a SimulationContext.
 *
 * @param db             Open SQLite database handle.
 * @param ctx            Simulation context to populate.
 * @param simulation_id  Which model instance to load.
 * @returns 0 on success, non-zero on error.
 */
int read_model(sqlite3* db, SimulationContext& ctx, const std::string& simulation_id);

/**
 * @brief Convenience: open a GeoPackage file and read a model.
 *
 * @param path           Path to the .gpkg file.
 * @param ctx            Simulation context to populate.
 * @param simulation_id  Which model instance to load.
 * @returns 0 on success, non-zero on error.
 */
int read_from_file(const std::string& path, SimulationContext& ctx,
                   const std::string& simulation_id);

} // namespace openswmm::gpkg

#endif // OPENSWMM_GEOPACKAGE_READER_HPP
