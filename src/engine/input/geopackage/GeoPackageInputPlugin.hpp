/**
 * @file GeoPackageInputPlugin.hpp
 * @brief IInputPlugin implementation for reading/writing SWMM models from/to GeoPackage.
 *
 * @details This plugin detects `.gpkg` file extensions and delegates to
 *          GeoPackageReader (for read) and GeoPackageWriter (for write).
 *          It replaces the text .inp format when a GeoPackage file is provided.
 *
 * @ingroup engine_geopackage
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_GEOPACKAGE_INPUT_PLUGIN_HPP
#define OPENSWMM_GEOPACKAGE_INPUT_PLUGIN_HPP

#include <openswmm/plugin_sdk/IInputPlugin.hpp>
#include <string>
#include <vector>

namespace openswmm::gpkg {

class GeoPackageInputPlugin : public IInputPlugin {
public:
    GeoPackageInputPlugin() = default;

    PluginState state() const noexcept override { return state_; }

    int initialize(const std::vector<std::string>& init_args,
                   const IPluginComponentInfo* info) override;

    int validate(const SimulationContext& ctx) override;

    /**
     * @brief Read a SWMM model from a GeoPackage file.
     *
     * @details Reads the model for the configured simulation_id. If no
     *          simulation_id was specified, reads the first (or only) model.
     *
     * @param path  Path to the .gpkg file.
     * @param ctx   SimulationContext to populate.
     * @returns 0 on success.
     */
    int read(const std::string& path, SimulationContext& ctx) override;

    /**
     * @brief Write a SWMM model to a GeoPackage file.
     *
     * @param path  Path to the output .gpkg file.
     * @param ctx   SimulationContext to serialize.
     * @returns 0 on success.
     */
    int write(const std::string& path, const SimulationContext& ctx) override;

    std::vector<std::string> skipped_sections() const override { return {}; }

    int finalize(const SimulationContext& ctx) override;

    const char* last_error_message() const noexcept override {
        return error_msg_.c_str();
    }

private:
    PluginState state_ = PluginState::UNLOADED;
    std::string error_msg_;
    std::string simulation_id_ = "default";
    int srs_id_ = 0;
};

} // namespace openswmm::gpkg

#endif // OPENSWMM_GEOPACKAGE_INPUT_PLUGIN_HPP
