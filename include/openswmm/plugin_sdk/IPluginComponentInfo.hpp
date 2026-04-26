 /**
 * @file IPluginComponentInfo.hpp
 * @brief Interface that describes a plugin component — metadata and factory methods.
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
 * @defgroup engine_plugin_sdk Plugin SDK Interfaces
 * @ingroup  engine_plugins
 *
 * @see IOutputPlugin.hpp
 * @see IReportPlugin.hpp
 * @see PluginState.hpp
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
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
    INPUT,    ///< Reads model data into SimulationContext
    OUTPUT,   ///< Writes time-series results during simulation
    REPORT,   ///< Writes summary statistics post-simulation
    STATE_IO  ///< Reads / writes simulation state (hot-start)
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
    INPUT_READ,    ///< Plugin reads a model input file (e.g. *.inp).
    OUTPUT_WRITE,  ///< Plugin writes a time-series results file (e.g. *.out).
    REPORT_WRITE,  ///< Plugin writes a summary report file (e.g. *.rpt).
    STATE_READ,    ///< Plugin reads a hot-start / state file.
    STATE_WRITE    ///< Plugin writes a hot-start / state file.
};

/**
 * @brief Convert a PluginRole to a stable, human-readable string.
 * @param role  Role enum value.
 * @returns Static null-terminated string (never NULL).
 * @ingroup engine_plugin_sdk
 */
