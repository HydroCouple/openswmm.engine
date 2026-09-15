// SPDX-License-Identifier: Apache-2.0
//
// Copyright 2026 Caleb Buahin
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/**
 * @file GeoPackagePluginInfo.hpp
 * @brief IPluginComponentInfo implementation for the GeoPackage I/O plugin.
 *
 * @details Makes the GeoPackage plugin discoverable by the PluginFactory.
 *          Provides factory methods for all three plugin types (input, output,
 *          report) since GeoPackage handles the full I/O lifecycle.
 *
 *          The plugin supports optional registration via register_plugin() /
 *          registered(). Registration allows external license managers or
 *          deployment tools to gate access when required. By default, the
 *          plugin is unregistered; calling register_plugin() with valid
 *          registration info activates it.
 *
 * @ingroup engine_geopackage
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_GEOPACKAGE_PLUGIN_INFO_HPP
#define OPENSWMM_GEOPACKAGE_PLUGIN_INFO_HPP

#include <openswmm/plugin_sdk/IPluginComponentInfo.hpp>
#include <string>
#include <vector>

namespace openswmm::gpkg {

/**
 * @brief IPluginComponentInfo for the GeoPackage I/O plugin.
 *
 * @details Singleton — obtain via GeoPackagePluginInfo::instance().
 *          The PluginFactory discovers this through the exported C function
 *          `openswmm_plugin_info()`.
 *
 * @ingroup engine_geopackage
 */
class GeoPackagePluginInfo : public IPluginComponentInfo {
public:

    /**
     * @brief Get the singleton instance.
     */
    static GeoPackagePluginInfo& instance() {
        static GeoPackagePluginInfo inst;
        return inst;
    }

    // -----------------------------------------------------------------------
    // Identity & metadata (IPluginComponentInfo)
    // -----------------------------------------------------------------------

    std::string id() const override {
        return "org.hydrocouple.openswmm.plugins.geopackage";
    }

    std::string caption() const override {
        return "GeoPackage I/O Plugin";
    }

    std::string description() const override {
        return "Reads and writes SWMM model definitions, simulation results, "
               "and observed data using the OGC GeoPackage format (SQLite with "
               "spatial extensions). Supports multi-run storage, network topology "
               "queries, and calibration workflows in a single .gpkg file.";
    }

    std::string version() const override {
        return "1.0.0";
    }

    std::string vendor() const override {
        return "HydroCouple";
    }

    std::string url() const override {
        return "https://hydrocouple.org/projects/openswmm/plugins/geopackage";
    }

    std::vector<std::string> tags() const override {
        return {"geopackage", "sqlite", "spatial", "input", "output", "report",
                "ogc", "gis", "calibration"};
    }

    // -----------------------------------------------------------------------
    // Licensing (IPluginComponentInfo)
    // -----------------------------------------------------------------------

    std::string license_type() const override {
        return "Apache-2.0";
    }

    std::string license_text() const override {
        return "Copyright (c) 2026 Caleb Buahin. All rights reserved.\n"
               "Licensed under the Apache License, Version 2.0 — see the\n"
               "LICENSE and NOTICE files for full text.";
    }

    // -----------------------------------------------------------------------
    // Capability queries (IPluginComponentInfo)
    // -----------------------------------------------------------------------

    bool has_input()  const noexcept override { return true; }
    bool has_output() const noexcept override { return true; }
    bool has_report() const noexcept override { return true; }

    // -----------------------------------------------------------------------
    // File-filter advertisement (IPluginComponentInfo)
    // -----------------------------------------------------------------------

    std::vector<FileFilter> file_filters() const override {
        const std::string desc        = "OGC GeoPackage";
        const std::vector<std::string> patterns = {"*.gpkg"};
        const std::vector<std::string> mimes    = {"application/geopackage+sqlite3"};
        return {
            FileFilter{ desc, patterns, PluginRole::INPUT_READ,    mimes },
            FileFilter{ desc, patterns, PluginRole::OUTPUT_WRITE,  mimes },
            FileFilter{ desc, patterns, PluginRole::REPORT_WRITE,  mimes },
        };
    }

    // -----------------------------------------------------------------------
    // Factory methods (IPluginComponentInfo)
    // -----------------------------------------------------------------------

    IInputPlugin*  create_input_plugin()  const override;
    IOutputPlugin* create_output_plugin() const override;
    IReportPlugin* create_report_plugin() const override;

    // -----------------------------------------------------------------------
    // Registration (IPluginComponentInfo overrides)
    // -----------------------------------------------------------------------

    bool register_plugin(const RegistrationInfo& info) override;
    bool registered() const noexcept override { return registered_; }
    RegistrationInfo registration_info() const override { return reg_info_; }

private:
    GeoPackagePluginInfo() = default;
    GeoPackagePluginInfo(const GeoPackagePluginInfo&) = delete;
    GeoPackagePluginInfo& operator=(const GeoPackagePluginInfo&) = delete;

    bool registered_ = false;
    RegistrationInfo reg_info_;
};

} // namespace openswmm::gpkg

// ============================================================================
// Slice RC.2 — `openswmm_plugin_info` is no longer a public symbol.
// ============================================================================
//
// The C factory function in GeoPackagePluginInfo.cpp is annotated with
// __attribute__((visibility("hidden"))) and the GeoPackage CMake target
// ships with CXX_VISIBILITY_PRESET=hidden / VISIBILITY_INLINES_HIDDEN=ON.
// Together these prevent the dlsym leak that PluginFactory::discover()
// was picking up on the engine's SHARED binary.
//
// Hosts that need the GeoPackage plugin should obtain it via the
// PluginFactory (it is now registered as a built-in in
// register_builtin_infos when OPENSWMM_HAS_GEOPACKAGE is defined) or via
// openswmm::discover_plugins_by_id() — NOT via dlsym on the engine
// library. See §R.3 of GUI_IMPLEMENTATION_PLAN.md (in the
// openswmm.gui repo) for the full rationale.

#endif // OPENSWMM_GEOPACKAGE_PLUGIN_INFO_HPP
