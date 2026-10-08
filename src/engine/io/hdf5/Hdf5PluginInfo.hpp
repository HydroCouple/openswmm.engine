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
 * @file Hdf5PluginInfo.hpp
 * @brief IPluginComponentInfo for the HDF5 model I/O plugin.
 *
 * @details Registered as a built-in by PluginFactory::register_builtin_infos(),
 *          exactly as GeoPackagePluginInfo is, so no dlopen and no [PLUGINS]
 *          row is needed to reach it.
 *
 * @ingroup engine_hdf5
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_HDF5_PLUGIN_INFO_HPP
#define OPENSWMM_HDF5_PLUGIN_INFO_HPP

#include <openswmm/plugin_sdk/IPluginComponentInfo.hpp>

#include <string>
#include <vector>

namespace openswmm::h5io {

/**
 * @brief Singleton describing the HDF5 I/O plugin.
 * @ingroup engine_hdf5
 */
class Hdf5PluginInfo : public IPluginComponentInfo {
public:
    static Hdf5PluginInfo& instance() {
        static Hdf5PluginInfo inst;
        return inst;
    }

    // --- identity -----------------------------------------------------------

    std::string id() const override {
        return "org.hydrocouple.openswmm.plugins.hdf5";
    }

    std::string caption() const override { return "HDF5 Model I/O Plugin"; }

    std::string description() const override {
        return "Writes SWMM model definitions to a columnar HDF5 container: one "
               "group per object class, one array per attribute, in AUTHORED "
               "units. Intended for array-shaped consumers — h5py, xarray, "
               "MATLAB, notebooks — that want the model without parsing text or "
               "issuing SQL. See src/engine/io/hdf5/STRATEGY.md.";
    }

    std::string version() const override { return "1.0.0"; }
    std::string vendor()  const override { return "HydroCouple"; }

    std::string url() const override {
        return "https://hydrocouple.org/projects/openswmm/plugins/hdf5";
    }

    std::vector<std::string> tags() const override {
        return {"hdf5", "h5", "columnar", "input", "output", "export",
                "analysis", "python"};
    }

    // --- licensing ----------------------------------------------------------

    std::string license_type() const override { return "Apache-2.0"; }

    std::string license_text() const override {
        return "Copyright (c) 2026 Caleb Buahin. All rights reserved.\n"
               "Licensed under the Apache License, Version 2.0 — see the\n"
               "LICENSE and NOTICE files for full text.";
    }

    // --- capabilities -------------------------------------------------------
    //
    // Only the model role is claimed today. Results and report plugins are
    // designed in STRATEGY.md §7 but not built, and advertising a capability
    // this plugin cannot honour would make swmm_model_write_with_plugin
    // resolve and then fail at the call rather than at the lookup.

    bool has_input()  const noexcept override { return true; }
    bool has_output() const noexcept override { return false; }
    bool has_report() const noexcept override { return false; }

    std::vector<FileFilter> file_filters() const override {
        const std::string desc = "OpenSWMM HDF5 model";
        const std::vector<std::string> patterns = {"*.h5", "*.hdf5"};
        const std::vector<std::string> mimes    = {"application/x-hdf5"};
        return { FileFilter{ desc, patterns, PluginRole::INPUT_READ, mimes } };
    }

    // --- factories ----------------------------------------------------------

    IInputPlugin* create_input_plugin() const override;

private:
    Hdf5PluginInfo() = default;
};

}  // namespace openswmm::h5io

#endif  // OPENSWMM_HDF5_PLUGIN_INFO_HPP