inline const char* plugin_role_to_string(PluginRole role) noexcept {
    switch (role) {
        case PluginRole::INPUT_READ:    return "INPUT_READ";
        case PluginRole::OUTPUT_WRITE:  return "OUTPUT_WRITE";
        case PluginRole::REPORT_WRITE:  return "REPORT_WRITE";
        case PluginRole::STATE_READ:    return "STATE_READ";
        case PluginRole::STATE_WRITE:   return "STATE_WRITE";
        default:                        return "UNKNOWN";
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
    virtual ~IPluginComponentInfo() = default;

    // -----------------------------------------------------------------------
    // Identity & metadata
    // -----------------------------------------------------------------------

    /**
     * @brief Unique plugin identifier in reverse-DNS notation.
     * @returns e.g. "org.hydrocouple.openswmm.plugins.hdf5output"
     */
    virtual std::string id() const = 0;

    /**
     * @brief Human-readable display name of the plugin.
     * @returns e.g. "HDF5 Output Plugin"
     */
    virtual std::string caption() const = 0;

    /**
     * @brief Detailed description of what this plugin does.
     */
    virtual std::string description() const = 0;

    /**
     * @brief Plugin version string (Semantic Versioning recommended).
     * @returns e.g. "1.0.0"
     */
    virtual std::string version() const = 0;

    /**
     * @brief Vendor / author name.
     * @returns e.g. "HydroCouple Consortium"
     */
    virtual std::string vendor() const = 0;

    /**
     * @brief URL to the plugin's home page or documentation.
     * @returns e.g. "https://hydrocouple.org/plugins/hdf5"
     */
    virtual std::string url() const = 0;

    /**
     * @brief Additional tags or keywords for discovery.
     * @returns Vector of keyword strings.
     */
    virtual std::vector<std::string> tags() const { return {}; }

    // -----------------------------------------------------------------------
    // Licensing
    // -----------------------------------------------------------------------

    /**
     * @brief SPDX license identifier for this plugin.
     * @returns e.g. "MIT", "GPL-3.0-only", "LicenseRef-Commercial"
     * @see https://spdx.org/licenses/
     */
    virtual std::string license_type() const = 0;

    /**
     * @brief Full license text for this plugin.
     * @details Return the complete license text (or a URL to it).
     * @returns License text string.
     */
    virtual std::string license_text() const = 0;

    // -----------------------------------------------------------------------
    // Capability queries
    // -----------------------------------------------------------------------

    /**
     * @brief True if this plugin can create an IInputPlugin.
     * @details The PluginFactory checks this before calling create_input_plugin().
     */
    virtual bool has_input() const noexcept { return false; }

    /**
     * @brief True if this plugin can create an IOutputPlugin.
     * @details The PluginFactory checks this before calling create_output_plugin().
     */
    virtual bool has_output() const noexcept { return false; }

    /**
     * @brief True if this plugin can create an IReportPlugin.
     * @details The PluginFactory checks this before calling create_report_plugin().
     */
    virtual bool has_report() const noexcept { return false; }

    /**
     * @brief True if this plugin can create an IStateIOPlugin.
     * @details The PluginFactory checks this before calling create_state_io_plugin().
     */
    virtual bool has_state_io() const noexcept { return false; }

    // -----------------------------------------------------------------------
    // File-filter advertisement (every plugin)
    // -----------------------------------------------------------------------

    /**
     * @brief File-format filters this plugin handles.
     *
     * @details Every plugin SHOULD override this and return one FileFilter per
     *          (role, format) pair it supports. The default empty return
     *          preserves binary compatibility for plugins built against older
     *          SDK headers, but PluginFactory logs a warning when a plugin
     *          advertises a capability (e.g., has_input() == true) yet
     *          returns no filter for the matching role.
     *
     *          Hosts (Qt GUIs, CLI, Python bindings) compose user-visible
     *          file dialogs from the union of filters across all loaded
     *          plugins, grouped by role.
     *
     * @returns Vector of filter entries; empty by default.
     */
    virtual std::vector<FileFilter> file_filters() const { return {}; }

    // -----------------------------------------------------------------------
    // Registration
    // -----------------------------------------------------------------------

    /**
     * @brief Register the plugin with the provided registration information.
     *
     * @details Override this to implement registration logic (license validation,
     *          activation token checks, etc.). The default implementation
     *          accepts any registration and returns true.
     *
     * @param info  Registration information.
     * @returns true if registration succeeded, false otherwise.
     */
    virtual bool register_plugin(const RegistrationInfo& info) {
        (void)info;
        return true;
    }

    /**
     * @brief Check whether the plugin is currently registered.
     *
     * @details Override this to report registration status. Plugins that do
     *          not require registration should return true (the default).
     *
     * @returns true if the plugin is registered and ready to use.
     */
    virtual bool registered() const noexcept { return true; }

    /**
     * @brief Get the current registration information.
     *
     * @details Returns the info passed to the most recent successful
     *          register_plugin() call. The default returns an empty struct.
     *
     * @returns Registration information (valid only if registered() is true).
     */
    virtual RegistrationInfo registration_info() const { return {}; }

    // -----------------------------------------------------------------------
    // Factory methods
    // -----------------------------------------------------------------------

    /**
     * @brief Create a new IInputPlugin instance.
     *
     * @details The PluginFactory calls this when has_input() returns true.
     *          The returned pointer is owned by the caller (PluginFactory).
     *          The factory will call delete on it during cleanup.
     *
     * @returns New IInputPlugin instance, or nullptr if not supported.
     */
    virtual IInputPlugin* create_input_plugin() const { return nullptr; }

    /**
     * @brief Create a new IOutputPlugin instance.
     *
     * @details The PluginFactory calls this when has_output() returns true.
     *          The returned pointer is owned by the caller (PluginFactory).
     *          The factory will call delete on it during cleanup.
     *
     * @returns New IOutputPlugin instance, or nullptr if not supported.
     */
    virtual IOutputPlugin* create_output_plugin() const { return nullptr; }

    /**
     * @brief Create a new IReportPlugin instance.
     *
     * @details The PluginFactory calls this when has_report() returns true.
     *
     * @returns New IReportPlugin instance, or nullptr if not supported.
     */
    virtual IReportPlugin* create_report_plugin() const { return nullptr; }

    /**
     * @brief Create a new IStateIOPlugin instance.
     *
     * @details The PluginFactory calls this when has_state_io() returns true.
     *          The returned pointer is owned by the caller (PluginFactory).
     *          The factory will call delete on it during cleanup.
     *
     * @returns New IStateIOPlugin instance, or nullptr if not supported.
     */
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
using PluginInfoFactory = IPluginComponentInfo*(*)();

} /* namespace openswmm */

#endif /* OPENSWMM_IPLUGIN_COMPONENT_INFO_HPP */
