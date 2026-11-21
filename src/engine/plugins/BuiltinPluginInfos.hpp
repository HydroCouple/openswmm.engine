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
 * @file      BuiltinPluginInfos.hpp
 * @brief     IPluginComponentInfo singletons for the engine's built-in default plugins.
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
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_BUILTIN_PLUGIN_INFOS_HPP
#define OPENSWMM_ENGINE_BUILTIN_PLUGIN_INFOS_HPP

#include "../../../include/openswmm/plugin_sdk/IPluginComponentInfo.hpp"

namespace openswmm {

class BuiltinDefaultInputPluginInfo final : public IPluginComponentInfo {
public:
    /** @brief Access process-lifetime builtin metadata without constructing plugin instances.
     * @return Borrowed singleton; never delete. First construction is thread-safe. */
    static BuiltinDefaultInputPluginInfo& instance() noexcept;

    /** @brief Copy the stable builtin ID.
     * @return Literal org.hydrocouple.openswmm.builtin.input; no IO or discovery occurs. */
    std::string id() const override { return "org.hydrocouple.openswmm.builtin.input"; }
    /** @brief Copy the UI caption.
     * @return Literal Default SWMM Input Plugin; no IO or discovery occurs. */
    std::string caption() const override { return "Default SWMM Input Plugin"; }
    /** @brief Copy the capability description.
     * @return Literal Reads and writes SWMM .inp model files.; no IO or discovery occurs. */
    std::string description() const override { return "Reads and writes SWMM .inp model files."; }
    /** @brief Copy the metadata version.
     * @return Literal 1.0.0; no IO or discovery occurs. */
    std::string version() const override { return "1.0.0"; }
    /** @brief Copy the vendor name.
     * @return Literal HydroCouple; no IO or discovery occurs. */
    std::string vendor() const override { return "HydroCouple"; }
    /** @brief Copy the project URL.
     * @return Literal https://hydrocouple.org; no IO or discovery occurs. */
    std::string url() const override { return "https://hydrocouple.org"; }
    /** @brief Copy the SPDX license token.
     * @return Literal Apache-2.0; no IO or discovery occurs. */
    std::string license_type() const override { return "Apache-2.0"; }
    /** @brief Copy the license display name.
     * @return Literal Apache License, Version 2.0; no IO or discovery occurs. */
    std::string license_text() const override { return "Apache License, Version 2.0"; }

    /** @brief Advertise this builtin metadata's matching capability.
     * @return True; does not imply its metadata factory will instantiate the builtin.
     * Actual default instances are injected separately by the engine. */
    bool has_input() const noexcept override { return true; }

    /** @brief Copy the builtin format/role advertisement.
     * @return One *.inp INPUT_READ text/plain filter; the description does not advertise a write role here. */
    std::vector<FileFilter> file_filters() const override;
};

class BuiltinDefaultOutputPluginInfo final : public IPluginComponentInfo {
public:
    /** @brief Access process-lifetime builtin metadata without constructing plugin instances.
     * @return Borrowed singleton; never delete. First construction is thread-safe. */
    static BuiltinDefaultOutputPluginInfo& instance() noexcept;

    /** @brief Copy the stable builtin ID.
     * @return Literal org.hydrocouple.openswmm.builtin.output; no IO or discovery occurs. */
    std::string id() const override { return "org.hydrocouple.openswmm.builtin.output"; }
    /** @brief Copy the UI caption.
     * @return Literal Default SWMM Output Plugin; no IO or discovery occurs. */
    std::string caption() const override { return "Default SWMM Output Plugin"; }
    /** @brief Copy the capability description.
     * @return Literal Writes the SWMM 5.x binary time-series results file (.out).; no IO or discovery occurs. */
    std::string description() const override {
        return "Writes the SWMM 5.x binary time-series results file (.out).";
    }
    /** @brief Copy the metadata version.
     * @return Literal 1.0.0; no IO or discovery occurs. */
    std::string version() const override { return "1.0.0"; }
    /** @brief Copy the vendor name.
     * @return Literal HydroCouple; no IO or discovery occurs. */
    std::string vendor() const override { return "HydroCouple"; }
    /** @brief Copy the project URL.
     * @return Literal https://hydrocouple.org; no IO or discovery occurs. */
    std::string url() const override { return "https://hydrocouple.org"; }
    /** @brief Copy the SPDX license token.
     * @return Literal Apache-2.0; no IO or discovery occurs. */
    std::string license_type() const override { return "Apache-2.0"; }
    /** @brief Copy the license display name.
     * @return Literal Apache License, Version 2.0; no IO or discovery occurs. */
    std::string license_text() const override { return "Apache License, Version 2.0"; }

    /** @brief Advertise this builtin metadata's matching capability.
     * @return True; does not imply its metadata factory will instantiate the builtin.
     * Actual default instances are injected separately by the engine. */
    bool has_output() const noexcept override { return true; }

