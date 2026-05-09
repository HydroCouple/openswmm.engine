/**
 * @file BuiltinPluginInfos.hpp
 * @brief IPluginComponentInfo singletons for the engine's built-in default plugins.
 *
 * @details The default Input/Output/Report/State-IO plugin instances are
 *          injected directly into PluginFactory via add_*_plugin() and do not
 *          go through dlopen-based discovery. To make their metadata —
 *          especially file_filters() — visible to hosts (Qt GUI, CLI), the
 *          PluginFactory registers these singletons as synthetic library
 *          entries (handle = nullptr) at construction time.
 *
 *          The factory methods on these info classes return nullptr because
 *          the actual instances live elsewhere; the entries exist purely for
 *          discovery and filter advertisement.
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_ENGINE_BUILTIN_PLUGIN_INFOS_HPP
#define OPENSWMM_ENGINE_BUILTIN_PLUGIN_INFOS_HPP

#include "../../../include/openswmm/plugin_sdk/IPluginComponentInfo.hpp"

namespace openswmm {

class BuiltinDefaultInputPluginInfo final : public IPluginComponentInfo {
public:
    static BuiltinDefaultInputPluginInfo& instance() noexcept;

    std::string id()           const override { return "org.hydrocouple.openswmm.builtin.input"; }
    std::string caption()      const override { return "Default SWMM Input Plugin"; }
    std::string description()  const override {
        return "Reads and writes SWMM .inp model files.";
    }
    std::string version()      const override { return "1.0.0"; }
    std::string vendor()       const override { return "HydroCouple"; }
    std::string url()          const override { return "https://hydrocouple.org"; }
    std::string license_type() const override { return "MIT"; }
    std::string license_text() const override { return "MIT License"; }

    bool has_input()  const noexcept override { return true; }

    std::vector<FileFilter> file_filters() const override;
};

class BuiltinDefaultOutputPluginInfo final : public IPluginComponentInfo {
public:
    static BuiltinDefaultOutputPluginInfo& instance() noexcept;

    std::string id()           const override { return "org.hydrocouple.openswmm.builtin.output"; }
    std::string caption()      const override { return "Default SWMM Output Plugin"; }
    std::string description()  const override {
        return "Writes the SWMM 5.x binary time-series results file (.out).";
    }
    std::string version()      const override { return "1.0.0"; }
    std::string vendor()       const override { return "HydroCouple"; }
    std::string url()          const override { return "https://hydrocouple.org"; }
    std::string license_type() const override { return "MIT"; }
    std::string license_text() const override { return "MIT License"; }

    bool has_output() const noexcept override { return true; }

    std::vector<FileFilter> file_filters() const override;
};

class BuiltinDefaultReportPluginInfo final : public IPluginComponentInfo {
public:
    static BuiltinDefaultReportPluginInfo& instance() noexcept;

    std::string id()           const override { return "org.hydrocouple.openswmm.builtin.report"; }
    std::string caption()      const override { return "Default SWMM Report Plugin"; }
    std::string description()  const override {
        return "Writes the SWMM summary report file (.rpt).";
    }
    std::string version()      const override { return "1.0.0"; }
    std::string vendor()       const override { return "HydroCouple"; }
    std::string url()          const override { return "https://hydrocouple.org"; }
    std::string license_type() const override { return "MIT"; }
    std::string license_text() const override { return "MIT License"; }

    bool has_report() const noexcept override { return true; }

    std::vector<FileFilter> file_filters() const override;
};

class BuiltinDefaultStateIOPluginInfo final : public IPluginComponentInfo {
public:
    static BuiltinDefaultStateIOPluginInfo& instance() noexcept;

    std::string id()           const override { return "org.hydrocouple.openswmm.builtin.state_io"; }
    std::string caption()      const override { return "Default Hot-Start / State IO Plugin"; }
    std::string description()  const override {
        return "Reads and writes OpenSWMM hot-start files (*.hs) and reads "
               "legacy SWMM5 hot-start files (*.hsf).";
    }
    std::string version()      const override { return "1.0.0"; }
    std::string vendor()       const override { return "HydroCouple"; }
    std::string url()          const override { return "https://hydrocouple.org"; }
    std::string license_type() const override { return "MIT"; }
    std::string license_text() const override { return "MIT License"; }

    bool has_state_io() const noexcept override { return true; }

    std::vector<FileFilter> file_filters() const override;
};

} /* namespace openswmm */

#endif /* OPENSWMM_ENGINE_BUILTIN_PLUGIN_INFOS_HPP */
