/**
 * @file GeoPackagePluginInfo.cpp
 * @brief IPluginComponentInfo implementation for the GeoPackage I/O plugin.
 * @ingroup engine_geopackage
 */

#include "GeoPackagePluginInfo.hpp"
#include "GeoPackageInputPlugin.hpp"
#include "GeoPackageOutputPlugin.hpp"
#include "GeoPackageReportPlugin.hpp"

namespace openswmm::gpkg {

IInputPlugin* GeoPackagePluginInfo::create_input_plugin() const {
    return new GeoPackageInputPlugin();
}

IOutputPlugin* GeoPackagePluginInfo::create_output_plugin() const {
    return new GeoPackageOutputPlugin();
}

IReportPlugin* GeoPackagePluginInfo::create_report_plugin() const {
    return new GeoPackageReportPlugin();
}

bool GeoPackagePluginInfo::register_plugin(const RegistrationInfo& info) {
    // Store the registration info
    reg_info_ = info;
    registered_ = true;
    return true;
}

} // namespace openswmm::gpkg

// ============================================================================
// C export
// ============================================================================

extern "C" openswmm::IPluginComponentInfo* openswmm_plugin_info(void) {
    return &openswmm::gpkg::GeoPackagePluginInfo::instance();
}
