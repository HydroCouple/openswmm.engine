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
// C export — Slice RC.2
// ============================================================================
//
// This factory function used to be the discovery entry point: PluginFactory
// scanned the engine's library directory, dlsym'd `openswmm_plugin_info` on
// each loadable .so / .dylib / .dll, and if non-null registered the returned
// info pointer. Because GeoPackage is statically linked into the engine
// SHARED, the symbol was visible from the engine binary itself — and the
// scan picked it up, producing a "phantom" GeoPackage plugin row alongside
// the explicit-built-ins.
//
// Slice RC.1 (APPROVED 2026-05-25) registers GeoPackagePluginInfo
// explicitly via PluginFactory::register_builtin_infos. The function below
// is preserved for compatibility with any third-party tool that resolved
// the symbol via dlsym, BUT it is now annotated `hidden` so it no longer
// leaks out of the engine SHARED. Combined with the
// CXX_VISIBILITY_PRESET=hidden / VISIBILITY_INLINES_HIDDEN=ON properties
// on the openswmm_geopackage target, this closes the symbol leak that
// PluginFactory::discover() was inadvertently picking up.
//
// On MSVC, visibility attributes are no-ops; the absence of
// __declspec(dllexport) (and the lack of a corresponding .def entry) is
// what keeps the symbol off the engine DLL export table.

#if defined(__GNUC__) || defined(__clang__)
#  define OPENSWMM_GPKG_HIDDEN __attribute__((visibility("hidden")))
#else
#  define OPENSWMM_GPKG_HIDDEN
#endif

extern "C" OPENSWMM_GPKG_HIDDEN
openswmm::IPluginComponentInfo* openswmm_plugin_info(void) {
    return &openswmm::gpkg::GeoPackagePluginInfo::instance();
}
