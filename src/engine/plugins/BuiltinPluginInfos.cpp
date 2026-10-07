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
 * @file BuiltinPluginInfos.cpp
 * @brief Singletons + file_filters() implementations for built-in plugins.
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "BuiltinPluginInfos.hpp"

namespace openswmm {

// ---------- DefaultInput -----------------------------------------------------

BuiltinDefaultInputPluginInfo& BuiltinDefaultInputPluginInfo::instance() noexcept {
    static BuiltinDefaultInputPluginInfo s;
    return s;
}

std::vector<FileFilter> BuiltinDefaultInputPluginInfo::file_filters() const {
    return {
        FileFilter{
            /* description */ "SWMM Input File",
            /* patterns    */ {"*.inp"},
            /* role        */ PluginRole::INPUT_READ,
            /* mime_types  */ {"text/plain"}
        }
    };
}

// ---------- DefaultOutput ----------------------------------------------------

BuiltinDefaultOutputPluginInfo& BuiltinDefaultOutputPluginInfo::instance() noexcept {
    static BuiltinDefaultOutputPluginInfo s;
    return s;
}

std::vector<FileFilter> BuiltinDefaultOutputPluginInfo::file_filters() const {
    return {
        FileFilter{
            /* description */ "SWMM Binary Output",
            /* patterns    */ {"*.out"},
            /* role        */ PluginRole::OUTPUT_WRITE,
            /* mime_types  */ {"application/octet-stream"}
        }
    };
}

// ---------- DefaultReport ----------------------------------------------------

BuiltinDefaultReportPluginInfo& BuiltinDefaultReportPluginInfo::instance() noexcept {
    static BuiltinDefaultReportPluginInfo s;
    return s;
}

std::vector<FileFilter> BuiltinDefaultReportPluginInfo::file_filters() const {
    return {
        FileFilter{
            /* description */ "SWMM Summary Report",
            /* patterns    */ {"*.rpt"},
            /* role        */ PluginRole::REPORT_WRITE,
            /* mime_types  */ {"text/plain"}
        }
    };
}

// ---------- DefaultStateIO --------------------------------------------------

BuiltinDefaultStateIOPluginInfo& BuiltinDefaultStateIOPluginInfo::instance() noexcept {
    static BuiltinDefaultStateIOPluginInfo s;
    return s;
}

std::vector<FileFilter> BuiltinDefaultStateIOPluginInfo::file_filters() const {
    const std::vector<std::string> mimes_hs  = {"application/x-openswmm-hotstart"};
    const std::vector<std::string> mimes_hsf = {"application/x-swmm5-hotstart"};
    return {
        FileFilter{ "OpenSWMM Hot-Start File", {"*.hs"},  PluginRole::STATE_READ,  mimes_hs  },
        FileFilter{ "OpenSWMM Hot-Start File", {"*.hs"},  PluginRole::STATE_WRITE, mimes_hs  },
        FileFilter{ "Legacy SWMM5 Hot-Start",  {"*.hsf"}, PluginRole::STATE_READ,  mimes_hsf },
    };
}

} /* namespace openswmm */
