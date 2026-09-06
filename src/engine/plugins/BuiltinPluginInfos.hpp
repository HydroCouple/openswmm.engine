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
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
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
    std::string license_type() const override { return "Apache-2.0"; }
    std::string license_text() const override { return "Apache License, Version 2.0"; }

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
    std::string license_type() const override { return "Apache-2.0"; }
    std::string license_text() const override { return "Apache License, Version 2.0"; }

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
    std::string license_type() const override { return "Apache-2.0"; }
    std::string license_text() const override { return "Apache License, Version 2.0"; }

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
    std::string license_type() const override { return "Apache-2.0"; }
    std::string license_text() const override { return "Apache License, Version 2.0"; }

    bool has_state_io() const noexcept override { return true; }

    std::vector<FileFilter> file_filters() const override;
};

} /* namespace openswmm */

#endif /* OPENSWMM_ENGINE_BUILTIN_PLUGIN_INFOS_HPP */