    /** @brief Copy the builtin format/role advertisement.
     * @return One *.out OUTPUT_WRITE application/octet-stream filter. */
    std::vector<FileFilter> file_filters() const override;
};

class BuiltinDefaultReportPluginInfo final : public IPluginComponentInfo {
public:
    /** @brief Access process-lifetime builtin metadata without constructing plugin instances.
     * @return Borrowed singleton; never delete. First construction is thread-safe. */
    static BuiltinDefaultReportPluginInfo& instance() noexcept;

    /** @brief Copy the stable builtin ID.
     * @return Literal org.hydrocouple.openswmm.builtin.report; no IO or discovery occurs. */
    std::string id() const override { return "org.hydrocouple.openswmm.builtin.report"; }
    /** @brief Copy the UI caption.
     * @return Literal Default SWMM Report Plugin; no IO or discovery occurs. */
    std::string caption() const override { return "Default SWMM Report Plugin"; }
    /** @brief Copy the capability description.
     * @return Literal Writes the SWMM summary report file (.rpt).; no IO or discovery occurs. */
    std::string description() const override {
        return "Writes the SWMM summary report file (.rpt).";
    }
    /** @brief Copy the metadata version.
     * @return Literal 1.0.0; no IO or discovery occurs. */
    std::string version() const override { return "1.0.0"; }
    /** @brief Copy the vendor name.
     * @return Literal HydroCouple; no IO or discovery occurs. */
    std::string vendor() const override { return "HydroCouple"; }
    /** @brief Copy the project URL.
     * @return Literal https://hydrocouple.org; no IO or discovery occurs. */
    std::string url() const override { return "https://hydrocouple.org"; }
    /** @brief Copy the SPDX license token.
     * @return Literal Apache-2.0; no IO or discovery occurs. */
    std::string license_type() const override { return "Apache-2.0"; }
    /** @brief Copy the license display name.
     * @return Literal Apache License, Version 2.0; no IO or discovery occurs. */
    std::string license_text() const override { return "Apache License, Version 2.0"; }

    /** @brief Advertise this builtin metadata's matching capability.
     * @return True; does not imply its metadata factory will instantiate the builtin.
     * Actual default instances are injected separately by the engine. */
    bool has_report() const noexcept override { return true; }

    /** @brief Copy the builtin format/role advertisement.
     * @return One *.rpt REPORT_WRITE text/plain filter. */
    std::vector<FileFilter> file_filters() const override;
};

class BuiltinDefaultStateIOPluginInfo final : public IPluginComponentInfo {
public:
    /** @brief Access process-lifetime builtin metadata without constructing plugin instances.
     * @return Borrowed singleton; never delete. First construction is thread-safe. */
    static BuiltinDefaultStateIOPluginInfo& instance() noexcept;

    /** @brief Copy the stable builtin ID.
     * @return Literal org.hydrocouple.openswmm.builtin.state_io; no IO or discovery occurs. */
    std::string id() const override { return "org.hydrocouple.openswmm.builtin.state_io"; }
    /** @brief Copy the UI caption.
     * @return Literal Default Hot-Start / State IO Plugin; no IO or discovery occurs. */
    std::string caption() const override { return "Default Hot-Start / State IO Plugin"; }
    /** @brief Copy the capability description.
     * @return Literal Reads and writes OpenSWMM hot-start files (*.hs) and reads legacy SWMM5 hot-start files (*.hsf).; no IO or discovery occurs. */
    std::string description() const override {
        return "Reads and writes OpenSWMM hot-start files (*.hs) and reads "
               "legacy SWMM5 hot-start files (*.hsf).";
    }
    /** @brief Copy the metadata version.
     * @return Literal 1.0.0; no IO or discovery occurs. */
    std::string version() const override { return "1.0.0"; }
    /** @brief Copy the vendor name.
     * @return Literal HydroCouple; no IO or discovery occurs. */
    std::string vendor() const override { return "HydroCouple"; }
    /** @brief Copy the project URL.
     * @return Literal https://hydrocouple.org; no IO or discovery occurs. */
    std::string url() const override { return "https://hydrocouple.org"; }
    /** @brief Copy the SPDX license token.
     * @return Literal Apache-2.0; no IO or discovery occurs. */
    std::string license_type() const override { return "Apache-2.0"; }
    /** @brief Copy the license display name.
     * @return Literal Apache License, Version 2.0; no IO or discovery occurs. */
    std::string license_text() const override { return "Apache License, Version 2.0"; }

    /** @brief Advertise this builtin metadata's matching capability.
     * @return True; does not imply its metadata factory will instantiate the builtin.
     * Actual default instances are injected separately by the engine. */
    bool has_state_io() const noexcept override { return true; }

    /** @brief Copy the builtin format/role advertisement.
     * @return Three filters: *.hs STATE_READ and STATE_WRITE with OpenSWMM hot-start MIME,
     * and *.hsf STATE_READ with legacy SWMM5 hot-start MIME. */
    std::vector<FileFilter> file_filters() const override;
};

} /* namespace openswmm */

#endif /* OPENSWMM_ENGINE_BUILTIN_PLUGIN_INFOS_HPP */
