/**
 * @file GeoPackageOutputPlugin.hpp
 * @brief IOutputPlugin that writes per-timestep results to a GeoPackage.
 * @ingroup engine_geopackage
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_GEOPACKAGE_OUTPUT_PLUGIN_HPP
#define OPENSWMM_GEOPACKAGE_OUTPUT_PLUGIN_HPP

#include "GpkgUtils.hpp"
#include <openswmm/plugin_sdk/IOutputPlugin.hpp>
#include <string>
#include <unordered_map>
#include <vector>

namespace openswmm::gpkg {

class GeoPackageOutputPlugin : public IOutputPlugin {
public:
    GeoPackageOutputPlugin() = default;

    PluginState state() const noexcept override { return state_; }

    int initialize(const std::vector<std::string>& init_args,
                   const IPluginComponentInfo* info) override;

    int validate(const SimulationContext& ctx) override;
    int prepare(const SimulationContext& ctx) override;
    int update(const SimulationSnapshot& snapshot) override;
    int finalize(const SimulationContext& ctx) override;

    const char* last_error_message() const noexcept override {
        return error_msg_.c_str();
    }

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

    void flush_buffer();
    int lookup_variable(const std::string& name, const std::string& obj_type);
};

} // namespace openswmm::gpkg

#endif // OPENSWMM_GEOPACKAGE_OUTPUT_PLUGIN_HPP
