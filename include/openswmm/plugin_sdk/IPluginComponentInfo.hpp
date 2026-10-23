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
 * @file      IPluginComponentInfo.hpp
 * @brief     Interface that describes a plugin component — metadata and factory methods.
 *
 * @details This interface is loosely inspired by the HydroCouple `IComponentInfo`
 *          interface (see https://hydrocouple.org/hydrocouple) but is not a strict
 *          implementation. It is designed to be simple enough for any plugin author
 *          to implement without depending on HydroCouple.
 *
 *          Each plugin shared library must export a C factory function:
 *
 * @code{.c}
 * // Exported from the plugin .so/.dylib/.dll:
 * extern "C" openswmm::IPluginComponentInfo* openswmm_plugin_info(void);
 * @endcode
 *
 *          The PluginFactory calls this function after dlopen() to retrieve the
 *          component info and then calls create_output_plugin() or
 *          create_report_plugin() to instantiate plugin instances.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

/**
 * @defgroup engine_plugin_sdk Plugin SDK Interfaces
 * @ingroup  engine_plugins
 *
 * @see IOutputPlugin.hpp
 * @see IReportPlugin.hpp
 * @see PluginState.hpp
 */

#ifndef OPENSWMM_IPLUGIN_COMPONENT_INFO_HPP
#define OPENSWMM_IPLUGIN_COMPONENT_INFO_HPP

#include <string>
#include <vector>
#include "PluginState.hpp"

namespace openswmm {

/* Forward declarations */
class IInputPlugin;
class IOutputPlugin;
class IReportPlugin;
class IStateIOPlugin;

/**
 * @brief Identifies a plugin capability.
 *
 * @details Retained for use in discovery results and filtering.
 *          A single plugin may support multiple types — use the
 *          has_input() / has_output() / has_report() / has_state_io()
 *          capability queries on IPluginComponentInfo instead of
 *          assuming a plugin has only one type.
 *
 * @ingroup engine_plugin_sdk
 */
enum class PluginType {
    INPUT,   ///< Reads model data into SimulationContext
    OUTPUT,  ///< Writes time-series results during simulation
    REPORT,  ///< Writes summary statistics post-simulation
    STATE_IO ///< Reads / writes simulation state (hot-start)
};

/**
 * @brief Identifies the I/O direction and category of a file filter entry.
 *
 * @details A single plugin may legitimately advertise multiple roles (e.g.,
 *          GeoPackage handles INPUT_READ, OUTPUT_WRITE, and REPORT_WRITE for
 *          the same `*.gpkg` extension). The GUI layer uses the role to
 *          pick which filters to show in each QFileDialog (Open vs. Save,
 *          model file vs. results file vs. hot-start, etc.).
 *
 * @ingroup engine_plugin_sdk
 */
enum class PluginRole : int {
    INPUT_READ,   ///< Plugin reads a model input file (e.g. *.inp).
    OUTPUT_WRITE, ///< Plugin writes a time-series results file (e.g. *.out).
    REPORT_WRITE, ///< Plugin writes a summary report file (e.g. *.rpt).
    STATE_READ,   ///< Plugin reads a hot-start / state file.
    STATE_WRITE   ///< Plugin writes a hot-start / state file.
};

/** @brief Read a stable static spelling for a plugin file role.
 * @param role Enum value; unrecognized returns UNKNOWN.
 * @return Nonnull immutable null-terminated literal, valid for process lifetime.
 * Never free; no state mutation, allocation or synchronization required. */
inline const char* plugin_role_to_string(PluginRole role) noexcept {
    switch (role) {
    case PluginRole::INPUT_READ:
        return "INPUT_READ";
    case PluginRole::OUTPUT_WRITE:
        return "OUTPUT_WRITE";
    case PluginRole::REPORT_WRITE:
        return "REPORT_WRITE";
    case PluginRole::STATE_READ:
        return "STATE_READ";
    case PluginRole::STATE_WRITE:
        return "STATE_WRITE";
    default:
        return "UNKNOWN";
    }
}

/**
 * @brief A file-format filter advertised by a plugin.
 *
 * @details Hosts (Qt GUIs, CLI tools, Python bindings, web frontends) consume
 *          this struct to build user-visible file pickers without hard-coding
 *          format lists. The plugin SDK is intentionally framework-free —
 *          Qt-style filter strings like `"SWMM Input (*.inp);;All Files (*)"`
 *          are assembled by the host from these entries.
 *
 *          Each plugin returns one entry per (role, format) combination.
 *
 * @ingroup engine_plugin_sdk
 */
struct FileFilter {
    /**
     * @brief Human-readable format name shown to the user.
     *
     * @details Examples: "SWMM Input File", "SWMM Binary Output",
     *          "OpenSWMM Hot-Start File". Hosts may translate this string
     *          (e.g., wrap in tr()).
     */
    std::string description;

    /**
     * @brief Glob patterns that match files in this format.
     *
     * @details Examples: {"*.inp"}, {"*.tif", "*.tiff"}. Use lowercase by
     *          convention; hosts that match case-insensitively need not
     *          duplicate "*.INP".
     */
    std::vector<std::string> patterns;

    /**
     * @brief I/O role this filter applies to.
     *
     * @details A single plugin may emit one filter per direction (e.g.,
     *          INPUT_READ + OUTPUT_WRITE for the same `*.gpkg`).
     */
    PluginRole role = PluginRole::INPUT_READ;

    /**
     * @brief Optional MIME types for hosts that need them (web, mobile).
     *
     * @details Empty for formats with no registered MIME type.
     *          Examples: {"text/plain"}, {"application/x-swmm-hotstart"}.
     */
    std::vector<std::string> mime_types;
};

/**
 * @brief Registration information for plugin activation.
 *
 * @details Contains fields that a license manager, deployment tool,
 *          or host application may supply to activate a plugin.
 *          Plugins that require registration gate their factory methods
 *          behind a successful register_plugin() call.
 *
 * @ingroup engine_plugin_sdk
 */
struct RegistrationInfo {
    std::string license_key;   ///< License key or activation token (may be empty for open plugins)
    std::string organization;  ///< Registering organization name
    std::string contact_email; ///< Contact email for the registrant
    std::string deployment_id; ///< Unique deployment or instance ID
};

/**
 * @brief Describes a plugin component: metadata, capabilities, and factory methods.
 *
 * @details Plugins implement this class and export it via `openswmm_plugin_info()`.
 *          The PluginFactory uses it to discover capabilities and create instances.
 *
 * @note Implement this as a singleton — the PluginFactory holds one pointer to
 *       the IPluginComponentInfo and may call create_*_plugin() multiple times.
 *
 * @ingroup engine_plugin_sdk
 */
class IPluginComponentInfo {
public:
    /** @brief Destroy component metadata only through its owning library/host lifetime.
     * PluginFactory borrows singleton metadata and does not delete it as an instance.
     * Keep library loaded until all created plugin instances/callbacks are destroyed;
     * base destructor performs no instance cleanup, registration or library unloading. */
    virtual ~IPluginComponentInfo() = default;

    // -----------------------------------------------------------------------
    // Identity & metadata
    // -----------------------------------------------------------------------

    /** @brief Copy the stable discovery identifier for this component.
     * @return Owned reverse-DNS-style ID, consistent across discovery/factory use; host
     * must not infer a unique supported capability from it. No automatic validation. */
    virtual std::string id() const = 0;

    /** @brief Copy the component display name.
     * @return Owned human-readable label; independent of stable id and library path. */
    virtual std::string caption() const = 0;

    /** @brief Copy the component's descriptive metadata.
     * @return Owned explanatory text; empty is allowed by this base interface. */
    virtual std::string description() const = 0;

    /** @brief Copy the component's declared version.
     * @return Owned version text; semantic versioning is recommended, not enforced by
     * this base. Does not independently prove binary ABI/schema compatibility. */
    virtual std::string version() const = 0;

    /** @brief Copy declared vendor/author metadata.
     * @return Owned display text; no identity/ownership or authorship verification. */
    virtual std::string vendor() const = 0;

    /** @brief Copy the declared home/documentation URL.
     * @return Owned text; not opened, fetched or validated by this interface. */
    virtual std::string url() const = 0;

    /** @brief Copy optional discovery keywords.
     * @return Owned strings; base empty. No capability inference, normalization or
     * registration gating is implied by tags. */
    virtual std::vector<std::string> tags() const { return {}; }

    // -----------------------------------------------------------------------
    // Licensing
    // -----------------------------------------------------------------------

    /** @brief Copy the component's declared license identifier.
     * @return Owned SPDX-style or LicenseRef text; base interface does not validate it
     * or grant rights independently of the component's license terms. */
    virtual std::string license_type() const = 0;

    /** @brief Copy declared license text or a reference URL.
     * @return Owned string for host display. No remote fetch, activation or registration
     * is performed by this query. */
    virtual std::string license_text() const = 0;

    // -----------------------------------------------------------------------
    // Capability queries
    // -----------------------------------------------------------------------

    /** @brief Query whether this component advertises the input capability.
     * @return False by default, true when corresponding create_input_plugin is
     * supported. Multiple capabilities can be true and factory loads all advertised
     * roles; no registration or instance creation occurs here. Must not throw. */
    virtual bool has_input() const noexcept { return false; }

    /** @brief Query whether this component advertises the output capability.
     * @return False by default, true when corresponding create_output_plugin is
     * supported. Multiple capabilities can be true and factory loads all advertised
     * roles; no registration or instance creation occurs here. Must not throw. */
    virtual bool has_output() const noexcept { return false; }

    /** @brief Query whether this component advertises the report capability.
     * @return False by default, true when corresponding create_report_plugin is
     * supported. Multiple capabilities can be true and factory loads all advertised
     * roles; no registration or instance creation occurs here. Must not throw. */
    virtual bool has_report() const noexcept { return false; }

    /** @brief Query whether this component advertises the state_io capability.
     * @return False by default, true when corresponding create_state_io_plugin is
     * supported. Multiple capabilities can be true and factory loads all advertised
     * roles; no registration or instance creation occurs here. Must not throw. */
    virtual bool has_state_io() const noexcept { return false; }

    // -----------------------------------------------------------------------
    // File-filter advertisement (every plugin)
    // -----------------------------------------------------------------------

    /** @brief Copy supported format/role filters for host discovery and file dialogs.
     * @return Owned entries with role, description, glob patterns and optional MIME types;
     * base empty. Each supported I/O role should advertise its filters. Host/factory may
     * warn on missing capability roles, but filters do not validate actual file contents,
     * readability or plugin ABI and do not select a single capability to instantiate. */
    virtual std::vector<FileFilter> file_filters() const { return {}; }

    // -----------------------------------------------------------------------
    // Registration
    // -----------------------------------------------------------------------

    /** @brief Apply implementation-defined registration data before protected factories.
     * @param info Borrowed registration fields for this call; copy retained data. Base
     * ignores all fields and returns true without storing them or doing network I/O.
     * @return True when implementation accepts, false otherwise. Implementations own
     * validation, side effects, repeat-call policy and factory gating; base supplies no
     * activation service or rollback. Serialize with factories/registration queries. */
    virtual bool register_plugin(const RegistrationInfo& info) {
        (void)info;
        return true;
    }

    /** @brief Query implementation-defined registration readiness.
     * @return Base true; overrides report their current activation status. No registration
     * side effect or synchronization is provided; must not throw. Serialize against changes. */
    virtual bool registered() const noexcept { return true; }

    /** @brief Copy implementation-reported current registration metadata.
     * @return Independent fields, base empty even after base register_plugin succeeded.
     * Overrides may report last successful registration when registered()==true. Serialize
     * with registration changes; returned copy has no lifetime tie to the component. */
    virtual RegistrationInfo registration_info() const { return {}; }

    // -----------------------------------------------------------------------
    // Factory methods
    // -----------------------------------------------------------------------

    /** @brief Allocate a distinct input plugin instance when supported and permitted.
     * @return New polymorphic instance owned by caller, or nullptr (base/unsupported/
     * registration refusal). Caller deletes it before unloading this metadata's library;
     * never return the shared metadata singleton as an instance. Multiple calls may
     * create independent engines' instances. Returned instance still needs lifecycle
     * initialization; allocation/implementation exceptions can propagate. Serialize with
     * registration changes and honor implementation-specific factory thread safety. */
    virtual IInputPlugin* create_input_plugin() const { return nullptr; }

    /** @brief Allocate a distinct output plugin instance when supported and permitted.
     * @return New polymorphic instance owned by caller, or nullptr (base/unsupported/
     * registration refusal). Caller deletes it before unloading this metadata's library;
     * never return the shared metadata singleton as an instance. Multiple calls may
     * create independent engines' instances. Returned instance still needs lifecycle
     * initialization; allocation/implementation exceptions can propagate. Serialize with
     * registration changes and honor implementation-specific factory thread safety. */
    virtual IOutputPlugin* create_output_plugin() const { return nullptr; }

    /** @brief Allocate a distinct report plugin instance when supported and permitted.
     * @return New polymorphic instance owned by caller, or nullptr (base/unsupported/
     * registration refusal). Caller deletes it before unloading this metadata's library;
     * never return the shared metadata singleton as an instance. Multiple calls may
     * create independent engines' instances. Returned instance still needs lifecycle
     * initialization; allocation/implementation exceptions can propagate. Serialize with
     * registration changes and honor implementation-specific factory thread safety. */
    virtual IReportPlugin* create_report_plugin() const { return nullptr; }

    /** @brief Allocate a distinct state_io plugin instance when supported and permitted.
     * @return New polymorphic instance owned by caller, or nullptr (base/unsupported/
     * registration refusal). Caller deletes it before unloading this metadata's library;
     * never return the shared metadata singleton as an instance. Multiple calls may
     * create independent engines' instances. Returned instance still needs lifecycle
     * initialization; allocation/implementation exceptions can propagate. Serialize with
     * registration changes and honor implementation-specific factory thread safety. */
    virtual IStateIOPlugin* create_state_io_plugin() const { return nullptr; }
};

// ---------------------------------------------------------------------------
// C factory function signature that plugin .so/.dylib/.dll must export
// ---------------------------------------------------------------------------

/**
 * @brief Typedef for the C factory function that plugin libraries must export.
 *
 * @details Every plugin shared library must export a function with this
 *          signature and the name `openswmm_plugin_info`. The PluginFactory
 *          looks this up via dlsym/GetProcAddress after loading the library.
 *
 * Example implementation in your plugin:
 * @code{.cpp}
 * static MyPluginComponentInfo g_info;
 *
 * extern "C" openswmm::IPluginComponentInfo* openswmm_plugin_info(void) {
 *     return &g_info;
 * }
 * @endcode
 */
using PluginInfoFactory = IPluginComponentInfo* (*)();

} /* namespace openswmm */

#endif /* OPENSWMM_IPLUGIN_COMPONENT_INFO_HPP */
