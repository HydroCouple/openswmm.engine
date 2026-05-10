/**
 * @file BuiltinPluginInfos.cpp
 * @brief Singletons + file_filters() implementations for built-in plugins.
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
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
