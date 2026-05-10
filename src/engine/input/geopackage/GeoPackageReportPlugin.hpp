/**
 * @file GeoPackageReportPlugin.hpp
 * @brief IReportPlugin that writes summary statistics to a GeoPackage.
 * @ingroup engine_geopackage
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_GEOPACKAGE_REPORT_PLUGIN_HPP
#define OPENSWMM_GEOPACKAGE_REPORT_PLUGIN_HPP

#include "GpkgUtils.hpp"
#include <openswmm/plugin_sdk/IReportPlugin.hpp>
#include <string>
#include <unordered_map>

namespace openswmm::gpkg {

class GeoPackageReportPlugin : public IReportPlugin {
public:
    GeoPackageReportPlugin() = default;

    PluginState state() const noexcept override { return state_; }

    int initialize(const std::vector<std::string>& init_args,
                   const IPluginComponentInfo* info) override;

    int validate(const SimulationContext& ctx) override;
    int prepare(const SimulationContext& ctx) override;
    int update(const SimulationSnapshot& snapshot) override;
    int write_summary(const SimulationContext& ctx) override;
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

    std::unordered_map<std::string, int> variable_ids_;
    int lookup_variable(const std::string& name, const std::string& obj_type);
};

} // namespace openswmm::gpkg

#endif // OPENSWMM_GEOPACKAGE_REPORT_PLUGIN_HPP
