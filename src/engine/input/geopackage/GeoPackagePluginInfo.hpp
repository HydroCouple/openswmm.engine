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
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
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
        return "MIT";
    }

    std::string license_text() const override {
        return "Copyright (c) 2026 HydroCouple. All rights reserved.\n"
               "MIT License — see LICENSE file for full text.";
    }

    // -----------------------------------------------------------------------
    // Capability queries (IPluginComponentInfo)
    // -----------------------------------------------------------------------

    bool has_input()  const noexcept override { return true; }
    bool has_output() const noexcept override { return true; }
    bool has_report() const noexcept override { return true; }

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
// C export for plugin discovery
// ============================================================================

extern "C" {

/**
 * @brief Plugin discovery entry point.
 *
 * @details The PluginFactory calls this after dlopen() to obtain the
 *          component info singleton. The returned pointer is NOT owned
 *          by the caller — it points to a static singleton.
 */
openswmm::IPluginComponentInfo* openswmm_plugin_info(void);

}

#endif // OPENSWMM_GEOPACKAGE_PLUGIN_INFO_HPP
