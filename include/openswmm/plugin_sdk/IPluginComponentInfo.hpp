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

/**
 * @brief Identifies a plugin capability.
 *
 * @details Retained for use in discovery results and filtering.
 *          A single plugin may support multiple types — use the
 *          has_input() / has_output() / has_report() capability
 *          queries on IPluginComponentInfo instead of assuming
 *          a plugin has only one type.
 *
 * @ingroup engine_plugin_sdk
 */
enum class PluginType {
    INPUT,   ///< Reads model data into SimulationContext
    OUTPUT,  ///< Writes time-series results during simulation
    REPORT   ///< Writes summary statistics post-simulation
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
