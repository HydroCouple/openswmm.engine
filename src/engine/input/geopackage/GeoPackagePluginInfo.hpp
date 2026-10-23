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
 * @file      GeoPackagePluginInfo.hpp
 * @brief     IPluginComponentInfo implementation for the GeoPackage I/O plugin.
 *
 * @details Makes the GeoPackage plugin discoverable by the PluginFactory.
 *          Provides factory methods for all three plugin types (input, output,
 *          report) since GeoPackage handles the full I/O lifecycle.
 *
 *          The plugin supports optional registration via register_plugin() /
 *          registered(). The stored registration record is metadata only: any record is accepted
 *          and factories do not enforce a registration gate. By default the
 *          registration flag is false.
 *
 * @ingroup engine_geopackage
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
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
     * @brief Borrow the process-local GeoPackage metadata singleton.
     *
     * Function-local construction is thread-safe; registration mutation is not synchronized.
     * @return Singleton reference, valid until static destruction; callers do not delete it.
     */
    static GeoPackagePluginInfo& instance() {
        static GeoPackagePluginInfo inst;
        return inst;
    }

    // -----------------------------------------------------------------------
    // Identity & metadata (IPluginComponentInfo)
    // -----------------------------------------------------------------------

    /**
     * @brief Copy plugin id metadata.
     * @return Independent owned string. Stable plugin identifier
     *     org.hydrocouple.openswmm.plugins.geopackage.
     */
    std::string id() const override { return "org.hydrocouple.openswmm.plugins.geopackage"; }

    /**
     * @brief Copy plugin caption metadata.
     * @return Independent owned string. Human-readable GeoPackage I/O Plugin caption.
     */
    std::string caption() const override { return "GeoPackage I/O Plugin"; }

    /**
     * @brief Copy plugin description metadata.
     * @return Independent owned string. Human-readable plugin capability description.
     */
    std::string description() const override {
        return "Reads and writes SWMM model definitions, simulation results, "
               "and observed data using the OGC GeoPackage format (SQLite with "
               "spatial extensions). Supports multi-run storage, network topology "
               "queries, and calibration workflows in a single .gpkg file.";
    }

    /**
     * @brief Copy plugin version metadata.
     * @return Independent owned string. Metadata version string 1.0.0, not the engine or GeoPackage
     *     schema version.
     */
    std::string version() const override { return "1.0.0"; }

    /**
     * @brief Copy plugin vendor metadata.
     * @return Independent owned string. HydroCouple vendor label.
     */
    std::string vendor() const override { return "HydroCouple"; }

    /**
     * @brief Copy plugin url metadata.
     * @return Independent owned string. Project metadata URL; this method performs no network request.
     */
    std::string url() const override {
        return "https://hydrocouple.org/projects/openswmm/plugins/geopackage";
    }

    /**
     * @brief Copy discovery/search tags.
     * @return Owned vector of lowercase metadata tags; does not probe runtime capability.
     */
    std::vector<std::string> tags() const override {
        return {"geopackage", "sqlite", "spatial", "input",      "output",
                "report",     "ogc",    "gis",     "calibration"};
    }

    // -----------------------------------------------------------------------
    // Licensing (IPluginComponentInfo)
    // -----------------------------------------------------------------------

    /**
     * @brief Copy plugin license type metadata.
     * @return Independent owned string. Apache-2.0 SPDX license identifier.
     */
    std::string license_type() const override { return "Apache-2.0"; }

    /**
     * @brief Copy plugin license text metadata.
     * @return Independent owned string. Short copyright/license notice; complete terms remain in
     *     LICENSE/NOTICE.
     */
    std::string license_text() const override {
        return "Copyright (c) 2026 Caleb Buahin. All rights reserved.\n"
               "Licensed under the Apache License, Version 2.0 — see the\n"
               "LICENSE and NOTICE files for full text.";
    }

    // -----------------------------------------------------------------------
    // Capability queries (IPluginComponentInfo)
    // -----------------------------------------------------------------------

    /**
     * @brief Advertise the built-in GeoPackage input factory.
     * @return Always true, regardless of registration status or destination availability.
     */
    bool has_input() const noexcept override { return true; }
    /**
     * @brief Advertise the built-in GeoPackage output factory.
     * @return Always true, regardless of registration status or destination availability.
     */
    bool has_output() const noexcept override { return true; }
    /**
     * @brief Advertise the built-in GeoPackage report factory.
     * @return Always true, regardless of registration status or destination availability.
     */
    bool has_report() const noexcept override { return true; }

    // -----------------------------------------------------------------------
    // File-filter advertisement (IPluginComponentInfo)
    // -----------------------------------------------------------------------

    /**
     * @brief Copy file-filter advertisements for all three GeoPackage IO roles.
     *
     * Does not inspect files or bind a particular model/database.
     * @return Owned filters for INPUT_READ/OUTPUT_WRITE/REPORT_WRITE using *.gpkg and
     *     application/geopackage+sqlite3.
     */
    std::vector<FileFilter> file_filters() const override {
        const std::string desc = "OGC GeoPackage";
        const std::vector<std::string> patterns = {"*.gpkg"};
        const std::vector<std::string> mimes = {"application/geopackage+sqlite3"};
        return {
            FileFilter{desc, patterns, PluginRole::INPUT_READ, mimes},
            FileFilter{desc, patterns, PluginRole::OUTPUT_WRITE, mimes},
            FileFilter{desc, patterns, PluginRole::REPORT_WRITE, mimes},
        };
    }

    // -----------------------------------------------------------------------
    // Factory methods (IPluginComponentInfo)
    // -----------------------------------------------------------------------

    /**
     * @brief Allocate a fresh GeoPackage input plugin instance.
     *
     * Does not inspect registered_ or open a database; allocation failure propagates.
     * @return New caller-owned plugin; delete through its interface after finalization/cleanup.
     */
    IInputPlugin* create_input_plugin() const override;
    /**
     * @brief Allocate a fresh GeoPackage output plugin instance.
     *
     * Does not inspect registered_ or open a database; allocation failure propagates.
     * @return New caller-owned plugin; delete through its interface after finalization/cleanup.
     */
    IOutputPlugin* create_output_plugin() const override;
    /**
     * @brief Allocate a fresh GeoPackage report plugin instance.
     *
     * Does not inspect registered_ or open a database; allocation failure propagates.
     * @return New caller-owned plugin; delete through its interface after finalization/cleanup.
     */
    IReportPlugin* create_report_plugin() const override;

    // -----------------------------------------------------------------------
    // Registration (IPluginComponentInfo overrides)
    // -----------------------------------------------------------------------

    /**
     * @brief Copy supplied registration metadata and mark the singleton registered.
     *
     * Caller serializes registration reads/writes. Copy/allocation exceptions propagate; plugin factory
     * methods do not consult registered_.
     *
     * @param[in] info Borrowed registration record copied verbatim, including empty or unverified
     *     fields.
     * @return Always true after a successful copy; does not validate credentials or enforce a license
     *     gate.
     */
    bool register_plugin(const RegistrationInfo& info) override;
    /**
     * @brief Read the stored metadata-registration flag.
     * @return False initially and true after register_plugin() completes; no credential verification is
     *     implied.
     */
    bool registered() const noexcept override { return registered_; }
    /**
     * @brief Copy stored registration metadata.
     * @return Owned record copy, initially default-constructed; allocation can throw.
     */
    RegistrationInfo registration_info() const override { return reg_info_; }

private:
    /**
     * @brief Construct singleton metadata with registration initially false.
     */
    GeoPackagePluginInfo() = default;
    /**
     * @brief Disallow copying the singleton metadata owner.
     */
    GeoPackagePluginInfo(const GeoPackagePluginInfo&) = delete;
    /**
     * @brief Disallow copy replacement of singleton metadata.
     */
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
