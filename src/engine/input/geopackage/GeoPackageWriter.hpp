/**
 * @file GeoPackageWriter.hpp
 * @brief Writes a SimulationContext to a GeoPackage file (model input tables).
 * @ingroup engine_geopackage
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_GEOPACKAGE_WRITER_HPP
#define OPENSWMM_GEOPACKAGE_WRITER_HPP

#include <string>

struct sqlite3;

namespace openswmm {
    struct SimulationContext;
}

namespace openswmm::gpkg {

/**
 * @brief Write the full model definition from a SimulationContext into a GeoPackage.
 *
 * @details Creates the schema if needed, registers the CRS, and writes all
 *          SWMM input sections as feature/attribute tables. The simulation_id
 *          groups all written data for this model instance.
 *
 * @param db             Open SQLite database handle (schema must exist or will be created).
 * @param ctx            Simulation context containing the parsed model.
 * @param simulation_id  Unique ID for this model instance.
 * @param srs_id         SRS ID to use for geometry encoding (default: 0 = undefined).
 */
void write_model(sqlite3* db, const SimulationContext& ctx,
                 const std::string& simulation_id, int srs_id = 0);

/**
 * @brief Convenience: create a new GeoPackage file, write schema + model.
 *
 * @param path           Output file path (created or overwritten).
 * @param ctx            Simulation context.
 * @param simulation_id  Unique ID for this model instance.
 * @returns 0 on success, non-zero on error.
 */
int write_to_file(const std::string& path, const SimulationContext& ctx,
                  const std::string& simulation_id);

} // namespace openswmm::gpkg

#endif // OPENSWMM_GEOPACKAGE_WRITER_HPP
