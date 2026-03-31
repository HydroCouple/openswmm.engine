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
class IOutputPlugin;
class IReportPlugin;

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
     * @brief Returns true if this plugin provides an IOutputPlugin instance.
     * @details An output plugin writes simulation result time series to a file
     *          or data store during simulation (via the IO thread).
     */
    virtual bool provides_output() const = 0;

    /**
     * @brief Returns true if this plugin provides an IReportPlugin instance.
     * @details A report plugin writes summary statistics at the end of a
     *          simulation run.
     */
    virtual bool provides_report() const = 0;

    // -----------------------------------------------------------------------
    // Factory methods
    // -----------------------------------------------------------------------

    /**
     * @brief Create a new IOutputPlugin instance.
     *
     * @details The PluginFactory calls this if provides_output() returns true.
     *          The returned pointer is owned by the caller (PluginFactory).
     *          The factory will call delete on it during cleanup.
     *
     * @returns New IOutputPlugin instance, or nullptr if not supported.
     */
    virtual IOutputPlugin* create_output_plugin() const { return nullptr; }

    /**
     * @brief Create a new IReportPlugin instance.
     *
     * @details The PluginFactory calls this if provides_report() returns true.
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
